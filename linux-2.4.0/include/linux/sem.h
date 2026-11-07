#ifndef _LINUX_SEM_H
#define _LINUX_SEM_H

#include <linux/ipc.h>

/* semop flags */
#define SEM_UNDO        0x1000  /* undo the operation on exit */

/* semctl Command Definitions. */
#define GETPID  11       /* get sempid */
#define GETVAL  12       /* get semval */
#define GETALL  13       /* get all semval's */
#define GETNCNT 14       /* get semncnt */
#define GETZCNT 15       /* get semzcnt */
#define SETVAL  16       /* set semval */
#define SETALL  17       /* set all semval's */

/* ipcs ctl cmds */
#define SEM_STAT 18
#define SEM_INFO 19

/* Obsolete, used only for backwards compatibility and libc5 compiles */
struct semid_ds {
	struct ipc_perm	sem_perm;		/* permissions .. see ipc.h */
	__kernel_time_t	sem_otime;		/* last semop time */
	__kernel_time_t	sem_ctime;		/* last change time */
	struct sem	*sem_base;		/* ptr to first semaphore in array */
	struct sem_queue *sem_pending;		/* pending operations to be processed */
	struct sem_queue **sem_pending_last;	/* last pending operation */
	struct sem_undo	*undo;			/* undo requests on this array */
	unsigned short	sem_nsems;		/* no. of semaphores in array */
};

/* Include the definition of semid64_ds */
#include <asm/sembuf.h>

/* semop system calls takes an array of these. */
/**
 * struct sembuf - System V信号量操作描述结构体
 * 供semop系统调用使用，描述对信号量集合里单个信号量的一次操作
 */
struct sembuf {
    // 信号量在信号量数组中的下标
    unsigned short  sem_num;    /* semaphore index in array */
    // 信号量操作值
    short           sem_op;     /* semaphore operation */
    // 操作标志
    short           sem_flg;    /* operation flags */
};

/* arg for semctl system calls. */
union semun {
	int val;			/* value for SETVAL */
	struct semid_ds *buf;		/* buffer for IPC_STAT & IPC_SET */
	unsigned short *array;		/* array for GETALL & SETALL */
	struct seminfo *__buf;		/* buffer for IPC_INFO */
	void *__pad;
};

struct  seminfo {
	int semmap;
	int semmni;
	int semmns;
	int semmnu;
	int semmsl;
	int semopm;
	int semume;
	int semusz;
	int semvmx;
	int semaem;
};

#define SEMMNI  128             /* <= IPCMNI  max # of semaphore identifiers */
#define SEMMSL  250             /* <= 8 000 max num of semaphores per id */
#define SEMMNS  (SEMMNI*SEMMSL) /* <= INT_MAX max # of semaphores in system */
#define SEMOPM  32	        /* <= 1 000 max num of ops per semop call */
#define SEMVMX  32767           /* <= 32767 semaphore maximum value */

/* unused */
#define SEMUME  SEMOPM          /* max num of undo entries per process */
#define SEMMNU  SEMMNS          /* num of undo structures system wide */
#define SEMAEM  (SEMVMX >> 1)   /* adjust on exit max value */
#define SEMMAP  SEMMNS          /* # of entries in semaphore map */
#define SEMUSZ  20		/* sizeof struct sem_undo */

#ifdef __KERNEL__

/* One semaphore structure for each semaphore in the system. */
/**
 * struct sem - 信号量集合里面单个信号量条目
 */
struct sem {
    // 信号量当前计数值
    int semval;     /* current value */
    // 最后一次操作该信号量的进程PID
    int sempid;     /* pid of last operation */
};


/* One sem_array data structure for each set of semaphores in the system. */
/**
 * struct sem_array - System V信号量集合内核管理对象
 * 一个sem_array代表一整个信号量集，包含多个独立struct sem
 */
struct sem_array {
    // IPC通用权限结构，包含key、uid、gid、mode、seq序列号
    struct kern_ipc_perm    sem_perm;    /* permissions .. see ipc.h */
    // 上一次semop操作成功的时间，o=operation
    time_t                  sem_otime;   /* last semop time */
    // 元数据修改时间，创建、IPC_SET、SETVAL、SETALL时更新，c=change
    time_t                  sem_ctime;   /* last change time */
    // 指向信号量数组首地址，数组紧跟sem_array之后分配
    struct sem             *sem_base;    /* ptr to first semaphore in array */
    // 阻塞等待链表头，存放因条件不满足休眠的semop任务
    struct sem_queue       *sem_pending; /* pending operations to be processed */
    // 等待链表尾二级指针，用于快速尾插，避免遍历链表找尾部
    struct sem_queue      **sem_pending_last; /* last pending operation */
    // sem_undo链表头，记录绑定该信号量集的所有进程undo回滚项
    struct sem_undo        *undo;        /* undo requests on this array */
    // 信号量集合内信号量的个数，集合创建后固定不可修改
    unsigned long           sem_nsems;   /* no. of semaphores in array */
};


/* One queue for each sleeping process in the system. */
/**
 * struct sem_queue - 封装一条阻塞的semop操作节点，挂载到sem_array的pending双向链表
 */
struct sem_queue {
    // 链表下一个节点
    struct sem_queue *  next;    /* next entry in the queue */
    // 前驱二级指针，*prev 指向自身，方便链表删除操作
    struct sem_queue ** prev;    /* previous entry in the queue, *(q->prev) == q */
    // 阻塞等待的进程
    struct task_struct* sleeper; /* this process */
    // 本次操作对应的undo回滚结构
    struct sem_undo *   undo;    /* undo structure */
    // 发起semop的进程pid
    int                 pid;     /* process id of requesting process */
    // 操作状态：成功、被信号中断、被删除(-EIDRM)
    int                 status;  /* completion status of operation */
    // 归属的信号量集合
    struct sem_array *  sma;     /* semaphore array for operations */
    // 信号量集对外id
    int                 id;      /* internal sem id */
    // 本次待执行的sembuf操作数组
    struct sembuf *     sops;    /* array of pending operations */
    // 本次semop包含多少条操作
    int                 nsops;   /* number of operations */
    // 标记：本次操作是否会修改信号量的值（sem_op>0 / sem_op<0）
    int                 alter;   /* operation will alter semaphore */
};

/* Each task has a list of undo requests. They are executed automatically
 * when the process exits.
 */
/**
 * struct sem_undo - SEM_UNDO 机制的回滚记录
 * 每条记录关联一个进程 + 一个信号量集，进程退出时自动回滚信号量
 */
struct sem_undo {
    // 同一进程下，下一条undo记录（进程维度链表）
    struct sem_undo *   proc_next;  /* next entry on this process */
    // 同一信号量集下，下一条undo记录（信号量集维度链表）
    struct sem_undo *   id_next;    /* next entry on this semaphore set */
    // 对应的信号量集semid
    int                 semid;      /* semaphore set identifier */
    // 调整值数组：保存需要回滚的增量，每个信号量对应一个semadj
    short *             semadj;     /* array of adjustments, one per semaphore */
};


asmlinkage long sys_semget (key_t key, int nsems, int semflg);
asmlinkage long sys_semop (int semid, struct sembuf *sops, unsigned nsops);
asmlinkage long sys_semctl (int semid, int semnum, int cmd, union semun arg);

#endif /* __KERNEL__ */

#endif /* _LINUX_SEM_H */
