/*
 *	linux/kernel/softirq.c
 *
 *	Copyright (C) 1992 Linus Torvalds
 *
 * Fixed a disable_bh()/enable_bh() race (was causing a console lockup)
 * due bh_mask_count not atomic handling. Copyright (C) 1998  Andrea Arcangeli
 *
 * Rewritten. Old one was good in 2.2, but in 2.3 it was immoral. --ANK (990903)
 */

#include <linux/config.h>
#include <linux/mm.h>
#include <linux/kernel_stat.h>
#include <linux/interrupt.h>
#include <linux/smp_lock.h>
#include <linux/init.h>
#include <linux/tqueue.h>

/*
   - No shared variables, all the data are CPU local.
   - If a softirq needs serialization, let it serialize itself
     by its own spinlocks.
   - Even if softirq is serialized, only local cpu is marked for
     execution. Hence, we get something sort of weak cpu binding.
     Though it is still not clear, will it result in better locality
     or will not.
   - These softirqs are not masked by global cli() and start_bh_atomic()
     (by clear reasons). Hence, old parts of code still using global locks
     MUST NOT use softirqs, but insert interfacing routines acquiring
     global locks. F.e. look at BHs implementation.

   Examples:
   - NET RX softirq. It is multithreaded and does not require
     any global serialization.
   - NET TX softirq. It kicks software netdevice queues, hence
     it is logically serialized per device, but this serialization
     is invisible to common code.
   - Tasklets: serialized wrt itself.
   - Bottom halves: globally serialized, grr...
 */

/* No separate irq_stat for s390, it is part of PSA */
#if !defined(CONFIG_ARCH_S390)

// Linux2.4 每个CPU独立的中断统计、状态结构，irq_stat是per-cpu数组：irq_stat[NR_CPUS]
irq_cpustat_t irq_stat[NR_CPUS];

#endif	/* CONFIG_ARCH_S390 */

// Linux2.4 软中断向量数组，总共32个软中断项，每个CPU共享这个数组
// __cacheline_aligned：数组起始地址按cacheline对齐，优化SMP访问性能
//使用 open_softirq 设置软中断回调函数
static struct softirq_action softirq_vec[32] __cacheline_aligned;

// Linux2.4 do_softirq：软中断核心处理函数
asmlinkage void do_softirq()
{
    int cpu = smp_processor_id();
    __u32 active, mask;

    // 判断：如果当前已经在中断上下文（硬中断/软中断），直接返回，禁止嵌套调用do_softirq
	// 设计者不希望在硬中断上下文里面执行软中
	// do_IRQ 内部调用 do_softirq 时，local_irq_count>0 → in_interrupt ()=true → 直接返回，不干活
    if (in_interrupt())
        return;

    local_bh_disable();   // ++__local_bh_count，关闭bottom half（tasklet等软中断重入保护）
    local_irq_disable();  // __cli()，关闭当前CPU硬件中断，防止并发修改softirq_active

    mask = softirq_mask(cpu);    // 当前CPU允许处理的软中断掩码（open_softirq设置）
    active = softirq_active(cpu) & mask; // 得到本次需要处理的软中断位图

    if (active) {
        struct softirq_action *h;
restart:
        // 先把即将要处理的这些软中断pending标记清除
        softirq_active(cpu) &= ~active;
        local_irq_enable();  // 打开本地硬件中断！软中断运行期间允许硬中断抢占它

        h = softirq_vec;     // 指向软中断回调注册表首地址
        mask &= ~active;

        // 遍历每一个置位的软中断，执行action回调
        do {
            if (active & 1)
                h->action(h);
            h++;
            active >>= 1;
        } while (active);

        local_irq_disable(); // 处理完一轮，再次关中断，检查有没有新来的pending软中断
        active = softirq_active(cpu);
        if ((active &= mask) != 0)
            goto retry; // 又产生新软中断，跳回restart继续处理
    }
    local_bh_enable(); // --__local_bh_count，开启bottom half

    /* Leave with locally disabled hard irqs. It is critical to close
     * window for infinite recursion, while we help local bh count,
     * it protected us. Now we are defenceless.
     */
    return;
retry:
    goto restart;
}


static spinlock_t softirq_mask_lock = SPIN_LOCK_UNLOCKED;

// Linux2.4 注册一个软中断，绑定回调函数，同时在所有CPU开启该软中断的允许掩码
void open_softirq(int nr, void (*action)(struct softirq_action*), void *data)
{
    unsigned long flags;
    int i;
    // 获取软中断全局锁，并保存本地中断标志
    spin_lock_irqsave(&softirq_mask_lock, flags);
    // 填入软中断向量表：保存私有数据和回调函数
    softirq_vec[nr].data = data;
    softirq_vec[nr].action = action;
    // 遍历所有CPU，在每个CPU的softirq_mask中置位，允许该CPU响应这个软中断nr
    for (i=0; i<NR_CPUS; i++)
        softirq_mask(i) |= (1<<nr);
    spin_unlock_irqrestore(&softirq_mask_lock, flags);
}


/* Tasklets */

struct tasklet_head tasklet_vec[NR_CPUS] __cacheline_aligned;

//从 do_soft_irq 中的 static struct softirq_action softirq_vec[32] __cacheline_aligned; 调用来的

/**
 * tasklet_action - TASKLET_SOFTIRQ对应的软中断处理回调，遍历执行普通tasklet
 * @a: softirq_action结构体指针，软中断框架传入
 *
 * 功能：取出当前CPU上普通tasklet链表，逐个执行tasklet回调；加锁保护，忙则重新入队
 * 约束：运行在软中断(BH)上下文，不允许睡眠；tasklet绑定在本CPU执行
 * 上下文：由do_softirq调用
 */
static void tasklet_action(struct softirq_action *a)
{
    // 获取当前CPU编号
    int cpu = smp_processor_id();
    struct tasklet_struct *list;

    // 关闭本地硬中断，保护tasklet_vec链表的取出操作
    local_irq_disable();
    // 取出本CPU全部待执行tasklet链表
    list = tasklet_vec[cpu].list;
    // 将全局链表头置空，链表脱离，避免调度新tasklet干扰本次遍历
    tasklet_vec[cpu].list = NULL;
    // 打开本地硬中断，恢复中断响应
    local_irq_enable();

    // 循环遍历取出的tasklet链表
    while (list != NULL) {
        struct tasklet_struct *t = list;
        // 链表向后移动，准备处理下一个节点
        list = list->next;

        // 尝试获取tasklet锁，防止多CPU并发执行同一个tasklet
        if (tasklet_trylock(t)) {
            // 判断tasklet引用计数为0，代表可以执行
            if (atomic_read(&t->count) == 0) {
                // 清除SCHED标记，标记该tasklet不再处于排队状态
                clear_bit(TASKLET_STATE_SCHED, &t->state);
                // 调用tasklet注册的回调函数
                t->func(t->data);
				/*
				 * talklet_trylock() uses test_and_set_bit that imply
				 * an mb when it returns zero, thus we need the explicit
				 * mb only here: while closing the critical section.
				 */
#ifdef CONFIG_SMP
                // SMP下内存屏障，保证解锁之前的写操作全局可见
                smp_mb__before_clear_bit();
#endif
                // 释放tasklet锁
                tasklet_unlock(t);
                continue;
            }
            // count不为0，不能执行，直接释放锁
            tasklet_unlock(t);
        }

        // 走到这里：拿不到锁或者count非0，需要重新放回链表等待下次软中断
        // 关闭本地中断，保护链表插入操作
        local_irq_disable();
        // 头插法，把当前tasklet重新放回本CPUtasklet链表
        t->next = tasklet_vec[cpu].list;
        tasklet_vec[cpu].list = t;
        // 再次置位TASKLET_SOFTIRQ软中断标记，下次调度时继续处理
        __cpu_raise_softirq(cpu, TASKLET_SOFTIRQ);
        // 恢复本地硬中断
        local_irq_enable();
    }
}

struct tasklet_head tasklet_hi_vec[NR_CPUS] __cacheline_aligned;

/**
 * 
 * raise_softirq(HI_SOFTIRQ)
    ↓
中断返回 / schedule / local_bh_enable → do_softirq
    ↓
tasklet_hi_action 遍历 hi tasklet 链表
 */
static void tasklet_hi_action(struct softirq_action *a)
{
	int cpu = smp_processor_id();
	struct tasklet_struct *list;

	local_irq_disable();
	list = tasklet_hi_vec[cpu].list;
	tasklet_hi_vec[cpu].list = NULL;
	local_irq_enable();

	while (list != NULL) {
		struct tasklet_struct *t = list;

		list = list->next;

		if (tasklet_trylock(t)) {
			if (atomic_read(&t->count) == 0) {
				clear_bit(TASKLET_STATE_SCHED, &t->state);

				t->func(t->data);
				tasklet_unlock(t);
				continue;
			}
			tasklet_unlock(t);
		}
		local_irq_disable();
		t->next = tasklet_hi_vec[cpu].list;
		tasklet_hi_vec[cpu].list = t;
		__cpu_raise_softirq(cpu, HI_SOFTIRQ);
		local_irq_enable();
	}
}

//初始化 tasklet_struct
void tasklet_init(struct tasklet_struct *t,
		  void (*func)(unsigned long), unsigned long data)
{
	t->func = func;
	t->data = data;
	t->state = 0;
	atomic_set(&t->count, 0);
}

/**
 * tasklet_kill - 销毁/等待一个tasklet执行完毕，确保它不再会被调度运行
 * @t: 目标tasklet_struct指针
 *
 * 说明：一般在驱动卸载的时候调用；**禁止在中断上下文调用**
 * 功能：
 * 1. 尝试抢占SCHED标记；如果tasklet已经被调度入队，循环等待它执行完成
 * 2. 等待tasklet执行过程中的锁（TASKLET_STATE_RUN）释放
 * 3. 清除SCHED标记，后续不会再被调度
 */
void tasklet_kill(struct tasklet_struct *t)
{
    // 不允许在中断上下文调用tasklet_kill，会打印警告
    if (in_interrupt())
        printk("Attempt to kill tasklet from interrupt\n");

    // test_and_set_bit：原子尝试置 TASKLET_STATE_SCHED
    // 如果返回真：说明SCHED位已经置1 → tasklet已经在hi/普通链表上，正在等待执行
    while (test_and_set_bit(TASKLET_STATE_SCHED, &t->state)) {
        current->state = TASK_RUNNING;
        do {
            // 设置SCHED_YIELD，主动让出CPU
            current->policy |= SCHED_YIELD;
            schedule();
        // 循环等待，直到SCHED标记被清掉（tasklet执行完、移出链表）
        } while (test_bit(TASKLET_STATE_SCHED, &t->state));
    }

    // 等待 TASKLET_STATE_RUN 位清零：tasklet正在执行中，等它跑完
    tasklet_unlock_wait(t);

    // 清除我们刚刚set的SCHED标记，这个tasklet不再可调度
    clear_bit(TASKLET_STATE_SCHED, &t->state);
}



/* Old style BHs */

static void (*bh_base[32])(void);

//tasklet的数组
struct tasklet_struct bh_task_vec[32];

/* BHs are serialized by spinlock global_bh_lock.

   It is still possible to make synchronize_bh() as
   spin_unlock_wait(&global_bh_lock). This operation is not used
   by kernel now, so that this lock is not made private only
   due to wait_on_irq().

   It can be removed only after auditing all the BHs.
 */
spinlock_t global_bh_lock = SPIN_LOCK_UNLOCKED;
/**
 * bh_action - 老BH(bottom half)的执行入口，由tasklet框架封装调用
 * @nr: BH编号，例如 TIMER_BH、TQUEUE_BH
 *
 * 功能：尝试获取全局BH锁与硬中断锁，执行bh_base数组注册的BH回调；
 *       拿锁失败则重新mark_bh，等待下次机会执行
 * 约束：运行在tasklet上下文（属于TASKLET_SOFTIRQ软中断）；2.4老式BH是串行执行，全局互斥
 * 上下文：软中断(BH/tasklet)上下文，不可睡眠
 * 
 mark_bh(nr)
    ↓
tasklet_hi_schedule(&bh_task_vec[nr])
    ↓
raise_softirq(HI_SOFTIRQ)
    ↓
中断返回 / schedule / local_bh_enable → do_softirq
    ↓
tasklet_hi_action 遍历 hi tasklet 链表
    ↓
bh_action(nr)       // <<===== 这里调用 bh_action
    ↓
拿 global_bh_lock，执行 bh_base[nr]()（timer_bh/tqueue_bh）
 */
static void bh_action(unsigned long nr)
{
    // 获取当前CPU编号
    int cpu = smp_processor_id();

    // 尝试获取全局BH锁：老式BH全局串行，同一时刻整个系统只能跑一个BH
    if (!spin_trylock(&global_bh_lock))
        goto resched;

    // 尝试获取硬irq锁，防止和硬中断上下文冲突
    if (!hardirq_trylock(cpu))
        goto resched_unlock;

    // 如果该BH编号注册了回调函数，则执行对应的BH处理函数
    if (bh_base[nr])
        bh_base[nr]();

    // 释放硬irq锁
    hardirq_endlock(cpu);
    // 释放全局BH锁
    spin_unlock(&global_bh_lock);
    return;

resched_unlock:
    // 获取hardirq_trylock失败，先释放global_bh_lock
    spin_unlock(&global_bh_lock);
resched:
    // 拿锁失败，重新标记该BH为pending，等待下次软中断时机再尝试执行
    mark_bh(nr);
}

//设置老式  BH处理函数
void init_bh(int nr, void (*routine)(void))
{
	bh_base[nr] = routine;
	mb();
}

void remove_bh(int nr)
{
	tasklet_kill(bh_task_vec+nr);
	bh_base[nr] = NULL;
}

// Linux2.4 软中断子系统初始化函数，start_kernel里面调用
void __init softirq_init()
{
    int i;
    // 循环初始化32个老式BH对应的tasklet，bh_task_vec是tasklet数组，对应32个BH编号
    /**
       static inline void mark_bh(int nr)
        {
            // 主要是 bh_action
            tasklet_hi_schedule(bh_task_vec+nr);
        }
     * 加入的是 tasklet_hi_vec 中
     */
    for (i=0; i<32; i++)
        tasklet_init(bh_task_vec+i, bh_action, i);

	// 注册 softirq_vec
    // 注册普通tasklet软中断：TASKLET_SOFTIRQ向量绑定tasklet_action回调
    // tasklet_schedule 会加入 tasklet任务 
    open_softirq(TASKLET_SOFTIRQ, tasklet_action, NULL);
    // 注册高优先级tasklet软中断：HI_SOFTIRQ向量绑定tasklet_hi_action回调
    // tasklet_hi_schedule 会加入 tasklet
    open_softirq(HI_SOFTIRQ, tasklet_hi_action, NULL);
}

void __run_task_queue(task_queue *list)
{
	struct list_head head, *next;
	unsigned long flags;

	spin_lock_irqsave(&tqueue_lock, flags);
	list_add(&head, list);
	list_del_init(list);
	spin_unlock_irqrestore(&tqueue_lock, flags);

	next = head.next;
	while (next != &head) {
		void (*f) (void *);
		struct tq_struct *p;
		void *data;

		p = list_entry(next, struct tq_struct, list);
		next = next->next;
		f = p->routine;
		data = p->data;
		wmb();
		p->sync = 0;
		if (f)
			f(data);
	}
}
