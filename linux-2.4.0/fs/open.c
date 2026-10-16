/*
 *  linux/fs/open.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/string.h>
#include <linux/mm.h>
#include <linux/utime.h>
#include <linux/file.h>
#include <linux/smp_lock.h>
#include <linux/quotaops.h>
#include <linux/dnotify.h>
#include <linux/module.h>
#include <linux/slab.h>

#include <asm/uaccess.h>

#define special_file(m) (S_ISCHR(m)||S_ISBLK(m)||S_ISFIFO(m)||S_ISSOCK(m))

int vfs_statfs(struct super_block *sb, struct statfs *buf)
{
	int retval = -ENODEV;

	if (sb) {
		retval = -ENOSYS;
		if (sb->s_op && sb->s_op->statfs) {
			memset(buf, 0, sizeof(struct statfs));
			lock_kernel();
			retval = sb->s_op->statfs(sb, buf);
			unlock_kernel();
		}
	}
	return retval;
}


asmlinkage long sys_statfs(const char * path, struct statfs * buf)
{
	struct nameidata nd;
	int error;

	error = user_path_walk(path, &nd);
	if (!error) {
		struct statfs tmp;
		error = vfs_statfs(nd.dentry->d_inode->i_sb, &tmp);
		if (!error && copy_to_user(buf, &tmp, sizeof(struct statfs)))
			error = -EFAULT;
		path_release(&nd);
	}
	return error;
}

asmlinkage long sys_fstatfs(unsigned int fd, struct statfs * buf)
{
	struct file * file;
	struct statfs tmp;
	int error;

	error = -EBADF;
	file = fget(fd);
	if (!file)
		goto out;
	error = vfs_statfs(file->f_dentry->d_inode->i_sb, &tmp);
	if (!error && copy_to_user(buf, &tmp, sizeof(struct statfs)))
		error = -EFAULT;
	fput(file);
out:
	return error;
}

int do_truncate(struct dentry *dentry, loff_t length)
{
	struct inode *inode = dentry->d_inode;
	int error;
	struct iattr newattrs;

	/* Not pretty: "inode->i_size" shouldn't really be signed. But it is. */
	if (length < 0)
		return -EINVAL;

	down(&inode->i_sem);
	newattrs.ia_size = length;
	newattrs.ia_valid = ATTR_SIZE | ATTR_CTIME;
	error = notify_change(dentry, &newattrs);
	up(&inode->i_sem);
	return error;
}

static inline long do_sys_truncate(const char * path, loff_t length)
{
	struct nameidata nd;
	struct inode * inode;
	int error;

	error = -EINVAL;
	if (length < 0)	/* sorry, but loff_t says... */
		goto out;

	error = user_path_walk(path, &nd);
	if (error)
		goto out;
	inode = nd.dentry->d_inode;

	error = -EACCES;
	if (!S_ISREG(inode->i_mode))
		goto dput_and_out;

	error = permission(inode,MAY_WRITE);
	if (error)
		goto dput_and_out;

	error = -EROFS;
	if (IS_RDONLY(inode))
		goto dput_and_out;

	error = -EPERM;
	if (IS_IMMUTABLE(inode) || IS_APPEND(inode))
		goto dput_and_out;

	/*
	 * Make sure that there are no leases.
	 */
	error = get_lease(inode, FMODE_WRITE);
	if (error)
		goto dput_and_out;

	error = get_write_access(inode);
	if (error)
		goto dput_and_out;

	error = locks_verify_truncate(inode, NULL, length);
	if (!error) {
		DQUOT_INIT(inode);
		error = do_truncate(nd.dentry, length);
	}
	put_write_access(inode);

dput_and_out:
	path_release(&nd);
out:
	return error;
}

asmlinkage long sys_truncate(const char * path, unsigned long length)
{
	return do_sys_truncate(path, length);
}

static inline long do_sys_ftruncate(unsigned int fd, loff_t length)
{
	struct inode * inode;
	struct dentry *dentry;
	struct file * file;
	int error;

	error = -EINVAL;
	if (length < 0)
		goto out;
	error = -EBADF;
	file = fget(fd);
	if (!file)
		goto out;
	dentry = file->f_dentry;
	inode = dentry->d_inode;
	error = -EACCES;
	if (!S_ISREG(inode->i_mode) || !(file->f_mode & FMODE_WRITE))
		goto out_putf;
	error = -EPERM;
	if (IS_IMMUTABLE(inode) || IS_APPEND(inode))
		goto out_putf;

	error = locks_verify_truncate(inode, file, length);
	if (!error)
		error = do_truncate(dentry, length);
out_putf:
	fput(file);
out:
	return error;
}

asmlinkage long sys_ftruncate(unsigned int fd, unsigned long length)
{
	return do_sys_ftruncate(fd, length);
}

/* LFS versions of truncate are only needed on 32 bit machines */
#if BITS_PER_LONG == 32
asmlinkage long sys_truncate64(const char * path, loff_t length)
{
	return do_sys_truncate(path, length);
}

asmlinkage long sys_ftruncate64(unsigned int fd, loff_t length)
{
	return do_sys_ftruncate(fd, length);
}
#endif

#if !(defined(__alpha__) || defined(__ia64__))

/*
 * sys_utime() can be implemented in user-level using sys_utimes().
 * Is this for backwards compatibility?  If so, why not move it
 * into the appropriate arch directory (for those architectures that
 * need it).
 */

/* If times==NULL, set access and modification to current time,
 * must be owner or have write permission.
 * Else, update from *times, must be owner or super user.
 */
asmlinkage long sys_utime(char * filename, struct utimbuf * times)
{
	int error;
	struct nameidata nd;
	struct inode * inode;
	struct iattr newattrs;

	error = user_path_walk(filename, &nd);
	if (error)
		goto out;
	inode = nd.dentry->d_inode;

	error = -EROFS;
	if (IS_RDONLY(inode))
		goto dput_and_out;

	/* Don't worry, the checks are done in inode_change_ok() */
	newattrs.ia_valid = ATTR_CTIME | ATTR_MTIME | ATTR_ATIME;
	if (times) {
		error = get_user(newattrs.ia_atime, &times->actime);
		if (!error) 
			error = get_user(newattrs.ia_mtime, &times->modtime);
		if (error)
			goto dput_and_out;

		newattrs.ia_valid |= ATTR_ATIME_SET | ATTR_MTIME_SET;
	} else {
		if (current->fsuid != inode->i_uid &&
		    (error = permission(inode,MAY_WRITE)) != 0)
			goto dput_and_out;
	}
	error = notify_change(nd.dentry, &newattrs);
dput_and_out:
	path_release(&nd);
out:
	return error;
}

#endif

/* If times==NULL, set access and modification to current time,
 * must be owner or have write permission.
 * Else, update from *times, must be owner or super user.
 */
asmlinkage long sys_utimes(char * filename, struct timeval * utimes)
{
	int error;
	struct nameidata nd;
	struct inode * inode;
	struct iattr newattrs;

	error = user_path_walk(filename, &nd);

	if (error)
		goto out;
	inode = nd.dentry->d_inode;

	error = -EROFS;
	if (IS_RDONLY(inode))
		goto dput_and_out;

	/* Don't worry, the checks are done in inode_change_ok() */
	newattrs.ia_valid = ATTR_CTIME | ATTR_MTIME | ATTR_ATIME;
	if (utimes) {
		struct timeval times[2];
		error = -EFAULT;
		if (copy_from_user(&times, utimes, sizeof(times)))
			goto dput_and_out;
		newattrs.ia_atime = times[0].tv_sec;
		newattrs.ia_mtime = times[1].tv_sec;
		newattrs.ia_valid |= ATTR_ATIME_SET | ATTR_MTIME_SET;
	} else {
		if ((error = permission(inode,MAY_WRITE)) != 0)
			goto dput_and_out;
	}
	error = notify_change(nd.dentry, &newattrs);
dput_and_out:
	path_release(&nd);
out:
	return error;
}

/*
 * access() needs to use the real uid/gid, not the effective uid/gid.
 * We do this by temporarily clearing all FS-related capabilities and
 * switching the fsuid/fsgid around to the real ones.
 */
asmlinkage long sys_access(const char * filename, int mode)
{
	struct nameidata nd;
	int old_fsuid, old_fsgid;
	kernel_cap_t old_cap;
	int res;

	if (mode & ~S_IRWXO)	/* where's F_OK, X_OK, W_OK, R_OK? */
		return -EINVAL;

	old_fsuid = current->fsuid;
	old_fsgid = current->fsgid;
	old_cap = current->cap_effective;

	current->fsuid = current->uid;
	current->fsgid = current->gid;

	/* Clear the capabilities if we switch to a non-root user */
	if (current->uid)
		cap_clear(current->cap_effective);
	else
		current->cap_effective = current->cap_permitted;

	res = user_path_walk(filename, &nd);
	if (!res) {
		res = permission(nd.dentry->d_inode, mode);
		/* SuS v2 requires we report a read only fs too */
		if(!res && (mode & S_IWOTH) && IS_RDONLY(nd.dentry->d_inode)
		   && !special_file(nd.dentry->d_inode->i_mode))
			res = -EROFS;
		path_release(&nd);
	}

	current->fsuid = old_fsuid;
	current->fsgid = old_fsgid;
	current->cap_effective = old_cap;

	return res;
}

asmlinkage long sys_chdir(const char * filename)
{
	int error;
	struct nameidata nd;
	char *name;

	name = getname(filename);
	error = PTR_ERR(name);
	if (IS_ERR(name))
		goto out;

	error = 0;
	if (path_init(name,LOOKUP_POSITIVE|LOOKUP_FOLLOW|LOOKUP_DIRECTORY,&nd))
		error = path_walk(name, &nd);
	putname(name);
	if (error)
		goto out;

	error = permission(nd.dentry->d_inode,MAY_EXEC);
	if (error)
		goto dput_and_out;

	set_fs_pwd(current->fs, nd.mnt, nd.dentry);

dput_and_out:
	path_release(&nd);
out:
	return error;
}

asmlinkage long sys_fchdir(unsigned int fd)
{
	struct file *file;
	struct dentry *dentry;
	struct inode *inode;
	struct vfsmount *mnt;
	int error;

	error = -EBADF;
	file = fget(fd);
	if (!file)
		goto out;

	dentry = file->f_dentry;
	mnt = file->f_vfsmnt;
	inode = dentry->d_inode;

	error = -ENOTDIR;
	if (!S_ISDIR(inode->i_mode))
		goto out_putf;

	error = permission(inode, MAY_EXEC);
	if (!error)
		set_fs_pwd(current->fs, mnt, dentry);
out_putf:
	fput(file);
out:
	return error;
}

asmlinkage long sys_chroot(const char * filename)
{
	int error;
	struct nameidata nd;
	char *name;

	name = getname(filename);
	error = PTR_ERR(name);
	if (IS_ERR(name))
		goto out;

	path_init(name, LOOKUP_POSITIVE | LOOKUP_FOLLOW |
		      LOOKUP_DIRECTORY | LOOKUP_NOALT, &nd);
	error = path_walk(name, &nd);	
	putname(name);
	if (error)
		goto out;

	error = permission(nd.dentry->d_inode,MAY_EXEC);
	if (error)
		goto dput_and_out;

	error = -EPERM;
	if (!capable(CAP_SYS_CHROOT))
		goto dput_and_out;

	set_fs_root(current->fs, nd.mnt, nd.dentry);
	set_fs_altroot();
	error = 0;
dput_and_out:
	path_release(&nd);
out:
	return error;
}

asmlinkage long sys_fchmod(unsigned int fd, mode_t mode)
{
	struct inode * inode;
	struct dentry * dentry;
	struct file * file;
	int err = -EBADF;
	struct iattr newattrs;

	file = fget(fd);
	if (!file)
		goto out;

	dentry = file->f_dentry;
	inode = dentry->d_inode;

	err = -EROFS;
	if (IS_RDONLY(inode))
		goto out_putf;
	err = -EPERM;
	if (IS_IMMUTABLE(inode) || IS_APPEND(inode))
		goto out_putf;
	if (mode == (mode_t) -1)
		mode = inode->i_mode;
	newattrs.ia_mode = (mode & S_IALLUGO) | (inode->i_mode & ~S_IALLUGO);
	newattrs.ia_valid = ATTR_MODE | ATTR_CTIME;
	err = notify_change(dentry, &newattrs);

out_putf:
	fput(file);
out:
	return err;
}

// sys_chmod：chmod系统调用入口，修改文件inode权限mode
// asmlinkage：x86-32系统调用，参数从栈上获取
// @filename：用户态传入文件路径字符串
// @mode：要设置的权限模式
// 返回：0成功；负数负错误码
asmlinkage long sys_chmod(const char * filename, mode_t mode)
{
	struct nameidata nd;  // 路径查找上下文，存放dentry、vfsmount
	struct inode * inode;  // 目标文件inode
	int error;
	struct iattr newattrs; // inode属性修改描述结构体，告诉内核要改哪些字段

	// user_path_walk：解析用户态路径，找到目标文件dentry，填充nd
	error = user_path_walk(filename, &nd);
	if (error)
		goto out;

	// 拿到对应的inode
	inode = nd.dentry->d_inode;

	error = -EROFS;

	// 判断inode所在文件系统是否只读（只读挂载）
	if (IS_RDONLY(inode))
		goto dput_and_out;

	error = -EPERM;

	// 判断文件是否设置了不可变位 / 追加位，这两种文件禁止修改权限
	if (IS_IMMUTABLE(inode) || IS_APPEND(inode))
		goto dput_and_out;

	// mode == -1 代表不修改权限，沿用原有inode->i_mode
	if (mode == (mode_t) -1)
		mode = inode->i_mode;
	
	// S_IALLUGO：权限掩码(rwxrwxrwx)，只替换权限位，保留inode->i_mode的文件类型位
	newattrs.ia_mode = (mode & S_IALLUGO) | (inode->i_mode & ~S_IALLUGO);

	// ia_valid标记本次要修改的属性：ATTR_MODE(权限) + ATTR_CTIME(更新inode变更时间ctime)
	newattrs.ia_valid = ATTR_MODE | ATTR_CTIME;

	// notify_change：VFS核心函数，提交inode属性变更，调用文件系统setattr回调
	error = notify_change(nd.dentry, &newattrs);

dput_and_out:
	path_release(&nd);
out:
	return error;
}

static int chown_common(struct dentry * dentry, uid_t user, gid_t group)
{
	struct inode * inode;
	int error;
	struct iattr newattrs;

	error = -ENOENT;
	if (!(inode = dentry->d_inode)) {
		printk("chown_common: NULL inode\n");
		goto out;
	}
	error = -EROFS;
	if (IS_RDONLY(inode))
		goto out;
	error = -EPERM;
	if (IS_IMMUTABLE(inode) || IS_APPEND(inode))
		goto out;
	if (user == (uid_t) -1)
		user = inode->i_uid;
	if (group == (gid_t) -1)
		group = inode->i_gid;
	newattrs.ia_mode = inode->i_mode;
	newattrs.ia_uid = user;
	newattrs.ia_gid = group;
	newattrs.ia_valid =  ATTR_UID | ATTR_GID | ATTR_CTIME;
	/*
	 * If the user or group of a non-directory has been changed by a
	 * non-root user, remove the setuid bit.
	 * 19981026	David C Niemi <niemi@tux.org>
	 *
	 * Changed this to apply to all users, including root, to avoid
	 * some races. This is the behavior we had in 2.0. The check for
	 * non-root was definitely wrong for 2.2 anyway, as it should
	 * have been using CAP_FSETID rather than fsuid -- 19990830 SD.
	 */
	if ((inode->i_mode & S_ISUID) == S_ISUID &&
		!S_ISDIR(inode->i_mode))
	{
		newattrs.ia_mode &= ~S_ISUID;
		newattrs.ia_valid |= ATTR_MODE;
	}
	/*
	 * Likewise, if the user or group of a non-directory has been changed
	 * by a non-root user, remove the setgid bit UNLESS there is no group
	 * execute bit (this would be a file marked for mandatory locking).
	 * 19981026	David C Niemi <niemi@tux.org>
	 *
	 * Removed the fsuid check (see the comment above) -- 19990830 SD.
	 */
	if (((inode->i_mode & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP)) 
		&& !S_ISDIR(inode->i_mode))
	{
		newattrs.ia_mode &= ~S_ISGID;
		newattrs.ia_valid |= ATTR_MODE;
	}
	error = DQUOT_TRANSFER(dentry, &newattrs);
out:
	return error;
}

asmlinkage long sys_chown(const char * filename, uid_t user, gid_t group)
{
	struct nameidata nd;
	int error;

	error = user_path_walk(filename, &nd);
	if (!error) {
		error = chown_common(nd.dentry, user, group);
		path_release(&nd);
	}
	return error;
}

asmlinkage long sys_lchown(const char * filename, uid_t user, gid_t group)
{
	struct nameidata nd;
	int error;

	error = user_path_walk_link(filename, &nd);
	if (!error) {
		error = chown_common(nd.dentry, user, group);
		path_release(&nd);
	}
	return error;
}


asmlinkage long sys_fchown(unsigned int fd, uid_t user, gid_t group)
{
	struct file * file;
	int error = -EBADF;

	file = fget(fd);
	if (file) {
		error = chown_common(file->f_dentry, user, group);
		fput(file);
	}
	return error;
}

/*
 * Note that while the flag value (low two bits) for sys_open means:
 *	00 - read-only
 *	01 - write-only
 *	10 - read-write
 *	11 - special
 * it is changed into
 *	00 - no permissions needed
 *	01 - read-permission
 *	10 - write-permission
 *	11 - read-write
 * for the internal routines (ie open_namei()/follow_link() etc). 00 is
 * used by symlinks.
 */
// filp_open：VFS层打开文件核心函数，由sys_open调用
// 参数 filename：内核态路径字符串；flags：open打开标志；mode：新建文件权限
// 返回：成功返回struct file*；失败返回ERR_PTR包装的负错误码
struct file *filp_open(const char * filename, int flags, int mode)
{
	int namei_flags, error;
	struct nameidata nd;  // nameidata：路径解析上下文，保存查找结果(dentry、vfsmount等)

	namei_flags = flags;  // 拷贝用户传入flags，转换成VFS内部namei使用的标志

	// 转换低2位访问权限位：sys_open的O_ACCMODE(读写位)转为open_namei内部权限编码
	if ((namei_flags+1) & O_ACCMODE)
		namei_flags++;

	// 如果带O_TRUNC(打开并截断文件)，追加写权限标记给内部路径查找函数
	if (namei_flags & O_TRUNC)
		namei_flags |= 2;

	// open_namei：核心路径查找函数，做路径解析、符号链接追踪、权限校验，结果存入nd
	error = open_namei(filename, namei_flags, mode, &nd);
	if (!error)
		// 路径查找成功，用找到的dentry和挂载实例nd.mnt，创建并返回struct file对象
		return dentry_open(nd.dentry, nd.mnt, flags);

	// 路径查找失败，返回包装了错误码的错误指针
	return ERR_PTR(error);
}

// dentry_open：根据已经拿到的dentry + vfsmount，实例化一个struct file文件对象，执行文件打开逻辑
// @dentry：已经解析好的目标文件目录项（path_walk/path_init/open_namei得到）
// @mnt：目标文件所在的vfsmount挂载实例
// @flags：open系统调用传入的打开标志 O_RDONLY/O_WRONLY/O_RDWR/O_TRUNC等
// 返回：成功返回struct file*；失败返回ERR_PTR(负数错误码)
struct file *dentry_open(struct dentry *dentry, struct vfsmount *mnt, int flags)
{
	struct file * f;
	struct inode *inode;
	int error;

	error = -ENFILE;

	// 1. 从filp缓存分配一个空闲的struct file实例
	f = get_empty_filp();
	if (!f)
		goto cleanup_dentry;  // 全局文件对象耗尽，ENFILE
	f->f_flags = flags;

	// 计算f_mode：FMODE_READ / FMODE_WRITE，(flags+1)&O_ACCMODE是旧内核的小技巧
	f->f_mode = (flags+1) & O_ACCMODE;
	inode = dentry->d_inode;  // 拿到dentry对应的inode

	// 如果本次打开是写模式，获取inode写访问权（防止只读inode被写打开，管理inode写引用计数）
	if (f->f_mode & FMODE_WRITE) {
		error = get_write_access(inode);
		if (error)
			goto cleanup_file;
	}

	// 绑定file对象：关联dentry、挂载点，文件读写位置初始化为0
	f->f_dentry = dentry;
	f->f_vfsmnt = mnt;
	f->f_pos = 0;
	f->f_reada = 0;

	// 获取inode的文件操作表（i_fop，如ext2_file_operations），引用计数+1
	f->f_op = fops_get(inode->i_fop);

	// 如果inode有超级块sb，把这个file加入sb的s_files链表（该文件系统下所有打开文件集合）
	if (inode->i_sb)
		file_move(f, &inode->i_sb->s_files);
	
	// 调用文件系统的open回调 i_fop->open，执行底层打开逻辑
	if (f->f_op && f->f_op->open) {
		//使用inode和file结构体来直接调用回调函数
		error = f->f_op->open(inode,f);
		if (error)
			goto cleanup_all;
	}

	// 清理只属于open阶段的临时标记：O_CREAT/O_EXCL/O_NOCTTY/O_TRUNC这些标志不再保留在file->f_flags
    // 这些只在open解析阶段使用，后续read/write不需要
	f->f_flags &= ~(O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC);

	return f;  // 成功返回file结构体，这就是用户态fd对应的内核对象

cleanup_all:
	fops_put(f->f_op);
	if (f->f_mode & FMODE_WRITE)
		put_write_access(inode);
	f->f_dentry = NULL;
	f->f_vfsmnt = NULL;
cleanup_file:
	put_filp(f);
cleanup_dentry:
	dput(dentry);
	mntput(mnt);
	return ERR_PTR(error);
}

/*
 * Find an empty file descriptor entry, and mark it busy.
 */
// 获取一个未被使用的文件描述符fd，成功返回fd号，失败返回负错误码
int get_unused_fd(void)
{
	// 获取当前进程的文件描述符表
	struct files_struct * files = current->files;
	int fd, error;

  	error = -EMFILE;

	// 加写锁，保护files_struct并发修改
	write_lock(&files->file_lock);

repeat:
	// 从next_fd位置开始，在open_fds位图中查找第一个为0的bit（空闲fd）
 	fd = find_next_zero_bit(files->open_fds, 
				files->max_fdset, 
				files->next_fd);

	/*
	 * N.B. For clone tasks sharing a files structure, this test
	 * will limit the total number of files that can be opened.
	 */
	// 检查是否超出进程允许打开文件的软限制RLIMIT_NOFILE
	if (fd >= current->rlim[RLIMIT_NOFILE].rlim_cur)
		goto out;

	/* Do we need to expand the fdset array? */
	// 找到的fd超出fd_set位图容量，需要扩容fd位图(open_fds/close_on_exec)
	if (fd >= files->max_fdset) {
		// 扩容fd_set位图
		error = expand_fdset(files, fd);
		if (!error) {
			// 扩容成功，error=0，重新循环查找空闲fd
			error = -EMFILE;
			goto repeat;
		}
		goto out;
	}
	
	/* 
	 * Check whether we need to expand the fd array.
	 */
	// 找到的fd超出fd指针数组容量，需要扩容struct file*指针数组
	if (fd >= files->max_fds) {
		// 扩容fd数组
		error = expand_fd_array(files, fd);
		if (!error) {
			// 扩容成功，重新循环查找
			error = -EMFILE;
			goto repeat;
		}
		goto out;
	}

	// 将fd对应的bit置1：标记该fd已经被占用
	FD_SET(fd, files->open_fds);

	// 清除close_on_exec标记：新分配fd默认不设置FD_CLOEXEC
	FD_CLR(fd, files->close_on_exec);

	// 更新下一次查找fd的起点，优化扫描
	files->next_fd = fd + 1;
#if 1
	/* Sanity check */
	if (files->fd[fd] != NULL) {
		printk("get_unused_fd: slot %d not NULL!\n", fd);
		files->fd[fd] = NULL;
	}
#endif
	error = fd;

out:
	write_unlock(&files->file_lock);
	return error;
}

/*
 * sys_open - open(2) 系统调用入口 (Linux 2.4 内核)
 * @filename: 用户态传入的文件路径字符串的用户空间虚拟地址
 * @flags:    打开标志位 (O_RDONLY, O_WRONLY, O_RDWR, O_CREAT, O_TRUNC 等)
 * @mode:     新建文件时使用的权限(模式)，受进程fs_struct->umask屏蔽；不创建文件时该参数忽略
 * 返回值:    成功返回文件描述符fd(非负整数)；失败返回负的错误码(-ENOENT,-EACCES等)
 *
 * asmlinkage: 告诉编译器，函数参数从CPU栈上获取（x86系统调用约定，系统调用入口强制要求）
 *
 * 整体流程：
 * 1. getname：把用户态的路径字符串拷贝到内核缓冲区（内核不能直接访问用户空间指针，必须拷贝）
 * 2. get_unused_fd：分配一个空闲的文件描述符编号
 * 3. filp_open：核心函数！VFS层打开文件，分配并填充struct file对象，执行路径查找、权限检查、inode查找
 * 4. fd_install：将 struct file * 挂入当前进程task_struct->files文件描述符表，建立fd -> file映射
 * 5. putname：释放getname分配的内核路径名缓冲区
 *
 * 错误分支：
 * filp_open失败返回错误指针 → 回收刚才申请的fd编号，把错误码作为返回值返回
 */
asmlinkage long sys_open(const char * filename, int flags, int mode)
{
	char * tmp;
	int fd, error;

#if BITS_PER_LONG != 32
	flags |= O_LARGEFILE;
#endif
	// 从用户空间拷贝filename字符串到内核内存；返回内核缓冲区指针
	tmp = getname(filename);
	fd = PTR_ERR(tmp);
	if (!IS_ERR(tmp)) {
		// 在当前进程的files_struct中，拿到一个未使用的fd编号
		fd = get_unused_fd();
		if (fd >= 0) {

			// VFS核心打开逻辑：解析路径、查找dentry/inode、构造struct file
			struct file *f = filp_open(tmp, flags, mode);
			error = PTR_ERR(f);
			if (IS_ERR(f))
				goto out_error;
			
			// 把struct file*填入进程的fd表，fd正式生效
			fd_install(fd, f);
		}
out:
		putname(tmp);
	}
	return fd;

out_error:
	put_unused_fd(fd);
	fd = error;
	goto out;
}

#ifndef __alpha__

/*
 * For backward compatibility?  Maybe this should be moved
 * into arch/i386 instead?
 */
asmlinkage long sys_creat(const char * pathname, int mode)
{
	return sys_open(pathname, O_CREAT | O_WRONLY | O_TRUNC, mode);
}

#endif

/*
 * "id" is the POSIX thread ID. We use the
 * files pointer for this..
 */
// filp_close：执行文件对象层面的关闭清理，sys_close调用它
// @filp：待关闭的struct file实例
// @id：fl_owner_t，文件锁持有者标识（当前进程files）
// 返回：flush回调的返回值，0表示成功
int filp_close(struct file *filp, fl_owner_t id)
{
	int retval;

	// file_count(filp) 读取 filp->f_count
    // 正常进入filp_close时引用计数至少为1；如果已经是0，打印告警
	if (!file_count(filp)) {
		printk("VFS: Close: file count is 0\n");
		return 0;
	}
	retval = 0;

	// 如果文件操作集实现了flush回调，则执行flush
	if (filp->f_op && filp->f_op->flush) {
		lock_kernel();
		retval = filp->f_op->flush(filp);
		unlock_kernel();
	}

	// 关闭dnotify监听（inotify前身），移除这个文件上的目录通知监听
	fcntl_dirnotify(0, filp, 0);

	// 移除该进程在此文件上持有的POSIX记录锁
	locks_remove_posix(filp, id);

	// 核心：递减filp引用计数。减到0时触发release回调、回收filp对象
	fput(filp);
	return retval;
}

/*
 * Careful here! We test whether the file pointer is NULL before
 * releasing the fd. This ensures that one clone task can't release
 * an fd while another clone is opening it.
 */
// sys_close：close系统调用入口，关闭用户态指定fd
// asmlinkage：x86-32系统调用，参数从栈上获取
// @fd：待关闭的文件描述符
// 返回值：0成功；负数错误码
asmlinkage long sys_close(unsigned int fd)
{
	struct file * filp;
	struct files_struct *files = current->files;

	// 加写锁：修改fd表（置空fd指针、修改位图）必须拿写锁，排斥其他fget读锁
	write_lock(&files->file_lock);
	if (fd >= files->max_fds)
		goto out_unlock;

	// 从fd数组取出对应的struct file指针
	filp = files->fd[fd];
	if (!filp)  // fd对应的文件指针为空，说明fd未打开，非法
		goto out_unlock;

	// 把进程fd数组里该fd对应的指针置NULL，切断用户态fd到filp的映射
	files->fd[fd] = NULL;

	// 清除close_on_exec位图标记：exec执行时是否自动关闭此fd的标记
	FD_CLR(fd, files->close_on_exec);

	// 把fd放回空闲fd池，这个fd号可以被后续open重新复用
	__put_unused_fd(files, fd);
	write_unlock(&files->file_lock);

	return filp_close(filp, files);

out_unlock:
	write_unlock(&files->file_lock);
	return -EBADF;
}

/*
 * This routine simulates a hangup on the tty, to arrange that users
 * are given clean terminals at login time.
 */
asmlinkage long sys_vhangup(void)
{
	if (capable(CAP_SYS_TTY_CONFIG)) {
		tty_vhangup(current->tty);
		return 0;
	}
	return -EPERM;
}
