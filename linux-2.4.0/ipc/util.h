/*
 * linux/ipc/util.h
 * Copyright (C) 1999 Christoph Rohland
 *
 * ipc helper functions (c) 1999 Manfred Spraul <manfreds@colorfullife.com>
 */

#define USHRT_MAX 0xffff
#define SEQ_MULTIPLIER	(IPCMNI)

void sem_init (void);
void msg_init (void);
void shm_init (void);

/**
 * struct ipc_ids - IPC资源的id管理头部结构（msg/shm/sem共用）
 * 用于管理一套IPC对象的id分配、序列号、并发保护
 */
struct ipc_ids {
    // entries数组总容量，最大可容纳的IPC资源数目
    int size;
    // 当前已经在用的IPC对象数量
    int in_use;
    // 当前已分配的最大内核id
    int max_id;
    // 序列号，用于id回收防复用（msg_buildid会用到）
    unsigned short seq;
    // 序列号上限，seq到达seq_max后回卷
    unsigned short seq_max;
    // 全局信号量，保护entries表的查找、新增、删除（sys_msgget中down(&msg_ids.sem)就是它）
    struct semaphore sem;
    // entries数组的自旋锁，细粒度保护entries数组
    spinlock_t ary;
    // ipc_id条目数组，存放各个IPC对象的指针
    struct ipc_id* entries;
};


struct ipc_id {
	struct kern_ipc_perm* p;
};


void __init ipc_init_ids(struct ipc_ids* ids, int size);

/* must be called with ids->sem acquired.*/
int ipc_findkey(struct ipc_ids* ids, key_t key);
int ipc_addid(struct ipc_ids* ids, struct kern_ipc_perm* new, int size);

/* must be called with both locks acquired. */
struct kern_ipc_perm* ipc_rmid(struct ipc_ids* ids, int id);

int ipcperms (struct kern_ipc_perm *ipcp, short flg);

/* for rare, potentially huge allocations.
 * both function can sleep
 */
void* ipc_alloc(int size);
void ipc_free(void* ptr, int size);

extern inline void ipc_lockall(struct ipc_ids* ids)
{
	spin_lock(&ids->ary);
}

extern inline struct kern_ipc_perm* ipc_get(struct ipc_ids* ids, int id)
{
	struct kern_ipc_perm* out;
	int lid = id % SEQ_MULTIPLIER;
	if(lid > ids->size)
		return NULL;

	out = ids->entries[lid].p;
	return out;
}

extern inline void ipc_unlockall(struct ipc_ids* ids)
{
	spin_unlock(&ids->ary);
}
/**
 * ipc_lock - 根据用户传入msgid，锁定并取出对应的kern_ipc_perm对象
 * @ids: ipc_ids管理头
 * @id: 用户态传进来的msgid
 * return: 成功返回IPC权限结构指针；失败返回NULL
 * 注意：返回非NULL时，ary自旋锁处于持有状态，调用方后续必须ipc_unlock解锁
 */
extern inline struct kern_ipc_perm* ipc_lock(struct ipc_ids* ids, int id)
{
    struct kern_ipc_perm* out;
    // 取模，从msgid中解析出内核数组下标lid
    int lid = id % SEQ_MULTIPLIER;
    // 下标超出entries数组容量，直接返回空
    if(lid > ids->size)
        return NULL;

    // 上锁，保护entries数组的访问
    spin_lock(&ids->ary);
    out = ids->entries[lid].p;
    // 槽位为空，释放自旋锁再返回NULL
    if(out==NULL)
        spin_unlock(&ids->ary);
    return out;
}

extern inline void ipc_unlock(struct ipc_ids* ids, int id)
{
	spin_unlock(&ids->ary);
}

/**
 * ipc_buildid - 拼接内核id与序列号，生成对外暴露给用户态的msgid
 * @ids: ipc_ids管理头
 * @id: 内核内部的数组下标id
 * @seq: IPC对象的序列号
 * return: 组合后的msgid，返回给用户程序
 */
extern inline int ipc_buildid(struct ipc_ids* ids, int id, int seq)
{
    // 序列号乘以SEQ_MULTIPLIER，加上数组下标id，组成对外id
    return SEQ_MULTIPLIER*seq + id;
}

extern inline int ipc_checkid(struct ipc_ids* ids, struct kern_ipc_perm* ipcp, int uid)
{
	if(uid/SEQ_MULTIPLIER != ipcp->seq)
		return 1;
	return 0;
}

void kernel_to_ipc64_perm(struct kern_ipc_perm *in, struct ipc64_perm *out);
void ipc64_perm_to_ipc_perm(struct ipc64_perm *in, struct ipc_perm *out);

#ifdef __ia64__
  /* On IA-64, we always use the "64-bit version" of the IPC structures.  */ 
# define ipc_parse_version(cmd)	IPC_64
#else
int ipc_parse_version (int *cmd);
#endif
