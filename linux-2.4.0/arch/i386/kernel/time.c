/*
 *  linux/arch/i386/kernel/time.c
 *
 *  Copyright (C) 1991, 1992, 1995  Linus Torvalds
 *
 * This file contains the PC-specific time handling details:
 * reading the RTC at bootup, etc..
 * 1994-07-02    Alan Modra
 *	fixed set_rtc_mmss, fixed time.year for >= 2000, new mktime
 * 1995-03-26    Markus Kuhn
 *      fixed 500 ms bug at call to set_rtc_mmss, fixed DS12887
 *      precision CMOS clock update
 * 1996-05-03    Ingo Molnar
 *      fixed time warps in do_[slow|fast]_gettimeoffset()
 * 1997-09-10	Updated NTP code according to technical memorandum Jan '96
 *		"A Kernel Model for Precision Timekeeping" by Dave Mills
 * 1998-09-05    (Various)
 *	More robust do_fast_gettimeoffset() algorithm implemented
 *	(works with APM, Cyrix 6x86MX and Centaur C6),
 *	monotonic gettimeofday() with fast_get_timeoffset(),
 *	drift-proof precision TSC calibration on boot
 *	(C. Scott Ananian <cananian@alumni.princeton.edu>, Andrew D.
 *	Balsa <andrebalsa@altern.org>, Philip Gladstone <philip@raptor.com>;
 *	ported from 2.0.35 Jumbo-9 by Michael Krause <m.krause@tu-harburg.de>).
 * 1998-12-16    Andrea Arcangeli
 *	Fixed Jumbo-9 code in 2.1.131: do_gettimeofday was missing 1 jiffy
 *	because was not accounting lost_ticks.
 * 1998-12-24 Copyright (C) 1998  Andrea Arcangeli
 *	Fixed a xtime SMP race (we need the xtime_lock rw spinlock to
 *	serialize accesses to xtime/lost_ticks).
 */

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/param.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/time.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/smp.h>

#include <asm/io.h>
#include <asm/smp.h>
#include <asm/irq.h>
#include <asm/msr.h>
#include <asm/delay.h>
#include <asm/mpspec.h>
#include <asm/uaccess.h>
#include <asm/processor.h>

#include <linux/mc146818rtc.h>
#include <linux/timex.h>
#include <linux/config.h>

#include <asm/fixmap.h>
#include <asm/cobalt.h>

/*
 * for x86_do_profile()
 */
#include <linux/irq.h>


unsigned long cpu_khz;	/* Detected as we calibrate the TSC */

/* Number of usecs that the last interrupt was delayed */
static int delay_at_last_interrupt;

static unsigned long last_tsc_low; /* lsb 32 bits of Time Stamp Counter */

/* Cached *multiplier* to convert TSC counts to microseconds.
 * (see the equation below).
 * Equal to 2^32 * (1 / (clocks per usec) ).
 * Initialized in time_init.
 */
unsigned long fast_gettimeoffset_quotient;

extern rwlock_t xtime_lock;
extern unsigned long wall_jiffies;

spinlock_t rtc_lock = SPIN_LOCK_UNLOCKED;

static inline unsigned long do_fast_gettimeoffset(void)
{
	register unsigned long eax, edx;

	/* Read the Time Stamp Counter */

	rdtsc(eax,edx);

	/* .. relative to previous jiffy (32 bits is enough) */
	eax -= last_tsc_low;	/* tsc_low delta */

	/*
         * Time offset = (tsc_low delta) * fast_gettimeoffset_quotient
         *             = (tsc_low delta) * (usecs_per_clock)
         *             = (tsc_low delta) * (usecs_per_jiffy / clocks_per_jiffy)
	 *
	 * Using a mull instead of a divl saves up to 31 clock cycles
	 * in the critical path.
         */

	__asm__("mull %2"
		:"=a" (eax), "=d" (edx)
		:"rm" (fast_gettimeoffset_quotient),
		 "0" (eax));

	/* our adjusted time offset in microseconds */
	return delay_at_last_interrupt + edx;
}

#define TICK_SIZE tick

spinlock_t i8253_lock = SPIN_LOCK_UNLOCKED;

extern spinlock_t i8259A_lock;

#ifndef CONFIG_X86_TSC

/* This function must be called with interrupts disabled 
 * It was inspired by Steve McCanne's microtime-i386 for BSD.  -- jrs
 * 
 * However, the pc-audio speaker driver changes the divisor so that
 * it gets interrupted rather more often - it loads 64 into the
 * counter rather than 11932! This has an adverse impact on
 * do_gettimeoffset() -- it stops working! What is also not
 * good is that the interval that our timer function gets called
 * is no longer 10.0002 ms, but 9.9767 ms. To get around this
 * would require using a different timing source. Maybe someone
 * could use the RTC - I know that this can interrupt at frequencies
 * ranging from 8192Hz to 2Hz. If I had the energy, I'd somehow fix
 * it so that at startup, the timer code in sched.c would select
 * using either the RTC or the 8253 timer. The decision would be
 * based on whether there was any other device around that needed
 * to trample on the 8253. I'd set up the RTC to interrupt at 1024 Hz,
 * and then do some jiggery to have a version of do_timer that 
 * advanced the clock by 1/1024 s. Every time that reached over 1/100
 * of a second, then do all the old code. If the time was kept correct
 * then do_gettimeoffset could just return 0 - there is no low order
 * divider that can be accessed.
 *
 * Ideally, you would be able to use the RTC for the speaker driver,
 * but it appears that the speaker driver really needs interrupt more
 * often than every 120 us or so.
 *
 * Anyway, this needs more thought....		pjsg (1993-08-28)
 * 
 * If you are really that interested, you should be reading
 * comp.protocols.time.ntp!
 */

static unsigned long do_slow_gettimeoffset(void)
{
	int count;

	static int count_p = LATCH;    /* for the first call after boot */
	static unsigned long jiffies_p = 0;

	/*
	 * cache volatile jiffies temporarily; we have IRQs turned off. 
	 */
	unsigned long jiffies_t;

	/* gets recalled with irq locally disabled */
	spin_lock(&i8253_lock);
	/* timer count may underflow right here */
	outb_p(0x00, 0x43);	/* latch the count ASAP */

	count = inb_p(0x40);	/* read the latched count */

	/*
	 * We do this guaranteed double memory access instead of a _p 
	 * postfix in the previous port access. Wheee, hackady hack
	 */
 	jiffies_t = jiffies;

	count |= inb_p(0x40) << 8;
	spin_unlock(&i8253_lock);

	/*
	 * avoiding timer inconsistencies (they are rare, but they happen)...
	 * there are two kinds of problems that must be avoided here:
	 *  1. the timer counter underflows
	 *  2. hardware problem with the timer, not giving us continuous time,
	 *     the counter does small "jumps" upwards on some Pentium systems,
	 *     (see c't 95/10 page 335 for Neptun bug.)
	 */

/* you can safely undefine this if you don't have the Neptune chipset */

#define BUGGY_NEPTUN_TIMER

	if( jiffies_t == jiffies_p ) {
		if( count > count_p ) {
			/* the nutcase */

			int i;

			spin_lock(&i8259A_lock);
			/*
			 * This is tricky when I/O APICs are used;
			 * see do_timer_interrupt().
			 */
			i = inb(0x20);
			spin_unlock(&i8259A_lock);

			/* assumption about timer being IRQ0 */
			if (i & 0x01) {
				/*
				 * We cannot detect lost timer interrupts ... 
				 * well, that's why we call them lost, don't we? :)
				 * [hmm, on the Pentium and Alpha we can ... sort of]
				 */
				count -= LATCH;
			} else {
#ifdef BUGGY_NEPTUN_TIMER
				/*
				 * for the Neptun bug we know that the 'latch'
				 * command doesnt latch the high and low value
				 * of the counter atomically. Thus we have to 
				 * substract 256 from the counter 
				 * ... funny, isnt it? :)
				 */

				count -= 256;
#else
				printk("do_slow_gettimeoffset(): hardware timer problem?\n");
#endif
			}
		}
	} else
		jiffies_p = jiffies_t;

	count_p = count;

	count = ((LATCH-1) - count) * TICK_SIZE;
	count = (count + LATCH/2) / LATCH;

	return count;
}

static unsigned long (*do_gettimeoffset)(void) = do_slow_gettimeoffset;

#else

#define do_gettimeoffset()	do_fast_gettimeoffset()

#endif

/*
 * This version of gettimeofday has microsecond resolution
 * and better than microsecond precision on fast x86 machines with TSC.
 */
void do_gettimeofday(struct timeval *tv)
{
	unsigned long flags;
	unsigned long usec, sec;

	read_lock_irqsave(&xtime_lock, flags);
	usec = do_gettimeoffset();
	{
		unsigned long lost = jiffies - wall_jiffies;
		if (lost)
			usec += lost * (1000000 / HZ);
	}
	sec = xtime.tv_sec;
	usec += xtime.tv_usec;
	read_unlock_irqrestore(&xtime_lock, flags);

	while (usec >= 1000000) {
		usec -= 1000000;
		sec++;
	}

	tv->tv_sec = sec;
	tv->tv_usec = usec;
}

void do_settimeofday(struct timeval *tv)
{
	write_lock_irq(&xtime_lock);
	/*
	 * This is revolting. We need to set "xtime" correctly. However, the
	 * value in this location is the value at the most recent update of
	 * wall time.  Discover what correction gettimeofday() would have
	 * made, and then undo it!
	 */
	tv->tv_usec -= do_gettimeoffset();
	tv->tv_usec -= (jiffies - wall_jiffies) * (1000000 / HZ);

	while (tv->tv_usec < 0) {
		tv->tv_usec += 1000000;
		tv->tv_sec--;
	}

	xtime = *tv;
	time_adjust = 0;		/* stop active adjtime() */
	time_status |= STA_UNSYNC;
	time_maxerror = NTP_PHASE_LIMIT;
	time_esterror = NTP_PHASE_LIMIT;
	write_unlock_irq(&xtime_lock);
}

/*
 * In order to set the CMOS clock precisely, set_rtc_mmss has to be
 * called 500 ms after the second nowtime has started, because when
 * nowtime is written into the registers of the CMOS clock, it will
 * jump to the next second precisely 500 ms later. Check the Motorola
 * MC146818A or Dallas DS12887 data sheet for details.
 *
 * BUG: This routine does not handle hour overflow properly; it just
 *      sets the minutes. Usually you'll only notice that after reboot!
 */
static int set_rtc_mmss(unsigned long nowtime)
{
	int retval = 0;
	int real_seconds, real_minutes, cmos_minutes;
	unsigned char save_control, save_freq_select;

	/* gets recalled with irq locally disabled */
	spin_lock(&rtc_lock);
	save_control = CMOS_READ(RTC_CONTROL); /* tell the clock it's being set */
	CMOS_WRITE((save_control|RTC_SET), RTC_CONTROL);

	save_freq_select = CMOS_READ(RTC_FREQ_SELECT); /* stop and reset prescaler */
	CMOS_WRITE((save_freq_select|RTC_DIV_RESET2), RTC_FREQ_SELECT);

	cmos_minutes = CMOS_READ(RTC_MINUTES);
	if (!(save_control & RTC_DM_BINARY) || RTC_ALWAYS_BCD)
		BCD_TO_BIN(cmos_minutes);

	/*
	 * since we're only adjusting minutes and seconds,
	 * don't interfere with hour overflow. This avoids
	 * messing with unknown time zones but requires your
	 * RTC not to be off by more than 15 minutes
	 */
	real_seconds = nowtime % 60;
	real_minutes = nowtime / 60;
	if (((abs(real_minutes - cmos_minutes) + 15)/30) & 1)
		real_minutes += 30;		/* correct for half hour time zone */
	real_minutes %= 60;

	if (abs(real_minutes - cmos_minutes) < 30) {
		if (!(save_control & RTC_DM_BINARY) || RTC_ALWAYS_BCD) {
			BIN_TO_BCD(real_seconds);
			BIN_TO_BCD(real_minutes);
		}
		CMOS_WRITE(real_seconds,RTC_SECONDS);
		CMOS_WRITE(real_minutes,RTC_MINUTES);
	} else {
		printk(KERN_WARNING
		       "set_rtc_mmss: can't update from %d to %d\n",
		       cmos_minutes, real_minutes);
		retval = -1;
	}

	/* The following flags have to be released exactly in this order,
	 * otherwise the DS12887 (popular MC146818A clone with integrated
	 * battery and quartz) will not reset the oscillator and will not
	 * update precisely 500 ms later. You won't find this mentioned in
	 * the Dallas Semiconductor data sheets, but who believes data
	 * sheets anyway ...                           -- Markus Kuhn
	 */
	CMOS_WRITE(save_control, RTC_CONTROL);
	CMOS_WRITE(save_freq_select, RTC_FREQ_SELECT);
	spin_unlock(&rtc_lock);

	return retval;
}

/* last time the cmos clock got updated */
static long last_rtc_update;

int timer_ack;

/*
 * timer_interrupt() needs to keep up the real-time clock,
 * as well as call the "do_timer()" routine every clocktick
 */
/**
 * do_timer_interrupt - 定时器中断核心处理函数
 * @irq: 中断号
 * @dev_id: 设备私有数据
 * @regs: 中断现场寄存器
 *
 * 功能：IOAPIC/VISWS/MCA等平台做硬件ack；调用do_timer完成节拍更新；采样profile性能统计；
 *       满足条件时把内核xtime同步写回CMOS RTC
 * 约束：硬中断上下文，由timer_interrupt上半部调用，持有xtime_lock
 * 上下文：硬中断上下文
 */
static inline void do_timer_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
#ifdef CONFIG_X86_IO_APIC
    if (timer_ack) {
        /*
         * Subtle, when I/O APICs are used we have to ack timer IRQ
         * manually to reset the IRR bit for do_slow_gettimeoffset().
         * This will also deassert NMI lines for the watchdog if run
         * on an 82489DX-based system.
         */
        // 获取8259A PIC锁，保护PIC端口操作
        spin_lock(&i8259A_lock);
        // 向主PIC 0x20端口发送0x0c，特殊的EOI应答
        outb(0x0c, 0x20);
        // 读OCW3，完成ack流程
        inb(0x20);
        // 释放PIC锁
        spin_unlock(&i8259A_lock);
    }
#endif
#ifdef CONFIG_VISWS
    /* Clear the interrupt */
    // Cobalt平台，清除定时器中断状态位
    co_cpu_write(CO_CPU_STAT,co_cpu_read(CO_CPU_STAT) & ~CO_STAT_TIMEINTR);
#endif
    // 最核心：jiffies++、更新xtime、触发timer_bh下半部处理内核定时器
    do_timer(regs);
/*
 * In the SMP case we use the local APIC timer interrupt to do the
 * profiling, except when we simulate SMP mode on a uniprocessor
 * system, in that case we have to call the local interrupt handler.
 */
#ifndef CONFIG_X86_LOCAL_APIC
    // 没有本地APIC：内核态触发性能采样profile，记录PC（eip）
    if (!user_mode(regs))
        x86_do_profile(regs->eip);
#else
    // 有APIC但未找到SMP配置，调用本地定时器profile函数
    if (!smp_found_config)
        smp_local_timer_interrupt(regs);
#endif
    /*
     * If we have an externally synchronized Linux clock, then update
     * CMOS clock accordingly every ~11 minutes. Set_rtc_mmss() has to be
     * called as close as possible to 500 ms before the new second starts.
     */
    // 时钟已同步、距离上次写RTC超过660秒、时间落在半秒附近，同步xtime到CMOS RTC
    if ((time_status & STA_UNSYNC) == 0 &&
        xtime.tv_sec > last_rtc_update + 660 &&
        xtime.tv_usec >= 500000 - ((unsigned) tick) / 2 &&
        xtime.tv_usec <= 500000 + ((unsigned) tick) / 2) {
        // 写入RTC成功，更新上次写入时间
        if (set_rtc_mmss(xtime.tv_sec) == 0)
            last_rtc_update = xtime.tv_sec;
        else
            // 写入失败，60秒后重试
            last_rtc_update = xtime.tv_sec - 600; /* do it again in 60 s */
    }
            
#ifdef CONFIG_MCA
    if( MCA_bus ) {
        /* The PS/2 uses level-triggered interrupts.  You can't
        turn them off, nor would you want to (any attempt to
        enable edge-triggered interrupts usually gets intercepted by a
        special hardware circuit).  Hence we have to acknowledge
        the timer interrupt.  Through some incredibly stupid
        design idea, the reset for IRQ 0 is done by setting the
        high bit of the PPI port B (0x61).  Note that some PS/2s,
        notably the 55SX, work fine if this is removed.  */
        // 读取0x61端口当前值（PS/2 PPI port B）
        irq = inb_p( 0x61 );
        // 置最高bit，复位IRQ0电平中断
        outb_p( irq|0x80, 0x61 );
    }
#endif
}

static int use_tsc;

/*
 * This is the same as the above, except we _also_ save the current
 * Time Stamp Counter value at the time of the timer interrupt, so that
 * we later on can estimate the time of day more exactly.
 */
/**
 * timer_interrupt - IRQ0(PIT定时器)中断上半部处理函数
 * @irq: 中断号，固定为0
 * @dev_id: 设备私有数据，此处为NULL
 * @regs: 中断发生时的寄存器现场
 *
 * 功能：系统节拍定时器中断上半部；带SA_INTERRUPT，运行时关闭本地BH；
 *       如果开启TSC，读取TSC+锁存PIT计数值，校正时间偏移；
 *       调用do_timer_interrupt完成核心节拍更新、jiffies递增、软定时器唤起
 * 约束：硬中断上下文，不可睡眠；持有xtime_lock保护xtime全局时间
 * 上下文：硬中断上下文，SA_INTERRUPT，BH被屏蔽
 */
static void timer_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
    int count;
    /*
     * Here we are in the timer irq handler. We just have irqs locally
     * disabled but we don't know if the timer_bh is running on the other
     * CPU. We need to avoid to SMP race with it. NOTE: we don' t need
     * the irq version of write_lock because as just said we have irq
     * locally disabled. -arca
     */
    // 获取xtime写锁，保护xtime全局墙上时间，防止和其他CPU上timer_bh产生SMP竞争
    write_lock(&xtime_lock);
    if (use_tsc)
    {
        /*
         * It is important that these two operations happen almost at
         * the same time. We do the RDTSC stuff first, since it's
         * faster. To avoid any inconsistencies, we need interrupts
         * disabled locally.
         */
        /*
         * Interrupts are just disabled locally since the timer irq
         * has the SA_INTERRUPT flag set. -arca
         */

        // 读取Pentium TSC时间戳计数器低32位，保存到last_tsc_low
        rdtscl(last_tsc_low);
        // 获取i8253 PIT端口访问自旋锁，保护对0x40/0x43硬件端口的并发访问
        spin_lock(&i8253_lock);
        // 向PIT控制端口0x43写入0x00，锁存当前通道0计数器值
        outb_p(0x00, 0x43);
        // 先读低字节锁存值
        count = inb_p(0x40);
        // 再读高字节，拼接成16位计数值
        count |= inb(0x40) << 8;
        // 释放PIT端口锁
        spin_unlock(&i8253_lock);
        // 换算PIT已经走过的tick时间
        count = ((LATCH-1) - count) * TICK_SIZE;
        // 计算本次中断实际发生相对节拍起点的偏移，用于gettimeofday精细校正
        delay_at_last_interrupt = (count + LATCH/2) / LATCH;
    }

    // 定时器中断核心逻辑：jiffies++、更新xtime、唤起timer_bh（定时器下半部）
    do_timer_interrupt(irq, NULL, regs);
    // 释放xtime写锁
    write_unlock(&xtime_lock);
}

/* not static: needed by APM */
unsigned long get_cmos_time(void)
{
	unsigned int year, mon, day, hour, min, sec;
	int i;

	/* The Linux interpretation of the CMOS clock register contents:
	 * When the Update-In-Progress (UIP) flag goes from 1 to 0, the
	 * RTC registers show the second which has precisely just started.
	 * Let's hope other operating systems interpret the RTC the same way.
	 */
	/* read RTC exactly on falling edge of update flag */
	for (i = 0 ; i < 1000000 ; i++)	/* may take up to 1 second... */
		if (CMOS_READ(RTC_FREQ_SELECT) & RTC_UIP)
			break;
	for (i = 0 ; i < 1000000 ; i++)	/* must try at least 2.228 ms */
		if (!(CMOS_READ(RTC_FREQ_SELECT) & RTC_UIP))
			break;
	do { /* Isn't this overkill ? UIP above should guarantee consistency */
		sec = CMOS_READ(RTC_SECONDS);
		min = CMOS_READ(RTC_MINUTES);
		hour = CMOS_READ(RTC_HOURS);
		day = CMOS_READ(RTC_DAY_OF_MONTH);
		mon = CMOS_READ(RTC_MONTH);
		year = CMOS_READ(RTC_YEAR);
	} while (sec != CMOS_READ(RTC_SECONDS));
	if (!(CMOS_READ(RTC_CONTROL) & RTC_DM_BINARY) || RTC_ALWAYS_BCD)
	  {
	    BCD_TO_BIN(sec);
	    BCD_TO_BIN(min);
	    BCD_TO_BIN(hour);
	    BCD_TO_BIN(day);
	    BCD_TO_BIN(mon);
	    BCD_TO_BIN(year);
	  }
	if ((year += 1900) < 1970)
		year += 100;
	return mktime(year, mon, day, hour, min, sec);
}

/**
 * irq0 - PIT定时器中断（IRQ0）对应的irqaction实例
 *
 * 功能：描述系统节拍定时器中断的处理信息，注册到IRQ0中断描述符
 * 约束：SA_INTERRUPT标志，代表该中断上半部执行时关闭本地软中断(BH)
 * 上下文：time_init中通过setup_irq完成注册，硬件定时器中断触发时执行timer_interrupt
 */
static struct irqaction irq0  = { timer_interrupt, SA_INTERRUPT, 0, "timer", NULL, NULL};

/* ------ Calibrate the TSC ------- 
 * Return 2^32 * (1 / (TSC clocks per usec)) for do_fast_gettimeoffset().
 * Too much 64-bit arithmetic here to do this cleanly in C, and for
 * accuracy's sake we want to keep the overhead on the CTC speaker (channel 2)
 * output busy loop as low as possible. We avoid reading the CTC registers
 * directly because of the awkward 8-bit access mechanism of the 82C54
 * device.
 */

#define CALIBRATE_LATCH	(5 * LATCH)
#define CALIBRATE_TIME	(5 * 1000020/HZ)

static unsigned long __init calibrate_tsc(void)
{
       /* Set the Gate high, disable speaker */
	outb((inb(0x61) & ~0x02) | 0x01, 0x61);

	/*
	 * Now let's take care of CTC channel 2
	 *
	 * Set the Gate high, program CTC channel 2 for mode 0,
	 * (interrupt on terminal count mode), binary count,
	 * load 5 * LATCH count, (LSB and MSB) to begin countdown.
	 */
	outb(0xb0, 0x43);			/* binary, mode 0, LSB/MSB, Ch 2 */
	outb(CALIBRATE_LATCH & 0xff, 0x42);	/* LSB of count */
	outb(CALIBRATE_LATCH >> 8, 0x42);	/* MSB of count */

	{
		unsigned long startlow, starthigh;
		unsigned long endlow, endhigh;
		unsigned long count;

		rdtsc(startlow,starthigh);
		count = 0;
		do {
			count++;
		} while ((inb(0x61) & 0x20) == 0);
		rdtsc(endlow,endhigh);

		last_tsc_low = endlow;

		/* Error: ECTCNEVERSET */
		if (count <= 1)
			goto bad_ctc;

		/* 64-bit subtract - gcc just messes up with long longs */
		__asm__("subl %2,%0\n\t"
			"sbbl %3,%1"
			:"=a" (endlow), "=d" (endhigh)
			:"g" (startlow), "g" (starthigh),
			 "0" (endlow), "1" (endhigh));

		/* Error: ECPUTOOFAST */
		if (endhigh)
			goto bad_ctc;

		/* Error: ECPUTOOSLOW */
		if (endlow <= CALIBRATE_TIME)
			goto bad_ctc;

		__asm__("divl %2"
			:"=a" (endlow), "=d" (endhigh)
			:"r" (endlow), "0" (0), "1" (CALIBRATE_TIME));

		return endlow;
	}

	/*
	 * The CTC wasn't reliable: we got a hit on the very first read,
	 * or the CPU was so fast/slow that the quotient wouldn't fit in
	 * 32 bits..
	 */
bad_ctc:
	return 0;
}

/**
 * time_init - 内核时间子系统初始化函数，start_kernel 阶段调用
 *
 * 功能：读取CMOS RTC获取初始墙上时间，检测并标定TSC时间戳计数器，注册系统定时器中断IRQ0
 * 约束：内核启动早期调用，中断此时尚未正式调度；属于架构相关初始化
 * 上下文：内核启动上下文，start_kernel中执行
 */
void __init time_init(void)
{
    extern int x86_udelay_tsc;

    // 从CMOS RTC硬件读取当前墙上时间，赋值给xtime（内核全局墙上时间）
    xtime.tv_sec = get_cmos_time();
    // 微秒初始化为0
    xtime.tv_usec = 0;
/*
 * If we have APM enabled or the CPU clock speed is variable
 * (CPU stops clock on HLT or slows clock to save power)
 * then the TSC timestamps may diverge by up to 1 jiffy from
 * 'real time' but nothing will break.
 * The most frequent case is that the CPU is "woken" from a halt
 * state by the timer interrupt itself, so we get 0 error. In the
 * rare cases where a driver would "wake" the CPU and request a
 * timestamp, the maximum error is < 1 jiffy. But timestamps are
 * still perfectly ordered.
 * Note that the TSC counter will be reset if APM suspends
 * to disk; this won't break the kernel, though, 'cuz we're
 * smart.  See arch/i386/kernel/apm.c.
 */
    /*
     *  Firstly we have to do a CPU check for chips with
     *  a potentially buggy TSC. At this point we haven't run
     *  the ident/bugs checks so we must run this hook as it
     *  may turn off the TSC flag.
     *
     *  NOTE: this doesnt yet handle SMP 486 machines where only
     *  some CPU's have a TSC. Thats never worked and nobody has
     *  moaned if you have the only one in the world - you fix it!
     */

    // 检测存在bug的TSC，有问题则清除TSC可用标记cpu_has_tsc
    dodgy_tsc();

    // 如果CPU支持TSC时间戳计数器
    if (cpu_has_tsc) {
        // 标定TSC，计算每个微秒对应的TSC计数
        unsigned long tsc_quotient = calibrate_tsc();
        if (tsc_quotient) {
            // 保存TSC标定系数，用于快速计算时间偏移
            fast_gettimeoffset_quotient = tsc_quotient;
            // 标记开启TSC快速时间获取
            use_tsc = 1;
            /*
             *  We could be more selective here I suspect
             *  and just enable this for the next intel chips ?
             */
            // 标记udelay延时函数使用TSC实现
            x86_udelay_tsc = 1;
#ifndef do_gettimeoffset
            // 把获取时间偏移的函数指针替换为TSC快速版本
            do_gettimeoffset = do_fast_gettimeoffset;
#endif
            // 配置快速gettimeofday接口
            do_get_fast_time = do_gettimeofday;
            /* report CPU clock rate in Hz.
             * The formula is (10^6 * 2^32) / (2^32 * 1 / (clocks/us)) =
             * clock/second. Our precision is about 100 ppm.
             */
            {   unsigned long eax=0, edx=1000;
                // 汇编除法，根据tsc_quotient算出cpu_khz（CPU千赫兹频率）
                __asm__("divl %2"
                        :"=a" (cpu_khz), "=d" (edx)
                        :"r" (tsc_quotient),
                        "0" (eax), "1" (edx));
                // 打印检测到的CPU主频
                printk("Detected %lu.%03lu MHz processor.\n", cpu_khz / 1000, cpu_khz % 1000);
            }
        }
    }
#ifdef CONFIG_VISWS
    printk("Starting Cobalt Timer system clock\n");
    // 设置定时器倒计时初值
    co_cpu_write(CO_CPU_TIMEVAL, CO_TIME_HZ/HZ);
    // 启动Cobalt硬件定时器
    co_cpu_write(CO_CPU_CTRL, co_cpu_read(CO_CPU_CTRL) | CO_CTRL_TIMERUN);
    // 解除定时器中断屏蔽，允许定时器中断上报
    co_cpu_write(CO_CPU_CTRL, co_cpu_read(CO_CPU_CTRL) & ~CO_CTRL_TIMEMASK);
    // 注册Cobalt平台定时器中断
    setup_irq(CO_IRQ_TIMER, &irq0);
#else
    // 普通x86平台：注册PIT定时器IRQ0，绑定irq0中断描述块
    setup_irq(0, &irq0);
#endif
}