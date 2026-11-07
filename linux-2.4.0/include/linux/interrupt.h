/* interrupt.h */
#ifndef _LINUX_INTERRUPT_H
#define _LINUX_INTERRUPT_H

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/smp.h>
#include <linux/cache.h>

#include <asm/bitops.h>
#include <asm/atomic.h>
#include <asm/ptrace.h>

// 驱动注册的中断动作节点，多个设备共享IRQ时串成单向链表
struct irqaction {
    // 驱动的中断服务处理函数
    void (*handler)(int, void *, struct pt_regs *);
    // 中断标志位，SA_SHIRQ代表共享中断
    unsigned long flags;
    // 设备名字，在/proc/interrupts展示
    const char *name;
    // 设备id，共享中断区分设备，释放中断时使用，共享中断不能为NULL
    void *dev_id;
    // 链表下一个节点
    struct irqaction *next;
};



/* Who gets which entry in bh_base.  Things which will occur most often
   should come first */
   
enum {
	TIMER_BH = 0,
	TQUEUE_BH,
	DIGI_BH,
	SERIAL_BH,
	RISCOM8_BH,
	SPECIALIX_BH,
	AURORA_BH,
	ESP_BH,
	SCSI_BH,
	IMMEDIATE_BH,
	CYCLADES_BH,
	CM206_BH,
	JS_BH,
	MACSERIAL_BH,
	ISICOM_BH
};

#include <asm/hardirq.h>
#include <asm/softirq.h>



/* PLEASE, avoid to allocate new softirqs, if you need not _really_ high
   frequency threaded job scheduling. For almost all the purposes
   tasklets are more than enough. F.e. all serial device BHs et
   al. should be converted to tasklets, not to softirqs.
 */

// Linux2.4 软中断编号枚举，定义各类softirq的向量下标，用于索引softirq_vec数组
// 枚举编号越小，优先级越高。
// `HI_SOFTIRQ(0) > NET_TX > NET_RX > TASKLET_SOFTIRQ`
// `handle_softirq`遍历的时候**从 0 号开始依次扫描**，编号靠前优先执行。
// 因此 `do_softirq` 中通过枚举值的大小，决定了优先级
enum
{
    HI_SOFTIRQ=0,       // 高优先级tasklet软中断，优先级最高
    NET_TX_SOFTIRQ,     // 网络发送软中断
    NET_RX_SOFTIRQ,     // 网络接收软中断
    TASKLET_SOFTIRQ     // 普通tasklet软中断
};


/* softirq mask and active fields moved to irq_cpustat_t in
 * asm/hardirq.h to get better cache usage.  KAO
 */
// 软中断动作
struct softirq_action
{
    void (*action)(struct softirq_action *); // 软中断处理回调函数
    void *data;                              // 传给回调的私有数据
};

asmlinkage void do_softirq(void);
extern void open_softirq(int nr, void (*action)(struct softirq_action*), void *data);

//设置标记位置
// __softirq_active 位标志
static inline void __cpu_raise_softirq(int cpu, int nr)
{
	softirq_active(cpu) |= (1<<nr);
}


/* I do not want to use atomic variables now, so that cli/sti */
//抛出软中断 
// 网络网卡中断：收到包、发包完成 → raise_softirq(NET_RX_SOFTIRQ) / raise_softirq(NET_TX_SOFTIRQ)
// 块设备中断：IO 完成，把后续的 bio 收尾交给软中断
// 串口、声卡等设备：中断里只做硬件 ack，复杂的解析、缓冲处理丢软中断
// tasklet_schedule() 里面最终会调用 raise_softirq(TASKLET_SOFTIRQ)。
static inline void raise_softirq(int nr)
{
    unsigned long flags;
    local_irq_save(flags);          // 保存当前CPU的中断标志，同时关闭本地硬中断
    __cpu_raise_softirq(smp_processor_id(), nr); // 在当前CPU标记nr号软中断pending
    local_irq_restore(flags);       // 恢复原来的中断标志（恢复关/开中断状态）
}

// 软中断子系统初始化函数声明，start_kernel里面调用
extern void softirq_init(void);     



/* Tasklets --- multithreaded analogue of BHs.

   Main feature differing them of generic softirqs: tasklet
   is running only on one CPU simultaneously.

   Main feature differing them of BHs: different tasklets
   may be run simultaneously on different CPUs.

   Properties:
   * If tasklet_schedule() is called, then tasklet is guaranteed
     to be executed on some cpu at least once after this.
   * If the tasklet is already scheduled, but its excecution is still not
     started, it will be executed only once.
   * If this tasklet is already running on another CPU (or schedule is called
     from tasklet itself), it is rescheduled for later.
   * Tasklet is strictly serialized wrt itself, but not
     wrt another tasklets. If client needs some intertask synchronization,
     he makes it with spinlocks.
 */
// Linux2.4 tasklet结构体，tasklet是封装在softirq之上的下半部机制
struct tasklet_struct
{
    // 链表指针，同一个CPU上待调度的tasklet串成单向链表
	// 每个 CPU 维护一条 tasklet 链表；`tasklet_schedule`会把 tasklet 节点挂到当前 CPU 的 tasklet 链表。
	//`tasklet_action`软中断处理函数遍历这个链表，挨个执行 func。
    struct tasklet_struct *next;
    // tasklet状态位图：TASKLET_STATE_SCHED(已挂起待执行)、TASKLET_STATE_RUN(正在运行)
    unsigned long state;
    // 引用计数，非0代表tasklet被禁用，不能执行
    atomic_t count;
    // tasklet业务处理回调函数
    void (*func)(unsigned long);
    // 传给func回调的自定义参数
    unsigned long data;
};


#define DECLARE_TASKLET(name, func, data) \
struct tasklet_struct name = { NULL, 0, ATOMIC_INIT(0), func, data }

#define DECLARE_TASKLET_DISABLED(name, func, data) \
struct tasklet_struct name = { NULL, 0, ATOMIC_INIT(1), func, data }


enum
{
	TASKLET_STATE_SCHED,	/* Tasklet is scheduled for execution */
	TASKLET_STATE_RUN	/* Tasklet is running (SMP only) */
};

struct tasklet_head
{
	struct tasklet_struct *list;
} __attribute__ ((__aligned__(SMP_CACHE_BYTES)));

extern struct tasklet_head tasklet_vec[NR_CPUS];
extern struct tasklet_head tasklet_hi_vec[NR_CPUS];

#ifdef CONFIG_SMP
#define tasklet_trylock(t) (!test_and_set_bit(TASKLET_STATE_RUN, &(t)->state))
#define tasklet_unlock_wait(t) while (test_bit(TASKLET_STATE_RUN, &(t)->state)) { /* NOTHING */ }
#define tasklet_unlock(t) clear_bit(TASKLET_STATE_RUN, &(t)->state)
#else
#define tasklet_trylock(t) 1
#define tasklet_unlock_wait(t) do { } while (0)
#define tasklet_unlock(t) do { } while (0)
#endif

/**
 * tasklet_schedule - 把tasklet加入当前CPU的tasklet等待链表，触发TASKLET_SOFTIRQ软中断
 * @t: 待调度的tasklet_struct结构体指针
 *
 * 功能：将tasklet标记为待执行，加入本CPU的tasklet链表，设置TASKLET_SOFTIRQ挂起标记
 * 约束：同一个tasklet不会重复入队；tasklet在调用本函数的CPU上执行
 * 上下文：支持硬中断上下文、进程上下文调用；回调函数不能睡眠
 *  open_softirq(TASKLET_SOFTIRQ, tasklet_action, NULL);
 */
static inline void tasklet_schedule(struct tasklet_struct *t)
{
    // 原子测试并置位SCHED标记：未调度才进入分支，避免重复入队
    if (!test_and_set_bit(TASKLET_STATE_SCHED, &t->state)) {
        int cpu = smp_processor_id();
        // 获取当前CPU编号
        unsigned long flags;

        // 保存中断标志，关闭本地硬中断，保护链表临界区
        local_irq_save(flags);
        // 头插法：新节点next指向原链表头
        t->next = tasklet_vec[cpu].list;
        // 更新链表头，挂载当前tasklet
        tasklet_vec[cpu].list = t;
        // 置位TASKLET_SOFTIRQ软中断pending位
        __cpu_raise_softirq(cpu, TASKLET_SOFTIRQ);
        // 恢复本地中断状态
        local_irq_restore(flags);
    }
}

/**
 * tasklet_hi_schedule - 调度高优先级tasklet，触发HI_SOFTIRQ软中断
 * @t: 待调度的tasklet_struct结构体指针
 *
 * 功能：将高优先级tasklet加入当前CPU高优先级tasklet链表，标记HI_SOFTIRQ软中断pending
 * 约束：同一个tasklet不会重复入队；在调用该函数的CPU上执行
 * 上下文：可在硬中断/进程上下文调用；tasklet回调禁止睡眠
 * 区别：相比普通tasklet_schedule，使用HI_SOFTIRQ，优先级高于TASKLET_SOFTIRQ
 *  open_softirq(HI_SOFTIRQ, tasklet_hi_action, NULL);
 */
static inline void tasklet_hi_schedule(struct tasklet_struct *t)
{
    // 原子测试并置位SCHED标记：未调度才进入分支，防止重复入队
    if (!test_and_set_bit(TASKLET_STATE_SCHED, &t->state)) {
        int cpu = smp_processor_id();
        // 获取当前CPU编号
        unsigned long flags;

        // 保存中断标志，关闭本地硬中断，保护高优先级tasklet链表操作
        local_irq_save(flags);
        // 头插法：当前节点next指向原有链表头部
        t->next = tasklet_hi_vec[cpu].list;
        // 更新链表头，挂载高优先级tasklet
        tasklet_hi_vec[cpu].list = t;
        // 置位HI_SOFTIRQ高优先级软中断pending位
        __cpu_raise_softirq(cpu, HI_SOFTIRQ);
        // 恢复本地CPU原始中断状态
        local_irq_restore(flags);
    }
}


static inline void tasklet_disable_nosync(struct tasklet_struct *t)
{
	atomic_inc(&t->count);
}

static inline void tasklet_disable(struct tasklet_struct *t)
{
	tasklet_disable_nosync(t);
	tasklet_unlock_wait(t);
}

static inline void tasklet_enable(struct tasklet_struct *t)
{
	atomic_dec(&t->count);
}

extern void tasklet_kill(struct tasklet_struct *t);
extern void tasklet_init(struct tasklet_struct *t,
			 void (*func)(unsigned long), unsigned long data);

#ifdef CONFIG_SMP

#define SMP_TIMER_NAME(name) name##__thr

#define SMP_TIMER_DEFINE(name, task) \
DECLARE_TASKLET(task, name##__thr, 0); \
static void name (unsigned long dummy) \
{ \
	tasklet_schedule(&(task)); \
}

#else /* CONFIG_SMP */

#define SMP_TIMER_NAME(name) name
#define SMP_TIMER_DEFINE(name, task)

#endif /* CONFIG_SMP */


/* Old BH definitions */

extern struct tasklet_struct bh_task_vec[];

/* It is exported _ONLY_ for wait_on_irq(). */
extern spinlock_t global_bh_lock;

static inline void mark_bh(int nr)
{
	// 主要是 bh_action
	tasklet_hi_schedule(bh_task_vec+nr);
}

extern void init_bh(int nr, void (*routine)(void));
extern void remove_bh(int nr);


/*
 * Autoprobing for irqs:
 *
 * probe_irq_on() and probe_irq_off() provide robust primitives
 * for accurate IRQ probing during kernel initialization.  They are
 * reasonably simple to use, are not "fooled" by spurious interrupts,
 * and, unlike other attempts at IRQ probing, they do not get hung on
 * stuck interrupts (such as unused PS2 mouse interfaces on ASUS boards).
 *
 * For reasonably foolproof probing, use them as follows:
 *
 * 1. clear and/or mask the device's internal interrupt.
 * 2. sti();
 * 3. irqs = probe_irq_on();      // "take over" all unassigned idle IRQs
 * 4. enable the device and cause it to trigger an interrupt.
 * 5. wait for the device to interrupt, using non-intrusive polling or a delay.
 * 6. irq = probe_irq_off(irqs);  // get IRQ number, 0=none, negative=multiple
 * 7. service the device to clear its pending interrupt.
 * 8. loop again if paranoia is required.
 *
 * probe_irq_on() returns a mask of allocated irq's.
 *
 * probe_irq_off() takes the mask as a parameter,
 * and returns the irq number which occurred,
 * or zero if none occurred, or a negative irq number
 * if more than one irq occurred.
 */
extern unsigned long probe_irq_on(void);	/* returns 0 on failure */
extern int probe_irq_off(unsigned long);	/* returns 0 or negative on failure */
extern unsigned int probe_irq_mask(unsigned long);	/* returns mask of ISA interrupts */

#endif
