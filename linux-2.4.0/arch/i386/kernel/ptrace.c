/* ptrace.c */
/* By Ross Biro 1/23/92 */
/*
 * Pentium III FXSR, SSE support
 *	Gareth Hughes <gareth@valinux.com>, May 2000
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/smp_lock.h>
#include <linux/errno.h>
#include <linux/ptrace.h>
#include <linux/user.h>

#include <asm/uaccess.h>
#include <asm/pgtable.h>
#include <asm/system.h>
#include <asm/processor.h>
#include <asm/i387.h>
#include <asm/debugreg.h>

/*
 * does not yet catch signals sent when the child dies.
 * in exit.c or in signal.c.
 */

/* determines which flags the user has access to. */
/* 1 = access 0 = no access */
#define FLAG_MASK 0x00044dd5

/* set's the trap flag. */
#define TRAP_FLAG 0x100

/*
 * Offset of eflags on child stack..
 */
#define EFL_OFFSET ((EFL-2)*4-sizeof(struct pt_regs))

/*
 * this routine will get a word off of the processes privileged stack. 
 * the offset is how far from the base addr as stored in the TSS.  
 * this routine assumes that all the privileged stacks are in our
 * data space.
 */   
static inline int get_stack_long(struct task_struct *task, int offset)
{
	unsigned char *stack;

	stack = (unsigned char *)task->thread.esp0;
	stack += offset;
	return (*((int *)stack));
}

/*
 * this routine will put a word on the processes privileged stack. 
 * the offset is how far from the base addr as stored in the TSS.  
 * this routine assumes that all the privileged stacks are in our
 * data space.
 */
static inline int put_stack_long(struct task_struct *task, int offset,
	unsigned long data)
{
	unsigned char * stack;

	stack = (unsigned char *) task->thread.esp0;
	stack += offset;
	*(unsigned long *) stack = data;
	return 0;
}

static int putreg(struct task_struct *child,
	unsigned long regno, unsigned long value)
{
	switch (regno >> 2) {
		case FS:
			if (value && (value & 3) != 3)
				return -EIO;
			child->thread.fs = value;
			return 0;
		case GS:
			if (value && (value & 3) != 3)
				return -EIO;
			child->thread.gs = value;
			return 0;
		case DS:
		case ES:
			if (value && (value & 3) != 3)
				return -EIO;
			value &= 0xffff;
			break;
		case SS:
		case CS:
			if ((value & 3) != 3)
				return -EIO;
			value &= 0xffff;
			break;
		case EFL:
			value &= FLAG_MASK;
			value |= get_stack_long(child, EFL_OFFSET) & ~FLAG_MASK;
			break;
	}
	if (regno > GS*4)
		regno -= 2*4;
	put_stack_long(child, regno - sizeof(struct pt_regs), value);
	return 0;
}

static unsigned long getreg(struct task_struct *child,
	unsigned long regno)
{
	unsigned long retval = ~0UL;

	switch (regno >> 2) {
		case FS:
			retval = child->thread.fs;
			break;
		case GS:
			retval = child->thread.gs;
			break;
		case DS:
		case ES:
		case SS:
		case CS:
			retval = 0xffff;
			/* fall through */
		default:
			if (regno > GS*4)
				regno -= 2*4;
			regno = regno - sizeof(struct pt_regs);
			retval &= get_stack_long(child, regno);
	}
	return retval;
}
/**
 * sys_ptrace - ptrace系统调用实现（Linux 2.4 i386）
 * @request: 操作命令，PTRACE_*
 * @pid: 目标进程pid
 * @addr: 目标地址（内存地址 / user区域偏移）
 * @data: 传入/传出参数、用户态缓冲区指针
 * 功能：进程调试、内存读写、寄存器读写、单步、syscall跟踪、attach/detach
 */
asmlinkage int sys_ptrace(long request, long pid, long addr, long data)
{
    struct task_struct *child;
    struct user * dummy = NULL;
    int i, ret;
    // 获取BKL大内核锁，2.4老内核保护全局task相关逻辑
    lock_kernel();
    ret = -EPERM;
    // PTRACE_TRACEME：当前进程主动声明允许父进程跟踪自己
    if (request == PTRACE_TRACEME) {
        /* are we already being traced? */
        // 如果本进程已经被跟踪，直接返回权限错误
        if (current->ptrace & PT_PTRACED)
            goto out;
        /* set the ptrace bit in the process flags. */
        // 打上被跟踪标记 PT_PTRACED
        current->ptrace |= PT_PTRACED;
        ret = 0;
        goto out;
    }
    ret = -ESRCH;
    // 读tasklist链表锁，遍历查找task
    read_lock(&tasklist_lock);
    child = find_task_by_pid(pid);
    if (child)
        get_task_struct(child);
    read_unlock(&tasklist_lock);
    // pid不存在，进程没找到
    if (!child)
        goto out;
    ret = -EPERM;
    // 禁止跟踪init(1号进程)
    if (pid == 1)   /* you may not mess with init */
        goto out_tsk;
    // PTRACE_ATTACH：跟踪者附加到一个已经在运行的目标进程
    if (request == PTRACE_ATTACH) {
        // 不能attach自己
        if (child == current)
            goto out_tsk;
        // 权限校验：uid/gid匹配 或者 拥有CAP_SYS_PTRACE特权
        if ((!child->dumpable ||
            (current->uid != child->euid) ||
            (current->uid != child->suid) ||
            (current->uid != child->uid) ||
            (current->gid != child->egid) ||
            (current->gid != child->sgid) ||
            (!cap_issubset(child->cap_permitted, current->cap_permitted)) ||
            (current->gid != child->gid)) && !capable(CAP_SYS_PTRACE))
            goto out_tsk;
        /* the same process cannot be attached many times */
        // 目标已经被别的进程跟踪，禁止重复attach
        if (child->ptrace & PT_PTRACED)
            goto out_tsk;
        // 给目标打上被跟踪标记
        child->ptrace |= PT_PTRACED;
        write_lock_irq(&tasklist_lock);
        // 修改父子关系：把child的父进程改成当前调试进程
        if (child->p_pptr != current) {
            REMOVE_LINKS(child);
            child->p_pptr = current;
            SET_LINKS(child);
        }
        write_unlock_irq(&tasklist_lock);
        // 发送SIGSTOP，让目标进程停下来，等待调试命令
        send_sig(SIGSTOP, child, 1);
        ret = 0;
        goto out_tsk;
    }
    ret = -ESRCH;
    // 后续所有命令，要求目标已经处于被跟踪状态
    if (!(child->ptrace & PT_PTRACED))
        goto out_tsk;
    // 除了PTRACE_KILL之外，其它ptrace命令要求目标是TASK_STOPPED
    if (child->state != TASK_STOPPED) {
        if (request != PTRACE_KILL)
            goto out_tsk;
    }
    // 必须是当前进程是child的父跟踪进程
    if (child->p_pptr != current)
        goto out_tsk;

    switch (request) {
    /* when I and D space are separate, these will need to be fixed. */
    // PTRACE_PEEKTEXT / PTRACE_PEEKDATA：读取目标进程一个long字
    case PTRACE_PEEKTEXT: /* read word at location addr. */ 
    case PTRACE_PEEKDATA: {
        unsigned long tmp;
        int copied;
        // 从child进程虚拟地址addr读取sizeof(tmp)字节，0=读
        copied = access_process_vm(child, addr, &tmp, sizeof(tmp), 0);
        ret = -EIO;
        // 读取字节数不对，IO错误
        if (copied != sizeof(tmp))
            break;
        // 读到的数据拷贝回用户空间data指针
        ret = put_user(tmp,(unsigned long *) data);
        break;
    }
    /* read the word at location addr in the USER area. */
    // PTRACE_PEEKUSR：读取user区域（寄存器、debug寄存器）
    case PTRACE_PEEKUSR: {
        unsigned long tmp;
        ret = -EIO;
        // 地址必须4字节对齐，并且不能越界struct user
        if ((addr & 3) || addr < 0 || 
            addr > sizeof(struct user) - 3)
            break;
        tmp = 0;  /* Default return condition */
        // 前17个long是通用寄存器，getreg读取pt_regs
        if(addr < 17*sizeof(long))
            tmp = getreg(child, addr);
        // 访问硬件debug寄存器 dr0~dr7
        if(addr >= (long) &dummy->u_debugreg[0] &&
            addr <= (long) &dummy->u_debugreg[7]){
            addr -= (long) &dummy->u_debugreg[0];
            addr = addr >> 2;
            tmp = child->thread.debugreg[addr];
        }
        // 结果拷贝回用户态
        ret = put_user(tmp,(unsigned long *) data);
        break;
    }
    /* when I and D space are separate, this will have to be fixed. */
    // PTRACE_POKETEXT / PTRACE_POKEDATA：向目标进程地址写入一个long
    case PTRACE_POKETEXT: /* write the word at location addr. */
    case PTRACE_POKEDATA:
        ret = 0;
        // access_process_vm最后参数1代表写
        if (access_process_vm(child, addr, &data, sizeof(data), 1) == sizeof(data))
            break;
        ret = -EIO;
        break;
    // PTRACE_POKEUSR：写user区域，修改寄存器/debug寄存器
    case PTRACE_POKEUSR: /* write the word at location addr in the USER area */
        ret = -EIO;
        if ((addr & 3) || addr < 0 || 
            addr > sizeof(struct user) - 3)
            break;
        // 通用寄存器区域，调用putreg修改pt_regs
        if (addr < 17*sizeof(long)) {
            ret = putreg(child, addr, data);
            break;
        }
        /* We need to be very careful here.  We implicitly
           want to modify a portion of the task_struct, and we
           have to be selective about what portions we allow someone
           to modify. */
        ret = -EIO;
        // 修改硬件debug寄存器dr0~dr7
        if(addr >= (long) &dummy->u_debugreg[0] &&
           addr <= (long) &dummy->u_debugreg[7]){
            // dr4、dr5不允许修改
            if(addr == (long) &dummy->u_debugreg[4]) break;
            if(addr == (long) &dummy->u_debugreg[5]) break;
            if(addr < (long) &dummy->u_debugreg[4] &&
               ((unsigned long) data) >= TASK_SIZE-3) break;
            
            if(addr == (long) &dummy->u_debugreg[7]) {
                // dr7控制寄存器，清掉保留位，校验合法
                data &= ~DR_CONTROL_RESERVED;
                for(i=0; i<4; i++)
                    if ((0x5f54 >> ((data >> (16 + 4*i)) & 0xf)) & 1)
                        goto out_tsk;
            }
            addr -= (long) &dummy->u_debugreg;
            addr = addr >> 2;
            child->thread.debugreg[addr] = data;
            ret = 0;
        }
        break;
    // PTRACE_SYSCALL：恢复运行，在系统调用进入/退出时停下
    // PTRACE_CONT：恢复运行，直到收到信号停下
    case PTRACE_SYSCALL: /* continue and stop at next (return from) syscall */
    case PTRACE_CONT: { /* restart after signal. */
        long tmp;
        ret = -EIO;
        // data参数是传递给子进程的信号号，不能超过_NSIG
        if ((unsigned long) data > _NSIG)
            break;
        if (request == PTRACE_SYSCALL)
            // 开启系统调用跟踪标记 PT_TRACESYS
            child->ptrace |= PT_TRACESYS;
        else
            // 关闭系统调用跟踪
            child->ptrace &= ~PT_TRACESYS;
        // exit_code保存要投递的信号
        child->exit_code = data;
    /* make sure the single step bit is not set. */
        // 清除eflags的TF单步陷阱标志
        tmp = get_stack_long(child, EFL_OFFSET) & ~TRAP_FLAG;
        put_stack_long(child, EFL_OFFSET,tmp);
        // 唤醒被停止的子进程，投入调度运行
        wake_up_process(child);
        ret = 0;
        break;
    }
/*
 * make the child exit.  Best I can do is send it a sigkill. 
 * perhaps it should be put in the status that it wants to 
 * exit.
 */
    // PTRACE_KILL：杀死被跟踪进程
    case PTRACE_KILL: {
        long tmp;
        ret = 0;
        // 已经是僵尸进程，直接返回成功
        if (child->state == TASK_ZOMBIE)    /* already dead */
            break;
        child->exit_code = SIGKILL;
        // 清除TF单步标志
        /* make sure the single step bit is not set. */
        tmp = get_stack_long(child, EFL_OFFSET) & ~TRAP_FLAG;
        put_stack_long(child, EFL_OFFSET, tmp);
        wake_up_process(child);
        break;
    }
    // PTRACE_SINGLESTEP：单步执行一条指令（设置TF陷阱标志）
    case PTRACE_SINGLESTEP: {  /* set the trap flag. */
        long tmp;
        ret = -EIO;
        if ((unsigned long) data > _NSIG)
            break;
        // 关闭syscall跟踪标记
        child->ptrace &= ~PT_TRACESYS;
        if ((child->ptrace & PT_DTRACE) == 0) {
            /* Spurious delayed TF traps may occur */
            child->ptrace |= PT_DTRACE;
        }
        // 设置eflags的TF位，开启硬件单步
        tmp = get_stack_long(child, EFL_OFFSET) | TRAP_FLAG;
        put_stack_long(child, EFL_OFFSET, tmp);
        child->exit_code = data;
        /* give it a chance to run. */
        wake_up_process(child);
        ret = 0;
        break;
    }
    // PTRACE_DETACH：解除跟踪，恢复进程原本父子关系，继续正常运行
    case PTRACE_DETACH: { /* detach a process that was attached. */
        long tmp;
        ret = -EIO;
        if ((unsigned long) data > _NSIG)
            break;
        // 清空ptrace所有跟踪标记
        child->ptrace = 0;
        child->exit_code = data;
        write_lock_irq(&tasklist_lock);
        // 恢复原来的父进程p_opptr（original parent）
        REMOVE_LINKS(child);
        child->p_pptr = child->p_opptr;
        SET_LINKS(child);
        write_unlock_irq(&tasklist_lock);
        /* make sure the single step bit is not set. */
        tmp = get_stack_long(child, EFL_OFFSET) & ~TRAP_FLAG;
        put_stack_long(child, EFL_OFFSET, tmp);
        wake_up_process(child);
        ret = 0;
        break;
    }
    // PTRACE_GETREGS：一次性读取全部通用寄存器
    case PTRACE_GETREGS: { /* Get all gp regs from the child. */
        if (!access_ok(VERIFY_WRITE, (unsigned *)data, 17*sizeof(long))) {
            ret = -EIO;
            break;
        }
        for ( i = 0; i < 17*sizeof(long); i += sizeof(long) ) {
            __put_user(getreg(child, i),(unsigned long *) data);
            data += sizeof(long);
        }
        ret = 0;
        break;
    }
    // PTRACE_SETREGS：一次性写入全部通用寄存器
    case PTRACE_SETREGS: { /* Set all gp regs in the child. */
        unsigned long tmp;
        if (!access_ok(VERIFY_READ, (unsigned *)data, 17*sizeof(long))) {
            ret = -EIO;
            break;
        }
        for ( i = 0; i < 17*sizeof(long); i += sizeof(long) ) {
            __get_user(tmp, (unsigned long *) data);
            putreg(child, i, tmp);
            data += sizeof(long);
        }
        ret = 0;
        break;
    }
    // PTRACE_GETFPREGS：读取传统FPU浮点寄存器
    case PTRACE_GETFPREGS: { /* Get the child FPU state. */
        if (!access_ok(VERIFY_WRITE, (unsigned *)data,
                   sizeof(struct user_i387_struct))) {
            ret = -EIO;
            break;
        }
        ret = 0;
        if ( !child->used_math ) {
            /* Simulate an empty FPU. */
            set_fpu_cwd(child, 0x037f);
            set_fpu_swd(child, 0x0000);
            set_fpu_twd(child, 0xffff);
        }
        get_fpregs((struct user_i387_struct *)data, child);
        break;
    }
    // PTRACE_SETFPREGS：写入传统FPU浮点寄存器
    case PTRACE_SETFPREGS: { /* Set the child FPU state. */
        if (!access_ok(VERIFY_READ, (unsigned *)data,
                   sizeof(struct user_i387_struct))) {
            ret = -EIO;
            break;
        }
        child->used_math = 1;
        set_fpregs(child, (struct user_i387_struct *)data);
        ret = 0;
        break;
    }
    // PTRACE_GETFPXREGS：读取SSE扩展浮点FXSR寄存器
    case PTRACE_GETFPXREGS: { /* Get the child extended FPU state. */
        if (!access_ok(VERIFY_WRITE, (unsigned *)data,
                   sizeof(struct user_fxsr_struct))) {
            ret = -EIO;
            break;
        }
        if ( !child->used_math ) {
            /* Simulate an empty FPU. */
            set_fpu_cwd(child, 0x037f);
            set_fpu_swd(child, 0x0000);
            set_fpu_twd(child, 0xffff);
            set_fpu_mxcsr(child, 0x1f80);
        }
        ret = get_fpxregs((struct user_fxsr_struct *)data, child);
        break;
    }
    // PTRACE_SETFPXREGS：写入SSE FXSR扩展浮点寄存器
    case PTRACE_SETFPXREGS: { /* Set the child extended FPU state. */
        if (!access_ok(VERIFY_READ, (unsigned *)data,
                   sizeof(struct user_fxsr_struct))) {
            ret = -EIO;
            break;
        }
        child->used_math = 1;
        ret = set_fpxregs(child, (struct user_fxsr_struct *)data);
        break;
    }
    // PTRACE_SETOPTIONS：设置ptrace选项，这里只支持PTRACE_O_TRACESYSGOOD
    case PTRACE_SETOPTIONS: {
        if (data & PTRACE_O_TRACESYSGOOD)
            child->ptrace |= PT_TRACESYSGOOD;
        else
            child->ptrace &= ~PT_TRACESYSGOOD;
        ret = 0;
        break;
    }
    default:
        // 不认识的request命令
        ret = -EIO;
        break;
    }
out_tsk:
    // 释放task_struct引用计数
    free_task_struct(child);
out:
    // 释放BKL大内核锁
    unlock_kernel();
    return ret;
}

/**
 * syscall_trace - 系统调用跟踪断点函数（Linux 2.4 i386）
 * 触发时机：系统调用入口、系统调用返回两个位置都会调用这个函数
 * 作用：如果开启PT_TRACESYS，就让子进程停下，通知tracer父进程（strace/gdb catch syscall）
 */
asmlinkage void syscall_trace(void)
{
    // 判断：必须同时满足 PT_PTRACED(被跟踪) + PT_TRACESYS(开启syscall跟踪)，否则直接返回，不拦截
    if ((current->ptrace & (PT_PTRACED|PT_TRACESYS)) !=
            (PT_PTRACED|PT_TRACESYS))
        return;
    /* the 0x80 provides a way for the tracing parent to distinguish
       between a syscall stop and SIGTRAP delivery */
    // 设置exit_code：基础是SIGTRAP；如果开启PT_TRACESYSGOOD，或上0x80标记
    // strace依靠这个0x80区分【syscall触发的trap】和【普通断点/单步产生的SIGTRAP】
    current->exit_code = SIGTRAP | ((current->ptrace & PT_TRACESYSGOOD)
            ? 0x80 : 0);
    // 修改进程状态为停止 TASK_STOPPED
    current->state = TASK_STOPPED;
    // 给跟踪父进程发送SIGCHLD，唤醒父进程wait4
    notify_parent(current, SIGCHLD);
    // 主动让出CPU，进入调度，子进程停在这里，等待父进程ptrace(PTRACE_SYSCALL/CONT)唤醒
    schedule();
    /*
     * this isn't the same as continuing with a signal, but it will do
     * for normal use.  strace only continues with a signal if the
     * stopping signal is not SIGTRAP.  -brl
     */
    // 被父进程唤醒之后，如果exit_code里携带了信号，投递这个信号给当前进程
    if (current->exit_code) {
        send_sig(current->exit_code, current, 1);
        current->exit_code = 0;
    }
}
