/*
 *  linux/kernel/signal.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  1997-11-02  Modified for POSIX.1b signals by Richard Henderson
 */

#include <linux/config.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/unistd.h>
#include <linux/smp_lock.h>
#include <linux/init.h>
#include <linux/sched.h>

#include <asm/uaccess.h>

/*
 * SLAB caches for signal bits.
 */

#define DEBUG_SIG 0

#if DEBUG_SIG
#define SIG_SLAB_DEBUG	(SLAB_DEBUG_FREE | SLAB_RED_ZONE /* | SLAB_POISON */)
#else
#define SIG_SLAB_DEBUG	0
#endif

static kmem_cache_t *sigqueue_cachep;

atomic_t nr_queued_signals;
int max_queued_signals = 1024;

void __init signals_init(void)
{
	sigqueue_cachep =
		kmem_cache_create("sigqueue",
				  sizeof(struct sigqueue),
				  __alignof__(struct sigqueue),
				  SIG_SLAB_DEBUG, NULL, NULL);
	if (!sigqueue_cachep)
		panic("signals_init(): cannot create sigqueue SLAB cache");
}


/* Given the mask, find the first available signal that should be serviced. */

static int
next_signal(struct task_struct *tsk, sigset_t *mask)
{
	unsigned long i, *s, *m, x;
	int sig = 0;
	
	s = tsk->pending.signal.sig;
	m = mask->sig;
	switch (_NSIG_WORDS) {
	default:
		for (i = 0; i < _NSIG_WORDS; ++i, ++s, ++m)
			if ((x = *s &~ *m) != 0) {
				sig = ffz(~x) + i*_NSIG_BPW + 1;
				break;
			}
		break;

	case 2: if ((x = s[0] &~ m[0]) != 0)
			sig = 1;
		else if ((x = s[1] &~ m[1]) != 0)
			sig = _NSIG_BPW + 1;
		else
			break;
		sig += ffz(~x);
		break;

	case 1: if ((x = *s &~ *m) != 0)
			sig = ffz(~x) + 1;
		break;
	}
	
	return sig;
}

static void flush_sigqueue(struct sigpending *queue)
{
	struct sigqueue *q, *n;

	sigemptyset(&queue->signal);
	q = queue->head;
	queue->head = NULL;
	queue->tail = &queue->head;

	while (q) {
		n = q->next;
		kmem_cache_free(sigqueue_cachep, q);
		atomic_dec(&nr_queued_signals);
		q = n;
	}
}

/*
 * Flush all pending signals for a task.
 */

void
flush_signals(struct task_struct *t)
{
	t->sigpending = 0;
	flush_sigqueue(&t->pending);
}

void exit_sighand(struct task_struct *tsk)
{
	struct signal_struct * sig = tsk->sig;

	spin_lock_irq(&tsk->sigmask_lock);
	if (sig) {
		tsk->sig = NULL;
		if (atomic_dec_and_test(&sig->count))
			kmem_cache_free(sigact_cachep, sig);
	}
	tsk->sigpending = 0;
	flush_sigqueue(&tsk->pending);
	spin_unlock_irq(&tsk->sigmask_lock);
}

/*
 * Flush all handlers for a task.
 */

void
flush_signal_handlers(struct task_struct *t)
{
	int i;
	struct k_sigaction *ka = &t->sig->action[0];
	for (i = _NSIG ; i != 0 ; i--) {
		if (ka->sa.sa_handler != SIG_IGN)
			ka->sa.sa_handler = SIG_DFL;
		ka->sa.sa_flags = 0;
		sigemptyset(&ka->sa.sa_mask);
		ka++;
	}
}

/* Notify the system that a driver wants to block all signals for this
 * process, and wants to be notified if any signals at all were to be
 * sent/acted upon.  If the notifier routine returns non-zero, then the
 * signal will be acted upon after all.  If the notifier routine returns 0,
 * then then signal will be blocked.  Only one block per process is
 * allowed.  priv is a pointer to private data that the notifier routine
 * can use to determine if the signal should be blocked or not.  */

void
block_all_signals(int (*notifier)(void *priv), void *priv, sigset_t *mask)
{
	unsigned long flags;

	spin_lock_irqsave(&current->sigmask_lock, flags);
	current->notifier_mask = mask;
	current->notifier_data = priv;
	current->notifier = notifier;
	spin_unlock_irqrestore(&current->sigmask_lock, flags);
}

/* Notify the system that blocking has ended. */

void
unblock_all_signals(void)
{
	unsigned long flags;

	spin_lock_irqsave(&current->sigmask_lock, flags);
	current->notifier = NULL;
	current->notifier_data = NULL;
	recalc_sigpending(current);
	spin_unlock_irqrestore(&current->sigmask_lock, flags);
}

static int collect_signal(int sig, struct sigpending *list, siginfo_t *info)
{
	if (sigismember(&list->signal, sig)) {
		/* Collect the siginfo appropriate to this signal.  */
		struct sigqueue *q, **pp;
		pp = &list->head;
		while ((q = *pp) != NULL) {
			if (q->info.si_signo == sig)
				goto found_it;
			pp = &q->next;
		}

		/* Ok, it wasn't in the queue.  We must have
		   been out of queue space.  So zero out the
		   info.  */
		sigdelset(&list->signal, sig);
		info->si_signo = sig;
		info->si_errno = 0;
		info->si_code = 0;
		info->si_pid = 0;
		info->si_uid = 0;
		return 1;

found_it:
		if ((*pp = q->next) == NULL)
			list->tail = pp;

		/* Copy the sigqueue information and free the queue entry */
		copy_siginfo(info, &q->info);
		kmem_cache_free(sigqueue_cachep,q);
		atomic_dec(&nr_queued_signals);

		/* Non-RT signals can exist multiple times.. */
		if (sig >= SIGRTMIN) {
			while ((q = *pp) != NULL) {
				if (q->info.si_signo == sig)
					goto found_another;
				pp = &q->next;
			}
		}

		sigdelset(&list->signal, sig);
found_another:
		return 1;
	}
	return 0;
}

/*
 * Dequeue a signal and return the element to the caller, which is 
 * expected to free it.
 *
 * All callers must be holding current->sigmask_lock.
 */

int
dequeue_signal(sigset_t *mask, siginfo_t *info)
{
	int sig = 0;

#if DEBUG_SIG
printk("SIG dequeue (%s:%d): %d ", current->comm, current->pid,
	signal_pending(current));
#endif

	sig = next_signal(current, mask);
	if (current->notifier) {
		if (sigismember(current->notifier_mask, sig)) {
			if (!(current->notifier)(current->notifier_data)) {
				current->sigpending = 0;
				return 0;
			}
		}
	}

	if (sig) {
		if (!collect_signal(sig, &current->pending, info))
			sig = 0;
				
		/* XXX: Once POSIX.1b timers are in, if si_code == SI_TIMER,
		   we need to xchg out the timer overrun values.  */
	}
	recalc_sigpending(current);

#if DEBUG_SIG
printk(" %d -> %d\n", signal_pending(current), sig);
#endif

	return sig;
}

static int rm_from_queue(int sig, struct sigpending *s)
{
	struct sigqueue *q, **pp;

	if (!sigismember(&s->signal, sig))
		return 0;

	sigdelset(&s->signal, sig);

	pp = &s->head;

	while ((q = *pp) != NULL) {
		if (q->info.si_signo == sig) {
			if ((*pp = q->next) == NULL)
				s->tail = pp;
			kmem_cache_free(sigqueue_cachep,q);
			atomic_dec(&nr_queued_signals);
			continue;
		}
		pp = &q->next;
	}
	return 1;
}

/*
 * Remove signal sig from t->pending.
 * Returns 1 if sig was found.
 *
 * All callers must be holding t->sigmask_lock.
 */
static int rm_sig_from_queue(int sig, struct task_struct *t)
{
	return rm_from_queue(sig, &t->pending);
}

/*
 * Bad permissions for sending the signal
 */

/**
 * bad_signal - 信号发送权限检查函数，判断当前进程是否无权向目标进程t发送sig信号
 * @sig: 待发送信号编号
 * @info: siginfo信号信息指针
 * @t: 目标进程task_struct
 *
 * 返回值：返回1 → 权限不足，不能发送；返回0 → 权限校验通过，可以发送
 *
 * 整体逻辑：
 * 整个return是一长串 && 连接的条件，**所有条件全部同时成立才返回1（拒绝发送）**
 * 只要任意一条不满足，表达式整体为false，返回0，放行。
 */
int bad_signal(int sig, struct siginfo *info, struct task_struct *t)
{
	return (!info || ((unsigned long)info != 1 && SI_FROMUSER(info))) //信号来自用户态
	    && ((sig != SIGCONT) || (current->session != t->session)) //SIGCONT 同义会话可以不用校验权限
	    && (current->euid ^ t->suid) && (current->euid ^ t->uid)  // 当前进程 `uid/euid` 和目标进程 `uid/suid` **全部两两都不相等**
	    && (current->uid ^ t->suid) && (current->uid ^ t->uid)
	    && !capable(CAP_KILL);
}

/*
 * Signal type:
 *    < 0 : global action (kill - spread to all non-blocked threads)
 *    = 0 : ignored
 *    > 0 : wake up.
 */
static int signal_type(int sig, struct signal_struct *signals)
{
	unsigned long handler;

	if (!signals)
		return 0;
	
	handler = (unsigned long) signals->action[sig-1].sa.sa_handler;
	if (handler > 1)
		return 1;

	/* "Ignore" handler.. Illogical, but that has an implicit handler for SIGCHLD */
	if (handler == 1)
		return sig == SIGCHLD;

	/* Default handler. Normally lethal, but.. */
	switch (sig) {

	/* Ignored */
	case SIGCONT: case SIGWINCH:
	case SIGCHLD: case SIGURG:
		return 0;

	/* Implicit behaviour */
	case SIGTSTP: case SIGTTIN: case SIGTTOU:
		return 1;

	/* Implicit actions (kill or do special stuff) */
	default:
		return -1;
	}
}
		

/*
 * Determine whether a signal should be posted or not.
 *
 * Signals with SIG_IGN can be ignored, except for the
 * special case of a SIGCHLD. 
 *
 * Some signals with SIG_DFL default to a non-action.
 */
static int ignored_signal(int sig, struct task_struct *t)
{
	/* Don't ignore traced or blocked signals */
	if ((t->ptrace & PT_PTRACED) || sigismember(&t->blocked, sig))
		return 0;

	return signal_type(sig, t->sig) == 0;
}

/*
 * Handle TASK_STOPPED cases etc implicit behaviour
 * of certain magical signals.
 *
 * SIGKILL gets spread out to every thread. 
 */
/**
 * handle_stop_signal - 专门处理停止/恢复类信号（SIGSTOP/SIGCONT/SIGTSTP等）的状态清理
 * @sig: 当前正在投递的信号
 * @t: 目标任务task_struct
 *
 * 注意：2.4版本，这个函数只做【信号队列清理、任务唤醒】，
 * 真正把整个线程组全部置为TASK_STOPPED的逻辑不在这个函数内部！
 * 只是stop类信号配套的辅助处理。
 */
static void handle_stop_signal(int sig, struct task_struct *t)
{
	switch (sig) {
	case SIGKILL: case SIGCONT:
		/* Wake up the process if stopped.  */
		// 如果当前任务处于停止状态(TASK_STOPPED)，唤醒它
		if (t->state == TASK_STOPPED)
			wake_up_process(t);
		t->exit_code = 0;
		// 移除pending队列里所有停止类信号：SIGSTOP、SIGTSTP、SIGTTOU、SIGTTIN
        // 收到SIGCONT/SIGKILL，就不再需要这些停止信号了
		rm_sig_from_queue(SIGSTOP, t);
		rm_sig_from_queue(SIGTSTP, t);
		rm_sig_from_queue(SIGTTOU, t);
		rm_sig_from_queue(SIGTTIN, t);
		break;

	case SIGSTOP: case SIGTSTP:
	case SIGTTIN: case SIGTTOU:
		/* If we're stopping again, cancel SIGCONT */
		// 收到停止类信号，则清除pending里待处理的SIGCONT
		rm_sig_from_queue(SIGCONT, t);
		break;
	}
}

static int send_signal(int sig, struct siginfo *info, struct sigpending *signals)
{
	struct sigqueue * q = NULL;

	/* Real-time signals must be queued if sent by sigqueue, or
	   some other real-time mechanism.  It is implementation
	   defined whether kill() does so.  We attempt to do so, on
	   the principle of least surprise, but since kill is not
	   allowed to fail with EAGAIN when low on memory we just
	   make sure at least one signal gets delivered and don't
	   pass on the info struct.  */

	if (atomic_read(&nr_queued_signals) < max_queued_signals) {
		q = kmem_cache_alloc(sigqueue_cachep, GFP_ATOMIC);
	}

	if (q) {
		atomic_inc(&nr_queued_signals);
		q->next = NULL;
		*signals->tail = q;
		signals->tail = &q->next;
		switch ((unsigned long) info) {
			case 0:
				q->info.si_signo = sig;
				q->info.si_errno = 0;
				q->info.si_code = SI_USER;
				q->info.si_pid = current->pid;
				q->info.si_uid = current->uid;
				break;
			case 1:
				q->info.si_signo = sig;
				q->info.si_errno = 0;
				q->info.si_code = SI_KERNEL;
				q->info.si_pid = 0;
				q->info.si_uid = 0;
				break;
			default:
				copy_siginfo(&q->info, info);
				break;
		}
	} else if (sig >= SIGRTMIN && info && (unsigned long)info != 1
		   && info->si_code != SI_USER) {
		/*
		 * Queue overflow, abort.  We may abort if the signal was rt
		 * and sent by user using something other than kill().
		 */
		return -EAGAIN;
	}

	sigaddset(&signals->signal, sig);
	return 0;
}

/*
 * Tell a process that it has a new active signal..
 *
 * NOTE! we rely on the previous spin_lock to
 * lock interrupts for us! We can only be called with
 * "sigmask_lock" held, and the local interrupt must
 * have been disabled when that got acquired!
 *
 * No need to set need_resched since signal event passing
 * goes through ->blocked
 */
static inline void signal_wake_up(struct task_struct *t)
{
	//这个 task 存在「没有被 blocked 阻塞的 pending 信号」，可以去处理信号
	// ret_from_syscall 的时候判断这个标记， 是 ENTRY(ret_from_sys_call) 这个
	// 判断标记，并调用 do_signal
	t->sigpending = 1;

	if (t->state & TASK_INTERRUPTIBLE) {
		wake_up_process(t);
		return;
	}

#ifdef CONFIG_SMP
	/*
	 * If the task is running on a different CPU 
	 * force a reschedule on the other CPU to make
	 * it notice the new signal quickly.
	 *
	 * The code below is a tad loose and might occasionally
	 * kick the wrong CPU if we catch the process in the
	 * process of changing - but no harm is done by that
	 * other than doing an extra (lightweight) IPI interrupt.
	 */
	spin_lock(&runqueue_lock);
	if (t->has_cpu && t->processor != smp_processor_id())
		smp_send_reschedule(t->processor);
	spin_unlock(&runqueue_lock);
#endif /* CONFIG_SMP */
}

/**
 * deliver_signal - 将信号投递到目标进程的pending队列，并按需触发信号唤醒
 * @sig: 信号编号
 * @info: 信号附加信息siginfo
 * @t: 目标进程task_struct
 *
 * 返回值：send_signal的返回值，0表示成功入队；负数为错误码
 *
 * 说明：
 *  1. 调用send_signal，把信号加入t->pending待处理信号队列
 *  2. 如果入队成功 并且 当前信号**没有被该线程阻塞(blocked掩码未置位)**，调用signal_wake_up
 *  3. signal_wake_up标记进程有未处理信号，用于内核在合适时机返回用户态处理信号
 */
static int deliver_signal(int sig, struct siginfo *info, struct task_struct *t)
{
	// send_signal：底层函数，分配sigqueue，添加到pending信号队列，置位pending.signal位图
	int retval = send_signal(sig, info, &t->pending);

	/*
     * 两个条件同时满足才执行signal_wake_up：
     * ① retval == 0：信号成功加入pending队列
     * ② !sigismember(&t->blocked, sig)：该信号**没有被当前线程阻塞**
     *    blocked是task私有信号阻塞掩码
     * 如果信号被blocked阻塞，就算进pending队列，也暂时不会处理，无需signal_wake_up
     */
	if (!retval && !sigismember(&t->blocked, sig))
		signal_wake_up(t);

	return retval;
}

/**
 * send_sig_info - 向指定task_struct投递信号（带siginfo附加信息），信号投递核心入口
 * @sig: 信号编号
 * @info: siginfo_t，信号附加信息（来源、错误码等，NULL用默认）
 * @t: 目标进程task_struct指针
 *
 * 返回值：0成功；-EINVAL信号非法；-EPERM权限不足
 *
 * 说明：
 *  1. 先做合法性、权限校验；0号信号仅用来探测进程存在性，不会真正投递信号
 *  2. 加sigmask_lock（关中断）保护pending信号队列
 *  3. 处理stop相关特殊信号逻辑（SIGSTOP/SIGCONT）
 *  4. 可忽略信号优化：如果信号是被忽略的，直接丢弃，不进pending队列
 *  5. 普通非实时信号（sig < SIGRTMIN）：pending里已有同信号则不再重复排队（只保留1个）
 *  6. deliver_signal：把信号加入pending队列
 *  7. 如果进程处于TASK_INTERRUPTIBLE且现在有pending信号，唤醒进程
 */
int
send_sig_info(int sig, struct siginfo *info, struct task_struct *t)
{
	unsigned long flags;
	int ret;


#if DEBUG_SIG
printk("SIG queue (%s:%d): %d ", t->comm, t->pid, sig);
#endif

	ret = -EINVAL;
	// 校验信号编号范围
	if (sig < 0 || sig > _NSIG)
		goto out_nolock;
	/* The somewhat baroque permissions check... */

	/* 复杂的权限检查：调用bad_signal，校验是否有权限向t发送该信号 */
	ret = -EPERM;
	if (bad_signal(sig, info, t))
		goto out_nolock;

	/* The null signal is a permissions and process existance probe.
	   No signal is actually delivered.  Same goes for zombies. */
	ret = 0;
	/*
     * 0号信号：仅权限探测 + 探测进程是否存在，**不会真正投递信号**
     * t->sig == NULL：僵尸进程，没有signal_struct，不投递
	 * 0号信号也是需要先过权限检测的
     */
	if (!sig || !t->sig)
		goto out_nolock;

	// // 自旋锁+保存中断标志，保护pending信号队列，防止并发修改
	spin_lock_irqsave(&t->sigmask_lock, flags);

	// 处理SIGSTOP/SIGCONT这类会改变进程stop状态的特殊信号
	handle_stop_signal(sig, t);

	/* Optimize away the signal, if it's a signal that can be
	   handled immediately (ie non-blocked and untraced) and
	   that is ignored (either explicitly or by default).  */

	/*
     * 优化：如果该信号设置为忽略（SIG_IGN / 默认忽略），直接丢弃
     * 不需要加入pending队列
     */
	if (ignored_signal(sig, t))
		goto out;

	/* Support queueing exactly one non-rt signal, so that we
	   can get more detailed information about the cause of
	   the signal. */
	/*
     * 普通非实时信号（标准信号，1~SIGRTMIN-1）：
     * 传统Unix语义：同一种普通信号pending队列只保留一份
     * 如果已经存在，直接返回，不再重复入队
     * 实时信号(RT信号)不受这个限制，可以多次排队
     */
	if (sig < SIGRTMIN && sigismember(&t->pending.signal, sig))
		goto out;

	// 真正把信号加入目标进程pending信号队列
	ret = deliver_signal(sig, info, t);
out:
	spin_unlock_irqrestore(&t->sigmask_lock, flags);
	if ((t->state & TASK_INTERRUPTIBLE) && signal_pending(t))
		wake_up_process(t);
out_nolock:
#if DEBUG_SIG
printk(" %d -> %d\n", signal_pending(t), ret);
#endif

	return ret;
}

/*
 * Force a signal that the process can't ignore: if necessary
 * we unblock the signal and change any SIG_IGN to SIG_DFL.
 */

int
force_sig_info(int sig, struct siginfo *info, struct task_struct *t)
{
	unsigned long int flags;

	spin_lock_irqsave(&t->sigmask_lock, flags);
	if (t->sig == NULL) {
		spin_unlock_irqrestore(&t->sigmask_lock, flags);
		return -ESRCH;
	}

	if (t->sig->action[sig-1].sa.sa_handler == SIG_IGN)
		t->sig->action[sig-1].sa.sa_handler = SIG_DFL;
	sigdelset(&t->blocked, sig);
	recalc_sigpending(t);
	spin_unlock_irqrestore(&t->sigmask_lock, flags);

	return send_sig_info(sig, info, t);
}

/*
 * kill_pg_info() sends a signal to a process group: this is what the tty
 * control characters do (^C, ^Z etc)
 */

int
kill_pg_info(int sig, struct siginfo *info, pid_t pgrp)
{
	int retval = -EINVAL;
	if (pgrp > 0) {
		struct task_struct *p;

		retval = -ESRCH;
		read_lock(&tasklist_lock);
		for_each_task(p) {
			if (p->pgrp == pgrp) {
				int err = send_sig_info(sig, info, p);
				if (retval)
					retval = err;
			}
		}
		read_unlock(&tasklist_lock);
	}
	return retval;
}

/*
 * kill_sl_info() sends a signal to the session leader: this is used
 * to send SIGHUP to the controlling process of a terminal when
 * the connection is lost.
 */

int
kill_sl_info(int sig, struct siginfo *info, pid_t sess)
{
	int retval = -EINVAL;
	if (sess > 0) {
		struct task_struct *p;

		retval = -ESRCH;
		read_lock(&tasklist_lock);
		for_each_task(p) {
			if (p->leader && p->session == sess) {
				int err = send_sig_info(sig, info, p);
				if (retval)
					retval = err;
			}
		}
		read_unlock(&tasklist_lock);
	}
	return retval;
}

/**
 * kill_proc_info - 根据pid查找进程并向该单个进程发送信号
 * @sig: 信号编号
 * @info: siginfo信号附加信息结构
 * @pid: 目标进程ID
 *
 * 返回值：成功0；找不到进程返回-ESRCH；send_sig_info返回其他错误码
 * 
 * 2.4 内核只发送给当前线程和posix语义不一样
 */
inline int
kill_proc_info(int sig, struct siginfo *info, pid_t pid)
{
	int error;
	struct task_struct *p;

	// 读锁保护全局tasklist，防止查找期间进程被销毁
	read_lock(&tasklist_lock);

	// 根据pid在全局task链表查找task_struct
	p = find_task_by_pid(pid);
	error = -ESRCH;
	if (p)
		// 找到目标进程，调用底层函数投递信号
		error = send_sig_info(sig, info, p);
	read_unlock(&tasklist_lock);
	return error;
}


/*
 * kill_something_info() interprets pid in interesting ways just like kill(2).
 *
 * POSIX specifies that kill(-1,sig) is unspecified, but what we have
 * is probably wrong.  Should make it like BSD or SYSV.
 */

/**
 * kill_something_info - kill系统调用的核心分发函数，根据pid参数语义分发信号发送逻辑
 * @sig: 待发送的信号编号
 * @info: siginfo信号附加信息结构
 * @pid: pid参数，语义不是单纯进程ID，根据不同取值含义不同
 *
 * 返回值：成功返回0；失败返回负错误码
 *
 * kill()系统调用pid参数4种语义：
 * pid == 0      → 发送信号给**当前进程所在进程组**所有进程
 * pid == -1     → 发送信号给除init(pid=1)和自己外的所有进程
 * pid < 0       → 发送信号给进程组，进程组id = -pid
 * pid > 0       → 发送信号给pid等于该值的单个进程
 */
static int kill_something_info(int sig, struct siginfo *info, int pid)
{
	if (!pid) {
		// pid=0：发给当前进程的进程组，调用kill_pg_info
		return kill_pg_info(sig, info, current->pgrp);
	} else if (pid == -1) {
		int retval = 0, count = 0;
		struct task_struct * p;

		// 所有进程发送信号
		// 遍历系统全部task_struct
		read_lock(&tasklist_lock);
		for_each_task(p) {

			// 条件：pid>1(排除init进程) && 不是当前进程自己
			if (p->pid > 1 && p != current) {
				int err = send_sig_info(sig, info, p);
				++count;
				// 只要不是权限错误-EPERM，就把错误保存到retval
                // 多个进程发送时：只要有一个非EPERM失败，最终返回这个错误
				if (err != -EPERM)
					retval = err;
			}
		}
		read_unlock(&tasklist_lock);

		/*
         * count == 0：没有找到任何符合条件进程，返回-ESRCH(No such process)
         * count >0：至少遍历过进程，返回retval（0代表全部成功）
         */
		return count ? retval : -ESRCH;
	} else if (pid < 0) {
		// pid < 0，取绝对值作为进程组ID，发送给整个进程组
		return kill_pg_info(sig, info, -pid);
	} else {

		// pid > 0，发给单个进程pid
		return kill_proc_info(sig, info, pid);
	}
}

/*
 * These are for backward compatibility with the rest of the kernel source.
 */

int
send_sig(int sig, struct task_struct *p, int priv)
{
	return send_sig_info(sig, (void*)(long)(priv != 0), p);
}

void
force_sig(int sig, struct task_struct *p)
{
	force_sig_info(sig, (void*)1L, p);
}

int
kill_pg(pid_t pgrp, int sig, int priv)
{
	return kill_pg_info(sig, (void *)(long)(priv != 0), pgrp);
}

int
kill_sl(pid_t sess, int sig, int priv)
{
	return kill_sl_info(sig, (void *)(long)(priv != 0), sess);
}

int
kill_proc(pid_t pid, int sig, int priv)
{
	return kill_proc_info(sig, (void *)(long)(priv != 0), pid);
}

/*
 * Joy. Or not. Pthread wants us to wake up every thread
 * in our parent group.
 */
static void wake_up_parent(struct task_struct *parent)
{
	struct task_struct *tsk = parent;

	do {
		wake_up_interruptible(&tsk->wait_chldexit);
		tsk = next_thread(tsk);
	} while (tsk != parent);
}

/*
 * Let a parent know about a status change of a child.
 */

 /**
 * do_notify_parent - 子进程状态变更（停止/退出）时，通知父进程，发送SIGCHLD类信号
 * @tsk: 状态发生变化的子进程task_struct
 * @sig: 要发给父进程的信号，一般是SIGCHLD
 *
 * 功能：填充siginfo信号附加信息结构，根据子进程状态区分事件类型（退出/被杀死/转储/停止/被调试捕获）
 * 调用send_sig_info投递带详细信息的信号给父进程，然后唤醒阻塞在wait4的父进程
 */
void do_notify_parent(struct task_struct *tsk, int sig)
{
	struct siginfo info;
	int why, status;

	info.si_signo = sig;
	info.si_errno = 0;
	info.si_pid = tsk->pid;
	info.si_uid = tsk->uid;

	/* FIXME: find out whether or not this is supposed to be c*time. */
	info.si_utime = tsk->times.tms_utime;
	info.si_stime = tsk->times.tms_stime;

	status = tsk->exit_code & 0x7f;
	why = SI_KERNEL;	/* shouldn't happen */
	switch (tsk->state) {
	case TASK_STOPPED:
		/* FIXME -- can we deduce CLD_TRAPPED or CLD_CONTINUED? */
		if (tsk->ptrace & PT_PTRACED)
			why = CLD_TRAPPED;
		else
			why = CLD_STOPPED;
		break;

	default:
		if (tsk->exit_code & 0x80)
			why = CLD_DUMPED;
		else if (tsk->exit_code & 0x7f)
			why = CLD_KILLED;
		else {
			why = CLD_EXITED;
			status = tsk->exit_code >> 8;
		}
		break;
	}
	info.si_code = why;
	info.si_status = status;

	// 发送信号
	send_sig_info(sig, &info, tsk->p_pptr);

	// 唤醒阻塞在wait4/waitpid的父进程
	wake_up_parent(tsk->p_pptr);
}


/*
 * We need the tasklist lock because it's the only
 * thing that protects out "parent" pointer.
 *
 * exit.c calls "do_notify_parent()" directly, because
 * it already has the tasklist lock.
 */
void
notify_parent(struct task_struct *tsk, int sig)
{
	read_lock(&tasklist_lock);
	do_notify_parent(tsk, sig);
	read_unlock(&tasklist_lock);
}

EXPORT_SYMBOL(dequeue_signal);
EXPORT_SYMBOL(flush_signals);
EXPORT_SYMBOL(force_sig);
EXPORT_SYMBOL(force_sig_info);
EXPORT_SYMBOL(kill_pg);
EXPORT_SYMBOL(kill_pg_info);
EXPORT_SYMBOL(kill_proc);
EXPORT_SYMBOL(kill_proc_info);
EXPORT_SYMBOL(kill_sl);
EXPORT_SYMBOL(kill_sl_info);
EXPORT_SYMBOL(notify_parent);
EXPORT_SYMBOL(recalc_sigpending);
EXPORT_SYMBOL(send_sig);
EXPORT_SYMBOL(send_sig_info);
EXPORT_SYMBOL(block_all_signals);
EXPORT_SYMBOL(unblock_all_signals);


/*
 * System call entry points.
 */

/*
 * We don't need to get the kernel lock - this is all local to this
 * particular thread.. (and that's good, because this is _heavily_
 * used by various programs)
 */

asmlinkage long
sys_rt_sigprocmask(int how, sigset_t *set, sigset_t *oset, size_t sigsetsize)
{
	int error = -EINVAL;
	sigset_t old_set, new_set;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	if (sigsetsize != sizeof(sigset_t))
		goto out;

	if (set) {
		error = -EFAULT;
		if (copy_from_user(&new_set, set, sizeof(*set)))
			goto out;
		sigdelsetmask(&new_set, sigmask(SIGKILL)|sigmask(SIGSTOP));

		spin_lock_irq(&current->sigmask_lock);
		old_set = current->blocked;

		error = 0;
		switch (how) {
		default:
			error = -EINVAL;
			break;
		case SIG_BLOCK:
			sigorsets(&new_set, &old_set, &new_set);
			break;
		case SIG_UNBLOCK:
			signandsets(&new_set, &old_set, &new_set);
			break;
		case SIG_SETMASK:
			break;
		}

		current->blocked = new_set;
		recalc_sigpending(current);
		spin_unlock_irq(&current->sigmask_lock);
		if (error)
			goto out;
		if (oset)
			goto set_old;
	} else if (oset) {
		spin_lock_irq(&current->sigmask_lock);
		old_set = current->blocked;
		spin_unlock_irq(&current->sigmask_lock);

	set_old:
		error = -EFAULT;
		if (copy_to_user(oset, &old_set, sizeof(*oset)))
			goto out;
	}
	error = 0;
out:
	return error;
}

long do_sigpending(void *set, unsigned long sigsetsize)
{
	long error = -EINVAL;
	sigset_t pending;

	if (sigsetsize > sizeof(sigset_t))
		goto out;

	spin_lock_irq(&current->sigmask_lock);
	sigandsets(&pending, &current->blocked, &current->pending.signal);
	spin_unlock_irq(&current->sigmask_lock);

	error = -EFAULT;
	if (!copy_to_user(set, &pending, sigsetsize))
		error = 0;
out:
	return error;
}	

asmlinkage long
sys_rt_sigpending(sigset_t *set, size_t sigsetsize)
{
	return do_sigpending(set, sigsetsize);
}

asmlinkage long
sys_rt_sigtimedwait(const sigset_t *uthese, siginfo_t *uinfo,
		    const struct timespec *uts, size_t sigsetsize)
{
	int ret, sig;
	sigset_t these;
	struct timespec ts;
	siginfo_t info;
	long timeout = 0;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	if (copy_from_user(&these, uthese, sizeof(these)))
		return -EFAULT;
		
	/*
	 * Invert the set of allowed signals to get those we
	 * want to block.
	 */
	sigdelsetmask(&these, sigmask(SIGKILL)|sigmask(SIGSTOP));
	signotset(&these);

	if (uts) {
		if (copy_from_user(&ts, uts, sizeof(ts)))
			return -EFAULT;
		if (ts.tv_nsec >= 1000000000L || ts.tv_nsec < 0
		    || ts.tv_sec < 0)
			return -EINVAL;
	}

	spin_lock_irq(&current->sigmask_lock);
	sig = dequeue_signal(&these, &info);
	if (!sig) {
		timeout = MAX_SCHEDULE_TIMEOUT;
		if (uts)
			timeout = (timespec_to_jiffies(&ts)
				   + (ts.tv_sec || ts.tv_nsec));

		if (timeout) {
			/* None ready -- temporarily unblock those we're
			 * interested while we are sleeping in so that we'll
			 * be awakened when they arrive.  */
			sigset_t oldblocked = current->blocked;
			sigandsets(&current->blocked, &current->blocked, &these);
			recalc_sigpending(current);
			spin_unlock_irq(&current->sigmask_lock);

			current->state = TASK_INTERRUPTIBLE;
			timeout = schedule_timeout(timeout);

			spin_lock_irq(&current->sigmask_lock);
			sig = dequeue_signal(&these, &info);
			current->blocked = oldblocked;
			recalc_sigpending(current);
		}
	}
	spin_unlock_irq(&current->sigmask_lock);

	if (sig) {
		ret = sig;
		if (uinfo) {
			if (copy_siginfo_to_user(uinfo, &info))
				ret = -EFAULT;
		}
	} else {
		ret = -EAGAIN;
		if (timeout)
			ret = -EINTR;
	}

	return ret;
}

//发送信号
asmlinkage long
sys_kill(int pid, int sig)
{
	struct siginfo info;

	info.si_signo = sig;
	info.si_errno = 0;
	info.si_code = SI_USER;
	info.si_pid = current->pid;
	info.si_uid = current->uid;

	return kill_something_info(sig, &info, pid);
}

asmlinkage long
sys_rt_sigqueueinfo(int pid, int sig, siginfo_t *uinfo)
{
	siginfo_t info;

	if (copy_from_user(&info, uinfo, sizeof(siginfo_t)))
		return -EFAULT;

	/* Not even root can pretend to send signals from the kernel.
	   Nor can they impersonate a kill(), which adds source info.  */
	if (info.si_code >= 0)
		return -EPERM;
	info.si_signo = sig;

	/* POSIX.1b doesn't mention process groups.  */
	return kill_proc_info(sig, &info, pid);
}

/**
 * do_sigaction - sigaction系统调用内核底层实现，设置/获取信号处理动作
 * @sig: 信号编号
 * @act: 用户传入的新信号行为，NULL表示只查询不修改
 * @oact: 输出参数，保存旧的信号行为，NULL表示不需要返回旧值
 *
 * 返回值：成功返回0；参数非法返回-EINVAL
 *
 * 功能：
 * 1. 合法性校验：信号范围，禁止修改SIGKILL/SIGSTOP的处理函数
 * 2. 读写进程sig->action[]数组，数组下标sig-1（信号从1开始编号）
 * 3. 自旋锁siglock保护action数组并发访问
 * 4. 若oact非空：把旧的信号动作拷贝给oact，对应获取原有handler
 * 5. 若act非空：更新信号动作；强制屏蔽sa_mask里的SIGKILL/SIGSTOP（这两个永远不能被阻塞）
 * 6. POSIX规则：当handler设为SIG_IGN，或者特定信号设为SIG_DFL且默认行为是忽略，则**清除进程挂起的pending信号**
 * 7. 清理pending信号后调用recalc_sigpending，重新计算进程是否有待处理信号
 */
int
do_sigaction(int sig, const struct k_sigaction *act, struct k_sigaction *oact)
{
	struct k_sigaction *k;

	// 校验信号编号范围；不能修改SIGKILL、SIGSTOP的信号处理函数
	if (sig < 1 || sig > _NSIG ||
	    (act && (sig == SIGKILL || sig == SIGSTOP)))
		return -EINVAL;

	// 取出该信号对应的k_sigaction结构体，进程sig结构里的action数组
	// struct signal_struct *sig;
	k = &current->sig->action[sig-1];

	spin_lock(&current->sig->siglock);

	// oact不为空：保存原来的信号处理配置，给用户态返回旧handler
	if (oact)
		*oact = *k;

	if (act) {
		// 使用用户传入的新配置覆盖当前信号action
		*k = *act;

		/*
         * 强制清除sa_mask中的SIGKILL、SIGSTOP
         * 这两个信号是不可阻塞的，用户就算在sa_mask里设置也无效
         */
		sigdelsetmask(&k->sa.sa_mask, sigmask(SIGKILL) | sigmask(SIGSTOP));

		/*
		 * POSIX 3.3.1.3:
		 *  "Setting a signal action to SIG_IGN for a signal that is
		 *   pending shall cause the pending signal to be discarded,
		 *   whether or not it is blocked."
		 *
		 *  "Setting a signal action to SIG_DFL for a signal that is
		 *   pending and whose default action is to ignore the signal
		 *   (for example, SIGCHLD), shall cause the pending signal to
		 *   be discarded, whether or not it is blocked"
		 *
		 * Note the silly behaviour of SIGCHLD: SIG_IGN means that the
		 * signal isn't actually ignored, but does automatic child
		 * reaping, while SIG_DFL is explicitly said by POSIX to force
		 * the signal to be ignored.
		 */

		/*
         * POSIX规范：
         * 1. 如果把信号处理设置为 SIG_IGN：进程pending队列里该信号直接丢弃，不管是否被屏蔽
         * 2. 如果设置为 SIG_DFL，且该信号默认动作就是忽略（SIGCHLD/SIGCONT/SIGWINCH），pending信号同样丢弃
         *
         * 特殊：SIGCHLD。SIG_IGN不是单纯忽略，内核会自动回收僵尸子进程；
         * SIG_DFL才是POSIX定义的“忽略”行为。
         */
		if (k->sa.sa_handler == SIG_IGN
		    || (k->sa.sa_handler == SIG_DFL
			&& (sig == SIGCONT ||
			    sig == SIGCHLD ||
			    sig == SIGWINCH))) {
			spin_lock_irq(&current->sigmask_lock);

			// 从进程pending信号队列删除sig
			if (rm_sig_from_queue(sig, current))
				recalc_sigpending(current);  // 重新计算：进程是否存在pending信号
			spin_unlock_irq(&current->sigmask_lock);
		}
	}

	spin_unlock(&current->sig->siglock);
	return 0;
}

int 
do_sigaltstack (const stack_t *uss, stack_t *uoss, unsigned long sp)
{
	stack_t oss;
	int error;

	if (uoss) {
		oss.ss_sp = (void *) current->sas_ss_sp;
		oss.ss_size = current->sas_ss_size;
		oss.ss_flags = sas_ss_flags(sp);
	}

	if (uss) {
		void *ss_sp;
		size_t ss_size;
		int ss_flags;

		error = -EFAULT;
		if (verify_area(VERIFY_READ, uss, sizeof(*uss))
		    || __get_user(ss_sp, &uss->ss_sp)
		    || __get_user(ss_flags, &uss->ss_flags)
		    || __get_user(ss_size, &uss->ss_size))
			goto out;

		error = -EPERM;
		if (on_sig_stack (sp))
			goto out;

		error = -EINVAL;
		/*
		 *
		 * Note - this code used to test ss_flags incorrectly
		 *  	  old code may have been written using ss_flags==0
		 *	  to mean ss_flags==SS_ONSTACK (as this was the only
		 *	  way that worked) - this fix preserves that older
		 *	  mechanism
		 */
		if (ss_flags != SS_DISABLE && ss_flags != SS_ONSTACK && ss_flags != 0)
			goto out;

		if (ss_flags == SS_DISABLE) {
			ss_size = 0;
			ss_sp = NULL;
		} else {
			error = -ENOMEM;
			if (ss_size < MINSIGSTKSZ)
				goto out;
		}

		current->sas_ss_sp = (unsigned long) ss_sp;
		current->sas_ss_size = ss_size;
	}

	if (uoss) {
		error = -EFAULT;
		if (copy_to_user(uoss, &oss, sizeof(oss)))
			goto out;
	}

	error = 0;
out:
	return error;
}

asmlinkage long
sys_sigpending(old_sigset_t *set)
{
	return do_sigpending(set, sizeof(*set));
}

#if !defined(__alpha__)
/* Alpha has its own versions with special arguments.  */

asmlinkage long
sys_sigprocmask(int how, old_sigset_t *set, old_sigset_t *oset)
{
	int error;
	old_sigset_t old_set, new_set;

	if (set) {
		error = -EFAULT;
		if (copy_from_user(&new_set, set, sizeof(*set)))
			goto out;
		new_set &= ~(sigmask(SIGKILL)|sigmask(SIGSTOP));

		spin_lock_irq(&current->sigmask_lock);
		old_set = current->blocked.sig[0];

		error = 0;
		switch (how) {
		default:
			error = -EINVAL;
			break;
		case SIG_BLOCK:
			sigaddsetmask(&current->blocked, new_set);
			break;
		case SIG_UNBLOCK:
			sigdelsetmask(&current->blocked, new_set);
			break;
		case SIG_SETMASK:
			current->blocked.sig[0] = new_set;
			break;
		}

		recalc_sigpending(current);
		spin_unlock_irq(&current->sigmask_lock);
		if (error)
			goto out;
		if (oset)
			goto set_old;
	} else if (oset) {
		old_set = current->blocked.sig[0];
	set_old:
		error = -EFAULT;
		if (copy_to_user(oset, &old_set, sizeof(*oset)))
			goto out;
	}
	error = 0;
out:
	return error;
}

#ifndef __sparc__
asmlinkage long
sys_rt_sigaction(int sig, const struct sigaction *act, struct sigaction *oact,
		 size_t sigsetsize)
{
	struct k_sigaction new_sa, old_sa;
	int ret = -EINVAL;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	if (sigsetsize != sizeof(sigset_t))
		goto out;

	if (act) {
		if (copy_from_user(&new_sa.sa, act, sizeof(new_sa.sa)))
			return -EFAULT;
	}

	ret = do_sigaction(sig, act ? &new_sa : NULL, oact ? &old_sa : NULL);

	if (!ret && oact) {
		if (copy_to_user(oact, &old_sa.sa, sizeof(old_sa.sa)))
			return -EFAULT;
	}
out:
	return ret;
}
#endif /* __sparc__ */
#endif

#if !defined(__alpha__) && !defined(__ia64__)
/*
 * For backwards compatibility.  Functionality superseded by sigprocmask.
 */
asmlinkage long
sys_sgetmask(void)
{
	/* SMP safe */
	return current->blocked.sig[0];
}

asmlinkage long
sys_ssetmask(int newmask)
{
	int old;

	spin_lock_irq(&current->sigmask_lock);
	old = current->blocked.sig[0];

	siginitset(&current->blocked, newmask & ~(sigmask(SIGKILL)|
						  sigmask(SIGSTOP)));
	recalc_sigpending(current);
	spin_unlock_irq(&current->sigmask_lock);

	return old;
}
#endif /* !defined(__alpha__) */

#if !defined(__alpha__) && !defined(__ia64__) && !defined(__mips__)
/*
 * For backwards compatibility.  Functionality superseded by sigaction.
 */
asmlinkage unsigned long
sys_signal(int sig, __sighandler_t handler)
{
	struct k_sigaction new_sa, old_sa;
	int ret;

	new_sa.sa.sa_handler = handler;
	new_sa.sa.sa_flags = SA_ONESHOT | SA_NOMASK;

	ret = do_sigaction(sig, &new_sa, &old_sa);

	return ret ? ret : (unsigned long)old_sa.sa.sa_handler;
}
#endif /* !alpha && !__ia64__ && !defined(__mips__) */
