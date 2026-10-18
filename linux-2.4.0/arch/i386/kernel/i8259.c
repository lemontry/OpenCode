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

#include <asm/system.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/bitops.h>
#include <asm/pgtable.h>
#include <asm/delay.h>
#include <asm/desc.h>

#include <linux/irq.h>

/*
 * Common place to define all x86 IRQ vectors
 *
 * This builds up the IRQ handler stubs using some ugly macros in irq.h
 *
 * These macros create the low-level assembly IRQ routines that save
 * register context and call do_IRQ(). do_IRQ() then does all the
 * operations that are needed to keep the AT (or SMP IOAPIC)
 * interrupt-controller happy.
 */

BUILD_COMMON_IRQ()

#define BI(x,y) \
	BUILD_IRQ(x##y)

#define BUILD_16_IRQS(x) \
	BI(x,0) BI(x,1) BI(x,2) BI(x,3) \
	BI(x,4) BI(x,5) BI(x,6) BI(x,7) \
	BI(x,8) BI(x,9) BI(x,a) BI(x,b) \
	BI(x,c) BI(x,d) BI(x,e) BI(x,f)

/*
 * ISA PIC or low IO-APIC triggered (INTA-cycle or APIC) interrupts:
 * (these are usually mapped to vectors 0x20-0x2f)
 */
BUILD_16_IRQS(0x0)

#ifdef CONFIG_X86_IO_APIC
/*
 * The IO-APIC gives us many more interrupt sources. Most of these 
 * are unused but an SMP system is supposed to have enough memory ...
 * sometimes (mostly wrt. hw bugs) we get corrupted vectors all
 * across the spectrum, so we really want to be prepared to get all
 * of these. Plus, more powerful systems might have more than 64
 * IO-APIC registers.
 *
 * (these are usually mapped into the 0x30-0xff vector range)
 */
		   BUILD_16_IRQS(0x1) BUILD_16_IRQS(0x2) BUILD_16_IRQS(0x3)
BUILD_16_IRQS(0x4) BUILD_16_IRQS(0x5) BUILD_16_IRQS(0x6) BUILD_16_IRQS(0x7)
BUILD_16_IRQS(0x8) BUILD_16_IRQS(0x9) BUILD_16_IRQS(0xa) BUILD_16_IRQS(0xb)
BUILD_16_IRQS(0xc) BUILD_16_IRQS(0xd)
#endif

#undef BUILD_16_IRQS
#undef BI


/*
 * The following vectors are part of the Linux architecture, there
 * is no hardware IRQ pin equivalent for them, they are triggered
 * through the ICC by us (IPIs)
 */
#ifdef CONFIG_SMP
BUILD_SMP_INTERRUPT(reschedule_interrupt,RESCHEDULE_VECTOR)
BUILD_SMP_INTERRUPT(invalidate_interrupt,INVALIDATE_TLB_VECTOR)
BUILD_SMP_INTERRUPT(call_function_interrupt,CALL_FUNCTION_VECTOR)
#endif

/*
 * every pentium local APIC has two 'local interrupts', with a
 * soft-definable vector attached to both interrupts, one of
 * which is a timer interrupt, the other one is error counter
 * overflow. Linux uses the local APIC timer interrupt to get
 * a much simpler SMP time architecture:
 */
#ifdef CONFIG_X86_LOCAL_APIC
BUILD_SMP_TIMER_INTERRUPT(apic_timer_interrupt,LOCAL_TIMER_VECTOR)
BUILD_SMP_INTERRUPT(error_interrupt,ERROR_APIC_VECTOR)
BUILD_SMP_INTERRUPT(spurious_interrupt,SPURIOUS_APIC_VECTOR)
#endif

/**
 * IRQ(x,y)：宏，## 是C预处理器的**符号拼接运算符**
 * IRQ##x##y##_interrupt 拼接成 IRQxy_interrupt
 * 例：IRQ(0,0) → IRQ00_interrupt
 */
#define IRQ(x,y) \
    IRQ##x##y##_interrupt

/**
 * IRQLIST_16(x)：一次性展开16个IRQ符号，0~f（16个）
 * 用于批量生成数组初始化列表，一行生成16个函数指针
 */
#define IRQLIST_16(x) \
    IRQ(x,0), IRQ(x,1), IRQ(x,2), IRQ(x,3), \
    IRQ(x,4), IRQ(x,5), IRQ(x,6), IRQ(x,7), \
    IRQ(x,8), IRQ(x,9), IRQ(x,a), IRQ(x,b), \
    IRQ(x,c), IRQ(x,d), IRQ(x,e), IRQ(x,f)

/**
 * interrupt[]：全局函数指针数组
 * 数组每个元素，存放对应IRQ的汇编stub入口地址
 * NR_IRQS：最大IRQ数量
 */
void (*interrupt[NR_IRQS])(void) = {
    IRQLIST_16(0x0),  // IRQ00_interrupt ~ IRQ0f_interrupt（16项）
#ifdef CONFIG_X86_IO_APIC
    // 如果开启IOAPIC，继续追加更多16个一组的IRQ入口
    IRQLIST_16(0x1), IRQLIST_16(0x2), IRQLIST_16(0x3),
    IRQLIST_16(0x4), IRQLIST_16(0x5), IRQLIST_16(0x6), IRQLIST_16(0x7),
    IRQLIST_16(0x8), IRQLIST_16(0x9), IRQLIST_16(0xa), IRQLIST_16(0xb),
    IRQLIST_16(0xc), IRQLIST_16(0xd)
#endif
};


#undef IRQ
#undef IRQLIST_16

/*
 * This is the 'legacy' 8259A Programmable Interrupt Controller,
 * present in the majority of PC/AT boxes.
 * plus some generic x86 specific things if generic specifics makes
 * any sense at all.
 * this file should become arch/i386/kernel/irq.c when the old irq.c
 * moves to arch independent land
 */

spinlock_t i8259A_lock = SPIN_LOCK_UNLOCKED;

static void end_8259A_irq (unsigned int irq)
{
	if (!(irq_desc[irq].status & (IRQ_DISABLED|IRQ_INPROGRESS)))
		enable_8259A_irq(irq);
}

#define shutdown_8259A_irq	disable_8259A_irq

void mask_and_ack_8259A(unsigned int);

static unsigned int startup_8259A_irq(unsigned int irq)
{ 
	enable_8259A_irq(irq);
	return 0; /* never anything pending */
}

static struct hw_interrupt_type i8259A_irq_type = {
	"XT-PIC",
	startup_8259A_irq,
	shutdown_8259A_irq,
	enable_8259A_irq,
	disable_8259A_irq,
	mask_and_ack_8259A,
	end_8259A_irq,
	NULL
};

/*
 * 8259A PIC functions to handle ISA devices:
 */

/*
 * This contains the irq mask for both 8259A irq controllers,
 */
static unsigned int cached_irq_mask = 0xffff;

#define __byte(x,y) 	(((unsigned char *)&(y))[x])
#define cached_21	(__byte(0,cached_irq_mask))
#define cached_A1	(__byte(1,cached_irq_mask))

/*
 * Not all IRQs can be routed through the IO-APIC, eg. on certain (older)
 * boards the timer interrupt is not really connected to any IO-APIC pin,
 * it's fed to the master 8259A's IR0 line only.
 *
 * Any '1' bit in this mask means the IRQ is routed through the IO-APIC.
 * this 'mixed mode' IRQ handling costs nothing because it's only used
 * at IRQ setup time.
 */
unsigned long io_apic_irqs;

void disable_8259A_irq(unsigned int irq)
{
	unsigned int mask = 1 << irq;
	unsigned long flags;

	spin_lock_irqsave(&i8259A_lock, flags);
	cached_irq_mask |= mask;
	if (irq & 8)
		outb(cached_A1,0xA1);
	else
		outb(cached_21,0x21);
	spin_unlock_irqrestore(&i8259A_lock, flags);
}

void enable_8259A_irq(unsigned int irq)
{
	unsigned int mask = ~(1 << irq);
	unsigned long flags;

	spin_lock_irqsave(&i8259A_lock, flags);
	cached_irq_mask &= mask;
	if (irq & 8)
		outb(cached_A1,0xA1);
	else
		outb(cached_21,0x21);
	spin_unlock_irqrestore(&i8259A_lock, flags);
}

int i8259A_irq_pending(unsigned int irq)
{
	unsigned int mask = 1<<irq;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&i8259A_lock, flags);
	if (irq < 8)
		ret = inb(0x20) & mask;
	else
		ret = inb(0xA0) & (mask >> 8);
	spin_unlock_irqrestore(&i8259A_lock, flags);

	return ret;
}

void make_8259A_irq(unsigned int irq)
{
	disable_irq_nosync(irq);
	io_apic_irqs &= ~(1<<irq);
	irq_desc[irq].handler = &i8259A_irq_type;
	enable_irq(irq);
}

/*
 * This function assumes to be called rarely. Switching between
 * 8259A registers is slow.
 * This has to be protected by the irq controller spinlock
 * before being called.
 */
static inline int i8259A_irq_real(unsigned int irq)
{
	int value;
	int irqmask = 1<<irq;

	if (irq < 8) {
		outb(0x0B,0x20);		/* ISR register */
		value = inb(0x20) & irqmask;
		outb(0x0A,0x20);		/* back to the IRR register */
		return value;
	}
	outb(0x0B,0xA0);		/* ISR register */
	value = inb(0xA0) & (irqmask >> 8);
	outb(0x0A,0xA0);		/* back to the IRR register */
	return value;
}

/*
 * Careful! The 8259A is a fragile beast, it pretty
 * much _has_ to be done exactly like this (mask it
 * first, _then_ send the EOI, and the order of EOI
 * to the two 8259s is important!
 */
void mask_and_ack_8259A(unsigned int irq)
{
	unsigned int irqmask = 1 << irq;
	unsigned long flags;

	spin_lock_irqsave(&i8259A_lock, flags);
	/*
	 * Lightweight spurious IRQ detection. We do not want
	 * to overdo spurious IRQ handling - it's usually a sign
	 * of hardware problems, so we only do the checks we can
	 * do without slowing down good hardware unnecesserily.
	 *
	 * Note that IRQ7 and IRQ15 (the two spurious IRQs
	 * usually resulting from the 8259A-1|2 PICs) occur
	 * even if the IRQ is masked in the 8259A. Thus we
	 * can check spurious 8259A IRQs without doing the
	 * quite slow i8259A_irq_real() call for every IRQ.
	 * This does not cover 100% of spurious interrupts,
	 * but should be enough to warn the user that there
	 * is something bad going on ...
	 */
	if (cached_irq_mask & irqmask)
		goto spurious_8259A_irq;
	cached_irq_mask |= irqmask;

handle_real_irq:
	if (irq & 8) {
		inb(0xA1);		/* DUMMY - (do we need this?) */
		outb(cached_A1,0xA1);
		outb(0x60+(irq&7),0xA0);/* 'Specific EOI' to slave */
		outb(0x62,0x20);	/* 'Specific EOI' to master-IRQ2 */
	} else {
		inb(0x21);		/* DUMMY - (do we need this?) */
		outb(cached_21,0x21);
		outb(0x60+irq,0x20);	/* 'Specific EOI' to master */
	}
	spin_unlock_irqrestore(&i8259A_lock, flags);
	return;

spurious_8259A_irq:
	/*
	 * this is the slow path - should happen rarely.
	 */
	if (i8259A_irq_real(irq))
		/*
		 * oops, the IRQ _is_ in service according to the
		 * 8259A - not spurious, go handle it.
		 */
		goto handle_real_irq;

	{
		static int spurious_irq_mask;
		/*
		 * At this point we can be sure the IRQ is spurious,
		 * lets ACK and report it. [once per IRQ]
		 */
		if (!(spurious_irq_mask & irqmask)) {
			printk("spurious 8259A interrupt: IRQ%d.\n", irq);
			spurious_irq_mask |= irqmask;
		}
		irq_err_count++;
		/*
		 * Theoretically we do not have to handle this IRQ,
		 * but in Linux this does not cause problems and is
		 * simpler for us.
		 */
		goto handle_real_irq;
	}
}

void __init init_8259A(int auto_eoi)
{
	unsigned long flags;

	spin_lock_irqsave(&i8259A_lock, flags);

	outb(0xff, 0x21);	/* mask all of 8259A-1 */
	outb(0xff, 0xA1);	/* mask all of 8259A-2 */

	/*
	 * outb_p - this has to work on a wide range of PC hardware.
	 */
	outb_p(0x11, 0x20);	/* ICW1: select 8259A-1 init */
	outb_p(0x20 + 0, 0x21);	/* ICW2: 8259A-1 IR0-7 mapped to 0x20-0x27 */
	outb_p(0x04, 0x21);	/* 8259A-1 (the master) has a slave on IR2 */
	if (auto_eoi)
		outb_p(0x03, 0x21);	/* master does Auto EOI */
	else
		outb_p(0x01, 0x21);	/* master expects normal EOI */

	outb_p(0x11, 0xA0);	/* ICW1: select 8259A-2 init */
	outb_p(0x20 + 8, 0xA1);	/* ICW2: 8259A-2 IR0-7 mapped to 0x28-0x2f */
	outb_p(0x02, 0xA1);	/* 8259A-2 is a slave on master's IR2 */
	outb_p(0x01, 0xA1);	/* (slave's support for AEOI in flat mode
				    is to be investigated) */

	if (auto_eoi)
		/*
		 * in AEOI mode we just have to mask the interrupt
		 * when acking.
		 */
		i8259A_irq_type.ack = disable_8259A_irq;
	else
		i8259A_irq_type.ack = mask_and_ack_8259A;

	udelay(100);		/* wait for 8259A to initialize */

	outb(cached_21, 0x21);	/* restore master IRQ mask */
	outb(cached_A1, 0xA1);	/* restore slave IRQ mask */

	spin_unlock_irqrestore(&i8259A_lock, flags);
}

/*
 * Note that on a 486, we don't want to do a SIGFPE on an irq13
 * as the irq is unreliable, and exception 16 works correctly
 * (ie as explained in the intel literature). On a 386, you
 * can't use exception 16 due to bad IBM design, so we have to
 * rely on the less exact irq13.
 *
 * Careful.. Not only is IRQ13 unreliable, but it is also
 * leads to races. IBM designers who came up with it should
 * be shot.
 */
 
static void math_error_irq(int cpl, void *dev_id, struct pt_regs *regs)
{
	extern void math_error(void *);
	outb(0,0xF0);
	if (ignore_irq13 || !boot_cpu_data.hard_math)
		return;
	math_error((void *)regs->eip);
}

/*
 * New motherboards sometimes make IRQ 13 be a PCI interrupt,
 * so allow interrupt sharing.
 */
static struct irqaction irq13 = { math_error_irq, 0, 0, "fpu", NULL, NULL };

/*
 * IRQ2 is cascade interrupt to second interrupt controller
 */

#ifndef CONFIG_VISWS
static struct irqaction irq2 = { no_action, 0, 0, "cascade", NULL, NULL};
#endif


void __init init_ISA_irqs (void)
{
	int i;

	init_8259A(0);

	for (i = 0; i < NR_IRQS; i++) {
		irq_desc[i].status = IRQ_DISABLED;
		irq_desc[i].action = 0;
		irq_desc[i].depth = 1;

		if (i < 16) {
			/*
			 * 16 old-style INTA-cycle interrupts:
			 */
			irq_desc[i].handler = &i8259A_irq_type;
		} else {
			/*
			 * 'high' PCI IRQs filled in on demand
			 */
			irq_desc[i].handler = &no_irq_type;
		}
	}
}

/**
 * void __init init_IRQ(void)
 * Linux2.4 x86 硬件中断初始化函数
 * __init：启动后该函数内存可以释放
 * 调用位置：start_kernel() -> trap_init()之后执行init_IRQ()
 * 功能：初始化ISA中断、批量填充IDT里外设硬件中断的中断门；注册SMP的IPI中断、本地APIC定时器中断；最后配置PIT8253时钟芯片
 * 注意：trap_init 填充CPU异常、int0x80；init_IRQ填充**外设硬件中断**向量
 */
void __init init_IRQ(void)
{
	int i;
#ifndef CONFIG_X86_VISWS_APIC
	// 初始化传统ISA中断（8259A主从两片PIC）
	init_ISA_irqs();
#else
	init_VISWS_APIC_irqs();
#endif
	/*
	 * Cover the whole vector space, no vector can escape
	 * us. (some of these will be overridden and become
	 * 'special' SMP interrupts)
	 */
	// 循环批量设置所有外部IRQ对应的IDT中断门描述符
	/**
	 * 其实就是如下代码：
	 * #define BUILD_IRQ(nr) \
		asmlinkage void IRQ_NAME(nr); \
		__asm__( \
		"\n"__ALIGN_STR"\n" \
		SYMBOL_NAME_STR(IRQ) #nr "_interrupt:\n\t" \
			"pushl $"#nr"-256\n\t" \
			"jmp common_interrupt");
	 */
	for (i = 0; i < NR_IRQS; i++) {
		// FIRST_EXTERNAL_VECTOR：外部硬件中断起始向量号
		int vector = FIRST_EXTERNAL_VECTOR + i;
		if (vector != SYSCALL_VECTOR) // 避开0x80系统调用向量，不要覆盖
			// set_intr_gate：中断门，进入中断处理自动关闭IF硬件中断
			set_intr_gate(vector, interrupt[i]);  //common_interrupt  --> do_IRQ (irq.c)
	}

#ifdef CONFIG_SMP
	/*
	 * IRQ0 must be given a fixed assignment and initialized,
	 * because it's used before the IO-APIC is set up.
	 */
	// SMP：IRQ0(PIT时钟中断)固定向量，IOAPIC初始化前就要能用
	set_intr_gate(FIRST_DEVICE_VECTOR, interrupt[0]);

	/*
	 * The reschedule interrupt is a CPU-to-CPU reschedule-helper
	 * IPI, driven by wakeup.
	 */
	// SMP跨CPU IPI：RESCHEDULE_VECTOR 调度中断，smp_send_reschedule发送这个IPI
	set_intr_gate(RESCHEDULE_VECTOR, reschedule_interrupt);
	// IPI：TLB失效中断，远程刷新其他CPU页表缓存
	set_intr_gate(INVALIDATE_TLB_VECTOR, invalidate_interrupt);
	// IPI：函数调用IPI，让别的CPU执行指定函数
	set_intr_gate(CALL_FUNCTION_VECTOR, call_function_interrupt);
#endif

#ifdef CONFIG_X86_LOCAL_APIC
	// 本地APIC定时器中断，SMP每个CPU本地时钟中断
	set_intr_gate(LOCAL_TIMER_VECTOR, apic_timer_interrupt);
	// APIC伪中断、APIC硬件错误中断
	set_intr_gate(SPURIOUS_APIC_VECTOR, spurious_interrupt);
	set_intr_gate(ERROR_APIC_VECTOR, error_interrupt);
#endif

	/*
	 * Set the clock to HZ Hz, we already have a valid
	 * vector now:
	 */
	// 配置PIT 8253定时器（IRQ0，时钟中断源）
	outb_p(0x34,0x43);        /* binary, mode 2, LSB/MSB, ch 0 */
	outb_p(LATCH & 0xff , 0x40);    /* LSB 写入计数低字节 */
	outb(LATCH >> 8 , 0x40);    /* MSB 写入计数高字节 */

#ifndef CONFIG_VISWS
	// 注册IRQ2，从8259A PIC级联中断
	setup_irq(2, &irq2);
#endif

	/*
	 * External FPU? Set up irq13 if so, for
	 * original braindamaged IBM FERR coupling.
	 */
	// 老式外部FPU协处理器，注册IRQ13浮点错误中断
	if (boot_cpu_data.hard_math && !cpu_has_fpu)
		setup_irq(13, &irq13);
}
