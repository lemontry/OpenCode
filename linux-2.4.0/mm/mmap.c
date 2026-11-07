/*
 *	linux/mm/mmap.c
 *
 * Written by obz.
 */
#include <linux/slab.h>
#include <linux/shm.h>
#include <linux/mman.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include <linux/swapctl.h>
#include <linux/smp_lock.h>
#include <linux/init.h>
#include <linux/file.h>

#include <asm/uaccess.h>
#include <asm/pgalloc.h>

/* description of effects of mapping type and prot in current implementation.
 * this is due to the limited x86 page protection hardware.  The expected
 * behavior is in parens:
 *
 * map_type	prot
 *		PROT_NONE	PROT_READ	PROT_WRITE	PROT_EXEC
 * MAP_SHARED	r: (no) no	r: (yes) yes	r: (no) yes	r: (no) yes
 *		w: (no) no	w: (no) no	w: (yes) yes	w: (no) no
 *		x: (no) no	x: (no) yes	x: (no) yes	x: (yes) yes
 *		
 * MAP_PRIVATE	r: (no) no	r: (yes) yes	r: (no) yes	r: (no) yes
 *		w: (no) no	w: (no) no	w: (copy) copy	w: (no) no
 *		x: (no) no	x: (no) yes	x: (no) yes	x: (yes) yes
 *
 */
pgprot_t protection_map[16] = {
	__P000, __P001, __P010, __P011, __P100, __P101, __P110, __P111,
	__S000, __S001, __S010, __S011, __S100, __S101, __S110, __S111
};

int sysctl_overcommit_memory;

/* Check that a process has enough memory to allocate a
 * new virtual mapping.
 */
int vm_enough_memory(long pages)
{
	/* Stupid algorithm to decide if we have enough memory: while
	 * simple, it hopefully works in most obvious cases.. Easy to
	 * fool it, but this should catch most mistakes.
	 */
	/* 23/11/98 NJC: Somewhat less stupid version of algorithm,
	 * which tries to do "TheRightThing".  Instead of using half of
	 * (buffers+cache), use the minimum values.  Allow an extra 2%
	 * of num_physpages for safety margin.
	 */

	long free;
	
        /* Sometimes we want to use more memory than we have. */
	if (sysctl_overcommit_memory)
	    return 1;

	free = atomic_read(&buffermem_pages);
	free += atomic_read(&page_cache_size);
	free += nr_free_pages();
	free += nr_swap_pages;
	return free > pages;
}

/* Remove one vm structure from the inode's i_mapping address space. */
static inline void __remove_shared_vm_struct(struct vm_area_struct *vma)
{
	struct file * file = vma->vm_file;

	if (file) {
		struct inode *inode = file->f_dentry->d_inode;
		if (vma->vm_flags & VM_DENYWRITE)
			atomic_inc(&inode->i_writecount);
		if(vma->vm_next_share)
			vma->vm_next_share->vm_pprev_share = vma->vm_pprev_share;
		*vma->vm_pprev_share = vma->vm_next_share;
	}
}

static inline void remove_shared_vm_struct(struct vm_area_struct *vma)
{
	lock_vma_mappings(vma);
	__remove_shared_vm_struct(vma);
	unlock_vma_mappings(vma);
}

/**
 * lock_vma_mappings - 锁定VMA对应的address_space共享链表锁，Linux2.4
 * @vma：待操作的虚拟内存区域
 */
void lock_vma_mappings(struct vm_area_struct *vma)
{
    struct address_space *mapping;

    mapping = NULL;
    // 如果是文件映射，取出inode对应的address_space
    if (vma->vm_file)
        mapping = vma->vm_file->f_dentry->d_inode->i_mapping;
    // 存在address_space，则获取它的i_shared_lock自旋锁
    if (mapping)
        spin_lock(&mapping->i_shared_lock);
}

void unlock_vma_mappings(struct vm_area_struct *vma)
{
	struct address_space *mapping;

	mapping = NULL;
	if (vma->vm_file)
		mapping = vma->vm_file->f_dentry->d_inode->i_mapping;
	if (mapping)
		spin_unlock(&mapping->i_shared_lock);
}

/*
 *  sys_brk() for the most part doesn't need the global kernel
 *  lock, except when an application is doing something nasty
 *  like trying to un-brk an area that has already been mapped
 *  to a regular file.  in this case, the unmapping will need
 *  to invoke file system routines that need the global lock.
 */
/**
 * sys_brk - brk系统调用实现，修改进程program break（堆上边界），Linux2.4
 * @brk: 用户传入目标断点地址
 * 返回值: 修改后mm->brk的值；失败返回当前brk（不会报错返回负数，由用户态判断）
 */
asmlinkage unsigned long sys_brk(unsigned long brk)
{
    unsigned long rlim, retval;
    unsigned long newbrk, oldbrk;
    struct mm_struct *mm = current->mm;

    // 获取mmap信号量，保护VMA链表操作
    down(&mm->mmap_sem);

    // 目标地址小于代码段结束地址，非法，直接退出
    if (brk < mm->end_code)
        goto out;
    // 对齐到页边界
    newbrk = PAGE_ALIGN(brk);
    // 当前brk对齐到页边界
    oldbrk = PAGE_ALIGN(mm->brk);
    // 页对齐之后没有变化，只需要更新mm->brk，无需修改VMA
    if (oldbrk == newbrk)
        goto set_brk;

    /* Always allow shrinking brk. */
    // 分支：缩小堆（brk变小），调用do_munmap释放中间虚拟地址区间
    if (brk <= mm->brk) {
        if (!do_munmap(mm, newbrk, oldbrk-newbrk))
            goto set_brk;
        goto out;
    }

    /* Check against rlimit.. */
    // 检查RLIMIT_DATA资源限制：数据段最大长度
    rlim = current->rlim[RLIMIT_DATA].rlim_cur;
    if (rlim < RLIM_INFINITY && brk - mm->start_data > rlim)
        goto out;

    /* Check against existing mmap mappings. */
    // 检查目标扩展区间会不会和已经存在的VMA重叠（防止和mmap区域冲突）
    if (find_vma_intersection(mm, oldbrk, newbrk+PAGE_SIZE))
        goto out;

    /* Check if we have enough memory.. */
    // 粗略检查系统是否有足够空闲内存
    if (!vm_enough_memory((newbrk-oldbrk) >> PAGE_SHIFT))
        goto out;

    /* Ok, looks good - let it rip. */
    // 分支：扩展堆，调用do_brk创建/扩展堆VMA
    if (do_brk(oldbrk, newbrk-oldbrk) != oldbrk)
        goto out;
set_brk:
    // 更新进程的brk断点（保存用户原始地址，不是页对齐地址）
    mm->brk = brk;
out:
    // 读出最终的brk作为返回值
    retval = mm->brk;
    up(&mm->mmap_sem);
    return retval;
}

/* Combine the mmap "prot" and "flags" argument into one "vm_flags" used
 * internally. Essentially, translate the "PROT_xxx" and "MAP_xxx" bits
 * into "VM_xxx".
 */
static inline unsigned long vm_flags(unsigned long prot, unsigned long flags)
{
#define _trans(x,bit1,bit2) \
((bit1==bit2)?(x&bit1):(x&bit1)?bit2:0)

	unsigned long prot_bits, flag_bits;
	prot_bits =
		_trans(prot, PROT_READ, VM_READ) |
		_trans(prot, PROT_WRITE, VM_WRITE) |
		_trans(prot, PROT_EXEC, VM_EXEC);
	flag_bits =
		_trans(flags, MAP_GROWSDOWN, VM_GROWSDOWN) |
		_trans(flags, MAP_DENYWRITE, VM_DENYWRITE) |
		_trans(flags, MAP_EXECUTABLE, VM_EXECUTABLE);
	return prot_bits | flag_bits;
#undef _trans
}

/**
 * do_mmap_pgoff - mmap核心实现，创建VMA并完成映射准备，Linux2.4
 * @file: 待映射的文件对象，NULL代表匿名映射
 * @addr: 用户期望的起始虚拟地址
 * @len: 映射长度（字节）
 * @prot: 页面保护权限 PROT_READ / PROT_WRITE / PROT_EXEC
 * @flags: mmap标志 MAP_PRIVATE / MAP_SHARED / MAP_FIXED / MAP_ANONYMOUS
 * @pgoff: 文件内偏移，单位：页
 * 返回值: 成功返回映射起始虚拟地址；失败返回负错误码
 */
unsigned long do_mmap_pgoff(struct file * file, unsigned long addr, unsigned long len,
    unsigned long prot, unsigned long flags, unsigned long pgoff)
{
    struct mm_struct * mm = current->mm;
    struct vm_area_struct * vma;
    int correct_wcount = 0;
    int error;

    // 如果是文件映射，必须实现f_op->mmap回调
    if (file && (!file->f_op || !file->f_op->mmap))
        return -ENODEV;

    // 长度页对齐；对齐后长度为0直接返回
    if ((len = PAGE_ALIGN(len)) == 0)
        return addr;

    // 映射范围不能超过用户态地址空间上限TASK_SIZE
    if (len > TASK_SIZE || addr > TASK_SIZE-len)
        return -EINVAL;

    // 检查页偏移溢出：偏移+长度页计数后回绕
    if ((pgoff + (len >> PAGE_SHIFT)) < pgoff)
        return -EINVAL;

    // VMA数量超出上限
    if (mm->map_count > MAX_MAP_COUNT)
        return -ENOMEM;

    // 如果进程开启MCL_FUTURE，检查锁内存资源限制RLIMIT_MEMLOCK
    if (mm->def_flags & VM_LOCKED) {
        unsigned long locked = mm->locked_vm << PAGE_SHIFT;
        locked += len;
        if (locked > current->rlim[RLIMIT_MEMLOCK].rlim_cur)
            return -EAGAIN;
    }

    /* Do simple checking here so the lower-level routines won't have
     * to. we assume access permissions have been handled by the open
     * of the memory object, so we don't do any here.
     */
    // 文件映射权限校验
    if (file != NULL) {
        switch (flags & MAP_TYPE) {
        case MAP_SHARED:
            // 共享可写映射，文件必须支持写
            if ((prot & PROT_WRITE) && !(file->f_mode & FMODE_WRITE))
                return -EACCES;

            // 追加模式文件，禁止共享写映射
            if (IS_APPEND(file->f_dentry->d_inode) && (file->f_mode & FMODE_WRITE))
                return -EACCES;

            // 文件存在强制锁，不能映射
            if (locks_verify_locked(file->f_dentry->d_inode))
                return -EAGAIN;

            /* fall through */
        case MAP_PRIVATE:
            // 私有映射，文件必须可读
            if (!(file->f_mode & FMODE_READ))
                return -EACCES;
            break;

        default:
            return -EINVAL;
        }
    }

    /* Obtain the address to map to. we verify (or select) it and ensure
     * that it represents a valid section of the address space.
     */
    if (flags & MAP_FIXED) {
        // MAP_FIXED：指定地址必须页对齐
        if (addr & ~PAGE_MASK)
            return -EINVAL;
    } else {
        // 不指定地址，内核自动找一块空闲虚拟区间
        addr = get_unmapped_area(addr, len);
        if (!addr)
            return -ENOMEM;
    }

    /* Determine the object being mapped and call the appropriate
     * specific mapper. the address has already been validated, but
     * not unmapped, but the maps are removed from the list.
     */
    // 从slab分配VMA结构体
    vma = kmem_cache_alloc(vm_area_cachep, SLAB_KERNEL);
    if (!vma)
        return -ENOMEM;

    // 初始化VMA基础成员
    vma->vm_mm = mm;
    vma->vm_start = addr;
    vma->vm_end = addr + len;
    vma->vm_flags = vm_flags(prot,flags) | mm->def_flags;

    if (file) {
        VM_ClearReadHint(vma);
        vma->vm_raend = 0;

        // 设置后续可修改权限标志
        if (file->f_mode & FMODE_READ)
            vma->vm_flags |= VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC;
        if (flags & MAP_SHARED) {
            vma->vm_flags |= VM_SHARED | VM_MAYSHARE;

            /* This looks strange, but when we don't have the file open
             * for writing, we can demote the shared mapping to a simpler
             * private mapping. That also takes care of a security hole
             * with ptrace() writing to a shared mapping without write
             * permissions.
             *
             * We leave the VM_MAYSHARE bit on, just to get correct output
             * from /proc/xxx/maps..
             */
            // 文件不可写，降级：去掉VM_SHARED、VM_MAYWRITE，保留VM_MAYSHARE用于/proc显示
            if (!(file->f_mode & FMODE_WRITE))
                vma->vm_flags &= ~(VM_MAYWRITE | VM_SHARED);
        }
    } else {
        // 匿名映射
        vma->vm_flags |= VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC;
        if (flags & MAP_SHARED)
            vma->vm_flags |= VM_SHARED | VM_MAYSHARE;
    }
    vma->vm_page_prot = protection_map[vma->vm_flags & 0x0f];
    vma->vm_ops = NULL;
    vma->vm_pgoff = pgoff;
    vma->vm_file = NULL;
    vma->vm_private_data = NULL;

    /* Clear old maps */
    error = -ENOMEM;
    // MAP_FIXED场景：清除该区间原有VMA映射
    if (do_munmap(mm, addr, len))
        goto free_vma;

    /* Check against address space limit. */
    // 检查进程总虚拟内存RLIMIT_AS限制
    if ((mm->total_vm << PAGE_SHIFT) + len
        > current->rlim[RLIMIT_AS].rlim_cur)
        goto free_vma;

    /* Private writable mapping? Check memory availability.. */
    // 私有可写映射、非MAP_NORESERVE：预校验系统内存余量
    if ((vma->vm_flags & (VM_SHARED | VM_WRITE)) == VM_WRITE &&
        !(flags & MAP_NORESERVE)                 &&
        !vm_enough_memory(len >> PAGE_SHIFT))
        goto free_vma;

    if (file) {
        // DENYWRITE：阻止其他进程对该文件写
        if (vma->vm_flags & VM_DENYWRITE) {
            error = deny_write_access(file);
            if (error)
                goto free_vma;
            correct_wcount = 1;
        }
        vma->vm_file = file;
        get_file(file);
        // 调用文件/驱动自定义mmap回调（设备驱动、文件系统在这里注册vm_ops）
        error = file->f_op->mmap(file, vma);
        if (error)
            goto unmap_and_free_vma;
    } else if (flags & MAP_SHARED) {
        // 共享匿名映射，走shmem建立共享内存
        error = shmem_zero_setup(vma);
        if (error)
            goto free_vma;
    }

    /* Can addr have changed??
     *
     * Answer: Yes, several device drivers can do it in their
     *         f_op->mmap method. -DaveM
     */
    // 驱动mmap回调有可能修改vma->vm_start，同步更新
    flags = vma->vm_flags;
    addr = vma->vm_start;

    // 把VMA插入mm的链表、AVL树
    insert_vm_struct(mm, vma);
    if (correct_wcount)
        atomic_inc(&file->f_dentry->d_inode->i_writecount);
    
    // 统计进程虚拟页数
    mm->total_vm += len >> PAGE_SHIFT;
    // 锁页：预分配物理页，不延迟缺页
    if (flags & VM_LOCKED) {
        mm->locked_vm += len >> PAGE_SHIFT;
        make_pages_present(addr, addr + len);
    }
    return addr;

unmap_and_free_vma:
    if (correct_wcount)
        atomic_inc(&file->f_dentry->d_inode->i_writecount);
    vma->vm_file = NULL;
    fput(file);
    /* Undo any partial mapping done by a device driver. */
    flush_cache_range(mm, vma->vm_start, vma->vm_end);
    zap_page_range(mm, vma->vm_start, vma->vm_end - vma->vm_start);
    flush_tlb_range(mm, vma->vm_start, vma->vm_end);
free_vma:
    kmem_cache_free(vm_area_cachep, vma);
    return error;
}

/* Get an address range which is currently unmapped.
 * For mmap() without MAP_FIXED and shmat() with addr=0.
 * Return value 0 means ENOMEM.
 */
#ifndef HAVE_ARCH_UNMAPPED_AREA
/**
 * get_unmapped_area - 在用户虚拟地址空间查找一块空闲区域，Linux2.4
 * @addr: 用户传入的期望起始地址，0代表由内核自主选择
 * @len: 需要的空闲区间长度（字节）
 * 返回值: 成功返回页对齐后的空闲起始地址；找不到返回0
 */
unsigned long get_unmapped_area(unsigned long addr, unsigned long len)
{
    struct vm_area_struct * vmm;

    // 申请长度超过整个用户地址空间上限，直接失败
    if (len > TASK_SIZE)
        return 0;
    // 用户没有指定地址，从默认起始位置 TASK_UNMAPPED_BASE 开始查找
    if (!addr)
        addr = TASK_UNMAPPED_BASE;
    // 起始地址对齐到页边界
    addr = PAGE_ALIGN(addr);

    // 从addr位置找到第一个VMA，向后遍历VMA链表寻找空隙
    for (vmm = find_vma(current->mm, addr); ; vmm = vmm->vm_next) {
        /* At this point:  (!vmm || addr < vmm->vm_end). */
        // 地址往后已经没有足够空间，超出用户地址上限，查找失败
        if (TASK_SIZE - len < addr)
            return 0;
        // 两种合法情况：后面没有VMA / 当前空隙足够放下本次映射
        if (!vmm || addr + len <= vmm->vm_start)
            return addr;
        // 空隙不够，跳到当前VMA的末尾，继续往后找
        addr = vmm->vm_end;
    }
}

#endif

#define vm_avl_empty	(struct vm_area_struct *) NULL

#include "mmap_avl.c"

/* Look up the first VMA which satisfies  addr < vm_end,  NULL if none. */
/**
 * find_vma - 根据虚拟地址查找所属的VMA，Linux2.4
 * @mm：待查找的进程内存描述符
 * @addr：目标虚拟地址
 * 返回值：找到则返回对应的vm_area_struct；找不到返回NULL
 */
struct vm_area_struct * find_vma(struct mm_struct * mm, unsigned long addr)
{
    struct vm_area_struct *vma = NULL;

    if (mm) {
        // 优先检查缓存，缓存命中率大约35%
        /* Check the cache first. */
        /* (Cache hit rate is typically around 35%.) */
        vma = mm->mmap_cache;
        // 判断缓存中的vma是否包含该地址
        if (!(vma && vma->vm_end > addr && vma->vm_start <= addr)) {
            // AVL树为空，退化成线性链表遍历
            if (!mm->mmap_avl) {
                /* Go through the linear list. */
                vma = mm->mmap;
                while (vma && vma->vm_end <= addr)
                    vma = vma->vm_next;
            } else {
                /* Then go through the AVL tree quickly. */
                struct vm_area_struct * tree = mm->mmap_avl;
                vma = NULL;
                for (;;) {
                    // 遍历到空节点，退出循环
                    if (tree == vm_avl_empty)
                        break;
                    // 当前节点结束地址大于addr，候选vma
                    if (tree->vm_end > addr) {
                        vma = tree;
                        // 地址落在当前vma区间内，查找成功
                        if (tree->vm_start <= addr)
                            break;
                        // 目标地址更小，往左子树搜索
                        tree = tree->vm_avl_left;
                    } else
                        // 当前vma整体在addr左边，往右子树搜索
                        tree = tree->vm_avl_right;
                }
            }
            // 更新缓存为本次查到的vma
            if (vma)
                mm->mmap_cache = vma;
        }
    }
    return vma;
}

/* Same as find_vma, but also return a pointer to the previous VMA in *pprev. */
struct vm_area_struct * find_vma_prev(struct mm_struct * mm, unsigned long addr,
				      struct vm_area_struct **pprev)
{
	if (mm) {
		if (!mm->mmap_avl) {
			/* Go through the linear list. */
			struct vm_area_struct * prev = NULL;
			struct vm_area_struct * vma = mm->mmap;
			while (vma && vma->vm_end <= addr) {
				prev = vma;
				vma = vma->vm_next;
			}
			*pprev = prev;
			return vma;
		} else {
			/* Go through the AVL tree quickly. */
			struct vm_area_struct * vma = NULL;
			struct vm_area_struct * last_turn_right = NULL;
			struct vm_area_struct * prev = NULL;
			struct vm_area_struct * tree = mm->mmap_avl;
			for (;;) {
				if (tree == vm_avl_empty)
					break;
				if (tree->vm_end > addr) {
					vma = tree;
					prev = last_turn_right;
					if (tree->vm_start <= addr)
						break;
					tree = tree->vm_avl_left;
				} else {
					last_turn_right = tree;
					tree = tree->vm_avl_right;
				}
			}
			if (vma) {
				if (vma->vm_avl_left != vm_avl_empty) {
					prev = vma->vm_avl_left;
					while (prev->vm_avl_right != vm_avl_empty)
						prev = prev->vm_avl_right;
				}
				if ((prev ? prev->vm_next : mm->mmap) != vma)
					printk("find_vma_prev: tree inconsistent with list\n");
				*pprev = prev;
				return vma;
			}
		}
	}
	*pprev = NULL;
	return NULL;
}

struct vm_area_struct * find_extend_vma(struct mm_struct * mm, unsigned long addr)
{
	struct vm_area_struct * vma;
	unsigned long start;

	addr &= PAGE_MASK;
	vma = find_vma(mm,addr);
	if (!vma)
		return NULL;
	if (vma->vm_start <= addr)
		return vma;
	if (!(vma->vm_flags & VM_GROWSDOWN))
		return NULL;
	start = vma->vm_start;
	if (expand_stack(vma, addr))
		return NULL;
	if (vma->vm_flags & VM_LOCKED) {
		make_pages_present(addr, start);
	}
	return vma;
}

/* Normal function to fix up a mapping
 * This function is the default for when an area has no specific
 * function.  This may be used as part of a more specific routine.
 * This function works out what part of an area is affected and
 * adjusts the mapping information.  Since the actual page
 * manipulation is done in do_mmap(), none need be done here,
 * though it would probably be more appropriate.
 *
 * By the time this function is called, the area struct has been
 * removed from the process mapping list, so it needs to be
 * reinserted if necessary.
 *
 * The 4 main cases are:
 *    Unmapping the whole area
 *    Unmapping from the start of the segment to a point in it
 *    Unmapping from an intermediate point to the end
 *    Unmapping between to intermediate points, making a hole.
 *
 * Case 4 involves the creation of 2 new areas, for each side of
 * the hole.  If possible, we reuse the existing area rather than
 * allocate a new one, and the return indicates whether the old
 * area was reused.
 */
static struct vm_area_struct * unmap_fixup(struct mm_struct *mm, 
	struct vm_area_struct *area, unsigned long addr, size_t len, 
	struct vm_area_struct *extra)
{
	struct vm_area_struct *mpnt;
	unsigned long end = addr + len;

	area->vm_mm->total_vm -= len >> PAGE_SHIFT;
	if (area->vm_flags & VM_LOCKED)
		area->vm_mm->locked_vm -= len >> PAGE_SHIFT;

	/* Unmapping the whole area. */
	if (addr == area->vm_start && end == area->vm_end) {
		if (area->vm_ops && area->vm_ops->close)
			area->vm_ops->close(area);
		if (area->vm_file)
			fput(area->vm_file);
		kmem_cache_free(vm_area_cachep, area);
		return extra;
	}

	/* Work out to one of the ends. */
	if (end == area->vm_end) {
		area->vm_end = addr;
		lock_vma_mappings(area);
		spin_lock(&mm->page_table_lock);
	} else if (addr == area->vm_start) {
		area->vm_pgoff += (end - area->vm_start) >> PAGE_SHIFT;
		area->vm_start = end;
		lock_vma_mappings(area);
		spin_lock(&mm->page_table_lock);
	} else {
	/* Unmapping a hole: area->vm_start < addr <= end < area->vm_end */
		/* Add end mapping -- leave beginning for below */
		mpnt = extra;
		extra = NULL;

		mpnt->vm_mm = area->vm_mm;
		mpnt->vm_start = end;
		mpnt->vm_end = area->vm_end;
		mpnt->vm_page_prot = area->vm_page_prot;
		mpnt->vm_flags = area->vm_flags;
		mpnt->vm_raend = 0;
		mpnt->vm_ops = area->vm_ops;
		mpnt->vm_pgoff = area->vm_pgoff + ((end - area->vm_start) >> PAGE_SHIFT);
		mpnt->vm_file = area->vm_file;
		mpnt->vm_private_data = area->vm_private_data;
		if (mpnt->vm_file)
			get_file(mpnt->vm_file);
		if (mpnt->vm_ops && mpnt->vm_ops->open)
			mpnt->vm_ops->open(mpnt);
		area->vm_end = addr;	/* Truncate area */

		/* Because mpnt->vm_file == area->vm_file this locks
		 * things correctly.
		 */
		lock_vma_mappings(area);
		spin_lock(&mm->page_table_lock);
		__insert_vm_struct(mm, mpnt);
	}

	__insert_vm_struct(mm, area);
	spin_unlock(&mm->page_table_lock);
	unlock_vma_mappings(area);
	return extra;
}

/*
 * Try to free as many page directory entries as we can,
 * without having to work very hard at actually scanning
 * the page tables themselves.
 *
 * Right now we try to free page tables if we have a nice
 * PGDIR-aligned area that got free'd up. We could be more
 * granular if we want to, but this is fast and simple,
 * and covers the bad cases.
 *
 * "prev", if it exists, points to a vma before the one
 * we just free'd - but there's no telling how much before.
 */
static void free_pgtables(struct mm_struct * mm, struct vm_area_struct *prev,
	unsigned long start, unsigned long end)
{
	unsigned long first = start & PGDIR_MASK;
	unsigned long last = end + PGDIR_SIZE - 1;
	unsigned long start_index, end_index;

	if (!prev) {
		prev = mm->mmap;
		if (!prev)
			goto no_mmaps;
		if (prev->vm_end > start) {
			if (last > prev->vm_start)
				last = prev->vm_start;
			goto no_mmaps;
		}
	}
	for (;;) {
		struct vm_area_struct *next = prev->vm_next;

		if (next) {
			if (next->vm_start < start) {
				prev = next;
				continue;
			}
			if (last > next->vm_start)
				last = next->vm_start;
		}
		if (prev->vm_end > first)
			first = prev->vm_end + PGDIR_SIZE - 1;
		break;
	}
no_mmaps:
	/*
	 * If the PGD bits are not consecutive in the virtual address, the
	 * old method of shifting the VA >> by PGDIR_SHIFT doesn't work.
	 */
	start_index = pgd_index(first);
	end_index = pgd_index(last);
	if (end_index > start_index) {
		clear_page_tables(mm, start_index, end_index - start_index);
		flush_tlb_pgtables(mm, first & PGDIR_MASK, last & PGDIR_MASK);
	}
}

/* Munmap is split into 2 main parts -- this part which finds
 * what needs doing, and the areas themselves, which do the
 * work.  This now handles partial unmappings.
 * Jeremy Fitzhardine <jeremy@sw.oz.au>
 */
int do_munmap(struct mm_struct *mm, unsigned long addr, size_t len)
{
	struct vm_area_struct *mpnt, *prev, **npp, *free, *extra;

	if ((addr & ~PAGE_MASK) || addr > TASK_SIZE || len > TASK_SIZE-addr)
		return -EINVAL;

	if ((len = PAGE_ALIGN(len)) == 0)
		return -EINVAL;

	/* Check if this memory area is ok - put it on the temporary
	 * list if so..  The checks here are pretty simple --
	 * every area affected in some way (by any overlap) is put
	 * on the list.  If nothing is put on, nothing is affected.
	 */
	mpnt = find_vma_prev(mm, addr, &prev);
	if (!mpnt)
		return 0;
	/* we have  addr < mpnt->vm_end  */

	if (mpnt->vm_start >= addr+len)
		return 0;

	/* If we'll make "hole", check the vm areas limit */
	if ((mpnt->vm_start < addr && mpnt->vm_end > addr+len)
	    && mm->map_count >= MAX_MAP_COUNT)
		return -ENOMEM;

	/*
	 * We may need one additional vma to fix up the mappings ... 
	 * and this is the last chance for an easy error exit.
	 */
	extra = kmem_cache_alloc(vm_area_cachep, SLAB_KERNEL);
	if (!extra)
		return -ENOMEM;

	npp = (prev ? &prev->vm_next : &mm->mmap);
	free = NULL;
	spin_lock(&mm->page_table_lock);
	for ( ; mpnt && mpnt->vm_start < addr+len; mpnt = *npp) {
		*npp = mpnt->vm_next;
		mpnt->vm_next = free;
		free = mpnt;
		if (mm->mmap_avl)
			avl_remove(mpnt, &mm->mmap_avl);
	}
	mm->mmap_cache = NULL;	/* Kill the cache. */
	spin_unlock(&mm->page_table_lock);

	/* Ok - we have the memory areas we should free on the 'free' list,
	 * so release them, and unmap the page range..
	 * If the one of the segments is only being partially unmapped,
	 * it will put new vm_area_struct(s) into the address space.
	 * In that case we have to be careful with VM_DENYWRITE.
	 */
	while ((mpnt = free) != NULL) {
		unsigned long st, end, size;
		struct file *file = NULL;

		free = free->vm_next;

		st = addr < mpnt->vm_start ? mpnt->vm_start : addr;
		end = addr+len;
		end = end > mpnt->vm_end ? mpnt->vm_end : end;
		size = end - st;

		if (mpnt->vm_flags & VM_DENYWRITE &&
		    (st != mpnt->vm_start || end != mpnt->vm_end) &&
		    (file = mpnt->vm_file) != NULL) {
			atomic_dec(&file->f_dentry->d_inode->i_writecount);
		}
		remove_shared_vm_struct(mpnt);
		mm->map_count--;

		flush_cache_range(mm, st, end);
		zap_page_range(mm, st, size);
		flush_tlb_range(mm, st, end);

		/*
		 * Fix the mapping, and free the old area if it wasn't reused.
		 */
		extra = unmap_fixup(mm, mpnt, st, size, extra);
		if (file)
			atomic_inc(&file->f_dentry->d_inode->i_writecount);
	}

	/* Release the extra vma struct if it wasn't used */
	if (extra)
		kmem_cache_free(vm_area_cachep, extra);

	free_pgtables(mm, prev, addr, addr+len);

	return 0;
}

asmlinkage long sys_munmap(unsigned long addr, size_t len)
{
	int ret;
	struct mm_struct *mm = current->mm;

	down(&mm->mmap_sem);
	ret = do_munmap(mm, addr, len);
	up(&mm->mmap_sem);
	return ret;
}

/*
 *  this is really a simplified "do_mmap".  it only handles
 *  anonymous maps.  eventually we may be able to do some
 *  brk-specific accounting here.
 */
/**
 * do_brk - 堆扩展底层函数，创建/扩展匿名堆VMA，Linux2.4
 * @addr: 扩展起始虚拟地址
 * @len: 需要扩展的长度
 * 返回值: 成功返回addr；失败返回负的错误码
 */
unsigned long do_brk(unsigned long addr, unsigned long len)
{
    struct mm_struct * mm = current->mm;
    struct vm_area_struct * vma;
    unsigned long flags, retval;

    // 长度对齐到页边界
    len = PAGE_ALIGN(len);
    // 长度为0，无需操作，直接返回起始地址
    if (!len)
        return addr;

    /*
     * mlock MCL_FUTURE?
     */
    // 如果进程设置了MCL_FUTURE，后续分配内存默认上锁
    if (mm->def_flags & VM_LOCKED) {
        unsigned long locked = mm->locked_vm << PAGE_SHIFT;
        locked += len;
        // 检查锁定内存是否超出RLIMIT_MEMLOCK限制
        if (locked > current->rlim[RLIMIT_MEMLOCK].rlim_cur)
            return -EAGAIN;
    }

    /*
     * Clear old maps.  this also does some error checking for us
     */
    // 先释放该区间已有的映射，防止重叠
    retval = do_munmap(mm, addr, len);
    if (retval != 0)
        return retval;

    /* Check against address space limits *after* clearing old maps... */
    // 检查进程总虚拟地址空间是否超出RLIMIT_AS
    if ((mm->total_vm << PAGE_SHIFT) + len
        > current->rlim[RLIMIT_AS].rlim_cur)
        return -ENOMEM;

    // 检查VMA数量是否达到上限MAX_MAP_COUNT
    if (mm->map_count > MAX_MAP_COUNT)
        return -ENOMEM;

    // 粗略检查系统是否有足够内存
    if (!vm_enough_memory(len >> PAGE_SHIFT))
        return -ENOMEM;

    // 生成VMA基础权限标志：读写执行，固定地址、私有映射，叠加进程默认标志
    flags = vm_flags(PROT_READ|PROT_WRITE|PROT_EXEC,
                MAP_FIXED|MAP_PRIVATE) | mm->def_flags;

    // 添加允许后续修改权限的标志
    flags |= VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC;
    

    /* Can we just expand an old anonymous mapping? */
    // 优先尝试：直接向后扩展已存在的堆匿名VMA，不用新建VMA
    if (addr) {
        struct vm_area_struct * vma = find_vma(mm, addr-1);
        if (vma && vma->vm_end == addr && !vma->vm_file && 
            vma->vm_flags == flags) {
            // 原有VMA的结束地址正好是本次扩展起点，直接扩大vm_end
            vma->vm_end = addr + len;
            goto out;
        }
    }   


    /*
     * create a vma struct for an anonymous mapping
     */
    // 上面扩展失败，分配新的VMA对象
    vma = kmem_cache_alloc(vm_area_cachep, SLAB_KERNEL);
    if (!vma)
        return -ENOMEM;

    // 初始化匿名VMA各个成员
    vma->vm_mm = mm;
    vma->vm_start = addr;
    vma->vm_end = addr + len;
    vma->vm_flags = flags;
    vma->vm_page_prot = protection_map[flags & 0x0f];
    vma->vm_ops = NULL;
    vma->vm_pgoff = 0;
    vma->vm_file = NULL;
    vma->vm_private_data = NULL;

    // 把新VMA插入mm的VMA链表、AVL树
    insert_vm_struct(mm, vma);

out:
    // 增加进程总虚拟页数统计
    mm->total_vm += len >> PAGE_SHIFT;
    // 如果是锁页，增加locked_vm计数，并且预分配物理页
    if (flags & VM_LOCKED) {
        mm->locked_vm += len >> PAGE_SHIFT;
        make_pages_present(addr, addr + len);
    }
    return addr;
}

/* Build the AVL tree corresponding to the VMA list. */
void build_mmap_avl(struct mm_struct * mm)
{
	struct vm_area_struct * vma;

	mm->mmap_avl = NULL;
	for (vma = mm->mmap; vma; vma = vma->vm_next)
		avl_insert(vma, &mm->mmap_avl);
}

/* Release all mmaps. */
void exit_mmap(struct mm_struct * mm)
{
	struct vm_area_struct * mpnt;

	release_segments(mm);
	spin_lock(&mm->page_table_lock);
	mpnt = mm->mmap;
	mm->mmap = mm->mmap_avl = mm->mmap_cache = NULL;
	spin_unlock(&mm->page_table_lock);
	mm->rss = 0;
	mm->total_vm = 0;
	mm->locked_vm = 0;
	while (mpnt) {
		struct vm_area_struct * next = mpnt->vm_next;
		unsigned long start = mpnt->vm_start;
		unsigned long end = mpnt->vm_end;
		unsigned long size = end - start;

		if (mpnt->vm_ops) {
			if (mpnt->vm_ops->close)
				mpnt->vm_ops->close(mpnt);
		}
		mm->map_count--;
		remove_shared_vm_struct(mpnt);
		flush_cache_range(mm, start, end);
		zap_page_range(mm, start, size);
		if (mpnt->vm_file)
			fput(mpnt->vm_file);
		kmem_cache_free(vm_area_cachep, mpnt);
		mpnt = next;
	}

	/* This is just debugging */
	if (mm->map_count)
		printk("exit_mmap: map count is %d\n", mm->map_count);

	clear_page_tables(mm, FIRST_USER_PGD_NR, USER_PTRS_PER_PGD);
}

/* Insert vm structure into process list sorted by address
 * and into the inode's i_mmap ring.  If vm_file is non-NULL
 * then the i_shared_lock must be held here.
 */
/**
 * __insert_vm_struct - 真正执行VMA插入的底层函数，插入到mm的VMA链表、AVL树、文件共享映射链表，Linux2.4
 * @mm：目标内存描述符
 * @vmp：待插入的VMA
 */
void __insert_vm_struct(struct mm_struct *mm, struct vm_area_struct *vmp)
{
    struct vm_area_struct **pprev;
    struct file * file;

    // AVL树还没有构建，直接遍历单向链表找插入位置
    if (!mm->mmap_avl) {
        pprev = &mm->mmap;
        while (*pprev && (*pprev)->vm_start <= vmp->vm_start)
            pprev = &(*pprev)->vm_next;
    } else {
        // AVL树已存在，调用avl_insert_neighbours在树中插入，拿到前驱、后继节点
        struct vm_area_struct *prev, *next;
        avl_insert_neighbours(vmp, &mm->mmap_avl, &prev, &next);
        pprev = (prev ? &prev->vm_next : &mm->mmap);
        // 校验链表和AVL树一致性，不一致打印告警
        if (*pprev != next)
            printk("insert_vm_struct: tree inconsistent with list\n");
    }
    // 把vmp挂入mmap单向链表
    vmp->vm_next = *pprev;
    *pprev = vmp;

    // VMA计数加一
    mm->map_count++;
    // VMA数量达到阈值，且还没有AVL树，则一次性构建AVL树加速查找
    if (mm->map_count >= AVL_MIN_MAP_COUNT && !mm->mmap_avl)
        build_mmap_avl(mm);

    file = vmp->vm_file;
    // 如果是文件映射，需要挂入inode的address_space共享链表
    if (file) {
        struct inode * inode = file->f_dentry->d_inode;
        struct address_space *mapping = inode->i_mapping;
        struct vm_area_struct **head;

        // VM_DENYWRITE：禁止其他进程写这个文件，递减inode写计数
        if (vmp->vm_flags & VM_DENYWRITE)
            atomic_dec(&inode->i_writecount);

        // 根据是否共享映射，选择i_mmap（私有） / i_mmap_shared（共享）链表头
        head = &mapping->i_mmap;
        if (vmp->vm_flags & VM_SHARED)
            head = &mapping->i_mmap_shared;
      
        /* insert vmp into inode's share list */
        // 头插法加入inode的VMA共享链表
        if((vmp->vm_next_share = *head) != NULL)
            (*head)->vm_pprev_share = &vmp->vm_next_share;
        *head = vmp;
        vmp->vm_pprev_share = head;
    }
}

/**
 * insert_vm_struct - 向进程地址空间插入一个VMA，Linux2.4
 * @mm：目标进程内存描述符
 * @vmp：待插入的VMA
 */
void insert_vm_struct(struct mm_struct *mm, struct vm_area_struct *vmp)
{
    // 锁住共享映射相关的address_space链表，防止并发修改
    lock_vma_mappings(vmp);
    // 获取页表自旋锁，保护mm内VMA链表、AVL树、页表相关结构
    spin_lock(&current->mm->page_table_lock);
    // 底层真正执行插入逻辑：加入链表、AVL树、更新map_count
    __insert_vm_struct(mm, vmp);
    // 释放页表自旋锁
    spin_unlock(&current->mm->page_table_lock);
    // 解锁共享映射链表
    unlock_vma_mappings(vmp);
}
