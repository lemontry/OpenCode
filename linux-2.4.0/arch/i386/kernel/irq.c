/*
 *	linux/arch/i386/kernel/irq.c
 *
 *	Copyright (C) 1992, 1998 Linus Torvalds, Ingo Molnar
 *
 * This file contains the code used by various IRQ handling routines:
 * asking for different IRQ's should be done through these routines
 * instead of just grabbing them. Thus setups with different IRQ numbers
 * shouldn't result in any weird surprises, and installing new handlers
 * should be easier.
 */

/*
 * (mostly architecture independent, will move to kernel/irq.c in 2.5.)
 *
 * IRQs are in fact implemented a bit like signal handlers for the kernel.
 * Naturally it's not a 1:1 relation, but there are similarities.
 */

#include <linux/config.h>
#include <linux/ptrace.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/timex.h>
#include <linux/malloc.h>
#include <linux/random.h>
#include <linux/smp_lock.h>
#include <linux/init.h>
#include <linux/kernel_stat.h>
#include <linux/irq.h>
#include <linux/proc_fs.h>

#include <asm/io.h>
#include <asm/smp.h>
#include <asm/system.h>
#include <asm/bitops.h>
#include <asm/uaccess.h>
#include <asm/pgalloc.h>
#include <asm/delay.h>
#include <asm/desc.h>
#include <asm/irq.h>



/*
 * Linux has a controller-independent x86 interrupt architecture.
 * every controller has a 'controller-template', that is used
 * by the main code to do the right thing. Each driver-visible
 * interrupt source is transparently wired to the apropriate
 * controller. Thus drivers need not be aware of the
 * interrupt-controller.
 *
 * Various interrupt controllers we handle: 8259 PIC, SMP IO-APIC,
 * PIIX4's internal 8259 PIC and SGI's Visual Workstation Cobalt (IO-)APIC.
 * (IO-APICs assumed to be messaging to Pentium local-APICs)
 *
 * the code is designed to be easily extended with new/different
 * interrupt controllers, without having to do assembly magic.
 */

/*
 * Controller mappings for all interrupt sources:
 */
irq_desc_t irq_desc[NR_IRQS] __cacheline_aligned =
	{ [0 ... NR_IRQS-1] = { 0, &no_irq_type, NULL, 0, SPIN_LOCK_UNLOCKED}};

static void register_irq_proc (unsigned int irq);

/*
 * Special irq handlers.
 */

void no_action(int cpl, void *dev_id, struct pt_regs *regs) { }

/*
 * Generic no controller code
 */

static void enable_none(unsigned int irq) { }
static unsigned int startup_none(unsigned int irq) { return 0; }
static void disable_none(unsigned int irq) { }
static void ack_none(unsigned int irq)
{
/*
 * 'what should we do if we get a hw irq event on an illegal vector'.
 * each architecture has to answer this themselves, it doesnt deserve
 * a generic callback i think.
 */
#if CONFIG_X86
	printk("unexpected IRQ trap at vector %02x\n", irq);
#ifdef CONFIG_X86_LOCAL_APIC
	/*
	 * Currently unexpected vectors happen only on SMP and APIC.
	 * We _must_ ack these because every local APIC has only N
	 * irq slots per priority level, and a 'hanging, unacked' IRQ
	 * holds up an irq slot - in excessive cases (when multiple
	 * unexpected vectors occur) that might lock up the APIC
	 * completely.
	 */
	ack_APIC_irq();
#endif
#endif
}

/* startup is the same as "enable", shutdown is same as "disable" */
#define shutdown_none	disable_none
#define end_none	enable_none

struct hw_interrupt_type no_irq_type = {
	"none",
	startup_none,
	shutdown_none,
	enable_none,
	disable_none,
	ack_none,
	end_none
};

volatile unsigned long irq_err_count;

/*
 * Generic, controller-independent functions:
 */

int get_irq_list(char *buf)
{
	int i, j;
	struct irqaction * action;
	char *p = buf;

	p += sprintf(p, "           ");
	for (j=0; j<smp_num_cpus; j++)
		p += sprintf(p, "CPU%d       ",j);
	*p++ = '\n';

	for (i = 0 ; i < NR_IRQS ; i++) {
		action = irq_desc[i].action;
		if (!action) 
			continue;
		p += sprintf(p, "%3d: ",i);
#ifndef CONFIG_SMP
		p += sprintf(p, "%10u ", kstat_irqs(i));
#else
		for (j = 0; j < smp_num_cpus; j++)
			p += sprintf(p, "%10u ",
				kstat.irqs[cpu_logical_map(j)][i]);
#endif
		p += sprintf(p, " %14s", irq_desc[i].handler->typename);
		p += sprintf(p, "  %s", action->name);

		for (action=action->next; action; action = action->next)
			p += sprintf(p, ", %s", action->name);
		*p++ = '\n';
	}
	p += sprintf(p, "NMI: ");
	for (j = 0; j < smp_num_cpus; j++)
		p += sprintf(p, "%10u ",
			nmi_count(cpu_logical_map(j)));
	p += sprintf(p, "\n");
#if CONFIG_SMP
	p += sprintf(p, "LOC: ");
	for (j = 0; j < smp_num_cpus; j++)
		p += sprintf(p, "%10u ",
			apic_timer_irqs[cpu_logical_map(j)]);
	p += sprintf(p, "\n");
#endif
	p += sprintf(p, "ERR: %10lu\n", irq_err_count);
	return p - buf;
}


/*
 * Global interrupt locks for SMP. Allow interrupts to come in on any
 * CPU, yet make cli/sti act globally to protect critical regions..
 */

#ifdef CONFIG_SMP
unsigned char global_irq_holder = NO_PROC_ID;
unsigned volatile long global_irq_lock; /* pendantic: long for set_bit --RR */

extern void show_stack(unsigned long* esp);

static void show(char * str)
{
	int i;
	int cpu = smp_processor_id();

	printk("\n%s, CPU %d:\n", str, cpu);
	printk("irq:  %d [",irqs_running());
	for(i=0;i < smp_num_cpus;i++)
		printk(" %d",local_irq_count(i));
	printk(" ]\nbh:   %d [",spin_is_locked(&global_bh_lock) ? 1 : 0);
	for(i=0;i < smp_num_cpus;i++)
		printk(" %d",local_bh_count(i));

	printk(" ]\nStack dumps:");
	for(i = 0; i < smp_num_cpus; i++) {
		unsigned long esp;
		if (i == cpu)
			continue;
		printk("\nCPU %d:",i);
		esp = init_tss[i].esp0;
		if (!esp) {
			/* tss->esp0 is set to NULL in cpu_init(),
			 * it's initialized when the cpu returns to user
			 * space. -- manfreds
			 */
			printk(" <unknown> ");
			continue;
		}
		esp &= ~(THREAD_SIZE-1);
		esp += sizeof(struct task_struct);
		show_stack((void*)esp);
 	}
	printk("\nCPU %d:",cpu);
	show_stack(NULL);
	printk("\n");
}
	
#define MAXCOUNT 100000000

/*
 * I had a lockup scenario where a tight loop doing
 * spin_unlock()/spin_lock() on CPU#1 was racing with
 * spin_lock() on CPU#0. CPU#0 should have noticed spin_unlock(), but
 * apparently the spin_unlock() information did not make it
 * through to CPU#0 ... nasty, is this by design, do we have to limit
 * 'memory update oscillation frequency' artificially like here?
 *
 * Such 'high frequency update' races can be avoided by careful design, but
 * some of our major constructs like spinlocks use similar techniques,
 * it would be nice to clarify this issue. Set this define to 0 if you
 * want to check whether your system freezes.  I suspect the delay done
 * by SYNC_OTHER_CORES() is in correlation with 'snooping latency', but
 * i thought that such things are guaranteed by design, since we use
 * the 'LOCK' prefix.
 */
#define SUSPECTED_CPU_OR_CHIPSET_BUG_WORKAROUND 0

#if SUSPECTED_CPU_OR_CHIPSET_BUG_WORKAROUND
# define SYNC_OTHER_CORES(x) udelay(x+1)
#else
/*
 * We have to allow irqs to arrive between __sti and __cli
 */
# define SYNC_OTHER_CORES(x) __asm__ __volatile__ ("nop")
#endif

static inline void wait_on_irq(int cpu)
{
	int count = MAXCOUNT;

	for (;;) {

		/*
		 * Wait until all interrupts are gone. Wait
		 * for bottom half handlers unless we're
		 * already executing in one..
		 */
		if (!irqs_running())
			if (local_bh_count(cpu) || !spin_is_locked(&global_bh_lock))
				break;

		/* Duh, we have to loop. Release the lock to avoid deadlocks */
		clear_bit(0,&global_irq_lock);

		for (;;) {
			if (!--count) {
				show("wait_on_irq");
				count = ~0;
			}
			__sti();
			SYNC_OTHER_CORES(cpu);
			__cli();
			if (irqs_running())
				continue;
			if (global_irq_lock)
				continue;
			if (!local_bh_count(cpu) && spin_is_locked(&global_bh_lock))
				continue;
			if (!test_and_set_bit(0,&global_irq_lock))
				break;
		}
	}
}

/*
 * This is called when we want to synchronize with
 * interrupts. We may for example tell a device to
 * stop sending interrupts: but to make sure there
 * are no interrupts that are executing on another
 * CPU we need to call this function.
 */
void synchronize_irq(void)
{
	if (irqs_running()) {
		/* Stupid approach */
		cli();
		sti();
	}
}

static inline void get_irqlock(int cpu)
{
	if (test_and_set_bit(0,&global_irq_lock)) {
		/* do we already hold the lock? */
		if ((unsigned char) cpu == global_irq_holder)
			return;
		/* Uhhuh.. Somebody else got it. Wait.. */
		do {
			do {
			} while (test_bit(0,&global_irq_lock));
		} while (test_and_set_bit(0,&global_irq_lock));		
	}
	/* 
	 * We also to make sure that nobody else is running
	 * in an interrupt context. 
	 */
	wait_on_irq(cpu);

	/*
	 * Ok, finally..
	 */
	global_irq_holder = cpu;
}

#define EFLAGS_IF_SHIFT 9

/*
 * A global "cli()" while in an interrupt context
 * turns into just a local cli(). Interrupts
 * should use spinlocks for the (very unlikely)
 * case that they ever want to protect against
 * each other.
 *
 * If we already have local interrupts disabled,
 * this will not turn a local disable into a
 * global one (problems with spinlocks: this makes
 * save_flags+cli+sti usable inside a spinlock).
 */
void __global_cli(void)
{
	unsigned int flags;

	__save_flags(flags);
	if (flags & (1 << EFLAGS_IF_SHIFT)) {
		int cpu = smp_processor_id();
		__cli();
		if (!local_irq_count(cpu))
			get_irqlock(cpu);
	}
}

void __global_sti(void)
{
	int cpu = smp_processor_id();

	if (!local_irq_count(cpu))
		release_irqlock(cpu);
	__sti();
}

/*
 * SMP flags value to restore to:
 * 0 - global cli
 * 1 - global sti
 * 2 - local cli
 * 3 - local sti
 */
unsigned long __global_save_flags(void)
{
	int retval;
	int local_enabled;
	unsigned long flags;
	int cpu = smp_processor_id();

	__save_flags(flags);
	local_enabled = (flags >> EFLAGS_IF_SHIFT) & 1;
	/* default to local */
	retval = 2 + local_enabled;

	/* check for global flags if we're not in an interrupt */
	if (!local_irq_count(cpu)) {
		if (local_enabled)
			retval = 1;
		if (global_irq_holder == cpu)
			retval = 0;
	}
	return retval;
}

void __global_restore_flags(unsigned long flags)
{
	switch (flags) {
	case 0:
		__global_cli();
		break;
	case 1:
		__global_sti();
		break;
	case 2:
		__cli();
		break;
	case 3:
		__sti();
		break;
	default:
		printk("global_restore_flags: %08lx (%08lx)\n",
			flags, (&flags)[-1]);
	}
}

#endif

/*
 * This should really return information about whether
 * we should do bottom half handling etc. Right now we
 * end up _always_ checking the bottom half, which is a
 * waste of time and is not what some drivers would
 * prefer.
 */
// Linux2.4 遍历IRQ动作链表，执行驱动注册的中断处理函数
// 入参 irq: 中断号；regs: 寄存器现场；action: irqaction链表头节点
int handle_IRQ_event(unsigned int irq, struct pt_regs * regs, struct irqaction * action)
{
    int status;
    // 获取当前CPU编号
    int cpu = smp_processor_id();

    // 进入中断上下文：更新irq_cpustat的中断计数，标记当前CPU正在处理中断
    irq_enter(cpu, irq);

    status = 1; /* Force the "do bottom halves" bit */
    // 如果驱动没有设置SA_INTERRUPT标志，则开启本地中断（允许中断嵌套）
    if (!(action->flags & SA_INTERRUPT))
        __sti();

    // 循环遍历irqaction单向链表，执行所有注册的中断handler（支持共享中断）
    do {
        // 收集所有action的flags到status变量
        status |= action->flags;
        // 调用驱动注册的中断服务函数
        action->handler(irq, action->dev_id, regs);
        // 移动到链表下一个节点
        action = action->next;
    } while (action);

    // 如果标记SA_SAMPLE_RANDOM，采集中断时间信息，作为内核随机数熵源
    if (status & SA_SAMPLE_RANDOM)
        add_interrupt_randomness(irq);

    // 关闭本地中断
    __cli();
    // 退出中断上下文，减少中断计数；退出前会检查并处理软中断
    irq_exit(cpu, irq);

    return status;
}

/*
 * Generic enable/disable code: this just calls
 * down into the PIC-specific version for the actual
 * hardware disable after having gotten the irq
 * controller lock. 
 */
 
/**
 *	disable_irq_nosync - disable an irq without waiting
 *	@irq: Interrupt to disable
 *
 *	Disable the selected interrupt line. Disables of an interrupt
 *	stack. Unlike disable_irq(), this function does not ensure existing
 *	instances of the IRQ handler have completed before returning.
 *
 *	This function may be called from IRQ context.
 */
 
// Linux2.4 屏蔽指定IRQ，不等待正在执行的中断处理函数（nosync = no sync，无同步等待）
static void inline disable_irq_nosync(unsigned int irq)
{
    irq_desc_t *desc = irq_desc + irq;   // 获取该中断对应的irq_desc描述符
    unsigned long flags;
    // 获取irq_desc的自旋锁，同时保存当前CPU中断标志
    spin_lock_irqsave(&desc->lock, flags);
    // depth是嵌套计数：depth++，如果递增前是0，代表第一次屏蔽这个irq
    if (!desc->depth++) {
        desc->status |= IRQ_DISABLED;   // 设置状态标记：该IRQ已被禁用
        desc->handler->disable(irq);    // 调用中断控制器hw_interrupt_type的disable回调，硬件屏蔽该IRQ
    }
    spin_unlock_irqrestore(&desc->lock, flags); // 释放自旋锁，恢复之前的中断标志
}

/**
 *	disable_irq - disable an irq and wait for completion
 *	@irq: Interrupt to disable
 *
 *	Disable the selected interrupt line. Disables of an interrupt
 *	stack. That is for two disables you need two enables. This
 *	function waits for any pending IRQ handlers for this interrupt
 *	to complete before returning. If you use this function while
 *	holding a resource the IRQ handler may need you will deadlock.
 *
 *	This function may be called - with care - from IRQ context.
 */
 
// Linux2.4 关闭指定irq，并且等待正在执行的该irq中断处理完成
void disable_irq(unsigned int irq)
{
    // 第一步：关闭该irq（屏蔽中断控制器上对应的irq，不会等待）
    disable_irq_nosync(irq);
    // 判断：当前CPU不在硬中断上下文（local_irq_count == 0）
    if (!local_irq_count(smp_processor_id())) {
        // 循环等待：直到该irq没有正在执行的handler（IRQ_INPROGRESS清除）
        do {
            barrier();
        } while (irq_desc[irq].status & IRQ_INPROGRESS);
    }
}


/**
 *	enable_irq - enable interrupt handling on an irq
 *	@irq: Interrupt to enable
 *
 *	Re-enables the processing of interrupts on this IRQ line
 *	providing no disable_irq calls are now in effect.
 *
 *	This function may be called from IRQ context.
 */
 
// Linux2.4 打开指定IRQ，和disable_irq / disable_irq_nosync配对
void enable_irq(unsigned int irq)
{
    irq_desc_t *desc = irq_desc + irq;  // 获取该中断描述符
    unsigned long flags;
    // 加锁+保存中断标志，保护irq_desc并发访问
    spin_lock_irqsave(&desc->lock, flags);
    switch (desc->depth) {
    case 1: {
        // depth=1：本次enable之后，屏蔽计数即将归零，可以真正打开硬件中断
        unsigned int status = desc->status & ~IRQ_DISABLED;
        desc->status = status;  // 清除禁用标记 IRQ_DISABLED

        // 如果存在待处理的挂起中断 IRQ_PENDING
        if ((status & (IRQ_PENDING | IRQ_REPLAY)) == IRQ_PENDING) {
            desc->status = status | IRQ_REPLAY;
            hw_resend_irq(desc->handler,irq); // 重发这个挂起的中断
        }
        desc->handler->enable(irq); // 调用中断控制器的enable，硬件打开IRQ
        /* fall-through 故意穿透到default，执行depth-- */
    }
    default:
        desc->depth--;
        break;
    case 0:
        // depth已经是0，说明enable次数 > disable次数，不平衡，打印警告
        printk("enable_irq(%u) unbalanced from %p\n", irq,
               __builtin_return_address(0));
    }
    spin_unlock_irqrestore(&desc->lock, flags);
}

/*
 * do_IRQ handles all normal device IRQ's (the special
 * SMP cross-CPU interrupts have their own specific
 * handlers).
 */
/**
 * do_IRQ - 硬件中断核心C处理入口（Linux2.4 i386，arch/i386/kernel/irq.c）
 * @regs: 栈上保存的寄存器上下文，SAVE_ALL构建的pt_regs
 * 返回值：固定返回1（汇编层使用）
 *
 * 调用来源：entry.S common_interrupt 中 call do_IRQ
 */
asmlinkage unsigned int do_IRQ(struct pt_regs regs)
{
    /*
     * We ack quickly, we don't want the irq controller
     * thinking we're snobs just because some other CPU has
     * disabled global interrupts (we have already done the
     * INT_ACK cycles, it's too late to try to pretend to the
     * controller that we aren't taking the interrupt).
     *
     * 0 return value means that this irq is already being
     * handled by some other CPU. (or is disabled)
     */
    // orig_eax：就是IRQxx_interrupt 压入栈的 nr-256，低8位取出irq号
    int irq = regs.orig_eax & 0xff; /* high bits used in ret_from_ code  */
    // 获取当前CPU编号，SMP多核使用
    int cpu = smp_processor_id();
    // irq_desc全局数组，每个irq对应一个irq_desc_t描述符
    irq_desc_t *desc = irq_desc + irq;
    struct irqaction * action;
    unsigned int status;

    // 统计：记录该CPU上这个IRQ触发次数
    kstat.irqs[cpu][irq]++;

    // 对该irq描述符加自旋锁，保护desc状态并发访问（SMP多核竞争）
    spin_lock(&desc->lock);

    // 调用中断控制器的ack回调：应答硬件中断（PIC/IOAPIC ack）
    desc->handler->ack(irq);

    /*
       REPLAY is when Linux resends an IRQ that was dropped earlier
       WAITING is used by probe to mark irqs that are being tested
    */
    // 清除IRQ_REPLAY、IRQ_WAITING标记，这两个标记本次不参与判断
    status = desc->status & ~(IRQ_REPLAY | IRQ_WAITING);
    // 置 IRQ_PENDING：标记有中断待处理
    status |= IRQ_PENDING; /* we _want_ to handle it */

    /*
     * If the IRQ is disabled for whatever reason, we cannot
     * use the action we have.
     */
    action = NULL;
    // 条件：IRQ没有被禁用，并且当前不在处理中（IRQ_INPROGRESS）
    if (!(status & (IRQ_DISABLED | IRQ_INPROGRESS))) {
        // 取出驱动注册的中断处理链表头 irqaction
        action = desc->action;
        status &= ~IRQ_PENDING; /* we commit to handling，认领本次中断 */
        status |= IRQ_INPROGRESS; /* 标记：正在处理该IRQ，防止多核并发处理同一个IRQ */
    }
    // 回写到irq描述符的status
    desc->status = status;

    /*
     * If there is no IRQ handler or it was disabled, exit early.
       Since we set PENDING, if another processor is handling
       a different instance of this same irq, the other processor
       will take care of it.
     */
    // 没有注册中断处理函数，或者IRQ被禁用，直接跳到out收尾
    if (!action)
        goto out;

    /*
     * Edge triggered interrupts need to remember
     * pending events.
     * This applies to any hw interrupts that allow a second
     * instance of the same irq to arrive while we are in do_IRQ
     * or in the handler. But the code here only handles the _second_
     * instance of the irq, not the third or fourth. So it is mostly
     * useful for irq hardware that does not mask cleanly in an
     * SMP environment.
     */
    // 循环：专门处理边沿触发中断，处理中断期间再次来了同个IRQ的场景
    for (;;) {
        spin_unlock(&desc->lock);
        // 核心：调用驱动注册的中断handler链表
        handle_IRQ_event(irq, &regs, action);
        spin_lock(&desc->lock);
        
        // 如果没有新的pending中断，退出循环
        if (!(desc->status & IRQ_PENDING))
            break;
        // 清除pending标记，继续循环处理这次新来的中断
        desc->status &= ~IRQ_PENDING;
    }
    // 清除正在处理标记，本次IRQ处理完成
    desc->status &= ~IRQ_INPROGRESS;

out:
    /*
     * The ->end() handler has to deal with interrupts which got
     * disabled while the handler was running.
     */
    // 调用中断控制器end回调，结束本次中断（根据硬件做掩码操作等）
	// 通知 PIC，这个 IRQ 的服务已经结束，PIC 可以继续接收**同优先级 / 更低优先级**的中断
    desc->handler->end(irq);
    // 释放irq描述符自旋锁
    spin_unlock(&desc->lock);

    // 检查是否有待执行软中断，如果有，执行do_softirq()
	// do_softirq 的时候，仍然处在中断上下文，此时不能发生进程调度
	// void schedule(void) 也会调用do_softirq，这个是**进程上下文**触发的软中断，这里跑软中断是允许调度的
	// __softirq_mask 和 __softirq_mask 判断
    if (softirq_active(cpu) & softirq_mask(cpu))
        do_softirq();

    return 1;
}

/**
 *	request_irq - allocate an interrupt line
 *	@irq: Interrupt line to allocate
 *	@handler: Function to be called when the IRQ occurs
 *	@irqflags: Interrupt type flags
 *	@devname: An ascii name for the claiming device
 *	@dev_id: A cookie passed back to the handler function
 *
 *	This call allocates interrupt resources and enables the
 *	interrupt line and IRQ handling. From the point this
 *	call is made your handler function may be invoked. Since
 *	your handler function must clear any interrupt the board 
 *	raises, you must take care both to initialise your hardware
 *	and to set up the interrupt handler in the right order.
 *
 *	Dev_id must be globally unique. Normally the address of the
 *	device data structure is used as the cookie. Since the handler
 *	receives this value it makes sense to use it.
 *
 *	If your interrupt is shared you must pass a non NULL dev_id
 *	as this is required when freeing the interrupt.
 *
 *	Flags:
 *
 *	SA_SHIRQ		Interrupt is shared
 *
 *	SA_INTERRUPT		Disable local interrupts while processing
 *
 *	SA_SAMPLE_RANDOM	The interrupt can be used for entropy
 *
 */
 /**
 * int request_irq(unsigned int irq,
 *                 void (*handler)(int, void *, struct pt_regs *),
 *                 unsigned long irqflags,
 *                 const char * devname,
 *                 void *dev_id)
 * Linux2.4 内核：驱动注册硬件中断服务函数的API
 * @irq: 要申请的硬件中断号（IRQ编号）
 * @handler: 驱动的中断服务程序ISR，中断触发后内核回调这个函数
 * @irqflags: 中断标志位，SA_SHIRQ(共享中断) / SA_INTERRUPT(中断上下文关中断)等
 * @devname: 设备名称，会展示在 /proc/interrupts
 * @dev_id: 设备唯一标识，共享中断时用来区分是哪个设备触发中断；free_irq时必须传入
 * 返回值：0成功；负数错误码(-EINVAL参数非法 / -ENOMEM内存不足)
 * 调用位置：设备驱动，在probe阶段注册中断；对应释放接口 free_irq
 */
int request_irq(unsigned int irq,
        void (*handler)(int, void *, struct pt_regs *),
        unsigned long irqflags,
        const char * devname,
        void *dev_id)
{
    int retval;
    struct irqaction * action;
#if 1
    /*
     * Sanity-check: shared interrupts should REALLY pass in
     * a real dev-ID, otherwise we'll have trouble later trying
     * to figure out which interrupt is which (messes up the
     * interrupt freeing logic etc).
     */
    // 安全检查：如果是共享中断SA_SHIRQ，dev_id不能为NULL
    // 共享中断：多个设备共用同一个IRQ号，靠dev_id区分，free_irq也要用dev_id匹配
    if (irqflags & SA_SHIRQ) {
        if (!dev_id)
            printk("Bad boy: %s (at 0x%x) called us without a dev_id!\n", devname, (&irq)[-1]);
    }
#endif
    // 校验IRQ号是否越界
    if (irq >= NR_IRQS)
        return -EINVAL;
    // 中断处理函数不能为空
    if (!handler)
        return -EINVAL;
    // 分配irqaction结构体，GFP_KERNEL：允许睡眠分配内存
    action = (struct irqaction *)
            kmalloc(sizeof(struct irqaction), GFP_KERNEL);
    if (!action)
        return -ENOMEM;
    // 填充irqaction结构体：保存驱动中断处理函数、标志、设备名、dev_id
    action->handler = handler;
    action->flags = irqflags;
    action->mask = 0;
    action->name = devname;
    action->next = NULL;
    action->dev_id = dev_id;
    // 核心函数：把action挂载到irq_desc的action链表上，完成注册
    retval = setup_irq(irq, action);
    // 注册失败，释放刚才分配的irqaction内存，防止内存泄漏
    if (retval)
        kfree(action);
    return retval;
}
/**
 *	free_irq - free an interrupt
 *	@irq: Interrupt line to free
 *	@dev_id: Device identity to free
 *
 *	Remove an interrupt handler. The handler is removed and if the
 *	interrupt line is no longer in use by any driver it is disabled.
 *	On a shared IRQ the caller must ensure the interrupt is disabled
 *	on the card it drives before calling this function. The function
 *	does not return until any executing interrupts for this IRQ
 *	have completed.
 *
 *	This function may be called from interrupt context. 
 *
 *	Bugs: Attempting to free an irq in a handler for the same irq hangs
 *	      the machine.
 */
// Linux2.4 释放中断函数，和request_irq/setup_irq成对使用，卸载驱动时注销中断
// irq：中断号；dev_id：注册中断时传入的设备id，用来定位要删除的irqaction节点
void free_irq(unsigned int irq, void *dev_id)
{
    irq_desc_t *desc;
    struct irqaction **p;
    unsigned long flags;
    // 合法性检查：中断号超出最大支持范围直接返回
    if (irq >= NR_IRQS)
        return;
    // 获取对应中断号的irq_desc描述符
    desc = irq_desc + irq;
    // 加自旋锁+保存中断状态，原子操作保护irqaction链表
    spin_lock_irqsave(&desc->lock,flags);
    // p是二级指针，遍历链表用
    p = &desc->action;
    for (;;) {
        struct irqaction * action = *p;
        if (action) {
            struct irqaction **pp = p;
            p = &action->next;
            // 根据dev_id匹配节点，共享中断必须依靠dev_id区分不同设备
            if (action->dev_id != dev_id)
                continue;
            // 找到目标节点，将节点从单向链表摘除
            *pp = action->next;
            // 如果摘除后链表为空（没有其他设备共享该IRQ）
            if (!desc->action) {
                desc->status |= IRQ_DISABLED; // 标记IRQ为禁用
                desc->handler->shutdown(irq); // 调用硬件控制器shutdown，关闭硬件中断
            }
            // 释放自旋锁，恢复中断标志
            spin_unlock_irqrestore(&desc->lock,flags);
#ifdef CONFIG_SMP
            // SMP多核场景：等待，确保其他CPU上正在执行的中断handler执行完毕
            while (desc->status & IRQ_INPROGRESS)
                barrier();
#endif
            // 释放irqaction结构体内存
            kfree(action);
            return;
        }
        // 链表为空，说明重复释放，打印警告
        printk("Trying to free free IRQ%d\n",irq);
        spin_unlock_irqrestore(&desc->lock,flags);
        return;
    }
}

/*
 * IRQ autodetection code..
 *
 * This depends on the fact that any interrupt that
 * comes in on to an unassigned handler will get stuck
 * with "IRQ_WAITING" cleared and the interrupt
 * disabled.
 */

static DECLARE_MUTEX(probe_sem);

/**
 *	probe_irq_on	- begin an interrupt autodetect
 *
 *	Commence probing for an interrupt. The interrupts are scanned
 *	and a mask of potential interrupt lines is returned.
 *
 */
 
unsigned long probe_irq_on(void)
{
	unsigned int i;
	irq_desc_t *desc;
	unsigned long val;
	unsigned long delay;

	down(&probe_sem);
	/* 
	 * something may have generated an irq long ago and we want to
	 * flush such a longstanding irq before considering it as spurious. 
	 */
	for (i = NR_IRQS-1; i > 0; i--)  {
		desc = irq_desc + i;

		spin_lock_irq(&desc->lock);
		if (!irq_desc[i].action) 
			irq_desc[i].handler->startup(i);
		spin_unlock_irq(&desc->lock);
	}

	/* Wait for longstanding interrupts to trigger. */
	for (delay = jiffies + HZ/50; time_after(delay, jiffies); )
		/* about 20ms delay */ synchronize_irq();

	/*
	 * enable any unassigned irqs
	 * (we must startup again here because if a longstanding irq
	 * happened in the previous stage, it may have masked itself)
	 */
	for (i = NR_IRQS-1; i > 0; i--) {
		desc = irq_desc + i;

		spin_lock_irq(&desc->lock);
		if (!desc->action) {
			desc->status |= IRQ_AUTODETECT | IRQ_WAITING;
			if (desc->handler->startup(i))
				desc->status |= IRQ_PENDING;
		}
		spin_unlock_irq(&desc->lock);
	}

	/*
	 * Wait for spurious interrupts to trigger
	 */
	for (delay = jiffies + HZ/10; time_after(delay, jiffies); )
		/* about 100ms delay */ synchronize_irq();

	/*
	 * Now filter out any obviously spurious interrupts
	 */
	val = 0;
	for (i = 0; i < NR_IRQS; i++) {
		irq_desc_t *desc = irq_desc + i;
		unsigned int status;

		spin_lock_irq(&desc->lock);
		status = desc->status;

		if (status & IRQ_AUTODETECT) {
			/* It triggered already - consider it spurious. */
			if (!(status & IRQ_WAITING)) {
				desc->status = status & ~IRQ_AUTODETECT;
				desc->handler->shutdown(i);
			} else
				if (i < 32)
					val |= 1 << i;
		}
		spin_unlock_irq(&desc->lock);
	}

	return val;
}

/*
 * Return a mask of triggered interrupts (this
 * can handle only legacy ISA interrupts).
 */
 
/**
 *	probe_irq_mask - scan a bitmap of interrupt lines
 *	@val:	mask of interrupts to consider
 *
 *	Scan the ISA bus interrupt lines and return a bitmap of
 *	active interrupts. The interrupt probe logic state is then
 *	returned to its previous value.
 *
 *	Note: we need to scan all the irq's even though we will
 *	only return ISA irq numbers - just so that we reset them
 *	all to a known state.
 */
unsigned int probe_irq_mask(unsigned long val)
{
	int i;
	unsigned int mask;

	mask = 0;
	for (i = 0; i < NR_IRQS; i++) {
		irq_desc_t *desc = irq_desc + i;
		unsigned int status;

		spin_lock_irq(&desc->lock);
		status = desc->status;

		if (status & IRQ_AUTODETECT) {
			if (i < 16 && !(status & IRQ_WAITING))
				mask |= 1 << i;

			desc->status = status & ~IRQ_AUTODETECT;
			desc->handler->shutdown(i);
		}
		spin_unlock_irq(&desc->lock);
	}
	up(&probe_sem);

	return mask & val;
}

/*
 * Return the one interrupt that triggered (this can
 * handle any interrupt source).
 */

/**
 *	probe_irq_off	- end an interrupt autodetect
 *	@val: mask of potential interrupts (unused)
 *
 *	Scans the unused interrupt lines and returns the line which
 *	appears to have triggered the interrupt. If no interrupt was
 *	found then zero is returned. If more than one interrupt is
 *	found then minus the first candidate is returned to indicate
 *	their is doubt.
 *
 *	The interrupt probe logic state is returned to its previous
 *	value.
 *
 *	BUGS: When used in a module (which arguably shouldnt happen)
 *	nothing prevents two IRQ probe callers from overlapping. The
 *	results of this are non-optimal.
 */
 
int probe_irq_off(unsigned long val)
{
	int i, irq_found, nr_irqs;

	nr_irqs = 0;
	irq_found = 0;
	for (i = 0; i < NR_IRQS; i++) {
		irq_desc_t *desc = irq_desc + i;
		unsigned int status;

		spin_lock_irq(&desc->lock);
		status = desc->status;

		if (status & IRQ_AUTODETECT) {
			if (!(status & IRQ_WAITING)) {
				if (!nr_irqs)
					irq_found = i;
				nr_irqs++;
			}
			desc->status = status & ~IRQ_AUTODETECT;
			desc->handler->shutdown(i);
		}
		spin_unlock_irq(&desc->lock);
	}
	up(&probe_sem);

	if (nr_irqs > 1)
		irq_found = -irq_found;
	return irq_found;
}

/* this was setup_x86_irq but it seems pretty generic */
// Linux2.4 底层注册中断action的核心函数，request_irq内部会调用setup_irq
// irq：要注册的中断号；new：待插入的irqaction节点
int setup_irq(unsigned int irq, struct irqaction * new)
{
	int shared = 0;
	unsigned long flags;
	struct irqaction *old, **p;

	//中断的描述信息
	irq_desc_t *desc = irq_desc + irq;

	/*
	 * Some drivers like serial.c use request_irq() heavily,
	 * so we have to be careful not to interfere with a
	 * running system.
	 */
	// 如果新action带有SA_SAMPLE_RANDOM标记，初始化该irq的随机熵采集
    // rand_initialize_irq可能睡眠，必须放到自旋锁外面执行
	if (new->flags & SA_SAMPLE_RANDOM) {
		/*
		 * This function might sleep, we want to call it first,
		 * outside of the atomic block.
		 * Yes, this might clear the entropy pool if the wrong
		 * driver is attempted to be loaded, without actually
		 * installing a new handler, but is this really a problem,
		 * only the sysadmin is able to do this.
		 */
		rand_initialize_irq(irq);
	}

	/*
	 * The following block of code has to be executed atomically
	 */
	spin_lock_irqsave(&desc->lock,flags);
	p = &desc->action;

	// 判断当前irq是否已经存在action（已有中断处理函数）
	if ((old = *p) != NULL) {
		/* Can't share interrupts unless both agree to */
		// 已有handler，检查是否双方都开启SA_SHIRQ共享标记
        // 老节点 和 新节点必须同时带有SA_SHIRQ，才允许共享IRQ
		if (!(old->flags & new->flags & SA_SHIRQ)) {
			spin_unlock_irqrestore(&desc->lock,flags);
			return -EBUSY;
		}

		/* add new interrupt at end of irq queue */
		// 共享中断场景：遍历到链表尾部，新节点追加到链表末尾
		do {
			p = &old->next;
			old = *p;
		} while (old);
		shared = 1;
	}

	*p = new;

	// 如果不是共享中断（链表之前为空，第一次注册）
    if (!shared) {
        desc->depth = 0; // 清空中断禁用嵌套计数
        // 清除IRQ禁用、自动探测、等待标记
        desc->status &= ~(IRQ_DISABLED | IRQ_AUTODETECT | IRQ_WAITING);
        // 调用硬件控制器startup回调，使能硬件IRQ
        desc->handler->startup(irq);
    }
	spin_unlock_irqrestore(&desc->lock,flags);

	//为当前中断注册proc函数
	register_irq_proc(irq);
	return 0;
}

static struct proc_dir_entry * root_irq_dir;
static struct proc_dir_entry * irq_dir [NR_IRQS];
static struct proc_dir_entry * smp_affinity_entry [NR_IRQS];

static unsigned long irq_affinity [NR_IRQS] = { [0 ... NR_IRQS-1] = ~0UL };

#define HEX_DIGITS 8

static int irq_affinity_read_proc (char *page, char **start, off_t off,
			int count, int *eof, void *data)
{
	if (count < HEX_DIGITS+1)
		return -EINVAL;
	return sprintf (page, "%08lx\n", irq_affinity[(long)data]);
}

static unsigned int parse_hex_value (const char *buffer,
		unsigned long count, unsigned long *ret)
{
	unsigned char hexnum [HEX_DIGITS];
	unsigned long value;
	int i;

	if (!count)
		return -EINVAL;
	if (count > HEX_DIGITS)
		count = HEX_DIGITS;
	if (copy_from_user(hexnum, buffer, count))
		return -EFAULT;

	/*
	 * Parse the first 8 characters as a hex string, any non-hex char
	 * is end-of-string. '00e1', 'e1', '00E1', 'E1' are all the same.
	 */
	value = 0;

	for (i = 0; i < count; i++) {
		unsigned int c = hexnum[i];

		switch (c) {
			case '0' ... '9': c -= '0'; break;
			case 'a' ... 'f': c -= 'a'-10; break;
			case 'A' ... 'F': c -= 'A'-10; break;
		default:
			goto out;
		}
		value = (value << 4) | c;
	}
out:
	*ret = value;
	return 0;
}

static int irq_affinity_write_proc (struct file *file, const char *buffer,
					unsigned long count, void *data)
{
	int irq = (long) data, full_count = count, err;
	unsigned long new_value;

	if (!irq_desc[irq].handler->set_affinity)
		return -EIO;

	err = parse_hex_value(buffer, count, &new_value);

#if CONFIG_SMP
	/*
	 * Do not allow disabling IRQs completely - it's a too easy
	 * way to make the system unusable accidentally :-) At least
	 * one online CPU still has to be targeted.
	 */
	if (!(new_value & cpu_online_map))
		return -EINVAL;
#endif

	irq_affinity[irq] = new_value;
	irq_desc[irq].handler->set_affinity(irq, new_value);

	return full_count;
}

static int prof_cpu_mask_read_proc (char *page, char **start, off_t off,
			int count, int *eof, void *data)
{
	unsigned long *mask = (unsigned long *) data;
	if (count < HEX_DIGITS+1)
		return -EINVAL;
	return sprintf (page, "%08lx\n", *mask);
}

static int prof_cpu_mask_write_proc (struct file *file, const char *buffer,
					unsigned long count, void *data)
{
	unsigned long *mask = (unsigned long *) data, full_count = count, err;
	unsigned long new_value;

	err = parse_hex_value(buffer, count, &new_value);
	if (err)
		return err;

	*mask = new_value;
	return full_count;
}

#define MAX_NAMELEN 10

// Linux2.4 创建 /proc/irq/<irq号> 目录以及 smp_affinity 文件
static void register_irq_proc (unsigned int irq)
{
    struct proc_dir_entry *entry;
    char name [MAX_NAMELEN];
    // 条件判断：
    // root_irq_dir：/proc/irq 根目录是否存在
    // handler == &no_irq_type：该irq没有有效的中断控制器
    // irq_dir[irq] != NULL：已经创建过该irq的proc目录，避免重复创建
    if (!root_irq_dir || (irq_desc[irq].handler == &no_irq_type) ||
            irq_dir[irq])
        return;
    memset(name, 0, MAX_NAMELEN);
    sprintf(name, "%d", irq);
    /* create /proc/irq/1234 */
    // 创建 /proc/irq/irq号 目录
    irq_dir[irq] = proc_mkdir(name, root_irq_dir);
    /* create /proc/irq/1234/smp_affinity */
    // 在上面目录里创建 smp_affinity 文件，权限0600，仅root读写
    entry = create_proc_entry("smp_affinity", 0600, irq_dir[irq]);
    entry->nlink = 1;
    // 私有data存irq编号，read/write回调里面拿到这个irq
    entry->data = (void *)(long)irq;
    // 绑定读写回调：读取、修改中断CPU亲和掩码
    entry->read_proc = irq_affinity_read_proc;
    entry->write_proc = irq_affinity_write_proc;
    // 保存entry指针，后续可以使用
    smp_affinity_entry[irq] = entry;
}

unsigned long prof_cpu_mask = -1;

void init_irq_proc (void)
{
	struct proc_dir_entry *entry;
	int i;

	/* create /proc/irq */
	root_irq_dir = proc_mkdir("irq", 0);

	/* create /proc/irq/prof_cpu_mask */
	entry = create_proc_entry("prof_cpu_mask", 0600, root_irq_dir);

	entry->nlink = 1;
	entry->data = (void *)&prof_cpu_mask;
	entry->read_proc = prof_cpu_mask_read_proc;
	entry->write_proc = prof_cpu_mask_write_proc;

	/*
	 * Create entries for all existing IRQs.
	 */
	for (i = 0; i < NR_IRQS; i++)
		register_irq_proc(i);
}

