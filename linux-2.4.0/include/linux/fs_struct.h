#ifndef _LINUX_FS_STRUCT_H
#define _LINUX_FS_STRUCT_H
#ifdef __KERNEL__

struct fs_struct {
	// 引用计数。多个进程共享此fs_struct时计数；count减到0就释放内存
	atomic_t count;
	rwlock_t lock;

	// 创建新文件时使用的文件权限掩码(umask)，用来屏蔽新建文件的权限位
	int umask;

	// 目录项dentry指针：目录项对象，VFS里代表目录/文件在内存中的名字节点
	// root: 进程的【根目录】对应的dentry，就是 / ；对应chroot修改的根
	// pwd：进程的【当前工作目录】dentry，就是pwd命令看到的当前目录
	// altroot：备用根目录dentry，极少用，用于替代根的备选（2.4很少用到）
	struct dentry * root, * pwd, * altroot;

	// vfsmount：挂载实例
	// root目录所属的挂载实例（根目录所在文件系统挂载对象）
	// pwd当前工作目录所属的挂载实例
	// altroot备用根对应的挂载实例
	struct vfsmount * rootmnt, * pwdmnt, * altrootmnt;
};

#define INIT_FS { \
	ATOMIC_INIT(1), \
	RW_LOCK_UNLOCKED, \
	0022, \
	NULL, NULL, NULL, NULL, NULL, NULL \
}

extern void exit_fs(struct task_struct *);
extern void set_fs_altroot(void);

/*
 * Replace the fs->{rootmnt,root} with {mnt,dentry}. Put the old values.
 * It can block. Requires the big lock held.
 */

static inline void set_fs_root(struct fs_struct *fs,
	struct vfsmount *mnt,
	struct dentry *dentry)
{
	struct dentry *old_root;
	struct vfsmount *old_rootmnt;
	write_lock(&fs->lock);
	old_root = fs->root;
	old_rootmnt = fs->rootmnt;
	fs->rootmnt = mntget(mnt);
	fs->root = dget(dentry);
	write_unlock(&fs->lock);
	if (old_root) {
		dput(old_root);
		mntput(old_rootmnt);
	}
}

/*
 * Replace the fs->{pwdmnt,pwd} with {mnt,dentry}. Put the old values.
 * It can block. Requires the big lock held.
 */

static inline void set_fs_pwd(struct fs_struct *fs,
	struct vfsmount *mnt,
	struct dentry *dentry)
{
	struct dentry *old_pwd;
	struct vfsmount *old_pwdmnt;
	write_lock(&fs->lock);
	old_pwd = fs->pwd;
	old_pwdmnt = fs->pwdmnt;
	fs->pwdmnt = mntget(mnt);
	fs->pwd = dget(dentry);
	write_unlock(&fs->lock);
	if (old_pwd) {
		dput(old_pwd);
		mntput(old_pwdmnt);
	}
}

struct fs_struct *copy_fs_struct(struct fs_struct *old);
void put_fs_struct(struct fs_struct *fs);

#endif
#endif
