#ifndef __ASM_HARDIRQ_H
#define __ASM_HARDIRQ_H

#include <linux/config.h>
#include <linux/threads.h>
#include <linux/irq.h>

/* entry.S is sensitive to the offsets of these fields */
/**
 * irq_cpustat_t - 每CPU中断/软中断统计状态结构体（Linux2.4）
 * ____cacheline_aligned：整个结构体对齐到cacheline边界，防止SMP伪共享
 * 每个CPU有一份独立的irq_cpustat_t，保存该CPU的中断相关计数器
 */
typedef struct {
	unsigned int __softirq_active;  // 已经触发、待处理的软中断位图（哪类softirq挂起了）
	unsigned int __softirq_mask;    // 软中断屏蔽掩码：哪些软中断类型被禁止执行
	unsigned int __local_irq_count; // 硬中断嵌套计数，in_irq()读取这个
	unsigned int __local_bh_count;  // bottom half软中断执行计数，in_interrupt()会读取它
	unsigned int __syscall_count;   // 系统调用计数（统计当前CPU发生系统调用的次数）
	// NMI = Non-Maskable Interrupt
	unsigned int __nmi_count;       // NMI中断计数，架构相关，统计不可屏蔽中断次数
} ____cacheline_aligned irq_cpustat_t;


#include <linux/irq_cpustat.h>	/* Standard mappings for irq_cpustat_t above */

/*
 * Are we in an interrupt context? Either doing bottom half
 * or hardware interrupt processing?
 */
/**
 * in_interrupt() - 判断当前CPU是否处于中断上下文（Linux 2.4）
 * 返回真：处于硬中断 或 软中断（bottom half）上下文；返回假：进程上下文
 *
 * local_irq_count(__cpu)：硬中断计数；local_bh_count(__cpu)：软中断(bh)计数
 * 只要任意一个不为0，就代表在中断上下文
 */
#define in_interrupt() ({ int __cpu = smp_processor_id(); \
	(local_irq_count(__cpu) + local_bh_count(__cpu) != 0); })

#define in_irq() (local_irq_count(smp_processor_id()) != 0)

#ifndef CONFIG_SMP

#define hardirq_trylock(cpu)	(local_irq_count(cpu) == 0)
#define hardirq_endlock(cpu)	do { } while (0)

#define irq_enter(cpu, irq)	(local_irq_count(cpu)++)
#define irq_exit(cpu, irq)	(local_irq_count(cpu)--)

#define synchronize_irq()	barrier()

#else

#include <asm/atomic.h>
#include <asm/smp.h>

extern unsigned char global_irq_holder;
extern unsigned volatile long global_irq_lock; /* long for set_bit -RR */

static inline int irqs_running (void)
{
	int i;

	for (i = 0; i < smp_num_cpus; i++)
		if (local_irq_count(i))
			return 1;
	return 0;
}

static inline void release_irqlock(int cpu)
{
	/* if we didn't own the irq lock, just ignore.. */
	if (global_irq_holder == (unsigned char) cpu) {
		global_irq_holder = NO_PROC_ID;
		clear_bit(0,&global_irq_lock);
	}
}

static inline void irq_enter(int cpu, int irq)
{
	++local_irq_count(cpu);

	while (test_bit(0,&global_irq_lock)) {
		/* nothing */;
	}
}

static inline void irq_exit(int cpu, int irq)
{
	--local_irq_count(cpu);
}

static inline int hardirq_trylock(int cpu)
{
	return !local_irq_count(cpu) && !test_bit(0,&global_irq_lock);
}

#define hardirq_endlock(cpu)	do { } while (0)

extern void synchronize_irq(void);

#endif /* CONFIG_SMP */

#endif /* __ASM_HARDIRQ_H */
