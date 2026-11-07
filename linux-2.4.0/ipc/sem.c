/*
 * linux/ipc/sem.c
 * Copyright (C) 1992 Krishna Balasubramanian
 * Copyright (C) 1995 Eric Schenk, Bruno Haible
 *
 * IMPLEMENTATION NOTES ON CODE REWRITE (Eric Schenk, January 1995):
 * This code underwent a massive rewrite in order to solve some problems
 * with the original code. In particular the original code failed to
 * wake up processes that were waiting for semval to go to 0 if the
 * value went to 0 and was then incremented rapidly enough. In solving
 * this problem I have also modified the implementation so that it
 * processes pending operations in a FIFO manner, thus give a guarantee
 * that processes waiting for a lock on the semaphore won't starve
 * unless another locking process fails to unlock.
 * In addition the following two changes in behavior have been introduced:
 * - The original implementation of semop returned the value
 *   last semaphore element examined on success. This does not
 *   match the manual page specifications, and effectively
 *   allows the user to read the semaphore even if they do not
 *   have read permissions. The implementation now returns 0
 *   on success as stated in the manual page.
 * - There is some confusion over whether the set of undo adjustments
 *   to be performed at exit should be done in an atomic manner.
 *   That is, if we are attempting to decrement the semval should we queue
 *   up and wait until we can do so legally?
 *   The original implementation attempted to do this.
 *   The current implementation does not do so. This is because I don't
 *   think it is the right thing (TM) to do, and because I couldn't
 *   see a clean way to get the old behavior with the new design.
 *   The POSIX standard and SVID should be consulted to determine
 *   what behavior is mandated.
 *
 * Further notes on refinement (Christoph Rohland, December 1998):
 * - The POSIX standard says, that the undo adjustments simply should
 *   redo. So the current implementation is o.K.
 * - The previous code had two flaws:
 *   1) It actively gave the semaphore to the next waiting process
 *      sleeping on the semaphore. Since this process did not have the
 *      cpu this led to many unnecessary context switches and bad
 *      performance. Now we only check which process should be able to
 *      get the semaphore and if this process wants to reduce some
 *      semaphore value we simply wake it up without doing the
 *      operation. So it has to try to get it later. Thus e.g. the
 *      running process may reacquire the semaphore during the current
 *      time slice. If it only waits for zero or increases the semaphore,
 *      we do the operation in advance and wake it up.
 *   2) It did not wake up all zero waiting processes. We try to do
 *      better but only get the semops right which only wait for zero or
 *      increase. If there are decrement operations in the operations
 *      array we do the same as before.
 *
 * /proc/sysvipc/sem support (c) 1999 Dragos Acostachioaie <dragos@iname.com>
 *
 * SMP-threaded, sysctl's added
 * (c) 1999 Manfred Spraul <manfreds@colorfullife.com>
 */

#include <linux/config.h>
#include <linux/malloc.h>
#include <linux/spinlock.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <asm/uaccess.h>
#include "util.h"


#define sem_lock(id)	((struct sem_array*)ipc_lock(&sem_ids,id))
#define sem_unlock(id)	ipc_unlock(&sem_ids,id)
#define sem_rmid(id)	((struct sem_array*)ipc_rmid(&sem_ids,id))
#define sem_checkid(sma, semid)	\
	ipc_checkid(&sem_ids,&sma->sem_perm,semid)
#define sem_buildid(id, seq) \
	ipc_buildid(&sem_ids, id, seq)

//信号量的管理结构
static struct ipc_ids sem_ids;

static int newary (key_t, int, int);
static void freeary (int id);
#ifdef CONFIG_PROC_FS
static int sysvipc_sem_read_proc(char *buffer, char **start, off_t offset, int length, int *eof, void *data);
#endif

#define SEMMSL_FAST	256 /* 512 bytes on stack */
#define SEMOPM_FAST	64  /* ~ 372 bytes on stack */

/*
 * linked list protection:
 *	sem_undo.id_next,
 *	sem_array.sem_pending{,last},
 *	sem_array.sem_undo: sem_lock() for read/write
 *	sem_undo.proc_next: only "current" is allowed to read/write that field.
 *	
 */

int sem_ctls[4] = {SEMMSL, SEMMNS, SEMOPM, SEMMNI};
#define sc_semmsl	(sem_ctls[0])
#define sc_semmns	(sem_ctls[1])
#define sc_semopm	(sem_ctls[2])
#define sc_semmni	(sem_ctls[3])

static int used_sems;

void __init sem_init (void)
{
	used_sems = 0;
	ipc_init_ids(&sem_ids,sc_semmni);

#ifdef CONFIG_PROC_FS
	create_proc_read_entry("sysvipc/sem", 0, 0, sysvipc_sem_read_proc, NULL);
#endif
}

/**
 * newary - 创建一个新的System V信号量集合，sys_semget创建分支调用
 * @key: IPC key
 * @nsems: 集合内信号量个数
 * @semflg: IPC标志+权限位
 * return: 成功返回组装后的semid；失败返回负错误码
 * 
 struct sem_array
+----------------------------+
| sem_perm、sem_nsems、时间等 |
+----------------------------+
| struct sem[0]              |
| struct sem[1]              |
| ...                        |
| struct sem[nsems-1]        |
+----------------------------+

 */
static int newary (key_t key, int nsems, int semflg)
{
    int id;
    struct sem_array *sma;
    int size;

    // 信号量数量不能为0
    if (!nsems)
        return -EINVAL;
    // 全局信号量总数上限校验：used_sems 是当前系统所有sem累加计数
    if (used_sems + nsems > sc_semmns)
        return -ENOSPC;

    // 内存大小 = sem_array管理头 + nsems个struct sem
    size = sizeof (*sma) + nsems * sizeof (struct sem);
    // ipc_alloc：IPC专用内存分配，和kmalloc类似
    sma = (struct sem_array *) ipc_alloc(size);
    if (!sma) {
        return -ENOMEM;
    }
    // 整块清零
    memset (sma, 0, size);
    // 分配sem_ids数组下标id，初始化sma->sem_perm（生成seq序列号）
    id = ipc_addid(&sem_ids, &sma->sem_perm, sc_semmni);
    if(id == -1) {
        ipc_free(sma, size);
        return -ENOSPC;
    }
    // 全局信号量计数累加
    used_sems += nsems;

    // 设置权限mode、IPC key
    sma->sem_perm.mode = (semflg & S_IRWXUGO);
    sma->sem_perm.key = key;

    // sem_base：信号量数组起始地址，紧跟在sem_array结构体后面
    sma->sem_base = (struct sem *) &sma[1];
    /* sma->sem_pending = NULL; */
    sma->sem_pending_last = &sma->sem_pending;
    /* sma->undo = NULL; */
    // 这个信号量集里面有多少个信号量
    sma->sem_nsems = nsems;
    // 创建时间
    sma->sem_ctime = CURRENT_TIME;
    // ipc_addid内部会上锁，这里释放细粒度自旋锁
    sem_unlock(id);

    // 合成对外给用户的semid：下标id + seq序列号
    return sem_buildid(id, sma->sem_perm.seq);
}

/**
 * sys_semget - System V信号量集创建/查找系统调用
 * @key: IPC键，IPC_PRIVATE代表私有信号量集
 * @nsems: 信号量集中包含信号量的个数
 * @semflg: IPC_CREAT / IPC_EXCL / 权限位
 * return: 成功返回semid；失败返回负错误码
 */
asmlinkage long sys_semget (key_t key, int nsems, int semflg)
{
    int id, err = -EINVAL;
    struct sem_array *sma;

    // 校验信号量数量：不能小于0，不能超过系统上限sc_semmsl
    if (nsems < 0 || nsems > sc_semmsl)
        return -EINVAL;
    // 全局信号量IPC粗粒度锁，保证查找+创建原子化
    down(&sem_ids.sem);
    
    if (key == IPC_PRIVATE) {
        // IPC_PRIVATE：直接新建，不做key查找
        err = newary(key, nsems, semflg);
    } else if ((id = ipc_findkey(&sem_ids, key)) == -1) {  /* key not used */
        // 根据key查找，没有找到
        if (!(semflg & IPC_CREAT))
            // 不要求创建，直接返回不存在
            err = -ENOENT;
        else
            // 创建新的信号量数组
            err = newary(key, nsems, semflg);
    } else if (semflg & IPC_CREAT && semflg & IPC_EXCL) {
        // IPC_CREAT | IPC_EXCL：要求新建，但key已经存在，返回-EEXIST
        err = -EEXIST;
    } else {
        // key找到已有信号量集，复用
        sma = sem_lock(id);
        if(sma==NULL)
            BUG();
        // 已有信号量集的信号量个数不能小于用户传入nsems
        if (nsems > sma->sem_nsems)
            err = -EINVAL;
        else if (ipcperms(&sma->sem_perm, semflg))
            // IPC权限校验失败
            err = -EACCES;
        else
            // 组装对外的semid（下标 + seq序列号）
            err = sem_buildid(id, sma->sem_perm.seq);
        sem_unlock(id);
    }

    up(&sem_ids.sem);
    return err;
}

/* doesn't acquire the sem_lock on error! */
static int sem_revalidate(int semid, struct sem_array* sma, int nsems, short flg)
{
	struct sem_array* smanew;

	smanew = sem_lock(semid);
	if(smanew==NULL)
		return -EIDRM;
	if(smanew != sma || sem_checkid(sma,semid) || sma->sem_nsems != nsems) {
		sem_unlock(semid);
		return -EIDRM;
	}

	if (ipcperms(&sma->sem_perm, flg)) {
		sem_unlock(semid);
		return -EACCES;
	}
	return 0;
}
/* Manage the doubly linked list sma->sem_pending as a FIFO:
 * insert new queue elements at the tail sma->sem_pending_last.
 */
static inline void append_to_queue (struct sem_array * sma,
				    struct sem_queue * q)
{
	*(q->prev = sma->sem_pending_last) = q;
	*(sma->sem_pending_last = &q->next) = NULL;
}

static inline void prepend_to_queue (struct sem_array * sma,
				     struct sem_queue * q)
{
	q->next = sma->sem_pending;
	*(q->prev = &sma->sem_pending) = q;
	if (q->next)
		q->next->prev = &q->next;
	else /* sma->sem_pending_last == &sma->sem_pending */
		sma->sem_pending_last = &q->next;
}

static inline void remove_from_queue (struct sem_array * sma,
				      struct sem_queue * q)
{
	*(q->prev) = q->next;
	if (q->next)
		q->next->prev = q->prev;
	else /* sma->sem_pending_last == &q->next */
		sma->sem_pending_last = q->prev;
	q->prev = NULL; /* mark as removed */
}

/*
 * Determine whether a sequence of semaphore operations would succeed
 * all at once. Return 0 if yes, 1 if need to sleep, else return error code.
 */

/**
 * try_atomic_semop - 尝试原子执行一组System V信号量操作
 * @sma: 信号量集合描述符
 * @sops: 待执行的sembuf操作数组
 * @nsops: 操作的数量
 * @un: sem_undo撤销结构，用于记录SEM_UNDO补偿值
 * @pid: 当前进程pid
 * @do_undo: 是否需要回滚（本次只是预检查，先不真正提交）
 * 返回值：0 全部操作成功；1 需要阻塞；负数为错误码
 * 功能：按原子语义尝试批量执行信号量操作，只要任意一条操作不满足条件，全部回滚
 */
static int try_atomic_semop (struct sem_array * sma, struct sembuf * sops,
                     int nsops, struct sem_undo *un, int pid,
                     int do_undo)
{
    int result, sem_op;
    struct sembuf *sop;
    struct sem * curr;

    // 遍历每一条信号量操作
    for (sop = sops; sop < sops + nsops; sop++) {
        // 定位当前操作对应的信号量对象
        curr = sma->sem_base + sop->sem_num;
        sem_op = sop->sem_op;

        // sem_op等于0，代表等待信号量值为0；当前semval不为0，条件不满足
        if (!sem_op && curr->semval)
            goto would_block;

        // 更新最后操作该信号量的pid信息
        curr->sempid = (curr->sempid << 16) | pid;
        // 先应用本次操作，修改semval
        curr->semval += sem_op;
        // 如果带SEM_UNDO标记，更新undo补偿值（退出时反向恢复）
        if (sop->sem_flg & SEM_UNDO)
            un->semadj[sop->sem_num] -= sem_op;

        // 信号量值小于0，资源不足，需要阻塞
        if (curr->semval < 0)
            goto would_block;
        // 信号量值超过上限SEMVMX，越界
        if (curr->semval > SEMVMX)
            goto out_of_range;
    }

    // do_undo标记置位：代表本次只是试探，成功后也要回滚，不持久生效
    if (do_undo)
    {
        sop--;
        result = 0;
        goto undo;
    }

    // 全部操作成功，更新信号量集合最后操作时间
    sma->sem_otime = CURRENT_TIME;
    return 0;

out_of_range:
    // 信号量数值超出合法范围，错误ERANGE
    result = -ERANGE;
    goto undo;

would_block:
    // 条件不满足，判断是否非阻塞
    if (sop->sem_flg & IPC_NOWAIT)
        result = -EAGAIN;
    else
        result = 1;

undo:
    // 回滚分支：反向撤销已经执行过的操作，保证整组操作原子性
    while (sop >= sops) {
        curr = sma->sem_base + sop->sem_num;
        // 恢复semval原值
        curr->semval -= sop->sem_op;
        // 恢复sempid
        curr->sempid >>= 16;

        // 同步撤销undo结构里面的补偿记录
        if (sop->sem_flg & SEM_UNDO)
            un->semadj[sop->sem_num] += sop->sem_op;
        sop--;
    }

    return result;
}

/* Go through the pending queue for the indicated semaphore
 * looking for tasks that can be completed.
 */
static void update_queue (struct sem_array * sma)
{
	int error;
	struct sem_queue * q;

	for (q = sma->sem_pending; q; q = q->next) {
			
		if (q->status == 1)
			continue;	/* this one was woken up before */

		error = try_atomic_semop(sma, q->sops, q->nsops,
					 q->undo, q->pid, q->alter);

		/* Does q->sleeper still need to sleep? */
		if (error <= 0) {
				/* Found one, wake it up */
			wake_up_process(q->sleeper);
			if (error == 0 && q->alter) {
				/* if q-> alter let it self try */
				q->status = 1;
				return;
			}
			q->status = error;
			remove_from_queue(sma,q);
		}
	}
}

/* The following counts are associated to each semaphore:
 *   semncnt        number of tasks waiting on semval being nonzero
 *   semzcnt        number of tasks waiting on semval being zero
 * This model assumes that a task waits on exactly one semaphore.
 * Since semaphore operations are to be performed atomically, tasks actually
 * wait on a whole sequence of semaphores simultaneously.
 * The counts we return here are a rough approximation, but still
 * warrant that semncnt+semzcnt>0 if the task is on the pending queue.
 */
static int count_semncnt (struct sem_array * sma, ushort semnum)
{
	int semncnt;
	struct sem_queue * q;

	semncnt = 0;
	for (q = sma->sem_pending; q; q = q->next) {
		struct sembuf * sops = q->sops;
		int nsops = q->nsops;
		int i;
		for (i = 0; i < nsops; i++)
			if (sops[i].sem_num == semnum
			    && (sops[i].sem_op < 0)
			    && !(sops[i].sem_flg & IPC_NOWAIT))
				semncnt++;
	}
	return semncnt;
}
static int count_semzcnt (struct sem_array * sma, ushort semnum)
{
	int semzcnt;
	struct sem_queue * q;

	semzcnt = 0;
	for (q = sma->sem_pending; q; q = q->next) {
		struct sembuf * sops = q->sops;
		int nsops = q->nsops;
		int i;
		for (i = 0; i < nsops; i++)
			if (sops[i].sem_num == semnum
			    && (sops[i].sem_op == 0)
			    && !(sops[i].sem_flg & IPC_NOWAIT))
				semzcnt++;
	}
	return semzcnt;
}

/* Free a semaphore set. */
static void freeary (int id)
{
	struct sem_array *sma;
	struct sem_undo *un;
	struct sem_queue *q;
	int size;

	sma = sem_rmid(id);

	/* Invalidate the existing undo structures for this semaphore set.
	 * (They will be freed without any further action in sem_exit()
	 * or during the next semop.)
	 */
	for (un = sma->undo; un; un = un->id_next)
		un->semid = -1;

	/* Wake up all pending processes and let them fail with EIDRM. */
	for (q = sma->sem_pending; q; q = q->next) {
		q->status = -EIDRM;
		q->prev = NULL;
		wake_up_process(q->sleeper); /* doesn't sleep */
	}
	sem_unlock(id);

	used_sems -= sma->sem_nsems;
	size = sizeof (*sma) + sma->sem_nsems * sizeof (struct sem);
	ipc_free(sma, size);
}

static unsigned long copy_semid_to_user(void *buf, struct semid64_ds *in, int version)
{
	switch(version) {
	case IPC_64:
		return copy_to_user(buf, in, sizeof(*in));
	case IPC_OLD:
	    {
		struct semid_ds out;

		ipc64_perm_to_ipc_perm(&in->sem_perm, &out.sem_perm);

		out.sem_otime	= in->sem_otime;
		out.sem_ctime	= in->sem_ctime;
		out.sem_nsems	= in->sem_nsems;

		return copy_to_user(buf, &out, sizeof(out));
	    }
	default:
		return -EINVAL;
	}
}

int semctl_nolock(int semid, int semnum, int cmd, int version, union semun arg)
{
	int err = -EINVAL;

	switch(cmd) {
	case IPC_INFO:
	case SEM_INFO:
	{
		struct seminfo seminfo;
		int max_id;

		memset(&seminfo,0,sizeof(seminfo));
		seminfo.semmni = sc_semmni;
		seminfo.semmns = sc_semmns;
		seminfo.semmsl = sc_semmsl;
		seminfo.semopm = sc_semopm;
		seminfo.semvmx = SEMVMX;
		seminfo.semmnu = SEMMNU;
		seminfo.semmap = SEMMAP;
		seminfo.semume = SEMUME;
		down(&sem_ids.sem);
		if (cmd == SEM_INFO) {
			seminfo.semusz = sem_ids.in_use;
			seminfo.semaem = used_sems;
		} else {
			seminfo.semusz = SEMUSZ;
			seminfo.semaem = SEMAEM;
		}
		max_id = sem_ids.max_id;
		up(&sem_ids.sem);
		if (copy_to_user (arg.__buf, &seminfo, sizeof(struct seminfo))) 
			return -EFAULT;
		return (max_id < 0) ? 0: max_id;
	}
	case SEM_STAT:
	{
		struct sem_array *sma;
		struct semid64_ds tbuf;
		int id;

		if(semid > sem_ids.size)
			return -EINVAL;

		memset(&tbuf,0,sizeof(tbuf));

		sma = sem_lock(semid);
		if(sma == NULL)
			return -EINVAL;

		err = -EACCES;
		if (ipcperms (&sma->sem_perm, S_IRUGO))
			goto out_unlock;
		id = sem_buildid(semid, sma->sem_perm.seq);

		kernel_to_ipc64_perm(&sma->sem_perm, &tbuf.sem_perm);
		tbuf.sem_otime  = sma->sem_otime;
		tbuf.sem_ctime  = sma->sem_ctime;
		tbuf.sem_nsems  = sma->sem_nsems;
		sem_unlock(semid);
		if (copy_semid_to_user (arg.buf, &tbuf, version))
			return -EFAULT;
		return id;
	}
	default:
		return -EINVAL;
	}
	return err;
out_unlock:
	sem_unlock(semid);
	return err;
}

int semctl_main(int semid, int semnum, int cmd, int version, union semun arg)
{
	struct sem_array *sma;
	struct sem* curr;
	int err;
	ushort fast_sem_io[SEMMSL_FAST];
	ushort* sem_io = fast_sem_io;
	int nsems;

	sma = sem_lock(semid);
	if(sma==NULL)
		return -EINVAL;

	nsems = sma->sem_nsems;

	err=-EIDRM;
	if (sem_checkid(sma,semid))
		goto out_unlock;

	err = -EACCES;
	if (ipcperms (&sma->sem_perm, (cmd==SETVAL||cmd==SETALL)?S_IWUGO:S_IRUGO))
		goto out_unlock;

	switch (cmd) {
	case GETALL:
	{
		ushort *array = arg.array;
		int i;

		if(nsems > SEMMSL_FAST) {
			sem_unlock(semid);			
			sem_io = ipc_alloc(sizeof(ushort)*nsems);
			if(sem_io == NULL)
				return -ENOMEM;
			err = sem_revalidate(semid, sma, nsems, S_IRUGO);
			if(err)
				goto out_free;
		}

		for (i = 0; i < sma->sem_nsems; i++)
			sem_io[i] = sma->sem_base[i].semval;
		sem_unlock(semid);
		err = 0;
		if(copy_to_user(array, sem_io, nsems*sizeof(ushort)))
			err = -EFAULT;
		goto out_free;
	}
	case SETALL:
	{
		int i;
		struct sem_undo *un;

		sem_unlock(semid);

		if(nsems > SEMMSL_FAST) {
			sem_io = ipc_alloc(sizeof(ushort)*nsems);
			if(sem_io == NULL)
				return -ENOMEM;
		}

		if (copy_from_user (sem_io, arg.array, nsems*sizeof(ushort))) {
			err = -EFAULT;
			goto out_free;
		}

		for (i = 0; i < nsems; i++) {
			if (sem_io[i] > SEMVMX) {
				err = -ERANGE;
				goto out_free;
			}
		}
		err = sem_revalidate(semid, sma, nsems, S_IWUGO);
		if(err)
			goto out_free;

		for (i = 0; i < nsems; i++)
			sma->sem_base[i].semval = sem_io[i];
		for (un = sma->undo; un; un = un->id_next)
			for (i = 0; i < nsems; i++)
				un->semadj[i] = 0;
		sma->sem_ctime = CURRENT_TIME;
		/* maybe some queued-up processes were waiting for this */
		update_queue(sma);
		err = 0;
		goto out_unlock;
	}
	case IPC_STAT:
	{
		struct semid64_ds tbuf;
		memset(&tbuf,0,sizeof(tbuf));
		kernel_to_ipc64_perm(&sma->sem_perm, &tbuf.sem_perm);
		tbuf.sem_otime  = sma->sem_otime;
		tbuf.sem_ctime  = sma->sem_ctime;
		tbuf.sem_nsems  = sma->sem_nsems;
		sem_unlock(semid);
		if (copy_semid_to_user (arg.buf, &tbuf, version))
			return -EFAULT;
		return 0;
	}
	/* GETVAL, GETPID, GETNCTN, GETZCNT, SETVAL: fall-through */
	}
	err = -EINVAL;
	if(semnum < 0 || semnum >= nsems)
		goto out_unlock;

	curr = &sma->sem_base[semnum];

	switch (cmd) {
	case GETVAL:
		err = curr->semval;
		goto out_unlock;
	case GETPID:
		err = curr->sempid & 0xffff;
		goto out_unlock;
	case GETNCNT:
		err = count_semncnt(sma,semnum);
		goto out_unlock;
	case GETZCNT:
		err = count_semzcnt(sma,semnum);
		goto out_unlock;
	case SETVAL:
	{
		int val = arg.val;
		struct sem_undo *un;
		err = -ERANGE;
		if (val > SEMVMX || val < 0)
			goto out_unlock;

		for (un = sma->undo; un; un = un->id_next)
			un->semadj[semnum] = 0;
		curr->semval = val;
		sma->sem_ctime = CURRENT_TIME;
		/* maybe some queued-up processes were waiting for this */
		update_queue(sma);
		err = 0;
		goto out_unlock;
	}
	}
out_unlock:
	sem_unlock(semid);
out_free:
	if(sem_io != fast_sem_io)
		ipc_free(sem_io, sizeof(ushort)*nsems);
	return err;
}

struct sem_setbuf {
	uid_t	uid;
	gid_t	gid;
	mode_t	mode;
};

static inline unsigned long copy_semid_from_user(struct sem_setbuf *out, void *buf, int version)
{
	switch(version) {
	case IPC_64:
	    {
		struct semid64_ds tbuf;

		if(copy_from_user(&tbuf, buf, sizeof(tbuf)))
			return -EFAULT;

		out->uid	= tbuf.sem_perm.uid;
		out->gid	= tbuf.sem_perm.gid;
		out->mode	= tbuf.sem_perm.mode;

		return 0;
	    }
	case IPC_OLD:
	    {
		struct semid_ds tbuf_old;

		if(copy_from_user(&tbuf_old, buf, sizeof(tbuf_old)))
			return -EFAULT;

		out->uid	= tbuf_old.sem_perm.uid;
		out->gid	= tbuf_old.sem_perm.gid;
		out->mode	= tbuf_old.sem_perm.mode;

		return 0;
	    }
	default:
		return -EINVAL;
	}
}

int semctl_down(int semid, int semnum, int cmd, int version, union semun arg)
{
	struct sem_array *sma;
	int err;
	struct sem_setbuf setbuf;
	struct kern_ipc_perm *ipcp;

	if(cmd == IPC_SET) {
		if(copy_semid_from_user (&setbuf, arg.buf, version))
			return -EFAULT;
	}
	sma = sem_lock(semid);
	if(sma==NULL)
		return -EINVAL;

	if (sem_checkid(sma,semid)) {
		err=-EIDRM;
		goto out_unlock;
	}	
	ipcp = &sma->sem_perm;
	
	if (current->euid != ipcp->cuid && 
	    current->euid != ipcp->uid && !capable(CAP_SYS_ADMIN)) {
	    	err=-EPERM;
		goto out_unlock;
	}

	switch(cmd){
	case IPC_RMID:
		freeary(semid);
		err = 0;
		break;
	case IPC_SET:
		ipcp->uid = setbuf.uid;
		ipcp->gid = setbuf.gid;
		ipcp->mode = (ipcp->mode & ~S_IRWXUGO)
				| (setbuf.mode & S_IRWXUGO);
		sma->sem_ctime = CURRENT_TIME;
		sem_unlock(semid);
		err = 0;
		break;
	default:
		sem_unlock(semid);
		err = -EINVAL;
		break;
	}
	return err;

out_unlock:
	sem_unlock(semid);
	return err;
}

/**
 * sys_semctl - System V信号量集控制系统调用
 * @semid: 信号量集id
 * @semnum: 信号量集中的索引（针对单个sem操作时使用）
 * @cmd: 控制命令
 * @arg: union semun，命令配套参数
 * return: 成功根据命令返回对应值；失败返回负错误码
 */
asmlinkage long sys_semctl (int semid, int semnum, int cmd, union semun arg)
{
    int err = -EINVAL;
    int version;

    if (semid < 0)
        return -EINVAL;

    // 解析IPC版本，兼容新旧IPC结构体布局
    version = ipc_parse_version(&cmd);

    switch(cmd) {
    case IPC_INFO:
    case SEM_INFO:
    case SEM_STAT:
        // 全局信息查询、遍历查询，不需要拿sem_ids.sem粗锁
        err = semctl_nolock(semid,semnum,cmd,version,arg);
        return err;
    case GETALL:
    case GETVAL:
    case GETPID:
    case GETNCNT:
    case GETZCNT:
    case IPC_STAT:
    case SETVAL:
    case SETALL:
        // 读写信号量值、状态、设置值；内部仅使用sem_lock细自旋锁
        err = semctl_main(semid,semnum,cmd,version,arg);
        return err;
    case IPC_RMID:
    case IPC_SET:
        // 修改类操作：删除信号量集 / 修改IPC权限uid/gid/mode
        // 需要全局粗锁 sem_ids.sem 保证原子
        down(&sem_ids.sem);
        err = semctl_down(semid,semnum,cmd,version,arg);
        up(&sem_ids.sem);
        return err;
    default:
        return -EINVAL;
    }
}


static struct sem_undo* freeundos(struct sem_array *sma, struct sem_undo* un)
{
	struct sem_undo* u;
	struct sem_undo** up;

	for(up = &current->semundo;(u=*up);up=&u->proc_next) {
		if(un==u) {
			un=u->proc_next;
			*up=un;
			kfree(u);
			return un;
		}
	}
	printk ("freeundos undo list error id=%d\n", un->semid);
	return un->proc_next;
}

/* returns without sem_lock on error! */
/**
 * alloc_undo - 分配并初始化一个sem_undo撤销结构
 * @sma: 信号量集合对象
 * @unp: 出参，成功时返回分配好的sem_undo结构体指针
 * @semid: 信号量集合id
 * @alter: 标记本次操作是否会修改信号量值（写操作）
 * 返回值：成功返回0，失败返回负错误码
 * 功能：SEM_UNDO机制底层，创建undo节点，同时挂入进程链表、信号量集合undo链表
 */
static int alloc_undo(struct sem_array *sma, struct sem_undo** unp, int semid, int alter)
{
    int size, nsems, error;
    struct sem_undo *un;

    // 获取该信号量集合内信号量的数量
    nsems = sma->sem_nsems;
    // 计算总大小：结构体本身 + semadj数组（每个信号量一个short记录撤销增量）
    size = sizeof(struct sem_undo) + sizeof(short)*nsems;
    // 释放信号量集合的锁
    sem_unlock(semid);

    // kmalloc分配undo结构内存
    un = (struct sem_undo *) kmalloc(size, GFP_KERNEL);
    if (!un)
        return -ENOMEM;

    // 整块内存清零初始化
    memset(un, 0, size);
    // 重新校验进程对该信号量集合的权限
    error = sem_revalidate(semid, sma, nsems, alter ? S_IWUGO : S_IRUGO);
    if(error) {
        // 权限校验失败，释放内存返回错误
        kfree(un);
        return error;
    }

    // semadj数组放在sem_undo结构体后面（柔性数组方式）
    un->semadj = (short *) &un[1];
    // 绑定所属信号量id
    un->semid = semid;
    // 插入到当前进程的semundo链表头部（进程维度链表）
    un->proc_next = current->semundo;
    current->semundo = un;
    // 插入到信号量集合sma的undo链表头部（信号量集合维度链表）
    un->id_next = sma->undo;
    sma->undo = un;
    // 回填出参，把新建undo结构返回给调用方
    *unp = un;
    return 0;
}

/**
 * sys_semop - System V信号量集semop系统调用，执行一组信号量操作（原子批操作）
 * @semid: 信号量集ID
 * @tsops: 用户态 struct sembuf 数组起始地址
 * @nsops: 本次要执行多少条sem操作
 * return: 成功返回0；失败返回负错误码
 */
asmlinkage long sys_semop (int semid, struct sembuf *tsops, unsigned nsops)
{
    int error = -EINVAL;
    struct sem_array *sma;
    struct sembuf fast_sops[SEMOPM_FAST];
    struct sembuf* sops = fast_sops, *sop;
    struct sem_undo *un;
    int undos = 0, decrease = 0, alter = 0;
    struct sem_queue queue;

    // 参数合法性检查：操作数至少1个，semid不能为负
    if (nsops < 1 || semid < 0)
        return -EINVAL;
    // 超过系统单次semop最大操作条数 sc_semopm
    if (nsops > sc_semopm)
        return -E2BIG;
    // 小操作数走栈上fast缓冲区，避免kmalloc；大操作数动态分配
    if(nsops > SEMOPM_FAST) {
        sops = kmalloc(sizeof(*sops)*nsops,GFP_KERNEL);
        if(sops==NULL)
            return -ENOMEM;
    }
    // 把用户态sembuf数组拷贝到内核缓冲区
    if (copy_from_user (sops, tsops, nsops * sizeof(*tsops))) {
        error=-EFAULT;
        goto out_free;
    }
    // 获取信号量集对象 + 持有细粒度自旋锁
    sma = sem_lock(semid);
    error=-EINVAL;
    if(sma==NULL)
        goto out_free;
    // 校验semid序列号，防止已经销毁复用id（-EIDRM）
    error = -EIDRM;
    if (sem_checkid(sma,semid))
        goto out_unlock_free;
    error = -EFBIG;
    // 遍历所有操作，预处理标记：
    // undos：是否有操作带SEM_UNDO；decrease：有没有sem_op<0（P操作，减）；alter：是否修改信号量
    for (sop = sops; sop < sops + nsops; sop++) {
        if (sop->sem_num >= sma->sem_nsems)
            goto out_unlock_free;
        if (sop->sem_flg & SEM_UNDO)
            undos++;
        if (sop->sem_op < 0)
            decrease = 1;
        if (sop->sem_op > 0)
            alter = 1;
    }
    alter |= decrease;

    // IPC权限校验：写操作需要S_IWUGO，只读（sem_op=0等待0）只需要S_IRUGO
    error = -EACCES;
    if (ipcperms(&sma->sem_perm, alter ? S_IWUGO : S_IRUGO))
        goto out_unlock_free;
    if (undos) {
        /* Make sure we have an undo structure
         * for this process and this semaphore set.
         */
        // 存在SEM_UNDO，查找当前进程绑定这个sem集的sem_undo节点
        un=current->semundo;
        while(un != NULL) {
            if(un->semid==semid)
                break;
            if(un->semid==-1)
                un=freeundos(sma,un);
             else
                un=un->proc_next;
        }
        // 没有找到就分配新的sem_undo，用于进程退出时回滚信号量
        if (!un) {
            error = alloc_undo(sma,&un,semid,alter);
            if(error)
                goto out_free;
        }
    } else
        un = NULL;

    // 尝试原子执行这一批sem操作，非阻塞尝试
    // error <=0：成功或者遇到直接返回的错误；error>0：条件不满足，需要阻塞睡眠
    error = try_atomic_semop (sma, sops, nsops, un, current->pid, 0);
    if (error <= 0)
        goto update;

    /* We need to sleep on this operation, so we put the current
     * task into the pending queue and go to sleep.
     */
    // 条件不满足，构造sem_queue等待节点，加入sem的pending等待链表
    queue.sma = sma;
    queue.sops = sops;
    queue.nsops = nsops;
    queue.undo = un;
    queue.pid = current->pid;
    queue.alter = decrease;
    queue.id = semid;
    // 会修改信号量的操作放队尾；纯等待（sem_op=0）插到队头，优先级更高
    if (alter)
        append_to_queue(sma ,&queue);
    else
        prepend_to_queue(sma ,&queue);
    current->semsleeping = &queue;

    // 睡眠循环，可中断 TASK_INTERRUPTIBLE，可以被信号唤醒
    for (;;) {
        struct sem_array* tmp;
        queue.status = -EINTR;
        queue.sleeper = current;
        current->state = TASK_INTERRUPTIBLE;
        // 放锁，睡眠前释放自旋锁，避免阻塞其他sem操作
        sem_unlock(semid);

        schedule();

        // 被唤醒之后，重新拿锁检查sem集是否还存在
        tmp = sem_lock(semid);
        if(tmp==NULL) {
            if(queue.status != -EIDRM)
                BUG();
            current->semsleeping = NULL;
            error = -EIDRM;
            goto out_free;
        }
        /*
         * If queue.status == 1 we where woken up and
         * have to retry else we simply return.
         * If an interrupt occurred we have to clean up the
         * queue
         *
         */
        // status==1：被别的V操作唤醒，重新尝试执行semop
        if (queue.status == 1)
        {
            error = try_atomic_semop (sma, sops, nsops, un,
                    current->pid,0);
            if (error <= 0) 
                break;
        } else {
            // 信号打断、或者被删除，直接返回状态
            error = queue.status;
            if (queue.prev) /* got Interrupt */
                break;
            /* Everything done by update_queue */
            current->semsleeping = NULL;
            goto out_unlock_free;
        }
    }
    current->semsleeping = NULL;
    // 成功执行，把自己从pending等待链表移除
    remove_from_queue(sma,&queue);
update:
    // 如果本次操作修改了信号量的值，调用update_queue唤醒等待链表上能满足条件的进程
    if (alter)
        update_queue (sma);
out_unlock_free:
    sem_unlock(semid);
out_free:
    // 如果不是栈上fast buffer，释放kmalloc内存
    if(sops != fast_sops)
        kfree(sops);
    return error;
}

/*
 * add semadj values to semaphores, free undo structures.
 * undo structures are not freed when semaphore arrays are destroyed
 * so some of them may be out of date.
 * IMPLEMENTATION NOTE: There is some confusion over whether the
 * set of adjustments that needs to be done should be done in an atomic
 * manner or not. That is, if we are attempting to decrement the semval
 * should we queue up and wait until we can do so legally?
 * The original implementation attempted to do this (queue and wait).
 * The current implementation does not do so. The POSIX standard
 * and SVID should be consulted to determine what behavior is mandated.
 */
void sem_exit (void)
{
	struct sem_queue *q;
	struct sem_undo *u, *un = NULL, **up, **unp;
	struct sem_array *sma;
	int nsems, i;

	/* If the current process was sleeping for a semaphore,
	 * remove it from the queue.
	 */
	if ((q = current->semsleeping)) {
		int semid = q->id;
		sma = sem_lock(semid);
		current->semsleeping = NULL;

		if (q->prev) {
			if(sma==NULL)
				BUG();
			remove_from_queue(q->sma,q);
		}
		if(sma!=NULL)
			sem_unlock(semid);
	}

	for (up = &current->semundo; (u = *up); *up = u->proc_next, kfree(u)) {
		int semid = u->semid;
		if(semid == -1)
			continue;
		sma = sem_lock(semid);
		if (sma == NULL)
			continue;

		if (u->semid == -1)
			goto next_entry;

		if (sem_checkid(sma,u->semid))
			goto next_entry;

		/* remove u from the sma->undo list */
		for (unp = &sma->undo; (un = *unp); unp = &un->id_next) {
			if (u == un)
				goto found;
		}
		printk ("sem_exit undo list error id=%d\n", u->semid);
		goto next_entry;
found:
		*unp = un->id_next;
		/* perform adjustments registered in u */
		nsems = sma->sem_nsems;
		for (i = 0; i < nsems; i++) {
			struct sem * sem = &sma->sem_base[i];
			sem->semval += u->semadj[i];
			if (sem->semval < 0)
				sem->semval = 0; /* shouldn't happen */
			sem->sempid = current->pid;
		}
		sma->sem_otime = CURRENT_TIME;
		/* maybe some queued-up processes were waiting for this */
		update_queue(sma);
next_entry:
		sem_unlock(semid);
	}
	current->semundo = NULL;
}

#ifdef CONFIG_PROC_FS
static int sysvipc_sem_read_proc(char *buffer, char **start, off_t offset, int length, int *eof, void *data)
{
	off_t pos = 0;
	off_t begin = 0;
	int i, len = 0;

	len += sprintf(buffer, "       key      semid perms      nsems   uid   gid  cuid  cgid      otime      ctime\n");
	down(&sem_ids.sem);

	for(i = 0; i <= sem_ids.max_id; i++) {
		struct sem_array *sma;
		sma = sem_lock(i);
		if(sma) {
			len += sprintf(buffer + len, "%10d %10d  %4o %10lu %5u %5u %5u %5u %10lu %10lu\n",
				sma->sem_perm.key,
				sem_buildid(i,sma->sem_perm.seq),
				sma->sem_perm.mode,
				sma->sem_nsems,
				sma->sem_perm.uid,
				sma->sem_perm.gid,
				sma->sem_perm.cuid,
				sma->sem_perm.cgid,
				sma->sem_otime,
				sma->sem_ctime);
			sem_unlock(i);

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
	up(&sem_ids.sem);
	*start = buffer + (offset - begin);
	len -= (offset - begin);
	if(len > length)
		len = length;
	if(len < 0)
		len = 0;
	return len;
}
#endif
