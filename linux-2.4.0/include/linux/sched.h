#ifndef _LINUX_SCHED_H
#define _LINUX_SCHED_H

#include <asm/param.h>	/* for HZ */

extern unsigned long event;

#include <linux/config.h>
#include <linux/binfmts.h>
#include <linux/personality.h>
#include <linux/threads.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/times.h>
#include <linux/timex.h>

#include <asm/system.h>
#include <asm/semaphore.h>
#include <asm/page.h>
#include <asm/ptrace.h>
#include <asm/mmu.h>

#include <linux/smp.h>
#include <linux/tty.h>
#include <linux/sem.h>
#include <linux/signal.h>
#include <linux/securebits.h>
#include <linux/fs_struct.h>

/*
 * cloning flags:
 */
#define CSIGNAL		0x000000ff	/* signal mask to be sent at exit */
#define CLONE_VM	0x00000100	/* set if VM shared between processes */
#define CLONE_FS	0x00000200	/* set if fs info shared between processes */
#define CLONE_FILES	0x00000400	/* set if open files shared between processes */
#define CLONE_SIGHAND	0x00000800	/* set if signal handlers and blocked signals shared */
#define CLONE_PID	0x00001000	/* set if pid shared */
#define CLONE_PTRACE	0x00002000	/* set if we want to let tracing continue on the child too */
#define CLONE_VFORK	0x00004000	/* set if the parent wants the child to wake it up on mm_release */
#define CLONE_PARENT	0x00008000	/* set if we want to have the same parent as the cloner */
#define CLONE_THREAD	0x00010000	/* Same thread group? */

#define CLONE_SIGNAL	(CLONE_SIGHAND | CLONE_THREAD)

/*
 * These are the constant used to fake the fixed-point load-average
 * counting. Some notes:
 *  - 11 bit fractions expand to 22 bits by the multiplies: this gives
 *    a load-average precision of 10 bits integer + 11 bits fractional
 *  - if you want to count load-averages more often, you need more
 *    precision, or rounding will get you. With 2-second counting freq,
 *    the EXP_n values would be 1981, 2034 and 2043 if still using only
 *    11 bit fractions.
 */
extern unsigned long avenrun[];		/* Load averages */

#define FSHIFT		11		/* nr of bits of precision */
#define FIXED_1		(1<<FSHIFT)	/* 1.0 as fixed-point */
#define LOAD_FREQ	(5*HZ)		/* 5 sec intervals */
#define EXP_1		1884		/* 1/exp(5sec/1min) as fixed-point */
#define EXP_5		2014		/* 1/exp(5sec/5min) */
#define EXP_15		2037		/* 1/exp(5sec/15min) */

#define CALC_LOAD(load,exp,n) \
	load *= exp; \
	load += n*(FIXED_1-exp); \
	load >>= FSHIFT;

#define CT_TO_SECS(x)	((x) / HZ)
#define CT_TO_USECS(x)	(((x) % HZ) * 1000000/HZ)

extern int nr_running, nr_threads;
extern int last_pid;

#include <linux/fs.h>
#include <linux/time.h>
#include <linux/param.h>
#include <linux/resource.h>
#include <linux/timer.h>

#include <asm/processor.h>

#define TASK_RUNNING		0
#define TASK_INTERRUPTIBLE	1
#define TASK_UNINTERRUPTIBLE	2
#define TASK_ZOMBIE		4
#define TASK_STOPPED		8

#define __set_task_state(tsk, state_value)		\
	do { (tsk)->state = (state_value); } while (0)
#ifdef CONFIG_SMP
#define set_task_state(tsk, state_value)		\
	set_mb((tsk)->state, (state_value))
#else
#define set_task_state(tsk, state_value)		\
	__set_task_state((tsk), (state_value))
#endif

#define __set_current_state(state_value)			\
	do { current->state = (state_value); } while (0)
#ifdef CONFIG_SMP
#define set_current_state(state_value)		\
	set_mb(current->state, (state_value))
#else
#define set_current_state(state_value)		\
	__set_current_state(state_value)
#endif

/*
 * Scheduling policies
 */
#define SCHED_OTHER		0
#define SCHED_FIFO		1
#define SCHED_RR		2

/*
 * This is an additional bit set when we want to
 * yield the CPU for one re-schedule..
 */
#define SCHED_YIELD		0x10

struct sched_param {
	int sched_priority;
};

#ifdef __KERNEL__

#include <linux/spinlock.h>

/*
 * This serializes "schedule()" and also protects
 * the run-queue from deletions/modifications (but
 * _adding_ to the beginning of the run-queue has
 * a separate lock).
 */
extern rwlock_t tasklist_lock;
extern spinlock_t runqueue_lock;
extern spinlock_t mmlist_lock;

extern void sched_init(void);
extern void init_idle(void);
extern void show_state(void);
extern void cpu_init (void);
extern void trap_init(void);
extern void update_process_times(int user);
extern void update_one_process(struct task_struct *p, unsigned long user,
			       unsigned long system, int cpu);

#define	MAX_SCHEDULE_TIMEOUT	LONG_MAX
extern signed long FASTCALL(schedule_timeout(signed long timeout));
asmlinkage void schedule(void);

extern int schedule_task(struct tq_struct *task);
extern void flush_scheduled_tasks(void);
extern int start_context_thread(void);
extern int current_is_keventd(void);

/*
 * The default fd array needs to be at least BITS_PER_LONG,
 * as this is the granularity returned by copy_fdset().
 */
#define NR_OPEN_DEFAULT BITS_PER_LONG

/*
 * Open file table structure
 */
struct files_struct {
	// 原子引用计数，多个进程CLONE_FILES共享本结构体时使用，count减到0释放内存
	atomic_t count;  

	// 读写锁，保护fd数组、fd位图的并发访问，防止竞态
	rwlock_t file_lock;

	// 当前fd数组的容量上限，fd编号最大可取 max_fds - 1
	int max_fds;

	// fd_set位图(open_fds/close_on_exec)能管理的最大fd数量
	int max_fdset;

	// 分配fd的扫描起点，get_unused_fd从该位置向后找空闲fd，优化查找效率
	int next_fd;

	// fd指针数组：fd[fd号] 指向struct file；初期指向内置fd_array，超量后指向堆分配大数组
	struct file ** fd;	/* current fd array */

	// 位图：标记哪些fd带有FD_CLOEXEC，exec加载新程序时自动关闭
	fd_set *close_on_exec;

	// 位图：标记当前哪些fd是已打开有效的文件描述符
	fd_set *open_fds;

	// 静态内置close_on_exec位图，少量fd场景直接使用，避免堆分配
	fd_set close_on_exec_init;

	// 静态内置open_fds位图，少量fd场景直接使用，避免堆分配
	fd_set open_fds_init;

	// 内置静态小数组，NR_OPEN_DEFAULT默认32；打开文件少时直接复用，减少malloc开销
	struct file * fd_array[NR_OPEN_DEFAULT];
};

#define INIT_FILES \
{ 							\
	count:		ATOMIC_INIT(1), 		\
	file_lock:	RW_LOCK_UNLOCKED, 		\
	max_fds:	NR_OPEN_DEFAULT, 		\
	max_fdset:	__FD_SETSIZE, 			\
	next_fd:	0, 				\
	fd:		&init_files.fd_array[0], 	\
	close_on_exec:	&init_files.close_on_exec_init, \
	open_fds:	&init_files.open_fds_init, 	\
	close_on_exec_init: { { 0, } }, 		\
	open_fds_init:	{ { 0, } }, 			\
	fd_array:	{ NULL, } 			\
}

/* Maximum number of active map areas.. This is a random (large) number */
#define MAX_MAP_COUNT	(65536)

/* Number of map areas at which the AVL tree is activated. This is arbitrary. */
#define AVL_MIN_MAP_COUNT	32

struct mm_struct {
	struct vm_area_struct * mmap;		/* list of VMAs */
	struct vm_area_struct * mmap_avl;	/* tree of VMAs */
	struct vm_area_struct * mmap_cache;	/* last find_vma result */
	pgd_t * pgd;
	atomic_t mm_users;			/* How many users with user space? */
	atomic_t mm_count;			/* How many references to "struct mm_struct" (users count as 1) */
	int map_count;				/* number of VMAs */
	struct semaphore mmap_sem;
	spinlock_t page_table_lock;

	struct list_head mmlist;		/* List of all active mm's */

	unsigned long start_code, end_code, start_data, end_data;
	unsigned long start_brk, brk, start_stack;
	unsigned long arg_start, arg_end, env_start, env_end;
	unsigned long rss, total_vm, locked_vm;
	unsigned long def_flags;
	unsigned long cpu_vm_mask;
	unsigned long swap_cnt;	/* number of pages to swap on next pass */
	unsigned long swap_address;

	/* Architecture-specific MM context */
	mm_context_t context;
};

#define INIT_MM(name) \
{			 				\
	mmap:		&init_mmap, 			\
	mmap_avl:	NULL, 				\
	mmap_cache:	NULL, 				\
	pgd:		swapper_pg_dir, 		\
	mm_users:	ATOMIC_INIT(2), 		\
	mm_count:	ATOMIC_INIT(1), 		\
	map_count:	1, 				\
	mmap_sem:	__MUTEX_INITIALIZER(name.mmap_sem), \
	page_table_lock: SPIN_LOCK_UNLOCKED, 		\
	mmlist:		LIST_HEAD_INIT(name.mmlist),	\
}

/**
 * struct signal_struct - 进程信号描述符，属于进程组共享的信号结构
 * @count: 引用计数，多个task_struct共享同一个signal_struct时使用(线程组)
 * @action[_NSIG]: 每个信号对应的处理动作数组，下标0对应信号1(SIGHUP)，sig-1索引
 *                数组元素为k_sigaction，包含sa_handler、sa_mask、sa_flags等
 * @siglock: 自旋锁，保护action数组并发读写，修改/读取信号handler时必须持有
 *
 * 说明：
 * 线程组内所有线程共用一份signal_struct，保存整个线程组的信号处理行为；
 * 而阻塞信号掩码sigmask保存在task_struct，是每个线程私有的。
 */
struct signal_struct {
	// 引用计数，线程组共享，clone时增加，exit时减少
	atomic_t		count;

	// 信号处理动作表，_NSIG是最大信号数量
	struct k_sigaction	action[_NSIG];

	// 自旋锁，保护action数组的并发访问
	spinlock_t		siglock;
};


#define INIT_SIGNALS {	\
	count:		ATOMIC_INIT(1), 		\
	action:		{ {{0,}}, }, 			\
	siglock:	SPIN_LOCK_UNLOCKED 		\
}

/*
 * Some day this will be a full-fledged user tracking system..
 */
/**
 * struct user_struct - 用户资源配额结构体，Linux2.4内核
 * 每个UID对应一个user_struct，记录该用户整体的资源统计与引用计数，用于资源限制RLIMIT
 * 当多个task属于同一个uid时，共享同一个user_struct实例
 * 
 * *xecve 执行程序，进程 UID 发生切换
* 执行 setuid 程序、普通程序，内核更新 task_struct 的 uid/euid/fsuid，需要获取对应 uid 的 user_struct。
 */
struct user_struct {
	// 引用计数，原子变量。有多少task/内核对象引用这个user_struct；
	// 当__count减到0，说明无任何引用，释放该user_struct内存
	atomic_t __count;	/* reference count */

	// 原子计数，该用户下当前存在的进程/任务总数；
	// 用于RLIMIT_NPROC限制：用户最大允许创建的进程数量，do_fork会校验这个值
	atomic_t processes;	/* How many processes does this user have? */

	// 原子计数，该用户所有进程累计打开的文件句柄总数；
	// 用于资源限制，统计该uid全局打开文件数目
	atomic_t files;		/* How many open files does this user have? */

	/* Hash table maintenance information */

	// 哈希链表后向指针，user_struct挂在uid哈希表中，冲突链表的next节点
	//  哈希链表前向二级指针，用于高效链表删除操作（内核经典hlist技巧）
	struct user_struct *next, **pprev;

	// 用户ID，这个user_struct对应的uid
	uid_t uid;
};

#define get_current_user() ({ 				\
	struct user_struct *__user = current->user;	\
	atomic_inc(&__user->__count);			\
	__user; })

extern struct user_struct root_user;
#define INIT_USER (&root_user)

/**
 * struct task_struct - 任务描述符，Linux 2.4 内核，代表一个任务（进程 / 轻量线程）
 * 内核里所有进程、线程都用这个结构体描述；do_fork 分配此结构，*p=*current 浅拷贝父任务。
 * 32位x86上部分成员偏移是硬编码给汇编使用，修改字段位置要小心。
 */
struct task_struct {
	/*
	 * offsets of these are hardcoded elsewhere - touch with care
	 */
	// 任务运行状态：-1不可运行，0就绪，>0停止
	volatile long state;	/* -1 unrunnable, 0 runnable, >0 stopped */

	// 进程标志位，PF_*系列标记
	unsigned long flags;	/* per process flags, defined below */

	// 是否存在待处理信号
	int sigpending;

	// 地址空间访问边界；用户线程0~0xBFFFFFFF，内核线程0~0xFFFFFFFF
	mm_segment_t addr_limit;	/* thread address space:
					 	0-0xBFFFFFFF for user-thead
						0-0xFFFFFFFF for kernel-thread
					 */
	// 执行域，用于兼容其他Unix的ABI
	struct exec_domain *exec_domain;

	// 需要调度标记，置1表示退出内核后要切换进程
	volatile long need_resched;

	// ptrace调试标记，gdb/strace使用
	unsigned long ptrace;

	// 大内核锁BKL的嵌套深度
	int lock_depth;		/* Lock depth */

/*
 * offset 32 begins here on 32-bit platforms. We keep
 * all fields in a single cacheline that are needed for
 * the goodness() loop in schedule().
 */
	// 时间片剩余计数，2.4调度器核心变量
	long counter;

	// nice静态优先级，-20~19，数值越大优先级越低
	long nice;

	// 调度策略：SCHED_OTHER / SCHED_FIFO / SCHED_RR
	unsigned long policy;

	// 进程地址空间；内核线程mm=NULL
	struct mm_struct *mm;

	// has_cpu：是否正在占有CPU；processor：当前所在CPU编号
	int has_cpu, processor;

	// CPU掩码，限制任务能够调度运行在哪些CPU
	unsigned long cpus_allowed;
	/*
	 * (only the 'next' pointer fits into the cacheline, but
	 * that's just fine.)
	 */
	// 就绪队列链表节点，就绪任务挂入对应CPU运行队列
	struct list_head run_list;

	// 任务进入睡眠的时间
	unsigned long sleep_time;

	// 全局任务双向链表，for_each_task遍历所有进程使用
	struct task_struct *next_task, *prev_task;

	// 当前正在使用的地址空间，内核线程会借用用户进程active_mm
	struct mm_struct *active_mm;

/* task state */
	// 二进制文件加载器，负责识别加载ELF/a.out等可执行程序
	struct linux_binfmt *binfmt;

	// exit_code退出返回码；exit_signal进程死亡时发给父进程的信号
	int exit_code, exit_signal;

	// 父进程消亡时发送给当前任务的信号
	int pdeath_signal;  /*  The signal sent when the parent dies  */
	/* ??? */

	// personality个性编号，用来匹配对应的exec_domain执行域
	unsigned long personality;

	// 是否允许生成core dump核心转储文件
	int dumpable:1;

	// 是否执行过exec；fork创建初始为0，exec成功后置1
	int did_exec:1;

	// TID，任务ID，每个task独立ID，线程也拥有自己的pid
	pid_t pid;

	// 进程组ID
	pid_t pgrp;

	// 终端关联的旧进程组
	pid_t tty_old_pgrp;

	// 会话ID
	pid_t session;

	// 线程组ID，同组线程tgid相同，就是用户态看到的PID
	pid_t tgid;
	/* boolean value for session group leader */

	// 是否是会话/线程组leader
	int leader;
	/* 
	 * pointers to (original) parent process, youngest child, younger sibling,
	 * older sibling, respectively.  (p->father can be replaced with 
	 * p->p_pptr->pid)
	 */
	// 家族指针：原父进程、当前父进程、子进程、年轻兄弟、年长兄弟
	// p_opptr = Original Parent Pointer，原始父进程指针
	// p_pptr = Parent Pointer，当前父进程指针（接收 SIGCHLD、wait4 的父）
	// p_cptr = Child Pointer， youngest child，最年幼子进程（最近创建的子）
	// p_ysptr = Younger Sibling Pointer，弟弟 / 弟兄弟指针（后出生的兄弟）
	// p_osptr = Older Sibling Pointer，哥哥 / 兄兄弟指针（先出生的兄弟）
	// 没有子进程链表，通过这些关系找到
	struct task_struct *p_opptr, *p_pptr, *p_cptr, *p_ysptr, *p_osptr;

	// 线程组链表，同一线程组所有任务串在一起
	struct list_head thread_group;

	/* PID hash table linkage. */
	// PID哈希表冲突链表后继指针
	struct task_struct *pidhash_next;

	// PID哈希表二级前驱指针，用于O(1)删除节点
	struct task_struct **pidhash_pprev;

	// wait4等待子进程退出的等待队列
	wait_queue_head_t wait_chldexit;	/* for wait4() */

	// vfork专用信号量，vfork子进程exec前阻塞父进程
	struct semaphore *vfork_sem;		/* for vfork() */

	// 实时任务优先级
	unsigned long rt_priority;

	// ITIMER_REAL定时器剩余值；ITIMER_PROF；ITIMER_VIRT
	// it_real_value - ITIMER_REAL：真实时间定时器剩余滴答数，墙上时钟，进程休眠也计时，超时发SIGALRM
	// it_prof_value - ITIMER_PROF：profiling性能统计定时器，进程用户态+内核态CPU耗时，超时发SIGPROF
	// it_virt_value - ITIMER_VIRTUAL：虚拟时间定时器，仅进程在用户态运行时才计时，超时发SIGVTALRM
	unsigned long it_real_value, it_prof_value, it_virt_value;

	// 三个定时器的间隔周期
	// it_real_incr：真实时间定时器周期，alarm 设置周期性闹钟时使用，超时发 SIGALRM
	// it_prof_incr：性能剖析定时器周期，用户 + 内核 CPU 时间都计数，超时发 SIGPROF
	// it_virt_incr：虚拟定时器周期，只在进程用户态运行时计时，超时发 SIGVTALRM
	unsigned long it_real_incr, it_prof_incr, it_virt_incr;

	// ITIMER_REAL真实时间定时器结构体
	struct timer_list real_timer;

	// 进程CPU时间统计（用户态、内核态、子进程时间）
	struct tms times;

	// 进程创建时刻
	unsigned long start_time;

	// 分CPU统计用户态CPU耗时、内核态CPU耗时
	long per_cpu_utime[NR_CPUS], per_cpu_stime[NR_CPUS];
/* mm fault and swap info: this can arguably be seen as either mm-specific or thread-specific */

	// 缺页、swap统计信息
    // min_flt轻度缺页，maj_flt重度缺页，nswap换出页数；c开头为子进程累计统计
	unsigned long min_flt, maj_flt, nswap, cmin_flt, cmaj_flt, cnswap;
	
	// 进程内存是否允许换出到swap分区
	int swappable:1;
/* process credentials */
	// uid - 真实用户ID：进程最初由哪个用户启动，身份本源，用于记账、信号权限判断
    // euid - 有效用户ID：绝大多数系统权限检查使用euid；setuid程序执行会修改euid
    // suid - 保存用户ID：它解决的是一个问题：程序临时放弃特权后，还能不能恢复原来的特权身份
    // fsuid - 文件系统UID：VFS文件操作权限校验专用ID，chmod/chown/open等文件相关鉴权看fsuid
	uid_t uid,euid,suid,fsuid;

	// gid - 真实组ID：进程启动者所属原始用户组
    // egid - 有效组ID：组权限校验使用egid，用于文件group权限(rwx)判断
    // sgid - 保存组ID：保存的组身份，可用于恢复有效组身份
    // fsgid - 文件系统GID：文件系统操作鉴权使用的组ID，配合fsuid完成inode权限检查
	gid_t gid,egid,sgid,fsgid;

	// 附属用户组数量
	int ngroups;

	// 附属组ID数组
	gid_t	groups[NGROUPS];

	// 能力集：有效能力、可继承能力、允许能力
	kernel_cap_t   cap_effective, cap_inheritable, cap_permitted;

	// exec执行后是否保留capabilities能力
	int keep_capabilities:1;

	//用户资源信息
	struct user_struct *user;

	//资源限制信息
	struct rlimit rlim[RLIM_NLIMITS];

	// 是否使用浮点运算单元
	unsigned short used_math;

	// 进程名称，最多15字符+字符串结束符
	char comm[16];
/* file system info */

	// 文件系统相关
    // 文件引用计数
	int link_count;

	// 关联终端，无终端则为NULL
	struct tty_struct *tty; /* NULL if no tty */

	// 当前持有的文件锁数量
	unsigned int locks; /* How many file locks are being held */
/* ipc stuff */
	// System V信号量撤销记录
	struct sem_undo *semundo;
	// 信号量等待队列
	struct sem_queue *semsleeping;
/* CPU-specific state of this task */

	// 线程底层硬件上下文（寄存器、栈等架构相关信息）
	struct thread_struct thread;

	//文件系统统计信息
	struct fs_struct *fs;

	//进程文件描述符表
	struct files_struct *files;
/* signal handlers */
	// 保护信号与阻塞掩码的自旋锁
	spinlock_t sigmask_lock;	/* Protects signal and blocked */
	// 信号处理函数集合
	struct signal_struct *sig;

	// 阻塞信号掩码
	sigset_t blocked;

	// 待投递信号链表
	struct sigpending pending;

	// 信号备用栈起始地址
	unsigned long sas_ss_sp;

	// 信号备用栈大小
	size_t sas_ss_size;

	// 通知回调函数
	int (*notifier)(void *priv);

	// 通知回调私有数据
	void *notifier_data;

	// 触发通知的信号掩码
	sigset_t *notifier_mask;
	
/* Thread group tracking */
	// 父进程exec标识
   	u32 parent_exec_id;

	// 当前进程exec标识
   	u32 self_exec_id;
/* Protection of (de-)allocation: mm, files, fs, tty */

	// 保护mm/files/fs/tty分配释放的自旋锁
	spinlock_t alloc_lock;
};

/*
 * Per process flags
 */
#define PF_ALIGNWARN	0x00000001	/* Print alignment warning msgs */
					/* Not implemented yet, only for 486*/
#define PF_STARTING	0x00000002	/* being created */
#define PF_EXITING	0x00000004	/* getting shut down */
#define PF_FORKNOEXEC	0x00000040	/* forked but didn't exec */
#define PF_SUPERPRIV	0x00000100	/* used super-user privileges */
#define PF_DUMPCORE	0x00000200	/* dumped core */
#define PF_SIGNALED	0x00000400	/* killed by a signal */
#define PF_MEMALLOC	0x00000800	/* Allocating memory */
#define PF_VFORK	0x00001000	/* Wake up parent in mm_release */

#define PF_USEDFPU	0x00100000	/* task used FPU this quantum (SMP) */

/*
 * Ptrace flags
 */

#define PT_PTRACED	0x00000001
#define PT_TRACESYS	0x00000002
#define PT_DTRACE	0x00000004	/* delayed trace (used on m68k, i386) */
#define PT_TRACESYSGOOD	0x00000008

/*
 * Limit the stack by to some sane default: root can always
 * increase this limit if needed..  8MB seems reasonable.
 */
#define _STK_LIM	(8*1024*1024)

#define DEF_COUNTER	(10*HZ/100)	/* 100 ms time slice */
#define MAX_COUNTER	(20*HZ/100)
#define DEF_NICE	(0)

/*
 *  INIT_TASK is used to set up the first task table, touch at
 * your own risk!. Base=0, limit=0x1fffff (=2MB)
 */
#define INIT_TASK(tsk)	\
{									\
    state:		0,						\
    flags:		0,						\
    sigpending:		0,						\
    addr_limit:		KERNEL_DS,					\
    exec_domain:	&default_exec_domain,				\
    lock_depth:		-1,						\
    counter:		DEF_COUNTER,					\
    nice:		DEF_NICE,					\
    policy:		SCHED_OTHER,					\
    mm:			NULL,						\
    active_mm:		&init_mm,					\
    cpus_allowed:	-1,						\
    run_list:		LIST_HEAD_INIT(tsk.run_list),			\
    next_task:		&tsk,						\
    prev_task:		&tsk,						\
    p_opptr:		&tsk,						\
    p_pptr:		&tsk,						\
    thread_group:	LIST_HEAD_INIT(tsk.thread_group),		\
    wait_chldexit:	__WAIT_QUEUE_HEAD_INITIALIZER(tsk.wait_chldexit),\
    real_timer:		{						\
	function:		it_real_fn				\
    },									\
    cap_effective:	CAP_INIT_EFF_SET,				\
    cap_inheritable:	CAP_INIT_INH_SET,				\
    cap_permitted:	CAP_FULL_SET,					\
    keep_capabilities:	0,						\
    rlim:		INIT_RLIMITS,					\
    user:		INIT_USER,					\
    comm:		"swapper",					\
    thread:		INIT_THREAD,					\
    fs:			&init_fs,					\
    files:		&init_files,					\
    sigmask_lock:	SPIN_LOCK_UNLOCKED,				\
    sig:		&init_signals,					\
    pending:		{ NULL, &tsk.pending.head, {{0}}},		\
    blocked:		{{0}},						\
    alloc_lock:		SPIN_LOCK_UNLOCKED				\
}


#ifndef INIT_TASK_SIZE
# define INIT_TASK_SIZE	2048*sizeof(long)
#endif

union task_union {
	struct task_struct task;
	unsigned long stack[INIT_TASK_SIZE/sizeof(long)];
};

extern union task_union init_task_union;

extern struct   mm_struct init_mm;
extern struct task_struct *init_tasks[NR_CPUS];

/* PID hashing. (shouldnt this be dynamic?) */
#define PIDHASH_SZ (4096 >> 2)
extern struct task_struct *pidhash[PIDHASH_SZ];

#define pid_hashfn(x)	((((x) >> 8) ^ (x)) & (PIDHASH_SZ - 1))

static inline void hash_pid(struct task_struct *p)
{
	// 根据pid算出哈希下标，取pidhash数组对应槽位的指针的地址
	struct task_struct **htable = &pidhash[pid_hashfn(p->pid)];

	// 把当前槽位原来的链表头，赋值给p的pidhash_next
	if((p->pidhash_next = *htable) != NULL)
		// 如果原来链表不为空，旧头结点的前驱指针指向p的pidhash_next
		(*htable)->pidhash_pprev = &p->pidhash_next;
	
	// 哈希槽头指针指向新进程p（头插法）
	*htable = p;
	// p的前驱指针，直接指向哈希数组槽位本身
	p->pidhash_pprev = htable;
}

static inline void unhash_pid(struct task_struct *p)
{
	if(p->pidhash_next)
		p->pidhash_next->pidhash_pprev = p->pidhash_pprev;
	*p->pidhash_pprev = p->pidhash_next;
}

static inline struct task_struct *find_task_by_pid(int pid)
{
	struct task_struct *p, **htable = &pidhash[pid_hashfn(pid)];

	for(p = *htable; p && p->pid != pid; p = p->pidhash_next)
		;

	return p;
}

/* per-UID process charging. */
extern struct user_struct * alloc_uid(uid_t);
extern void free_uid(struct user_struct *);

#include <asm/current.h>

extern unsigned long volatile jiffies;
extern unsigned long itimer_ticks;
extern unsigned long itimer_next;
extern struct timeval xtime;
extern void do_timer(struct pt_regs *);

extern unsigned int * prof_buffer;
extern unsigned long prof_len;
extern unsigned long prof_shift;

#define CURRENT_TIME (xtime.tv_sec)

extern void FASTCALL(__wake_up(wait_queue_head_t *q, unsigned int mode, unsigned int wq_mode));
extern void FASTCALL(__wake_up_sync(wait_queue_head_t *q, unsigned int mode, unsigned int wq_mode));
extern void FASTCALL(sleep_on(wait_queue_head_t *q));
extern long FASTCALL(sleep_on_timeout(wait_queue_head_t *q,
				      signed long timeout));
extern void FASTCALL(interruptible_sleep_on(wait_queue_head_t *q));
extern long FASTCALL(interruptible_sleep_on_timeout(wait_queue_head_t *q,
						    signed long timeout));
extern void FASTCALL(wake_up_process(struct task_struct * tsk));

#define wake_up(x)			__wake_up((x),TASK_UNINTERRUPTIBLE | TASK_INTERRUPTIBLE,WQ_FLAG_EXCLUSIVE)
#define wake_up_all(x)			__wake_up((x),TASK_UNINTERRUPTIBLE | TASK_INTERRUPTIBLE,0)
#define wake_up_sync(x)			__wake_up_sync((x),TASK_UNINTERRUPTIBLE | TASK_INTERRUPTIBLE,WQ_FLAG_EXCLUSIVE)
#define wake_up_interruptible(x)	__wake_up((x),TASK_INTERRUPTIBLE,WQ_FLAG_EXCLUSIVE)
#define wake_up_interruptible_all(x)	__wake_up((x),TASK_INTERRUPTIBLE,0)
#define wake_up_interruptible_sync(x)	__wake_up_sync((x),TASK_INTERRUPTIBLE,WQ_FLAG_EXCLUSIVE)

extern int in_group_p(gid_t);
extern int in_egroup_p(gid_t);

extern void proc_caches_init(void);
extern void flush_signals(struct task_struct *);
extern void flush_signal_handlers(struct task_struct *);
extern int dequeue_signal(sigset_t *, siginfo_t *);
extern void block_all_signals(int (*notifier)(void *priv), void *priv,
			      sigset_t *mask);
extern void unblock_all_signals(void);
extern int send_sig_info(int, struct siginfo *, struct task_struct *);
extern int force_sig_info(int, struct siginfo *, struct task_struct *);
extern int kill_pg_info(int, struct siginfo *, pid_t);
extern int kill_sl_info(int, struct siginfo *, pid_t);
extern int kill_proc_info(int, struct siginfo *, pid_t);
extern void notify_parent(struct task_struct *, int);
extern void do_notify_parent(struct task_struct *, int);
extern void force_sig(int, struct task_struct *);
extern int send_sig(int, struct task_struct *, int);
extern int kill_pg(pid_t, int, int);
extern int kill_sl(pid_t, int, int);
extern int kill_proc(pid_t, int, int);
extern int do_sigaction(int, const struct k_sigaction *, struct k_sigaction *);
extern int do_sigaltstack(const stack_t *, stack_t *, unsigned long);

static inline int signal_pending(struct task_struct *p)
{
	return (p->sigpending != 0);
}

/*
 * Re-calculate pending state from the set of locally pending
 * signals, globally pending signals, and blocked signals.
 */
static inline int has_pending_signals(sigset_t *signal, sigset_t *blocked)
{
	unsigned long ready;
	long i;

	switch (_NSIG_WORDS) {
	default:
		for (i = _NSIG_WORDS, ready = 0; --i >= 0 ;)
			ready |= signal->sig[i] &~ blocked->sig[i];
		break;

	case 4: ready  = signal->sig[3] &~ blocked->sig[3];
		ready |= signal->sig[2] &~ blocked->sig[2];
		ready |= signal->sig[1] &~ blocked->sig[1];
		ready |= signal->sig[0] &~ blocked->sig[0];
		break;

	case 2: ready  = signal->sig[1] &~ blocked->sig[1];
		ready |= signal->sig[0] &~ blocked->sig[0];
		break;

	case 1: ready  = signal->sig[0] &~ blocked->sig[0];
	}
	return ready !=	0;
}

/* Reevaluate whether the task has signals pending delivery.
   This is required every time the blocked sigset_t changes.
   All callers should have t->sigmask_lock.  */

static inline void recalc_sigpending(struct task_struct *t)
{
	t->sigpending = has_pending_signals(&t->pending.signal, &t->blocked);
}

/* True if we are on the alternate signal stack.  */

static inline int on_sig_stack(unsigned long sp)
{
	return (sp - current->sas_ss_sp < current->sas_ss_size);
}

static inline int sas_ss_flags(unsigned long sp)
{
	return (current->sas_ss_size == 0 ? SS_DISABLE
		: on_sig_stack(sp) ? SS_ONSTACK : 0);
}

extern int request_irq(unsigned int,
		       void (*handler)(int, void *, struct pt_regs *),
		       unsigned long, const char *, void *);
extern void free_irq(unsigned int, void *);

/*
 * This has now become a routine instead of a macro, it sets a flag if
 * it returns true (to do BSD-style accounting where the process is flagged
 * if it uses root privs). The implication of this is that you should do
 * normal permissions checks first, and check suser() last.
 *
 * [Dec 1997 -- Chris Evans]
 * For correctness, the above considerations need to be extended to
 * fsuser(). This is done, along with moving fsuser() checks to be
 * last.
 *
 * These will be removed, but in the mean time, when the SECURE_NOROOT 
 * flag is set, uids don't grant privilege.
 */
static inline int suser(void)
{
	if (!issecure(SECURE_NOROOT) && current->euid == 0) { 
		current->flags |= PF_SUPERPRIV;
		return 1;
	}
	return 0;
}

static inline int fsuser(void)
{
	if (!issecure(SECURE_NOROOT) && current->fsuid == 0) {
		current->flags |= PF_SUPERPRIV;
		return 1;
	}
	return 0;
}

/*
 * capable() checks for a particular capability.  
 * New privilege checks should use this interface, rather than suser() or
 * fsuser(). See include/linux/capability.h for defined capabilities.
 */

static inline int capable(int cap)
{
#if 1 /* ok now */
	if (cap_raised(current->cap_effective, cap))
#else
	if (cap_is_fs_cap(cap) ? current->fsuid == 0 : current->euid == 0)
#endif
	{
		current->flags |= PF_SUPERPRIV;
		return 1;
	}
	return 0;
}

/*
 * Routines for handling mm_structs
 */
extern struct mm_struct * mm_alloc(void);

extern struct mm_struct * start_lazy_tlb(void);
extern void end_lazy_tlb(struct mm_struct *mm);

/* mmdrop drops the mm and the page tables */
extern inline void FASTCALL(__mmdrop(struct mm_struct *));
static inline void mmdrop(struct mm_struct * mm)
{
	if (atomic_dec_and_test(&mm->mm_count))
		__mmdrop(mm);
}

/* mmput gets rid of the mappings and all user-space */
extern void mmput(struct mm_struct *);
/* Remove the current tasks stale references to the old mm_struct */
extern void mm_release(void);

/*
 * Routines for handling the fd arrays
 */
extern struct file ** alloc_fd_array(int);
extern int expand_fd_array(struct files_struct *, int nr);
extern void free_fd_array(struct file **, int);

extern fd_set *alloc_fdset(int);
extern int expand_fdset(struct files_struct *, int nr);
extern void free_fdset(fd_set *, int);

extern int  copy_thread(int, unsigned long, unsigned long, unsigned long, struct task_struct *, struct pt_regs *);
extern void flush_thread(void);
extern void exit_thread(void);

extern void exit_mm(struct task_struct *);
extern void exit_files(struct task_struct *);
extern void exit_sighand(struct task_struct *);

extern void daemonize(void);

extern int do_execve(char *, char **, char **, struct pt_regs *);
extern int do_fork(unsigned long, unsigned long, struct pt_regs *, unsigned long);

extern void FASTCALL(add_wait_queue(wait_queue_head_t *q, wait_queue_t * wait));
extern void FASTCALL(add_wait_queue_exclusive(wait_queue_head_t *q, wait_queue_t * wait));
extern void FASTCALL(remove_wait_queue(wait_queue_head_t *q, wait_queue_t * wait));

#define __wait_event(wq, condition) 					\
do {									\
	wait_queue_t __wait;						\
	init_waitqueue_entry(&__wait, current);				\
									\
	add_wait_queue(&wq, &__wait);					\
	for (;;) {							\
		set_current_state(TASK_UNINTERRUPTIBLE);		\
		if (condition)						\
			break;						\
		schedule();						\
	}								\
	current->state = TASK_RUNNING;					\
	remove_wait_queue(&wq, &__wait);				\
} while (0)

#define wait_event(wq, condition) 					\
do {									\
	if (condition)	 						\
		break;							\
	__wait_event(wq, condition);					\
} while (0)

#define __wait_event_interruptible(wq, condition, ret)			\
do {									\
	wait_queue_t __wait;						\
	init_waitqueue_entry(&__wait, current);				\
									\
	add_wait_queue(&wq, &__wait);					\
	for (;;) {							\
		set_current_state(TASK_INTERRUPTIBLE);			\
		if (condition)						\
			break;						\
		if (!signal_pending(current)) {				\
			schedule();					\
			continue;					\
		}							\
		ret = -ERESTARTSYS;					\
		break;							\
	}								\
	current->state = TASK_RUNNING;					\
	remove_wait_queue(&wq, &__wait);				\
} while (0)
	
#define wait_event_interruptible(wq, condition)				\
({									\
	int __ret = 0;							\
	if (!(condition))						\
		__wait_event_interruptible(wq, condition, __ret);	\
	__ret;								\
})

#define REMOVE_LINKS(p) do { \
	(p)->next_task->prev_task = (p)->prev_task; \
	(p)->prev_task->next_task = (p)->next_task; \
	if ((p)->p_osptr) \
		(p)->p_osptr->p_ysptr = (p)->p_ysptr; \
	if ((p)->p_ysptr) \
		(p)->p_ysptr->p_osptr = (p)->p_osptr; \
	else \
		(p)->p_pptr->p_cptr = (p)->p_osptr; \
	} while (0)

// 静态数据成员
// union task_union init_task_union
// __attribute__((section("init_task"))) =
// { INIT_TASK(init_task_union.task) };

// init_task 是 0 号进程
#define SET_LINKS(p) do { \
	/* ========== 第一部分：挂入【全局所有进程双向循环链表】（init_task为链表头）*/ \
	(p)->next_task = &init_task; \
	(p)->prev_task = init_task.prev_task; \
	init_task.prev_task->next_task = (p); \
	init_task.prev_task = (p); \
	/*========== 第二部分：维护【父子、兄弟亲属链表】*/ \
	(p)->p_ysptr = NULL; \
	/*p_pptr = parent，父进程；p_cptr = parent最小子(youngest child)*/ \
	if (((p)->p_osptr = (p)->p_pptr->p_cptr) != NULL) \
		(p)->p_osptr->p_ysptr = p; \
	(p)->p_pptr->p_cptr = p; \
	} while (0)

#define for_each_task(p) \
	for (p = &init_task ; (p = p->next_task) != &init_task ; )

#define next_thread(p) \
	list_entry((p)->thread_group.next, struct task_struct, thread_group)

static inline void del_from_runqueue(struct task_struct * p)
{
	nr_running--;
	p->sleep_time = jiffies;
	list_del(&p->run_list);
	p->run_list.next = NULL;
}

static inline int task_on_runqueue(struct task_struct *p)
{
	return (p->run_list.next != NULL);
}

static inline void unhash_process(struct task_struct *p)
{
	if (task_on_runqueue(p)) BUG();
	write_lock_irq(&tasklist_lock);
	nr_threads--;
	unhash_pid(p);
	REMOVE_LINKS(p);
	list_del(&p->thread_group);
	write_unlock_irq(&tasklist_lock);
}

static inline void task_lock(struct task_struct *p)
{
	spin_lock(&p->alloc_lock);
}

static inline void task_unlock(struct task_struct *p)
{
	spin_unlock(&p->alloc_lock);
}

/* write full pathname into buffer and return start of pathname */
static inline char * d_path(struct dentry *dentry, struct vfsmount *vfsmnt,
				char *buf, int buflen)
{
	char *res;
	struct vfsmount *rootmnt;
	struct dentry *root;
	read_lock(&current->fs->lock);
	rootmnt = mntget(current->fs->rootmnt);
	root = dget(current->fs->root);
	read_unlock(&current->fs->lock);
	spin_lock(&dcache_lock);
	res = __d_path(dentry, vfsmnt, root, rootmnt, buf, buflen);
	spin_unlock(&dcache_lock);
	dput(root);
	mntput(rootmnt);
	return res;
}

#endif /* __KERNEL__ */

#endif
