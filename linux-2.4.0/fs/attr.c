/*
 *  linux/fs/attr.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *  changes by Thomas Schoebel-Theuer
 */

#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/smp_lock.h>
#include <linux/dnotify.h>
#include <linux/fcntl.h>

/* Taken over from the old code... */

/* POSIX UID/GID verification for setting inode attributes. */

// inode_change_ok：VFS通用inode属性修改权限检查函数
// 用于notify_change，校验当前进程是否有权限修改iattr指定的inode属性
// @inode：待修改的目标inode
// @attr：struct iattr，标记本次要修改哪些属性
// 返回：0代表权限校验通过；-EPERM 权限不足
int inode_change_ok(struct inode *inode, struct iattr *attr)
{
	int retval = -EPERM;
	unsigned int ia_valid = attr->ia_valid;

	/* If force is set do it anyway. */

	// ATTR_FORCE强制标记：跳过所有权限检查，直接放行（内核内部使用）
	if (ia_valid & ATTR_FORCE)
		goto fine;

	/* Make sure a caller can chown. */
	// 修改UID(chown)权限校验：
    // 不是文件所有者，并且要修改uid，同时没有CAP_CHOWN能力 → 拒绝
	if ((ia_valid & ATTR_UID) &&
	    (current->fsuid != inode->i_uid ||
	     attr->ia_uid != inode->i_uid) && !capable(CAP_CHOWN))
		goto error;

	/* Make sure caller can chgrp. */

	// 修改GID(chgrp)校验：
    // 目标gid不是当前进程所属组，且gid要变更，没有CAP_CHOWN能力 → 拒绝
	if ((ia_valid & ATTR_GID) &&
	    (!in_group_p(attr->ia_gid) && attr->ia_gid != inode->i_gid) &&
	    !capable(CAP_CHOWN))
		goto error;

	/* Make sure a caller can chmod. */
	// 修改mode(chmod)权限校验
	if (ia_valid & ATTR_MODE) {
		// 不是文件属主，且没有CAP_FOWNER能力，不能修改权限
		if ((current->fsuid != inode->i_uid) && !capable(CAP_FOWNER))
			goto error;
		/* Also check the setgid bit! */
		// 没有CAP_FSETID能力，且进程不在目标组内 → 自动清除setgid位S_ISGID
		if (!in_group_p((ia_valid & ATTR_GID) ? attr->ia_gid :
				inode->i_gid) && !capable(CAP_FSETID))
			attr->ia_mode &= ~S_ISGID;
	}

	/* Check for setting the inode time. */
	// 手动设置atime/mtime（utimes）权限校验
	if (ia_valid & (ATTR_MTIME_SET | ATTR_ATIME_SET)) {
		if (current->fsuid != inode->i_uid && !capable(CAP_FOWNER))
			goto error;
	}
fine:
	retval = 0;
error:
	return retval;
}

void inode_setattr(struct inode * inode, struct iattr * attr)
{
	unsigned int ia_valid = attr->ia_valid;

	if (ia_valid & ATTR_UID)
		inode->i_uid = attr->ia_uid;
	if (ia_valid & ATTR_GID)
		inode->i_gid = attr->ia_gid;
	if (ia_valid & ATTR_SIZE)
		vmtruncate(inode, attr->ia_size);
	if (ia_valid & ATTR_ATIME)
		inode->i_atime = attr->ia_atime;
	if (ia_valid & ATTR_MTIME)
		inode->i_mtime = attr->ia_mtime;
	if (ia_valid & ATTR_CTIME)
		inode->i_ctime = attr->ia_ctime;
	if (ia_valid & ATTR_MODE) {
		inode->i_mode = attr->ia_mode;
		if (!in_group_p(inode->i_gid) && !capable(CAP_FSETID))
			inode->i_mode &= ~S_ISGID;
	}

	//表示inode信息更新
	mark_inode_dirty(inode);
}

static int setattr_mask(unsigned int ia_valid)
{
	unsigned long dn_mask = 0;

	if (ia_valid & ATTR_UID)
		dn_mask |= DN_ATTRIB;
	if (ia_valid & ATTR_GID)
		dn_mask |= DN_ATTRIB;
	if (ia_valid & ATTR_SIZE)
		dn_mask |= DN_MODIFY;
	/* both times implies a utime(s) call */
	if ((ia_valid & (ATTR_ATIME|ATTR_MTIME)) == (ATTR_ATIME|ATTR_MTIME))
		dn_mask |= DN_ATTRIB;
	else if (ia_valid & ATTR_ATIME)
		dn_mask |= DN_ACCESS;
	else if (ia_valid & ATTR_MTIME)
		dn_mask |= DN_MODIFY;
	if (ia_valid & ATTR_MODE)
		dn_mask |= DN_ATTRIB;
	return dn_mask;
}

// notify_change：VFS中修改inode属性的通用入口函数，chmod/chown/truncate等都会调用
// @dentry：目标文件的目录项
// @attr：struct iattr，描述本次要修改哪些inode属性、新属性值
// 返回：0成功；负数错误码
int notify_change(struct dentry * dentry, struct iattr * attr)
{
	struct inode *inode = dentry->d_inode;
	int error;
	time_t now = CURRENT_TIME;  // 获取内核当前时间
	unsigned int ia_valid = attr->ia_valid;  // 取出属性修改掩码

	if (!inode)
		BUG();

	attr->ia_ctime = now;

	// 没有指定手动设置atime，则atime自动更新为当前时间
	if (!(ia_valid & ATTR_ATIME_SET))
		attr->ia_atime = now;

	// 没有指定手动设置mtime，则mtime自动更新为当前时间
	if (!(ia_valid & ATTR_MTIME_SET))
		attr->ia_mtime = now;

	lock_kernel();
	if (inode->i_op && inode->i_op->setattr) 
		// 文件系统自定义setattr回调（ext2等文件系统会提供i_op->setattr）
		error = inode->i_op->setattr(dentry, attr);
	else {
		// 没有文件系统自定义setattr回调，走VFS默认流程
		error = inode_change_ok(inode, attr);  // 权限检查，判断是否允许修改这些属性
		if (!error)
			inode_setattr(inode, attr);  // 直接修改inode内存中的属性字段
	}
	unlock_kernel();
	if (!error) {
		unsigned long dn_mask = setattr_mask(ia_valid);
		if (dn_mask)
			inode_dir_notify(dentry->d_parent->d_inode, dn_mask);
	}
	return error;
}
