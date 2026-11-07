#ifndef _I386_SEMAPHORE_H
#define _I386_SEMAPHORE_H

#include <linux/linkage.h>

#ifdef __KERNEL__

/*
 * SMP- and interrupt-safe semaphores..
 *
 * (C) Copyright 1996 Linus Torvalds
 *
 * Modified 1996-12-23 by Dave Grothe <dave@gcom.com> to fix bugs in
 *                     the original code and to make semaphore waits
 *                     interruptible so that processes waiting on
 *                     semaphores can be killed.
 * Modified 1999-02-14 by Andrea Arcangeli, split the sched.c helper
 *		       functions in asm/sempahore-helper.h while fixing a
 *		       potential and subtle race discovered by Ulrich Schmid
 *		       in down_interruptible(). Since I started to play here I
 *		       also implemented the `trylock' semaphore operation.
 *          1999-07-02 Artur Skawina <skawina@geocities.com>
 *                     Optimized "0(ecx)" -> "(ecx)" (the assembler does not
 *                     do this). Changed calling sequences from push/jmp to
 *                     traditional call/ret.
 * Modified 2001-01-01 Andreas Franck <afranck@gmx.de>
 *		       Some hacks to ensure compatibility with recent
 *		       GCC snapshots, to avoid stack corruption when compiling
 *		       with -fomit-frame-pointer. It's not sure if this will
 *		       be fixed in GCC, as our previous implementation was a
 *		       bit dubious.
 *
 * If you would like to see an analysis of this implementation, please
 * ftp to gcom.com and download the file
 * /pub/linux/src/semaphore/semaphore-2.0.24.tar.gz.
 *
 */

#include <asm/system.h>
#include <asm/atomic.h>
#include <asm/rwlock.h>
#include <linux/wait.h>

/**
 * struct semaphore - Linux 2.4 内核信号量（睡眠锁，不是自旋锁）
 */
struct semaphore {
	// 资源计数，正数代表可用资源；0 代表无资源；负数代表有进程在等待
	atomic_t count;
	// 记录当前正在这个信号量上睡眠等待的进程数量
	int sleepers;

	// 等待队列头，所有阻塞在该信号量上的进程挂在这个等待队列
	wait_queue_head_t wait;
#if WAITQUEUE_DEBUG
	// 调试用魔数，开启WAITQUEUE_DEBUG才存在，用来检测结构体损坏
	long __magic;
#endif
};

#if WAITQUEUE_DEBUG
# define __SEM_DEBUG_INIT(name) \
		, (int)&(name).__magic
#else
# define __SEM_DEBUG_INIT(name)
#endif

#define __SEMAPHORE_INITIALIZER(name,count) \
{ ATOMIC_INIT(count), 0, __WAIT_QUEUE_HEAD_INITIALIZER((name).wait) \
	__SEM_DEBUG_INIT(name) }

#define __MUTEX_INITIALIZER(name) \
	__SEMAPHORE_INITIALIZER(name,1)

#define __DECLARE_SEMAPHORE_GENERIC(name,count) \
	struct semaphore name = __SEMAPHORE_INITIALIZER(name,count)

#define DECLARE_MUTEX(name) __DECLARE_SEMAPHORE_GENERIC(name,1)
#define DECLARE_MUTEX_LOCKED(name) __DECLARE_SEMAPHORE_GENERIC(name,0)

static inline void sema_init (struct semaphore *sem, int val)
{
/*
 *	*sem = (struct semaphore)__SEMAPHORE_INITIALIZER((*sem),val);
 *
 * i'd rather use the more flexible initialization above, but sadly
 * GCC 2.7.2.3 emits a bogus warning. EGCS doesnt. Oh well.
 */
	atomic_set(&sem->count, val);
	sem->sleepers = 0;
	init_waitqueue_head(&sem->wait);
#if WAITQUEUE_DEBUG
	sem->__magic = (int)&sem->__magic;
#endif
}

static inline void init_MUTEX (struct semaphore *sem)
{
	sema_init(sem, 1);
}

static inline void init_MUTEX_LOCKED (struct semaphore *sem)
{
	sema_init(sem, 0);
}

asmlinkage void __down_failed(void /* special register calling convention */);
asmlinkage int  __down_failed_interruptible(void  /* params in registers */);
asmlinkage int  __down_failed_trylock(void  /* params in registers */);
asmlinkage void __up_wakeup(void /* special register calling convention */);

asmlinkage void __down(struct semaphore * sem);
asmlinkage int  __down_interruptible(struct semaphore * sem);
asmlinkage int  __down_trylock(struct semaphore * sem);
asmlinkage void __up(struct semaphore * sem);

/*
 * This is ugly, but we want the default case to fall through.
 * "__down_failed" is a special asm handler that calls the C
 * routine that actually waits. See arch/i386/kernel/semaphore.c
 */
/**
 * down - 获取信号量，不可中断（Linux2.4 x86）
 * @sem: 信号量结构体指针
 * 作用：申请信号量资源；资源充足直接返回；资源不足进入睡眠等待
 * 是自旋锁之外的内核同步原语，会让出CPU，不是忙等
 */
static inline void down(struct semaphore * sem)
{
#if WAITQUEUE_DEBUG
    // 调试：校验信号量magic魔数，检测内存损坏
    CHECK_MAGIC(sem->__magic);
#endif
    __asm__ __volatile__(
        "# atomic down operation\n\t"
        // LOCK：总线锁，保证SMP下原子操作；decl 自减1：sem->count--
        LOCK "decl %0\n\t"
        // js：符号标志位=1则跳转。如果count减完变成负数 → 资源不够，跳转到2标号
        "js 2f\n"
        // 减完 >=0，资源充足，走到1标号，直接退出，拿到信号量
        "1:\n"
        ".section .text.lock,\"ax\"\n"
        // 资源不足，调用C函数 __down_failed 进入等待队列睡眠
        "2:\tcall __down_failed\n\t"
        // 从__down_failed唤醒返回后，跳回1，直接退出
        "jmp 1b\n"
        ".previous"
        :"=m" (sem->count)
        :"c" (sem)
        :"memory");
}


static inline int down_interruptible(struct semaphore * sem)
{
	int result;

#if WAITQUEUE_DEBUG
	CHECK_MAGIC(sem->__magic);
#endif

	__asm__ __volatile__(
		"# atomic interruptible down operation\n\t"
		LOCK "decl %1\n\t"     /* --sem->count */
		"js 2f\n\t"
		"xorl %0,%0\n"
		"1:\n"
		".section .text.lock,\"ax\"\n"
		"2:\tcall __down_failed_interruptible\n\t"
		"jmp 1b\n"
		".previous"
		:"=a" (result), "=m" (sem->count)
		:"c" (sem)
		:"memory");
	return result;
}

static inline int down_trylock(struct semaphore * sem)
{
	int result;

#if WAITQUEUE_DEBUG
	CHECK_MAGIC(sem->__magic);
#endif

	__asm__ __volatile__(
		"# atomic interruptible down operation\n\t"
		LOCK "decl %1\n\t"     /* --sem->count */
		"js 2f\n\t"
		"xorl %0,%0\n"
		"1:\n"
		".section .text.lock,\"ax\"\n"
		"2:\tcall __down_failed_trylock\n\t"
		"jmp 1b\n"
		".previous"
		:"=a" (result), "=m" (sem->count)
		:"c" (sem)
		:"memory");
	return result;
}

/*
 * Note! This is subtle. We jump to wake people up only if
 * the semaphore was negative (== somebody was waiting on it).
 * The default case (no contention) will result in NO
 * jumps for both down() and up().
 */
/**
 * up - 释放信号量（Linux2.4 x86）
 * @sem: 信号量指针
 * 功能：归还信号量资源；如果有进程正在等待，则触发唤醒逻辑
 */
static inline void up(struct semaphore * sem)
{
#if WAITQUEUE_DEBUG
    // 调试：校验信号量magic，检测内存损坏
    CHECK_MAGIC(sem->__magic);
#endif
    __asm__ __volatile__(
        "# atomic up operation\n\t"
        // LOCK总线锁，原子执行 incl：sem->count++
        LOCK "incl %0\n\t"
        // jle：小于等于0则跳转到标号2
        "jle 2f\n"
        // count > 0，没有等待进程，直接走到标号1，返回
        "1:\n"
        // 把失败唤醒分支放到独立section，优化cache局部性
        ".section .text.lock,\"ax\"\n"
        // count <= 0，说明存在等待进程，调用__up_wakeup执行唤醒
        "2:\tcall __up_wakeup\n\t"
        // 唤醒函数返回后跳到标号1，结束
        "jmp 1b\n"
        // 切回原来的代码段
        ".previous"
        :"=m" (sem->count)
        :"c" (sem)       // 输入约束：把sem指针放到ecx，__up_wakeup从ecx取参数
        :"memory");      // 内存屏障，禁止编译器对这块汇编前后内存访问重排
}

/* rw mutexes (should that be mutices? =) -- throw rw
 * spinlocks and semaphores together, and this is what we
 * end up with...
 *
 * The lock is initialized to BIAS.  This way, a writer
 * subtracts BIAS ands gets 0 for the case of an uncontended
 * lock.  Readers decrement by 1 and see a positive value
 * when uncontended, negative if there are writers waiting
 * (in which case it goes to sleep).
 *
 * The value 0x01000000 supports up to 128 processors and
 * lots of processes.  BIAS must be chosen such that subl'ing
 * BIAS once per CPU will result in the long remaining
 * negative.
 *
 * In terms of fairness, this should result in the lock
 * flopping back and forth between readers and writers
 * under heavy use.
 *
 *		-ben
 */
struct rw_semaphore {
	atomic_t		count;
	volatile unsigned char	write_bias_granted;
	volatile unsigned char	read_bias_granted;
	volatile unsigned char	pad1;
	volatile unsigned char	pad2;
	wait_queue_head_t	wait;
	wait_queue_head_t	write_bias_wait;
#if WAITQUEUE_DEBUG
	long			__magic;
	atomic_t		readers;
	atomic_t		writers;
#endif
};

#if WAITQUEUE_DEBUG
#define __RWSEM_DEBUG_INIT	, ATOMIC_INIT(0), ATOMIC_INIT(0)
#else
#define __RWSEM_DEBUG_INIT	/* */
#endif

#define __RWSEM_INITIALIZER(name,count) \
{ ATOMIC_INIT(count), 0, 0, 0, 0, __WAIT_QUEUE_HEAD_INITIALIZER((name).wait), \
	__WAIT_QUEUE_HEAD_INITIALIZER((name).write_bias_wait) \
	__SEM_DEBUG_INIT(name) __RWSEM_DEBUG_INIT }

#define __DECLARE_RWSEM_GENERIC(name,count) \
	struct rw_semaphore name = __RWSEM_INITIALIZER(name,count)

#define DECLARE_RWSEM(name) __DECLARE_RWSEM_GENERIC(name,RW_LOCK_BIAS)
#define DECLARE_RWSEM_READ_LOCKED(name) __DECLARE_RWSEM_GENERIC(name,RW_LOCK_BIAS-1)
#define DECLARE_RWSEM_WRITE_LOCKED(name) __DECLARE_RWSEM_GENERIC(name,0)

static inline void init_rwsem(struct rw_semaphore *sem)
{
	atomic_set(&sem->count, RW_LOCK_BIAS);
	sem->read_bias_granted = 0;
	sem->write_bias_granted = 0;
	init_waitqueue_head(&sem->wait);
	init_waitqueue_head(&sem->write_bias_wait);
#if WAITQUEUE_DEBUG
	sem->__magic = (long)&sem->__magic;
	atomic_set(&sem->readers, 0);
	atomic_set(&sem->writers, 0);
#endif
}

/* we use FASTCALL convention for the helpers */
extern struct rw_semaphore *FASTCALL(__down_read_failed(struct rw_semaphore *sem));
extern struct rw_semaphore *FASTCALL(__down_write_failed(struct rw_semaphore *sem));
extern struct rw_semaphore *FASTCALL(__rwsem_wake(struct rw_semaphore *sem));

static inline void down_read(struct rw_semaphore *sem)
{
#if WAITQUEUE_DEBUG
	if (sem->__magic != (long)&sem->__magic)
		BUG();
#endif
	__build_read_lock(sem, "__down_read_failed");
#if WAITQUEUE_DEBUG
	if (sem->write_bias_granted)
		BUG();
	if (atomic_read(&sem->writers))
		BUG();
	atomic_inc(&sem->readers);
#endif
}

static inline void down_write(struct rw_semaphore *sem)
{
#if WAITQUEUE_DEBUG
	if (sem->__magic != (long)&sem->__magic)
		BUG();
#endif
	__build_write_lock(sem, "__down_write_failed");
#if WAITQUEUE_DEBUG
	if (atomic_read(&sem->writers))
		BUG();
	if (atomic_read(&sem->readers))
		BUG();
	if (sem->read_bias_granted)
		BUG();
	if (sem->write_bias_granted)
		BUG();
	atomic_inc(&sem->writers);
#endif
}

/* When a reader does a release, the only significant
 * case is when there was a writer waiting, and we've
 * bumped the count to 0: we must wake the writer up.
 */
static inline void __up_read(struct rw_semaphore *sem)
{
	__asm__ __volatile__(
		"# up_read\n\t"
		LOCK "incl %0\n\t"
		"jz 2f\n"			/* only do the wake if result == 0 (ie, a writer) */
		"1:\n\t"
		".section .text.lock,\"ax\"\n"
		"2:\tcall __rwsem_wake\n\t"
		"jmp 1b\n"
		".previous"
		:"=m" (sem->count)
		:"a" (sem)
		:"memory"
		);
}

/* releasing the writer is easy -- just release it and
 * wake up any sleepers.
 */
static inline void __up_write(struct rw_semaphore *sem)
{
	__asm__ __volatile__(
		"# up_write\n\t"
		LOCK "addl $" RW_LOCK_BIAS_STR ",%0\n"
		"jc 2f\n"			/* only do the wake if the result was -'ve to 0/+'ve */
		"1:\n\t"
		".section .text.lock,\"ax\"\n"
		"2:\tcall __rwsem_wake\n\t"
		"jmp 1b\n"
		".previous"
		:"=m" (sem->count)
		:"a" (sem)
		:"memory"
		);
}

static inline void up_read(struct rw_semaphore *sem)
{
#if WAITQUEUE_DEBUG
	if (sem->write_bias_granted)
		BUG();
	if (atomic_read(&sem->writers))
		BUG();
	atomic_dec(&sem->readers);
#endif
	__up_read(sem);
}

static inline void up_write(struct rw_semaphore *sem)
{
#if WAITQUEUE_DEBUG
	if (sem->read_bias_granted)
		BUG();
	if (sem->write_bias_granted)
		BUG();
	if (atomic_read(&sem->readers))
		BUG();
	if (atomic_read(&sem->writers) != 1)
		BUG();
	atomic_dec(&sem->writers);
#endif
	__up_write(sem);
}

#endif
#endif
