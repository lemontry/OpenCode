/*
 *  linux/kernel/exit.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/config.h>
#include <linux/malloc.h>
#include <linux/interrupt.h>
#include <linux/smp_lock.h>
#include <linux/module.h>
#ifdef CONFIG_BSD_PROCESS_ACCT
#include <linux/acct.h>
#endif

#include <asm/uaccess.h>
#include <asm/pgtable.h>
#include <asm/mmu_context.h>

extern void sem_exit (void);
extern struct task_struct *child_reaper;

int getrusage(struct task_struct *, int, struct rusage *);

static void release_task(struct task_struct * p)
{
	if (p != current) {
#ifdef CONFIG_SMP
		/*
		 * Wait to make sure the process isn't on the
		 * runqueue (active on some other CPU still)
		 */
		for (;;) {
			task_lock(p);
			if (!p->has_cpu)
				break;
			task_unlock(p);
			do {
				barrier();
			} while (p->has_cpu);
		}
		task_unlock(p);
#endif
		atomic_dec(&p->user->processes);
		free_uid(p->user);
		unhash_process(p);

		release_thread(p);
		current->cmin_flt += p->min_flt + p->cmin_flt;
		current->cmaj_flt += p->maj_flt + p->cmaj_flt;
		current->cnswap += p->nswap + p->cnswap;
		/*
		 * Potentially available timeslices are retrieved
		 * here - this way the parent does not get penalized
		 * for creating too many processes.
		 *
		 * (this cannot be used to artificially 'generate'
		 * timeslices, because any timeslice recovered here
		 * was given away by the parent in the first place.)
		 */
		current->counter += p->counter;
		if (current->counter >= MAX_COUNTER)
			current->counter = MAX_COUNTER;
		free_task_struct(p);
	} else {
		printk("task releasing itself\n");
	}
}

/*
 * This checks not only the pgrp, but falls back on the pid if no
 * satisfactory pgrp is found. I dunno - gdb doesn't work correctly
 * without this...
 */
int session_of_pgrp(int pgrp)
{
	struct task_struct *p;
	int fallback;

	fallback = -1;
	read_lock(&tasklist_lock);
	for_each_task(p) {
 		if (p->session <= 0)
 			continue;
		if (p->pgrp == pgrp) {
			fallback = p->session;
			break;
		}
		if (p->pid == pgrp)
			fallback = p->session;
	}
	read_unlock(&tasklist_lock);
	return fallback;
}

/*
 * Determine if a process group is "orphaned", according to the POSIX
 * definition in 2.2.2.52.  Orphaned process groups are not to be affected
 * by terminal-generated stop signals.  Newly orphaned process groups are
 * to receive a SIGHUP and a SIGCONT.
 *
 * "I ask you, have you ever known what it is to be an orphan?"
 */
/**
 * will_become_orphaned_pgrp - 判断：当ignored_task退出之后，指定进程组pgrp是否会变成孤儿进程组
 * @pgrp: 待检测的进程组ID
 * @ignored_task: 即将退出的那个进程，遍历的时候跳过这个进程
 *
 * 返回值：
 *      1：该进程组会变成孤儿进程组
 *      0：该进程组仍然存在外部父进程连接，不会成为孤儿进程组
 *
 * 孤儿进程组定义(POSIX)：组内所有进程的父进程，要么属于本组，要么属于别的会话；
 * 换句话说，不存在任何一个父进程，和子在同一个会话、但不在同一个进程组。
 * 只要找到一个这样的外部父，这个进程组就不是孤儿组。
 */
static int will_become_orphaned_pgrp(int pgrp, struct task_struct * ignored_task)
{
	struct task_struct *p;

	read_lock(&tasklist_lock);

	// 遍历系统中全部任务
	for_each_task(p) {
		// 满足下面任一条件，跳过当前任务p，不做判断
		if ((p == ignored_task) ||  // 跳过即将退出的进程本身
		    (p->pgrp != pgrp) ||  // p不在我们要检查的进程组，跳过
		    (p->state == TASK_ZOMBIE) ||  // p是僵尸进程，忽略
		    (p->p_pptr->pid == 1))  // p的父是init(1号进程)，init不属于外部连接，跳过
			continue;

		// 核心判断：p的【当前父进程】和p同一会话，但是不在同一个进程组
		if ((p->p_pptr->pgrp != pgrp) &&  //父进程进程组不在这一个进程组
		    (p->p_pptr->session == p->session)) {  //并且是相同会话
			read_unlock(&tasklist_lock);
 			return 0;  // 找到这样的外部父 → 进程组不会变成孤儿，返回0
		}
	}
	read_unlock(&tasklist_lock);
	return 1;	/* (sighing) "Often!" */
}

int is_orphaned_pgrp(int pgrp)
{
	return will_become_orphaned_pgrp(pgrp, 0);
}

//这个进程组是否有停止的进程 TASK_STOPPED
static inline int has_stopped_jobs(int pgrp)
{
	int retval = 0;
	struct task_struct * p;

	read_lock(&tasklist_lock);
	for_each_task(p) {
		if (p->pgrp != pgrp)
			continue;
		if (p->state != TASK_STOPPED)
			continue;
		retval = 1;
		break;
	}
	read_unlock(&tasklist_lock);
	return retval;
}

/*
 * When we die, we re-parent all our children.
 * Try to give them to another thread in our process
 * group, and if no such member exists, give it to
 * the global child reaper process (ie "init")
 */
/**
 * forget_original_parent - 当father进程即将退出，修改所有子进程的原始父p_opptr
 * @father: 正在退出的进程（当前进程current）
 *
 * 核心作用：
 * 遍历系统全部task，找出所有【原始父p_opptr等于father】的任务，
 * 将它们的原始父指针迁移到reaper（领养者）；
 * reaper优先选同线程组其他线程，若无则使用全局child_reaper（一般是init进程PID=1）。
 * p_opptr：original parent，**原始父**，fork/clone创建该子的那个进程，永久记录创建来源；
 * p_pptr：real parent，当前父，wait/ptrace会修改它，而p_opptr在这里统一迁移。
 */
static inline void forget_original_parent(struct task_struct * father)
{
	struct task_struct * p, *reaper;

	read_lock(&tasklist_lock);

	/* Next in our thread group */
	// 优先选择father同线程组的下一个线程作为领养者reaper
	reaper = next_thread(father);
	if (reaper == father)
		reaper = child_reaper;

	// 遍历系统内所有任务（全量扫描全局task链表）
	for_each_task(p) {
		if (p->p_opptr == father) {
			/* We dont want people slaying init */
			p->exit_signal = SIGCHLD;
			p->self_exec_id++;

			// 修改原始父指针，p的原始父变成reaper（领养者）
			p->p_opptr = reaper;

			// 如果子设置了pdeath_signal（父死亡时发送的信号），发送该信号给子p
			if (p->pdeath_signal) send_sig(p->pdeath_signal, p, 0);
		}
	}
	read_unlock(&tasklist_lock);
}

static inline void close_files(struct files_struct * files)
{
	int i, j;

	j = 0;
	for (;;) {
		unsigned long set;
		i = j * __NFDBITS;
		if (i >= files->max_fdset || i >= files->max_fds)
			break;
		set = files->open_fds->fds_bits[j++];
		while (set) {
			if (set & 1) {
				struct file * file = xchg(&files->fd[i], NULL);
				if (file)
					filp_close(file, files);
			}
			i++;
			set >>= 1;
		}
	}
}

void put_files_struct(struct files_struct *files)
{
	if (atomic_dec_and_test(&files->count)) {
		close_files(files);
		/*
		 * Free the fd and fdset arrays if we expanded them.
		 */
		if (files->fd != &files->fd_array[0])
			free_fd_array(files->fd, files->max_fds);
		if (files->max_fdset > __FD_SETSIZE) {
			free_fdset(files->open_fds, files->max_fdset);
			free_fdset(files->close_on_exec, files->max_fdset);
		}
		kmem_cache_free(files_cachep, files);
	}
}

static inline void __exit_files(struct task_struct *tsk)
{
	struct files_struct * files = tsk->files;

	if (files) {
		task_lock(tsk);
		tsk->files = NULL;
		task_unlock(tsk);
		put_files_struct(files);
	}
}

void exit_files(struct task_struct *tsk)
{
	__exit_files(tsk);
}

static inline void __put_fs_struct(struct fs_struct *fs)
{
	/* No need to hold fs->lock if we are killing it */
	if (atomic_dec_and_test(&fs->count)) {
		dput(fs->root);
		mntput(fs->rootmnt);
		dput(fs->pwd);
		mntput(fs->pwdmnt);
		if (fs->altroot) {
			dput(fs->altroot);
			mntput(fs->altrootmnt);
		}
		kmem_cache_free(fs_cachep, fs);
	}
}

void put_fs_struct(struct fs_struct *fs)
{
	__put_fs_struct(fs);
}

static inline void __exit_fs(struct task_struct *tsk)
{
	struct fs_struct * fs = tsk->fs;

	if (fs) {
		task_lock(tsk);
		tsk->fs = NULL;
		task_unlock(tsk);
		__put_fs_struct(fs);
	}
}

void exit_fs(struct task_struct *tsk)
{
	__exit_fs(tsk);
}

/*
 * We can use these to temporarily drop into
 * "lazy TLB" mode and back.
 */
struct mm_struct * start_lazy_tlb(void)
{
	struct mm_struct *mm = current->mm;
	current->mm = NULL;
	/* active_mm is still 'mm' */
	atomic_inc(&mm->mm_count);
	enter_lazy_tlb(mm, current, smp_processor_id());
	return mm;
}

void end_lazy_tlb(struct mm_struct *mm)
{
	struct mm_struct *active_mm = current->active_mm;

	current->mm = mm;
	if (mm != active_mm) {
		current->active_mm = mm;
		activate_mm(active_mm, mm);
	}
	mmdrop(active_mm);
}

/*
 * Turn us into a lazy TLB process if we
 * aren't already..
 */
/**
 * __exit_mm - 释放进程的用户地址空间mm_struct，do_exit中调用
 * @tsk: 要释放地址空间的进程task_struct指针
 *
 * 功能：执行mm_release处理vfork等待逻辑；切断task与mm的关联；
 * 进入lazy_tlb模式；减少mm引用计数，mmput负责最终回收mm资源
 */
static inline void __exit_mm(struct task_struct * tsk)
{
	struct mm_struct * mm = tsk->mm;

	// 核心：处理vfork场景，唤醒阻塞等待的父进程；释放vfork相关信号量
	mm_release();
	if (mm) {
		atomic_inc(&mm->mm_count);
		if (mm != tsk->active_mm) BUG();
		/* more a memory barrier than a real lock */
		task_lock(tsk);
		// 切断task_struct和mm_struct之间的关联，进程不再拥有地址空间
		tsk->mm = NULL;  
		task_unlock(tsk);
		enter_lazy_tlb(mm, current, smp_processor_id());

		// mm引用计数减一；引用计数归零时，真正销毁mm、VMA、页表等内存资源
		mmput(mm);
	}
}

void exit_mm(struct task_struct *tsk)
{
	__exit_mm(tsk);
}

/*
 * Send signals to all our closest relatives so that they know
 * to properly mourn us..
 */

/**
 * exit_notify - do_exit 内部调用，进程退出通知处理核心函数
 *
 * 功能：
 * 1. 处理孤儿进程：本进程退出，将它的所有子进程重新领养（父指针修改为原始父，最终孤儿交给init）
 * 2. 检测因本进程退出而变成孤儿的进程组；若组内存在停止的作业，发送SIGHUP + SIGCONT（POSIX规范）
 * 3. 设置当前进程状态为 TASK_ZOMBIE（僵尸态）
 * 4. 向父进程发送退出通知信号（通常SIGCHLD），唤醒阻塞在wait4的父进程
 * 5. 清理子进程的ptrace跟踪标记，重建子进程的家族链表关系
 */
static void exit_notify(void)
{
	struct task_struct * p, *t;

	// 处理原始父关系，本进程的孤儿子后续会被重新领养
	forget_original_parent(current);

	/*
	 * Check to see if any process groups have become orphaned
	 * as a result of our exiting, and if they have any stopped
	 * jobs, send them a SIGHUP and then a SIGCONT.  (POSIX 3.2.2.2)
	 *
	 * Case i: Our father is in a different pgrp than we are
	 * and we were the only connection outside, so our pgrp
	 * is about to become orphaned.
	 */
	 
	// t 为本进程当前父进程
	t = current->p_pptr;
	
	// 判断条件：父进程进程组 != 当前进程组；同一会话；本进程退出后本组会变成孤儿；组内存在停止作业
	// 新版本：只挑选状态为 STOPPED 的任务，逐个发送 SIGHUP + SIGCONT。
	if ((t->pgrp != current->pgrp) &&  //父进程不在本组
	    (t->session == current->session) &&
	    will_become_orphaned_pgrp(current->pgrp, current) &&
	    has_stopped_jobs(current->pgrp)) {
			
		// 向整个进程组发送SIGHUP
		kill_pg(current->pgrp,SIGHUP,1);

		// 向整个进程组发送SIGCONT，唤醒停止作业
		kill_pg(current->pgrp,SIGCONT,1);
	}

	/* Let father know we died 
	 *
	 * Thread signals are configurable, but you aren't going to use
	 * that to send signals to arbitary processes. 
	 * That stops right now.
	 *
	 * If the parent exec id doesn't match the exec id we saved
	 * when we started then we know the parent has changed security
	 * domain.
	 *
	 * If our self_exec id doesn't match our parent_exec_id then
	 * we have changed execution domain as these two values started
	 * the same after a fork.
	 *	
	 */
	
	// 安全校验：非SIGCHLD通知场景，且exec域发生变化、没有CAP_KILL权限，强制改成SIGCHLD
	if(current->exit_signal != SIGCHLD &&
	    ( current->parent_exec_id != t->self_exec_id  ||
	      current->self_exec_id != current->parent_exec_id) 
	    && !capable(CAP_KILL))
		current->exit_signal = SIGCHLD;


	/*
	 * This loop does two things:
	 *
  	 * A.  Make init inherit all the child processes
	 * B.  Check to see if any process groups have become orphaned
	 *	as a result of our exiting, and if they have any stopped
	 *	jobs, send them a SIGHUP and then a SIGCONT.  (POSIX 3.2.2.2)
	 */

	write_lock_irq(&tasklist_lock);
	// 设置当前进程为僵尸态，资源已经释放，等待父wait4收尸
	current->state = TASK_ZOMBIE;

	// 向父进程发送退出信号（SIGCHLD），唤醒wait4等待的父进程
	do_notify_parent(current, current->exit_signal);

	// 遍历本进程所有子进程（p_cptr是最小子，p_osptr遍历兄弟链）
	while (current->p_cptr != NULL) {
		p = current->p_cptr;
		current->p_cptr = p->p_osptr;
		p->p_ysptr = NULL;
		p->ptrace = 0;

		p->p_pptr = p->p_opptr;
		p->p_osptr = p->p_pptr->p_cptr;
		if (p->p_osptr)
			p->p_osptr->p_ysptr = p;
		p->p_pptr->p_cptr = p;
		if (p->state == TASK_ZOMBIE)
			do_notify_parent(p, p->exit_signal);
		/*
		 * process group orphan check
		 * Case ii: Our child is in a different pgrp
		 * than we are, and it was the only connection
		 * outside, so the child pgrp is now orphaned.
		 */
		if ((p->pgrp != current->pgrp) &&
		    (p->session == current->session)) {
			int pgrp = p->pgrp;

			write_unlock_irq(&tasklist_lock);

			// 判断该进程组是孤儿进程组并且存在停止作业
			if (is_orphaned_pgrp(pgrp) && has_stopped_jobs(pgrp)) {
				kill_pg(pgrp,SIGHUP,1);
				kill_pg(pgrp,SIGCONT,1);
			}
			write_lock_irq(&tasklist_lock);
		}
	}
	// 释放tasklist写锁
	write_unlock_irq(&tasklist_lock);
}

/**
 * do_exit - 进程退出的核心处理函数，完成进程资源回收，使进程进入僵尸态
 * @code: 进程退出码，将保存到task_struct->exit_code，供父进程wait4读取
 *
 * NORET_TYPE：函数不会正常返回，schedule()后永远不会回到do_exit后续代码
 * 功能概述：依次释放内存、文件、文件系统、信号处理等资源；通知父进程；
 * 调用schedule切换出去，本进程不再被调度，变成TASK_ZOMBIE僵尸进程，
 * 等待父进程通过wait4完成最终PCB释放(release_task)
 */
NORET_TYPE void do_exit(long code)
{
	struct task_struct *tsk = current;

	// 不允许在中断上下文调用do_exit，中断处理函数不能被杀死，直接panic崩溃
	if (in_interrupt())
		panic("Aiee, killing interrupt handler!");

	// 禁止杀死idle空闲进程(PID=0)，idle是CPU空闲时运行的基础进程
	if (!tsk->pid)
		panic("Attempted to kill the idle task!");
	
	// 禁止杀死init进程(PID=1)，init是所有孤儿进程的领养父进程，不能终止
	if (tsk->pid == 1)
		panic("Attempted to kill init!");
	tsk->flags |= PF_EXITING;  // 标记PF_EXITING：本进程正在退出中

	// 同步删除进程的实时定时器，等待定时器回调执行完毕，其实就是删除链表
	del_timer_sync(&tsk->real_timer);  

fake_volatile:
#ifdef CONFIG_BSD_PROCESS_ACCT

	// 如果开启BSD进程记账，记录进程退出信息到记账文件
	acct_process(code);
#endif

	// 释放进程地址空间mm，销毁页表、VMA，断开内存资源
	// 这里会通过vfork中的等待
	__exit_mm(tsk);

	// 内核大锁，保护后续进程相关资源操作
	lock_kernel();

	// 释放进程持有的信号量
	sem_exit();

	// 关闭所有打开文件，释放files_struct文件描述符表
	__exit_files(tsk);

	// 释放fs_struct：根目录、当前工作目录等文件系统资源
	__exit_fs(tsk);

	// 释放信号处理相关资源，注销信号处理函数
	exit_sighand(tsk);

	// 销毁线程相关上下文（寄存器、线程栈相关）
	exit_thread();

	// 如果是进程组组长
	if (current->leader)
		disassociate_ctty(1);  // 脱离控制终端，向前台进程组发送SIGHUP

	// 释放exec_domain执行域引用计数
	put_exec_domain(tsk->exec_domain);

	// 如果使用的二进制格式模块存在，减少模块引用计数
	if (tsk->binfmt && tsk->binfmt->module)
		__MOD_DEC_USE_COUNT(tsk->binfmt->module);

	tsk->exit_code = code;  // 保存退出码，供父进程wait4读取

	 // 退出通知核心函数：设置进程为僵尸态，给父发SIGCHLD，处理孤儿进程领养
	exit_notify(); 

	// 触发调度切换；本进程状态已改为僵尸，schedule永不返回
	schedule();  
	BUG();
/*
 * In order to get rid of the "volatile function does return" message
 * I did this little loop that confuses gcc to think do_exit really
 * is volatile. In fact it's schedule() that is volatile in some
 * circumstances: when current->state = ZOMBIE, schedule() never
 * returns.
 *
 * In fact the natural way to do all this is to have the label and the
 * goto right after each other, but I put the fake_volatile label at
 * the start of the function just in case something /really/ bad
 * happens, and the schedule returns. This way we can try again. I'm
 * not paranoid: it's just that everybody is out to get me.
 */
	goto fake_volatile;
}

NORET_TYPE void up_and_exit(struct semaphore *sem, long code)
{
	if (sem)
		up(sem);
	
	do_exit(code);
}

//任务退出
asmlinkage long sys_exit(int error_code)
{
	do_exit((error_code&0xff)<<8);
}

/**
 * sys_wait4 - wait for child process to change state（等待子进程状态改变）
 * @pid:        要等待的子进程PID，4种取值含义：
 *              pid > 0     : 等待PID等于该值的指定子进程
 *              pid == 0    : 等待与调用进程同进程组的任意子进程
 *              pid == -1   : 等待任意子进程
 *              pid < -1    : 等待进程组ID等于 abs(pid) 的进程组内任意子进程
 * @stat_addr:  用户态指针，用于存放子进程退出/停止状态码；传NULL则不保存状态
 * @options:    等待行为选项掩码，支持标志：
 *              WNOHANG     : 非阻塞等待，没有可回收子进程直接返回0，不睡眠
 *              WUNTRACED   : 若子进程被暂停(stopped)，也返回该子进程状态
 *              __WNOTHREAD : 仅扫描当前task的子，不遍历同线程组其他线程的子进程
 *              __WCLONE    : 只等待clone创建的轻量级子任务（exit_signal≠SIGCHLD）
 *              __WALL      : 等待全部子进程（fork普通子 + clone轻量级子）
 * @ru:         用户态rusage结构体指针，用于获取子进程资源使用统计；传NULL不采集
 *
 * 返回值:
 *      >0 : 成功，返回状态发生变化的子进程PID
 *       0 : WNOHANG模式下，当前没有就绪的子进程
 *     <0 : 错误，返回负错误码
 *          -EINVAL   options包含非法标志
 *          -ECHILD   不存在符合条件的子进程
 *          -ERESTARTSYS 等待过程收到信号，系统调用需要重启
 */
asmlinkage long sys_wait4(pid_t pid,unsigned int * stat_addr, int options, struct rusage * ru)
{
	int flag, retval;

	// 定义等待队列节点wait，绑定当前进程current；子进程状态变更时用来唤醒父进程
	DECLARE_WAITQUEUE(wait, current);
	struct task_struct *tsk;

	// 校验options掩码，只允许指定的5个标志，存在非法bit位则返回-EINVAL
	if (options & ~(WNOHANG|WUNTRACED|__WNOTHREAD|__WCLONE|__WALL))
		return -EINVAL;

	// 将当前进程加入【子进程退出等待队列wait_chldexit】
    // 子进程退出/停止时，会唤醒这个等待队列上等待的父进程
	add_wait_queue(&current->wait_chldexit,&wait);
repeat:
	flag = 0;

	// 设置当前进程状态为TASK_INTERRUPTIBLE（可中断睡眠），等待子状态变化
	current->state = TASK_INTERRUPTIBLE;

	// 加tasklist_lock读锁，保护全局进程链表、task_struct亲属指针，防止遍历期间链表被修改
	read_lock(&tasklist_lock);
	tsk = current;
	do {
		struct task_struct *p;
		// 遍历tsk的所有子进程：从p_cptr(最新创建的最小子)开始，顺着p_osptr(哥哥指针)遍历整条兄弟链
	 	for (p = tsk->p_cptr ; p ; p = p->p_osptr) {
			if (pid>0) {
				// pid>0：筛选PID完全匹配的子进程
				if (p->pid != pid)
					continue;
			} else if (!pid) {
				// pid==0：筛选和当前进程同进程组的子进程
				if (p->pgrp != current->pgrp)
					continue;
			} else if (pid != -1) {
				// pid<-1：筛选进程组ID等于 -pid 的子进程
				if (p->pgrp != -pid)
					continue;
			}
			/* Wait for all children (clone and not) if __WALL is set;
			 * otherwise, wait for clone children *only* if __WCLONE is
			 * set; otherwise, wait for non-clone children *only*.  (Note:
			 * A "clone" child here is one that reports to its parent
			 * using a signal other than SIGCHLD.) */
			// 如果设置__WALL：等待所有子进程（普通fork子 + clone轻量级子线程）
            // 否则：仅当__WCLONE置位时，只等待clone创建的子；否则只等待普通fork子进程
            // 注：这里的clone子指退出时发送非SIGCHLD信号通知父的任务
			if (((p->exit_signal != SIGCHLD) ^ ((options & __WCLONE) != 0))
			    && !(options & __WALL))
				continue;
			
			// 找到符合筛选条件的子进程
			flag = 1;
			switch (p->state) {
			case TASK_STOPPED:  // 子进程被暂停（stopped）
				if (!p->exit_code)
					continue;
				// 没有WUNTRACED，且没有被ptrace跟踪，则不报告stopped子进程状态
				if (!(options & WUNTRACED) && !(p->ptrace & PT_PTRACED))
					continue;
				read_unlock(&tasklist_lock);
				// 获取子进程资源使用统计信息
				retval = ru ? getrusage(p, RUSAGE_BOTH, ru) : 0; 
				if (!retval && stat_addr) 
					// 将状态码拷贝回用户态stat_addr
					retval = put_user((p->exit_code << 8) | 0x7f, stat_addr);
				if (!retval) {
					p->exit_code = 0;
					retval = p->pid;
				}
				goto end_wait4;
			case TASK_ZOMBIE: // 子进程僵尸态（已经退出，等待父收尸）

				// 累加子进程的用户态、内核态时间到父进程的子进程时间统计
				current->times.tms_cutime += p->times.tms_utime + p->times.tms_cutime;
				current->times.tms_cstime += p->times.tms_stime + p->times.tms_cstime;
				read_unlock(&tasklist_lock);
				// 获取资源使用统计
				retval = ru ? getrusage(p, RUSAGE_BOTH, ru) : 0;
				// 把子退出码拷贝给用户态stat_addr
				if (!retval && stat_addr)
					retval = put_user(p->exit_code, stat_addr);
				if (retval)
					goto end_wait4; 
				retval = p->pid;
				// 场景：子进程被ptrace，p_pptr被临时修改为调试器，p_opptr是原始父进程
				if (p->p_opptr != p->p_pptr) {
					write_lock_irq(&tasklist_lock);
					REMOVE_LINKS(p);  // 从进程链表摘除PCB
					p->p_pptr = p->p_opptr;  // 恢复父指针为原始父进程
					SET_LINKS(p);  // 重新挂入进程家族链表
					do_notify_parent(p, SIGCHLD); // 向原始父发送SIGCHLD信号
					write_unlock_irq(&tasklist_lock);
				} else
					release_task(p); // 正常情况：释放僵尸子进程的task_struct，回收资源
				goto end_wait4;
			default:  // 子进程运行/睡眠，不满足等待条件，继续循环
				continue;
			}
		}

		// __WNOTHREAD：不跨线程查找，只查找当前task的子，不遍历同线程组其他线程
		if (options & __WNOTHREAD)
			break;

		// // 遍历同线程组内其他线程，查看它们的子进程
		tsk = next_thread(tsk);
	} while (tsk != current);
	read_unlock(&tasklist_lock);
	if (flag) {  // 找到符合条件的子进程，但子进程不是stopped/zombie，还在运行
		retval = 0;
		if (options & WNOHANG)  // WNOHANG非阻塞，直接返回0
			goto end_wait4;
		retval = -ERESTARTSYS;
		if (signal_pending(current))  // 收到信号，退出，系统调用可重启
			goto end_wait4;
		schedule();   // 让出CPU，睡眠，等待子进程状态变化
		goto repeat;  // 被唤醒后，重新扫描子进程
	}
	retval = -ECHILD;  // 没有匹配的子进程，返回-ECHILD
end_wait4:
	current->state = TASK_RUNNING;  // 恢复当前进程为就绪态
	remove_wait_queue(&current->wait_chldexit,&wait);  // 从等待队列移除等待节点
	return retval;
}

#if !defined(__alpha__) && !defined(__ia64__)

/*
 * sys_waitpid() remains for compatibility. waitpid() should be
 * implemented by calling sys_wait4() from libc.a.
 */
asmlinkage long sys_waitpid(pid_t pid,unsigned int * stat_addr, int options)
{
	return sys_wait4(pid, stat_addr, options, NULL);
}

#endif
