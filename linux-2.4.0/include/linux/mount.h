/*
 *
 * Definitions for mount interface. This describes the in the kernel build 
 * linkedlist with mounted filesystems.
 *
 * Author:  Marco van Wieringen <mvw@planets.elm.net>
 *
 * Version: $Id: mount.h,v 2.0 1996/11/17 16:48:14 mvw Exp mvw $
 *
 */
#ifndef _LINUX_MOUNT_H
#define _LINUX_MOUNT_H
#ifdef __KERNEL__

#define MNT_VISIBLE	1

struct vfsmount
{
	// 挂载点dentry：父文件系统上，用来当挂载入口的那个目录项（宿主目录dentry）
	struct dentry *mnt_mountpoint;	/* dentry of mountpoint */

	// 被挂载文件系统的根dentry；follow_down穿越挂载点后，dentry会切换为此
	struct dentry *mnt_root;	/* root of the mounted tree */

	// 父vfsmount，当前这个挂载依附在哪一个挂载实例之上，构成挂载树；..回退（follow_up）依赖此字段
	struct vfsmount *mnt_parent;	/* fs we are mounted on */

	// 链表节点：同一个super_block(同一个文件系统实例)的所有vfsmount，通过此字段链入sb->s_mounts
	struct list_head mnt_instances;	/* other vfsmounts of the same fs */

	// 堆叠挂载链表节点：把本vfsmount挂到挂载点dentry的 d_vfsmnt 链表；同一目录多挂载靠这个链表保存
	// clash：冲突的意思，表示冲突的挂载点信息
	struct list_head mnt_clash;	/* those who are mounted on (other */
					/* instances) of the same dentry */

	// 指向超级块sb；sb记录文件系统类型、块设备、inode操作集，决定IO落到哪个设备
	struct super_block *mnt_sb;	/* pointer to superblock */

	// 链表头：所有挂载在当前文件系统内部目录上的子挂载实例
	struct list_head mnt_mounts;	/* list of children, anchored here */

	// 子挂载节点：子mnt用此字段挂入父mnt的 mnt_mounts 链表
	struct list_head mnt_child;	/* and going through their mnt_child */

	// vfsmount引用计数；mntget()增加，mntput()减少；计数>0时不能销毁释放
	atomic_t mnt_count;

	// 挂载标志，例如MNT_VISIBLE，表示该挂载在/proc/mounts可见
	int mnt_flags;

	// 设备名字符串，如"/dev/sda1"，proc挂载这里是"none"
	char *mnt_devname;		/* Name of device e.g. /dev/dsk/hda1 */

	// 全局链表节点：链入系统全局vfsmntlist，遍历生成/proc/mounts
	struct list_head mnt_list;

	// 记录发起本次mount操作的用户uid
	uid_t mnt_owner;
};

//增加引用计数并返回挂载点数据结构 vfsmount
static inline struct vfsmount *mntget(struct vfsmount *mnt)
{
	if (mnt)
		atomic_inc(&mnt->mnt_count);
	return mnt;
}

static inline void mntput(struct vfsmount *mnt)
{
	if (mnt) {
		if (atomic_dec_and_test(&mnt->mnt_count))
			BUG();
	}
}

#endif
#endif /* _LINUX_MOUNT_H */
