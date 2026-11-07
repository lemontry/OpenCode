#ifndef __irq_h
#define __irq_h

#include <linux/cache.h>
#include <linux/spinlock.h>

#include <asm/irq.h>
#include <asm/ptrace.h>

/*
 * IRQ line status.
 */
#define IRQ_INPROGRESS	1	/* IRQ handler active - do not enter! */
#define IRQ_DISABLED	2	/* IRQ disabled - do not enter! */
#define IRQ_PENDING	4	/* IRQ pending - replay on enable */
#define IRQ_REPLAY	8	/* IRQ has been replayed but not acked yet */
#define IRQ_AUTODETECT	16	/* IRQ is being autodetected */
#define IRQ_WAITING	32	/* IRQ not yet seen - for autodetection */
#define IRQ_LEVEL	64	/* IRQ level triggered */
#define IRQ_MASKED	128	/* IRQ masked - shouldn't be seen again */
#define IRQ_PER_CPU	256	/* IRQ is per CPU */

/*
 * Interrupt controller descriptor. This is all we need
 * to describe about the low-level hardware. 
 */
/**
 * struct hw_interrupt_type
 * Linux2.4 内核：硬件中断控制器操作抽象结构体（中断芯片描述符）
 * 作用：对不同的中断控制器（8259A PIC / IOAPIC / 本地APIC）做统一抽象，
 *       内核上层中断代码（do_IRQ）不需要关心底层是PIC还是APIC，统一调用这组回调函数。
 * 每个irq_desc->handler 指向该结构。
 */
struct hw_interrupt_type {
	const char * typename;                  // 中断控制器名字，用于打印调试，如 "XT-PIC"、"IO-APIC"
	// 启动/打开中断，第一次启用该IRQ，可做硬件初始化
	unsigned int (*startup)(unsigned int irq);
	// 关闭、销毁IRQ，释放硬件资源
	void (*shutdown)(unsigned int irq);
	// 使能中断（unmask，取消屏蔽）
	void (*enable)(unsigned int irq);
	// 屏蔽中断（mask）
	void (*disable)(unsigned int irq);
	// ack：应答中断，通知中断控制器"我收到这个中断了"，清除中断请求位IRR/ISR
	void (*ack)(unsigned int irq);
	// end：中断处理结束后的收尾动作；判断是否需要重新打开中断
	void (*end)(unsigned int irq);
	// 设置中断亲和性：把这个IRQ绑定到指定CPU掩码（SMP IOAPIC使用）
	void (*set_affinity)(unsigned int irq, unsigned long mask);
};

typedef struct hw_interrupt_type  hw_irq_controller;

/*
 * This is the "IRQ descriptor", which contains various information
 * about the irq, including what kind of hardware handling it has,
 * whether it is disabled etc etc.
 *
 * Pad this out to 32 bytes for cache and indexing reasons.
 */
// Linux2.4 单个IRQ中断描述符，全局数组irq_desc[]，一个IRQ号对应一个实例
// ____cacheline_aligned：结构体对齐到cacheline，SMP避免cache伪共享
typedef struct {
    // IRQ状态位图：IRQ_INPROGRESS/IRQ_DISABLED/IRQ_PENDING等标志
    unsigned int status;
    // 硬件中断控制器操作集，8259A/IOAPIC的底层函数指针集合
    hw_irq_controller *handler;
    // IRQ动作链表，保存驱动注册的中断处理函数，支持共享中断
	//硬件上**多个外设可以共用同一条物理中断信号线**，
	//所以内核设计链表，把多个设备的中断处理函数串起来，中断触发后依次调用全部 handler。
	// 老式 ISA 总线，中断引脚数量有限，硬件设计允许多块设备**并联接在同一根 IRQ 物理线上**
	// 例如：ISA 声卡、网卡可以共用 IRQ5。
	// 当任意一个设备触发中断，这条物理线电平变化，CPU 收到同一个 IRQ 号。
	// request_irq 这个函数中设置和注册
    struct irqaction *action;
    // 中断禁用嵌套计数，disable_irq自增，enable_irq自减；depth>0代表逻辑关闭
	// 	depth == 0：IRQ 开启状态
	//	depth > 0：被 disable_irq/disable_irq_nosync 屏蔽多少次
    unsigned int depth;
    // 自旋锁，保护本结构体内部所有成员，中断上下文使用，不能睡眠
    spinlock_t lock;
} ____cacheline_aligned irq_desc_t;


extern irq_desc_t irq_desc [NR_IRQS];

#include <asm/hw_irq.h> /* the arch dependent stuff */

extern int handle_IRQ_event(unsigned int, struct pt_regs *, struct irqaction *);
extern int setup_irq(unsigned int , struct irqaction * );

extern hw_irq_controller no_irq_type;  /* needed in every arch ? */
extern void no_action(int cpl, void *dev_id, struct pt_regs *regs);

extern volatile unsigned long irq_err_count;

#endif /* __asm_h */

