/*
 *  linux/fs/proc/inode.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/sched.h>
#include <linux/proc_fs.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/file.h>
#include <linux/locks.h>
#include <linux/limits.h>
#define __NO_VERSION__
#include <linux/module.h>
#include <linux/smp_lock.h>

#include <asm/system.h>
#include <asm/uaccess.h>

extern void free_proc_entry(struct proc_dir_entry *);

struct proc_dir_entry * de_get(struct proc_dir_entry *de)
{
	if (de)
		atomic_inc(&de->count);
	return de;
}

/*
 * Decrements the use count and checks for deferred deletion.
 */
void de_put(struct proc_dir_entry *de)
{
	if (de) {	
		lock_kernel();		
		if (!atomic_read(&de->count)) {
			printk("de_put: entry %s already free!\n", de->name);
			unlock_kernel();
			return;
		}

		if (atomic_dec_and_test(&de->count)) {
			if (de->deleted) {
				printk("de_put: deferred delete of %s\n",
					de->name);
				free_proc_entry(de);
			}
		}		
		unlock_kernel();
	}
}

/*
 * Decrement the use count of the proc_dir_entry.
 */
static void proc_delete_inode(struct inode *inode)
{
	struct proc_dir_entry *de = inode->u.generic_ip;

	inode->i_state = I_CLEAR;

	if (PROC_INODE_PROPER(inode)) {
		proc_pid_delete_inode(inode);
		return;
	}
	if (de) {
		if (de->owner)
			__MOD_DEC_USE_COUNT(de->owner);
		de_put(de);
	}
}

struct vfsmount *proc_mnt;

static void proc_read_inode(struct inode * inode)
{
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
}

static int proc_statfs(struct super_block *sb, struct statfs *buf)
{
	buf->f_type = PROC_SUPER_MAGIC;
	buf->f_bsize = PAGE_SIZE/sizeof(long);
	buf->f_bfree = 0;
	buf->f_bavail = 0;
	buf->f_ffree = 0;
	buf->f_namelen = NAME_MAX;
	return 0;
}

static struct super_operations proc_sops = { 
	read_inode:	proc_read_inode,
	put_inode:	force_delete,
	delete_inode:	proc_delete_inode,
	statfs:		proc_statfs,
};


static int parse_options(char *options,uid_t *uid,gid_t *gid)
{
	char *this_char,*value;

	*uid = current->uid;
	*gid = current->gid;
	if (!options) return 1;
	for (this_char = strtok(options,","); this_char; this_char = strtok(NULL,",")) {
		if ((value = strchr(this_char,'=')) != NULL)
			*value++ = 0;
		if (!strcmp(this_char,"uid")) {
			if (!value || !*value)
				return 0;
			*uid = simple_strtoul(value,&value,0);
			if (*value)
				return 0;
		}
		else if (!strcmp(this_char,"gid")) {
			if (!value || !*value)
				return 0;
			*gid = simple_strtoul(value,&value,0);
			if (*value)
				return 0;
		}
		else return 1;
	}
	return 1;
}

struct inode * proc_get_inode(struct super_block * sb, int ino,
				struct proc_dir_entry * de)
{
	struct inode * inode;

	/*
	 * Increment the use count so the dir entry can't disappear.
	 */
	de_get(de);
#if 1
/* shouldn't ever happen */
if (de && de->deleted)
printk("proc_iget: using deleted entry %s, count=%d\n", de->name, atomic_read(&de->count));
#endif

	inode = iget(sb, ino);
	if (!inode)
		goto out_fail;
	
	inode->u.generic_ip = (void *) de;
	if (de) {
		if (de->mode) {
			inode->i_mode = de->mode;
			inode->i_uid = de->uid;
			inode->i_gid = de->gid;
		}
		if (de->size)
			inode->i_size = de->size;
		if (de->nlink)
			inode->i_nlink = de->nlink;
		if (de->owner)
			__MOD_INC_USE_COUNT(de->owner);
		if (S_ISBLK(de->mode)||S_ISCHR(de->mode)||S_ISFIFO(de->mode))
			init_special_inode(inode,de->mode,kdev_t_to_nr(de->rdev));
		else {
			if (de->proc_iops)
				inode->i_op = de->proc_iops;
			if (de->proc_fops)
				inode->i_fop = de->proc_fops;
		}
	}

out:
	return inode;

out_fail:
	de_put(de);
	goto out;
}			

/**
 * proc_read_super - proc文件系统专属的read_super回调（Linux2.4）
 * 由VFS的read_super函数调用，负责填充super_block，创建proc根inode与根dentry
 * @s: VFS分配好的空白super_block外壳
 * @data: 挂载传入的参数数据
 * @silent: 静默标记，为1时不打印错误信息
 * 返回值：成功返回s；失败返回NULL
 */
struct super_block *proc_read_super(struct super_block *s,void *data, 
				    int silent)
{
	struct inode * root_inode;
	struct task_struct *p;

	s->s_blocksize = 1024; // 块大小，proc是虚拟文件系统，仅用于占位，无真实磁盘块
	s->s_blocksize_bits = 10;  // blocksize = 1 << 10 = 1024
	s->s_magic = PROC_SUPER_MAGIC; // 文件系统魔数，用来识别是proc超级块
	s->s_op = &proc_sops;  // 挂载sb的操作函数集（super_operations）

	// 创建proc根inode，inode号为PROC_ROOT_INO，关联proc根的inode操作proc_root
	root_inode = proc_get_inode(s, PROC_ROOT_INO, &proc_root);
	if (!root_inode)
		goto out_no_root;
	/*
     * Fixup the root inode's nlink value
     * proc根inode的硬链接数修正：系统中每一个进程，都会在/proc下有一个pid目录，
     * 每一个进程就相当于根目录的一个子链接，所以遍历所有task，累加i_nlink
     */
	read_lock(&tasklist_lock);
	for_each_task(p) if (p->pid) root_inode->i_nlink++;
	read_unlock(&tasklist_lock);

	// d_alloc_root：分配根dentry，绑定刚才创建的root_inode，赋值给sb->s_root
    // sb->s_root就是proc文件系统的根目录项，后续所有proc子目录都挂在这个dentry下
	s->s_root = d_alloc_root(root_inode);
	if (!s->s_root)
		goto out_no_root;
	
	// 解析挂载参数，设置根inode的uid/gid（例如mount -o uid=xxx,gid=xxx）
	parse_options(data, &root_inode->i_uid, &root_inode->i_gid);
	return s;

out_no_root:
	printk("proc_read_super: get root inode failed\n");
	iput(root_inode);
	return NULL;
}
