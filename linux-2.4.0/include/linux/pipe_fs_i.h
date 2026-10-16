#ifndef _LINUX_PIPE_FS_I_H
#define _LINUX_PIPE_FS_I_H

#define PIPEFS_MAGIC 0x50495045
/**
 * struct pipe_inode_info - Linux 2.4 管道inode私有信息，存放在inode->i_pipe
 * 管道环形缓冲区控制结构体，附着在pipefs的inode上；
 * 同一个管道的读、写两个struct file，共享同一个inode，也就共享这一份pipe_inode_info
 */
struct pipe_inode_info {
	wait_queue_head_t wait;		// 等待队列：读等待、写等待共用这一个等待头
	char *base;					// 环形缓冲区起始地址（一页物理内存，4KB）
	unsigned int start;			// 环形缓冲区读指针：下一个要读取的位置

	unsigned int readers;		// 当前读端引用计数：有多少个打开的读文件描述符
	unsigned int writers;		// 当前写端引用计数：有多少个打开的写文件描述符

	unsigned int waiting_readers;	// 正在阻塞等待数据的读进程数量
	unsigned int waiting_writers;	// 正在阻塞等待缓冲区空间的写进程数量

	unsigned int r_counter;	// 读引用计数器，用于唤醒逻辑，防止惊群
	unsigned int w_counter;	// 写引用计数器，用于唤醒逻辑
};

/* Differs from PIPE_BUF in that PIPE_SIZE is the length of the actual
   memory allocation, whereas PIPE_BUF makes atomicity guarantees.  */
#define PIPE_SIZE		PAGE_SIZE

#define PIPE_SEM(inode)		(&(inode).i_sem)
#define PIPE_WAIT(inode)	(&(inode).i_pipe->wait)
#define PIPE_BASE(inode)	((inode).i_pipe->base)
#define PIPE_START(inode)	((inode).i_pipe->start)
#define PIPE_LEN(inode)		((inode).i_size)
#define PIPE_READERS(inode)	((inode).i_pipe->readers)
#define PIPE_WRITERS(inode)	((inode).i_pipe->writers)
#define PIPE_WAITING_READERS(inode)	((inode).i_pipe->waiting_readers)
#define PIPE_WAITING_WRITERS(inode)	((inode).i_pipe->waiting_writers)
#define PIPE_RCOUNTER(inode)	((inode).i_pipe->r_counter)
#define PIPE_WCOUNTER(inode)	((inode).i_pipe->w_counter)

#define PIPE_EMPTY(inode)	(PIPE_LEN(inode) == 0)
#define PIPE_FULL(inode)	(PIPE_LEN(inode) == PIPE_SIZE)
#define PIPE_FREE(inode)	(PIPE_SIZE - PIPE_LEN(inode))
#define PIPE_END(inode)	((PIPE_START(inode) + PIPE_LEN(inode)) & (PIPE_SIZE-1))
#define PIPE_MAX_RCHUNK(inode)	(PIPE_SIZE - PIPE_START(inode))
#define PIPE_MAX_WCHUNK(inode)	(PIPE_SIZE - PIPE_END(inode))

/* Drop the inode semaphore and wait for a pipe event, atomically */
void pipe_wait(struct inode * inode);

struct inode* pipe_new(struct inode* inode);

#endif
