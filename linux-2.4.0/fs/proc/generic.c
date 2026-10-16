/*
 * proc/fs/generic.c --- generic routines for the proc-fs
 *
 * This file contains generic proc-fs routines for handling
 * directories and files.
 * 
 * Copyright (C) 1991, 1992 Linus Torvalds.
 * Copyright (C) 1997 Theodore Ts'o
 */

#include <asm/uaccess.h>

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/proc_fs.h>
#include <linux/stat.h>
#define __NO_VERSION__
#include <linux/module.h>
#include <asm/bitops.h>

static ssize_t proc_file_read(struct file * file, char * buf,
			      size_t nbytes, loff_t *ppos);
static ssize_t proc_file_write(struct file * file, const char * buffer,
			       size_t count, loff_t *ppos);
static loff_t proc_file_lseek(struct file *, loff_t, int);

int proc_match(int len, const char *name,struct proc_dir_entry * de)
{
	if (!de || !de->low_ino)
		return 0;
	if (de->namelen != len)
		return 0;
	return !memcmp(name, de->name, len);
}

//proc 普通文件对应的操作
static struct file_operations proc_file_operations = {
	llseek:		proc_file_lseek,
	read:		proc_file_read,
	write:		proc_file_write,
};

#ifndef MIN
#define MIN(a,b) (((a) < (b)) ? (a) : (b))
#endif

/* buffer size is one page but our output routines use some slack for overruns */
#define PROC_BLOCK_SIZE	(PAGE_SIZE - 1024)

/**
 * proc_file_read - proc文件的通用读文件操作（Linux2.4，file_operations.read回调）
 * @file: 打开的文件对象 struct file
 * @buf: 用户态缓冲区，读到的数据拷贝到这里
 * @nbytes: 用户本次想要读取的字节数
 * @ppos: 读写位置指针（文件偏移，in/out参数，读完会更新）
 * 返回值：成功返回读到的字节数；负数为负错误码
 *
 * 原理：proc文件不是磁盘文件，数据是**实时动态生成**。
 * 内核分配一页内核页作为临时缓冲区，调用proc_dir_entry注册的回调生成文本，
 * 再把生成的数据拷贝到用户空间buf，更新文件偏移。
 */
static ssize_t
proc_file_read(struct file * file, char * buf, size_t nbytes, loff_t *ppos)
{
	// 从file对象拿到dentry，再拿到inode
	struct inode * inode = file->f_dentry->d_inode;
	char 	*page;
	ssize_t	retval=0;
	int	eof=0;
	ssize_t	n, count;
	char	*start;
	struct proc_dir_entry * dp;

	// proc的inode把私有数据proc_dir_entry放在inode->u.generic_ip
	dp = (struct proc_dir_entry *) inode->u.generic_ip;

	// 申请一页内核内存（4KB），GFP_KERNEL：允许睡眠分配内存
	if (!(page = (char*) __get_free_page(GFP_KERNEL)))
		return -ENOMEM;

	while ((nbytes > 0) && !eof)
	{
		// 单次最多读取PROC_BLOCK_SIZE（一般就是一页4k），取用户请求nbytes和4k较小值
		count = MIN(PROC_BLOCK_SIZE, nbytes);

		start = NULL;
		if (dp->get_info) {
			/*
             * Handle backwards compatibility with the old net
             * routines.
             * 旧版本兼容回调，早期网络相关proc文件使用get_info
             */
			n = dp->get_info(page, &start, *ppos, count);
			if (n < count)
				eof = 1;
		} else if (dp->read_proc) {
			// 主流回调：read_proc，由注册proc文件的驱动填写，动态生成内容
			// 一般来说如果 start 不是NULL，有两种值
			//  1. 返回在page中，此时start >= page：page ≤ *start < page + 4096
			//  2. 返回一个偏移，例如*start = (char *) 10000，此时start < page
			n = dp->read_proc(page, &start, *ppos,
					  count, &eof, dp->data);
		} else
			break;

		if (!start) {
			/*
             * For proc files that are less than 4k
             * 兼容模式：回调没有设置start，数据从page+当前文件偏移*ppos开始
             */
			start = page + *ppos;
			n -= *ppos;
			if (n <= 0)
				break;
			if (n > count)
				n = count;
		}
		if (n == 0)
			break;	/* End of file */
		if (n < 0) {
			if (retval == 0)
				retval = n;
			break;
		}
		
		/* This is a hack to allow mangling of file pos independent
         * of actual bytes read.  Simply place the data at page,
         * return the bytes, and set `start' to the desired offset
         * as an unsigned int. - Paul.Russell@rustcorp.com.au
         * 注释说明：hack技巧，驱动可以自主控制文件偏移ppos，不一定按读取字节数递增
         * 驱动把start填成数字，内核就会用start的值修改ppos，而不是默认+n
         */

		// copy_to_user：内核page的数据拷贝到用户buf，返回未能成功拷贝的字节数
 		n -= copy_to_user(buf, start < page ? page : start, n);
		if (n == 0) {
			if (retval == 0)
				retval = -EFAULT;
			break;
		}

		// 更新文件偏移ppos：
        // 如果start < page 是特殊hack场景，直接把start当数字赋值给偏移；否则偏移 +=读到字节n
		*ppos += start < page ? (long)start : n; /* Move down the file */
		nbytes -= n;
		buf += n;
		retval += n;
	}
	free_page((unsigned long) page);
	return retval;
}

//写入数据到proc文件中
static ssize_t
proc_file_write(struct file * file, const char * buffer,
		size_t count, loff_t *ppos)
{
	struct inode *inode = file->f_dentry->d_inode;
	struct proc_dir_entry * dp;
	
	dp = (struct proc_dir_entry *) inode->u.generic_ip;

	if (!dp->write_proc)
		return -EIO;

	/* FIXME: does this routine need ppos?  probably... */
	// 调用写入接口就行了
	return dp->write_proc(file, buffer, count, dp->data);
}


static loff_t
proc_file_lseek(struct file * file, loff_t offset, int orig)
{
    switch (orig) {
    case 0:
	if (offset < 0)
	    return -EINVAL;    
	file->f_pos = offset;
	return(file->f_pos);
    case 1:
	if (offset + file->f_pos < 0)
	    return -EINVAL;    
	file->f_pos += offset;
	return(file->f_pos);
    case 2:
	return(-EINVAL);
    default:
	return(-EINVAL);
    }
}

/*
 * This function parses a name such as "tty/driver/serial", and
 * returns the struct proc_dir_entry for "/proc/tty/driver", and
 * returns "serial" in residual.
 */

 /**
 * xlate = translate
 * xlate_proc_name - 解析proc路径字符串，逐级查找已存在的父目录项
 * @name: 输入的完整路径字符串，例如 "net/ipv4/xxx"
 * @ret: 输出参数，返回找到的父目录 proc_dir_entry*
 * @residual: 输出参数，返回路径中最后一段待创建的文件名（剩余字符串）
 *
 * 功能说明：
 * 当 create_proc_entry 的 parent 参数传入 NULL 时调用该函数。
 * 把路径按 '/' 分割，逐级在proc目录树里查找已经存在的目录；
 * 查找失败返回 -ENOENT；查找成功返回0，ret指向最后一级父目录，residual指向最后一段名字。
 *
 * 返回值：0 成功；-ENOENT 路径中间某一级目录不存在
 */
static int xlate_proc_name(const char *name,
			   struct proc_dir_entry **ret, const char **residual)
{
	const char     		*cp = name, *next;
	struct proc_dir_entry	*de;
	int			len;

	//从根目录开始
	de = &proc_root;
	while (1) {
		next = strchr(cp, '/');
		if (!next)
			break;

		len = next - cp;

		/*
         * 在当前目录 de 的子目录链表 subdir 中遍历匹配
         * proc_dir_entry 通过 next 成员串联成单向链表
         */
		for (de = de->subdir; de ; de = de->next) {
			// proc_match：比较长度+字符串，匹配成功则de指向找到的子目录，跳出for
			if (proc_match(len, cp, de))
				break;
		}

		// for循环遍历完链表都没找到匹配项 → 中间目录不存在，返回-ENOENT
		if (!de)
			return -ENOENT;

		// cp 移动到下一段，跳过当前目录名 + '/' 分隔符
		cp += len + 1;
	}
	// 循环结束：cp指向路径最后一段（待创建的文件/目录名称）
	// *ret 保存找到的父目录项
	*residual = cp;
	*ret = de;
	return 0;
}

//inode 号是 proc 内部用来唯一标识一个文件 / 目录的整数；内核 VFS（虚拟文件系统）靠 inode 号区分同一个文件系统下不同文件。
//vfs中
// inode：代表文件本身（元数据：大小、权限、属主、文件类型）
// dentry：目录项，代表目录里的名字（文件名）
// 用户态执行 ls -i /proc/xxx 看到的数字，就是这个 low_ino。
static unsigned char proc_alloc_map[PROC_NDYNAMIC / 8];

/**
 * make_inode_number - 分配proc动态inode编号
 *
 * 功能：从位图proc_alloc_map中寻找第一个空闲bit，标记为已占用，
 *       返回对应的proc动态inode号；位图满了返回-1。
 * 返回值：成功返回分配到的inode号；无空闲inode时返回-1
 */
static int make_inode_number(void)
{
	// 在proc_alloc_map位图中查找第一个值为0的bit下标i
    // PROC_NDYNAMIC：动态inode位图总bit数量
	int i = find_first_zero_bit((void *) proc_alloc_map, PROC_NDYNAMIC);
	if (i<0 || i>=PROC_NDYNAMIC) 
		return -1;

	// 校验：找不到空闲位(i>=PROC_NDYNAMIC)或异常返回，分配失败
	set_bit(i, (void *) proc_alloc_map);

	// PROC_DYNAMIC_FIRST：动态inode起始基准号
    // 最终inode = 基准号 + 位图偏移i
	return PROC_DYNAMIC_FIRST + i;
}

static int proc_readlink(struct dentry *dentry, char *buffer, int buflen)
{
	char *s=((struct proc_dir_entry *)dentry->d_inode->u.generic_ip)->data;
	return vfs_readlink(dentry, buffer, buflen, s);
}

static int proc_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	char *s=((struct proc_dir_entry *)dentry->d_inode->u.generic_ip)->data;
	return vfs_follow_link(nd, s);
}

//proc link文件对应的操作
static struct inode_operations proc_link_inode_operations = {
	readlink:	proc_readlink,
	follow_link:	proc_follow_link,
};

/*
 * As some entries in /proc are volatile, we want to 
 * get rid of unused dentries.  This could be made 
 * smarter: we could keep a "volatile" flag in the 
 * inode to indicate which ones to keep.
 */
static int proc_delete_dentry(struct dentry * dentry)
{
	return 1;
}

static struct dentry_operations proc_dentry_operations =
{
	d_delete:	proc_delete_dentry,
};

/*
 * Don't create negative dentries here, return -ENOENT by hand
 * instead.
 */
struct dentry *proc_lookup(struct inode * dir, struct dentry *dentry)
{
	struct inode *inode;
	struct proc_dir_entry * de;
	int error;

	error = -ENOENT;
	inode = NULL;
	de = (struct proc_dir_entry *) dir->u.generic_ip;
	if (de) {
		for (de = de->subdir; de ; de = de->next) {
			if (!de || !de->low_ino)
				continue;
			if (de->namelen != dentry->d_name.len)
				continue;
			if (!memcmp(dentry->d_name.name, de->name, de->namelen)) {
				int ino = de->low_ino;
				error = -EINVAL;
				inode = proc_get_inode(dir->i_sb, ino, de);
				break;
			}
		}
	}

	if (inode) {
		dentry->d_op = &proc_dentry_operations;
		d_add(dentry, inode);
		return NULL;
	}
	return ERR_PTR(error);
}

/*
 * This returns non-zero if at EOF, so that the /proc
 * root directory can use this and check if it should
 * continue with the <pid> entries..
 *
 * Note that the VFS-layer doesn't care about the return
 * value of the readdir() call, as long as it's non-negative
 * for success..
 */

 /**
 * proc_readdir - proc目录的readdir回调（Linux2.4，目录读取，file_operations.readdir）
 * @filp: 打开的目录文件struct file
 * @dirent: 传给filldir的上下文，用来填充目录项到用户态
 * @filldir: 回调函数，内核用来把一条目录条目拷贝到用户空间
 * 返回值：
 *    返回1：读取完成，没有更多目录项
 *    返回0：本次填充成功，还有条目/或者缓冲区满了，下次继续读
 *
 * 作用：读取proc目录下的子条目，返回. ..以及注册的所有proc子节点。
 * proc目录的子项保存在proc_dir_entry链表，不是磁盘，每次readdir遍历链表动态生成。
 */
int proc_readdir(struct file * filp,
	void * dirent, filldir_t filldir)
{
	struct proc_dir_entry * de;
	unsigned int ino;
	int i;

	// 获取当前目录对应的inode
	struct inode *inode = filp->f_dentry->d_inode;

	// 当前目录inode号
	ino = inode->i_ino;

	// proc私有描述符，存在inode->u.generic_ip
	de = (struct proc_dir_entry *) inode->u.generic_ip;
	if (!de)
		return -EINVAL;

	// filp->f_pos：目录读取的位置偏移（目录迭代器，不是字节偏移！）
    // 目录的f_pos是条目序号，0=第一条，1=第二条，以此类推
	i = filp->f_pos;
	switch (i) {
		case 0:
			// 第0项： "." 当前目录
            // filldir(上下文,名字,名字长度,目录偏移,inode号,文件类型)
			if (filldir(dirent, ".", 1, i, ino, DT_DIR) < 0)
				return 0;
			i++;
			filp->f_pos++;
			/* fall through */
		case 1:
			// 第1项： ".." 父目录
			if (filldir(dirent, "..", 2, i,
				    filp->f_dentry->d_parent->d_inode->i_ino,
				    DT_DIR) < 0)
				return 0;
			i++;
			filp->f_pos++;
			/* fall through */
		default:
			// 跳过前2条(. ..)，从子链表开始遍历
			de = de->subdir;
			i -= 2;

			// 循环：根据f_pos跳到要读取的起始节点（定位）
			for (;;) {
				if (!de)
					return 1;
				if (!i)
					break;
				de = de->next;
				i--;
			}
			// 从定位到的节点开始，循环填充剩下所有子目录项
			do {
				if (filldir(dirent, de->name, de->namelen, filp->f_pos,
					    de->low_ino, de->mode >> 12) < 0)
					return 0;
				filp->f_pos++;
				de = de->next;
			} while (de);
	}
	return 1;
}

/*
 * These are the generic /proc directory operations. They
 * use the in-memory "struct proc_dir_entry" tree to parse
 * the /proc directory.
 */
// proc 目录对应的文件操作
static struct file_operations proc_dir_operations = {
	read:			generic_read_dir,
	readdir:		proc_readdir,
};

/*
 * proc directories can do almost nothing..
 */
static struct inode_operations proc_dir_inode_operations = {
	lookup:		proc_lookup,
};

/**
 * proc_register - 将新建的proc目录项注册挂载到父proc目录链表
 * @dir: 父目录的proc_dir_entry指针
 * @dp: 待注册的子条目（文件/目录/符号链接）
 *
 * 功能：分配inode号，把dp插入父目录的子节点单向链表，
 *       根据文件类型补齐默认的inode操作/文件操作函数集，更新父目录硬链接计数
 * 返回值：成功返回0；make_inode_number分配失败返回-EAGAIN
 */
static int proc_register(struct proc_dir_entry * dir, struct proc_dir_entry * dp)
{
	int	i;
	
	// 分配proc内部使用的inode编号
	i = make_inode_number();
	if (i < 0)
		return -EAGAIN;

	// 将分配到的inode号保存到条目low_ino成员
	dp->low_ino = i;

	/* ========== 链表头插：把当前dp加入父目录dir的子节点链表 ==========
     * 父dir->subdir保存链表第一个子节点
     * 新节点next指向原来第一个子节点，再让父subdir指向新节点（头插法）
     */
	dp->next = dir->subdir;
	dp->parent = dir;
	dir->subdir = dp;

	// 判断条目类型：目录
	if (S_ISDIR(dp->mode)) {
		// 如果目录没有指定inode操作集，则填充目录默认fops/iops
		if (dp->proc_iops == NULL) {
			dp->proc_fops = &proc_dir_operations;
			dp->proc_iops = &proc_dir_inode_operations;
		}
		// 新增子目录，父目录硬链接nlink +1
        // 因为每新建一个子目录，子目录里面的".."就会指向父目录，父链接计数+1
		dir->nlink++;
	} else if (S_ISLNK(dp->mode)) {
		// 符号链接未指定iops时，赋予符号链接默认inode操作集
		if (dp->proc_iops == NULL)
			dp->proc_iops = &proc_link_inode_operations;
	} else if (S_ISREG(dp->mode)) {
		// 普通文件未指定文件操作集，填充默认proc文件fops
		if (dp->proc_fops == NULL)
			dp->proc_fops = &proc_file_operations;
	}
	return 0;
}

/*
 * Kill an inode that got unregistered..
 */
static void proc_kill_inodes(struct proc_dir_entry *de)
{
	struct list_head *p;
	struct super_block *sb = proc_mnt->mnt_sb;

	/*
	 * Actually it's a partial revoke().
	 */
	file_list_lock();
	for (p = sb->s_files.next; p != &sb->s_files; p = p->next) {
		struct file * filp = list_entry(p, struct file, f_list);
		struct dentry * dentry;
		struct inode * inode;

		dentry = filp->f_dentry;
		if (!dentry)
			continue;
		if (dentry->d_op != &proc_dentry_operations)
			continue;
		inode = dentry->d_inode;
		if (inode->u.generic_ip != de)
			continue;
		fops_put(filp->f_op);
		filp->f_op = NULL;
	}
	file_list_unlock();
}

struct proc_dir_entry *proc_symlink(const char *name,
		struct proc_dir_entry *parent, const char *dest)
{
	struct proc_dir_entry *ent = NULL;
	const char *fn = name;
	int len;

	if (!parent && xlate_proc_name(name, &parent, &fn) != 0)
		goto out;
	len = strlen(fn);

	ent = kmalloc(sizeof(struct proc_dir_entry) + len + 1, GFP_KERNEL);
	if (!ent)
		goto out;
	memset(ent, 0, sizeof(struct proc_dir_entry));
	memcpy(((char *) ent) + sizeof(*ent), fn, len + 1);
	ent->name = ((char *) ent) + sizeof(*ent);
	ent->namelen = len;
	ent->nlink = 1;
	ent->mode = S_IFLNK|S_IRUGO|S_IWUGO|S_IXUGO;
	ent->data = kmalloc((ent->size=strlen(dest))+1, GFP_KERNEL);
	if (!ent->data) {
		kfree(ent);
		goto out;
	}
	strcpy((char*)ent->data,dest);

	proc_register(parent, ent);
	
out:
	return ent;
}

struct proc_dir_entry *proc_mknod(const char *name, mode_t mode,
		struct proc_dir_entry *parent, kdev_t rdev)
{
	struct proc_dir_entry *ent = NULL;
	const char *fn = name;
	int len;

	if (!parent && xlate_proc_name(name, &parent, &fn) != 0)
		goto out;
	len = strlen(fn);

	ent = kmalloc(sizeof(struct proc_dir_entry) + len + 1, GFP_KERNEL);
	if (!ent)
		goto out;
	memset(ent, 0, sizeof(struct proc_dir_entry));
	memcpy(((char *) ent) + sizeof(*ent), fn, len + 1);
	ent->name = ((char *) ent) + sizeof(*ent);
	ent->namelen = len;
	ent->nlink = 1;
	ent->mode = mode;
	ent->rdev = rdev;

	proc_register(parent, ent);
	
out:
	return ent;
}

struct proc_dir_entry *proc_mkdir(const char *name, struct proc_dir_entry *parent)
{
	struct proc_dir_entry *ent = NULL;
	const char *fn = name;
	int len;

	/*
     * 如果parent传NULL，调用xlate_proc_name：
     * 解析name路径，自动找到对应的父目录，同时修正fn为最终的文件名
     * 解析失败直接跳转到out返回NULL
     */
	if (!parent && xlate_proc_name(name, &parent, &fn) != 0)
		goto out;
	
	// 获取最终文件名fn的长度
	len = strlen(fn);

	/*
     * kmalloc分配内存：
     * 大小 = proc_dir_entry结构体大小 + 文件名长度 + 1（字符串结束符'\0'）
     * 【关键点】：把文件名直接紧跟在proc_dir_entry结构体后面一块内存里存放
     * GFP_KERNEL：常规内核内存分配标识，可睡眠
     */
	ent = kmalloc(sizeof(struct proc_dir_entry) + len + 1, GFP_KERNEL);
	if (!ent)
		goto out;
	memset(ent, 0, sizeof(struct proc_dir_entry));

	// 把文件名fn拷贝到结构体后面预留的内存空间，包含末尾'\0'
	memcpy(((char *) ent) + sizeof(*ent), fn, len + 1);

	// 让ent->name指向结构体后面存放文件名的缓冲区
	ent->name = ((char *) ent) + sizeof(*ent);
	ent->namelen = len;
	ent->proc_fops = &proc_dir_operations;
	ent->proc_iops = &proc_dir_inode_operations;
	ent->nlink = 2;
	ent->mode = S_IFDIR | S_IRUGO | S_IXUGO;

	// 注册一个目录
	proc_register(parent, ent);
	
out:
	return ent;
}

/**
 * create_proc_entry - 创建proc文件/目录项
 * @name: 待创建条目名称（字符串）
 * @mode: 文件权限模式，包含文件类型(S_IFREG/S_IFDIR)和权限位
 * @parent: 父目录proc_dir_entry，NULL代表使用根proc目录，会经过xlate_proc_name转换
 *
 * 返回值：成功返回proc_dir_entry指针；内存分配失败/名称解析失败返回NULL
 */
struct proc_dir_entry *create_proc_entry(const char *name, mode_t mode,
					 struct proc_dir_entry *parent)
{
	struct proc_dir_entry *ent = NULL;
	const char *fn = name;
	int len;

	/*
     * 如果parent传NULL，调用xlate_proc_name：
     * 解析name路径，自动找到对应的父目录，同时修正fn为最终的文件名
     * 解析失败直接跳转到out返回NULL
     */
	if (!parent && xlate_proc_name(name, &parent, &fn) != 0)
		goto out;

	// 获取最终文件名fn的长度
	len = strlen(fn);

	/*
     * kmalloc分配内存：
     * 大小 = proc_dir_entry结构体大小 + 文件名长度 + 1（字符串结束符'\0'）
     * 【关键点】：把文件名直接紧跟在proc_dir_entry结构体后面一块内存里存放
     * GFP_KERNEL：常规内核内存分配标识，可睡眠
     */
	ent = kmalloc(sizeof(struct proc_dir_entry) + len + 1, GFP_KERNEL);
	if (!ent)
		goto out;
	memset(ent, 0, sizeof(struct proc_dir_entry));

	// 把文件名fn拷贝到结构体后面预留的内存空间，包含末尾'\0'
	memcpy(((char *) ent) + sizeof(*ent), fn, len + 1);
	// 让ent->name指向结构体后面存放文件名的缓冲区
	ent->name = ((char *) ent) + sizeof(*ent);
	// 记录文件名长度，proc内部使用，避免多次strlen
	ent->namelen = len;

	// 判断mode是否是目录类型 S_ISDIR(mode)
	if (S_ISDIR(mode)) {
		// 如果用户没有设置任何ugo权限，则默认赋予：所有用户可读、可执行(进入目录)
		if ((mode & S_IALLUGO) == 0)
		mode |= S_IRUGO | S_IXUGO;

		// 目录的文件操作函数集
		ent->proc_fops = &proc_dir_operations;
		ent->proc_iops = &proc_dir_inode_operations;
		ent->nlink = 2;
	} else {
		// 非目录场景：如果mode没有指定文件类型，默认设置为普通文件 S_IFREG
		if ((mode & S_IFMT) == 0)
			mode |= S_IFREG;
		if ((mode & S_IALLUGO) == 0)
			mode |= S_IRUGO;

		// 普通文件硬链接数为1
		ent->nlink = 1;
	}
	// 把最终修正后的mode写入proc条目
	ent->mode = mode;

	proc_register(parent, ent);
	
out:
	return ent;
}

void free_proc_entry(struct proc_dir_entry *de)
{
	int ino = de->low_ino;

	if (ino < PROC_DYNAMIC_FIRST ||
	    ino >= PROC_DYNAMIC_FIRST+PROC_NDYNAMIC)
		return;
	if (S_ISLNK(de->mode) && de->data)
		kfree(de->data);
	kfree(de);
}

/*
 * Remove a /proc entry and free it if it's not currently in use.
 * If it is in use, we set the 'deleted' flag.
 */
void remove_proc_entry(const char *name, struct proc_dir_entry *parent)
{
	struct proc_dir_entry **p;
	struct proc_dir_entry *de;
	const char *fn = name;
	int len;

	if (!parent && xlate_proc_name(name, &parent, &fn) != 0)
		goto out;
	len = strlen(fn);
	for (p = &parent->subdir; *p; p=&(*p)->next ) {
		if (!proc_match(len, fn, *p))
			continue;
		de = *p;
		*p = de->next;
		de->next = NULL;
		if (S_ISDIR(de->mode))
			parent->nlink--;
		clear_bit(de->low_ino-PROC_DYNAMIC_FIRST,
				(void *) proc_alloc_map);
		proc_kill_inodes(de);
		de->nlink = 0;
		if (!atomic_read(&de->count))
			free_proc_entry(de);
		else {
			de->deleted = 1;
			printk("remove_proc_entry: %s/%s busy, count=%d\n",
				parent->name, de->name, atomic_read(&de->count));
		}
		break;
	}
out:
	return;
}
