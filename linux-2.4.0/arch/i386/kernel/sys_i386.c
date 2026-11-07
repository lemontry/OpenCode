/*
 * linux/arch/i386/kernel/sys_i386.c
 *
 * This file contains various random system calls that
 * have a non-standard calling sequence on the Linux/i386
 * platform.
 */

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/smp_lock.h>
#include <linux/sem.h>
#include <linux/msg.h>
#include <linux/shm.h>
#include <linux/stat.h>
#include <linux/mman.h>
#include <linux/file.h>
#include <linux/utsname.h>

#include <asm/uaccess.h>
#include <asm/ipc.h>

/*
 * sys_pipe() is the normal C calling standard for creating
 * a pipe. It's not the way Unix traditionally does this, though.
 */
asmlinkage int sys_pipe(unsigned long * fildes)
{
	int fd[2];
	int error;

	error = do_pipe(fd);
	if (!error) {
		if (copy_to_user(fildes, fd, 2*sizeof(int)))
			error = -EFAULT;
	}
	return error;
}

/* common code for old and new mmaps */
static inline long do_mmap2(
	unsigned long addr, unsigned long len,
	unsigned long prot, unsigned long flags,
	unsigned long fd, unsigned long pgoff)
{
	int error = -EBADF;
	struct file * file = NULL;

	flags &= ~(MAP_EXECUTABLE | MAP_DENYWRITE);
	if (!(flags & MAP_ANONYMOUS)) {
		file = fget(fd);
		if (!file)
			goto out;
	}

	down(&current->mm->mmap_sem);
	error = do_mmap_pgoff(file, addr, len, prot, flags, pgoff);
	up(&current->mm->mmap_sem);

	if (file)
		fput(file);
out:
	return error;
}

asmlinkage long sys_mmap2(unsigned long addr, unsigned long len,
	unsigned long prot, unsigned long flags,
	unsigned long fd, unsigned long pgoff)
{
	return do_mmap2(addr, len, prot, flags, fd, pgoff);
}

/*
 * Perform the select(nd, in, out, ex, tv) and mmap() system
 * calls. Linux/i386 didn't use to be able to handle more than
 * 4 system call parameters, so these system calls used a memory
 * block for parameter passing..
 */

struct mmap_arg_struct {
	unsigned long addr;
	unsigned long len;
	unsigned long prot;
	unsigned long flags;
	unsigned long fd;
	unsigned long offset;
};

asmlinkage int old_mmap(struct mmap_arg_struct *arg)
{
	struct mmap_arg_struct a;
	int err = -EFAULT;

	if (copy_from_user(&a, arg, sizeof(a)))
		goto out;

	err = -EINVAL;
	if (a.offset & ~PAGE_MASK)
		goto out;

	err = do_mmap2(a.addr, a.len, a.prot, a.flags, a.fd, a.offset >> PAGE_SHIFT);
out:
	return err;
}


extern asmlinkage int sys_select(int, fd_set *, fd_set *, fd_set *, struct timeval *);

struct sel_arg_struct {
	unsigned long n;
	fd_set *inp, *outp, *exp;
	struct timeval *tvp;
};

asmlinkage int old_select(struct sel_arg_struct *arg)
{
	struct sel_arg_struct a;

	if (copy_from_user(&a, arg, sizeof(a)))
		return -EFAULT;
	/* sys_select() does the appropriate kernel locking */
	return sys_select(a.n, a.inp, a.outp, a.exp, a.tvp);
}

/*
 * sys_ipc() is the de-multiplexer for the SysV IPC calls..
 *
 * This is really horribly ugly.
 */
/**
 * sys_ipc - Linux2.4 统一IPC入口系统调用（兼容老ABI）
 * 说明：早期Linux没有分开的semget/semop/msgget/shmat等独立syscall，
 *       全部复用sys_ipc这一个系统调用号，靠call子命令区分具体操作
 * @call: 高16位version版本号 | 低16位子命令（SEMOP/MSGSND/SHMAT...）
 * @first: 参数1
 * @second: 参数2
 * @third: 参数3
 * @ptr: 用户空间指针参数
 * @fifth: 参数5
 * 返回值：子ipc函数的返回值，失败返回负错误码
 */
asmlinkage int sys_ipc (uint call, int first, int second,
        int third, void *ptr, long fifth)
{
    int version, ret;

    // 把call高16位拆出来作为版本号，低16位保留子命令号，用于向前兼容
    version = call >> 16; /* hack for backward compatibility */
    call &= 0xffff;

    switch (call) {
    case SEMOP:
        // 信号量P/V操作：semop
        return sys_semop (first, (struct sembuf *)ptr, second);
    case SEMGET:
        // 创建/获取信号量集：semget
        return sys_semget (first, second, third);
    case SEMCTL: {
        // 信号量控制：semctl
        union semun fourth;
        if (!ptr)
            return -EINVAL;
        // 从用户空间取出union semun参数
        if (get_user(fourth.__pad, (void **) ptr))
            return -EFAULT;
        return sys_semctl (first, second, third, fourth);
    }

    case MSGSND:
        // 消息队列发送：msgsnd
        return sys_msgsnd (first, (struct msgbuf *) ptr, 
                second, third);
    case MSGRCV:
        // 消息队列接收：msgrcv，分版本兼容
        switch (version) {
        case 0: {
            struct ipc_kludge tmp;
            if (!ptr)
                return -EINVAL;
            
            // 拷贝用户传入的ipc_kludge封装结构
            if (copy_from_user(&tmp,
                        (struct ipc_kludge *) ptr, 
                        sizeof (tmp)))
                return -EFAULT;
            return sys_msgrcv (first, tmp.msgp, second,
                        tmp.msgtyp, third);
        }
        default:
            // 新版本直接使用ptr、fifth参数
            return sys_msgrcv (first,
                        (struct msgbuf *) ptr,
                        second, fifth, third);
        }
    case MSGGET:
        // 创建/获取消息队列：msgget
        return sys_msgget ((key_t) first, second);
    case MSGCTL:
        // 消息队列控制：msgctl
        return sys_msgctl (first, second, (struct msqid_ds *) ptr);

    case SHMAT:
        // 共享内存挂载：shmat，版本区分
        switch (version) {
        default: {
            ulong raddr;
            ret = sys_shmat (first, (char *) ptr, second, &raddr);
            if (ret)
                return ret;
            // 把映射后的虚拟地址写回用户空间
            return put_user (raddr, (ulong *) third);
        }
        case 1: /* iBCS2 emulator entry point */
            // iBCS2二进制兼容模式，直接将地址输出到third指针
            if (!segment_eq(get_fs(), get_ds()))
                return -EINVAL;
            return sys_shmat (first, (char *) ptr, second, (ulong *) third);
        }
    case SHMDT: 
        // 共享内存脱离：shmdt
        return sys_shmdt ((char *)ptr);
    case SHMGET:
        // 创建/获取共享内存段：shmget
        return sys_shmget (first, second, third);
    case SHMCTL:
        // 共享内存控制：shmctl
        return sys_shmctl (first, second,
                (struct shmid_ds *) ptr);
    default:
        // 未知子命令
        return -EINVAL;
    }
}

/*
 * Old cruft
 */
asmlinkage int sys_uname(struct old_utsname * name)
{
	int err;
	if (!name)
		return -EFAULT;
	down_read(&uts_sem);
	err=copy_to_user(name, &system_utsname, sizeof (*name));
	up_read(&uts_sem);
	return err?-EFAULT:0;
}

asmlinkage int sys_olduname(struct oldold_utsname * name)
{
	int error;

	if (!name)
		return -EFAULT;
	if (!access_ok(VERIFY_WRITE,name,sizeof(struct oldold_utsname)))
		return -EFAULT;
  
  	down_read(&uts_sem);
	
	error = __copy_to_user(&name->sysname,&system_utsname.sysname,__OLD_UTS_LEN);
	error |= __put_user(0,name->sysname+__OLD_UTS_LEN);
	error |= __copy_to_user(&name->nodename,&system_utsname.nodename,__OLD_UTS_LEN);
	error |= __put_user(0,name->nodename+__OLD_UTS_LEN);
	error |= __copy_to_user(&name->release,&system_utsname.release,__OLD_UTS_LEN);
	error |= __put_user(0,name->release+__OLD_UTS_LEN);
	error |= __copy_to_user(&name->version,&system_utsname.version,__OLD_UTS_LEN);
	error |= __put_user(0,name->version+__OLD_UTS_LEN);
	error |= __copy_to_user(&name->machine,&system_utsname.machine,__OLD_UTS_LEN);
	error |= __put_user(0,name->machine+__OLD_UTS_LEN);
	
	up_read(&uts_sem);
	
	error = error ? -EFAULT : 0;

	return error;
}

asmlinkage int sys_pause(void)
{
	current->state = TASK_INTERRUPTIBLE;
	schedule();
	return -ERESTARTNOHAND;
}

