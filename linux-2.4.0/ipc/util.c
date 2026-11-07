/*
 * linux/ipc/util.c
 * Copyright (C) 1992 Krishna Balasubramanian
 *
 * Sep 1997 - Call suser() last after "normal" permission checks so we
 *            get BSD style process accounting right.
 *            Occurs in several places in the IPC code.
 *            Chris Evans, <chris@ferret.lmh.ox.ac.uk>
 * Nov 1999 - ipc helper functions, unified SMP locking
 *	      Manfred Spraul <manfreds@colorfullife.com>
 */

#include <linux/config.h>
#include <linux/mm.h>
#include <linux/shm.h>
#include <linux/init.h>
#include <linux/msg.h>
#include <linux/smp_lock.h>
#include <linux/vmalloc.h>
#include <linux/malloc.h>
#include <linux/highuid.h>

#if defined(CONFIG_SYSVIPC)

#include "util.h"

/**
 *	ipc_init	-	initialise IPC subsystem
 *
 *	The various system5 IPC resources (semaphores, messages and shared
 *	memory are initialised
 */
 
void __init ipc_init (void)
{
	sem_init();
	msg_init();
	shm_init();
	return;
}

/**
 *	ipc_init_ids		-	initialise IPC identifiers
 *	@ids: Identifier set
 *	@size: Number of identifiers
 *
 *	Given a size for the ipc identifier range (limited below IPCMNI)
 *	set up the sequence range to use then allocate and initialise the
 *	array itself. 
 */
 
/**
 * ipc_init_ids - 初始化ipc_ids管理结构
 * @ids: 待初始化的ipc_ids对象（msg_ids/sem_ids/shm_ids）
 * @size: 预期最大条目数
 */
void __init ipc_init_ids(struct ipc_ids* ids, int size)
{
    int i;
    // 初始化信号量sem，初值为1，互斥锁
    sema_init(&ids->sem,1);

    // 不能超过IPC最大上限IPCMNI
    if(size > IPCMNI)
        size = IPCMNI;
    // 设置entries数组容量
    ids->size = size;
    // 当前在用IPC对象数量初始化为0
    ids->in_use = 0;
    // 当前最大内核id初始为-1，表示还没有分配任何id
    ids->max_id = -1;
    // 序列号起始值为0
    ids->seq = 0;
    {
        int seq_limit = INT_MAX/SEQ_MULTIPLIER;
        // 计算seq上限，不超过unsigned short最大值
        if(seq_limit > USHRT_MAX)
            ids->seq_max = USHRT_MAX;
        else
            ids->seq_max = seq_limit;
    }

    // 分配ipc_id条目数组
    ids->entries = ipc_alloc(sizeof(struct ipc_id)*size);

    if(ids->entries == NULL) {
        // 内存分配失败，打印错误，禁用该类IPC
        printk(KERN_ERR "ipc_init_ids() failed, ipc service disabled.\n");
        ids->size = 0;
    }
    // 初始化ary自旋锁，初始为未上锁状态
    ids->ary = SPIN_LOCK_UNLOCKED;
    // 遍历清空，所有条目的指针初始置NULL，表示空闲
    for(i=0;i<size;i++)
        ids->entries[i].p = NULL;
}

/**
 *	ipc_findkey	-	find a key in an ipc identifier set	
 *	@ids: Identifier set
 *	@key: The key to find
 *
 *	Returns the identifier if found or -1 if not.
 */
 
/**
 * ipc_findkey - 根据key遍历查找对应的IPC内核id
 * @ids: ipc_ids管理头
 * @key: 用户传入的IPC键值
 * return: 找到返回内核id，未找到返回-1
 */
int ipc_findkey(struct ipc_ids* ids, key_t key)
{
    int id;
    struct kern_ipc_perm* p;

    // 遍历0到max_id范围内所有槽位
    for (id = 0; id <= ids->max_id; id++) {
        // 获取当前槽位保存的IPC权限结构指针
        p = ids->entries[id].p;
        // 槽位为空，跳过
        if(p==NULL)
            continue;
        // 匹配key，返回对应的内核id
        if (key == p->key)
            return id;
    }
    // 全部遍历完毕，没有匹配的key
    return -1;
}


/**
 * grow_ary - 扩容ipc_ids的entries条目数组
 * @ids: ipc_ids管理头
 * @newsize: 期望的新容量
 * return: 扩容之后最终生效的容量
 */
static int grow_ary(struct ipc_ids* ids, int newsize)
{
    struct ipc_id* new;
    struct ipc_id* old;
    int i;

    // 容量不能超过IPC全局最大限制IPCMNI
    if(newsize > IPCMNI)
        newsize = IPCMNI;
    // 新容量不大于现有容量，无需扩容，直接返回
    if(newsize <= ids->size)
        return newsize;

    // 分配更大的entries数组空间
    new = ipc_alloc(sizeof(struct ipc_id)*newsize);
    if(new == NULL)
        return ids->size;
    // 拷贝原有数组内容到新数组
    memcpy(new, ids->entries, sizeof(struct ipc_id)*ids->size);
    // 新增的槽位全部初始化为空闲NULL
    for(i=ids->size;i<newsize;i++) {
        new[i].p = NULL;
    }
    // 自旋锁保护entries指针替换操作
    spin_lock(&ids->ary);

    old = ids->entries;
    ids->entries = new;
    i = ids->size;
    ids->size = newsize;
    spin_unlock(&ids->ary);
    // 释放旧数组内存
    ipc_free(old, sizeof(struct ipc_id)*i);
    return ids->size;
}

/**
 *	ipc_addid 	-	add an IPC identifier
 *	@ids: IPC identifier set
 *	@new: new IPC permission set
 *	@size: new size limit for the id array
 *
 *	Add an entry 'new' to the IPC arrays. The permissions object is
 *	initialised and the first free entry is set up and the id assigned
 *	is returned. The list is returned in a locked state on success.
 *	On failure the list is not locked and -1 is returned.
 */
 
/**
 * ipc_addid - 在ipc_ids管理表中分配一个空闲id，挂载新的IPC对象
 * @ids: ipc_ids管理头（msg_ids/sem_ids/shm_ids）
 * @new: 新IPC对象的kern_ipc_perm成员地址
 * @size: 预期最大容量
 * return: 成功返回分配到的内核id，失败返回-1
 */
int ipc_addid(struct ipc_ids* ids, struct kern_ipc_perm* new, int size)
{
    int id;

    // 根据size扩容entries数组
    size = grow_ary(ids,size);
    // 遍历entries数组，寻找空闲槽位（p等于NULL）
    for (id = 0; id < size; id++) {
        if(ids->entries[id].p == NULL)
            goto found;
    }
    return -1;
found:
    // 正在使用的IPC对象计数加一
    ids->in_use++;
    // 更新当前最大id
    if (id > ids->max_id)
        ids->max_id = id;

    // 设置创建者、属主的uid
    new->cuid = new->uid = current->euid;
    // 设置创建者、属主的gid
    new->gid = new->cgid = current->egid;

    // 分配序列号，序列号自增
    new->seq = ids->seq++;
    // 序列号到达上限后回绕归零
    if(ids->seq > ids->seq_max)
        ids->seq = 0;

    // 拿自旋锁保护entries数组
    spin_lock(&ids->ary);
    // 将新IPC对象挂载到找到的空闲槽位
    ids->entries[id].p = new;
    return id;
}

/**
 *	ipc_rmid	-	remove an IPC identifier
 *	@ids: identifier set
 *	@id: Identifier to remove
 *
 *	The identifier must be valid, and in use. The kernel will panic if
 *	fed an invalid identifier. The entry is removed and internal
 *	variables recomputed. The object associated with the identifier
 *	is returned.
 */
 
struct kern_ipc_perm* ipc_rmid(struct ipc_ids* ids, int id)
{
	struct kern_ipc_perm* p;
	int lid = id % SEQ_MULTIPLIER;
	if(lid > ids->size)
		BUG();
	p = ids->entries[lid].p;
	ids->entries[lid].p = NULL;
	if(p==NULL)
		BUG();
	ids->in_use--;

	if (lid == ids->max_id) {
		do {
			lid--;
			if(lid == -1)
				break;
		} while (ids->entries[lid].p == NULL);
		ids->max_id = lid;
	}
	return p;
}

/**
 *	ipc_alloc	-	allocate ipc space
 *	@size: size desired
 *
 *	Allocate memory from the appropriate pools and return a pointer to it.
 *	NULL is returned if the allocation fails
 */
 
void* ipc_alloc(int size)
{
	void* out;
	if(size > PAGE_SIZE)
		out = vmalloc(size);
	else
		out = kmalloc(size, GFP_KERNEL);
	return out;
}

/**
 *	ipc_free	-	free ipc space
 *	@ptr: pointer returned by ipc_alloc
 *	@size: size of block
 *
 *	Free a block created with ipc_alloc. The caller must know the size
 *	used in the allocation call.
 */
 
void ipc_free(void* ptr, int size)
{
	if(size > PAGE_SIZE)
		vfree(ptr);
	else
		kfree(ptr);
}

/**
 *	ipcperms	-	check IPC permissions
 *	@ipcp: IPC permission set
 *	@flag: desired permission set.
 *
 *	Check user, group, other permissions for access
 *	to ipc resources. return 0 if allowed
 */
 
int ipcperms (struct kern_ipc_perm *ipcp, short flag)
{	/* flag will most probably be 0 or S_...UGO from <linux/stat.h> */
	int requested_mode, granted_mode;

	requested_mode = (flag >> 6) | (flag >> 3) | flag;
	granted_mode = ipcp->mode;
	if (current->euid == ipcp->cuid || current->euid == ipcp->uid)
		granted_mode >>= 6;
	else if (in_group_p(ipcp->cgid) || in_group_p(ipcp->gid))
		granted_mode >>= 3;
	/* is there some bit set in requested_mode but not in granted_mode? */
	if ((requested_mode & ~granted_mode & 0007) && 
	    !capable(CAP_IPC_OWNER))
		return -1;

	return 0;
}

/*
 * Functions to convert between the kern_ipc_perm structure and the
 * old/new ipc_perm structures
 */

/**
 *	kernel_to_ipc64_perm	-	convert kernel ipc permissions to user
 *	@in: kernel permissions
 *	@out: new style IPC permissions
 *
 *	Turn the kernel object 'in' into a set of permissions descriptions
 *	for returning to userspace (out).
 */
 

void kernel_to_ipc64_perm (struct kern_ipc_perm *in, struct ipc64_perm *out)
{
	out->key	= in->key;
	out->uid	= in->uid;
	out->gid	= in->gid;
	out->cuid	= in->cuid;
	out->cgid	= in->cgid;
	out->mode	= in->mode;
	out->seq	= in->seq;
}

/**
 *	ipc64_perm_to_ipc_perm	-	convert old ipc permissions to new
 *	@in: new style IPC permissions
 *	@out: old style IPC permissions
 *
 *	Turn the new style permissions object in into a compatibility
 *	object and store it into the 'out' pointer.
 */
 
void ipc64_perm_to_ipc_perm (struct ipc64_perm *in, struct ipc_perm *out)
{
	out->key	= in->key;
	out->uid	= NEW_TO_OLD_UID(in->uid);
	out->gid	= NEW_TO_OLD_GID(in->gid);
	out->cuid	= NEW_TO_OLD_UID(in->cuid);
	out->cgid	= NEW_TO_OLD_GID(in->cgid);
	out->mode	= in->mode;
	out->seq	= in->seq;
}

#ifndef __ia64__

/**
 *	ipc_parse_version	-	IPC call version
 *	@cmd: pointer to command
 *
 *	Return IPC_64 for new style IPC and IPC_OLD for old style IPC. 
 *	The cmd value is turned from an encoding command and version into
 *	just the command code.
 */
 
int ipc_parse_version (int *cmd)
{
	if (*cmd & IPC_64) {
		*cmd ^= IPC_64;
		return IPC_64;
	} else {
		return IPC_OLD;
	}
}

#endif /* __ia64__ */

#else
/*
 * Dummy functions when SYSV IPC isn't configured
 */

void sem_exit (void)
{
    return;
}

asmlinkage long sys_semget (key_t key, int nsems, int semflg)
{
	return -ENOSYS;
}

asmlinkage long sys_semop (int semid, struct sembuf *sops, unsigned nsops)
{
	return -ENOSYS;
}

asmlinkage long sys_semctl (int semid, int semnum, int cmd, union semun arg)
{
	return -ENOSYS;
}

asmlinkage long sys_msgget (key_t key, int msgflg)
{
	return -ENOSYS;
}

asmlinkage long sys_msgsnd (int msqid, struct msgbuf *msgp, size_t msgsz, int msgflg)
{
	return -ENOSYS;
}

asmlinkage long sys_msgrcv (int msqid, struct msgbuf *msgp, size_t msgsz, long msgtyp,
		       int msgflg)
{
	return -ENOSYS;
}

asmlinkage long sys_msgctl (int msqid, int cmd, struct msqid_ds *buf)
{
	return -ENOSYS;
}

asmlinkage long sys_shmget (key_t key, size_t size, int shmflag)
{
	return -ENOSYS;
}

asmlinkage long sys_shmat (int shmid, char *shmaddr, int shmflg, ulong *addr)
{
	return -ENOSYS;
}

asmlinkage long sys_shmdt (char *shmaddr)
{
	return -ENOSYS;
}

asmlinkage long sys_shmctl (int shmid, int cmd, struct shmid_ds *buf)
{
	return -ENOSYS;
}

#endif /* CONFIG_SYSVIPC */
