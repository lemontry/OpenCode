/*
 *  linux/fs/fifo.c
 *
 *  written by Paul H. Hargrove
 *
 *  Fixes:
 *	10-06-1999, AV: fixed OOM handling in fifo_open(), moved
 *			initialization there, switched to external
 *			allocation of pipe_inode_info.
 */

#include <linux/mm.h>
#include <linux/malloc.h>
#include <linux/smp_lock.h>

static void wait_for_partner(struct inode* inode, unsigned int* cnt)
{
	int cur = *cnt;	
	while(cur == *cnt) {
		pipe_wait(inode);
		if(signal_pending(current))
			break;
	}
}

static void wake_up_partner(struct inode* inode)
{
	wake_up_interruptible(PIPE_WAIT(*inode));
}

/**
 * 
 * 
 * open("/tmp/myfifo", O_RDONLY)
    → sys_open
        → path_walk 拿到FIFO的dentry、inode
        → dentry_open
            → 取 inode->i_fop = &def_fifo_fops（init_special_inode 设置的）
            → 调用 def_fifo_fops.open，也就是 fifo_open
 */
/**
 * fifo_open - FIFO有名管道的open回调函数（Linux2.4）
 * @inode: FIFO对应的inode
 * @filp: 本次打开生成的file结构体
 * 返回值：0成功，负数错误码
 * 说明：匿名pipe是pipe_new直接创建；FIFO是第一次open的时候才调用pipe_new分配pipe缓冲区
 */
static int fifo_open(struct inode *inode, struct file *filp)
{
    int ret;
    ret = -ERESTARTSYS;
    // 获取BKL大内核锁
    lock_kernel();
    // 可中断的方式获取管道互斥信号量
    if (down_interruptible(PIPE_SEM(*inode)))
        goto err_nolock_nocleanup;
    // 如果inode还没有绑定pipe私有信息，说明是第一次打开这个FIFO
    if (!inode->i_pipe) {
        ret = -ENOMEM;
        // 分配pipe_inode_info、环形缓冲区
        if(!pipe_new(inode))
            goto err_nocleanup;
    }
    filp->f_version = 0;
    // 根据打开模式 f_mode 分支：1=O_RDONLY，2=O_WRONLY，3=O_RDWR
    switch (filp->f_mode) {
    case 1:
    /*
     *  O_RDONLY
     *  POSIX.1 says that O_NONBLOCK means return with the FIFO
     *  opened, even when there is no process writing the FIFO.
     */
        // 绑定读侧的文件操作集 read_fifo_fops（pipe_read、pipe_poll）
        filp->f_op = &read_fifo_fops;
        // 读计数器+1
        PIPE_RCOUNTER(*inode)++;
        // 第一个读进程打开，唤醒等待的写方
        if (PIPE_READERS(*inode)++ == 0)
            wake_up_partner(inode);
        // 当前没有任何写者
        if (!PIPE_WRITERS(*inode)) {
            if ((filp->f_flags & O_NONBLOCK)) {
                /* suppress POLLHUP until we have
                 * seen a writer */
                // 非阻塞打开：直接返回，记录写版本号，poll时用来判断POLLHUP
                filp->f_version = PIPE_WCOUNTER(*inode);
            } else 
            {
                // 阻塞打开：等待写者到来，等待标记是WCOUNTER变化
                wait_for_partner(inode, &PIPE_WCOUNTER(*inode));
                // 等待过程收到信号，退出
                if(signal_pending(current))
                    goto err_rd;
            }
        }
        break;
    
    case 2:
    /*
     *  O_WRONLY
     *  POSIX.1 says that O_NONBLOCK means return -1 with
     *  errno=ENXIO when there is no process reading the FIFO.
     */
        ret = -ENXIO;
        // 只写+非阻塞，并且当前没有读者 → 直接返回ENXIO（POSIX规定）
        if ((filp->f_flags & O_NONBLOCK) && !PIPE_READERS(*inode))
            goto err;
        // 绑定写侧文件操作集 write_fifo_fops（pipe_write、pipe_poll）
        filp->f_op = &write_fifo_fops;
        // 写版本计数器++，用于poll检测写端关闭
        PIPE_WCOUNTER(*inode)++;
        // 第一个写进程打开，唤醒等待的读者
        if (!PIPE_WRITERS(*inode)++)
            wake_up_partner(inode);
        // 目前没有读者，阻塞等待读者打开FIFO
        if (!PIPE_READERS(*inode)) {
            wait_for_partner(inode, &PIPE_RCOUNTER(*inode));
            if (signal_pending(current))
                goto err_wr;
        }
        break;
    
    case 3:
    /*
     *  O_RDWR
     *  POSIX.1 leaves this case "undefined" when O_NONBLOCK is set.
     *  This implementation will NEVER block on a O_RDWR open, since
     *  the process can at least talk to itself.
     */
        // 读写打开，绑定读写fops，open不会阻塞（自己既读又写）
        filp->f_op = &rdwr_fifo_fops;
        // 读者计数+1
        PIPE_READERS(*inode)++;
        // 写者计数+1
        PIPE_WRITERS(*inode)++;
        PIPE_RCOUNTER(*inode)++;
        PIPE_WCOUNTER(*inode)++;
        // 第一个读者或者第一个写者，唤醒对端
        if (PIPE_READERS(*inode) == 1 || PIPE_WRITERS(*inode) == 1)
            wake_up_partner(inode);
        break;
    default:
        ret = -EINVAL;
        goto err;
    }
    /* Ok! */
    up(PIPE_SEM(*inode));
    unlock_kernel();
    return 0;
err_rd:
    // 读打开被信号打断，读计数回退；没有读者了唤醒等待队列
    if (!--PIPE_READERS(*inode))
        wake_up_interruptible(PIPE_WAIT(*inode));
    ret = -ERESTARTSYS;
    goto err;
err_wr:
    // 写打开被信号打断，写计数回退；没有写者唤醒队列
    if (!--PIPE_WRITERS(*inode))
        wake_up_interruptible(PIPE_WAIT(*inode));
    ret = -ERESTARTSYS;
    goto err;
err:
    // 打开失败，如果读写计数都为0，释放pipe缓冲区和pipe_inode_info
    if (!PIPE_READERS(*inode) && !PIPE_WRITERS(*inode)) {
        struct pipe_inode_info *info = inode->i_pipe;
        inode->i_pipe = NULL;
        free_page((unsigned long)info->base);
        kfree(info);
    }
err_nocleanup:
    up(PIPE_SEM(*inode));
err_nolock_nocleanup:
    unlock_kernel();
    return ret;
}

/*
 * Dummy default file-operations: the only thing this does
 * is contain the open that then fills in the correct operations
 * depending on the access mode of the file...
 */
struct file_operations def_fifo_fops = {
	open:		fifo_open,	/* will set read or write pipe_fops */
};
