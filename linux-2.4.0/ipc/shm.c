/*
 * linux/ipc/shm.c
 * Copyright (C) 1992, 1993 Krishna Balasubramanian
 *	 Many improvements/fixes by Bruno Haible.
 * Replaced `struct shm_desc' by `struct vm_area_struct', July 1994.
 * Fixed the shm swap deallocation (shm_unuse()), August 1998 Andrea Arcangeli.
 *
 * /proc/sysvipc/shm support (c) 1999 Dragos Acostachioaie <dragos@iname.com>
 * BIGMEM support, Andrea Arcangeli <andrea@suse.de>
 * SMP thread shm, Jean-Luc Boyard <jean-luc.boyard@siemens.fr>
 * HIGHMEM support, Ingo Molnar <mingo@redhat.com>
 * Make shmmax, shmall, shmmni sysctl'able, Christoph Rohland <cr@sap.com>
 * Shared /dev/zero support, Kanoj Sarcar <kanoj@sgi.com>
 * Move the mm functionality over to mm/shmem.c, Christoph Rohland <cr@sap.com>
 *
 */

#include <linux/config.h>
#include <linux/malloc.h>
#include <linux/shm.h>
#include <linux/init.h>
#include <linux/file.h>
#include <linux/mman.h>
#include <linux/proc_fs.h>
#include <asm/uaccess.h>

#include "util.h"

/**
 * struct shmid_kernel - System V共享内存内核管理对象，用户态不可见
 */
struct shmid_kernel /* private to the kernel */
{    
    // IPC通用权限结构：key、uid/gid、mode、seq序列号
    struct kern_ipc_perm    shm_perm;
    // 底层shmem匿名文件，真正承载内存页面
    struct file *       shm_file;
    // 对外给用户的shmid（buildid合成：seq + 数组下标）
    int         id;
    // 当前attach挂载的进程数量
    unsigned long       shm_nattch;
    // 共享内存段字节大小
    unsigned long       shm_segsz;
    // 最后一次attach时间
    time_t          shm_atim;
    // 最后一次detach时间
    time_t          shm_dtim;
    // 创建/最后一次IPC_SET修改时间
    time_t          shm_ctim;
    // 创建该shm段的进程pid
    pid_t           shm_cprid;
    // 最后一次执行shmat/shmdt的进程pid
    pid_t           shm_lprid;
};


#define shm_flags	shm_perm.mode

static struct file_operations shm_file_operations;
static struct vm_operations_struct shm_vm_ops;

//共享内存的管理结构
static struct ipc_ids shm_ids;

#define shm_lock(id)	((struct shmid_kernel*)ipc_lock(&shm_ids,id))
#define shm_unlock(id)	ipc_unlock(&shm_ids,id)
#define shm_lockall()	ipc_lockall(&shm_ids)
#define shm_unlockall()	ipc_unlockall(&shm_ids)
#define shm_get(id)	((struct shmid_kernel*)ipc_get(&shm_ids,id))
#define shm_buildid(id, seq) \
	ipc_buildid(&shm_ids, id, seq)

static int newseg (key_t key, int shmflg, size_t size);
static void shm_open (struct vm_area_struct *shmd);
static void shm_close (struct vm_area_struct *shmd);
#ifdef CONFIG_PROC_FS
static int sysvipc_shm_read_proc(char *buffer, char **start, off_t offset, int length, int *eof, void *data);
#endif

size_t	shm_ctlmax = SHMMAX;
size_t 	shm_ctlall = SHMALL;
int 	shm_ctlmni = SHMMNI;

static int shm_tot; /* total number of shared memory pages */

void __init shm_init (void)
{
	ipc_init_ids(&shm_ids, 1);
	create_proc_read_entry("sysvipc/shm", 0, 0, sysvipc_shm_read_proc, NULL);
}

static inline int shm_checkid(struct shmid_kernel *s, int id)
{
	if (ipc_checkid(&shm_ids,&s->shm_perm,id))
		return -EIDRM;
	return 0;
}

static inline struct shmid_kernel *shm_rmid(int id)
{
	return (struct shmid_kernel *)ipc_rmid(&shm_ids,id);
}

//添加一个共享内存
static inline int shm_addid(struct shmid_kernel *shp)
{
	return ipc_addid(&shm_ids, &shp->shm_perm, shm_ctlmni+1);
}



static inline void shm_inc (int id) {
	struct shmid_kernel *shp;

	if(!(shp = shm_lock(id)))
		BUG();
	shp->shm_atim = CURRENT_TIME;
	shp->shm_lprid = current->pid;
	shp->shm_nattch++;
	shm_unlock(id);
}

/* This is called by fork, once for every shm attach. */
static void shm_open (struct vm_area_struct *shmd)
{
	shm_inc (shmd->vm_file->f_dentry->d_inode->i_ino);
}

/*
 * shm_destroy - free the struct shmid_kernel
 *
 * @shp: struct to free
 *
 * It has to be called with shp and shm_ids.sem locked
 */
static void shm_destroy (struct shmid_kernel *shp)
{
	shm_tot -= (shp->shm_segsz + PAGE_SIZE - 1) >> PAGE_SHIFT;
	shm_rmid (shp->id);
	fput (shp->shm_file);
	kfree (shp);
}

/*
 * remove the attach descriptor shmd.
 * free memory for segment if it is marked destroyed.
 * The descriptor has already been removed from the current->mm->mmap list
 * and will later be kfree()d.
 */
static void shm_close (struct vm_area_struct *shmd)
{
	struct file * file = shmd->vm_file;
	int id = file->f_dentry->d_inode->i_ino;
	struct shmid_kernel *shp;

	down (&shm_ids.sem);
	/* remove from the list of attaches of the shm segment */
	if(!(shp = shm_lock(id)))
		BUG();
	shp->shm_lprid = current->pid;
	shp->shm_dtim = CURRENT_TIME;
	shp->shm_nattch--;
	if(shp->shm_nattch == 0 &&
	   shp->shm_flags & SHM_DEST)
		shm_destroy (shp);

	shm_unlock(id);
	up (&shm_ids.sem);
}

static int shm_mmap(struct file * file, struct vm_area_struct * vma)
{
	UPDATE_ATIME(file->f_dentry->d_inode);
	vma->vm_ops = &shm_vm_ops;
	shm_inc(file->f_dentry->d_inode->i_ino);
	return 0;
}

static struct file_operations shm_file_operations = {
	mmap:	shm_mmap
};

static struct vm_operations_struct shm_vm_ops = {
	open:	shm_open,	/* callback for a new vm-area open */
	close:	shm_close,	/* callback for when the vm-area is released */
	nopage:	shmem_nopage,
};

/**
 * newseg - 创建一个新的System V共享内存段，sys_shmget创建分支调用
 * @key: IPC key
 * @shmflg: 标志与权限位
 * @size: 用户请求的共享内存大小
 * return: 成功返回组装好的shmid；失败返回负错误码
 */
static int newseg (key_t key, int shmflg, size_t size)
{
    int error;
    struct shmid_kernel *shp;
    // 向上对齐到page页数
    int numpages = (size + PAGE_SIZE -1) >> PAGE_SHIFT;
    struct file * file;
    char name[13];
    int id;

    // 校验段大小：大于等于SHMMIN，不超过系统全局上限shm_ctlmax
    if (size < SHMMIN || size > shm_ctlmax)
        return -EINVAL;

    // 全局shm总页数超限，返回-ENOSPC
    if (shm_tot + numpages >= shm_ctlall)
        return -ENOSPC;

    // 分配shmid_kernel管理结构体
    shp = (struct shmid_kernel *) kmalloc (sizeof (*shp), GFP_USER);
    if (!shp)
        return -ENOMEM;
    // 给shmem伪文件命名 SYSV+key的十六进制
    sprintf (name, "SYSV%08x", key);
    // shmem_file_setup：在tmpfs/shmem层建立一个匿名shmem文件（内核文件）
    file = shmem_file_setup(name, size);
    error = PTR_ERR(file);
    if (IS_ERR(file))
        goto no_file;

    error = -ENOSPC;
    // 分配shm数组下标id，初始化shp->shm_perm（seq序列号在这里生成）
    id = shm_addid(shp);
    if(id == -1) 
        goto no_id;
    // 填充IPC权限、属性
    shp->shm_perm.key = key;
    shp->shm_flags = (shmflg & S_IRWXUGO);
    shp->shm_cprid = current->pid;    // 创建者pid
    shp->shm_lprid = 0;
    shp->shm_atim = shp->shm_dtim = 0;
    shp->shm_ctim = CURRENT_TIME;     // 创建时间
    shp->shm_segsz = size;            // 段大小
    shp->shm_nattch = 0;              // 当前挂载计数，初始0
    // 对外暴露的shmid = seq * SEQ_MULTIPLIER + id
    shp->id = shm_buildid(id,shp->shm_perm.seq);
    // 绑定shmem文件
    shp->shm_file = file;
    // inode编号设置为shmid
    file->f_dentry->d_inode->i_ino = shp->id;
    // 替换文件操作集为shm专属ops
    file->f_op = &shm_file_operations;
    // 全局shm页数统计累加
    shm_tot += numpages;
    // 释放shm对象自旋锁（shm_addid内部会上锁）
    shm_unlock (id);
    return shp->id;

no_id:
    // shm_addid失败，释放file引用
    fput(file);
no_file:
    // 释放shmid_kernel结构体
    kfree(shp);
    return error;
}

/**
 * sys_shmget - System V共享内存段创建/查找系统调用
 * @key: IPC键值，IPC_PRIVATE代表私有段
 * @size: 共享内存段大小
 * @shmflg: 标志位 IPC_CREAT / IPC_EXCL / 权限位
 * return: 成功返回shmid；失败返回负错误码
 */
asmlinkage long sys_shmget (key_t key, size_t size, int shmflg)
{
    struct shmid_kernel *shp;
    int err, id = 0;

    // 拿shm全局粗粒度信号量，保护整个查找+创建流程
    down(&shm_ids.sem);
    if (key == IPC_PRIVATE) {
        // IPC_PRIVATE：总是新建共享内存段，不按key查找
        err = newseg(key, shmflg, size);
    } else if ((id = ipc_findkey(&shm_ids, key)) == -1) {
        // 根据key查找，没有找到对应shm段
        if (!(shmflg & IPC_CREAT))
            // 没有指定创建，直接返回不存在
            err = -ENOENT;
        else
            // 指定创建，调用newseg分配新段
            err = newseg(key, shmflg, size);
    } else if ((shmflg & IPC_CREAT) && (shmflg & IPC_EXCL)) {
        // IPC_CREAT | IPC_EXCL：要求新建，但key已经存在，返回已存在
        err = -EEXIST;
    } else {
        // key找到已有shm段，直接复用
        shp = shm_lock(id);
        if(shp==NULL)
            BUG();
        // 检查：已有段的空间不能小于用户请求size
        if (shp->shm_segsz < size)
            err = -EINVAL;
        else if (ipcperms(&shp->shm_perm, shmflg))
            // 权限校验失败
            err = -EACCES;
        else
            // 组装对外的shmid（类似ipc_buildid，下标+序列号）
            err = shm_buildid(id, shp->shm_perm.seq);
        shm_unlock(id);
    }
    // 释放全局shm信号量
    up(&shm_ids.sem);
    return err;
}

static inline unsigned long copy_shmid_to_user(void *buf, struct shmid64_ds *in, int version)
{
	switch(version) {
	case IPC_64:
		return copy_to_user(buf, in, sizeof(*in));
	case IPC_OLD:
	    {
		struct shmid_ds out;

		ipc64_perm_to_ipc_perm(&in->shm_perm, &out.shm_perm);
		out.shm_segsz	= in->shm_segsz;
		out.shm_atime	= in->shm_atime;
		out.shm_dtime	= in->shm_dtime;
		out.shm_ctime	= in->shm_ctime;
		out.shm_cpid	= in->shm_cpid;
		out.shm_lpid	= in->shm_lpid;
		out.shm_nattch	= in->shm_nattch;

		return copy_to_user(buf, &out, sizeof(out));
	    }
	default:
		return -EINVAL;
	}
}

struct shm_setbuf {
	uid_t	uid;
	gid_t	gid;
	mode_t	mode;
};	

static inline unsigned long copy_shmid_from_user(struct shm_setbuf *out, void *buf, int version)
{
	switch(version) {
	case IPC_64:
	    {
		struct shmid64_ds tbuf;

		if (copy_from_user(&tbuf, buf, sizeof(tbuf)))
			return -EFAULT;

		out->uid	= tbuf.shm_perm.uid;
		out->gid	= tbuf.shm_perm.gid;
		out->mode	= tbuf.shm_flags;

		return 0;
	    }
	case IPC_OLD:
	    {
		struct shmid_ds tbuf_old;

		if (copy_from_user(&tbuf_old, buf, sizeof(tbuf_old)))
			return -EFAULT;

		out->uid	= tbuf_old.shm_perm.uid;
		out->gid	= tbuf_old.shm_perm.gid;
		out->mode	= tbuf_old.shm_flags;

		return 0;
	    }
	default:
		return -EINVAL;
	}
}

static inline unsigned long copy_shminfo_to_user(void *buf, struct shminfo64 *in, int version)
{
	switch(version) {
	case IPC_64:
		return copy_to_user(buf, in, sizeof(*in));
	case IPC_OLD:
	    {
		struct shminfo out;

		if(in->shmmax > INT_MAX)
			out.shmmax = INT_MAX;
		else
			out.shmmax = (int)in->shmmax;

		out.shmmin	= in->shmmin;
		out.shmmni	= in->shmmni;
		out.shmseg	= in->shmseg;
		out.shmall	= in->shmall; 

		return copy_to_user(buf, &out, sizeof(out));
	    }
	default:
		return -EINVAL;
	}
}

static void shm_get_stat (unsigned long *rss, unsigned long *swp) 
{
	int i;

	*rss = 0;
	*swp = 0;

	for(i = 0; i <= shm_ids.max_id; i++) {
		struct shmid_kernel* shp;
		struct inode * inode;

		shp = shm_get(i);
		if(shp == NULL)
			continue;
		inode = shp->shm_file->f_dentry->d_inode;
		spin_lock (&inode->u.shmem_i.lock);
		*rss += inode->i_mapping->nrpages;
		*swp += inode->u.shmem_i.swapped;
		spin_unlock (&inode->u.shmem_i.lock);
	}
}

/**
 * sys_shmctl - System V共享内存控制系统调用
 * @shmid: 共享内存段id
 * @cmd: 控制命令 IPC_STAT / IPC_SET / IPC_RMID / SHM_LOCK / SHM_UNLOCK / IPC_INFO / SHM_INFO / SHM_STAT
 * @buf: 用户态shmid_ds缓冲区，用于传入或者读出信息
 * return: 成功返回非负值；失败返回负错误码
 */
asmlinkage long sys_shmctl (int shmid, int cmd, struct shmid_ds *buf)
{
    struct shm_setbuf setbuf;
    struct shmid_kernel *shp;
    int err, version;

    if (cmd < 0 || shmid < 0)
        return -EINVAL;

    // 区分老版本/新版本ipc结构体版本，适配32/64兼容
    version = ipc_parse_version(&cmd);

    switch (cmd) { /* replace with proc interface ? */
    case IPC_INFO:
    {
        struct shminfo64 shminfo;

        memset(&shminfo,0,sizeof(shminfo));
        // 填充系统全局shm上限参数
        shminfo.shmmni = shminfo.shmseg = shm_ctlmni;
        shminfo.shmmax = shm_ctlmax;
        shminfo.shmall = shm_ctlall;

        shminfo.shmmin = SHMMIN;
        // 拷贝全局shm配置信息给用户
        if(copy_shminfo_to_user (buf, &shminfo, version))
            return -EFAULT;
        /* reading a integer is always atomic */
        err= shm_ids.max_id;
        if(err<0)
            err = 0;
        return err;
    }
    case SHM_INFO:
    {
        struct shm_info shm_info;

        memset(&shm_info,0,sizeof(shm_info));
        down(&shm_ids.sem);
        shm_lockall();
        shm_info.used_ids = shm_ids.in_use;
        // 获取shm占用rss、swap统计
        shm_get_stat (&shm_info.shm_rss, &shm_info.shm_swp);
        shm_info.shm_tot = shm_tot;
        shm_info.swap_attempts = 0;
        shm_info.swap_successes = 0;
        err = shm_ids.max_id;
        shm_unlockall();
        up(&shm_ids.sem);
        if(copy_to_user (buf, &shm_info, sizeof(shm_info)))
            return -EFAULT;

        return err < 0 ? 0 : err;
    }
    case SHM_STAT:
    case IPC_STAT:
    {
        struct shmid64_ds tbuf;
        int result;
        memset(&tbuf, 0, sizeof(tbuf));
        // 根据shmid拿到内核shmid_kernel对象，持有ary自旋锁
        shp = shm_lock(shmid);
        if(shp==NULL)
            return -EINVAL;
        if(cmd==SHM_STAT) {
            err = -EINVAL;
            if (shmid > shm_ids.max_id)
                goto out_unlock;
            // SHM_STAT：按数组下标查询，返回合成shmid
            result = shm_buildid(shmid, shp->shm_perm.seq);
        } else {
            err = shm_checkid(shp,shmid);
            if(err)
                goto out_unlock;
            result = 0;
        }
        err=-EACCES;
        // 读权限校验
        if (ipcperms (&shp->shm_perm, S_IRUGO))
            goto out_unlock;
        // 内核shmid_kernel字段拷贝到临时导出结构体
        kernel_to_ipc64_perm(&shp->shm_perm, &tbuf.shm_perm);
        tbuf.shm_segsz  = shp->shm_segsz;
        tbuf.shm_atime  = shp->shm_atim;
        tbuf.shm_dtime  = shp->shm_dtim;
        tbuf.shm_ctime  = shp->shm_ctim;
        tbuf.shm_cpid   = shp->shm_cprid;
        tbuf.shm_lpid   = shp->shm_lprid;
        tbuf.shm_nattch = shp->shm_nattch;
        shm_unlock(shmid);
        // 拷贝到用户态shmid_ds
        if(copy_shmid_to_user (buf, &tbuf, version))
            return -EFAULT;
        return result;
    }
    case SHM_LOCK:
    case SHM_UNLOCK:
    {
/* Allow superuser to lock segment in memory */
/* Should the pages be faulted in here or leave it to user? */
/* need to determine interaction with current->swappable */
        // 需要CAP_IPC_LOCK权限
        if (!capable(CAP_IPC_LOCK))
            return -EPERM;

        shp = shm_lock(shmid);
        if(shp==NULL)
            return -EINVAL;
        err = shm_checkid(shp,shmid);
        if(err)
            goto out_unlock;
        if(cmd==SHM_LOCK) {
            // 标记shmem inode锁定：页面不允许被swap换出
            shp->shm_file->f_dentry->d_inode->u.shmem_i.locked = 1;
            shp->shm_flags |= SHM_LOCKED;
        } else {
            // 解除锁定，页面可以被swap
            shp->shm_file->f_dentry->d_inode->u.shmem_i.locked = 0;
            shp->shm_flags &= ~SHM_LOCKED;
        }
        shm_unlock(shmid);
        return err;
    }
    case IPC_RMID:
    {
        /*
         *  We cannot simply remove the file. The SVID states
         *  that the block remains until the last person
         *  detaches from it, then is deleted. A shmat() on
         *  an RMID segment is legal in older Linux and if 
         *  we change it apps break...
         *
         *  Instead we set a destroyed flag, and then blow
         *  the name away when the usage hits zero.
         */
        down(&shm_ids.sem);
        shp = shm_lock(shmid);
        err = -EINVAL;
        if (shp == NULL) 
            goto out_up;
        err = shm_checkid(shp, shmid);
        if (err == 0) {
            if (shp->shm_nattch){
                // 还有进程attach挂载：只打SHM_DEST标记，key置为IPC_PRIVATE，不再能通过key查到
                shp->shm_flags |= SHM_DEST;
                /* Do not find it any more */
                shp->shm_perm.key = IPC_PRIVATE;
            } else
                // 没有任何进程attach，直接销毁shm段
                shm_destroy (shp);
        }
        /* Unlock */
        shm_unlock(shmid);
        up(&shm_ids.sem);
        return err;
    }

    case IPC_SET:
    {
        // 从用户buf读取uid/gid/mode
        if(copy_shmid_from_user (&setbuf, buf, version))
            return -EFAULT;
        down(&shm_ids.sem);
        shp = shm_lock(shmid);
        err=-EINVAL;
        if(shp==NULL)
            goto out_up;
        err = shm_checkid(shp,shmid);
        if(err)
            goto out_unlock_up;
        err=-EPERM;
        // 权限校验：创建者/所有者/管理员才能修改
        if (current->euid != shp->shm_perm.uid &&
            current->euid != shp->shm_perm.cuid && 
            !capable(CAP_SYS_ADMIN)) {
            goto out_unlock_up;
        }

        // 更新所有者、组、权限，刷新修改时间shm_ctim
        shp->shm_perm.uid = setbuf.uid;
        shp->shm_perm.gid = setbuf.gid;
        shp->shm_flags = (shp->shm_flags & ~S_IRWXUGO)
            | (setbuf.mode & S_IRWXUGO);
        shp->shm_ctim = CURRENT_TIME;
        break;
    }

    default:
        return -EINVAL;
    }

    err = 0;
out_unlock_up:
    shm_unlock(shmid);
out_up:
    up(&shm_ids.sem);
    return err;
out_unlock:
    shm_unlock(shmid);
    return err;
}


/*
 * Fix shmaddr, allocate descriptor, map shm, add attach descriptor to lists.
 */
/**
 * sys_shmat - System V共享内存attach，把shm段映射到进程虚拟地址空间
 * @shmid: 共享内存id
 * @shmaddr: 用户指定映射起始虚拟地址，0由内核自动分配
 * @shmflg: 标志 SHM_RDONLY / SHM_RND
 * @raddr: 输出参数，内核把成功映射后的虚拟地址写回用户态
 * return: 成功0，失败负错误码
 */
asmlinkage long sys_shmat (int shmid, char *shmaddr, int shmflg, ulong *raddr)
{
    struct shmid_kernel *shp;
    unsigned long addr;
    struct file * file;
    int    err;
    unsigned long flags;
    unsigned long prot;
    unsigned long o_flags;
    int acc_mode;
    void *user_addr;

    if (shmid < 0)
        return -EINVAL;

    // 分支：用户传入shmaddr != 0，指定映射地址
    if ((addr = (ulong)shmaddr)) {
        // 检查是否满足SHMLBA对齐（共享内存对齐粒度）
        if (addr & (SHMLBA-1)) {
            if (shmflg & SHM_RND)
                addr &= ~(SHMLBA-1);     /* round down 向下对齐 */
            else
                // 未指定SHM_RND但地址不对齐，直接报错
                return -EINVAL;
        }
        // 用户指定地址：MAP_FIXED，强制映射到该地址
        flags = MAP_SHARED | MAP_FIXED;
    } else
        // shmaddr=0：内核自动选空闲虚拟地址，不带MAP_FIXED
        flags = MAP_SHARED;

    if (shmflg & SHM_RDONLY) {
        // 只读attach
        prot = PROT_READ;
        o_flags = O_RDONLY;
        acc_mode = S_IRUGO;
    } else {
        // 读写attach
        prot = PROT_READ | PROT_WRITE;
        o_flags = O_RDWR;
        acc_mode = S_IRUGO | S_IWUGO;
    }

    /*
     * We cannot rely on the fs check since SYSV IPC does have an
     * aditional creator id...
     */
    // 拿到shm内核对象，自旋锁保护
    shp = shm_lock(shmid);
    if(shp == NULL)
        return -EINVAL;
    // IPC权限检查，不走普通文件权限，走kern_ipc_perm
    if (ipcperms(&shp->shm_perm, acc_mode)) {
        shm_unlock(shmid);
        return -EACCES;
    }
    // 取出底层shmem匿名file（前面shmem_file_setup创建的）
    file = shp->shm_file;
    // attach计数先+1
    shp->shm_nattch++;
    shm_unlock(shmid);

    // mmap信号量，保护进程vma树
    down(&current->mm->mmap_sem);
    // 核心：do_mmap，对shmem文件建立共享映射
    user_addr = (void *) do_mmap (file, addr, file->f_dentry->d_inode->i_size, prot, flags, 0);
    up(&current->mm->mmap_sem);

    // 再拿全局shm_ids.sem，重新检查shm对象状态
    down (&shm_ids.sem);
    if(!(shp = shm_lock(shmid)))
        BUG();
    // 注意：不管do_mmap成功失败，都要把刚才+1的nattch减掉！
    shp->shm_nattch--;
    // 关键判断：如果当前nattch减到0，并且已经标记SHM_DEST（IPC_RMID打过删除标记）
    if(shp->shm_nattch == 0 &&
       shp->shm_flags & SHM_DEST)
        shm_destroy (shp);
    shm_unlock(shmid);
    up (&shm_ids.sem);

    // 把映射后的虚拟地址返回给用户
    *raddr = (unsigned long) user_addr;
    err = 0;
    if (IS_ERR(user_addr))
        err = PTR_ERR(user_addr);
    return err;
}

/*
 * detach and kill segment if marked destroyed.
 * The work is done in shm_close.
 */
asmlinkage long sys_shmdt (char *shmaddr)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *shmd, *shmdnext;

	down(&mm->mmap_sem);
	for (shmd = mm->mmap; shmd; shmd = shmdnext) {
		shmdnext = shmd->vm_next;
		if (shmd->vm_ops == &shm_vm_ops
		    && shmd->vm_start - (shmd->vm_pgoff << PAGE_SHIFT) == (ulong) shmaddr)
			do_munmap(mm, shmd->vm_start, shmd->vm_end - shmd->vm_start);
	}
	up(&mm->mmap_sem);
	return 0;
}

#ifdef CONFIG_PROC_FS
static int sysvipc_shm_read_proc(char *buffer, char **start, off_t offset, int length, int *eof, void *data)
{
	off_t pos = 0;
	off_t begin = 0;
	int i, len = 0;

	down(&shm_ids.sem);
	len += sprintf(buffer, "       key      shmid perms       size  cpid  lpid nattch   uid   gid  cuid  cgid      atime      dtime      ctime\n");

	for(i = 0; i <= shm_ids.max_id; i++) {
		struct shmid_kernel* shp;

		shp = shm_lock(i);
		if(shp!=NULL) {
#define SMALL_STRING "%10d %10d  %4o %10u %5u %5u  %5d %5u %5u %5u %5u %10lu %10lu %10lu\n"
#define BIG_STRING   "%10d %10d  %4o %21u %5u %5u  %5d %5u %5u %5u %5u %10lu %10lu %10lu\n"
			char *format;

			if (sizeof(size_t) <= sizeof(int))
				format = SMALL_STRING;
			else
				format = BIG_STRING;
			len += sprintf(buffer + len, format,
				shp->shm_perm.key,
				shm_buildid(i, shp->shm_perm.seq),
				shp->shm_flags,
				shp->shm_segsz,
				shp->shm_cprid,
				shp->shm_lprid,
				shp->shm_nattch,
				shp->shm_perm.uid,
				shp->shm_perm.gid,
				shp->shm_perm.cuid,
				shp->shm_perm.cgid,
				shp->shm_atim,
				shp->shm_dtim,
				shp->shm_ctim);
			shm_unlock(i);

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
	up(&shm_ids.sem);
	*start = buffer + (offset - begin);
	len -= (offset - begin);
	if(len > length)
		len = length;
	if(len < 0)
		len = 0;
	return len;
}
#endif
