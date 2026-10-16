/*
 *  linux/fs/namei.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 * Some corrections by tytso.
 */

/* [Feb 1997 T. Schoebel-Theuer] Complete rewrite of the pathname
 * lookup logic.
 */
/* [Feb-Apr 2000, AV] Rewrite to the new namespace architecture.
 */

#include <linux/init.h>
#include <linux/mm.h>
#include <linux/proc_fs.h>
#include <linux/smp_lock.h>
#include <linux/quotaops.h>
#include <linux/pagemap.h>
#include <linux/dcache.h>
#include <linux/dnotify.h>

#include <asm/uaccess.h>
#include <asm/unaligned.h>
#include <asm/semaphore.h>
#include <asm/page.h>
#include <asm/pgtable.h>

#include <asm/namei.h>

#define ACC_MODE(x) ("\000\004\002\006"[(x)&O_ACCMODE])

/* [Feb-1997 T. Schoebel-Theuer]
 * Fundamental changes in the pathname lookup mechanisms (namei)
 * were necessary because of omirr.  The reason is that omirr needs
 * to know the _real_ pathname, not the user-supplied one, in case
 * of symlinks (and also when transname replacements occur).
 *
 * The new code replaces the old recursive symlink resolution with
 * an iterative one (in case of non-nested symlink chains).  It does
 * this with calls to <fs>_follow_link().
 * As a side effect, dir_namei(), _namei() and follow_link() are now 
 * replaced with a single function lookup_dentry() that can handle all 
 * the special cases of the former code.
 *
 * With the new dcache, the pathname is stored at each inode, at least as
 * long as the refcount of the inode is positive.  As a side effect, the
 * size of the dcache depends on the inode cache and thus is dynamic.
 *
 * [29-Apr-1998 C. Scott Ananian] Updated above description of symlink
 * resolution to correspond with current state of the code.
 *
 * Note that the symlink resolution is not *completely* iterative.
 * There is still a significant amount of tail- and mid- recursion in
 * the algorithm.  Also, note that <fs>_readlink() is not used in
 * lookup_dentry(): lookup_dentry() on the result of <fs>_readlink()
 * may return different results than <fs>_follow_link().  Many virtual
 * filesystems (including /proc) exhibit this behavior.
 */

/* [24-Feb-97 T. Schoebel-Theuer] Side effects caused by new implementation:
 * New symlink semantics: when open() is called with flags O_CREAT | O_EXCL
 * and the name already exists in form of a symlink, try to create the new
 * name indicated by the symlink. The old code always complained that the
 * name already exists, due to not following the symlink even if its target
 * is nonexistent.  The new semantics affects also mknod() and link() when
 * the name is a symlink pointing to a non-existant name.
 *
 * I don't know which semantics is the right one, since I have no access
 * to standards. But I found by trial that HP-UX 9.0 has the full "new"
 * semantics implemented, while SunOS 4.1.1 and Solaris (SunOS 5.4) have the
 * "old" one. Personally, I think the new semantics is much more logical.
 * Note that "ln old new" where "new" is a symlink pointing to a non-existing
 * file does succeed in both HP-UX and SunOs, but not in Solaris
 * and in the old Linux semantics.
 */

/* [16-Dec-97 Kevin Buhr] For security reasons, we change some symlink
 * semantics.  See the comments in "open_namei" and "do_link" below.
 *
 * [10-Sep-98 Alan Modra] Another symlink change.
 */

/* [Feb-Apr 2000 AV] Complete rewrite. Rules for symlinks:
 *	inside the path - always follow.
 *	in the last component in creation/removal/renaming - never follow.
 *	if LOOKUP_FOLLOW passed - follow.
 *	if the pathname has trailing slashes - follow.
 *	otherwise - don't follow.
 * (applied in that order).
 *
 * [Jun 2000 AV] Inconsistent behaviour of open() in case if flags==O_CREAT
 * restored for 2.4. This is the last surviving part of old 4.2BSD bug.
 * During the 2.4 we need to fix the userland stuff depending on it -
 * hopefully we will be able to get rid of that wart in 2.5. So far only
 * XEmacs seems to be relying on it...
 */

/* In order to reduce some races, while at the same time doing additional
 * checking and hopefully speeding things up, we copy filenames to the
 * kernel data space before using them..
 *
 * POSIX.1 2.4: an empty pathname is invalid (ENOENT).
 */
static inline int do_getname(const char *filename, char *page)
{
	int retval;
	unsigned long len = PATH_MAX + 1;

	if ((unsigned long) filename >= TASK_SIZE) {
		if (!segment_eq(get_fs(), KERNEL_DS))
			return -EFAULT;
	} else if (TASK_SIZE - (unsigned long) filename < PAGE_SIZE)
		len = TASK_SIZE - (unsigned long) filename;

	retval = strncpy_from_user((char *)page, filename, len);
	if (retval > 0) {
		if (retval < len)
			return 0;
		return -ENAMETOOLONG;
	} else if (!retval)
		retval = -ENOENT;
	return retval;
}

/**
 * getname - 把用户态路径名字符串拷贝到内核name缓冲区
 * @filename: 用户态传入的路径字符串虚拟地址（用户空间指针）
 *
 * 返回值：成功返回内核缓冲区指针；失败返回带错误码的ERR_PTR，可用IS_ERR判断
 *
 * 配套：getname 必须成对调用 putname() 释放，否则内核name缓存泄漏
 */
char * getname(const char * filename)
{
	char *tmp, *result;

	result = ERR_PTR(-ENOMEM);

	/*
     * __getname()：从内核name缓存池分配一个name缓冲区
     * 内核维护专门的name缓存slab，专门存放文件路径，大小一般是PATH_MAX（256）
     * 分配成功返回缓冲区内核地址；失败返回NULL
     */
	tmp = __getname();
	if (tmp)  {
		int retval = do_getname(filename, tmp);

		result = tmp;
		if (retval < 0) {
			putname(tmp);
			result = ERR_PTR(retval);
		}
	}
	return result;
}

/*
 *	permission()
 *
 * is used to check for read/write/execute permissions on a file.
 * We use "fsuid" for this, letting us set arbitrary permissions
 * for filesystem access without changing the "normal" uids which
 * are used for other things..
 */
int vfs_permission(struct inode * inode,int mask)
{
	int mode = inode->i_mode;

	if ((mask & S_IWOTH) && IS_RDONLY(inode) &&
		 (S_ISREG(mode) || S_ISDIR(mode) || S_ISLNK(mode)))
		return -EROFS; /* Nobody gets write access to a read-only fs */

	if ((mask & S_IWOTH) && IS_IMMUTABLE(inode))
		return -EACCES; /* Nobody gets write access to an immutable file */

	if (current->fsuid == inode->i_uid)
		mode >>= 6;
	else if (in_group_p(inode->i_gid))
		mode >>= 3;

	if (((mode & mask & S_IRWXO) == mask) || capable(CAP_DAC_OVERRIDE))
		return 0;

	/* read and search access */
	if ((mask == S_IROTH) ||
	    (S_ISDIR(inode->i_mode)  && !(mask & ~(S_IROTH | S_IXOTH))))
		if (capable(CAP_DAC_READ_SEARCH))
			return 0;

	return -EACCES;
}

int permission(struct inode * inode,int mask)
{
	if (inode->i_op && inode->i_op->permission) {
		int retval;
		lock_kernel();
		retval = inode->i_op->permission(inode, mask);
		unlock_kernel();
		return retval;
	}
	return vfs_permission(inode, mask);
}

/*
 * get_write_access() gets write permission for a file.
 * put_write_access() releases this write permission.
 * This is used for regular files.
 * We cannot support write (and maybe mmap read-write shared) accesses and
 * MAP_DENYWRITE mmappings simultaneously. The i_writecount field of an inode
 * can have the following values:
 * 0: no writers, no VM_DENYWRITE mappings
 * < 0: (-i_writecount) vm_area_structs with VM_DENYWRITE set exist
 * > 0: (i_writecount) users are writing to the file.
 *
 * Normally we operate on that counter with atomic_{inc,dec} and it's safe
 * except for the cases where we don't hold i_writecount yet. Then we need to
 * use {get,deny}_write_access() - these functions check the sign and refuse
 * to do the change if sign is wrong. Exclusion between them is provided by
 * spinlock (arbitration_lock) and I'll rip the second arsehole to the first
 * who will try to move it in struct inode - just leave it here.
 */
static spinlock_t arbitration_lock = SPIN_LOCK_UNLOCKED;
int get_write_access(struct inode * inode)
{
	spin_lock(&arbitration_lock);
	if (atomic_read(&inode->i_writecount) < 0) {
		spin_unlock(&arbitration_lock);
		return -ETXTBSY;
	}
	atomic_inc(&inode->i_writecount);
	spin_unlock(&arbitration_lock);
	return 0;
}
int deny_write_access(struct file * file)
{
	spin_lock(&arbitration_lock);
	if (atomic_read(&file->f_dentry->d_inode->i_writecount) > 0) {
		spin_unlock(&arbitration_lock);
		return -ETXTBSY;
	}
	atomic_dec(&file->f_dentry->d_inode->i_writecount);
	spin_unlock(&arbitration_lock);
	return 0;
}

void path_release(struct nameidata *nd)
{
	dput(nd->dentry);
	mntput(nd->mnt);
}

/*
 * Internal lookup() using the new generic dcache.
 * SMP-safe
 */
// cached_lookup：在dentry缓存中查找子目录项，并且做缓存有效性校验
// @parent: 父目录dentry
// @name: qstr，待查找文件名（name,len,hash）
// @flags: 查找标志 LOOKUP_*
// 返回：找到有效dentry则返回（带引用计数）；没找到/缓存失效返回NULL
static struct dentry * cached_lookup(struct dentry * parent, struct qstr * name, int flags)
{
	// d_lookup：在父目录的dentry哈希链表中，根据qstr查找缓存的dentry
	struct dentry * dentry = d_lookup(parent, name);

	// 命中缓存，并且该dentry实现了d_revalidate回调（需要校验缓存有效性）
	if (dentry && dentry->d_op && dentry->d_op->d_revalidate) {
		// d_revalidate：校验这个缓存dentry是否仍然有效
        // 返回true：缓存有效；false：缓存失效
		if (!dentry->d_op->d_revalidate(dentry, flags) && !d_invalidate(dentry)) {

			// 缓存无效，并且d_invalidate执行成功：标记该dentry为无效
			dput(dentry);  // 释放本次d_lookup拿到的引用
			dentry = NULL;
		}
	}
	return dentry;
}

/*
 * This is called when everything else fails, and we actually have
 * to go to the low-level filesystem to find out what we should do..
 *
 * We get the directory semaphore, and after getting that we also
 * make sure that nobody added the entry to the dcache in the meantime..
 * SMP-safe
 */
// real_lookup：dentry缓存miss之后，真正向底层文件系统lookup，加载目录项
// @parent：父目录dentry
// @name：qstr，待查找文件名
// @flags：LOOKUP_*查找标记
// 返回：成功返回新创建的dentry（引用计数+1）；失败返回ERR_PTR负错误码
static struct dentry * real_lookup(struct dentry * parent, struct qstr * name, int flags)
{
	struct dentry * result;
	struct inode *dir = parent->d_inode;

	// 获取目录inode的信号量，串行化对同一个目录的lookup/创建/删除操作
	down(&dir->i_sem);
	/*
	 * First re-do the cached lookup just in case it was created
	 * while we waited for the directory semaphore..
	 *
	 * FIXME! This could use version numbering or similar to
	 * avoid unnecessary cache lookups.
	 */

	// 【重要竞态处理】
    // 在我们等待获取i_sem的这段时间里，别的CPU/进程有可能已经把这个dentry加入缓存了。
    // 所以拿到锁之后，先再查一遍dentry缓存，避免重复创建dentry。
	result = d_lookup(parent, name);
	if (!result) {
		// 缓存依然miss：分配一个空dentry，挂载到parent，名字填入name
		struct dentry * dentry = d_alloc(parent, name);
		result = ERR_PTR(-ENOMEM);
		if (dentry) {
			lock_kernel();
			// 调用文件系统inode操作集的lookup回调（真正读磁盘目录）
            // dir->i_op->lookup：底层文件系统实现（ext2_lookup等）
            // 参数dentry：刚分配好的空dentry，lookup会填充dentry->d_inode
			result = dir->i_op->lookup(dir, dentry);
			unlock_kernel();

			// result非0：lookup返回错误码，查找失败，释放刚分配的dentry
			if (result)
				dput(dentry);
			else
				// 查找成功，dentry已经被lookup填充好inode，把它作为返回值
				result = dentry;
		}
		up(&dir->i_sem);
		return result;
	}

	/*
	 * Uhhuh! Nasty case: the cache was re-populated while
	 * we waited on the semaphore. Need to revalidate.
	 */
	// 分支：拿到i_sem锁之后，d_lookup发现缓存里已经有这个dentry了！
    // 说明：在等锁期间，其他任务抢先完成查找并放入dentry cache。
    up(&dir->i_sem); // 目录信号量可以先释放，后面只是校验缓存有效性，不需要目录锁

	// 和cached_lookup逻辑一致：如果dentry需要有效性校验d_revalidate
	if (result->d_op && result->d_op->d_revalidate) {
		if (!result->d_op->d_revalidate(result, flags) && !d_invalidate(result)) {
			dput(result);
			result = ERR_PTR(-ENOENT);
		}
	}
	return result;
}

static inline int do_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	int err;
	if (current->link_count >= 8)
		goto loop;
	current->link_count++;
	UPDATE_ATIME(dentry->d_inode);
	err = dentry->d_inode->i_op->follow_link(dentry, nd);
	current->link_count--;
	return err;
loop:
	path_release(nd);
	return -ELOOP;
}

static inline int __follow_up(struct vfsmount **mnt, struct dentry **base)
{
	struct vfsmount *parent;
	struct dentry *dentry;
	spin_lock(&dcache_lock);
	parent=(*mnt)->mnt_parent;
	if (parent == *mnt) {
		spin_unlock(&dcache_lock);
		return 0;
	}
	mntget(parent);
	dentry=dget((*mnt)->mnt_mountpoint);
	spin_unlock(&dcache_lock);
	dput(*base);
	*base = dentry;
	mntput(*mnt);
	*mnt = parent;
	return 1;
}

int follow_up(struct vfsmount **mnt, struct dentry **dentry)
{
	return __follow_up(mnt, dentry);
}

/**
 * __follow_down - 挂载穿越的底层实现（Linux2.4，follow_down的内部版本）
 * @mnt: 传入传出参数，当前vfsmount指针的地址
 * @dentry: 传入传出参数，当前dentry指针的地址
 *
 * 返回值：1 成功，发生挂载跳转；0 没有找到匹配挂载，不跳转
 *
 * 功能：当当前dentry是挂载点，遍历d_vfsmnt链表，找到「父挂载恰好是当前mnt」的vfsmount，
 * 把路径查找上下文(mnt/dentry)切换到这个挂载的根目录 mnt_root。
 * 仅处理**直接挂载**，支持同一个目录上多重复合挂载（嵌套挂载场景）
 * 调用前提：调用者一般先通过 d_mountpoint 判断 dentry->d_vfsmnt 是否非空。
 */
static inline int __follow_down(struct vfsmount **mnt, struct dentry **dentry)
{
	struct list_head *p;

	// 拿dcache自旋锁，保护dentry挂载链表遍历
	spin_lock(&dcache_lock);

	// d_vfsmnt是挂载点dentry上挂载的所有vfsmount的链表头
    // p 取链表第一个有效节点（跳过链表头本身）
	p = (*dentry)->d_vfsmnt.next;

	// 循环遍历d_vfsmnt链表，遍历终止条件：回到链表头
	while (p != &(*dentry)->d_vfsmnt) {
		struct vfsmount *tmp;
		tmp = list_entry(p, struct vfsmount, mnt_clash);

		/*
         * 关键判断：tmp这个挂载实例的父mnt == 当前正在使用的mnt
         * 含义：这个挂载，正是在【当前这个文件系统】挂载到该dentry上的那一条挂载
         * 同一个dentry可以叠加多层挂载，我们只匹配直接子挂载
		 * 一般来说一个目录只会有一个挂载点
         */
		if (tmp->mnt_parent == *mnt) {
			// mntget：增加新挂载tmp的引用计数，防止并发umount销毁
			*mnt = mntget(tmp);
			spin_unlock(&dcache_lock);

			// 释放原来旧mnt的引用
			mntput(tmp->mnt_parent);
			
			// 释放原来挂载点dentry的引用
			dput(*dentry);

			// 切换dentry为新挂载的根目录dentry，dget增加引用
			*dentry = dget(tmp->mnt_root);
			return 1;
		}

		// 看下一个挂载（多层挂载场景）
		p = p->next;
	}
	spin_unlock(&dcache_lock);
	return 0;
}

//挂载点穿越
int follow_down(struct vfsmount **mnt, struct dentry **dentry)
{
	return __follow_down(mnt,dentry);
}
 
static inline void follow_dotdot(struct nameidata *nd)
{
	while(1) {
		struct vfsmount *parent;
		struct dentry *dentry;
		read_lock(&current->fs->lock);
		if (nd->dentry == current->fs->root &&
		    nd->mnt == current->fs->rootmnt)  {
			read_unlock(&current->fs->lock);
			break;
		}
		read_unlock(&current->fs->lock);
		spin_lock(&dcache_lock);
		if (nd->dentry != nd->mnt->mnt_root) {
			dentry = dget(nd->dentry->d_parent);
			spin_unlock(&dcache_lock);
			dput(nd->dentry);
			nd->dentry = dentry;
			break;
		}
		parent=nd->mnt->mnt_parent;
		if (parent == nd->mnt) {
			spin_unlock(&dcache_lock);
			break;
		}
		mntget(parent);
		dentry=dget(nd->mnt->mnt_mountpoint);
		spin_unlock(&dcache_lock);
		dput(nd->dentry);
		nd->dentry = dentry;
		mntput(nd->mnt);
		nd->mnt = parent;
	}
}
/*
 * Name resolution.
 *
 * This is the basic name resolution function, turning a pathname
 * into the final dentry.
 *
 * We expect 'base' to be positive and a directory.
 */
// path_walk：VFS核心路径遍历函数，逐级拆分路径分量、查找dentry、处理符号链接、挂载点
// @name：待解析的路径字符串
// @nd：nameidata，路径查找上下文，保存当前mnt、dentry、查找标记
// 返回：0 成功；负数错误码失败；成功后nd保存最终dentry/mnt
int path_walk(const char * name, struct nameidata *nd)
{
	struct dentry *dentry;
	struct inode *inode;
	int err;
	unsigned int lookup_flags = nd->flags;  // 从nd取出本次查找标志LOOKUP_*

	// 跳过路径开头连续的斜杠，例如 "///a/b" -> "a/b"，相对nd->dentry的路径
	while (*name=='/')
		name++;
	if (!*name)
		goto return_base;  // 只剩空串，直接返回，nd保留当前目录

	inode = nd->dentry->d_inode;  // 当前目录的inode

	// 如果正在递归解析符号链接，强制开启LOOKUP_FOLLOW（跟随符号链接）
	if (current->link_count)
		lookup_flags = LOOKUP_FOLLOW;

	/* At this point we know we have a real path component. */
	// 主循环：循环处理每一级目录分量 a / b / c，一轮循环处理一段
	for(;;) {
		unsigned long hash;
		struct qstr this;  // qstr：保存当前路径分量（名字、长度、hash）
		unsigned int c;

		// 权限校验：进入目录需要 MAY_EXEC 权限（目录的x权限代表可进入、可查找内部文件）
		err = permission(inode, MAY_EXEC);
		dentry = ERR_PTR(err);
 		if (err)
			break;

		this.name = name;  // 记录当前分量起始地址
		c = *(const unsigned char *)name;

		hash = init_name_hash();

		// 循环读取字符，计算文件名hash，直到遇到'\0'或者'/'，截取一个路径分量
		do {
			name++;
			hash = partial_name_hash(c, hash);
			c = *(const unsigned char *)name;
		} while (c && (c != '/'));
		this.len = name - (const char *) this.name;  // 分量名字长度
		this.hash = end_name_hash(hash);  // 最终hash值，用于dentry哈希查找

		/* remove trailing slashes? */
		// c == '\0' 代表这是路径最后一个分量，没有后续目录
		if (!c)
			goto last_component;  //这个是最后一个文件或者目录了

		// 跳过分量后面连续多个斜杠，a//b 等价 a/b
		while (*++name == '/');
		if (!*name)
			goto last_with_slashes;  // 最后分量尾部带斜杠，如 a/b/，最后带/只能是目录

		/*
		 * "." and ".." are special - ".." especially so because it has
		 * to be able to know about the current root directory and
		 * parent relationships.
		 */
		// 处理特殊目录项 . 和 ..
		if (this.name[0] == '.') switch (this.len) {
			default:
				break;
			case 2:	
				if (this.name[1] != '.')
					break;
				follow_dotdot(nd);  // 处理 .. 回到父目录（会考虑chroot/mount边界）
				inode = nd->dentry->d_inode;
				/* fallthrough */
			case 1:
				continue;  // "." 当前目录，直接跳过，继续下一轮循环
		}
		/*
		 * See if the low-level filesystem might want
		 * to use its own hash..
		 */
		// 如果文件系统自定义d_hash，调用文件系统回调更新qstr的hash
		if (nd->dentry->d_op && nd->dentry->d_op->d_hash) {
			// 调用文件系统自定义d_hash函数，传入当前目录dentry、以及待查找的文件名qstr(&this)
			// 按文件系统规则，重新计算文件名 hash，填入this.hash。
			// 因为底层文件系统可能不区分文件名的大小写，所以需要重新计算hash
			err = nd->dentry->d_op->d_hash(nd->dentry, &this);
			if (err < 0)
				break;  // 回调返回负错误码，终止路径查找
		}
		/* This does the actual lookups.. */
		// 优先在dentry cache缓存查找；缓存miss再调用real_lookup从底层文件系统读取
		dentry = cached_lookup(nd->dentry, &this, LOOKUP_CONTINUE);
		if (!dentry) {
			//没有从缓存查找到，走真实查找
			// 读取文件系统的dentry数据
			dentry = real_lookup(nd->dentry, &this, LOOKUP_CONTINUE);
			err = PTR_ERR(dentry);
			if (IS_ERR(dentry))
				break;
		}
		/* Check mountpoints.. */
		// 如果当前dentry是挂载点，穿透挂载，进入子文件系统
		while (d_mountpoint(dentry) && __follow_down(&nd->mnt, &dentry))
			;

		//到这里，查找到了新的目录（或者文件）对应的dentry项目
		err = -ENOENT;

		//查找到的文件的inode
		inode = dentry->d_inode;
		if (!inode)
			goto out_dput;
		err = -ENOTDIR; 
		if (!inode->i_op)
			goto out_dput;

		// 找到符号链接，调用do_follow_link递归解析链接目标
		if (inode->i_op->follow_link) {
			err = do_follow_link(dentry, nd);
			dput(dentry);
			if (err)
				goto return_err;
			err = -ENOENT;
			inode = nd->dentry->d_inode;
			if (!inode)
				break;
			err = -ENOTDIR; 
			if (!inode->i_op)
				break;
		} else {
			// 普通目录项，替换nd中的当前dentry，释放旧dentry引用
			dput(nd->dentry);
			nd->dentry = dentry;
		}
		err = -ENOTDIR; 
		if (!inode->i_op->lookup)  // 下一级需要lookup，当前必须是目录
			break;
		continue;
		/* here ends the main loop */

last_with_slashes:
	// 路径末尾带 / ，例如 /a/b/，强制开启FOLLOW并且要求目标必须是目录
		lookup_flags |= LOOKUP_FOLLOW | LOOKUP_DIRECTORY;
last_component:
		// 到达路径最后一段分量
		if (lookup_flags & LOOKUP_PARENT)
			goto lookup_parent;  // O_CREAT场景：只解析父目录，保存最后分量到nd->last

		// 最后分量是 . / ..
		if (this.name[0] == '.') switch (this.len) {
			default:
				break;
			case 2:	
				if (this.name[1] != '.')
					break;
				follow_dotdot(nd);
				inode = nd->dentry->d_inode;
				/* fallthrough */
			case 1:
				goto return_base;
		}

		// 文件系统自定义hash
		if (nd->dentry->d_op && nd->dentry->d_op->d_hash) {
			err = nd->dentry->d_op->d_hash(nd->dentry, &this);
			if (err < 0)
				break;
		}

		// 查找最后分量，LOOKUP_CONTINUE=0
		dentry = cached_lookup(nd->dentry, &this, 0);
		if (!dentry) {
			dentry = real_lookup(nd->dentry, &this, 0);
			err = PTR_ERR(dentry);
			if (IS_ERR(dentry))
				break;
		}

		// 挂载点穿透
		while (d_mountpoint(dentry) && __follow_down(&nd->mnt, &dentry))
			;
		inode = dentry->d_inode;

		// 如果需要跟随符号链接，并且是符号链接，执行do_follow_link
		if ((lookup_flags & LOOKUP_FOLLOW)
		    && inode && inode->i_op && inode->i_op->follow_link) {
			err = do_follow_link(dentry, nd);
			dput(dentry);
			if (err)
				goto return_err;
			inode = nd->dentry->d_inode;
		} else {
			dput(nd->dentry);
			nd->dentry = dentry;
		}
		err = -ENOENT;
		if (!inode)
			goto no_inode;

		// 如果标记LOOKUP_DIRECTORY，目标必须是目录
		if (lookup_flags & LOOKUP_DIRECTORY) {
			err = -ENOTDIR; 
			if (!inode->i_op || !inode->i_op->lookup)
				break;
		}
		goto return_base;
no_inode:

		// negative dentry，目录项存在但无inode（文件不存在）
		err = -ENOENT;
		if (lookup_flags & (LOOKUP_POSITIVE|LOOKUP_DIRECTORY))
			break;
		goto return_base;
lookup_parent:

		// LOOKUP_PARENT（O_CREAT创建文件场景）
        // 解析到父目录为止，**不查找最后文件名**，把最后一段名字保存进nd->last
		nd->last = this;
		nd->last_type = LAST_NORM;
		if (this.name[0] != '.')
			goto return_base;
		if (this.len == 1)
			nd->last_type = LAST_DOT;
		else if (this.len == 2 && this.name[1] == '.')
			nd->last_type = LAST_DOTDOT;
return_base:
		return 0;  // 路径遍历成功返回
out_dput:
		dput(dentry); // 释放本次查到的dentry引用
		break;
	}
	path_release(nd);  // 出错，释放nd内部mnt/dentry引用
return_err:
	return err;
}

/* SMP-safe */
/* returns 1 if everything is done */
static int __emul_lookup_dentry(const char *name, struct nameidata *nd)
{
	if (path_walk(name, nd))
		return 0;

	if (!nd->dentry->d_inode) {
		struct nameidata nd_root;
		nd_root.last_type = LAST_ROOT;
		nd_root.flags = nd->flags;
		read_lock(&current->fs->lock);
		nd_root.mnt = mntget(current->fs->rootmnt);
		nd_root.dentry = dget(current->fs->root);
		read_unlock(&current->fs->lock);
		if (path_walk(name, &nd_root))
			return 1;
		if (nd_root.dentry->d_inode) {
			path_release(nd);
			nd->dentry = nd_root.dentry;
			nd->mnt = nd_root.mnt;
			nd->last = nd_root.last;
			return 1;
		}
		path_release(&nd_root);
	}
	return 1;
}

void set_fs_altroot(void)
{
	char *emul = __emul_prefix();
	struct nameidata nd;
	struct vfsmount *mnt = NULL, *oldmnt;
	struct dentry *dentry = NULL, *olddentry;
	if (emul) {
		read_lock(&current->fs->lock);
		nd.mnt = mntget(current->fs->rootmnt);
		nd.dentry = dget(current->fs->root);
		read_unlock(&current->fs->lock);
		nd.flags = LOOKUP_FOLLOW|LOOKUP_DIRECTORY|LOOKUP_POSITIVE;
		if (path_walk(emul,&nd) == 0) {
			mnt = nd.mnt;
			dentry = nd.dentry;
		}
	}
	write_lock(&current->fs->lock);
	oldmnt = current->fs->altrootmnt;
	olddentry = current->fs->altroot;
	current->fs->altrootmnt = mnt;
	current->fs->altroot = dentry;
	write_unlock(&current->fs->lock);
	if (olddentry) {
		dput(olddentry);
		mntput(oldmnt);
	}
}

// walk_init_root：绝对路径初始化，设置nameidata的起点为进程根目录(或altroot替代根)
// @name：待解析路径（以'/'开头的绝对路径）
// @nd：nameidata路径查找上下文
// 返回：成功返回1；altroot匹配成功直接返回0，终止普通path_walk流程
static inline int
walk_init_root(const char *name, struct nameidata *nd)
{
	// 加读锁保护进程fs_struct，防止并发修改root/altroot
	read_lock(&current->fs->lock);

	// 如果进程设置了替代根altroot，并且没有关闭altroot特性(LOOKUP_NOALT)
	if (current->fs->altroot && !(nd->flags & LOOKUP_NOALT)) {
		nd->mnt = mntget(current->fs->altrootmnt);
		nd->dentry = dget(current->fs->altroot);
		read_unlock(&current->fs->lock);
		if (__emul_lookup_dentry(name,nd))
			return 0;
		read_lock(&current->fs->lock);
	}
	nd->mnt = mntget(current->fs->rootmnt);
	nd->dentry = dget(current->fs->root);
	read_unlock(&current->fs->lock);
	return 1;
}

/* SMP-safe */
// path_init：路径查找初始化函数，初始化nameidata上下文nd，准备path_walk
// @name：待解析的文件路径字符串
// @flags：本次查找的标志位(LOOKUP_*)
// @nd：nameidata结构体，保存路径遍历的上下文
// 返回值：1 表示初始化成功；0 失败
int path_init(const char *name, unsigned int flags, struct nameidata *nd)
{
	// 默认标记最后分量为根，应对全是/的特殊路径
	nd->last_type = LAST_ROOT; /* if there are only slashes... */
	nd->flags = flags;

	// 判断是否是绝对路径（以/开头）
	if (*name=='/')
		return walk_init_root(name,nd);  // 绝对路径：初始化挂载与dentry为根目录，返回1
	
	// 相对路径，使用进程当前工作目录pwd
	read_lock(&current->fs->lock);
	nd->mnt = mntget(current->fs->pwdmnt);  // 增加pwd所在挂载实例引用计数，赋值给nd->mnt
	nd->dentry = dget(current->fs->pwd);  // 增加当前工作目录dentry引用计数，赋值给nd->dentry
	read_unlock(&current->fs->lock);
	return 1;
}

/*
 * Restricted form of lookup. Doesn't follow links, single-component only,
 * needs parent already locked. Doesn't follow mounts.
 * SMP-safe.
 */
struct dentry * lookup_hash(struct qstr *name, struct dentry * base)
{
	struct dentry * dentry;
	struct inode *inode;
	int err;

	inode = base->d_inode;
	err = permission(inode, MAY_EXEC);
	dentry = ERR_PTR(err);
	if (err)
		goto out;

	/*
	 * See if the low-level filesystem might want
	 * to use its own hash..
	 */
	if (base->d_op && base->d_op->d_hash) {
		err = base->d_op->d_hash(base, name);
		dentry = ERR_PTR(err);
		if (err < 0)
			goto out;
	}

	dentry = cached_lookup(base, name, 0);
	if (!dentry) {
		struct dentry *new = d_alloc(base, name);
		dentry = ERR_PTR(-ENOMEM);
		if (!new)
			goto out;
		lock_kernel();
		dentry = inode->i_op->lookup(inode, new);
		unlock_kernel();
		if (!dentry)
			dentry = new;
		else
			dput(new);
	}
out:
	return dentry;
}

/* SMP-safe */
struct dentry * lookup_one(const char * name, struct dentry * base)
{
	unsigned long hash;
	struct qstr this;
	unsigned int c;

	this.name = name;
	c = *(const unsigned char *)name;
	if (!c)
		goto access;

	hash = init_name_hash();
	do {
		name++;
		if (c == '/')
			goto access;
		hash = partial_name_hash(c, hash);
		c = *(const unsigned char *)name;
	} while (c);
	this.len = name - (const char *) this.name;
	this.hash = end_name_hash(hash);

	return lookup_hash(&this, base);
access:
	return ERR_PTR(-EACCES);
}

/*
 *	namei()
 *
 * is used by most simple commands to get the inode of a specified name.
 * Open, link etc use their own routines, but this is enough for things
 * like 'chmod' etc.
 *
 * namei exists in two versions: namei/lnamei. The only difference is
 * that namei follows links, while lnamei does not.
 * SMP-safe
 */
int __user_walk(const char *name, unsigned flags, struct nameidata *nd)
{
	char *tmp;
	int err;

	tmp = getname(name);
	err = PTR_ERR(tmp);
	if (!IS_ERR(tmp)) {
		err = 0;
		if (path_init(tmp, flags, nd))
			err = path_walk(tmp, nd);
		putname(tmp);
	}
	return err;
}

/*
 * It's inline, so penalty for filesystems that don't use sticky bit is
 * minimal.
 */
static inline int check_sticky(struct inode *dir, struct inode *inode)
{
	if (!(dir->i_mode & S_ISVTX))
		return 0;
	if (inode->i_uid == current->fsuid)
		return 0;
	if (dir->i_uid == current->fsuid)
		return 0;
	return !capable(CAP_FOWNER);
}

/*
 *	Check whether we can remove a link victim from directory dir, check
 *  whether the type of victim is right.
 *  1. We can't do it if dir is read-only (done in permission())
 *  2. We should have write and exec permissions on dir
 *  3. We can't remove anything from append-only dir
 *  4. We can't do anything with immutable dir (done in permission())
 *  5. If the sticky bit on dir is set we should either
 *	a. be owner of dir, or
 *	b. be owner of victim, or
 *	c. have CAP_FOWNER capability
 *  6. If the victim is append-only or immutable we can't do antyhing with
 *     links pointing to it.
 *  7. If we were asked to remove a directory and victim isn't one - ENOTDIR.
 *  8. If we were asked to remove a non-directory and victim isn't one - EISDIR.
 *  9. We can't remove a root or mountpoint.
 */
static inline int may_delete(struct inode *dir,struct dentry *victim, int isdir)
{
	int error;
	if (!victim->d_inode || victim->d_parent->d_inode != dir)
		return -ENOENT;
	error = permission(dir,MAY_WRITE | MAY_EXEC);
	if (error)
		return error;
	if (IS_APPEND(dir))
		return -EPERM;
	if (check_sticky(dir, victim->d_inode)||IS_APPEND(victim->d_inode)||
	    IS_IMMUTABLE(victim->d_inode))
		return -EPERM;
	if (isdir) {
		if (!S_ISDIR(victim->d_inode->i_mode))
			return -ENOTDIR;
		if (IS_ROOT(victim))
			return -EBUSY;
	} else if (S_ISDIR(victim->d_inode->i_mode))
		return -EISDIR;
	return 0;
}

/*	Check whether we can create an object with dentry child in directory
 *  dir.
 *  1. We can't do it if child already exists (open has special treatment for
 *     this case, but since we are inlined it's OK)
 *  2. We can't do it if dir is read-only (done in permission())
 *  3. We should have write and exec permissions on dir
 *  4. We can't do it if dir is immutable (done in permission())
 */
static inline int may_create(struct inode *dir, struct dentry *child) {
	if (child->d_inode)
		return -EEXIST;
	if (IS_DEADDIR(dir))
		return -ENOENT;
	return permission(dir,MAY_WRITE | MAY_EXEC);
}

/* 
 * Special case: O_CREAT|O_EXCL implies O_NOFOLLOW for security
 * reasons.
 *
 * O_DIRECTORY translates into forcing a directory lookup.
 */
static inline int lookup_flags(unsigned int f)
{
	unsigned long retval = LOOKUP_FOLLOW;

	if (f & O_NOFOLLOW)
		retval &= ~LOOKUP_FOLLOW;
	
	if ((f & (O_CREAT|O_EXCL)) == (O_CREAT|O_EXCL))
		retval &= ~LOOKUP_FOLLOW;
	
	if (f & O_DIRECTORY)
		retval |= LOOKUP_DIRECTORY;

	return retval;
}

int vfs_create(struct inode *dir, struct dentry *dentry, int mode)
{
	int error;

	mode &= S_IALLUGO & ~current->fs->umask;
	mode |= S_IFREG;

	down(&dir->i_zombie);
	error = may_create(dir, dentry);
	if (error)
		goto exit_lock;

	error = -EACCES;	/* shouldn't it be ENOSYS? */
	if (!dir->i_op || !dir->i_op->create)
		goto exit_lock;

	DQUOT_INIT(dir);
	lock_kernel();
	error = dir->i_op->create(dir, dentry, mode);
	unlock_kernel();
exit_lock:
	up(&dir->i_zombie);
	if (!error)
		inode_dir_notify(dir, DN_CREATE);
	return error;
}

/*
 *	open_namei()
 *
 * namei for open - this is in fact almost the whole open-routine.
 *
 * Note that the low bits of "flag" aren't the same as in the open
 * system call - they are 00 - no permissions needed
 *			  01 - read permission needed
 *			  10 - write permission needed
 *			  11 - read/write permissions needed
 * which is a lot more logical, and also allows the "no perm" needed
 * for symlinks (where the permissions are checked later).
 * SMP-safe
 */

// open_namei - VFS路径解析与打开前校验核心函数(Linux2.4)
// @pathname: 内核态文件路径字符串
// @flag: filp_open传入的内部namei_flags标志（已经完成读写位转换）
// @mode: 创建文件时的初始权限mode，不带O_CREAT时本参数无效
// @nd: nameidata结构体，保存路径查找上下文，存放解析结果dentry/vfsmount
// 返回值：成功返回0；失败返回负错误码；成功时nd中填充好目标dentry和mnt，供dentry_open使用
int open_namei(const char * pathname, int flag, int mode, struct nameidata *nd)
{
	int acc_mode, error = 0;
	struct inode *inode;
	struct dentry *dentry;
	struct dentry *dir;
	int count = 0;

	// 提取本次打开操作需要的访问权限（读/写）
	acc_mode = ACC_MODE(flag);

	/*
	 * The simplest case - just a plain lookup.
	 */
	// 分支1：不带O_CREAT，只做路径查找打开已有文件
	if (!(flag & O_CREAT)) {
		if (path_init(pathname, lookup_flags(flag), nd)) // 初始化nameidata查找上下文
			error = path_walk(pathname, nd);  // 核心路径遍历，逐级解析路径、追踪符号链接
		if (error)
			return error;
		dentry = nd->dentry;  // 拿到解析完成的目标dentry
		goto ok;
	}

	/*
	 * Create - we need to know the parent.
	 */
	// 分支2：带O_CREAT，创建文件场景，LOOKUP_PARENT表示只解析到父目录，保留最后一段文件名
	if (path_init(pathname, LOOKUP_PARENT, nd))
		error = path_walk(pathname, nd);
	if (error)
		return error;

	/*
	 * We have the parent and last component. First of all, check
	 * that we are not asked to creat(2) an obvious directory - that
	 * will not do.
	 */
	error = -EISDIR;
	// 校验最后一段文件名合法性，不能是目录类特殊项
	if (nd->last_type != LAST_NORM || nd->last.name[nd->last.len])
		goto exit;

	dir = nd->dentry;// dir保存父目录dentry
	down(&dir->d_inode->i_sem); // 获取父目录inode信号量，保护目录修改（创建文件）
	dentry = lookup_hash(&nd->last, nd->dentry);  // 在父目录哈希中查找文件名对应的dentry

do_last:
	error = PTR_ERR(dentry);
	if (IS_ERR(dentry)) {  // lookup_hash失败
		up(&dir->d_inode->i_sem);
		goto exit;
	}

	/* Negative dentry, just create the file */
	// 找到negative dentry（目录项存在，但无inode，代表文件不存在），执行新建文件
	if (!dentry->d_inode) {
		error = vfs_create(dir->d_inode, dentry, mode);  // VFS创建文件，调用具体文件系统create
		up(&dir->d_inode->i_sem);  // 释放父目录信号量
		dput(nd->dentry);
		nd->dentry = dentry;  // nd指向新建文件dentry
		if (error)
			goto exit;
		/* Don't check for write permission, don't truncate */
		acc_mode = 0;  // 新建文件，不再额外检查权限
		flag &= ~O_TRUNC;  // 新建文件不需要截断
		goto ok;
	}

	/*
	 * It already exists.
	 */
	// 文件已经存在
	up(&dir->d_inode->i_sem);

	error = -EEXIST;
	if (flag & O_EXCL)  // O_CREAT|O_EXCL要求文件不能存在，文件已存在返回EEXIST
		goto exit_dput;

	if (d_mountpoint(dentry)) {  // 命中挂载点，处理挂载穿越
		error = -ELOOP;
		if (flag & O_NOFOLLOW) // O_NOFOLLOW不跟随挂载点，报错
			goto exit_dput;
		do __follow_down(&nd->mnt,&dentry); while(d_mountpoint(dentry)); // 穿透挂载实例
	}
	error = -ENOENT;
	if (!dentry->d_inode)
		goto exit_dput;
	// 如果是符号链接，跳转到符号链接解析分支do_link
	if (dentry->d_inode->i_op && dentry->d_inode->i_op->follow_link)
		goto do_link;

	dput(nd->dentry);
	nd->dentry = dentry;
	error = -EISDIR;
	if (dentry->d_inode && S_ISDIR(dentry->d_inode->i_mode)) // 不能创建同名目录
		goto exit;
ok:
	error = -ENOENT;
	inode = dentry->d_inode;  
	if (!inode)
		goto exit;

	error = -ELOOP;
	if (S_ISLNK(inode->i_mode))  // 普通打开场景不能直接是符号链接本身
		goto exit;
	
	error = -EISDIR;
	// 禁止以写方式打开目录
	if (S_ISDIR(inode->i_mode) && (flag & FMODE_WRITE))
		goto exit;

	error = permission(inode,acc_mode);  // 核心权限检查：inode权限校验
	if (error)
		goto exit;

	/*
	 * FIFO's, sockets and device files are special: they don't
	 * actually live on the filesystem itself, and as such you
	 * can write to them even if the filesystem is read-only.
	 */
	// 特殊文件判断：管道、socket、字符/块设备不受文件系统只读限制
	if (S_ISFIFO(inode->i_mode) || S_ISSOCK(inode->i_mode)) {
	    	flag &= ~O_TRUNC;  // fifo/socket不支持截断
	} else if (S_ISBLK(inode->i_mode) || S_ISCHR(inode->i_mode)) {
		error = -EACCES;
		if (IS_NODEV(inode))  // 文件系统挂载nodev，禁止访问设备文件
			goto exit;

		flag &= ~O_TRUNC;
	} else {
		error = -EROFS;
		// 普通文件：只读文件系统不能写
		if (IS_RDONLY(inode) && (flag & 2))
			goto exit;
	}
	/*
	 * An append-only file must be opened in append mode for writing.
	 */
	error = -EPERM;
	if (IS_APPEND(inode)) {  // 追加属性文件检查
		if  ((flag & FMODE_WRITE) && !(flag & O_APPEND))  // 写打开必须带O_APPEND
			goto exit;
		if (flag & O_TRUNC)
			goto exit;
	}

	/*
	 * Ensure there are no outstanding leases on the file.
	 */
	error = get_lease(inode, flag);  // 文件租约检查
	if (error)
		goto exit;

	// O_TRUNC 截断文件逻辑
	if (flag & O_TRUNC) {
		error = get_write_access(inode);  // 获取inode写引用计数
		if (error)
			goto exit;

		/*
		 * Refuse to truncate files with mandatory locks held on them.
		 */
		error = locks_verify_locked(inode);  // 检查是否存在强制锁，有锁禁止truncate
		if (!error) {
			DQUOT_INIT(inode);  // 磁盘配额初始化
			
			error = do_truncate(dentry, 0);  // 将文件截断为0长度
		}
		put_write_access(inode);  // 释放inode写引用
		if (error)
			goto exit;
	} else
		if (flag & FMODE_WRITE)
			DQUOT_INIT(inode); // 写文件，初始化磁盘配额

	return 0;  // 所有校验全部通过，返回0，上层filp_open调用dentry_open构造struct file

exit_dput:
	dput(dentry);  // 释放dentry引用
exit:
	path_release(nd);
	return error;

do_link:
	// 符号链接处理分支，手动follow_link解析链接目标
	error = -ELOOP;
	if (flag & O_NOFOLLOW)
		goto exit_dput;
	/*
	 * This is subtle. Instead of calling do_follow_link() we do the
	 * thing by hands. The reason is that this way we have zero link_count
	 * and path_walk() (called from ->follow_link) honoring LOOKUP_PARENT.
	 * After that we have the parent and last component, i.e.
	 * we are in the same situation as after the first path_walk().
	 * Well, almost - if the last component is normal we get its copy
	 * stored in nd->last.name and we will have to putname() it when we
	 * are done. Procfs-like symlinks just set LAST_BIND.
	 */
	UPDATE_ATIME(dentry->d_inode);
	error = dentry->d_inode->i_op->follow_link(dentry, nd);
	dput(dentry);
	if (error)
		return error;
	if (nd->last_type == LAST_BIND) {
		dentry = nd->dentry;
		goto ok;
	}
	error = -EISDIR;
	if (nd->last_type != LAST_NORM)
		goto exit;
	if (nd->last.name[nd->last.len]) {
		putname(nd->last.name);
		goto exit;
	}
	if (count++==32) {
		dentry = nd->dentry;
		putname(nd->last.name);
		goto ok;
	}
	dir = nd->dentry;
	down(&dir->d_inode->i_sem);
	dentry = lookup_hash(&nd->last, nd->dentry);
	putname(nd->last.name);
	goto do_last;
}

/* SMP-safe */
static struct dentry *lookup_create(struct nameidata *nd, int is_dir)
{
	struct dentry *dentry;

	down(&nd->dentry->d_inode->i_sem);
	dentry = ERR_PTR(-EEXIST);
	if (nd->last_type != LAST_NORM)
		goto fail;
	dentry = lookup_hash(&nd->last, nd->dentry);
	if (IS_ERR(dentry))
		goto fail;
	if (!is_dir && nd->last.name[nd->last.len] && !dentry->d_inode)
		goto enoent;
	return dentry;
enoent:
	dput(dentry);
	dentry = ERR_PTR(-ENOENT);
fail:
	return dentry;
}

int vfs_mknod(struct inode *dir, struct dentry *dentry, int mode, dev_t dev)
{
	int error = -EPERM;

	mode &= ~current->fs->umask;

	down(&dir->i_zombie);
	if ((S_ISCHR(mode) || S_ISBLK(mode)) && !capable(CAP_MKNOD))
		goto exit_lock;

	error = may_create(dir, dentry);
	if (error)
		goto exit_lock;

	error = -EPERM;
	if (!dir->i_op || !dir->i_op->mknod)
		goto exit_lock;

	DQUOT_INIT(dir);
	lock_kernel();
	error = dir->i_op->mknod(dir, dentry, mode, dev);
	unlock_kernel();
exit_lock:
	up(&dir->i_zombie);
	if (!error)
		inode_dir_notify(dir, DN_CREATE);
	return error;
}

asmlinkage long sys_mknod(const char * filename, int mode, dev_t dev)
{
	int error = 0;
	char * tmp;
	struct dentry * dentry;
	struct nameidata nd;

	if (S_ISDIR(mode))
		return -EPERM;
	tmp = getname(filename);
	if (IS_ERR(tmp))
		return PTR_ERR(tmp);

	if (path_init(tmp, LOOKUP_PARENT, &nd))
		error = path_walk(tmp, &nd);
	if (error)
		goto out;
	dentry = lookup_create(&nd, 0);
	error = PTR_ERR(dentry);
	if (!IS_ERR(dentry)) {
		switch (mode & S_IFMT) {
		case 0: case S_IFREG:
			error = vfs_create(nd.dentry->d_inode,dentry,mode);
			break;
		case S_IFCHR: case S_IFBLK: case S_IFIFO: case S_IFSOCK:
			error = vfs_mknod(nd.dentry->d_inode,dentry,mode,dev);
			break;
		case S_IFDIR:
			error = -EPERM;
			break;
		default:
			error = -EINVAL;
		}
		dput(dentry);
	}
	up(&nd.dentry->d_inode->i_sem);
	path_release(&nd);
out:
	putname(tmp);

	return error;
}

int vfs_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	int error;

	down(&dir->i_zombie);
	error = may_create(dir, dentry);
	if (error)
		goto exit_lock;

	error = -EPERM;
	if (!dir->i_op || !dir->i_op->mkdir)
		goto exit_lock;

	DQUOT_INIT(dir);
	mode &= (S_IRWXUGO|S_ISVTX) & ~current->fs->umask;
	lock_kernel();
	error = dir->i_op->mkdir(dir, dentry, mode);
	unlock_kernel();

exit_lock:
	up(&dir->i_zombie);
	if (!error)
		inode_dir_notify(dir, DN_CREATE);
	return error;
}

asmlinkage long sys_mkdir(const char * pathname, int mode)
{
	int error = 0;
	char * tmp;

	tmp = getname(pathname);
	error = PTR_ERR(tmp);
	if (!IS_ERR(tmp)) {
		struct dentry *dentry;
		struct nameidata nd;

		if (path_init(tmp, LOOKUP_PARENT, &nd))
			error = path_walk(tmp, &nd);
		if (error)
			goto out;
		dentry = lookup_create(&nd, 1);
		error = PTR_ERR(dentry);
		if (!IS_ERR(dentry)) {
			error = vfs_mkdir(nd.dentry->d_inode, dentry, mode);
			dput(dentry);
		}
		up(&nd.dentry->d_inode->i_sem);
		path_release(&nd);
out:
		putname(tmp);
	}

	return error;
}

/*
 * We try to drop the dentry early: we should have
 * a usage count of 2 if we're the only user of this
 * dentry, and if that is true (possibly after pruning
 * the dcache), then we drop the dentry now.
 *
 * A low-level filesystem can, if it choses, legally
 * do a
 *
 *	if (!d_unhashed(dentry))
 *		return -EBUSY;
 *
 * if it cannot handle the case of removing a directory
 * that is still in use by something else..
 */
static void d_unhash(struct dentry *dentry)
{
	dget(dentry);
	switch (atomic_read(&dentry->d_count)) {
	default:
		shrink_dcache_parent(dentry);
		if (atomic_read(&dentry->d_count) != 2)
			break;
	case 2:
		d_drop(dentry);
	}
}

int vfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	int error;

	error = may_delete(dir, dentry, 1);
	if (error)
		return error;

	if (!dir->i_op || !dir->i_op->rmdir)
		return -EPERM;

	DQUOT_INIT(dir);

	double_down(&dir->i_zombie, &dentry->d_inode->i_zombie);
	d_unhash(dentry);
	if (IS_DEADDIR(dir))
		error = -ENOENT;
	else if (d_mountpoint(dentry))
		error = -EBUSY;
	else {
		lock_kernel();
		error = dir->i_op->rmdir(dir, dentry);
		unlock_kernel();
		if (!error)
			dentry->d_inode->i_flags |= S_DEAD;
	}
	double_up(&dir->i_zombie, &dentry->d_inode->i_zombie);
	if (!error) {
		inode_dir_notify(dir, DN_DELETE);
		d_delete(dentry);
	}
	dput(dentry);

	return error;
}

asmlinkage long sys_rmdir(const char * pathname)
{
	int error = 0;
	char * name;
	struct dentry *dentry;
	struct nameidata nd;

	name = getname(pathname);
	if(IS_ERR(name))
		return PTR_ERR(name);

	if (path_init(name, LOOKUP_PARENT, &nd))
		error = path_walk(name, &nd);
	if (error)
		goto exit;

	switch(nd.last_type) {
		case LAST_DOTDOT:
			error = -ENOTEMPTY;
			goto exit1;
		case LAST_DOT:
			error = -EINVAL;
			goto exit1;
		case LAST_ROOT:
			error = -EBUSY;
			goto exit1;
	}
	down(&nd.dentry->d_inode->i_sem);
	dentry = lookup_hash(&nd.last, nd.dentry);
	error = PTR_ERR(dentry);
	if (!IS_ERR(dentry)) {
		error = vfs_rmdir(nd.dentry->d_inode, dentry);
		dput(dentry);
	}
	up(&nd.dentry->d_inode->i_sem);
exit1:
	path_release(&nd);
exit:
	putname(name);
	return error;
}

int vfs_unlink(struct inode *dir, struct dentry *dentry)
{
	int error;

	down(&dir->i_zombie);
	error = may_delete(dir, dentry, 0);
	if (!error) {
		error = -EPERM;
		if (dir->i_op && dir->i_op->unlink) {
			DQUOT_INIT(dir);
			if (d_mountpoint(dentry))
				error = -EBUSY;
			else {
				lock_kernel();
				error = dir->i_op->unlink(dir, dentry);
				unlock_kernel();
				if (!error)
					d_delete(dentry);
			}
		}
	}
	up(&dir->i_zombie);
	if (!error)
		inode_dir_notify(dir, DN_DELETE);
	return error;
}

asmlinkage long sys_unlink(const char * pathname)
{
	int error = 0;
	char * name;
	struct dentry *dentry;
	struct nameidata nd;

	name = getname(pathname);
	if(IS_ERR(name))
		return PTR_ERR(name);

	if (path_init(name, LOOKUP_PARENT, &nd))
		error = path_walk(name, &nd);
	if (error)
		goto exit;
	error = -EISDIR;
	if (nd.last_type != LAST_NORM)
		goto exit1;
	down(&nd.dentry->d_inode->i_sem);
	dentry = lookup_hash(&nd.last, nd.dentry);
	error = PTR_ERR(dentry);
	if (!IS_ERR(dentry)) {
		/* Why not before? Because we want correct error value */
		if (nd.last.name[nd.last.len])
			goto slashes;
		error = vfs_unlink(nd.dentry->d_inode, dentry);
	exit2:
		dput(dentry);
	}
	up(&nd.dentry->d_inode->i_sem);
exit1:
	path_release(&nd);
exit:
	putname(name);

	return error;

slashes:
	error = !dentry->d_inode ? -ENOENT :
		S_ISDIR(dentry->d_inode->i_mode) ? -EISDIR : -ENOTDIR;
	goto exit2;
}

int vfs_symlink(struct inode *dir, struct dentry *dentry, const char *oldname)
{
	int error;

	down(&dir->i_zombie);
	error = may_create(dir, dentry);
	if (error)
		goto exit_lock;

	error = -EPERM;
	if (!dir->i_op || !dir->i_op->symlink)
		goto exit_lock;

	DQUOT_INIT(dir);
	lock_kernel();
	error = dir->i_op->symlink(dir, dentry, oldname);
	unlock_kernel();

exit_lock:
	up(&dir->i_zombie);
	if (!error)
		inode_dir_notify(dir, DN_CREATE);
	return error;
}

asmlinkage long sys_symlink(const char * oldname, const char * newname)
{
	int error = 0;
	char * from;
	char * to;

	from = getname(oldname);
	if(IS_ERR(from))
		return PTR_ERR(from);
	to = getname(newname);
	error = PTR_ERR(to);
	if (!IS_ERR(to)) {
		struct dentry *dentry;
		struct nameidata nd;

		if (path_init(to, LOOKUP_PARENT, &nd))
			error = path_walk(to, &nd);
		if (error)
			goto out;
		dentry = lookup_create(&nd, 0);
		error = PTR_ERR(dentry);
		if (!IS_ERR(dentry)) {
			error = vfs_symlink(nd.dentry->d_inode, dentry, from);
			dput(dentry);
		}
		up(&nd.dentry->d_inode->i_sem);
		path_release(&nd);
out:
		putname(to);
	}
	putname(from);
	return error;
}

int vfs_link(struct dentry *old_dentry, struct inode *dir, struct dentry *new_dentry)
{
	struct inode *inode;
	int error;

	down(&dir->i_zombie);
	error = -ENOENT;
	inode = old_dentry->d_inode;
	if (!inode)
		goto exit_lock;

	error = may_create(dir, new_dentry);
	if (error)
		goto exit_lock;

	error = -EXDEV;
	if (dir->i_dev != inode->i_dev)
		goto exit_lock;

	/*
	 * A link to an append-only or immutable file cannot be created.
	 */
	error = -EPERM;
	if (IS_APPEND(inode) || IS_IMMUTABLE(inode))
		goto exit_lock;
	if (!dir->i_op || !dir->i_op->link)
		goto exit_lock;

	DQUOT_INIT(dir);
	lock_kernel();
	error = dir->i_op->link(old_dentry, dir, new_dentry);
	unlock_kernel();

exit_lock:
	up(&dir->i_zombie);
	if (!error)
		inode_dir_notify(dir, DN_CREATE);
	return error;
}

/*
 * Hardlinks are often used in delicate situations.  We avoid
 * security-related surprises by not following symlinks on the
 * newname.  --KAB
 *
 * We don't follow them on the oldname either to be compatible
 * with linux 2.0, and to avoid hard-linking to directories
 * and other special files.  --ADM
 */
asmlinkage long sys_link(const char * oldname, const char * newname)
{
	int error;
	char * from;
	char * to;

	from = getname(oldname);
	if(IS_ERR(from))
		return PTR_ERR(from);
	to = getname(newname);
	error = PTR_ERR(to);
	if (!IS_ERR(to)) {
		struct dentry *new_dentry;
		struct nameidata nd, old_nd;

		error = 0;
		if (path_init(from, LOOKUP_POSITIVE, &old_nd))
			error = path_walk(from, &old_nd);
		if (error)
			goto exit;
		if (path_init(to, LOOKUP_PARENT, &nd))
			error = path_walk(to, &nd);
		if (error)
			goto out;
		error = -EXDEV;
		if (old_nd.mnt != nd.mnt)
			goto out_release;
		new_dentry = lookup_create(&nd, 0);
		error = PTR_ERR(new_dentry);
		if (!IS_ERR(new_dentry)) {
			error = vfs_link(old_nd.dentry, nd.dentry->d_inode, new_dentry);
			dput(new_dentry);
		}
		up(&nd.dentry->d_inode->i_sem);
out_release:
		path_release(&nd);
out:
		path_release(&old_nd);
exit:
		putname(to);
	}
	putname(from);

	return error;
}

/*
 * The worst of all namespace operations - renaming directory. "Perverted"
 * doesn't even start to describe it. Somebody in UCB had a heck of a trip...
 * Problems:
 *	a) we can get into loop creation. Check is done in is_subdir().
 *	b) race potential - two innocent renames can create a loop together.
 *	   That's where 4.4 screws up. Current fix: serialization on
 *	   sb->s_vfs_rename_sem. We might be more accurate, but that's another
 *	   story.
 *	c) we have to lock _three_ objects - parents and victim (if it exists).
 *	   And that - after we got ->i_sem on parents (until then we don't know
 *	   whether the target exists at all, let alone whether it is a directory
 *	   or not). Solution: ->i_zombie. Taken only after ->i_sem. Always taken
 *	   on link creation/removal of any kind. And taken (without ->i_sem) on
 *	   directory that will be removed (both in rmdir() and here).
 *	d) some filesystems don't support opened-but-unlinked directories,
 *	   either because of layout or because they are not ready to deal with
 *	   all cases correctly. The latter will be fixed (taking this sort of
 *	   stuff into VFS), but the former is not going away. Solution: the same
 *	   trick as in rmdir().
 *	e) conversion from fhandle to dentry may come in the wrong moment - when
 *	   we are removing the target. Solution: we will have to grab ->i_zombie
 *	   in the fhandle_to_dentry code. [FIXME - current nfsfh.c relies on
 *	   ->i_sem on parents, which works but leads to some truely excessive
 *	   locking].
 */
int vfs_rename_dir(struct inode *old_dir, struct dentry *old_dentry,
	       struct inode *new_dir, struct dentry *new_dentry)
{
	int error;
	struct inode *target;

	if (old_dentry->d_inode == new_dentry->d_inode)
		return 0;

	error = may_delete(old_dir, old_dentry, 1);
	if (error)
		return error;

	if (new_dir->i_dev != old_dir->i_dev)
		return -EXDEV;

	if (!new_dentry->d_inode)
		error = may_create(new_dir, new_dentry);
	else
		error = may_delete(new_dir, new_dentry, 1);
	if (error)
		return error;

	if (!old_dir->i_op || !old_dir->i_op->rename)
		return -EPERM;

	/*
	 * If we are going to change the parent - check write permissions,
	 * we'll need to flip '..'.
	 */
	if (new_dir != old_dir) {
		error = permission(old_dentry->d_inode, MAY_WRITE);
	}
	if (error)
		return error;

	DQUOT_INIT(old_dir);
	DQUOT_INIT(new_dir);
	down(&old_dir->i_sb->s_vfs_rename_sem);
	error = -EINVAL;
	if (is_subdir(new_dentry, old_dentry))
		goto out_unlock;
	target = new_dentry->d_inode;
	if (target) { /* Hastur! Hastur! Hastur! */
		triple_down(&old_dir->i_zombie,
			    &new_dir->i_zombie,
			    &target->i_zombie);
		d_unhash(new_dentry);
	} else
		double_down(&old_dir->i_zombie,
			    &new_dir->i_zombie);
	if (IS_DEADDIR(old_dir)||IS_DEADDIR(new_dir))
		error = -ENOENT;
	else if (d_mountpoint(old_dentry)||d_mountpoint(new_dentry))
		error = -EBUSY;
	else 
		error = old_dir->i_op->rename(old_dir, old_dentry, new_dir, new_dentry);
	if (target) {
		if (!error)
			target->i_flags |= S_DEAD;
		triple_up(&old_dir->i_zombie,
			  &new_dir->i_zombie,
			  &target->i_zombie);
		if (d_unhashed(new_dentry))
			d_rehash(new_dentry);
		dput(new_dentry);
	} else
		double_up(&old_dir->i_zombie,
			  &new_dir->i_zombie);
		
	if (!error)
		d_move(old_dentry,new_dentry);
out_unlock:
	up(&old_dir->i_sb->s_vfs_rename_sem);
	return error;
}

int vfs_rename_other(struct inode *old_dir, struct dentry *old_dentry,
	       struct inode *new_dir, struct dentry *new_dentry)
{
	int error;

	if (old_dentry->d_inode == new_dentry->d_inode)
		return 0;

	error = may_delete(old_dir, old_dentry, 0);
	if (error)
		return error;

	if (new_dir->i_dev != old_dir->i_dev)
		return -EXDEV;

	if (!new_dentry->d_inode)
		error = may_create(new_dir, new_dentry);
	else
		error = may_delete(new_dir, new_dentry, 0);
	if (error)
		return error;

	if (!old_dir->i_op || !old_dir->i_op->rename)
		return -EPERM;

	DQUOT_INIT(old_dir);
	DQUOT_INIT(new_dir);
	double_down(&old_dir->i_zombie, &new_dir->i_zombie);
	if (d_mountpoint(old_dentry)||d_mountpoint(new_dentry))
		error = -EBUSY;
	else
		error = old_dir->i_op->rename(old_dir, old_dentry, new_dir, new_dentry);
	double_up(&old_dir->i_zombie, &new_dir->i_zombie);
	if (error)
		return error;
	/* The following d_move() should become unconditional */
	if (!(old_dir->i_sb->s_type->fs_flags & FS_ODD_RENAME)) {
		d_move(old_dentry, new_dentry);
	}
	return 0;
}

int vfs_rename(struct inode *old_dir, struct dentry *old_dentry,
	       struct inode *new_dir, struct dentry *new_dentry)
{
	int error;
	if (S_ISDIR(old_dentry->d_inode->i_mode))
		error = vfs_rename_dir(old_dir,old_dentry,new_dir,new_dentry);
	else
		error = vfs_rename_other(old_dir,old_dentry,new_dir,new_dentry);
	if (!error) {
		if (old_dir == new_dir)
			inode_dir_notify(old_dir, DN_RENAME);
		else {
			inode_dir_notify(old_dir, DN_DELETE);
			inode_dir_notify(new_dir, DN_CREATE);
		}
	}
	return error;
}

static inline int do_rename(const char * oldname, const char * newname)
{
	int error = 0;
	struct dentry * old_dir, * new_dir;
	struct dentry * old_dentry, *new_dentry;
	struct nameidata oldnd, newnd;

	if (path_init(oldname, LOOKUP_PARENT, &oldnd))
		error = path_walk(oldname, &oldnd);

	if (error)
		goto exit;

	if (path_init(newname, LOOKUP_PARENT, &newnd))
		error = path_walk(newname, &newnd);
	if (error)
		goto exit1;

	error = -EXDEV;
	if (oldnd.mnt != newnd.mnt)
		goto exit2;

	old_dir = oldnd.dentry;
	error = -EBUSY;
	if (oldnd.last_type != LAST_NORM)
		goto exit2;

	new_dir = newnd.dentry;
	if (newnd.last_type != LAST_NORM)
		goto exit2;

	double_lock(new_dir, old_dir);

	old_dentry = lookup_hash(&oldnd.last, old_dir);
	error = PTR_ERR(old_dentry);
	if (IS_ERR(old_dentry))
		goto exit3;
	/* source must exist */
	error = -ENOENT;
	if (!old_dentry->d_inode)
		goto exit4;
	/* unless the source is a directory trailing slashes give -ENOTDIR */
	if (!S_ISDIR(old_dentry->d_inode->i_mode)) {
		error = -ENOTDIR;
		if (oldnd.last.name[oldnd.last.len])
			goto exit4;
		if (newnd.last.name[newnd.last.len])
			goto exit4;
	}
	new_dentry = lookup_hash(&newnd.last, new_dir);
	error = PTR_ERR(new_dentry);
	if (IS_ERR(new_dentry))
		goto exit4;

	lock_kernel();
	error = vfs_rename(old_dir->d_inode, old_dentry,
				   new_dir->d_inode, new_dentry);
	unlock_kernel();

	dput(new_dentry);
exit4:
	dput(old_dentry);
exit3:
	double_up(&new_dir->d_inode->i_sem, &old_dir->d_inode->i_sem);
exit2:
	path_release(&newnd);
exit1:
	path_release(&oldnd);
exit:
	return error;
}

asmlinkage long sys_rename(const char * oldname, const char * newname)
{
	int error;
	char * from;
	char * to;

	from = getname(oldname);
	if(IS_ERR(from))
		return PTR_ERR(from);
	to = getname(newname);
	error = PTR_ERR(to);
	if (!IS_ERR(to)) {
		error = do_rename(from,to);
		putname(to);
	}
	putname(from);
	return error;
}

int vfs_readlink(struct dentry *dentry, char *buffer, int buflen, const char *link)
{
	int len;

	len = PTR_ERR(link);
	if (IS_ERR(link))
		goto out;

	len = strlen(link);
	if (len > (unsigned) buflen)
		len = buflen;
	if (copy_to_user(buffer, link, len))
		len = -EFAULT;
out:
	return len;
}

static inline int
__vfs_follow_link(struct nameidata *nd, const char *link)
{
	int res = 0;
	char *name;
	if (IS_ERR(link))
		goto fail;

	if (*link == '/') {
		path_release(nd);
		if (!walk_init_root(link, nd))
			/* weird __emul_prefix() stuff did it */
			goto out;
	}
	res = path_walk(link, nd);
out:
	if (current->link_count || res || nd->last_type!=LAST_NORM)
		return res;
	/*
	 * If it is an iterative symlinks resolution in open_namei() we
	 * have to copy the last component. And all that crap because of
	 * bloody create() on broken symlinks. Furrfu...
	 */
	name = __getname();
	if (IS_ERR(name))
		goto fail_name;
	strcpy(name, nd->last.name);
	nd->last.name = name;
	return 0;
fail_name:
	link = name;
fail:
	path_release(nd);
	return PTR_ERR(link);
}

int vfs_follow_link(struct nameidata *nd, const char *link)
{
	return __vfs_follow_link(nd, link);
}

/* get the link contents into pagecache */
static char *page_getlink(struct dentry * dentry, struct page **ppage)
{
	struct page * page;
	struct address_space *mapping = dentry->d_inode->i_mapping;
	page = read_cache_page(mapping, 0, (filler_t *)mapping->a_ops->readpage,
				NULL);
	if (IS_ERR(page))
		goto sync_fail;
	wait_on_page(page);
	if (!Page_Uptodate(page))
		goto async_fail;
	*ppage = page;
	return kmap(page);

async_fail:
	page_cache_release(page);
	return ERR_PTR(-EIO);

sync_fail:
	return (char*)page;
}

int page_readlink(struct dentry *dentry, char *buffer, int buflen)
{
	struct page *page = NULL;
	char *s = page_getlink(dentry, &page);
	int res = vfs_readlink(dentry,buffer,buflen,s);
	if (page) {
		kunmap(page);
		page_cache_release(page);
	}
	return res;
}

int page_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	struct page *page = NULL;
	char *s = page_getlink(dentry, &page);
	int res = __vfs_follow_link(nd, s);
	if (page) {
		kunmap(page);
		page_cache_release(page);
	}
	return res;
}

struct inode_operations page_symlink_inode_operations = {
	readlink:	page_readlink,
	follow_link:	page_follow_link,
};
