/*
 * structures and definitions for the int 15, ax=e820 memory map
 * scheme.
 *
 * In a nutshell, arch/i386/boot/setup.S populates a scratch table
 * in the empty_zero_block that contains a list of usable address/size
 * duples.   In arch/i386/kernel/setup.c, this information is
 * transferred into the e820map, and in arch/i386/mm/init.c, that
 * new information is used to mark pages reserved or not.
 *
 */
#ifndef __E820_HEADER
#define __E820_HEADER

// 零页内偏移：e820entry数组起始
#define E820MAP	0x2d0		/* our map */
#define E820MAX	32		/* number of entries in E820MAP */

// 零页内偏移：条目数量（char）
#define E820NR	0x1e8		/* # entries in E820MAP */

#define E820_RAM	1
#define E820_RESERVED	2
#define E820_ACPI	3 /* usable as RAM once ACPI tables have been read */
#define E820_NVS	4

#define HIGH_MEMORY	(1024*1024)

#ifndef __ASSEMBLY__

/**
 * struct e820map - E820内存映射表容器，保存BIOS返回的全部内存段描述
 * 来源：int 0x15 eax=0xE820 中断，由setup.S先存入零页，再由copy_e820_map拷贝到此
 * 用途：内核启动阶段识别物理内存分布，区分可用RAM、保留内存、ACPI区域等
 */
struct e820map {
    int nr_map;
    // nr_map：有效e820entry条目数量，代表map数组里面有多少条有效的内存段
    struct e820entry {
        unsigned long long addr;    /* start of memory segment */
        // addr：本段内存的起始物理地址（64位，支持大于4G地址）
        unsigned long long size;    /* size of memory segment */
        // size：本段内存的长度，单位字节
        unsigned long type;         /* type of memory segment */
        // type：内存段类型，标记这块内存能不能被内核使用
        // 1 = E820_RAM      可用主内存，内核可以分配使用
        // 2 = E820_RESERVED 保留内存（BIOS、MMIO、显存、ROM），禁止使用
        // 3 = E820_ACPI     ACPI可回收内存，ACPI初始化完成后可以回收
        // 4 = E820_NVS      ACPI休眠专用，内核不能占用
        // 5 = E820_UNUSABLE 损坏内存，不可使用
    } map[E820MAX];
    // map[]：E820条目数组，每一条描述一段连续物理地址区间
    // E820MAX：内核宏，限制最多支持多少条内存段（2.4一般是32）
};

extern struct e820map e820;
#endif/*!__ASSEMBLY__*/

#endif/*__E820_HEADER*/
