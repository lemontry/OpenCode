/*
 *  linux/kernel/fork.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 *  'fork.c' contains the help-routines for the 'fork' system call
 * (see also entry.S and others).
 * Fork is rather simple, once you get the hang of it, but the memory
 * management can be a bitch. See 'mm/memory.c': 'copy_page_tables()'
 */

#include <linux/config.h>
#include <linux/malloc.h>
#include <linux/init.h>
#include <linux/unistd.h>
#include <linux/smp_lock.h>
#include <linux/module.h>
#include <linux/vmalloc.h>

#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/uaccess.h>
#include <asm/mmu_context.h>

/* The idle threads do not count.. */
int nr_threads;
int nr_running;

int max_threads;
unsigned long total_forks;	/* Handle normal Linux uptimes. */
int last_pid;

struct task_struct *pidhash[PIDHASH_SZ];

void add_wait_queue(wait_queue_head_t *q, wait_queue_t * wait)
{
	unsigned long flags;

	wq_write_lock_irqsave(&q->lock, flags);
	wait->flags = 0;
	__add_wait_queue(q, wait);
	wq_write_unlock_irqrestore(&q->lock, flags);
}

void add_wait_queue_exclusive(wait_queue_head_t *q, wait_queue_t * wait)
{
	unsigned long flags;

	wq_write_lock_irqsave(&q->lock, flags);
	wait->flags = WQ_FLAG_EXCLUSIVE;
	__add_wait_queue_tail(q, wait);
	wq_write_unlock_irqrestore(&q->lock, flags);
}

void remove_wait_queue(wait_queue_head_t *q, wait_queue_t * wait)
{
	unsigned long flags;

	wq_write_lock_irqsave(&q->lock, flags);
	__remove_wait_queue(q, wait);
	wq_write_unlock_irqrestore(&q->lock, flags);
}

void __init fork_init(unsigned long mempages)
{
	/*
	 * The default maximum number of threads is set to a safe
	 * value: the thread structures can take up at most half
	 * of memory.
	 */
	max_threads = mempages / (THREAD_SIZE/PAGE_SIZE) / 2;

	init_task.rlim[RLIMIT_NPROC].rlim_cur = max_threads/2;
	init_task.rlim[RLIMIT_NPROC].rlim_max = max_threads/2;
}

/* Protects next_safe and last_pid. */
spinlock_t lastpid_lock = SPIN_LOCK_UNLOCKED;

/**
 * get_pid - 分配新PID（Linux2.4内核）
 * @flags: clone_flags，判断是否带CLONE_PID
 * 返回值：分配得到的pid；CLONE_PID时直接返回current->pid，不分配新PID
 *
 * 核心逻辑：递增last_pid，同时检查pid/pgrp/session冲突，保证全局唯一性；
 * 会跳过1~299，留给系统守护进程。
 * 注意：2.4的PID分配算法比较朴素，遍历任务链表查重；现代内核用IDR管理PID。
 * 
 * 想象内核手里有一个**计数器 last_pid**，用来发 PID 号，还有个安全标记 `next_safe`。
 * 规则：PID 从 300 开始发，0~299 留给系统后台守护进程。
 * 
 * 1. 来一个新建任务，计数器 `last_pid +=1`，直接把这个号给它。
 * 2. `next_safe` 记着：**在到达这个数字之前，号码肯定不会撞号，可以直接发号，不用挨个检查**。
 * 	只要 last_pid < next_safe：直接拿号走人，不做全系统扫描，速度很快。
 */
static int get_pid(unsigned long flags)
{
	// next_safe：缓存一个安全上限，last_pid到达该值后才需要全量扫描任务链表，优化性能
	static int next_safe = PID_MAX;
	struct task_struct *p;

	// CLONE_PID：复用当前进程pid，不分配新PID，用于SMP创建其他CPU的idle进程(pid=0)
	if (flags & CLONE_PID)
		return current->pid;

	// lastpid_lock：保护全局变量last_pid的自旋锁，防止多核并发争抢PID
	spin_lock(&lastpid_lock);

	// last_pid自增；如果超过0x7FFF（高16位非0），重置到300，跳过0~299预留给系统daemon
	if((++last_pid) & 0xffff8000) {
		last_pid = 300;		/* Skip daemons etc. */
		goto inside;
	}

	// 当last_pid到达next_safe，说明有可能出现PID冲突，需要扫描全任务链表校验
	if(last_pid >= next_safe) {
inside:
		next_safe = PID_MAX;  // 临时设置上限为PID_MAX，PID_MAX是系统PID最大值
		read_lock(&tasklist_lock);  // 读锁保护全局任务链表，防止遍历过程任务被删除
	repeat:
		// 遍历系统中所有task_struct
		for_each_task(p) {
			// 冲突判定：last_pid和任意task的 pid / pgrp / session 重复，都不能使用
			if(p->pid == last_pid	||
			   p->pgrp == last_pid	||
			   p->session == last_pid) {

				// PID冲突，last_pid+1，重新检查
				if(++last_pid >= next_safe) {

					// 超出上限，重置回300，从头继续找可用pid
					if(last_pid & 0xffff8000)
						last_pid = 300;
					next_safe = PID_MAX;
				}
				goto repeat;
			}
			/*
             * 记录比当前last_pid大的最小pid/pgrp/session，更新next_safe
             * 含义：下一次到next_safe之前，大概率不会冲突，不需要扫描全任务链表
             * 是一种优化手段，避免每次分配PID都遍历全部进程
			 * 
			 * 这里就是 next_safe 往下调整，保证只要 last_pid < next_safe：直接拿号走人，不做全系统扫描，速度很快。
             */
			if(p->pid > last_pid && next_safe > p->pid)
				next_safe = p->pid;
			if(p->pgrp > last_pid && next_safe > p->pgrp)
				next_safe = p->pgrp;
			if(p->session > last_pid && next_safe > p->session)
				next_safe = p->session;
		}
		read_unlock(&tasklist_lock);
	}
	spin_unlock(&lastpid_lock);

	return last_pid;
}

static inline int dup_mmap(struct mm_struct * mm)
{
	struct vm_area_struct * mpnt, *tmp, **pprev;
	int retval;

	flush_cache_mm(current->mm);
	mm->locked_vm = 0;
	mm->mmap = NULL;
	mm->mmap_avl = NULL;
	mm->mmap_cache = NULL;
	mm->map_count = 0;
	mm->cpu_vm_mask = 0;
	mm->swap_cnt = 0;
	mm->swap_address = 0;
	pprev = &mm->mmap;
	for (mpnt = current->mm->mmap ; mpnt ; mpnt = mpnt->vm_next) {
		struct file *file;

		retval = -ENOMEM;
		if(mpnt->vm_flags & VM_DONTCOPY)
			continue;
		tmp = kmem_cache_alloc(vm_area_cachep, SLAB_KERNEL);
		if (!tmp)
			goto fail_nomem;
		*tmp = *mpnt;
		tmp->vm_flags &= ~VM_LOCKED;
		tmp->vm_mm = mm;
		mm->map_count++;
		tmp->vm_next = NULL;
		file = tmp->vm_file;
		if (file) {
			struct inode *inode = file->f_dentry->d_inode;
			get_file(file);
			if (tmp->vm_flags & VM_DENYWRITE)
				atomic_dec(&inode->i_writecount);
      
			/* insert tmp into the share list, just after mpnt */
			spin_lock(&inode->i_mapping->i_shared_lock);
			if((tmp->vm_next_share = mpnt->vm_next_share) != NULL)
				mpnt->vm_next_share->vm_pprev_share =
					&tmp->vm_next_share;
			mpnt->vm_next_share = tmp;
			tmp->vm_pprev_share = &mpnt->vm_next_share;
			spin_unlock(&inode->i_mapping->i_shared_lock);
		}

		/* Copy the pages, but defer checking for errors */
		retval = copy_page_range(mm, current->mm, tmp);
		if (!retval && tmp->vm_ops && tmp->vm_ops->open)
			tmp->vm_ops->open(tmp);

		/*
		 * Link in the new vma even if an error occurred,
		 * so that exit_mmap() can clean up the mess.
		 */
		*pprev = tmp;
		pprev = &tmp->vm_next;

		if (retval)
			goto fail_nomem;
	}
	retval = 0;
	if (mm->map_count >= AVL_MIN_MAP_COUNT)
		build_mmap_avl(mm);

fail_nomem:
	flush_tlb_mm(current->mm);
	return retval;
}

spinlock_t mmlist_lock __cacheline_aligned = SPIN_LOCK_UNLOCKED;

#define allocate_mm()	(kmem_cache_alloc(mm_cachep, SLAB_KERNEL))
#define free_mm(mm)	(kmem_cache_free(mm_cachep, (mm)))

// 初始化新分配出来的mm_struct，fork拷贝地址空间时调用
static struct mm_struct * mm_init(struct mm_struct * mm)
{
    // mm_users：正在使用该地址空间的task数量，初始1
    atomic_set(&mm->mm_users, 1);
    // mm_count：mm结构体本身的引用计数（内核内部使用，和mm_users不一样）
    atomic_set(&mm->mm_count, 1);
    // 初始化mmap_sem信号量，保护vma链表（mmap/brk/mprotect等操作）
    init_MUTEX(&mm->mmap_sem);
    // 初始化页表自旋锁，保护页表并发修改
    mm->page_table_lock = SPIN_LOCK_UNLOCKED;
    // 分配页全局目录PGD（一级页表，x86 32位）
    mm->pgd = pgd_alloc();
    if (mm->pgd)
        return mm; // PGD分配成功，返回初始化好的mm_struct

    // PGD分配失败，释放mm_struct，返回NULL
    free_mm(mm);
    return NULL;
}

	

/*
 * Allocate and initialize an mm_struct.
 */
struct mm_struct * mm_alloc(void)
{
	struct mm_struct * mm;

	mm = allocate_mm();
	if (mm) {
		memset(mm, 0, sizeof(*mm));
		return mm_init(mm);
	}
	return NULL;
}

/*
 * Called when the last reference to the mm
 * is dropped: either by a lazy thread or by
 * mmput. Free the page directory and the mm.
 */
inline void __mmdrop(struct mm_struct *mm)
{
	if (mm == &init_mm) BUG();
	pgd_free(mm->pgd);
	destroy_context(mm);
	free_mm(mm);
}

/*
 * Decrement the use count and release all resources for an mm.
 */
void mmput(struct mm_struct *mm)
{
	if (atomic_dec_and_lock(&mm->mm_users, &mmlist_lock)) {
		list_del(&mm->mmlist);
		spin_unlock(&mmlist_lock);
		exit_mmap(mm);
		mmdrop(mm);
	}
}

/* Please note the differences between mmput and mm_release.
 * mmput is called whenever we stop holding onto a mm_struct,
 * error success whatever.
 *
 * mm_release is called after a mm_struct has been removed
 * from the current process.
 *
 * This difference is important for error handling, when we
 * only half set up a mm_struct for a new process and need to restore
 * the old one.  Because we mmput the new mm_struct before
 * restoring the old one. . .
 * Eric Biederman 10 January 1998
 */
void mm_release(void)
{
	struct task_struct *tsk = current;

	/* notify parent sleeping on vfork() */
	if (tsk->flags & PF_VFORK) {
		tsk->flags &= ~PF_VFORK;
		up(tsk->p_opptr->vfork_sem);
	}
}

// 拷贝/共享mm_struct虚拟内存描述符，clone_flags控制是否共享地址空间
static int copy_mm(unsigned long clone_flags, struct task_struct * tsk)
{
	struct mm_struct * mm, *oldmm;
	int retval;

	// 初始化新进程的缺页统计：次缺页、主缺页，以及swap换入统计
	tsk->min_flt = tsk->maj_flt = 0;
	tsk->cmin_flt = tsk->cmaj_flt = 0;
	tsk->nswap = tsk->cnswap = 0;

	// 先置空新任务的mm和active_mm
	tsk->mm = NULL;
	tsk->active_mm = NULL;

	/*
	 * Are we cloning a kernel thread?
	 *
	 * We need to steal a active VM for that..
	 */
	oldmm = current->mm;
	if (!oldmm)
		return 0;

	// CLONE_VM：共享虚拟地址空间（pthread线程使用）
	if (clone_flags & CLONE_VM) {
		atomic_inc(&oldmm->mm_users);
		mm = oldmm;
		goto good_mm;
	}

	//这里是新创建进程了

	retval = -ENOMEM;

	// 分配一个全新的mm_struct结构体
	mm = allocate_mm();
	if (!mm)
		goto fail_nomem;

	/* Copy the current MM stuff.. */
	// 把父mm_struct的基础成员拷贝到新mm
	memcpy(mm, oldmm, sizeof(*mm));

	// 初始化新mm的内部字段（信号、锁、引用计数等）
	if (!mm_init(mm))
		goto fail_nomem;

	// 加mmap_sem读写信号量，保护父进程vma链表，防止拷贝期间并发修改内存映射
	down(&oldmm->mmap_sem);

	// dup_mmap：核心！拷贝父进程所有vma虚拟内存区域，建立写时复制页表
	retval = dup_mmap(mm);

	up(&oldmm->mmap_sem);

	/*
	 * Add it to the mmlist after the parent.
	 *
	 * Doing it this way means that we can order
	 * the list, and fork() won't mess up the
	 * ordering significantly.
	 */
	// 全局mmlist链表，保存系统所有mm_struct，上锁后把新mm插入父mm后面
	spin_lock(&mmlist_lock);
	list_add(&mm->mmlist, &oldmm->mmlist);
	spin_unlock(&mmlist_lock);

	if (retval)
		goto free_pt;

	/*
	 * child gets a private LDT (if there was an LDT in the parent)
	 */

	// 拷贝段描述符LDT，x86下的局部描述符表
	copy_segments(tsk, mm);

	// 初始化硬件上下文（x86的MMU上下文）
	if (init_new_context(tsk,mm))
		goto free_pt;

good_mm:
	tsk->mm = mm;
	tsk->active_mm = mm;
	return 0;

free_pt:
	mmput(mm);
fail_nomem:
	return retval;
}

// 复制一份新的fs_struct文件系统上下文，fork创建独立fs时调用
static inline struct fs_struct *__copy_fs_struct(struct fs_struct *old)
{
	// 从slab缓存分配fs_struct对象
	struct fs_struct *fs = kmem_cache_alloc(fs_cachep, GFP_KERNEL);
	/* We don't need to lock fs - think why ;-) */
	if (fs) {
		atomic_set(&fs->count, 1);
		fs->lock = RW_LOCK_UNLOCKED;
		fs->umask = old->umask;
		read_lock(&old->lock);

		//直接拷贝这些文件系统信息
		fs->rootmnt = mntget(old->rootmnt);
		fs->root = dget(old->root);
		fs->pwdmnt = mntget(old->pwdmnt);
		fs->pwd = dget(old->pwd);
		if (old->altroot) {
			fs->altrootmnt = mntget(old->altrootmnt);
			fs->altroot = dget(old->altroot);
		} else {
			fs->altrootmnt = NULL;
			fs->altroot = NULL;
		}	
		read_unlock(&old->lock);
	}
	return fs;
}

struct fs_struct *copy_fs_struct(struct fs_struct *old)
{
	return __copy_fs_struct(old);
}

// 拷贝/共享文件系统上下文信息（根目录、当前工作目录、umask）
static inline int copy_fs(unsigned long clone_flags, struct task_struct * tsk)
{
	// 判断CLONE_FS标记：共享fs_struct，多用于线程
	if (clone_flags & CLONE_FS) {
		// 共享模式：仅原子增加fs_struct引用计数，不复制结构体
		atomic_inc(&current->fs->count);
		return 0;
	}

	// 非共享，调用__copy_fs_struct复制一份全新的fs_struct
	tsk->fs = __copy_fs_struct(current->fs);
	if (!tsk->fs)
		return -1;
	return 0;
}

static int count_open_files(struct files_struct *files, int size)
{
	int i;
	
	/* Find the last open fd */
	for (i = size/(8*sizeof(long)); i > 0; ) {
		if (files->open_fds->fds_bits[--i])
			break;
	}
	i = (i+1) * 8 * sizeof(long);
	return i;
}

// 拷贝/共享文件描述符表，clone_flags决定是共享还是复制；tsk是新创建任务
static int copy_files(unsigned long clone_flags, struct task_struct * tsk)
{
	struct files_struct *oldf, *newf;
	struct file **old_fds, **new_fds;
	int open_files, nfds, size, i, error = 0;

	/*
	 * A background process may not have any files ...
	 */
	// oldf：当前父进程(current)的文件描述符表
	oldf = current->files;
	if (!oldf)
		goto out;

	// 判断标记：CLONE_FILES 表示共享文件描述符（pthread线程就带这个flag）
	// 线程创建带 CLONE_FILES 标记
	if (clone_flags & CLONE_FILES) {

		// 共享：仅原子增加files_struct引用计数，不拷贝数据
		atomic_inc(&oldf->count);
		goto out;
	}

	// 走到这里：不共享，需要完整复制一份新的files_struct
	tsk->files = NULL;
	error = -ENOMEM;

	// 从slab缓存分配全新files_struct对象
	newf = kmem_cache_alloc(files_cachep, SLAB_KERNEL);
	if (!newf) 
		goto out;

	// 新的fd表引用计数初始化为1，只有当前新任务持有
	atomic_set(&newf->count, 1);

	newf->file_lock	    = RW_LOCK_UNLOCKED;   // 读写锁，保护fd表并发访问
	newf->next_fd	    = 0;  // 下一个可用fd号
	newf->max_fds	    = NR_OPEN_DEFAULT;  // fd数组默认最大数量
	newf->max_fdset	    = __FD_SETSIZE;  // fd_set位图最大位数

	//先使用静态的，不用分配内存
	newf->close_on_exec = &newf->close_on_exec_init;  // exec时自动关闭的fd位图
	newf->open_fds	    = &newf->open_fds_init;  // 标记fd是否打开的位图
	newf->fd	    = &newf->fd_array[0];  // file*指针数组，存放每个fd对应的struct file*

	/* We don't yet have the oldf readlock, but even if the old
           fdset gets grown now, we'll only copy up to "size" fds */
	size = oldf->max_fdset;
	// 如果父进程fd位图超过默认大小，则扩展新files的fdset
	if (size > __FD_SETSIZE) {
		newf->max_fdset = 0;
		write_lock(&newf->file_lock);
		error = expand_fdset(newf, size);  // 扩容open_fds、close_on_exec位图
		write_unlock(&newf->file_lock);
		if (error)
			goto out_release;
	}
	// 加读锁保护父进程fd表，防止拷贝期间父进程并发修改fd
	read_lock(&oldf->file_lock);

	// 统计父进程当前打开的fd总数
	open_files = count_open_files(oldf, size);

	/*
	 * Check whether we need to allocate a larger fd array.
	 * Note: we're not a clone task, so the open count won't
	 * change.
	 */
	nfds = NR_OPEN_DEFAULT;
	// 如果打开文件数量超过默认fd数组大小，扩容fd指针数组
	if (open_files > nfds) {
		read_unlock(&oldf->file_lock);
		newf->max_fds = 0;
		write_lock(&newf->file_lock);
		error = expand_fd_array(newf, open_files);
		write_unlock(&newf->file_lock);
		if (error) 
			goto out_release;
		nfds = newf->max_fds;
		read_lock(&oldf->file_lock);
	}

	old_fds = oldf->fd;
	new_fds = newf->fd;

	// 拷贝open_fds位图（标记哪些fd是打开的）
	memcpy(newf->open_fds->fds_bits, oldf->open_fds->fds_bits, open_files/8);

	// 拷贝close_on_exec位图（标记exec时需要自动关闭的fd）
	memcpy(newf->close_on_exec->fds_bits, oldf->close_on_exec->fds_bits, open_files/8);

	// 循环拷贝每一个打开的struct file指针，并增加file引用计数
	for (i = open_files; i != 0; i--) {
		struct file *f = *old_fds++;
		if (f)
			get_file(f);
		*new_fds++ = f;  // 增加struct file的f_count引用计数，文件不会被提前释放
	}
	read_unlock(&oldf->file_lock);  // 释放父fd表读锁

	/* compute the remainder to be cleared */
	// fd数组剩下未使用的位置清零
	size = (newf->max_fds - open_files) * sizeof(struct file *);

	/* This is long word aligned thus could use a optimized version */ 
	memset(new_fds, 0, size); 

	// fd位图剩余bit清零
	if (newf->max_fdset > open_files) {
		int left = (newf->max_fdset-open_files)/8;
		int start = open_files / (8 * sizeof(unsigned long));
		
		memset(&newf->open_fds->fds_bits[start], 0, left);
		memset(&newf->close_on_exec->fds_bits[start], 0, left);
	}

	// 将新任务的files指向新拷贝出来的files_struct
	tsk->files = newf;
	error = 0;
out:
	return error;

out_release:
	free_fdset (newf->close_on_exec, newf->max_fdset);
	free_fdset (newf->open_fds, newf->max_fdset);
	kmem_cache_free(files_cachep, newf);
	goto out;
}

// 拷贝/共享信号处理表signal_struct，fork/clone时调用
static inline int copy_sighand(unsigned long clone_flags, struct task_struct * tsk)
{
	struct signal_struct *sig;

	// CLONE_SIGHAND：共享信号处理函数表（线程组，pthread_create带此标记）
	if (clone_flags & CLONE_SIGHAND) {
		atomic_inc(&current->sig->count);
		return 0;
	}

	// 不共享，分配全新signal_struct
	sig = kmem_cache_alloc(sigact_cachep, GFP_KERNEL);
	tsk->sig = sig;
	if (!sig)
		return -1;
	spin_lock_init(&sig->siglock);
	atomic_set(&sig->count, 1);

	// 拷贝信号处理action数组：每个信号对应的handler、sa_flags等信息
	// 说明fork的时候，信号处理函数保持一致
	memcpy(tsk->sig->action, current->sig->action, sizeof(tsk->sig->action));
	return 0;
}

static inline void copy_flags(unsigned long clone_flags, struct task_struct *p)
{
	unsigned long new_flags = p->flags;

	new_flags &= ~(PF_SUPERPRIV | PF_USEDFPU | PF_VFORK);
	new_flags |= PF_FORKNOEXEC;
	if (!(clone_flags & CLONE_PTRACE))
		p->ptrace = 0;
	if (clone_flags & CLONE_VFORK)
		new_flags |= PF_VFORK;
	p->flags = new_flags;
}

/*
 *  Ok, this is the main fork-routine. It copies the system process
 * information (task[nr]) and sets up the necessary registers. It also
 * copies the data segment in its entirety.  The "stack_start" and
 * "stack_top" arguments are simply passed along to the platform
 * specific copy_thread() routine.  Most platforms ignore stack_top.
 * For an example that's using stack_top, see
 * arch/ia64/kernel/process.c.
 */
/**
 * do_fork - 创建新进程/轻量线程，fork/clone/vfork共用核心函数（Linux2.4）
 * @clone_flags: 克隆标志位(CLONE_*)，控制父子资源共享；低8位为子退出时发给父的信号(通常SIGCHLD)
 * @stack_start: 用户态新栈起始地址，pthread创建线程时传入用户栈；fork/vfork为0，复用父栈
 * @regs: pt_regs，保存陷入内核时的寄存器上下文，用于子进程返回用户态
 * @stack_size: 用户栈大小，clone创建线程时指定栈大小，fork/vfork该参数为0
 *
 * 返回值：成功返回新任务PID；失败返回负错误码
 *
 * 说明：sys_fork/sys_clone/sys_vfork系统调用入口，全部调用do_fork。
 * 根据clone_flags决定是创建独立进程、vfork进程、POSIX线程(CLONE_THREAD)。
 * 核心逻辑：分配task_struct -> 拷贝/共享资源(文件、fs、信号、内存) -> 设置线程上下文、PID、调度参数
 *          -> 加入全局任务链表/pidhash -> 唤醒子任务；vfork场景父进程阻塞等待子exec/exit
 */
int do_fork(unsigned long clone_flags, unsigned long stack_start,
	    struct pt_regs *regs, unsigned long stack_size)
{
	int retval = -ENOMEM;
	struct task_struct *p;

	// 定义vfork用的信号量，CLONE_VFORK时父进程在此信号量上阻塞
	DECLARE_MUTEX_LOCKED(sem);

	// CLONE_PID：仅内核启动0号idle进程允许使用，普通进程禁止
	// 设置 CLONE_PID：新创建的子 task，和父 task 拥有相同的 pid（pid=0）
	// SMP 多核启动，为其他 CPU 创建各自独立的 idle 0 号进程
	if (clone_flags & CLONE_PID) {
		/* This is only allowed from the boot up thread */
		if (current->pid)
			return -EPERM;
	}
	
	// 将vfork信号量挂到当前进程，子exit/exec时会up这个sem唤醒父
	current->vfork_sem = &sem;

	// 分配task_struct任务描述符
	p = alloc_task_struct();
	if (!p)
		goto fork_out;

	// 浅拷贝父task_struct所有成员，后面根据clone_flags选择性覆盖/共享资源
	*p = *current;

	retval = -EAGAIN;
	// 用户的进程数限制RLIMIT_NPROC：该user已达最大进程数，禁止fork
	if (atomic_read(&p->user->processes) >= p->rlim[RLIMIT_NPROC].rlim_cur)
		goto bad_fork_free;
	
	// user结构体引用计数+1，用户下进程计数+1
	atomic_inc(&p->user->__count);
	atomic_inc(&p->user->processes);  //任务数目增加了

	/*
	 * Counter increases are protected by
	 * the kernel lock so nr_threads can't
	 * increase under us (but it may decrease).
	 */
	// 全局总线程数上限max_threads，超过直接拒绝
	if (nr_threads >= max_threads)
		goto bad_fork_cleanup_count;
	
	// 增加exec_domain引用计数
	get_exec_domain(p->exec_domain);

	// 如果子进程的二进制加载器（binfmt）是内核模块，就增加这个模块的引用计数，阻止用户执行 `rmmod` 卸载该模块
	// 只要还有进程在用这个 binfmt，模块不能被删掉。
	if (p->binfmt && p->binfmt->module)
		__MOD_INC_USE_COUNT(p->binfmt->module);

	p->did_exec = 0; // 子进程尚未执行exec
	p->swappable = 0;  // 临时标记：task不可换出，初始化阶段
	p->state = TASK_UNINTERRUPTIBLE;  // 初始状态：不可中断睡眠，防止初始化未完成被调度

	copy_flags(clone_flags, p);  // 拷贝进程标志位，设置p->flags

	// 获取新PID；CLONE_THREAD线程组内线程TID独立，tgid和主线程相同
	// 一般就是下一个id
	p->pid = get_pid(clone_flags);

	// 初始化运行链表节点
	p->run_list.next = NULL;
	p->run_list.prev = NULL;

	// 设置父子指针 p_pptr(父)、p_opptr(原始父)，ptrace追踪会改变父指针
	if ((clone_flags & CLONE_VFORK) || !(clone_flags & CLONE_PARENT)) {
		p->p_opptr = current;
		if (!(p->ptrace & PT_PTRACED))
			p->p_pptr = current;
	}
	// 子链表置空
	p->p_cptr = NULL;
	init_waitqueue_head(&p->wait_chldexit);  // 子退出等待队列初始化
	p->vfork_sem = NULL;  // 子进程不持有vfork信号量，信号量属于父进程栈
	spin_lock_init(&p->alloc_lock);

	p->sigpending = 0;
	init_sigpending(&p->pending);  // 初始化待处理信号

	// 初始化定时器：itimer间隔定时器
	p->it_real_value = p->it_virt_value = p->it_prof_value = 0;
	p->it_real_incr = p->it_virt_incr = p->it_prof_incr = 0;
	init_timer(&p->real_timer);
	p->real_timer.data = (unsigned long) p;

	p->leader = 0;		/* session leadership doesn't inherit */
	p->tty_old_pgrp = 0;

	// 初始化时间统计，子进程初始CPU时间清零
	p->times.tms_utime = p->times.tms_stime = 0;
	p->times.tms_cutime = p->times.tms_cstime = 0;
#ifdef CONFIG_SMP
	{
		int i;
		p->has_cpu = 0;
		p->processor = current->processor;
		/* ?? should we just memset this ?? */
		for(i = 0; i < smp_num_cpus; i++)
			p->per_cpu_utime[i] = p->per_cpu_stime[i] = 0;
		spin_lock_init(&p->sigmask_lock);
	}
#endif
	p->lock_depth = -1;		/* -1 = no lock，BKL锁深度初始无锁 */
	p->start_time = jiffies;  // 任务创建时间，内核滴答数

	retval = -ENOMEM;
	/* copy all the process information */
	// 资源拷贝函数：依据clone_flags决定【拷贝新副本】or【共享父资源】
	if (copy_files(clone_flags, p))  // 文件描述符表 files_struct
		goto bad_fork_cleanup;
	if (copy_fs(clone_flags, p))  // fs_struct：根目录、cwd、umask
		goto bad_fork_cleanup_files;

	// fork的时候，信号处理函数保持一致
	if (copy_sighand(clone_flags, p))  // 信号处理表 sighand_struct
		goto bad_fork_cleanup_fs;
	
	if (copy_mm(clone_flags, p))  // 地址空间 mm_struct（CLONE_VM则共享）
		goto bad_fork_cleanup_sighand;

	// 拷贝线程上下文：栈、寄存器，准备子进程调度返回；
    // 子进程在此处会看到返回值0，父看到pid
	// 主要拷贝 struct thread_struct thread;
	retval = copy_thread(0, clone_flags, stack_start, stack_size, p, regs);
	if (retval)
		goto bad_fork_cleanup_sighand;
	p->semundo = NULL;
	
	/* Our parent execution domain becomes current domain
	   These must match for thread signalling to apply */
	   
	//父任务的执行id
	p->parent_exec_id = p->self_exec_id;

	/* ok, now we should be set up.. */
	p->swappable = 1;  // 初始化完成，允许task换出
	p->exit_signal = clone_flags & CSIGNAL;  // 子退出发给父的信号，fork为SIGCHLD
	p->pdeath_signal = 0;

	/*
	 * "share" dynamic priority between parent and child, thus the
	 * total amount of dynamic priorities in the system doesnt change,
	 * more scheduling fairness. This is only important in the first
	 * timeslice, on the long run the scheduling behaviour is unchanged.
	 */
	// 调度时间片均分：父子平分当前父剩余时间片，保证调度公平
	p->counter = (current->counter + 1) >> 1;
	current->counter >>= 1;
	if (!current->counter)
		current->need_resched = 1;  // 父时间片耗尽，标记需要重新调度

	/*
	 * Ok, add it to the run-queues and make it
	 * visible to the rest of the system.
	 *
	 * Let it rip!
	 */
	retval = p->pid;
	p->tgid = retval;  // 默认线程组TGID=PID（独立进程），如果是线程，下面设置回来
	INIT_LIST_HEAD(&p->thread_group);
	write_lock_irq(&tasklist_lock);  // tasklist全局大锁，保护任务链表、pidhash
	if (clone_flags & CLONE_THREAD) {
		// POSIX线程：TGID继承主线程tgid，加入主线程的thread_group链表
		p->tgid = current->tgid;
		list_add(&p->thread_group, &current->thread_group);
	}
	SET_LINKS(p);  // 把任务加入全局task链表
	hash_pid(p);  // 将task挂载到pidhash哈希表，通过pid查找task
	nr_threads++;  // 全局线程计数+1
	write_unlock_irq(&tasklist_lock);

	if (p->ptrace & PT_PTRACED)
		send_sig(SIGSTOP, p, 1);  // 如果被ptrace追踪，新建任务先发送SIGSTOP暂停

	wake_up_process(p);		/* do this last 唤醒子进程，放到就绪队列 */
	++total_forks;

fork_out:
	// CLONE_VFORK场景：父进程阻塞，直到子进程调用exec或exit，up(&sem)唤醒父
	// current->vfork_sem = NULL; 在 mm_release 中清理
	if ((clone_flags & CLONE_VFORK) && (retval > 0)) 
		down(&sem);
	return retval;

	// 下面是错误回滚分支，逆序释放前面分配的资源
bad_fork_cleanup_sighand:
	exit_sighand(p);
bad_fork_cleanup_fs:
	exit_fs(p); /* blocking */
bad_fork_cleanup_files:
	exit_files(p); /* blocking */
bad_fork_cleanup:
	put_exec_domain(p->exec_domain);
	if (p->binfmt && p->binfmt->module)
		__MOD_DEC_USE_COUNT(p->binfmt->module);
bad_fork_cleanup_count:
	atomic_dec(&p->user->processes);
	free_uid(p->user);
bad_fork_free:
	free_task_struct(p);
	goto fork_out;
}

/* SLAB cache for signal_struct structures (tsk->sig) */
kmem_cache_t *sigact_cachep;

/* SLAB cache for files_struct structures (tsk->files) */
kmem_cache_t *files_cachep;

/* SLAB cache for fs_struct structures (tsk->fs) */
kmem_cache_t *fs_cachep;

/* SLAB cache for vm_area_struct structures */
kmem_cache_t *vm_area_cachep;

/* SLAB cache for mm_struct structures (tsk->mm) */
kmem_cache_t *mm_cachep;

void __init proc_caches_init(void)
{
	sigact_cachep = kmem_cache_create("signal_act",
			sizeof(struct signal_struct), 0,
			SLAB_HWCACHE_ALIGN, NULL, NULL);
	if (!sigact_cachep)
		panic("Cannot create signal action SLAB cache");

	files_cachep = kmem_cache_create("files_cache", 
			 sizeof(struct files_struct), 0, 
			 SLAB_HWCACHE_ALIGN, NULL, NULL);
	if (!files_cachep) 
		panic("Cannot create files SLAB cache");

	fs_cachep = kmem_cache_create("fs_cache", 
			 sizeof(struct fs_struct), 0, 
			 SLAB_HWCACHE_ALIGN, NULL, NULL);
	if (!fs_cachep) 
		panic("Cannot create fs_struct SLAB cache");
 
	vm_area_cachep = kmem_cache_create("vm_area_struct",
			sizeof(struct vm_area_struct), 0,
			SLAB_HWCACHE_ALIGN, NULL, NULL);
	if(!vm_area_cachep)
		panic("vma_init: Cannot alloc vm_area_struct SLAB cache");

	mm_cachep = kmem_cache_create("mm_struct",
			sizeof(struct mm_struct), 0,
			SLAB_HWCACHE_ALIGN, NULL, NULL);
	if(!mm_cachep)
		panic("vma_init: Cannot alloc mm_struct SLAB cache");
}
