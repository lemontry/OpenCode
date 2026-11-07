/*
 * linux/ipc/msg.c
 * Copyright (C) 1992 Krishna Balasubramanian 
 *
 * Removed all the remaining kerneld mess
 * Catch the -EFAULT stuff properly
 * Use GFP_KERNEL for messages as in 1.2
 * Fixed up the unchecked user space derefs
 * Copyright (C) 1998 Alan Cox & Andi Kleen
 *
 * /proc/sysvipc/msg support (c) 1999 Dragos Acostachioaie <dragos@iname.com>
 *
 * mostly rewritten, threaded and wake-one semantics added
 * MSGMAX limit removed, sysctl's added
 * (c) 1999 Manfred Spraul <manfreds@colorfullife.com>
 */

#include <linux/config.h>
#include <linux/malloc.h>
#include <linux/msg.h>
#include <linux/spinlock.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/list.h>
#include <asm/uaccess.h>
#include "util.h"

/* sysctl: */
int msg_ctlmax = MSGMAX;
int msg_ctlmnb = MSGMNB;
int msg_ctlmni = MSGMNI;

/* one msg_receiver structure for each sleeping receiver */
struct msg_receiver {
	struct list_head r_list;
	struct task_struct* r_tsk;

	int r_mode;
	long r_msgtype;
	long r_maxsize;

	struct msg_msg* volatile r_msg;
};

/* one msg_sender for each sleeping sender */
struct msg_sender {
	struct list_head list;
	struct task_struct* tsk;
};

struct msg_msgseg {
	struct msg_msgseg* next;
	/* the next part of the message follows immediately */
};
/* one msg_msg structure for each message */
struct msg_msg {
	struct list_head m_list; 
	long  m_type;          
	int m_ts;           /* message text size */
	struct msg_msgseg* next;
	/* the actual message follows immediately */
};

#define DATALEN_MSG	(PAGE_SIZE-sizeof(struct msg_msg))
#define DATALEN_SEG	(PAGE_SIZE-sizeof(struct msg_msgseg))

/* one msq_queue structure for each present queue on the system */
/**
 * struct msg_queue - 内核消息队列管理结构体
 * 每一个System V消息队列对应一个msg_queue实例
 */
struct msg_queue {
    // IPC权限信息：key、uid、gid、mode、seq等
    struct kern_ipc_perm q_perm;
    // 最后一次调用msgsnd发送消息的时间
    time_t q_stime;         /* last msgsnd time */
    // 最后一次调用msgrcv接收消息的时间
    time_t q_rtime;         /* last msgrcv time */
    // 队列属性最后修改时间（创建/ctl修改）
    time_t q_ctime;         /* last change time */
    // 队列中当前所有消息占用总字节数
    unsigned long q_cbytes;     /* current number of bytes on queue */
    // 队列里面当前消息的条数
    unsigned long q_qnum;       /* number of messages in queue */
    // 队列允许存放的最大字节上限
    unsigned long q_qbytes;     /* max number of bytes on queue */
    // 最后执行msgsnd的进程pid
    pid_t q_lspid;          /* pid of last msgsnd */
    // 最后执行msgrcv的进程pid
    pid_t q_lrpid;          /* last receive pid */

    // 消息链表：挂msg_msg消息节点
    struct list_head q_messages;
    // 接收等待队列：阻塞在msgrcv等待消息的进程
    struct list_head q_receivers;
    // 发送等待队列：阻塞在msgsnd等待队列腾出空间的进程
    struct list_head q_senders;
};


#define SEARCH_ANY		1
#define SEARCH_EQUAL		2
#define SEARCH_NOTEQUAL		3
#define SEARCH_LESSEQUAL	4

static atomic_t msg_bytes = ATOMIC_INIT(0);
static atomic_t msg_hdrs = ATOMIC_INIT(0);

//消息队列结构
static struct ipc_ids msg_ids;

#define msg_lock(id)	((struct msg_queue*)ipc_lock(&msg_ids,id))
#define msg_unlock(id)	ipc_unlock(&msg_ids,id)
#define msg_rmid(id)	((struct msg_queue*)ipc_rmid(&msg_ids,id))
#define msg_checkid(msq, msgid)	\
	ipc_checkid(&msg_ids,&msq->q_perm,msgid)
#define msg_buildid(id, seq) \
	ipc_buildid(&msg_ids, id, seq)

static void freeque (int id);
static int newque (key_t key, int msgflg);
#ifdef CONFIG_PROC_FS
static int sysvipc_msg_read_proc(char *buffer, char **start, off_t offset, int length, int *eof, void *data);
#endif
/**
 * msg_init - System V消息队列模块初始化函数
 * 标记__init：内核启动阶段调用，初始化完成后该段内存会被释放
 */
void __init msg_init (void)
{
    // 初始化msg_ids结构体：信号量、id数组、序列号等IPC管理结构
    ipc_init_ids(&msg_ids,msg_ctlmni);

#ifdef CONFIG_PROC_FS
    // 如果开启procfs，创建/proc/sysvipc/msg节点，供用户态查看消息队列列表
    create_proc_read_entry("sysvipc/msg", 0, 0, sysvipc_msg_read_proc, NULL);
#endif
}


/**
 * newque - 创建一个新的消息队列对象，并加入IPC管理表
 * @key: IPC键值
 * @msgflg: 创建标志，包含权限位
 * return: 成功返回拼接后的msgid；失败返回负错误码
 */
static int newque (key_t key, int msgflg)
{
    int id;
    struct msg_queue *msq;

    // 分配msg_queue结构体内存
    msq  = (struct msg_queue *) kmalloc (sizeof (*msq), GFP_KERNEL);
    if (!msq) 
        return -ENOMEM;
    // 向msg_ids表中新增IPC条目，分配内核id，初始化kern_ipc_perm中的seq
    id = ipc_addid(&msg_ids, &msq->q_perm, msg_ctlmni);
    if(id == -1) {
        // id分配失败，释放内存返回资源不足
        kfree(msq);
        return -ENOSPC;
    }
    // 设置IPC权限mode，从msgflg里提取权限位
    msq->q_perm.mode = (msgflg & S_IRWXUGO);
    // 设置该消息队列对应的IPC key
    msq->q_perm.key = key;

    // 最后一次发送消息时间置0
    msq->q_stime = msq->q_rtime = 0;
    // 设置创建时间为当前内核时间
    msq->q_ctime = CURRENT_TIME;
    // 当前队列字节数、消息数量初始化为0
    msq->q_cbytes = msq->q_qnum = 0;
    // 队列允许的最大总字节数
    msq->q_qbytes = msg_ctlmnb;
    // 最后发送、接收消息的pid初始化为0
    msq->q_lspid = msq->q_lrpid = 0;
    // 消息链表头初始化，保存队列里所有msg消息
    INIT_LIST_HEAD(&msq->q_messages);
    // 等待接收消息的进程链表
    INIT_LIST_HEAD(&msq->q_receivers);
    // 等待发送消息的进程链表
    INIT_LIST_HEAD(&msq->q_senders);
    // 释放该消息队列对应的锁
    msg_unlock(id);

    // 内核id + 序列号拼接，生成返回给用户态的msgid
    return msg_buildid(id,msq->q_perm.seq);
}

static void free_msg(struct msg_msg* msg)
{
	struct msg_msgseg* seg;
	seg = msg->next;
	kfree(msg);
	while(seg != NULL) {
		struct msg_msgseg* tmp = seg->next;
		kfree(seg);
		seg = tmp;
	}
}

static struct msg_msg* load_msg(void* src, int len)
{
	struct msg_msg* msg;
	struct msg_msgseg** pseg;
	int err;
	int alen;

	alen = len;
	if(alen > DATALEN_MSG)
		alen = DATALEN_MSG;

	msg = (struct msg_msg *) kmalloc (sizeof(*msg) + alen, GFP_KERNEL);
	if(msg==NULL)
		return ERR_PTR(-ENOMEM);

	msg->next = NULL;

	if (copy_from_user(msg+1, src, alen)) {
		err = -EFAULT;
		goto out_err;
	}

	len -= alen;
	src = ((char*)src)+alen;
	pseg = &msg->next;
	while(len > 0) {
		struct msg_msgseg* seg;
		alen = len;
		if(alen > DATALEN_SEG)
			alen = DATALEN_SEG;
		seg = (struct msg_msgseg *) kmalloc (sizeof(*seg) + alen, GFP_KERNEL);
		if(seg==NULL) {
			err=-ENOMEM;
			goto out_err;
		}
		*pseg = seg;
		seg->next = NULL;
		if(copy_from_user (seg+1, src, alen)) {
			err = -EFAULT;
			goto out_err;
		}
		pseg = &seg->next;
		len -= alen;
		src = ((char*)src)+alen;
	}
	return msg;

out_err:
	free_msg(msg);
	return ERR_PTR(err);
}

static int store_msg(void* dest, struct msg_msg* msg, int len)
{
	int alen;
	struct msg_msgseg *seg;

	alen = len;
	if(alen > DATALEN_MSG)
		alen = DATALEN_MSG;
	if(copy_to_user (dest, msg+1, alen))
		return -1;

	len -= alen;
	dest = ((char*)dest)+alen;
	seg = msg->next;
	while(len > 0) {
		alen = len;
		if(alen > DATALEN_SEG)
			alen = DATALEN_SEG;
		if(copy_to_user (dest, seg+1, alen))
			return -1;
		len -= alen;
		dest = ((char*)dest)+alen;
		seg=seg->next;
	}
	return 0;
}

static inline void ss_add(struct msg_queue* msq, struct msg_sender* mss)
{
	mss->tsk=current;
	current->state=TASK_INTERRUPTIBLE;
	list_add_tail(&mss->list,&msq->q_senders);
}

static inline void ss_del(struct msg_sender* mss)
{
	if(mss->list.next != NULL)
		list_del(&mss->list);
}

static void ss_wakeup(struct list_head* h, int kill)
{
	struct list_head *tmp;

	tmp = h->next;
	while (tmp != h) {
		struct msg_sender* mss;
		
		mss = list_entry(tmp,struct msg_sender,list);
		tmp = tmp->next;
		if(kill)
			mss->list.next=NULL;
		wake_up_process(mss->tsk);
	}
}

static void expunge_all(struct msg_queue* msq, int res)
{
	struct list_head *tmp;

	tmp = msq->q_receivers.next;
	while (tmp != &msq->q_receivers) {
		struct msg_receiver* msr;
		
		msr = list_entry(tmp,struct msg_receiver,r_list);
		tmp = tmp->next;
		msr->r_msg = ERR_PTR(res);
		wake_up_process(msr->r_tsk);
	}
}

static void freeque (int id)
{
	struct msg_queue *msq;
	struct list_head *tmp;

	msq = msg_rmid(id);

	expunge_all(msq,-EIDRM);
	ss_wakeup(&msq->q_senders,1);
	msg_unlock(id);
		
	tmp = msq->q_messages.next;
	while(tmp != &msq->q_messages) {
		struct msg_msg* msg = list_entry(tmp,struct msg_msg,m_list);
		tmp = tmp->next;
		atomic_dec(&msg_hdrs);
		free_msg(msg);
	}
	atomic_sub(msq->q_cbytes, &msg_bytes);
	kfree(msq);
}

/**
 * sys_msgget - 获取或者创建一个System V消息队列
 * @key: IPC键值，IPC_PRIVATE代表私有消息队列
 * @msgflg: 标志位，包含IPC_CREAT、IPC_EXCL以及权限掩码
 * return: 成功返回消息队列标识符msgid；失败返回负的错误码
 */
asmlinkage long sys_msgget (key_t key, int msgflg)
{
    int id, ret = -EPERM;
    struct msg_queue *msq;
    
    // 申请全局IPC信号量，保护消息队列哈希表，防止并发竞争
    down(&msg_ids.sem);
    if (key == IPC_PRIVATE)
        // IPC_PRIVATE私有队列，无需查找，直接新建消息队列
        ret = newque(key, msgflg);
    else if ((id = ipc_findkey(&msg_ids, key)) == -1) { /* key not used */
        if (!(msgflg & IPC_CREAT))
            // 没有找到对应key，且未指定IPC_CREAT，返回不存在
            ret = -ENOENT;
        else
            // 没有找到对应key，带IPC_CREAT标记，新建消息队列
            ret = newque(key, msgflg);
    } else if (msgflg & IPC_CREAT && msgflg & IPC_EXCL) {
        // key对应的队列已经存在，CREAT+EXCL排他模式，返回已存在
        ret = -EEXIST;
    } else {
        // 根据查到的内核id，锁定这个消息队列对象
        msq = msg_lock(id);
        if(msq==NULL)
            BUG();
        if (ipcperms(&msq->q_perm, msgflg))
            // IPC权限校验失败，返回无访问权限
            ret = -EACCES;
        else
            // 拼接对外给用户的msgid（内核id + 序列号，防止id复用竞态）
            ret = msg_buildid(id, msq->q_perm.seq);
        // 释放当前消息队列对象锁
        msg_unlock(id);
    }
    // 释放全局IPC信号量
    up(&msg_ids.sem);
    return ret;
}

static inline unsigned long copy_msqid_to_user(void *buf, struct msqid64_ds *in, int version)
{
	switch(version) {
	case IPC_64:
		return copy_to_user (buf, in, sizeof(*in));
	case IPC_OLD:
	    {
		struct msqid_ds out;

		memset(&out,0,sizeof(out));

		ipc64_perm_to_ipc_perm(&in->msg_perm, &out.msg_perm);

		out.msg_stime		= in->msg_stime;
		out.msg_rtime		= in->msg_rtime;
		out.msg_ctime		= in->msg_ctime;

		if(in->msg_cbytes > USHRT_MAX)
			out.msg_cbytes	= USHRT_MAX;
		else
			out.msg_cbytes	= in->msg_cbytes;
		out.msg_lcbytes		= in->msg_cbytes;

		if(in->msg_qnum > USHRT_MAX)
			out.msg_qnum	= USHRT_MAX;
		else
			out.msg_qnum	= in->msg_qnum;

		if(in->msg_qbytes > USHRT_MAX)
			out.msg_qbytes	= USHRT_MAX;
		else
			out.msg_qbytes	= in->msg_qbytes;
		out.msg_lqbytes		= in->msg_qbytes;

		out.msg_lspid		= in->msg_lspid;
		out.msg_lrpid		= in->msg_lrpid;

		return copy_to_user (buf, &out, sizeof(out));
	    }
	default:
		return -EINVAL;
	}
}

struct msq_setbuf {
	unsigned long	qbytes;
	uid_t		uid;
	gid_t		gid;
	mode_t		mode;
};

static inline unsigned long copy_msqid_from_user(struct msq_setbuf *out, void *buf, int version)
{
	switch(version) {
	case IPC_64:
	    {
		struct msqid64_ds tbuf;

		if (copy_from_user (&tbuf, buf, sizeof (tbuf)))
			return -EFAULT;

		out->qbytes		= tbuf.msg_qbytes;
		out->uid		= tbuf.msg_perm.uid;
		out->gid		= tbuf.msg_perm.gid;
		out->mode		= tbuf.msg_perm.mode;

		return 0;
	    }
	case IPC_OLD:
	    {
		struct msqid_ds tbuf_old;

		if (copy_from_user (&tbuf_old, buf, sizeof (tbuf_old)))
			return -EFAULT;

		out->uid		= tbuf_old.msg_perm.uid;
		out->gid		= tbuf_old.msg_perm.gid;
		out->mode		= tbuf_old.msg_perm.mode;

		if(tbuf_old.msg_qbytes == 0)
			out->qbytes	= tbuf_old.msg_lqbytes;
		else
			out->qbytes	= tbuf_old.msg_qbytes;

		return 0;
	    }
	default:
		return -EINVAL;
	}
}
/**
 * sys_msgctl - System V消息队列控制系统调用
 * @msqid: 用户态传入的消息队列id
 * @cmd: 控制命令，IPC_STAT / IPC_SET / IPC_RMID / IPC_INFO / MSG_INFO / MSG_STAT
 * @buf: 用户态缓冲区，用于传入或者读出消息队列信息
 * return: 成功返回对应值，失败返回负错误码
 */
asmlinkage long sys_msgctl (int msqid, int cmd, struct msqid_ds *buf)
{
    int err, version;
    struct msg_queue *msq;
    struct msq_setbuf setbuf;
    struct kern_ipc_perm *ipcp;
    
    // 参数合法性校验，msqid和cmd不能为负数
    if (msqid < 0 || cmd < 0)
        return -EINVAL;

    // 解析版本，适配不同版本用户态结构体布局
    version = ipc_parse_version(&cmd);

    switch (cmd) {
    case IPC_INFO: 
    case MSG_INFO: 
    { 
        struct msginfo msginfo;
        int max_id;
        // 用户缓冲区指针为空，返回坏地址错误
        if (!buf)
            return -EFAULT;
        /* We must not return kernel stack data.
         * due to padding, it's not enough
         * to set all member fields.
         */
        // 清空栈上msginfo结构体，避免padding脏数据泄露到用户态
        memset(&msginfo,0,sizeof(msginfo)); 
        // 填充系统全局消息队列限额参数
        msginfo.msgmni = msg_ctlmni;
        msginfo.msgmax = msg_ctlmax;
        msginfo.msgmnb = msg_ctlmnb;
        msginfo.msgssz = MSGSSZ;
        msginfo.msgseg = MSGSEG;
        // 拿全局IPC信号量，保护msg_ids
        down(&msg_ids.sem);
        if (cmd == MSG_INFO) {
            // MSG_INFO：读取实时统计值
            msginfo.msgpool = msg_ids.in_use;
            msginfo.msgmap = atomic_read(&msg_hdrs);
            msginfo.msgtql = atomic_read(&msg_bytes);
        } else {
            // IPC_INFO：返回编译时固定宏参数
            msginfo.msgmap = MSGMAP;
            msginfo.msgpool = MSGPOOL;
            msginfo.msgtql = MSGTQL;
        }
        // 记录当前最大id
        max_id = msg_ids.max_id;
        // 释放全局IPC信号量
        up(&msg_ids.sem);
        // 拷贝信息到用户缓冲区
        if (copy_to_user (buf, &msginfo, sizeof(struct msginfo)))
            return -EFAULT;
        // 返回当前最大id，无队列返回0
        return (max_id < 0) ? 0: max_id;
    }
    case MSG_STAT:
    case IPC_STAT:
    {
        struct msqid64_ds tbuf;
        int success_return;
        // 用户缓冲区为空，返回坏地址
        if (!buf)
            return -EFAULT;
        // MSG_STAT时，msqid不能超过数组容量
        if(cmd == MSG_STAT && msqid > msg_ids.size)
            return -EINVAL;

        // 清空临时内核缓冲
        memset(&tbuf,0,sizeof(tbuf));

        // 根据id锁定消息队列对象
        msq = msg_lock(msqid);
        if (msq == NULL)
            return -EINVAL;

        if(cmd == MSG_STAT) {
            // MSG_STAT：返回组装好的msgid
            success_return = msg_buildid(msqid, msq->q_perm.seq);
        } else {
            err = -EIDRM;
            // IPC_STAT：校验序列号，判断队列是否已经被删除
            if (msg_checkid(msq,msqid))
                goto out_unlock;
            success_return = 0;
        }
        err = -EACCES;
        // 权限校验，检查是否可读
        if (ipcperms (&msq->q_perm, S_IRUGO))
            goto out_unlock;

        // 内核perm结构体转为64位版本的ipc_perm
        kernel_to_ipc64_perm(&msq->q_perm, &tbuf.msg_perm);
        // 拷贝消息队列各项时间、计数、pid信息
        tbuf.msg_stime  = msq->q_stime;
        tbuf.msg_rtime  = msq->q_rtime;
        tbuf.msg_ctime  = msq->q_ctime;
        tbuf.msg_cbytes = msq->q_cbytes;
        tbuf.msg_qnum   = msq->q_qnum;
        tbuf.msg_qbytes = msq->q_qbytes;
        tbuf.msg_lspid  = msq->q_lspid;
        tbuf.msg_lrpid  = msq->q_lrpid;
        // 释放消息队列对象锁
        msg_unlock(msqid);
        // 把内核临时缓冲区数据拷贝到用户buf，适配版本
        if (copy_msqid_to_user(buf, &tbuf, version))
            return -EFAULT;
        return success_return;
    }
    case IPC_SET:
        // 用户缓冲区为空，返回坏地址
        if (!buf)
            return -EFAULT;
        // 从用户态读取要设置的参数到setbuf
        if (copy_msqid_from_user (&setbuf, buf, version))
            return -EFAULT;
        break;
    case IPC_RMID:
        break;
    default:
        return  -EINVAL;
    }

    // IPC_SET / IPC_RMID 走到这里，先拿全局IPC信号量
    down(&msg_ids.sem);
    // 锁定目标消息队列
    msq = msg_lock(msqid);
    err=-EINVAL;
    if (msq == NULL)
        goto out_up;

    err = -EIDRM;
    // 校验id+序列号，防止访问已经删除的旧队列
    if (msg_checkid(msq,msqid))
        goto out_unlock_up;
    ipcp = &msq->q_perm;
    err = -EPERM;
    // 权限检查：创建者/属主/管理员才允许修改或删除
    if (current->euid != ipcp->cuid && 
        current->euid != ipcp->uid && !capable(CAP_SYS_ADMIN))
        /* We _could_ check for CAP_CHOWN above, but we don't */
        goto out_unlock_up;

    switch (cmd) {
    case IPC_SET:
    {
        // 修改后的队列上限超过系统限额，且没有资源管理权限则拒绝
        if (setbuf.qbytes > msg_ctlmnb && !capable(CAP_SYS_RESOURCE))
            goto out_unlock_up;
        // 更新队列最大字节限额
        msq->q_qbytes = setbuf.qbytes;

        // 更新属主uid、gid、权限mode
        ipcp->uid = setbuf.uid;
        ipcp->gid = setbuf.gid;
        ipcp->mode = (ipcp->mode & ~S_IRWXUGO) | 
            (S_IRWXUGO & setbuf.mode);
        // 更新修改时间
        msq->q_ctime = CURRENT_TIME;
        /* sleeping receivers might be excluded by
         * stricter permissions.
         */
        // 权限收紧，唤醒阻塞接收进程，返回EAGAIN
        expunge_all(msq,-EAGAIN);
        /* sleeping senders might be able to send
         * due to a larger queue size.
         */
        // 队列限额变大，唤醒等待发送的进程
        ss_wakeup(&msq->q_senders,0);
        msg_unlock(msqid);
        break;
    }
    case IPC_RMID:
        // 删除消息队列，freeque负责释放资源、唤醒等待进程
        freeque (msqid); 
        break;
    }
    err = 0;
out_up:
    // 释放全局IPC信号量
    up(&msg_ids.sem);
    return err;
out_unlock_up:
    // 先释放队列锁，再跳转到释放全局信号量
    msg_unlock(msqid);
    goto out_up;
out_unlock:
    // 仅释放队列锁，不需要全局信号量（STAT/INFO分支没有拿sem）
    msg_unlock(msqid);
    return err;
}

static int testmsg(struct msg_msg* msg,long type,int mode)
{
	switch(mode)
	{
		case SEARCH_ANY:
			return 1;
		case SEARCH_LESSEQUAL:
			if(msg->m_type <=type)
				return 1;
			break;
		case SEARCH_EQUAL:
			if(msg->m_type == type)
				return 1;
			break;
		case SEARCH_NOTEQUAL:
			if(msg->m_type != type)
				return 1;
			break;
	}
	return 0;
}

/**
 * pipelined_send - 流水线发送优化：直接把消息交给等待的接收进程，不进消息队列
 * @msq: 目标消息队列
 * @msg: 待发送的内核消息节点msg_msg
 * return: 1 找到匹配接收者、消息直接转交成功；0 没有匹配接收者，需要入队
 */
int inline pipelined_send(struct msg_queue* msq, struct msg_msg* msg)
{
    struct list_head* tmp;

    // 遍历接收等待链表 q_receivers
    tmp = msq->q_receivers.next;
    while (tmp != &msq->q_receivers) {
        struct msg_receiver* msr;
        // 从链表节点拿到msg_receiver接收等待对象
        msr = list_entry(tmp,struct msg_receiver,r_list);
        tmp = tmp->next;
        // testmsg 判断：这条消息是否满足该接收进程的接收条件（类型、模式MSG_*）
        if(testmsg(msg,msr->r_msgtype,msr->r_mode)) {
            // 满足条件，把该接收者从等待链表摘除
            list_del(&msr->r_list);
            // 接收缓冲区最大长度不足消息长度
            if(msr->r_maxsize < msg->m_ts) {
                // 给接收者设置错误指针 -E2BIG
                msr->r_msg = ERR_PTR(-E2BIG);
                // 唤醒接收进程
                wake_up_process(msr->r_tsk);
            } else {
                // 直接把消息指针交给接收者，跳过消息入队
                msr->r_msg = msg;
                // 更新队列最后发送pid、最后接收时间
                msq->q_lspid = msr->r_tsk->pid;
                msq->q_rtime = CURRENT_TIME;
                // 唤醒阻塞的接收进程
                wake_up_process(msr->r_tsk);
                // 转交成功，返回1，上层不再执行list_add_tail入队
                return 1;
            }
        }
    }
    // 遍历完所有等待接收进程，没有匹配的，返回0，消息需要加入q_messages队列
    return 0;
}

/**
 * sys_msgsnd - System V消息队列发送消息系统调用
 * @msqid: 消息队列id
 * @msgp: 用户态msgbuf缓冲区地址
 * @msgsz: 消息正文长度
 * @msgflg: 标志位，支持IPC_NOWAIT非阻塞
 * return: 成功返回0；失败返回负错误码
 */
asmlinkage long sys_msgsnd (int msqid, struct msgbuf *msgp, size_t msgsz, int msgflg)
{
    struct msg_queue *msq;
    struct msg_msg *msg;
    long mtype;
    int err;
    
    // 校验消息长度、msqid合法性
    if (msgsz > msg_ctlmax || (long) msgsz < 0 || msqid < 0)
        return -EINVAL;
    // 从用户空间读取消息类型mtype
    if (get_user(mtype, &msgp->mtype))
        return -EFAULT; 
    // 消息类型必须大于等于1
    if (mtype < 1)
        return -EINVAL;

    // 分配内核msg_msg，拷贝用户态消息正文到内核
    msg = load_msg(msgp->mtext, msgsz);
    if(IS_ERR(msg))
        return PTR_ERR(msg);

    // 填充消息类型、消息长度
    msg->m_type = mtype;
    msg->m_ts = msgsz;

    // 根据msqid拿到消息队列对象，持有ary自旋锁
    msq = msg_lock(msqid);
    err=-EINVAL;
    if(msq==NULL)
        goto out_free;
retry:
    err= -EIDRM;
    // 校验序列号，判断队列是否已经被删除
    if (msg_checkid(msq,msqid))
        goto out_unlock_free;

    err=-EACCES;
    // 写权限校验
    if (ipcperms(&msq->q_perm, S_IWUGO)) 
        goto out_unlock_free;

    // 判断队列是否满：总字节超限 / 消息条数超限
    if(msgsz + msq->q_cbytes > msq->q_qbytes ||
        1 + msq->q_qnum > msq->q_qbytes) {
        struct msg_sender s;

        // 设置了IPC_NOWAIT，不阻塞直接返回-EAGAIN
        if(msgflg&IPC_NOWAIT) {
            err=-EAGAIN;
            goto out_unlock_free;
        }
        // 将当前发送进程加入q_senders等待链表
        ss_add(msq, &s);
        // 释放自旋锁，让出CPU
        msg_unlock(msqid);
        schedule();
        current->state= TASK_RUNNING;

        // 唤醒后重新上锁获取队列
        msq = msg_lock(msqid);
        err = -EIDRM;
        if(msq==NULL)
            goto out_free;
        // 将当前进程从发送等待链表移除
        ss_del(&s);
        
        // 唤醒时收到信号，返回-EINTR
        if (signal_pending(current)) {
            err=-EINTR;
            goto out_unlock_free;
        }
        // 重新走检查逻辑，重试发送
        goto retry;
    }

    // 尝试pipelined_send：直接唤醒等待该类型消息的接收者，不进队列
    if(!pipelined_send(msq,msg)) {
        /* noone is waiting for this message, enqueue it */
        // 没有等待的接收进程，把消息追加到消息队列尾部
        list_add_tail(&msg->m_list,&msq->q_messages);
        // 更新队列字节计数、消息数量
        msq->q_cbytes += msgsz;
        msq->q_qnum++;
        // 全局统计计数
        atomic_add(msgsz,&msg_bytes);
        atomic_inc(&msg_hdrs);
    }
    
    err = 0;
    msg = NULL;
    // 更新最后发送消息的pid和发送时间
    msq->q_lspid = current->pid;
    msq->q_stime = CURRENT_TIME;

out_unlock_free:
    // 释放ary自旋锁
    msg_unlock(msqid);
out_free:
    // 如果消息没有成功入队/转交，释放内核消息内存
    if(msg!=NULL)
        free_msg(msg);
    return err;
}

int inline convert_mode(long* msgtyp, int msgflg)
{
	/* 
	 *  find message of correct type.
	 *  msgtyp = 0 => get first.
	 *  msgtyp > 0 => get first message of matching type.
	 *  msgtyp < 0 => get message with least type must be < abs(msgtype).  
	 */
	if(*msgtyp==0)
		return SEARCH_ANY;
	if(*msgtyp<0) {
		*msgtyp=-(*msgtyp);
		return SEARCH_LESSEQUAL;
	}
	if(msgflg & MSG_EXCEPT)
		return SEARCH_NOTEQUAL;
	return SEARCH_EQUAL;
}

/**
 * sys_msgrcv - System V消息队列接收消息系统调用
 * @msqid: 消息队列id
 * @msgp: 用户态msgbuf缓冲区
 * @msgsz: 用户缓冲区mtext最大长度
 * @msgtyp: 需要匹配的消息类型
 * @msgflg: 标志位：IPC_NOWAIT、MSG_NOERROR
 * return: 成功返回拷贝到用户态的消息长度；失败返回负错误码
 */
asmlinkage long sys_msgrcv (int msqid, struct msgbuf *msgp, size_t msgsz,
        long msgtyp, int msgflg)
{
    struct msg_queue *msq;
    struct msg_receiver msr_d;
    struct list_head* tmp;
    struct msg_msg* msg, *found_msg;
    int err;
    int mode;

    // 参数合法性检查
    if (msqid < 0 || (long) msgsz < 0)
        return -EINVAL;
    // 根据msgtyp、msgflg转换成内部检索模式
    mode = convert_mode(&msgtyp,msgflg);

    // 根据msqid拿到消息队列，持有ary自旋锁
    msq = msg_lock(msqid);
    if(msq==NULL)
        return -EINVAL;
retry:
    err=-EACCES;
    // 读权限校验
    if (ipcperms (&msq->q_perm, S_IRUGO))
        goto out_unlock;

    // 遍历消息链表q_messages，寻找匹配的消息
    tmp = msq->q_messages.next;
    found_msg=NULL;
    while (tmp != &msq->q_messages) {
        msg = list_entry(tmp,struct msg_msg,m_list);
        // testmsg 判断消息是否匹配检索条件
        if(testmsg(msg,msgtyp,mode)) {
            found_msg = msg;
            // SEARCH_LESSEQUAL 模式：找类型 <= msgtyp 的消息
            if(mode == SEARCH_LESSEQUAL && msg->m_type != 1) {
                found_msg=msg;
                msgtyp=msg->m_type-1;
            } else {
                found_msg=msg;
                break;
            }
        }
        tmp = tmp->next;
    }
    if(found_msg) {
        msg=found_msg;
        // 消息长度超过用户缓冲区，并且没有MSG_NOERROR标记，返回-E2BIG
        if ((msgsz < msg->m_ts) && !(msgflg & MSG_NOERROR)) {
            err=-E2BIG;
            goto out_unlock;
        }
        // 把匹配消息从消息链表摘除
        list_del(&msg->m_list);
        // 更新队列统计信息：消息条数、接收时间、接收pid、总字节
        msq->q_qnum--;
        msq->q_rtime = CURRENT_TIME;
        msq->q_lrpid = current->pid;
        msq->q_cbytes -= msg->m_ts;
        atomic_sub(msg->m_ts,&msg_bytes);
        atomic_dec(&msg_hdrs);
        // 唤醒等待发送的进程（队列腾出空间了）
        ss_wakeup(&msq->q_senders,0);
        msg_unlock(msqid);
out_success:
        // 实际拷贝长度取消息长度和用户缓冲区的较小值
        msgsz = (msgsz > msg->m_ts) ? msg->m_ts : msgsz;
        // 把mtype和消息正文拷贝回用户空间
        if (put_user (msg->m_type, &msgp->mtype) ||
            store_msg(msgp->mtext, msg, msgsz)) {
              msgsz = -EFAULT;
        }
        // 释放内核消息节点
        free_msg(msg);
        return msgsz;
    } else
    {
        struct msg_queue *t;
        /* no message waiting. Prepare for pipelined
         * receive.
         */
        // 没有匹配消息，并且设置IPC_NOWAIT，直接返回ENOMSG
        if (msgflg & IPC_NOWAIT) {
            err=-ENOMSG;
            goto out_unlock;
        }
        // 当前接收进程加入q_receivers等待链表，用于pipelined_send直接交付
        list_add_tail(&msr_d.r_list,&msq->q_receivers);
        msr_d.r_tsk = current;
        msr_d.r_msgtype = msgtyp;
        msr_d.r_mode = mode;
        // MSG_NOERROR：接收时不截断报错，设置最大长度为无穷大
        if(msgflg & MSG_NOERROR)
            msr_d.r_maxsize = INT_MAX;
         else
            msr_d.r_maxsize = msgsz;
        // 默认标记为-EAGAIN：等待消息
        msr_d.r_msg = ERR_PTR(-EAGAIN);
        // 设置为可中断睡眠
        current->state = TASK_INTERRUPTIBLE;
        // 释放自旋锁，准备调度睡眠
        msg_unlock(msqid);

        schedule();
        current->state = TASK_RUNNING;

        // 被唤醒后，先看是否pipelined_send直接交付了消息
        msg = (struct msg_msg*) msr_d.r_msg;
        if(!IS_ERR(msg)) 
            goto out_success;

        // 拿到锁再次检查，防止唤醒和拿锁之间消息到达
        t = msg_lock(msqid);
        if(t==NULL)
            msqid=-1;
        msg = (struct msg_msg*)msr_d.r_msg;
        if(!IS_ERR(msg)) {
            /* our message arived while we waited for
             * the spinlock. Process it.
             */
            if(msqid!=-1)
                msg_unlock(msqid);
            goto out_success;
        }
        err = PTR_ERR(msg);
        if(err == -EAGAIN) {
            if(msqid==-1)
                BUG();
            // 从接收等待链表删除自己
            list_del(&msr_d.r_list);
            // 如果是信号唤醒，返回EINTR；否则重试一轮查找
            if (signal_pending(current))
                err=-EINTR;
             else
                goto retry;
        }
    }
out_unlock:
    if(msqid!=-1)
        msg_unlock(msqid);
    return err;
}

#ifdef CONFIG_PROC_FS
static int sysvipc_msg_read_proc(char *buffer, char **start, off_t offset, int length, int *eof, void *data)
{
	off_t pos = 0;
	off_t begin = 0;
	int i, len = 0;

	down(&msg_ids.sem);
	len += sprintf(buffer, "       key      msqid perms      cbytes       qnum lspid lrpid   uid   gid  cuid  cgid      stime      rtime      ctime\n");

	for(i = 0; i <= msg_ids.max_id; i++) {
		struct msg_queue * msq;
		msq = msg_lock(i);
		if(msq != NULL) {
			len += sprintf(buffer + len, "%10d %10d  %4o  %10lu %10lu %5u %5u %5u %5u %5u %5u %10lu %10lu %10lu\n",
				msq->q_perm.key,
				msg_buildid(i,msq->q_perm.seq),
				msq->q_perm.mode,
				msq->q_cbytes,
				msq->q_qnum,
				msq->q_lspid,
				msq->q_lrpid,
				msq->q_perm.uid,
				msq->q_perm.gid,
				msq->q_perm.cuid,
				msq->q_perm.cgid,
				msq->q_stime,
				msq->q_rtime,
				msq->q_ctime);
			msg_unlock(i);

			pos += len;
			if(pos < offset) {
				len = 0;
				begin = pos;
			}
			if(pos > offset + length)
				goto done;
		}

	}
	*eof = 1;
done:
	up(&msg_ids.sem);
	*start = buffer + (offset - begin);
	len -= (offset - begin);
	if(len > length)
		len = length;
	if(len < 0)
		len = 0;
	return len;
}
#endif
