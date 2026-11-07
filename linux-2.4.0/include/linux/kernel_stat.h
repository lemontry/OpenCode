#ifndef _LINUX_KERNEL_STAT_H
#define _LINUX_KERNEL_STAT_H

#include <linux/config.h>
#include <asm/irq.h>
#include <linux/smp.h>
#include <linux/threads.h>

/*
 * 'kernel_stat.h' contains the definitions needed for doing
 * some kernel statistics (CPU usage, context switches ...),
 * used by rstatd/perfmeter
 */

#define DK_MAX_MAJOR 16
#define DK_MAX_DISK 16

/**
 * struct kernel_stat - 内核全局统计结构体（Linux2.4，供/proc/stat、vmstat等采集系统指标）
 * 全局实例：kstat，前面do_IRQ里用到 kstat.irqs[cpu][irq]++
 */
struct kernel_stat {
    // CPU时间统计：每个CPU的 用户态 / nice低优先级用户态 / 内核态 时间（单位：jiffies）
    unsigned int per_cpu_user[NR_CPUS],
                 per_cpu_nice[NR_CPUS],
                 per_cpu_system[NR_CPUS];

    // 磁盘IO统计：dk_drive[主设备号][磁盘号]
    unsigned int dk_drive[DK_MAX_MAJOR][DK_MAX_DISK];        // 磁盘请求计数
    unsigned int dk_drive_rio[DK_MAX_MAJOR][DK_MAX_DISK];    // 读IO请求次数
    unsigned int dk_drive_wio[DK_MAX_MAJOR][DK_MAX_DISK];    // 写IO请求次数
    unsigned int dk_drive_rblk[DK_MAX_MAJOR][DK_MAX_DISK];   // 读块数量
    unsigned int dk_drive_wblk[DK_MAX_MAJOR][DK_MAX_DISK];    // 写块数量

    // 页面交换统计
    unsigned int pgpgin, pgpgout;    // 页面调入、调出（磁盘<->内存，page in / page out）
    unsigned int pswpin, pswpout;    // swap交换分区换入、换出

#if !defined(CONFIG_ARCH_S390)
    // 中断统计：每个CPU、每个IRQ的触发次数
    // do_IRQ 中：kstat.irqs[cpu][irq]++; 就是更新这个字段
    unsigned int irqs[NR_CPUS][NR_IRQS];
#endif

    // 网络统计
    unsigned int ipackets, opackets;  // 入包、出包数量
    unsigned int ierrors, oerrors;    // 入错误、出错误包
    unsigned int collisions;          // 网络冲突（半双工以太网）

    // 进程上下文切换总次数
    unsigned int context_swtch;
};

extern struct kernel_stat kstat;

#if !defined(CONFIG_ARCH_S390)
/*
 * Number of interrupts per specific IRQ source, since bootup
 */
extern inline int kstat_irqs (int irq)
{
	int i, sum=0;

	for (i = 0 ; i < smp_num_cpus ; i++)
		sum += kstat.irqs[cpu_logical_map(i)][irq];

	return sum;
}
#endif

#endif /* _LINUX_KERNEL_STAT_H */
