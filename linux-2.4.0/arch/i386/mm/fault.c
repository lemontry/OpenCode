/*
 *  linux/arch/i386/mm/fault.c
 *
 *  Copyright (C) 1995  Linus Torvalds
 */

#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/ptrace.h>
#include <linux/mman.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/smp_lock.h>
#include <linux/interrupt.h>
#include <linux/init.h>

#include <asm/system.h>
#include <asm/uaccess.h>
#include <asm/pgalloc.h>
#include <asm/hardirq.h>

extern void die(const char *,struct pt_regs *,long);

/*
 * Ugly, ugly, but the goto's result in better assembly..
 */
int __verify_write(const void * addr, unsigned long size)
{
	struct vm_area_struct * vma;
	unsigned long start = (unsigned long) addr;

	if (!size)
		return 1;

	vma = find_vma(current->mm, start);
	if (!vma)
		goto bad_area;
	if (vma->vm_start > start)
		goto check_stack;

good_area:
	if (!(vma->vm_flags & VM_WRITE))
		goto bad_area;
	size--;
	size += start & ~PAGE_MASK;
	size >>= PAGE_SHIFT;
	start &= PAGE_MASK;

	for (;;) {
		if (handle_mm_fault(current->mm, vma, start, 1) <= 0)
			goto bad_area;
		if (!size)
			break;
		size--;
		start += PAGE_SIZE;
		if (start < vma->vm_end)
			continue;
		vma = vma->vm_next;
		if (!vma || vma->vm_start != start)
			goto bad_area;
		if (!(vma->vm_flags & VM_WRITE))
			goto bad_area;;
	}
	return 1;

check_stack:
	if (!(vma->vm_flags & VM_GROWSDOWN))
		goto bad_area;
	if (expand_stack(vma, start) == 0)
		goto good_area;

bad_area:
	return 0;
}

extern spinlock_t console_lock, timerlist_lock;

/*
 * Unlock any spinlocks which will prevent us from getting the
 * message out (timerlist_lock is aquired through the
 * console unblank code)
 */
void bust_spinlocks(void)
{
	spin_lock_init(&console_lock);
	spin_lock_init(&timerlist_lock);
}

asmlinkage void do_invalid_op(struct pt_regs *, unsigned long);
extern unsigned long idt;

/*
 * This routine handles page faults.  It determines the address,
 * and the problem, and then passes it off to one of the appropriate
 * routines.
 *
 * error_code:
 *	bit 0 == 0 means no page found, 1 means protection fault
 *	bit 1 == 0 means read, 1 means write
 *	bit 2 == 0 means kernel, 1 means user-mode
 */
/**
 * asmlinkage void do_page_fault(struct pt_regs *regs, unsigned long error_code)
 * @regs:        发生缺页异常时的CPU寄存器现场（用户/内核栈、eip、eflags等）
 * @error_code:  CPU自动压入栈的缺页错误码，标识异常原因
 *
 * 【说明】这是Linux 2.4 x86-32 原版do_page_fault，缺页异常总入口（#14异常）
 * asmlinkage：约定参数从栈上取，不是寄存器传参，适配异常调用ABI
 * error_code bit定义(x86):
 *      bit0(P): 0=页不存在；1=页存在（保护错误，权限违规）
 *      bit1(W): 0=读访问；1=写访问
 *      bit2(U): 0=内核态访问；1=用户态访问
 */
asmlinkage void do_page_fault(struct pt_regs *regs, unsigned long error_code)
{
	struct task_struct *tsk;
	struct mm_struct *mm;
	struct vm_area_struct * vma;
	unsigned long address;
	unsigned long page;
	unsigned long fixup;
	int write;
	siginfo_t info;

	/*
	 * 从cr2寄存器读出触发缺页的虚拟地址
	 * cr2由CPU硬件自动写入：保存导致page fault的VA
	 */
	__asm__("movl %%cr2,%0":"=r" (address));

	/* 获取当前正在运行的task */
	tsk = current;

	/*
	 * 内核空间vmalloc区域缺页分支
	 * TASK_SIZE：用户空间地址上限；>=TASK_SIZE 属于内核虚拟地址
	 * vmalloc 页表是按需从init_mm（主内核页表）同步拷贝PDE/PMD
	 * 注意：这个路径不能拿锁！可能在中断上下文，只做页表拷贝，不分配内存
	 */
	if (address >= TASK_SIZE)
		goto vmalloc_fault;

	/* 当前进程的内存描述符 */
	mm = tsk->mm;
	info.si_code = SEGV_MAPERR;    /* 默认：地址没有映射（找不到vma） */

	/*
	 * 禁止在中断上下文、或者没有mm的内核线程里处理用户缺页
	 * 中断上下文 / 内核线程(mm=NULL) 直接走异常错误路径
	 */
	if (in_interrupt() || !mm)
		goto no_context;

	/* 拿mmap读写信号量，保护vma链表/AVL树（前面你看的avl_insert就在这套体系里） */
	down(&mm->mmap_sem);

	/* 在进程vma集合里查找包含该地址的vma，底层就是find_vma，2.4是AVL树实现 */
	vma = find_vma(mm, address);
	if (!vma)
		goto bad_area;
	/* find_vma返回的是vm_start > address的第一个vma；
	 * 如果vma->vm_start <= address，说明地址落在这个vma区间内，合法映射区域 */
	if (vma->vm_start <= address)
		goto good_area;

	/* 地址在vma之前，且该vma不是可向下增长栈（VM_GROWSDOWN，用户栈）→非法 */
	if (!(vma->vm_flags & VM_GROWSDOWN))
		goto bad_area;

	/*
	 * error_code bit2=1：用户态访问栈
	 * 栈向下增长，esp是栈顶；address+32 < esp 代表访问esp下方很远位置，属于非法栈访问
	 * +32是兼容pusha这类后递减栈指令的硬件行为
	 */
	if (error_code & 4) {
		if (address + 32 < regs->esp)
			goto bad_area;
	}
	/* 尝试扩展栈vma，向下扩大栈区间；失败则报错 */
	if (expand_stack(vma, address))
		goto bad_area;

/*
 * good_area分支：找到了合法有效的vma，地址落在vma范围内
 * 接下来校验访问权限、判断是读缺页/写缺页
 */
good_area:
	info.si_code = SEGV_ACCERR;  /* 错误类型更新：有映射，但权限不允许 */
	write = 0;
	/* 只看error_code低两位 bit1(写) bit0(存在位) */
	switch (error_code & 3) {
		default:	/* 3: bit1=1,bit0=1：写，页已经存在 → WP写保护错误 */
#ifdef TEST_VERIFY_AREA
			if (regs->cs == KERNEL_CS)
				printk("WP fault at %08lx\n", regs->eip);
#endif
			/* fall through */
		case 2:		/* 2: bit1=1,bit0=0：写，页面不在内存（缺页） */
			if (!(vma->vm_flags & VM_WRITE))
				goto bad_area;  /* vma不可写，权限违规 */
			write++;         /* 标记本次fault是写缺页，触发COW */
			break;
		case 1:		/* 1: bit1=0,bit0=1：读，页已存在 → 权限错误 */
			goto bad_area;
		case 0:		/* 0: bit1=0,bit0=0：读，页面不在内存（读缺页） */
			if (!(vma->vm_flags & (VM_READ | VM_EXEC)))
				goto bad_area;
	}

	/*
	 * handle_mm_fault：核心缺页处理函数
	 * 做页分配、读磁盘、COW、填充页表，返回值含义：
	 * 1：次缺页(minor fault)，页在pagecache，无IO
	 * 2：主缺页(major fault)，需要磁盘IO
	 * 0：失败，发SIGBUS
	 * 其他：OOM
	 */
	switch (handle_mm_fault(mm, vma, address, write)) {
	case 1:
		tsk->min_flt++;
		break;
	case 2:
		tsk->maj_flt++;
		break;
	case 0:
		goto do_sigbus;
	default:
		goto out_of_memory;
	}

	/* vm86 DOS虚拟机模式特殊处理：屏幕内存0xA0000区域标记bitmap */
	if (regs->eflags & VM_MASK) {
		unsigned long bit = (address - 0xA0000) >> PAGE_SHIFT;
		if (bit < 32)
			tsk->thread.screen_bitmap |= 1 << bit;
	}
	up(&mm->mmap_sem);
	return;

/*
 * bad_area：找到vma但是地址非法 / 权限不匹配
 * 先释放mmap_sem，再处理信号
 */
bad_area:
	up(&mm->mmap_sem);

bad_area_nosemaphore:
	/* bit2=1：用户态触发fault，直接给进程发送SIGSEGV段错误信号 */
	if (error_code & 4) {
		tsk->thread.cr2 = address;
		tsk->thread.error_code = error_code;
		tsk->thread.trap_no = 14;
		info.si_signo = SIGSEGV;
		info.si_errno = 0;
		/* info.si_code 前面已经赋值 SEGV_MAPERR / SEGV_ACCERR */
		info.si_addr = (void *)address;
		force_sig_info(SIGSEGV, &info, tsk);
		return;
	}

	/* Pentium F0 0F C7 C8 经典CPU bug workaround */
	if (boot_cpu_data.f00f_bug) {
		unsigned long nr;
		
		nr = (address - idt) >> 3;

		if (nr == 6) {
			do_invalid_op(regs, 0);
			return;
		}
	}

/*
 * no_context：内核态发生缺页（内核访问非法地址）
 * 优先查异常修正表exception table：内核里__get_user等带fixup的位置
 * 如果找到fixup地址，修改eip跳转至修复代码，不Oops
 */
no_context:
	if ((fixup = search_exception_table(regs->eip)) != 0) {
		regs->eip = fixup;
		return;
	}

	/* 找不到修复点 → Oops，内核崩溃打印 */
	bust_spinlocks();

	if (address < PAGE_SIZE)
		printk(KERN_ALERT "Unable to handle kernel NULL pointer dereference");
	else
		printk(KERN_ALERT "Unable to handle kernel paging request");
	printk(" at virtual address %08lx\n",address);
	printk(" printing eip:\n");
	printk("%08lx\n", regs->eip);
	asm("movl %%cr3,%0":"=r" (page));
	page = ((unsigned long *) __va(page))[address >> 22];
	printk(KERN_ALERT "*pde = %08lx\n", page);
	if (page & 1) {
		page &= PAGE_MASK;
		address &= 0x003ff000;
		page = ((unsigned long *) __va(page))[address >> PAGE_SHIFT];
		printk(KERN_ALERT "*pte = %08lx\n", page);
	}
	die("Oops", regs, error_code);
	do_exit(SIGKILL);

/*
 * out_of_memory：handle_mm_fault分配内存失败，OOM
 */
out_of_memory:
	up(&mm->mmap_sem);
	printk("VM: killing process %s\n", tsk->comm);
	if (error_code & 4)
		do_exit(SIGKILL);
	goto no_context;

/*
 * do_sigbus：缺页处理返回0，硬件/文件映射错误（比如缺页在损坏的文件映射）
 * 发送SIGBUS总线错误信号
 */
do_sigbus:
	up(&mm->mmap_sem);

	tsk->thread.cr2 = address;
	tsk->thread.error_code = error_code;
	tsk->thread.trap_no = 14;
	info.si_code = SIGBUS;
	info.si_errno = 0;
	info.si_code = BUS_ADRERR;
	info.si_addr = (void *)address;
	force_sig_info(SIGBUS, &info, tsk);

	/* 如果是内核态，没有用户进程接收信号，走no_context Oops */
	if (!(error_code & 4))
		goto no_context;
	return;

/*
 * vmalloc_fault：内核vmalloc区域缺页
 * vmalloc的页表主副本在init_mm，进程页表只在访问时同步顶层pgd/pmd
 * 只同步高层页目录，不分配page，所以不能拿锁
 */
vmalloc_fault:
	{
		int offset = __pgd_offset(address);
		pgd_t *pgd, *pgd_k;
		pmd_t *pmd, *pmd_k;

		/* 当前进程活跃mm的pgd 和 init_mm的基准pgd */
		pgd = tsk->active_mm->pgd + offset;
		pgd_k = init_mm.pgd + offset;

		/* 当前进程pgd不存在，而init_mm里存在 → 拷贝pgd项 */
		if (!pgd_present(*pgd)) {
			if (!pgd_present(*pgd_k))
				goto bad_area_nosemaphore;
			set_pgd(pgd, *pgd_k);
			return;
		}

		pmd = pmd_offset(pgd, address);
		pmd_k = pmd_offset(pgd_k, address);

		/* pmd已经存在或者init_mm里pmd不存在，失败 */
		if (pmd_present(*pmd) || !pmd_present(*pmd_k))
			goto bad_area_nosemaphore;
		set_pmd(pmd, *pmd_k);
		return;
	}
}
 