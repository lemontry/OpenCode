/*
 *  linux/fs/pipe.c
 *
 *  Copyright (C) 1991, 1992, 1999  Linus Torvalds
 */

#include <linux/mm.h>
#include <linux/file.h>
#include <linux/poll.h>
#include <linux/malloc.h>
#include <linux/module.h>
#include <linux/init.h>

#include <asm/uaccess.h>

/*
 * We use a start+len construction, which provides full use of the 
 * allocated memory.
 * -- Florian Coosmann (FGC)
 * 
 * Reads with count = 0 should always return 0.
 * -- Julian Bradfield 1999-06-07.
 */

/* Drop the inode semaphore and wait for a pipe event, atomically */
/**
 * pipe_wait - 管道读写阻塞等待函数（Linux2.4）
 * @inode: 管道对应的inode
 * 作用：管道条件不满足时，释放管道信号量，进程睡眠，被唤醒后重新拿回信号量
 */
void pipe_wait(struct inode * inode)
{
    // 在当前进程栈上声明等待节点，绑定current当前进程
    DECLARE_WAITQUEUE(wait, current);
    // 设置进程状态：可中断睡眠，可以被信号唤醒
    current->state = TASK_INTERRUPTIBLE;
    // 将等待节点加入管道的等待队列 PIPE_WAIT(*inode)
    add_wait_queue(PIPE_WAIT(*inode), &wait);
    // 释放管道的信号量！重点：先解锁，别的进程可以操作管道缓冲区
    up(PIPE_SEM(*inode));
    // 让出CPU，进入睡眠，等待wake_up唤醒
    schedule();
    // 被唤醒之后，把等待节点从等待队列摘除
    remove_wait_queue(PIPE_WAIT(*inode), &wait);
    // 恢复进程状态为运行态
    current->state = TASK_RUNNING;
    // 重新获取管道信号量，继续操作管道缓冲区
    down(PIPE_SEM(*inode));
}

/**
 * pipe_read - 管道读端的read回调函数（read_pipe_fops->read）
 * @filp: 读端的struct file
 * @buf: 用户态接收缓冲区
 * @count: 用户想要读取的字节数
 * @ppos: 文件偏移指针
 *
 * 返回值：成功返回读到的字节数；0代表EOF；负数是错误码
 * 逻辑：阻塞/非阻塞读环形管道buffer，空时睡眠等待写端写入或者写端全部关闭
 */
static ssize_t
pipe_read(struct file *filp, char *buf, size_t count, loff_t *ppos)
{
    struct inode *inode = filp->f_dentry->d_inode;
    ssize_t size, read, ret;

    // Seeks are not allowed on pipes.
    ret = -ESPIPE;
    read = 0;
    // 管道不支持偏移，ppos必须等于&filp->f_pos，否则返回ESPIPE
    if (ppos != &filp->f_pos)
        goto out_nolock;

    // Always return 0 on null read.
    ret = 0;
    // 用户读0字节，直接返回0
    if (count == 0)
        goto out_nolock;

    // Get the pipe semaphore
    ret = -ERESTARTSYS;
    // 获取管道信号量，保护pipe的环形缓冲区、读写计数；可被信号打断
    if (down_interruptible(PIPE_SEM(*inode)))
        goto out_nolock;

    // 如果管道当前缓冲区为空
    if (PIPE_EMPTY(*inode)) {
do_more_read:
        ret = 0;
        // 缓冲区空 并且 所有写端都关闭 → 返回0，代表EOF
        if (!PIPE_WRITERS(*inode))
            goto out;

        ret = -EAGAIN;
        // 非阻塞模式、管道为空、还有写者存在 → 返回-EAGAIN
        if (filp->f_flags & O_NONBLOCK)
            goto out;

        // 阻塞模式：循环等待数据到来
        for (;;) {
            // 等待读进程计数+1，给写端判断有没有等待的读者
            PIPE_WAITING_READERS(*inode)++;
            // 在管道等待队列上睡眠
            pipe_wait(inode);
            // 唤醒后，等待读者计数减回
            PIPE_WAITING_READERS(*inode)--;

			//先设置中断
            ret = -ERESTARTSYS;

            // 睡眠过程收到信号，返回可重启系统调用错误
            if (signal_pending(current))
                goto out;

            ret = 0;
            // 被唤醒，管道里面有数据了，跳出等待循环，开始读数据
            if (!PIPE_EMPTY(*inode))
                break;
            // 再次检查：依然为空，写端全部关闭 → EOF
            if (!PIPE_WRITERS(*inode))
                goto out;
        }
    }

    // Read what data is available.
    ret = -EFAULT;
    // 循环拷贝缓冲区数据到用户空间，直到读完用户请求count或者管道空
    while (count > 0 && (size = PIPE_LEN(*inode))) {
        // 环形缓冲区当前的读起始地址
        char *pipebuf = PIPE_BASE(*inode) + PIPE_START(*inode);
        // 本次最多可以读取的块大小（防止越界到页尾）
        ssize_t chars = PIPE_MAX_RCHUNK(*inode);
        if (chars > count)
            chars = count;
        if (chars > size)
            chars = size;

        // 拷贝内核管道buffer → 用户buf；失败返回-EFAULT
        if (copy_to_user(buf, pipebuf, chars))
            goto out;

        read += chars;
        // 更新读指针start，环形回绕（PIPE_SIZE是4096）
        PIPE_START(*inode) += chars;
        PIPE_START(*inode) &= (PIPE_SIZE - 1);
        // 有效数据长度减少
        PIPE_LEN(*inode) -= chars;
        count -= chars;
        buf += chars;
    }

    // Cache behaviour optimization
    // 管道读空之后，把start重置为0，减少环形取模的开销
    if (!PIPE_LEN(*inode))
        PIPE_START(*inode) = 0;

    // 如果用户还想要更多数据，并且有写进程正在等待写、当前是阻塞模式
    if (count && PIPE_WAITING_WRITERS(*inode) && !(filp->f_flags & O_NONBLOCK)) {
        /*
         * We know that we are going to sleep: signal
         * writers synchronously that there is more
         * room.
         */
        // 同步唤醒写进程，写进程拿到空间写入数据
        wake_up_interruptible_sync(PIPE_WAIT(*inode));
        // 唤醒写者之后管道不应该还有数据，否则异常
        if (!PIPE_EMPTY(*inode))
            BUG();
        // 回到标签，继续等待新数据
        goto do_more_read;
    }

    // Signal writers asynchronously that there is more room.
    // 读完一部分数据，缓冲区腾出空间，异步唤醒阻塞的写进程
    wake_up_interruptible(PIPE_WAIT(*inode));

    ret = read;
out:
    // 释放管道互斥信号量
    up(PIPE_SEM(*inode));
out_nolock:
    // 如果已经读到数据，优先返回读到的长度，覆盖错误码
    if (read)
        ret = read;
    return ret;
}
/**
 * pipe_write - 管道写端 write 回调（write_pipe_fops->write）
 * @filp: 写端对应的struct file
 * @buf: 用户态待写入数据缓冲区
 * @count: 用户想要写入的字节数
 * @ppos: 文件偏移指针
 *
 * 返回：成功返回已写入字节数；失败 -EPIPE / -EAGAIN / -ERESTARTSYS / -EFAULT
 * 核心规则：PIPE_BUF以内的写保证原子；没有读端时写触发SIGPIPE
 */
static ssize_t
pipe_write(struct file *filp, const char *buf, size_t count, loff_t *ppos)
{
    struct inode *inode = filp->f_dentry->d_inode;
    ssize_t free, written, ret;

    /* Seeks are not allowed on pipes. */
    ret = -ESPIPE;
    written = 0;
    // 管道不支持seek，ppos必须等于filp->f_pos，否则返回ESPIPE
    if (ppos != &filp->f_pos)
        goto out_nolock;

    /* Null write succeeds. */
    ret = 0;
    // 写入0字节直接成功返回0
    if (count == 0)
        goto out_nolock;

    ret = -ERESTARTSYS;
    // 获取管道信号量，可被信号打断
    if (down_interruptible(PIPE_SEM(*inode)))
        goto out_nolock;

    /* No readers yields SIGPIPE. */
    // 关键：管道已经没有任何读端，写操作触发SIGPIPE
    if (!PIPE_READERS(*inode))
        goto sigpipe;

    /* If count <= PIPE_BUF, we have to make it atomic. */
    // 原子写判断：小于等于PIPE_BUF，要求一次写完，free取count；大报文可以分段写，free=1
    free = (count <= PIPE_BUF ? count : 1);

    /* Wait, or check for, available space. */
    if (filp->f_flags & O_NONBLOCK) {
        ret = -EAGAIN;
        // 非阻塞模式，空闲空间不够本次最小要求，直接返回EAGAIN
        if (PIPE_FREE(*inode) < free)
            goto out;
    } else {
        // 阻塞模式：空闲空间不足，循环等待空间
        while (PIPE_FREE(*inode) < free) {
            // 等待写者计数+1，给读端判断是否有写者在等空间
            PIPE_WAITING_WRITERS(*inode)++;
            pipe_wait(inode);
            PIPE_WAITING_WRITERS(*inode)--;

            ret = -ERESTARTSYS;
            // 等待时收到信号，退出
            if (signal_pending(current))
                goto out;
            // 唤醒后检查：读端全部关闭，走SIGPIPE
            if (!PIPE_READERS(*inode))
                goto sigpipe;
        }
    }

    /* Copy into available space. */
    ret = -EFAULT;
    // 循环把用户数据拷贝进管道环形缓冲区
    while (count > 0) {
        int space;
        // 取环形缓冲区写入位置：PIPE_END = start + len
        char *pipebuf = PIPE_BASE(*inode) + PIPE_END(*inode);
        // 本次可以写入的最大块（防止跨页边界）
        ssize_t chars = PIPE_MAX_WCHUNK(*inode);

        if ((space = PIPE_FREE(*inode)) != 0) {
            if (chars > count)
                chars = count;
            if (chars > space)
                chars = space;
            // 用户空间 → 内核管道buffer
            if (copy_from_user(pipebuf, buf, chars))
                goto out;

            written += chars;
            PIPE_LEN(*inode) += chars;
            count -= chars;
            buf += chars;
            space = PIPE_FREE(*inode);
            continue;
        }

        ret = written;
        // 非阻塞，没空间了直接退出循环
        if (filp->f_flags & O_NONBLOCK)
            break;

        // 阻塞模式，缓冲区满，继续睡眠等待读端读出数据
        do {
            /*
             * Synchronous wake-up: it knows that this process
             * is going to give up this CPU, so it doesnt have
             * to do idle reschedules.
             */
            // 同步唤醒读进程，不触发调度
            wake_up_interruptible_sync(PIPE_WAIT(*inode));
            PIPE_WAITING_WRITERS(*inode)++;
            pipe_wait(inode);
            PIPE_WAITING_WRITERS(*inode)--;

            if (signal_pending(current))
                goto out;
            if (!PIPE_READERS(*inode))
                goto sigpipe;
        } while (!PIPE_FREE(*inode));
        ret = -EFAULT;
    }

    /* Signal readers asynchronously that there is more data. */
    // 写入完成，异步唤醒阻塞的读进程，通知有新数据
    wake_up_interruptible(PIPE_WAIT(*inode));
    // 更新inode修改时间，标记inode为脏（pipefs虽然不落地磁盘，维护元数据时间）
    inode->i_ctime = inode->i_mtime = CURRENT_TIME;
    mark_inode_dirty(inode);

out:
    up(PIPE_SEM(*inode));
out_nolock:
    // 如果已经成功写入部分数据，优先返回已写长度，覆盖错误码
    if (written)
        ret = written;
    return ret;

sigpipe:
    // 如果已经写进去一部分数据，先正常返回，不发信号
    if (written)
        goto out;
    up(PIPE_SEM(*inode));
    // 没有读端，且本次一个字节都没写，向当前进程发送SIGPIPE，返回-EPIPE
    send_sig(SIGPIPE, current, 0);
    return -EPIPE;
}

static loff_t
pipe_lseek(struct file *file, loff_t offset, int orig)
{
	return -ESPIPE;
}

static ssize_t
bad_pipe_r(struct file *filp, char *buf, size_t count, loff_t *ppos)
{
	return -EBADF;
}

static ssize_t
bad_pipe_w(struct file *filp, const char *buf, size_t count, loff_t *ppos)
{
	return -EBADF;
}

static int
pipe_ioctl(struct inode *pino, struct file *filp,
	   unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
		case FIONREAD:
			return put_user(PIPE_LEN(*pino), (int *)arg);
		default:
			return -EINVAL;
	}
}

/* No kernel lock held - fine */
/**
 * pipe_poll - pipe文件的poll回调函数（Linux2.4）
 * @filp: 文件描述符对应的file结构体
 * @wait: poll_table，poll机制用来挂载等待节点
 * 返回值：poll事件掩码 POLLIN/POLLOUT/POLLHUP/POLLERR
 */
static unsigned int
pipe_poll(struct file *filp, poll_table *wait)
{
    unsigned int mask;
    // 获取管道对应的inode
    struct inode *inode = filp->f_dentry->d_inode;
    // 把当前进程挂到管道的等待队列 PIPE_WAIT(*inode)
    poll_wait(filp, PIPE_WAIT(*inode), wait);
    /* Reading only -- no need for acquiring the semaphore.  */
    // 默认：可读、正常可读
    mask = POLLIN | POLLRDNORM;
    // 如果管道缓冲区为空 → 改成可写事件
    if (PIPE_EMPTY(*inode))
        mask = POLLOUT | POLLWRNORM;
    // 没有写端，并且版本号发生变化，标记POLLHUP（读端收到挂起）
    if (!PIPE_WRITERS(*inode) && filp->f_version != PIPE_WCOUNTER(*inode))
        mask |= POLLHUP;
    // 没有读端存在，标记POLLERR（写端异常）
    if (!PIPE_READERS(*inode))
        mask |= POLLERR;
    return mask;
}

/* FIXME: most Unices do not set POLLERR for fifos */
#define fifo_poll pipe_poll

/**
 * pipe_release - 管道通用释放逻辑，读端/写端release都会调用它
 * @inode: 管道对应的inode
 * @decr: 需要减去的读端计数
 * @decw: 需要减去的写端计数
 *
 * 功能：减少读者、写者引用计数；判断是否所有端都关闭，是则释放管道缓冲区与pipe_inode_info；否则唤醒阻塞在管道上的进程
 */
static int
pipe_release(struct inode *inode, int decr, int decw)
{
    // 获取管道信号量，保护pipe_inode_info里面的计数、缓冲区状态
    down(PIPE_SEM(*inode));

    // 读引用计数减去decr
    PIPE_READERS(*inode) -= decr;
    // 写引用计数减去decw
    PIPE_WRITERS(*inode) -= decw;

    // 判断：读端、写端全部关闭
    if (!PIPE_READERS(*inode) && !PIPE_WRITERS(*inode)) {
        struct pipe_inode_info *info = inode->i_pipe;
        // 置空i_pipe，防止别人再访问已经释放的管道信息
        inode->i_pipe = NULL;
        // 释放管道环形缓冲区page（前面pipe_new分配的单页）
        free_page((unsigned long) info->base);
        // 释放pipe_inode_info控制结构体
        kfree(info);
    } else {
        // 还有读写端存活，唤醒等待队列上阻塞的读写进程
        // 典型场景：写端关闭，唤醒读进程，让读进程拿到EOF；读端关闭，唤醒写进程触发SIGPIPE
        wake_up_interruptible(PIPE_WAIT(*inode));
    }

    up(PIPE_SEM(*inode));
    return 0;
}


static int
pipe_read_release(struct inode *inode, struct file *filp)
{
	return pipe_release(inode, 1, 0);
}

static int
pipe_write_release(struct inode *inode, struct file *filp)
{
	return pipe_release(inode, 0, 1);
}

static int
pipe_rdwr_release(struct inode *inode, struct file *filp)
{
	int decr, decw;

	decr = (filp->f_mode & FMODE_READ) != 0;
	decw = (filp->f_mode & FMODE_WRITE) != 0;
	return pipe_release(inode, decr, decw);
}

static int
pipe_read_open(struct inode *inode, struct file *filp)
{
	/* We could have perhaps used atomic_t, but this and friends
	   below are the only places.  So it doesn't seem worthwhile.  */
	down(PIPE_SEM(*inode));
	PIPE_READERS(*inode)++;
	up(PIPE_SEM(*inode));

	return 0;
}

static int
pipe_write_open(struct inode *inode, struct file *filp)
{
	down(PIPE_SEM(*inode));
	PIPE_WRITERS(*inode)++;
	up(PIPE_SEM(*inode));

	return 0;
}

static int
pipe_rdwr_open(struct inode *inode, struct file *filp)
{
	down(PIPE_SEM(*inode));
	if (filp->f_mode & FMODE_READ)
		PIPE_READERS(*inode)++;
	if (filp->f_mode & FMODE_WRITE)
		PIPE_WRITERS(*inode)++;
	up(PIPE_SEM(*inode));

	return 0;
}

/*
 * The file_operations structs are not static because they
 * are also used in linux/fs/fifo.c to do operations on FIFOs.
 */

//fifo 操作
struct file_operations read_fifo_fops = {
	llseek:		pipe_lseek,
	read:		pipe_read,
	write:		bad_pipe_w,
	poll:		fifo_poll,
	ioctl:		pipe_ioctl,
	open:		pipe_read_open,  //正常打开不走这里
	release:	pipe_read_release,
};

struct file_operations write_fifo_fops = {
	llseek:		pipe_lseek,
	read:		bad_pipe_r,
	write:		pipe_write,
	poll:		fifo_poll,
	ioctl:		pipe_ioctl,
	open:		pipe_write_open,  //正常打开不走这里
	release:	pipe_write_release,
};

struct file_operations rdwr_fifo_fops = {
	llseek:		pipe_lseek,
	read:		pipe_read,
	write:		pipe_write,
	poll:		fifo_poll,
	ioctl:		pipe_ioctl,
	open:		pipe_rdwr_open,  //正常打开不走这里
	release:	pipe_rdwr_release,
};

struct file_operations read_pipe_fops = {
    llseek:     pipe_lseek,      // lseek，管道一般不支持偏移移动，里面直接返回ESPIPE
    read:       pipe_read,       // ✅ 读端：合法读回调
    write:      bad_pipe_w,      // ❌ 读端禁止write，调用就报错
    poll:       pipe_poll,       // poll/select，检测是否可读
    ioctl:      pipe_ioctl,      // 管道ioctl控制
    open:       pipe_read_open,  // 打开读端：PIPE_READERS计数++
    release:    pipe_read_release,// 关闭读端：PIPE_READERS计数--，唤醒写端
};

struct file_operations write_pipe_fops = {
    llseek:     pipe_lseek,
    read:       bad_pipe_r,      // ❌ 写端禁止read，调用报错
    write:      pipe_write,      // ✅ 写端：合法写回调
    poll:       pipe_poll,
    ioctl:      pipe_ioctl,
    open:       pipe_write_open, // 打开写端：PIPE_WRITERS计数++
    release:    pipe_write_release,// 关闭写端：PIPE_WRITERS计数--，唤醒读端
};
struct file_operations rdwr_pipe_fops = {
	llseek:		pipe_lseek,
	read:		pipe_read,
	write:		pipe_write,
	poll:		pipe_poll,
	ioctl:		pipe_ioctl,
	open:		pipe_rdwr_open,
	release:	pipe_rdwr_release,
};
/**
 * pipe_new - 为管道inode分配并初始化管道私有缓冲区与pipe_inode_info
 * @inode: 已经分配好的pipefs的inode（来自new_inode）
 *
 * 返回值：成功返回inode；失败释放资源返回NULL
 */
struct inode* pipe_new(struct inode* inode)
{
    unsigned long page;
    // 申请一页物理内存，作为管道环形缓冲区，GFP_USER：允许睡眠、可申请用户页
    page = __get_free_page(GFP_USER);
    if (!page)
        return NULL;

    // 分配管道私有控制结构体 pipe_inode_info，挂到 inode->i_pipe
    inode->i_pipe = kmalloc(sizeof(struct pipe_inode_info), GFP_KERNEL);
    if (!inode->i_pipe)
        goto fail_page;

    //初始化管道信息
    // 初始化等待队列：读写阻塞时在这里睡眠
    init_waitqueue_head(PIPE_WAIT(*inode));
    // 环形缓冲区起始地址，就是刚才分配的page
    PIPE_BASE(*inode) = (char*) page;
    // 环形缓冲区起始偏移、当前数据长度，初始都是0
    PIPE_START(*inode) = PIPE_LEN(*inode) = 0;
    // 读者计数、写者计数初始0
    PIPE_READERS(*inode) = PIPE_WRITERS(*inode) = 0;
    // 正在等待的读进程、等待的写进程计数
    PIPE_WAITING_READERS(*inode) = PIPE_WAITING_WRITERS(*inode) = 0;
    // 读写引用计数器，初始为1
    PIPE_RCOUNTER(*inode) = PIPE_WCOUNTER(*inode) = 1;

    return inode;

fail_page:
    // pipe_inode_info分配失败，先释放前面拿到的page，防止泄漏
    free_page(page);
    return NULL;
}

// 全局变量：保存pipefs的挂载实例（vfsmount），内核全程持有
static struct vfsmount *pipe_mnt;

/**
 * pipefs_delete_dentry - pipefs的dentry删除回调，属于struct dentry_operations
 * @dentry: 待销毁的目录项
 * 返回1：告诉dcache：这个dentry可以直接释放、不需要保留缓存
 */
static int pipefs_delete_dentry(struct dentry *dentry)
{
    return 1;
}
static struct dentry_operations pipefs_dentry_operations = {
	d_delete:	pipefs_delete_dentry,
};

static struct inode * get_pipe_inode(void)
{
	//创建一个inode
	struct inode *inode = get_empty_inode();

	if (!inode)
		goto fail_inode;

	//管道的inode
	if(!pipe_new(inode))
		goto fail_iput;
	PIPE_READERS(*inode) = PIPE_WRITERS(*inode) = 1;
	//管道的读写
	inode->i_fop = &rdwr_pipe_fops;
	inode->i_sb = pipe_mnt->mnt_sb;

	/*
	 * Mark the inode dirty from the very beginning,
	 * that way it will never be moved to the dirty
	 * list because "mark_inode_dirty()" will think
	 * that it already _is_ on the dirty list.
	 */
	inode->i_state = I_DIRTY;
	inode->i_mode = S_IFIFO | S_IRUSR | S_IWUSR;
	inode->i_uid = current->fsuid;
	inode->i_gid = current->fsgid;
	inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
	inode->i_blksize = PAGE_SIZE;
	return inode;

fail_iput:
	iput(inode);
fail_inode:
	return NULL;
}

/**
 * do_pipe - Linux 2.4 匿名管道的内核实现，pipe系统调用的核心C函数
 * @fd: 用户传入int数组，成功后 fd[0]=读端fd，fd[1]=写端fd
 *
 * 调用链路：sys_pipe -> do_pipe
 * 核心：创建**匿名管道**，管道本质是 pipefs 伪文件系统的inode，内核环形缓冲区；
 * 生成两个struct file：读文件、写文件，两个file共享同一个inode、同一个dentry；
 * 分配两个空闲文件描述符，调用fd_install绑定到当前进程files_struct。
 *
 * 无名管道特点：半双工，依托VFS，没有磁盘实体，只存在内核内存；
 * 读端f1使用read_pipe_fops，写端f2使用write_pipe_fops。
 */
int do_pipe(int *fd)
{
	struct qstr this;
	char name[32];
	struct dentry *dentry;
	struct inode * inode;
	struct file *f1, *f2;
	int error;
	int i,j;

	error = -ENFILE;
	// 1. 从全局空闲file链表取出两个空的struct file，分别给读端、写端
	f1 = get_empty_filp();
	if (!f1)
		goto no_files;

	f2 = get_empty_filp();
	if (!f2)
		goto close_f1;

	// 2. 创建管道inode，分配pipe内核缓冲区（一页内存，4K）
	inode = get_pipe_inode();
	if (!inode)
		goto close_f12;

	//获取一个fd
	error = get_unused_fd();
	if (error < 0)
		goto close_f12_inode;
	i = error;

	error = get_unused_fd();
	if (error < 0)
		goto close_f12_inode_i;
	j = error;

	error = -ENOMEM;

	// 管道是匿名文件，名字是 "[inode编号]"，pipefs内部名称
	sprintf(name, "[%lu]", inode->i_ino);
	this.name = name;
	this.len = strlen(name);
	this.hash = inode->i_ino; /* will go */

	// 4. 在pipefs根目录下分配dentry（目录项），管道匿名dentry
	dentry = d_alloc(pipe_mnt->mnt_sb->s_root, &this);
	if (!dentry)
		goto close_f12_inode_i_j;

	// 设置pipefs专属dentry操作集
	dentry->d_op = &pipefs_dentry_operations;
	d_add(dentry, inode);
	f1->f_vfsmnt = f2->f_vfsmnt = mntget(mntget(pipe_mnt));
	f1->f_dentry = f2->f_dentry = dget(dentry);

	/* read file */
	f1->f_pos = f2->f_pos = 0;
	f1->f_flags = O_RDONLY;
	f1->f_op = &read_pipe_fops;
	f1->f_mode = 1;
	f1->f_version = 0;

	/* write file */
	f2->f_flags = O_WRONLY;
	f2->f_op = &write_pipe_fops;
	f2->f_mode = 2;
	f2->f_version = 0;

	fd_install(i, f1);
	fd_install(j, f2);
	fd[0] = i;
	fd[1] = j;
	return 0;

close_f12_inode_i_j:
	put_unused_fd(j);
close_f12_inode_i:
	put_unused_fd(i);
close_f12_inode:
	free_page((unsigned long) PIPE_BASE(*inode));
	kfree(inode->i_pipe);
	inode->i_pipe = NULL;
	iput(inode);
close_f12:
	put_filp(f2);
close_f1:
	put_filp(f1);
no_files:
	return error;	
}

/*
 * pipefs should _never_ be mounted by userland - too much of security hassle,
 * no real gain from having the whole whorehouse mounted. So we don't need
 * any operations on the root directory. However, we need a non-trivial
 * d_name - pipe: will go nicely and kill the special-casing in procfs.
 */
static int pipefs_statfs(struct super_block *sb, struct statfs *buf)
{
	buf->f_type = PIPEFS_MAGIC;
	buf->f_bsize = 1024;
	buf->f_namelen = 255;
	return 0;
}

static struct super_operations pipefs_ops = {
	statfs:		pipefs_statfs,
};

/**
 * pipefs_read_super - pipefs 的读超级块回调，file_system_type->get_sb 指向它
 * @sb: 内核已经分配好的super_block实例
 * @data: mount传入的参数，pipefs不用
 * @silent: 是否静默，不打印报错信息
 *
 * 功能：构造pipefs的超级块、根inode、根dentry；pipefs是内存伪文件系统，无磁盘读取
 */
static struct super_block * pipefs_read_super(struct super_block *sb, void *data, int silent)
{
    // 分配根目录inode，隶属于这个新的super_block
    struct inode *root = new_inode(sb);
    if (!root)
        return NULL;

    // 设置根inode属性：目录类型，权限0600，属主root
    root->i_mode = S_IFDIR | S_IRUSR | S_IWUSR;
    root->i_uid = root->i_gid = 0;
    // 三个时间戳统一设为当前时间
    root->i_atime = root->i_mtime = root->i_ctime = CURRENT_TIME;

    // 设置超级块信息
    sb->s_blocksize = 1024;
    sb->s_blocksize_bits = 10;  // 2^10 = 1024
    sb->s_magic = PIPEFS_MAGIC; // 文件系统魔数，用来标识pipefs
    sb->s_op    = &pipefs_ops;   // super_block的操作方法集

    // 分配根dentry，名字叫 "pipe:"
    sb->s_root = d_alloc(NULL, &(const struct qstr) { "pipe:", 5, 0 });
    if (!sb->s_root) {
        // dentry分配失败，释放刚才建好的inode
        iput(root);
        return NULL;
    }
    // dentry反向关联super block
    sb->s_root->d_sb = sb;
    // 根目录的父dentry指向自己（VFS根目录惯例：自环）
    sb->s_root->d_parent = sb->s_root;

    // 把dentry和inode绑定，实例化这个dentry
    d_instantiate(sb->s_root, root);

    return sb;
}

static DECLARE_FSTYPE(pipe_fs_type, "pipefs", pipefs_read_super,
	FS_NOMOUNT|FS_SINGLE);

/**
 * init_pipe_fs - 管道文件系统初始化函数，__init 标记，内核启动阶段执行
 *
 * 功能：注册 pipefs 文件系统类型，并且内核内部自动挂载 pipefs，供匿名管道使用
 * pipefs 是一个只存在于内存、没有磁盘后端的特殊文件系统（伪文件系统）
 */
static int __init init_pipe_fs(void)
{
    // 1. 向VFS注册文件系统类型 pipe_fs_type（对应 register_filesystem，你前面看过）
    int err = register_filesystem(&pipe_fs_type);
    if (!err) {
        // 2. 内核内部挂载，不是用户态mount命令；返回vfsmount指针
        pipe_mnt = kern_mount(&pipe_fs_type);
        // 取出错误码（如果是错误指针，PTR_ERR拿到负数错误号）
        err = PTR_ERR(pipe_mnt);
        if (IS_ERR(pipe_mnt))
            // 挂载失败，回滚：注销刚才注册的文件系统类型
            unregister_filesystem(&pipe_fs_type);
        else
            // 挂载成功，err置0表示无错误
            err = 0;
    }
    return err;
}

static void __exit exit_pipe_fs(void)
{
	unregister_filesystem(&pipe_fs_type);
	kern_umount(pipe_mnt);
}

module_init(init_pipe_fs)
module_exit(exit_pipe_fs)
