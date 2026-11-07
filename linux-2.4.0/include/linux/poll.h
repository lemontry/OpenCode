#ifndef _LINUX_POLL_H
#define _LINUX_POLL_H

#include <asm/poll.h>

#ifdef __KERNEL__

#include <linux/wait.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <asm/uaccess.h>

struct poll_table_page;

typedef struct poll_table_struct {
	int error;
	struct poll_table_page * table;
} poll_table;

extern void __pollwait(struct file * filp, wait_queue_head_t * wait_address, poll_table *p);

/**
 * poll_wait - poll机制辅助封装函数（Linux2.4）
 * @filp: 待监听的文件对象
 * @wait_address: 等待队列头，要把进程挂到这个等待队列上
 * @p: poll_table，poll的上下文控制结构
 * 作用：条件满足时调用__pollwait，把当前进程的等待节点加入等待队列
 * 注意：本函数**不会让进程休眠**，仅仅是登记等待队列
 */
extern inline void poll_wait(struct file * filp, wait_queue_head_t * wait_address, poll_table *p)
{
    // 合法性判断：poll_table 和等待队列头都不为空才执行
    if (p && wait_address)
        __pollwait(filp, wait_address, p);
}


static inline void poll_initwait(poll_table* pt)
{
	pt->error = 0;
	pt->table = NULL;
}
extern void poll_freewait(poll_table* pt);


/*
 * Scaleable version of the fd_set.
 */

typedef struct {
	unsigned long *in, *out, *ex;
	unsigned long *res_in, *res_out, *res_ex;
} fd_set_bits;

/*
 * How many longwords for "nr" bits?
 */
#define FDS_BITPERLONG	(8*sizeof(long))
#define FDS_LONGS(nr)	(((nr)+FDS_BITPERLONG-1)/FDS_BITPERLONG)
#define FDS_BYTES(nr)	(FDS_LONGS(nr)*sizeof(long))

/*
 * We do a VERIFY_WRITE here even though we are only reading this time:
 * we'll write to it eventually..
 *
 * Use "unsigned long" accesses to let user-mode fd_set's be long-aligned.
 */
static inline
int get_fd_set(unsigned long nr, void *ufdset, unsigned long *fdset)
{
	nr = FDS_BYTES(nr);
	if (ufdset) {
		int error;
		error = verify_area(VERIFY_WRITE, ufdset, nr);
		if (!error && __copy_from_user(fdset, ufdset, nr))
			error = -EFAULT;
		return error;
	}
	memset(fdset, 0, nr);
	return 0;
}

static inline
void set_fd_set(unsigned long nr, void *ufdset, unsigned long *fdset)
{
	if (ufdset)
		__copy_to_user(ufdset, fdset, FDS_BYTES(nr));
}

static inline
void zero_fd_set(unsigned long nr, unsigned long *fdset)
{
	memset(fdset, 0, FDS_BYTES(nr));
}

extern int do_select(int n, fd_set_bits *fds, long *timeout);

#endif /* KERNEL */

#endif /* _LINUX_POLL_H */
