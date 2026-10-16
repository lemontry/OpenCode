/*
 *  linux/fs/read_write.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/malloc.h> 
#include <linux/stat.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/uio.h>
#include <linux/smp_lock.h>
#include <linux/dnotify.h>

#include <asm/uaccess.h>

struct file_operations generic_ro_fops = {
	read:		generic_file_read,
	mmap:		generic_file_mmap,
};

ssize_t generic_read_dir(struct file *filp, char *buf, size_t siz, loff_t *ppos)
{
	return -EISDIR;
}

// default_llseek：VFS 默认的 llseek 实现
// 当文件的 file_operations 没有自定义 llseek 回调（f_op->llseek == NULL）时使用这个函数
// @file：打开的文件实例 struct file
// @offset：传入的偏移量
// @origin：基准位置，0=SEEK_SET，1=SEEK_CUR，2=SEEK_END
// 返回：新文件偏移loff_t；负数为错误码
loff_t default_llseek(struct file *file, loff_t offset, int origin)
{
	long long retval;

	switch (origin) {
		case 2:  // SEEK_END：从文件末尾开始定位
            // 文件末尾 = inode记录的文件大小，offset叠加在文件大小之上
			offset += file->f_dentry->d_inode->i_size;
			break;
		case 1:// SEEK_CUR：从当前文件偏移f_pos开始定位
			offset += file->f_pos;
	}
	retval = -EINVAL;// 默认返回参数无效错误
    // 新偏移不能为负数，文件偏移不允许负值
	if (offset >= 0) {
		// 如果新偏移和当前f_pos不一样，才更新
		if (offset != file->f_pos) {
			file->f_pos = offset;  // 更新struct file的文件偏移
			file->f_reada = 0;  // 重置预读状态，seek之后顺序预读失效
			file->f_version = ++event;  // 文件版本号+1，标记文件位置发生变更
		}
		retval = offset;
	}
	return retval;
}

// llseek：VFS内部lseek封装函数，选择底层llseek回调执行偏移修改
// @file：struct file 文件打开实例
// @offset：要移动的偏移量（64位 loff_t）
// @origin：基准位置 SEEK_SET/SEEK_CUR/SEEK_END
// 返回：新的文件偏移，负数表示错误
static inline loff_t llseek(struct file *file, loff_t offset, int origin)
{
	loff_t (*fn)(struct file *, loff_t, int);
	loff_t retval;

	// 默认使用VFS通用实现 default_llseek
	fn = default_llseek;

	// 如果文件操作集自定义了llseek，则优先选用底层实现
	// 普通 ext2 文件：ext2_file_lseek 几乎和 default_llseek类似
	if (file->f_op && file->f_op->llseek)
		fn = file->f_op->llseek;
	lock_kernel();
	retval = fn(file, offset, origin);
	unlock_kernel();
	return retval;
}

// sys_lseek：lseek系统调用入口，修改文件读写偏移f_pos
// asmlinkage：x86-32系统调用，参数从栈上获取
// @fd：文件描述符
// @offset：偏移值（off_t，32位类型）
// @origin：基准位置，0=SEEK_SET，1=SEEK_CUR，2=SEEK_END
// 返回：新文件偏移(off_t)；负数代表错误
asmlinkage off_t sys_lseek(unsigned int fd, off_t offset, unsigned int origin)
{
	off_t retval;
	struct file * file;

	retval = -EBADF;
	//获取文件对象
	file = fget(fd);
	if (!file)
		goto bad;
	retval = -EINVAL;
	if (origin <= 2) {
		// llseek：底层64位lseek实现，返回loff_t（64位长偏移）
		loff_t res = llseek(file, offset, origin);
		retval = res;
		if (res != (loff_t)retval)
			retval = -EOVERFLOW;	/* LFS: should only happen on 32 bit platforms */
	}
	fput(file);
bad:
	return retval;
}

#if !defined(__alpha__)
asmlinkage long sys_llseek(unsigned int fd, unsigned long offset_high,
			   unsigned long offset_low, loff_t * result,
			   unsigned int origin)
{
	int retval;
	struct file * file;
	loff_t offset;

	retval = -EBADF;
	file = fget(fd);
	if (!file)
		goto bad;
	retval = -EINVAL;
	if (origin > 2)
		goto out_putf;

	offset = llseek(file, ((loff_t) offset_high << 32) | offset_low,
			origin);

	retval = (int)offset;
	if (offset >= 0) {
		retval = -EFAULT;
		if (!copy_to_user(result, &offset, sizeof(offset)))
			retval = 0;
	}
out_putf:
	fput(file);
bad:
	return retval;
}
#endif

// sys_read：read系统调用入口函数（2.4内核）
// asmlinkage：标记函数，要求参数从栈上获取（x86 32位系统调用约定）
// @fd：用户态传入的文件描述符
// @buf：用户态缓冲区地址，存放读到的数据
// @count：期望读取的字节数
// 返回值：>0 实际读到字节；0读到文件末尾；负数负错误码
asmlinkage ssize_t sys_read(unsigned int fd, char * buf, size_t count)
{
	ssize_t ret;
	struct file * file;

	ret = -EBADF;  // 默认错误：无效文件描述符

	// 根据fd，从当前进程files_struct拿到struct file，并且f_count引用计数+1
	file = fget(fd);
	if (file) {

		// 校验：这个打开的file实例是否具备读权限（打开时的FMODE_READ）
		if (file->f_mode & FMODE_READ) {

			// 检查文件锁，验证本次读区域是否被锁禁止
			ret = locks_verify_area(FLOCK_VERIFY_READ, file->f_dentry->d_inode,
						file, file->f_pos, count);
			if (!ret) {
				// 取出文件操作表里的read回调函数指针
				ssize_t (*read)(struct file *, char *, size_t, loff_t *);
				ret = -EINVAL;
				if (file->f_op && (read = file->f_op->read) != NULL)
					// 调用底层文件系统read，传入file、用户buf、count、文件偏移指针&f_pos
                    // read内部会更新file->f_pos
					ret = read(file, buf, count, &file->f_pos);
			}
		}

		// 如果本次读取成功读到数据(ret>0)，发送目录访问事件通知 DN_ACCESS
        // 通知父目录inode：它下面的文件被访问了（inotify前身，dnotify）
		if (ret > 0)
			inode_dir_notify(file->f_dentry->d_parent->d_inode,
				DN_ACCESS);

		// 释放fget拿到的引用计数，f_count--
		fput(file);
	}
	return ret;
}

asmlinkage ssize_t sys_write(unsigned int fd, const char * buf, size_t count)
{
	ssize_t ret;
	struct file * file;

	ret = -EBADF;
	file = fget(fd);
	if (file) {
		if (file->f_mode & FMODE_WRITE) {
			struct inode *inode = file->f_dentry->d_inode;
			ret = locks_verify_area(FLOCK_VERIFY_WRITE, inode, file,
				file->f_pos, count);
			if (!ret) {
				ssize_t (*write)(struct file *, const char *, size_t, loff_t *);
				ret = -EINVAL;
				if (file->f_op && (write = file->f_op->write) != NULL)
					ret = write(file, buf, count, &file->f_pos);
			}
		}
		if (ret > 0)
			inode_dir_notify(file->f_dentry->d_parent->d_inode,
				DN_MODIFY);
		fput(file);
	}
	return ret;
}


static ssize_t do_readv_writev(int type, struct file *file,
			       const struct iovec * vector,
			       unsigned long count)
{
	typedef ssize_t (*io_fn_t)(struct file *, char *, size_t, loff_t *);
	typedef ssize_t (*iov_fn_t)(struct file *, const struct iovec *, unsigned long, loff_t *);

	size_t tot_len;
	struct iovec iovstack[UIO_FASTIOV];
	struct iovec *iov=iovstack;
	ssize_t ret, i;
	io_fn_t fn;
	iov_fn_t fnv;
	struct inode *inode;

	/*
	 * First get the "struct iovec" from user memory and
	 * verify all the pointers
	 */
	ret = 0;
	if (!count)
		goto out_nofree;
	ret = -EINVAL;
	if (count > UIO_MAXIOV)
		goto out_nofree;
	if (!file->f_op)
		goto out_nofree;
	if (count > UIO_FASTIOV) {
		ret = -ENOMEM;
		iov = kmalloc(count*sizeof(struct iovec), GFP_KERNEL);
		if (!iov)
			goto out_nofree;
	}
	ret = -EFAULT;
	if (copy_from_user(iov, vector, count*sizeof(*vector)))
		goto out;

	/* BSD readv/writev returns EINVAL if one of the iov_len
	   values < 0 or tot_len overflowed a 32-bit integer. -ink */
	tot_len = 0;
	ret = -EINVAL;
	for (i = 0 ; i < count ; i++) {
		size_t tmp = tot_len;
		int len = iov[i].iov_len;
		if (len < 0)
			goto out;
		(u32)tot_len += len;
		if (tot_len < tmp || tot_len < (u32)len)
			goto out;
	}

	inode = file->f_dentry->d_inode;
	/* VERIFY_WRITE actually means a read, as we write to user space */
	ret = locks_verify_area((type == VERIFY_WRITE
				 ? FLOCK_VERIFY_READ : FLOCK_VERIFY_WRITE),
				inode, file, file->f_pos, tot_len);
	if (ret) goto out;

	fnv = (type == VERIFY_WRITE ? file->f_op->readv : file->f_op->writev);
	if (fnv) {
		ret = fnv(file, iov, count, &file->f_pos);
		goto out;
	}

	/* VERIFY_WRITE actually means a read, as we write to user space */
	fn = (type == VERIFY_WRITE ? file->f_op->read :
	      (io_fn_t) file->f_op->write);

	ret = 0;
	vector = iov;
	while (count > 0) {
		void * base;
		size_t len;
		ssize_t nr;

		base = vector->iov_base;
		len = vector->iov_len;
		vector++;
		count--;

		nr = fn(file, base, len, &file->f_pos);

		if (nr < 0) {
			if (!ret) ret = nr;
			break;
		}
		ret += nr;
		if (nr != len)
			break;
	}

out:
	if (iov != iovstack)
		kfree(iov);
out_nofree:
	/* VERIFY_WRITE actually means a read, as we write to user space */
	if ((ret + (type == VERIFY_WRITE)) > 0)
		inode_dir_notify(file->f_dentry->d_parent->d_inode,
			(type == VERIFY_WRITE) ? DN_MODIFY : DN_ACCESS);
	return ret;
}

asmlinkage ssize_t sys_readv(unsigned long fd, const struct iovec * vector,
			     unsigned long count)
{
	struct file * file;
	ssize_t ret;


	ret = -EBADF;
	file = fget(fd);
	if (!file)
		goto bad_file;
	if (file->f_op && (file->f_mode & FMODE_READ) &&
	    (file->f_op->readv || file->f_op->read))
		ret = do_readv_writev(VERIFY_WRITE, file, vector, count);
	fput(file);

bad_file:
	return ret;
}

asmlinkage ssize_t sys_writev(unsigned long fd, const struct iovec * vector,
			      unsigned long count)
{
	struct file * file;
	ssize_t ret;


	ret = -EBADF;
	file = fget(fd);
	if (!file)
		goto bad_file;
	if (file->f_op && (file->f_mode & FMODE_WRITE) &&
	    (file->f_op->writev || file->f_op->write))
		ret = do_readv_writev(VERIFY_READ, file, vector, count);
	fput(file);

bad_file:
	return ret;
}

/* From the Single Unix Spec: pread & pwrite act like lseek to pos + op +
   lseek back to original location.  They fail just like lseek does on
   non-seekable files.  */

asmlinkage ssize_t sys_pread(unsigned int fd, char * buf,
			     size_t count, loff_t pos)
{
	ssize_t ret;
	struct file * file;
	ssize_t (*read)(struct file *, char *, size_t, loff_t *);

	ret = -EBADF;
	file = fget(fd);
	if (!file)
		goto bad_file;
	if (!(file->f_mode & FMODE_READ))
		goto out;
	ret = locks_verify_area(FLOCK_VERIFY_READ, file->f_dentry->d_inode,
				file, pos, count);
	if (ret)
		goto out;
	ret = -EINVAL;
	if (!file->f_op || !(read = file->f_op->read))
		goto out;
	if (pos < 0)
		goto out;
	ret = read(file, buf, count, &pos);
	if (ret > 0)
		inode_dir_notify(file->f_dentry->d_parent->d_inode, DN_ACCESS);
out:
	fput(file);
bad_file:
	return ret;
}

asmlinkage ssize_t sys_pwrite(unsigned int fd, const char * buf,
			      size_t count, loff_t pos)
{
	ssize_t ret;
	struct file * file;
	ssize_t (*write)(struct file *, const char *, size_t, loff_t *);

	ret = -EBADF;
	file = fget(fd);
	if (!file)
		goto bad_file;
	if (!(file->f_mode & FMODE_WRITE))
		goto out;
	ret = locks_verify_area(FLOCK_VERIFY_WRITE, file->f_dentry->d_inode,
				file, pos, count);
	if (ret)
		goto out;
	ret = -EINVAL;
	if (!file->f_op || !(write = file->f_op->write))
		goto out;
	if (pos < 0)
		goto out;

	ret = write(file, buf, count, &pos);
	if (ret > 0)
		inode_dir_notify(file->f_dentry->d_parent->d_inode, DN_MODIFY);
out:
	fput(file);
bad_file:
	return ret;
}
