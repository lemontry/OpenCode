#ifndef _ASM_SEGMENT_H
#define _ASM_SEGMENT_H

// x86-32 Linux 2.4 GDT 段选择子
// __KERNEL_CS：内核代码段选择子
#define __KERNEL_CS 0x10
// __KERNEL_DS：内核数据段选择子
#define __KERNEL_DS 0x18

// __USER_CS：用户代码段选择子
#define __USER_CS   0x23
// __USER_DS：用户数据段选择子
#define __USER_DS   0x2B
#endif
