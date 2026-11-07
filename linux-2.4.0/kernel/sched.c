/*
 *  linux/kernel/sched.c
 *
 *  Kernel scheduler and related syscalls
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  1996-12-23  Modified by Dave Grothe to fix bugs in semaphores and
 *              make semaphores SMP safe
 *  1998-11-19	Implemented schedule_timeout() and related stuff
 *		by Andrea Arcangeli
 *  1998-12-28  Implemented better SMP scheduling by Ingo Molnar
 */

/*
 * 'sched.c' is the main kernel file. It contains scheduling primitives
 * (sleep_on, wakeup, schedule etc) as well as a number of simple system
 * call functions (type getpid()), which just extract a field from
 * current-task
 */

#include <linux/config.h>
#include <linux/mm.h>
#include <linux/init.h>
#include <linux/smp_lock.h>
#include <linux/interrupt.h>
#include <linux/kernel_stat.h>

#include <asm/uaccess.h>
#include <asm/mmu_context.h>

extern void timer_bh(void);
extern void tqueue_bh(void);
extern void immediate_bh(void);

/*
 * scheduler variables
 */

unsigned securebits = SECUREBITS_DEFAULT; /* systemwide security settings */

extern void mem_use(void);

/*
 * Scheduling quanta.
 *
 * NOTE! The unix "nice" value influences how long a process
 * gets. The nice value ranges from -20 to +19, where a -20
 * is a "high-priority" task, and a "+10" is a low-priority
 * task.
 *
 * We want the time-slice to be around 50ms or so, so this
 * calculation depends on the value of HZ.
 */
#if HZ < 200
#define TICK_SCALE(x)	((x) >> 2)
#elif HZ < 400
#define TICK_SCALE(x)	((x) >> 1)
#elif HZ < 800
#define TICK_SCALE(x)	(x)
#elif HZ < 1600
#define TICK_SCALE(x)	((x) << 1)
#else
#define TICK_SCALE(x)	((x) << 2)
#endif

#define NICE_TO_TICKS(nice)	(TICK_SCALE(20-(nice))+1)


/*
 *	Init task must be ok at boot for the ix86 as we will check its signals
 *	via the SMP irq return path.
 */
 
struct task_struct * init_tasks[NR_CPUS] = {&init_task, };

/*
 * The tasklist_lock protects the linked list of processes.
 *
 * The runqueue_lock locks the parts that actually access
 * and change the run-queues, and have to be interrupt-safe.
 *
 * If both locks are to be concurrently held, the runqueue_lock
 * nests inside the tasklist_lock.
 */
spinlock_t runqueue_lock __cacheline_aligned = SPIN_LOCK_UNLOCKED;  /* inner */
rwlock_t tasklist_lock __cacheline_aligned = RW_LOCK_UNLOCKED;	/* outer */

static LIST_HEAD(runqueue_head);

/*
 * We align per-CPU scheduling data on cacheline boundaries,
 * to prevent cacheline ping-pong.
 */
/**
 * aligned_data - 每CPU调度数据数组，按cacheline对齐（Linux2.4 SMP）
 * __cacheline_aligned：整个数组的每个元素放在独立cacheline，防止伪共享(false sharing)
 * NR_CPUS：内核编译时配置的最大CPU数量
 */
static union {
	// 调度器每CPU私有数据
	struct schedule_data {
		struct task_struct * curr;	// 当前CPU上正在运行的任务指针
		cycles_t last_schedule;	// 上一次发生上下文切换的CPU周期计数（RDTSC）
	} schedule_data;
	// 填充字节，保证整个union大小等于一条cacheline
	char __pad [SMP_CACHE_BYTES];
} aligned_data [NR_CPUS] __cacheline_aligned = { {{&init_task,0}} };

//CPU当前任务
#define cpu_curr(cpu) aligned_data[(cpu)].schedule_data.curr
#define last_schedule(cpu) aligned_data[(cpu)].schedule_data.last_schedule

struct kernel_stat kstat;

#ifdef CONFIG_SMP

#define idle_task(cpu) (init_tasks[cpu_number_map(cpu)])
#define can_schedule(p,cpu) ((!(p)->has_cpu) && \
				((p)->cpus_allowed & (1 << cpu)))

#else

#define idle_task(cpu) (&init_task)
#define can_schedule(p,cpu) (1)

#endif

void scheduling_functions_start_here(void) { }

/*
 * This is the function that decides how desirable a process is..
 * You can weigh different processes against each other depending
 * on what CPU they've run on lately etc to try to handle cache
 * and TLB miss penalties.
 *
 * Return values:
 *	 -1000: never select this
 *	     0: out of time, recalculate counters (but it might still be
 *		selected)
 *	   +ve: "goodness" value (the larger, the better)
 *	 +1000: realtime process, select this.
 */

/**
 * static inline int goodness(struct task_struct * p, int this_cpu, struct mm_struct *this_mm)
 * @p: 待评估的就绪进程
 * @this_cpu: 当前正在执行调度的CPU编号
 * @this_mm: 当前CPU上正在运行进程的mm地址空间
 * 返回值: weight，分数。分数越高，进程越优先被调度选中
 * 调用位置：pick_next_task，遍历runqueue里面所有就绪任务，逐个调用goodness打分，选出最高分进程
 * Linux2.4 O(n)调度器的打分核心函数
 */
static inline int goodness(struct task_struct * p, int this_cpu, struct mm_struct *this_mm)
{
	int weight;
	/*
	 * select the current process after every other
	 * runnable process, but before the idle thread.
	 * Also, dont trigger a counter recalculation.
	 */
	weight = -1; // 默认分数-1，代表不参与调度

	// 1. 如果进程设置了 SCHED_YIELD（主动sched_yield让出CPU），直接返回weight=-1
	if (p->policy & SCHED_YIELD)
		goto out;

	/*
	 * Non-RT process - normal case first.
	 */
	// 2. 普通分时进程 SCHED_OTHER
	if (p->policy == SCHED_OTHER) {
		/*
		 * Give the process a first-approximation goodness value
		 * according to the number of clock-ticks it has left.
		 *
		 * Don't do any other calculations if the time slice is
		 * over..
		 */
		weight = p->counter; // 基础分：剩余时间片tick数
		if (!weight)          // counter等于0，时间片耗尽，分数保持0，不优先
			goto out;

#ifdef CONFIG_SMP
		/* Give a largish advantage to the same processor...   */
		/* (this is equivalent to penalizing other processors) */
		// SMP优化：进程上次就在本CPU运行，加分，倾向于不迁移，利用cache
		if (p->processor == this_cpu)
			weight += PROC_CHANGE_PENALTY;
#endif
		/* .. and a slight advantage to the current MM */
		// 如果进程mm和当前CPU正在运行进程mm相同（共享地址空间，线程）或者是内核线程(!p->mm)，轻微加分
		if (p->mm == this_mm || !p->mm)
			weight += 1;

		// nice值修正：nice范围(-20 ~ +19)，20-nice，nice越小（优先级越高），加分越多
		weight += 20 - p->nice;
		goto out;
	}

	/*
	 * Realtime process, select the first one on the
	 * runqueue (taking priorities within processes
	 * into account).
	 */
	// 3. 实时进程 SCHED_FIFO / SCHED_RR
	// 基础分1000 + 实时优先级rt_priority，保证实时进程分数一定大于普通SCHED_OTHER
	weight = 1000 + p->rt_priority;

out:
	return weight;
}
/*
 * the 'goodness value' of replacing a process on a given CPU.
 * positive value means 'replace', zero or negative means 'dont'.
 */
static inline int preemption_goodness(struct task_struct * prev, struct task_struct * p, int cpu)
{
	return goodness(p, cpu, prev->active_mm) - goodness(prev, cpu, prev->active_mm);
}

/*
 * This is ugly, but reschedule_idle() is very timing-critical.
 * We are called with the runqueue spinlock held and we must
 * not claim the tasklist_lock.
 */
static FASTCALL(void reschedule_idle(struct task_struct * p));

/**
 * static void reschedule_idle(struct task_struct * p)
 * @p: 待尝试推送迁移的就绪进程
 * 仅SMP平台为核心逻辑，UP单核有简化分支
 * 调用位置：__schedule_tail，wake_up_process也会调用
 * 功能：SMP负载均衡，尝试寻找合适CPU，把就绪进程p推送过去运行；
 *       要么找空闲idle CPU直接放上去；要么评估能否抢占目标CPU上当前正在运行的进程，发送IPI中断触发对方CPU调度
 */
static void reschedule_idle(struct task_struct * p)
{
#ifdef CONFIG_SMP
	int this_cpu = smp_processor_id();
	struct task_struct *tsk, *target_tsk;
	int cpu, best_cpu, i, max_prio;
	cycles_t oldest_idle;
	/*
	 * shortcut if the woken up task's last CPU is
	 * idle now.
	 */
	// 1. 优先尝试：进程p上次运行的CPU（p->processor），缓存亲和，优先复用
	best_cpu = p->processor;
	// can_schedule：检查p是否允许在best_cpu上调度（亲和性等限制）
	if (can_schedule(p, best_cpu)) {
		tsk = idle_task(best_cpu);
		// 判断目标CPU当前正在跑idle空闲进程
		if (cpu_curr(best_cpu) == tsk) {
			int need_resched;
send_now_idle:
			/*
			 * If need_resched == -1 then we can skip sending
			 * the IPI altogether, tsk->need_resched is
			 * actively watched by the idle thread.
			 */
			need_resched = tsk->need_resched;
			tsk->need_resched = 1; // 标记目标CPU需要重新调度
			// 如果不是本CPU，并且原来没有待调度标记，发送跨CPU IPI调度中断
			// 通过中断，触发CPU调度
			if ((best_cpu != this_cpu) && !need_resched)
				smp_send_reschedule(best_cpu);
			return;
		}
	}

	/*
	 * We know that the preferred CPU has a cache-affine current
	 * process, lets try to find a new idle CPU for the woken-up
	 * process. Select the least recently active idle CPU. (that
	 * one will have the least active cache context.) Also find
	 * the executing process which has the least priority.
	 */
	// 2. 首选CPU不是idle，遍历所有CPU寻找候选目标
	oldest_idle = (cycles_t) -1;
	target_tsk = NULL;
	max_prio = 1;
	for (i = 0; i < smp_num_cpus; i++) {
		cpu = cpu_logical_map(i);
		if (!can_schedule(p, cpu))
			continue; // p不能跑在这个CPU，跳过
		tsk = cpu_curr(cpu); // 获取该CPU当前正在运行的任务
		/*
		 * We use the first available idle CPU. This creates
		 * a priority list between idle CPUs, but this is not
		 * a problem.
		 */
		if (tsk == idle_task(cpu)) {
			// 找到idle CPU：选择最久没有活动的idle CPU（cache污染最小）
			if (last_schedule(cpu) < oldest_idle) {
				oldest_idle = last_schedule(cpu);
				target_tsk = tsk;
			}
		} else {
			// CPU正在跑用户进程，计算抢占优先级preemption_goodness，评估p能不能抢占这个tsk
			if (oldest_idle == -1ULL) {
				int prio = preemption_goodness(tsk, p, cpu);
				if (prio > max_prio) {
					max_prio = prio;
					target_tsk = tsk;
				}
			}
		}
	}
	tsk = target_tsk;
	if (tsk) {
		if (oldest_idle != -1ULL) {
			// 找到空闲CPU，跳转到前面的idle处理分支
			best_cpu = tsk->processor;
			goto send_now_idle;
		}
		// 需要抢占目标CPU上正在运行的进程：置need_resched，发送IPI
		tsk->need_resched = 1;
		if (tsk->processor != this_cpu)
			smp_send_reschedule(tsk->processor);
	}
	return;

#else /* UP 单核版本 */
	// 单核简化逻辑：评估p是否可以抢占当前CPU正在运行的任务
	int this_cpu = smp_processor_id();
	struct task_struct *tsk;
	tsk = cpu_curr(this_cpu);
	// preemption_goodness计算抢占收益，大于1代表可以抢占
	if (preemption_goodness(tsk, p, this_cpu) > 1)
		tsk->need_resched = 1;
#endif
}

/*
 * Careful!
 *
 * This has to add the process to the _beginning_ of the
 * run-queue, not the end. See the comment about "This is
 * subtle" in the scheduler proper..
 */
/**
 * add_to_runqueue - 将就绪任务加入运行队列（Linux2.4 O(n)调度器）
 * @p: task_struct 待加入就绪队列的进程PCB
 *
 * 注意：2.4原版调度器是单全局runqueue_head，所有CPU共用这个就绪链表
 */
static inline void add_to_runqueue(struct task_struct * p)
{
	// 把任务的run_list节点，头插到全局就绪队列runqueue_head
	list_add(&p->run_list, &runqueue_head);
	// 全局就绪进程计数 +1，代表当前有多少TASK_RUNNING任务
	nr_running++;
}

/**
 * move_last_runqueue - 将进程从就绪队列头部移到队列尾部（Linux2.4 O(n)调度器）
 * @p: task_struct 待移动的进程PCB
 */
static inline void move_last_runqueue(struct task_struct * p)
{
	// 第一步：把p->run_list节点从runqueue_head双向链表中摘除
	list_del(&p->run_list);

	// 第二步：使用list_add_tail，把节点插到链表的末尾
	list_add_tail(&p->run_list, &runqueue_head);
}

//放到最前面
static inline void move_first_runqueue(struct task_struct * p)
{
	list_del(&p->run_list);
	list_add(&p->run_list, &runqueue_head);
}

/*
 * Wake up a process. Put it on the run-queue if it's not
 * already there.  The "current" process is always on the
 * run-queue (except when the actual re-schedule is in
 * progress), and as such you're allowed to do the simpler
 * "current->state = TASK_RUNNING" to mark yourself runnable
 * without the overhead of this.
 */
inline void wake_up_process(struct task_struct * p)
{
	unsigned long flags;

	/*
	 * We want the common case fall through straight, thus the goto.
	 */
	// 获取运行队列自旋锁，并保存中断标志位（关中断）
	spin_lock_irqsave(&runqueue_lock, flags);

	// 将任务状态置为可运行 TASK_RUNNING
	p->state = TASK_RUNNING;

	// 判断该task是否已经在runqueue就绪队列上
	if (task_on_runqueue(p))
		goto out;

	// 不在就绪队列：把任务加入runqueue就绪链表
	// list_add(&p->run_list, &runqueue_head);
	add_to_runqueue(p);

	// 尝试抢占当前CPU，看是否需要调度
	reschedule_idle(p);
out:
	spin_unlock_irqrestore(&runqueue_lock, flags);
}

static inline void wake_up_process_synchronous(struct task_struct * p)
{
	unsigned long flags;

	/*
	 * We want the common case fall through straight, thus the goto.
	 */
	spin_lock_irqsave(&runqueue_lock, flags);
	p->state = TASK_RUNNING;
	if (task_on_runqueue(p))
		goto out;
	add_to_runqueue(p);
out:
	spin_unlock_irqrestore(&runqueue_lock, flags);
}

static void process_timeout(unsigned long __data)
{
	struct task_struct * p = (struct task_struct *) __data;

	wake_up_process(p);
}

/**
 * signed long schedule_timeout(signed long timeout)
 * @timeout: 要休眠的jiffies节拍数
 * 返回值：0 = 定时器到期，正常休眠完成；>0 = 被信号提前唤醒，剩余还需要等待的jiffies
 * 作用：让当前进程休眠指定的jiffies时长，底层被nanosleep、wait_event_timeout等大量内核接口调用
 * 前提：调用本函数前，必须预先设置 current->state 为 TASK_INTERRUPTIBLE / TASK_UNINTERRUPTIBLE
 */
signed long schedule_timeout(signed long timeout)
{
	struct timer_list timer;  // 内核定时器局部变量，栈上分配
	unsigned long expire;

	switch (timeout)
	{
	case MAX_SCHEDULE_TIMEOUT:
		/*
		 * These two special cases are useful to be comfortable
		 * in the caller. Nothing more. We could take
		 * MAX_SCHEDULE_TIMEOUT from one of the negative value
		 * but I' d like to return a valid offset (>=0) to allow
		 * the caller to do everything it want with the retval.
		 */

		 /*
		 * 特殊值：无限等待，不设置定时器
		 * 直接调用schedule()让出CPU，永久睡眠，等待外部事件唤醒（信号/其他wakeup）
		 * 唤醒后直接返回，不做时间计算
		 */
		schedule();
		goto out;
	default:
		/*
		 * Another bit of PARANOID. Note that the retval will be
		 * 0 since no piece of kernel is supposed to do a check
		 * for a negative retval of schedule_timeout() (since it
		 * should never happens anyway). You just have the printk()
		 * that will tell you if something is gone wrong and where.
		 */
		// 防御性检查：timeout不能为负数
		if (timeout < 0)
		{
			printk(KERN_ERR "schedule_timeout: wrong timeout "
			       "value %lx from %p\n", timeout,
			       __builtin_return_address(0));
			current->state = TASK_RUNNING;
			goto out;
		}
	}

	// 计算定时器到期绝对时间点：当前jiffies + 需要等待的节拍数
	expire = timeout + jiffies;

	// 初始化timer_list定时器
	init_timer(&timer);
	timer.expires = expire;					// 定时器到期的jiffies
	timer.data = (unsigned long) current;		// 传给回调函数的参数：当前进程task_struct指针
	timer.function = process_timeout;			// 定时器到期回调函数

	add_timer(&timer);	// 将定时器插入内核全局定时器链表，内核会在expire时刻触发回调

	schedule();			// 让出CPU，调度其他进程运行；本进程进入睡眠等待唤醒

	del_timer_sync(&timer); // 唤醒后，尝试删除定时器；同步等待：如果定时器正在执行，等它执行完毕再返回

	// 计算还剩多少没等到的时间：到期点 - 当前jiffies
	timeout = expire - jiffies;
out:
	// 负数代表已经超时，返回0；>=0返回剩余jiffies
	return timeout < 0 ? 0 : timeout;
}

/*
 * schedule_tail() is getting called from the fork return path. This
 * cleans up all remaining scheduler things, without impacting the
 * common case.
 */
/**
 * static inline void __schedule_tail(struct task_struct *prev)
 * @prev: 刚刚被切换下CPU的旧进程
 * 调用位置：schedule() 末尾，switch_to 返回之后执行
 * 作用：SMP平台下完成进程切换后的收尾工作，清除SCHED_YIELD标记、清除has_cpu标记，
 *       如果prev仍然是TASK_RUNNING，则尝试把旧进程推到别的CPU上运行（负载均衡reschedule_idle）
 */
static inline void __schedule_tail(struct task_struct *prev)
{
#ifdef CONFIG_SMP
	int policy;
	/*
	 * prev->policy can be written from here only before `prev'
	 * can be scheduled (before setting prev->has_cpu to zero).
	 * Of course it must also be read before allowing prev
	 * to be rescheduled, but since the write depends on the read
	 * to complete, wmb() is enough. (the spin_lock() acquired
	 * before setting has_cpu is not enough because the spin_lock()
	 * common code semantics allows code outside the critical section
	 * to enter inside the critical section)
	 */
	// 读取旧进程调度策略，清除 SCHED_YIELD 标记（主动让出CPU标记）
	policy = prev->policy;
	prev->policy = policy & ~SCHED_YIELD;
	wmb(); // 写内存屏障：保证policy的写入先完成，再修改后面has_cpu，防止乱序

	/*
	 * fast path falls through. We have to clear has_cpu before
	 * checking prev->state to avoid a wakeup race - thus we
	 * also have to protect against the task exiting early.
	 */
	task_lock(prev);       // 对task_struct加锁，防止prev进程并发退出
	prev->has_cpu = 0;     // 清除标记：prev不再占有当前CPU
	mb();                  // 全内存屏障：保证has_cpu=0对其他CPU可见后，再读取state

	if (prev->state == TASK_RUNNING)
		goto needs_resched; // 旧进程依然就绪，需要尝试推到其他CPU
out_unlock:
	task_unlock(prev);     /* Synchronise here with release_task() if prev is TASK_ZOMBIE */
	return;

	/*
	 * Slow path - we 'push' the previous process and
	 * reschedule_idle() will attempt to find a new
	 * processor for it. (but it might preempt the
	 * current process as well.) We must take the runqueue
	 * lock and re-check prev->state to be correct. It might
	 * still happen that this process has a preemption
	 * 'in progress' already - but this is not a problem and
	 * might happen in other circumstances as well.
	 */
needs_resched:
	{
		unsigned long flags;
		/*
		 * Avoid taking the runqueue lock in cases where
		 * no preemption-check is necessery:
		 */
		// 如果prev是idle进程，或者本来就是SCHED_YIELD主动放弃CPU，不需要迁移
		if ((prev == idle_task(smp_processor_id())) ||
				(policy & SCHED_YIELD))
			goto out_unlock;

		// 获取runqueue锁，关中断
		spin_lock_irqsave(&runqueue_lock, flags);
		// 再次检查state（并发race保护），确认仍然就绪
		if (prev->state == TASK_RUNNING)
			reschedule_idle(prev); // 尝试将prev推送到其他空闲CPU运行
		spin_unlock_irqrestore(&runqueue_lock, flags);
		goto out_unlock;
	}
#else
	// 单CPU版本简单处理：只清除SCHED_YIELD标记，没有SMP负载迁移逻辑
	prev->policy &= ~SCHED_YIELD;
#endif /* CONFIG_SMP */
}

void schedule_tail(struct task_struct *prev)
{
	__schedule_tail(prev);
}

/*
 *  'schedule()' is the scheduler function. It's a very simple and nice
 * scheduler: it's not perfect, but certainly works for most things.
 *
 * The goto is "interesting".
 *
 *   NOTE!!  Task 0 is the 'idle' task, which gets called when no other
 * tasks can run. It can not be killed, and it cannot sleep. The 'state'
 * information in task[0] is never used.
 */
/**
 * asmlinkage void schedule - Linux 2.4 O(n) 调度器主函数
 *
 * 触发时机：
 *  1. 中断返回时检测 current->need_resched = 1
 *  2. 进程主动调用 schedule() 主动放弃CPU（如sleep、wait）
 * 功能：选出下一个要在当前CPU运行的任务，完成上下文切换
 * 核心特点：全局单一runqueue，遍历全部就绪任务做goodness打分 O(n)
 */
asmlinkage void schedule(void)
{
	struct schedule_data * sched_data;
	struct task_struct *prev, *next, *p;
	struct list_head *tmp;
	int this_cpu, c;

	if (!current->active_mm) BUG();  // 合法性检查：当前进程必须有active_mm
need_resched_back:
	//这个进程切换出去
	prev = current;  // prev = 即将让出CPU的旧进程
	this_cpu = prev->processor;  // 获取当前CPU编号

	if (in_interrupt())
		goto scheduling_in_interrupt;  // 禁止在中断上下文调用schedule，直接BUG

	release_kernel_lock(prev, this_cpu);

	/* Do "administrative" work here while we don't hold any locks */
	/* 无锁状态下处理软中断 */
	// 任务切换的时候处理软件中断
	if (softirq_active(this_cpu) & softirq_mask(this_cpu))
		goto handle_softirq;
handle_softirq_back:

	/*
	 * 'sched_data' is protected by the fact that we can run
	 * only one process per CPU.
	 */
	// sched_data是每CPU调度数据，单CPU同一时刻只有一个进程运行，无需额外锁
	sched_data = & aligned_data[this_cpu].schedule_data;

	// 加全局就绪队列自旋锁，关中断保护runqueue
	spin_lock_irq(&runqueue_lock);

	/* move an exhausted RR process to be last.. */
	/* RR实时策略：时间片耗尽则移到队列尾部 */
	// SCHED_RR 实时策略（Round Robin 实时轮询）
	/*
	称：SCHED_RR = Round-Robin，**实时轮询**
	- 同样是**实时进程**，`rt_priority`（1~99，越大优先级越高），优先级高于 SCHED_OTHER
	- 和 FIFO 几乎一样，唯一差别：**带时间片**

	1. 高优先级抢占低优先级；
	2. 同优先级 RR 任务：进程拿到 CPU，运行到**时间片耗尽**；
	3. 时间片用完：内核把它移动到就绪队列**同优先级组的队尾**（就是前面看到的`move_last_runqueue()`），调度同优先级下一个就绪 RR 任务；
	4. 如果进程主动 sleep，时间片不会被消耗。
	*/
	if (prev->policy == SCHED_RR)
		goto move_rr_last;
move_rr_back:

	// 根据prev的状态，决定是否从就绪队列摘除
	switch (prev->state) {
		case TASK_INTERRUPTIBLE:
			// 可中断睡眠：如果有未处理信号，状态改回TASK_RUNNING，保留在就绪队列
			if (signal_pending(prev)) {
				prev->state = TASK_RUNNING;
				break;
			}
		default:
			// 无信号，fallthrough，从就绪队列摘除
			del_from_runqueue(prev);
		case TASK_RUNNING:
		// TASK_RUNNING：进程仍然就绪，保留在runqueue，不摘除
	}
	prev->need_resched = 0;  // 清除重调度标志

	/*
	 * this is the scheduler proper:
	 */

	 /* ========== 调度核心：挑选next进程 ========== */
repeat_schedule:
	/*
	 * Default process to select..
	 */
	next = idle_task(this_cpu); // 默认选中本CPU的idle 0号进程
	c = -1000;  // goodness权重初始最小值

	// 如果prev仍然是TASK_RUNNING，先计算它的权重
	if (prev->state == TASK_RUNNING)
		goto still_running;

still_running_back:
	// 遍历全局就绪队列所有任务 O(n)！
	list_for_each(tmp, &runqueue_head) {
		p = list_entry(tmp, struct task_struct, run_list);
		if (can_schedule(p, this_cpu)) { // 判断p能否调度到当前CPU（SMP亲和性）
			// goodness：计算进程的调度权重，数值越大优先级越高
			int weight = goodness(p, this_cpu, prev->active_mm);
			if (weight > c)
				c = weight, next = p;  // 记录权重最高的进程作为next
		}
	}

	/* Do we need to re-calculate counters? */
	/* c==0 代表所有就绪进程counter全部耗尽，需要全局重算时间片 */
	if (!c)
		goto recalculate;
	/*
	 * from this point on nothing can prevent us from
	 * switching to the next task, save this fact in
	 * sched_data.
	 */
	// 选中next，记录到每CPU调度数据
	sched_data->curr = next;
#ifdef CONFIG_SMP
 	next->has_cpu = 1;
	next->processor = this_cpu;
#endif
	spin_unlock_irq(&runqueue_lock);  // 释放runqueue全局锁

	if (prev == next)
		goto same_process;  // 选中的还是原来进程，不需要上下文切换

#ifdef CONFIG_SMP
 	/*
 	 * maintain the per-process 'last schedule' value.
 	 * (this has to be recalculated even if we reschedule to
 	 * the same process) Currently this is only used on SMP,
	 * and it's approximate, so we do not have to maintain
	 * it while holding the runqueue spinlock.
 	 */
 	sched_data->last_schedule = get_cycles();  // 记录本次调度时间戳

	/*
	 * We drop the scheduler lock early (it's a global spinlock),
	 * thus we have to lock the previous process from getting
	 * rescheduled during switch_to().
	 */

#endif /* CONFIG_SMP */

	kstat.context_swtch++; // 全局上下文切换统计计数+1
	/*
	 * there are 3 processes which are affected by a context switch:
	 *
	 * prev == .... ==> (last => next)
	 *
	 * It's the 'much more previous' 'prev' that is on next's stack,
	 * but prev is set to (the just run) 'last' process by switch_to().
	 * This might sound slightly confusing but makes tons of sense.
	 */
	prepare_to_switch();  // 切换前准备工作（预取、缓存相关）

	/* 切换内存描述符 mm_struct，处理用户态/内核线程lazy tlb */
	{
		struct mm_struct *mm = next->mm;
		struct mm_struct *oldmm = prev->active_mm;
		if (!mm) {
			// next是内核线程，没有自己mm，复用prev的active_mm（lazy tlb机制）
			if (next->active_mm) BUG();
			next->active_mm = oldmm;
			atomic_inc(&oldmm->mm_count);
			enter_lazy_tlb(oldmm, next, this_cpu);
		} else {
			// 用户进程，切换页表
			if (next->active_mm != mm) BUG();
			switch_mm(oldmm, mm, next, this_cpu);
		}

		if (!prev->mm) {
			// prev是内核线程，不再复用mm，释放引用
			prev->active_mm = NULL;
			mmdrop(oldmm);
		}
	}

	/*
	 * This just switches the register state and the
	 * stack.
	 */
	// 核心：切换栈+寄存器，汇编switch_to
	switch_to(prev, next, prev);
	__schedule_tail(prev);  // 切换回来之后的收尾工作

same_process:
	reacquire_kernel_lock(current);  // 重新拿回BKL大内核锁
	if (current->need_resched)
		goto need_resched_back;  // 切换完成后又出现重调度标记，再次调度

	return;

	// ========= 分支：所有进程时间片耗尽，全局重新计算counter =========
recalculate:
	{
		struct task_struct *p;
		spin_unlock_irq(&runqueue_lock);
		read_lock(&tasklist_lock);
		// 遍历系统所有task，重新计算counter时间片
		for_each_task(p)
			p->counter = (p->counter >> 1) + NICE_TO_TICKS(p->nice);
		read_unlock(&tasklist_lock);
		spin_lock_irq(&runqueue_lock);
	}
	goto repeat_schedule;

	// ========= 分支：prev依旧处于TASK_RUNNING状态 =========
still_running:
	c = goodness(prev, this_cpu, prev->active_mm);
	next = prev;
	goto still_running_back;

	// ========= 分支：处理软中断 =========
handle_softirq:
	do_softirq();
	goto handle_softirq_back;

	// ========= 分支：SCHED_RR时间片用完，挪到就绪队列尾部 =========
move_rr_last:
	if (!prev->counter) {
		prev->counter = NICE_TO_TICKS(prev->nice);
		move_last_runqueue(prev);
	}
	goto move_rr_back;

	// ========= 异常：中断上下文调用schedule，内核崩溃 =========
scheduling_in_interrupt:
	printk("Scheduling in interrupt\n");
	BUG();
	return;
}

/**
 * __wake_up_common - 等待队列唤醒的核心遍历逻辑，Linux2.4
 * @q: 等待队列头
 * @mode: 状态掩码，只唤醒 state & mode 不为0的进程（TASK_INTERRUPTIBLE / TASK_UNINTERRUPTIBLE）
 * @wq_mode: 唤醒标记，区分唤醒单个/全部，同时包含 WQ_FLAG_EXCLUSIVE 排他标记
 * @sync: 是否同步唤醒；同步唤醒不会主动触发调度
 */
static inline void __wake_up_common (wait_queue_head_t *q, unsigned int mode,
                     unsigned int wq_mode, const int sync)
{
    struct list_head *tmp, *head;
    struct task_struct *p, *best_exclusive;
    unsigned long flags;
    int best_cpu, irq;
    // 空指针直接返回
    if (!q)
        goto out;
    // 记录当前CPU编号，用于中断上下文优先唤醒本CPU亲和的排他进程
    best_cpu = smp_processor_id();
    // 判断当前是否处于中断上下文（in_interrupt：硬中断/软中断/BH都算）
    irq = in_interrupt();
    best_exclusive = NULL;
    // 获取等待队列的写锁 + 保存本地中断标志，关本地中断，保护链表遍历
    wq_write_lock_irqsave(&q->lock, flags);
#if WAITQUEUE_DEBUG
    // 调试：校验等待队列头magic，检测内存破坏
    CHECK_MAGIC_WQHEAD(q);
#endif
    head = &q->task_list;
#if WAITQUEUE_DEBUG
    // 调试：链表完整性检查
        if (!head->next || !head->prev)
            WQ_BUG();
#endif
    // tmp 从第一个等待节点开始遍历链表
    tmp = head->next;
    while (tmp != head) {
        unsigned int state;
        // 通过list_entry，由链表指针拿到wait_queue_t节点
        wait_queue_t *curr = list_entry(tmp, wait_queue_t, task_list);
        // 先移动tmp，防止wake过程中节点被释放导致断链
        tmp = tmp->next;
#if WAITQUEUE_DEBUG
        // 调试：校验当前等待节点magic
        CHECK_MAGIC(curr->__magic);
#endif
        // 取出节点绑定的进程描述符
        p = curr->task;
        state = p->state;
        // 判断：进程状态匹配要唤醒的mode
        if (state & mode) {
#if WAITQUEUE_DEBUG
            // 调试：记录是谁调用的wakeup，用于定位唤醒来源
            curr->__waker = (long)__builtin_return_address(0);
#endif
            /*
             * If waking up from an interrupt context then
             * prefer processes which are affine to this
             * CPU.
             * 如果在中断上下文唤醒：优先选择和当前CPU亲和的排他等待进程
             */
            if (irq && (curr->flags & wq_mode & WQ_FLAG_EXCLUSIVE)) {
                // 第一个排他进程先暂存
                if (!best_exclusive)
                    best_exclusive = p;
                // 如果该排他进程正好运行在当前CPU，直接选中它，跳出遍历
                if (p->processor == best_cpu) {
                    best_exclusive = p;
                    break;
                }
            } else {
                // 非中断上下文，或者是非排他节点：直接唤醒
                if (sync)
                    // 同步唤醒：把进程置就绪，但不触发调度
                    wake_up_process_synchronous(p);
                else
                    // 普通唤醒：唤醒进程，可能触发调度
                    wake_up_process(p);
                // 如果是排他节点，唤醒这一个之后就停止遍历（排他：只唤醒一个）
                if (curr->flags & wq_mode & WQ_FLAG_EXCLUSIVE)
                    break;
            }
        }
    }
    // 遍历结束，如果之前选中了最优排他进程，唤醒它
    if (best_exclusive) {
        if (sync)
            wake_up_process_synchronous(best_exclusive);
        else
            wake_up_process(best_exclusive);
    }
    // 释放锁，恢复本地中断标志
    wq_write_unlock_irqrestore(&q->lock, flags);
out:
    return;
}

/
/**
 * __wake_up - 唤醒等待队列上的进程，包装函数（Linux2.4）
 * @q: 等待队列头
 * @mode: 要唤醒的进程状态掩码，例如 TASK_INTERRUPTIBLE / TASK_UNINTERRUPTIBLE
 * @wq_mode: 唤醒模式，控制唤醒一个还是全部
 */
void __wake_up(wait_queue_head_t *q, unsigned int mode, unsigned int wq_mode)
{
    // 调用真正的遍历、唤醒逻辑
    __wake_up_common(q, mode, wq_mode, 0);
}


void __wake_up_sync(wait_queue_head_t *q, unsigned int mode, unsigned int wq_mode)
{
	__wake_up_common(q, mode, wq_mode, 1);
}

#define	SLEEP_ON_VAR				\
	unsigned long flags;			\
	wait_queue_t wait;			\
	init_waitqueue_entry(&wait, current);

#define	SLEEP_ON_HEAD					\
	wq_write_lock_irqsave(&q->lock,flags);		\
	__add_wait_queue(q, &wait);			\
	wq_write_unlock(&q->lock);

#define	SLEEP_ON_TAIL						\
	wq_write_lock_irq(&q->lock);				\
	__remove_wait_queue(q, &wait);				\
	wq_write_unlock_irqrestore(&q->lock,flags);

void interruptible_sleep_on(wait_queue_head_t *q)
{
	SLEEP_ON_VAR

	current->state = TASK_INTERRUPTIBLE;

	SLEEP_ON_HEAD
	schedule();
	SLEEP_ON_TAIL
}

long interruptible_sleep_on_timeout(wait_queue_head_t *q, long timeout)
{
	SLEEP_ON_VAR

	current->state = TASK_INTERRUPTIBLE;

	SLEEP_ON_HEAD
	timeout = schedule_timeout(timeout);
	SLEEP_ON_TAIL

	return timeout;
}

void sleep_on(wait_queue_head_t *q)
{
	SLEEP_ON_VAR
	
	current->state = TASK_UNINTERRUPTIBLE;

	SLEEP_ON_HEAD
	schedule();
	SLEEP_ON_TAIL
}

long sleep_on_timeout(wait_queue_head_t *q, long timeout)
{
	SLEEP_ON_VAR
	
	current->state = TASK_UNINTERRUPTIBLE;

	SLEEP_ON_HEAD
	timeout = schedule_timeout(timeout);
	SLEEP_ON_TAIL

	return timeout;
}

void scheduling_functions_end_here(void) { }

#ifndef __alpha__

/*
 * This has been replaced by sys_setpriority.  Maybe it should be
 * moved into the arch dependent tree for those ports that require
 * it for backward compatibility?
 */

asmlinkage long sys_nice(int increment)
{
	long newprio;

	/*
	 *	Setpriority might change our priority at the same moment.
	 *	We don't have to worry. Conceptually one call occurs first
	 *	and we have a single winner.
	 */
	if (increment < 0) {
		if (!capable(CAP_SYS_NICE))
			return -EPERM;
		if (increment < -40)
			increment = -40;
	}
	if (increment > 40)
		increment = 40;

	newprio = current->nice + increment;
	if (newprio < -20)
		newprio = -20;
	if (newprio > 19)
		newprio = 19;
	current->nice = newprio;
	return 0;
}

#endif

static inline struct task_struct *find_process_by_pid(pid_t pid)
{
	struct task_struct *tsk = current;

	if (pid)
		tsk = find_task_by_pid(pid);
	return tsk;
}

static int setscheduler(pid_t pid, int policy, 
			struct sched_param *param)
{
	struct sched_param lp;
	struct task_struct *p;
	int retval;

	retval = -EINVAL;
	if (!param || pid < 0)
		goto out_nounlock;

	retval = -EFAULT;
	if (copy_from_user(&lp, param, sizeof(struct sched_param)))
		goto out_nounlock;

	/*
	 * We play safe to avoid deadlocks.
	 */
	read_lock_irq(&tasklist_lock);
	spin_lock(&runqueue_lock);

	p = find_process_by_pid(pid);

	retval = -ESRCH;
	if (!p)
		goto out_unlock;
			
	if (policy < 0)
		policy = p->policy;
	else {
		retval = -EINVAL;
		if (policy != SCHED_FIFO && policy != SCHED_RR &&
				policy != SCHED_OTHER)
			goto out_unlock;
	}
	
	/*
	 * Valid priorities for SCHED_FIFO and SCHED_RR are 1..99, valid
	 * priority for SCHED_OTHER is 0.
	 */
	retval = -EINVAL;
	if (lp.sched_priority < 0 || lp.sched_priority > 99)
		goto out_unlock;
	if ((policy == SCHED_OTHER) != (lp.sched_priority == 0))
		goto out_unlock;

	retval = -EPERM;
	if ((policy == SCHED_FIFO || policy == SCHED_RR) && 
	    !capable(CAP_SYS_NICE))
		goto out_unlock;
	if ((current->euid != p->euid) && (current->euid != p->uid) &&
	    !capable(CAP_SYS_NICE))
		goto out_unlock;

	retval = 0;
	p->policy = policy;
	p->rt_priority = lp.sched_priority;
	if (task_on_runqueue(p))
		move_first_runqueue(p);

	current->need_resched = 1;

out_unlock:
	spin_unlock(&runqueue_lock);
	read_unlock_irq(&tasklist_lock);

out_nounlock:
	return retval;
}

asmlinkage long sys_sched_setscheduler(pid_t pid, int policy, 
				      struct sched_param *param)
{
	return setscheduler(pid, policy, param);
}

asmlinkage long sys_sched_setparam(pid_t pid, struct sched_param *param)
{
	return setscheduler(pid, -1, param);
}

asmlinkage long sys_sched_getscheduler(pid_t pid)
{
	struct task_struct *p;
	int retval;

	retval = -EINVAL;
	if (pid < 0)
		goto out_nounlock;

	retval = -ESRCH;
	read_lock(&tasklist_lock);
	p = find_process_by_pid(pid);
	if (p)
		retval = p->policy & ~SCHED_YIELD;
	read_unlock(&tasklist_lock);

out_nounlock:
	return retval;
}

asmlinkage long sys_sched_getparam(pid_t pid, struct sched_param *param)
{
	struct task_struct *p;
	struct sched_param lp;
	int retval;

	retval = -EINVAL;
	if (!param || pid < 0)
		goto out_nounlock;

	read_lock(&tasklist_lock);
	p = find_process_by_pid(pid);
	retval = -ESRCH;
	if (!p)
		goto out_unlock;
	lp.sched_priority = p->rt_priority;
	read_unlock(&tasklist_lock);

	/*
	 * This one might sleep, we cannot do it with a spinlock held ...
	 */
	retval = copy_to_user(param, &lp, sizeof(*param)) ? -EFAULT : 0;

out_nounlock:
	return retval;

out_unlock:
	read_unlock(&tasklist_lock);
	return retval;
}

asmlinkage long sys_sched_yield(void)
{
	/*
	 * Trick. sched_yield() first counts the number of truly 
	 * 'pending' runnable processes, then returns if it's
	 * only the current processes. (This test does not have
	 * to be atomic.) In threaded applications this optimization
	 * gets triggered quite often.
	 */

	int nr_pending = nr_running;

#if CONFIG_SMP
	int i;

	// Substract non-idle processes running on other CPUs.
	for (i = 0; i < smp_num_cpus; i++)
		if (aligned_data[i].schedule_data.curr != idle_task(i))
			nr_pending--;
#else
	// on UP this process is on the runqueue as well
	nr_pending--;
#endif
	if (nr_pending) {
		/*
		 * This process can only be rescheduled by us,
		 * so this is safe without any locking.
		 */
		if (current->policy == SCHED_OTHER)
			current->policy |= SCHED_YIELD;
		current->need_resched = 1;
	}
	return 0;
}

asmlinkage long sys_sched_get_priority_max(int policy)
{
	int ret = -EINVAL;

	switch (policy) {
	case SCHED_FIFO:
	case SCHED_RR:
		ret = 99;
		break;
	case SCHED_OTHER:
		ret = 0;
		break;
	}
	return ret;
}

asmlinkage long sys_sched_get_priority_min(int policy)
{
	int ret = -EINVAL;

	switch (policy) {
	case SCHED_FIFO:
	case SCHED_RR:
		ret = 1;
		break;
	case SCHED_OTHER:
		ret = 0;
	}
	return ret;
}

asmlinkage long sys_sched_rr_get_interval(pid_t pid, struct timespec *interval)
{
	struct timespec t;
	struct task_struct *p;
	int retval = -EINVAL;

	if (pid < 0)
		goto out_nounlock;

	retval = -ESRCH;
	read_lock(&tasklist_lock);
	p = find_process_by_pid(pid);
	if (p)
		jiffies_to_timespec(p->policy & SCHED_FIFO ? 0 : NICE_TO_TICKS(p->nice),
				    &t);
	read_unlock(&tasklist_lock);
	if (p)
		retval = copy_to_user(interval, &t, sizeof(t)) ? -EFAULT : 0;
out_nounlock:
	return retval;
}

static void show_task(struct task_struct * p)
{
	unsigned long free = 0;
	int state;
	static const char * stat_nam[] = { "R", "S", "D", "Z", "T", "W" };

	printk("%-8s  ", p->comm);
	state = p->state ? ffz(~p->state) + 1 : 0;
	if (((unsigned) state) < sizeof(stat_nam)/sizeof(char *))
		printk(stat_nam[state]);
	else
		printk(" ");
#if (BITS_PER_LONG == 32)
	if (p == current)
		printk(" current  ");
	else
		printk(" %08lX ", thread_saved_pc(&p->thread));
#else
	if (p == current)
		printk("   current task   ");
	else
		printk(" %016lx ", thread_saved_pc(&p->thread));
#endif
	{
		unsigned long * n = (unsigned long *) (p+1);
		while (!*n)
			n++;
		free = (unsigned long) n - (unsigned long)(p+1);
	}
	printk("%5lu %5d %6d ", free, p->pid, p->p_pptr->pid);
	if (p->p_cptr)
		printk("%5d ", p->p_cptr->pid);
	else
		printk("      ");
	if (!p->mm)
		printk(" (L-TLB) ");
	else
		printk(" (NOTLB) ");
	if (p->p_ysptr)
		printk("%7d", p->p_ysptr->pid);
	else
		printk("       ");
	if (p->p_osptr)
		printk(" %5d\n", p->p_osptr->pid);
	else
		printk("\n");

#ifdef CONFIG_X86
/* This is very useful, but only works on x86 right now */
	{
		extern void show_trace(unsigned long);
		show_trace(p->thread.esp);
	}
#endif
}

char * render_sigset_t(sigset_t *set, char *buffer)
{
	int i = _NSIG, x;
	do {
		i -= 4, x = 0;
		if (sigismember(set, i+1)) x |= 1;
		if (sigismember(set, i+2)) x |= 2;
		if (sigismember(set, i+3)) x |= 4;
		if (sigismember(set, i+4)) x |= 8;
		*buffer++ = (x < 10 ? '0' : 'a' - 10) + x;
	} while (i >= 4);
	*buffer = 0;
	return buffer;
}

void show_state(void)
{
	struct task_struct *p;

#if (BITS_PER_LONG == 32)
	printk("\n"
	       "                         free                        sibling\n");
	printk("  task             PC    stack   pid father child younger older\n");
#else
	printk("\n"
	       "                                 free                        sibling\n");
	printk("  task                 PC        stack   pid father child younger older\n");
#endif
	read_lock(&tasklist_lock);
	for_each_task(p)
		show_task(p);
	read_unlock(&tasklist_lock);
}

/*
 *	Put all the gunge required to become a kernel thread without
 *	attached user resources in one place where it belongs.
 */

void daemonize(void)
{
	struct fs_struct *fs;


	/*
	 * If we were started as result of loading a module, close all of the
	 * user space pages.  We don't need them, and if we didn't close them
	 * they would be locked into memory.
	 */
	exit_mm(current);

	current->session = 1;
	current->pgrp = 1;

	/* Become as one with the init task */

	exit_fs(current);	/* current->fs->count--; */
	fs = init_task.fs;
	current->fs = fs;
	atomic_inc(&fs->count);
 	exit_files(current);
	current->files = init_task.files;
	atomic_inc(&current->files->count);
}

void __init init_idle(void)
{
	struct schedule_data * sched_data;
	sched_data = &aligned_data[smp_processor_id()].schedule_data;

	if (current != &init_task && task_on_runqueue(current)) {
		printk("UGH! (%d:%d) was on the runqueue, removing.\n",
			smp_processor_id(), current->pid);
		del_from_runqueue(current);
	}
	sched_data->curr = current;
	sched_data->last_schedule = get_cycles();
}

extern void init_timervecs (void);
/**
 * sched_init - 调度子系统早期初始化函数，start_kernel 中调用
 *
 * 功能：初始化pid哈希表、定时器向量、注册三类核心BH回调；初始化0号进程（init_task）的lazy tlb环境
 * 约束：内核启动早期，仅引导CPU执行；此时只有init_task(0号空闲进程)
 * 上下文：内核启动初始化上下文
 */
void __init sched_init(void)
{
    /*
     * We have to do a little magic to get the first
     * process right in SMP mode.
     */
    // 获取当前引导CPU编号
    int cpu = smp_processor_id();
    int nr;

    // 设置0号初始进程的归属CPU
    init_task.processor = cpu;

    // 清空pid哈希表，pidhash用于根据pid快速查找task_struct
    for(nr = 0; nr < PIDHASH_SZ; nr++)
        pidhash[nr] = NULL;

    // 初始化定时器向量，内核timer_list定时器底层容器
    init_timervecs();

    // 注册TIMER_BH下半部，回调timer_bh，到期内核定时器在这里处理
    init_bh(TIMER_BH, timer_bh);
    // 注册TQUEUE_BH下半部，回调tqueue_bh，处理tq_timer延时任务队列
    init_bh(TQUEUE_BH, tqueue_bh);
    // 注册IMMEDIATE_BH下半部，回调immediate_bh，立即型任务队列
    init_bh(IMMEDIATE_BH, immediate_bh);
    /*
     * The boot idle thread does lazy MMU switching as well:
     */
    // 增加init_mm引用计数，init_mm是0号空闲进程的内存描述符
    atomic_inc(&init_mm.mm_count);
    // 进入lazy tlb模式，空闲进程不刷新TLB，SMP下用于优化mmu切换开销
    enter_lazy_tlb(&init_mm, current, cpu);
}