/*
 *  linux/fs/file_table.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *  Copyright (C) 1997 David S. Miller (davem@caip.rutgers.edu)
 */

#include <linux/string.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/smp_lock.h>

/* sysctl tunables... */
struct files_stat_struct files_stat = {0, 0, NR_FILE};

/* Here the new files go */
static LIST_HEAD(anon_list);
/* And here the free ones sit */
static LIST_HEAD(free_list);
/* public *and* exported. Not pretty! */
spinlock_t files_lock = SPIN_LOCK_UNLOCKED;

/* Find an unused file structure and return a pointer to it.
 * Returns NULL, if there are no more free file structures or
 * we run out of memory.
 *
 * SMP-safe.
 */

// get_empty_filp：获取一个空闲的 struct file 对象（filp）
// 从空闲链表 free_list 取，不足时从slab缓存新建；超级用户可以使用预留文件资源
// 返回：成功返回初始化完毕的struct file*；资源耗尽返回NULL（上层会返回ENFILE）
struct file * get_empty_filp(void)
{
	static int old_max = 0;
	struct file * f;

	// 全局文件链表锁，保护 free_list、anon_list、files_stat 等全局变量
	file_list_lock();

	// 如果空闲filp数量大于预留数量 NR_RESERVED_FILES，则正常取用空闲filp
	if (files_stat.nr_free_files > NR_RESERVED_FILES) {
	used_one:
		// 从free_list链表头部取出一个空闲struct file
		f = list_entry(free_list.next, struct file, f_list);
		list_del(&f->f_list);
		files_stat.nr_free_files--;
	new_one:
		memset(f, 0, sizeof(*f));
		atomic_set(&f->f_count,1);
		f->f_version = ++event;
		f->f_uid = current->fsuid;
		f->f_gid = current->fsgid;
		list_add(&f->f_list, &anon_list);
		file_list_unlock();
		return f;
	}
	/*
	 * Use a reserved one if we're the superuser
	 */
	// 空闲filp已经进入预留池：只有root(euid=0)才允许消耗预留的filp
	if (files_stat.nr_free_files && !current->euid)
		goto used_one;
	/*
	 * Allocate a new one if we're below the limit.
	 */
	// 当前全局总打开文件数还没到上限max_files，可以新建filp
	if (files_stat.nr_files < files_stat.max_files) {
		file_list_unlock();
		// 从filp专用slab缓存分配内存
		f = kmem_cache_alloc(filp_cachep, SLAB_KERNEL);
		file_list_lock();
		if (f) {
			files_stat.nr_files++;  // 全局filp总数+1
			goto new_one;  // 跳转到new_one分支做统一初始化
		}
		/* Big problems... */
		printk("VFS: filp allocation failed\n");

	} else if (files_stat.max_files > old_max) {
		// 达到全局文件上限 file-max，打印一次告警，old_max防止重复刷屏
		printk("VFS: file-max limit %d reached\n", files_stat.max_files);
		old_max = files_stat.max_files;
	}
	file_list_unlock();
	return NULL;
}

/*
 * Clear and initialize a (private) struct file for the given dentry,
 * and call the open function (if any).  The caller must verify that
 * inode->i_fop is not NULL.
 */
int init_private_file(struct file *filp, struct dentry *dentry, int mode)
{
	memset(filp, 0, sizeof(*filp));
	filp->f_mode   = mode;
	atomic_set(&filp->f_count, 1);
	filp->f_dentry = dentry;
	filp->f_uid    = current->fsuid;
	filp->f_gid    = current->fsgid;
	filp->f_op     = dentry->d_inode->i_fop;
	if (filp->f_op->open)
		return filp->f_op->open(dentry->d_inode, filp);
	else
		return 0;
}

// fput：减少struct file（filp）的引用计数，引用计数降到0时执行完整的资源回收
// 和fget成对使用；fget做f_count++，fput做f_count--
// @file：待释放引用的struct file对象
void fput(struct file * file)
{
	// 提前缓存file里面关联的VFS对象指针（dentry、挂载点、inode）
    // 后面会把file成员置NULL，所以先保存
	struct dentry * dentry = file->f_dentry;
	struct vfsmount * mnt = file->f_vfsmnt;
	struct inode * inode = dentry->d_inode;

	if (atomic_dec_and_test(&file->f_count)) {

		// 引用计数归零，开始清理资源
        // 移除flock建议锁（和filp_close里面POSIX记录锁locks_remove_posix区分）
		locks_remove_flock(file);

		// 如果文件操作集提供release回调，执行底层release
        // 驱动/文件系统在这里释放私有资源（private_data）
		if (file->f_op && file->f_op->release)
			file->f_op->release(inode, file);

		// 减少f_op(file_operations)的引用计数（dentry_open中fops_get增加）
		fops_put(file->f_op);

		// 清空file里的dentry、vfsmnt指针，防止野指针
		file->f_dentry = NULL;
		file->f_vfsmnt = NULL;

		// 如果本次打开是可写模式，释放inode的写访问引用
		if (file->f_mode & FMODE_WRITE)
			put_write_access(inode);

		// 释放dentry引用计数（dput：dentry引用减1，dcache缓存管理）
		dput(dentry);

		// 释放vfsmount挂载实例引用计数
		if (mnt)
			mntput(mnt);
		
		// 全局filp链表锁，操作free_list空闲链表
		file_list_lock();

		// 从anon_list摘除这个filp
		list_del(&file->f_list);

		// 加入free_list空闲链表，供get_empty_filp复用
		list_add(&file->f_list, &free_list);
		files_stat.nr_free_files++;  // 空闲filp数目+1
		file_list_unlock();
	}
}

// fget：根据fd，获取当前进程对应的struct file，并增加引用计数
// @fd：用户传入的文件描述符
// 返回：找到返回struct file*（引用计数+1）；找不到返回NULL
struct file * fget(unsigned int fd)
{
	struct file * file;

	// current 是当前进程task_struct；files_struct 保存本进程所有打开文件描述符表
	struct files_struct *files = current->files;

	read_lock(&files->file_lock);

	// fcheck：在files_struct里按fd查找，返回对应的struct file，只查表，不加引用计数
	file = fcheck(fd);
	if (file)
		get_file(file);  // 找到file，将file->f_count 引用计数 +1
	read_unlock(&files->file_lock);
	return file;
}

/* Here. put_filp() is SMP-safe now. */

void put_filp(struct file *file)
{
	if(atomic_dec_and_test(&file->f_count)) {
		file_list_lock();
		list_del(&file->f_list);
		list_add(&file->f_list, &free_list);
		files_stat.nr_free_files++;
		file_list_unlock();
	}
}

void file_move(struct file *file, struct list_head *list)
{
	if (!list)
		return;
	file_list_lock();
	list_del(&file->f_list);
	list_add(&file->f_list, list);
	file_list_unlock();
}

void file_moveto(struct file *new, struct file *old)
{
	file_list_lock();
	list_del(&new->f_list);
	list_add(&new->f_list, &old->f_list);
	file_list_unlock();
}

int fs_may_remount_ro(struct super_block *sb)
{
	struct list_head *p;

	/* Check that no files are currently opened for writing. */
	file_list_lock();
	for (p = sb->s_files.next; p != &sb->s_files; p = p->next) {
		struct file *file = list_entry(p, struct file, f_list);
		struct inode *inode;

		if (!file->f_dentry)
			continue;

		inode = file->f_dentry->d_inode;

		/* File with pending delete? */
		if (inode->i_nlink == 0)
			goto too_bad;

		/* Writable file? */
		if (S_ISREG(inode->i_mode) && (file->f_mode & FMODE_WRITE))
			goto too_bad;
	}
	file_list_unlock();
	return 1; /* Tis' cool bro. */
too_bad:
	file_list_unlock();
	return 0;
}
