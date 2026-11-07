/*
 * linux/kernel/ldt.c
 *
 * Copyright (C) 1992 Krishna Balasubramanian and Linus Torvalds
 * Copyright (C) 1999 Ingo Molnar <mingo@redhat.com>
 */

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/smp_lock.h>
#include <linux/vmalloc.h>

#include <asm/uaccess.h>
#include <asm/system.h>
#include <asm/ldt.h>
#include <asm/desc.h>

/*
 * read_ldt() is not really atomic - this is not a problem since
 * synchronization of reads and writes done to the LDT has to be
 * assured by user-space anyway. Writes are atomic, to protect
 * the security checks done on new descriptors.
 */
/**
 * read_ldt - 将当前进程LDT内容拷贝到用户缓冲区
 * @ptr: 用户态目标缓冲区地址
 * @bytecount: 用户缓冲区最大字节数
 * 返回值：成功返回拷贝字节数；失败返回-EFAULT
 */
static int read_ldt(void * ptr, unsigned long bytecount)
{
    int err;
    unsigned long size;
    // 获取当前进程mm_struct
    struct mm_struct * mm = current->mm;

    err = 0;
    // 该进程没有分配LDT，直接返回0
    if (!mm->context.segments)
        goto out;

    // LDT总大小：LDT_ENTRIES个表项 × 每个LDT描述符大小
    size = LDT_ENTRIES*LDT_ENTRY_SIZE;
    // 不能超过用户传入缓冲区长度，取较小值
    if (size > bytecount)
        size = bytecount;

    // 默认返回拷贝字节数
    err = size;
    // 内核LDT缓冲区拷贝到用户空间
    if (copy_to_user(ptr, mm->context.segments, size))
        err = -EFAULT;
out:
    return err;
}

/**
 * write_ldt - 写入/修改当前进程LDT中的一个段描述符
 * @ptr: 用户态传入的 modify_ldt_ldt_s 结构体指针
 * @bytecount: 用户传入数据长度
 * @oldmode: 1=func=1旧式模式；0=func=0x11新模式
 * 返回值：成功返回0；失败返回负错误码
 */
static int write_ldt(void * ptr, unsigned long bytecount, int oldmode)
{
    struct mm_struct * mm = current->mm;
    __u32 entry_1, entry_2, *lp;
    int error;
    struct modify_ldt_ldt_s ldt_info;

    // 长度必须等于modify_ldt_ldt_s结构体大小
    error = -EINVAL;
    if (bytecount != sizeof(ldt_info))
        goto out;
    // 从用户空间拷贝ldt_info
    error = -EFAULT;    
    if (copy_from_user(&ldt_info, ptr, sizeof(ldt_info)))
        goto out;

    error = -EINVAL;
    // LDT表项编号不能超过最大LDT条目数
    if (ldt_info.entry_number >= LDT_ENTRIES)
        goto out;
    // contents == 3 特殊类型，旧式模式不支持；且要求段标记为不存在
    if (ldt_info.contents == 3) {
        if (oldmode)
            goto out;
        if (ldt_info.seg_not_present == 0)
            goto out;
    }

    /*
     * Horrible dependencies! Try to get rid of this. This is wrong,
     * as it only reloads the ldt for the first process with this
     * mm. The implications are that you should really make sure that
     * you have a ldt before you do the first clone(), otherwise
     * you get strange behaviour (the kernel is safe, it's just user
     * space strangeness).
     *
     * we have two choices: either we preallocate the LDT descriptor
     * and can do a shared modify_ldt(), or we postallocate it and do
     * an smp message pass to update it. Currently we are a bit
     * un-nice to user-space and reload the LDT only on the next
     * schedule. (only an issue on SMP)
     *
     * the GDT index of the LDT is allocated dynamically, and is
     * limited by MAX_LDT_DESCRIPTORS.
     */
    down(&mm->mmap_sem);
    // 如果当前mm还没有分配LDT缓冲区
    if (!mm->context.segments) {
        error = -ENOMEM;
        // vmalloc分配一整张LDT表
        mm->context.segments = vmalloc(LDT_ENTRIES*LDT_ENTRY_SIZE);
        if (!mm->context.segments)
            goto out_unlock;
        memset(mm->context.segments, 0, LDT_ENTRIES*LDT_ENTRY_SIZE);
        
        // 如果mm已经被多个进程共享（clone），打印警告
        if (atomic_read(&mm->mm_users) > 1)
            printk(KERN_WARNING "LDT allocated for cloned task!\n");
        /*
         * Possibly do an SMP cross-call to other CPUs to reload
         * their LDTs?
         */
        load_LDT(mm);
    }

    // 计算目标LDT表项在内核segments缓冲区的地址
    // 每个LDT表项8字节：entry_number << 3
    lp = (__u32 *) ((ldt_info.entry_number << 3) + (char *) mm->context.segments);

    // 支持用户清空LDT表项（全部置0）
    if (ldt_info.base_addr == 0 && ldt_info.limit == 0) {
        if (oldmode ||
            (ldt_info.contents == 0     &&
             ldt_info.read_exec_only == 1   &&
             ldt_info.seg_32bit == 0       &&
             ldt_info.limit_in_pages == 0   &&
             ldt_info.seg_not_present == 1  &&
             ldt_info.useable == 0 )) {
            entry_1 = 0;
            entry_2 = 0;
            goto install;
        }
    }

    // 拼接LDT描述符低32bit entry_1
    // base_addr[15:0] 放在高16位，limit[15:0]放在低16位
    entry_1 = ((ldt_info.base_addr & 0x0000ffff) << 16) |
        (ldt_info.limit & 0x0ffff);
    // 拼接LDT描述符高32bit entry_2
    entry_2 = (ldt_info.base_addr & 0xff000000) |
        ((ldt_info.base_addr & 0x00ff0000) >> 16) |
        (ldt_info.limit & 0xf0000) |
        ((ldt_info.read_exec_only ^ 1) << 9) |
        (ldt_info.contents << 10) |
        ((ldt_info.seg_not_present ^ 1) << 15) |
        (ldt_info.seg_32bit << 22) |
        (ldt_info.limit_in_pages << 23) |
        0x7000;
    // 新模式才支持useable标记位
    if (!oldmode)
        entry_2 |= (ldt_info.useable << 20);

    /* Install the new entry ...  */
install:
    // 写入LDT两项（8字节描述符）
    *lp = entry_1;
    *(lp+1) = entry_2;
    error = 0;

out_unlock:
    up(&mm->mmap_sem);
out:
    return error;
}


/**
 * sys_modify_ldt - 系统调用，读写进程的LDT（局部描述符表）
 * @func: 操作类型
 * @ptr: 用户态缓冲区指针
 * @bytecount: 读写字节长度
 * 返回值：成功返回读写长度；失败返回负数错误码
 */
asmlinkage int sys_modify_ldt(int func, void *ptr, unsigned long bytecount)
{
    int ret = -ENOSYS;

    switch (func) {
    // func=0：读取当前进程LDT到用户缓冲区
    case 0:
        ret = read_ldt(ptr, bytecount);
        break;
    // func=1：写入LDT，标志位1（旧模式，段描述符）
    case 1:
        ret = write_ldt(ptr, bytecount, 1);
        break;
    // func=0x11：写入LDT，标志位0（新的LDT段，现代方式）
    case 0x11:
        ret = write_ldt(ptr, bytecount, 0);
        break;
    }
    return ret;
}
