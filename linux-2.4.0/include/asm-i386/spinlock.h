#ifndef __ASM_SPINLOCK_H
#define __ASM_SPINLOCK_H

#include <asm/atomic.h>
#include <asm/rwlock.h>
#include <asm/page.h>

extern int printk(const char * fmt, ...)
	__attribute__ ((format (printf, 1, 2)));

/* It seems that people are forgetting to
 * initialize their spinlocks properly, tsk tsk.
 * Remember to turn this off in 2.4. -ben
 */
#define SPINLOCK_DEBUG	0

/*
 * Your basic SMP spinlocks, allowing only a single CPU anywhere
 */

//自旋锁的实现
typedef struct {
	volatile unsigned int lock;
#if SPINLOCK_DEBUG
	//调试标记
	unsigned magic;
#endif
} spinlock_t;

#define SPINLOCK_MAGIC	0xdead4ead

#if SPINLOCK_DEBUG
#define SPINLOCK_MAGIC_INIT	, SPINLOCK_MAGIC
#else
#define SPINLOCK_MAGIC_INIT	/* */
#endif

//没有占用锁s
#define SPIN_LOCK_UNLOCKED (spinlock_t) { 1 SPINLOCK_MAGIC_INIT }

//初始化自旋锁
#define spin_lock_init(x)	do { *(x) = SPIN_LOCK_UNLOCKED; } while(0)

/*
 * Simple spin lock operations.  There are two variants, one clears IRQ's
 * on the local processor, one does not.
 *
 * We make no fairness assumptions. They have a cost.
 */

//小于0，就是被占用锁了
#define spin_is_locked(x)	(*(volatile char *)(&(x)->lock) <= 0)

/**
 * spin_unlock_wait - 等待自旋锁被解锁，本身不获取锁
 * @x: 自旋锁指针
 * barrier(): 编译器屏障，防止编译器优化掉这个循环
 * 逻辑：循环检测锁状态，直到锁变成未锁定；只等、不抢锁
 */
#define spin_unlock_wait(x)	do { barrier(); } while(spin_is_locked(x))

/**
 * spin_lock_string  2.4 x86 32位自旋锁核心汇编模板
 * lock ; decb %0      原子将锁字节减1
 * js 2f               如果结果为负（原来已经是0，拿锁失败），跳到自旋等待段
 * .section .text.lock,"ax"  自旋等待循环放到单独section，不占用主代码高速缓存
 * 2:  cmpb $0,%0      循环检测锁的值
 *     rep;nop         空循环，降低总线压力
 *     jle 2b          锁还是<=0，继续自旋等待
 *     jmp 1b          看到锁释放，回到开头重新尝试抢锁
 * .previous           切回原来的代码段
 */
#define spin_lock_string \
	"\n1:\t" \
	"lock ; decb %0\n\t" \
	"js 2f\n" \
	".section .text.lock,\"ax\"\n" \
	"2:\t" \
	"cmpb $0,%0\n\t" \
	"rep;nop\n\t" \
	"jle 2b\n\t" \
	"jmp 1b\n" \
	".previous"

/*
 * This works. Despite all the confusion.
 */
/**
 * spin_unlock_string  2.4 x86-32 自旋锁释放汇编模板
 * movb $1,%0：直接将锁字节写回数值1，标记自旋锁为空闲状态
 */
#define spin_unlock_string \
    "movb $1,%0"

/**
 * spin_trylock - 尝试获取自旋锁，非阻塞，拿到返回1，失败直接返回0
 * @lock: 自旋锁指针
 * 逻辑：用xchgb交换，把0写入锁变量，读出原来的值；
 * 如果原来的值>0（等于1，空闲），代表抢锁成功；否则锁已经被持有，失败
 */
static inline int spin_trylock(spinlock_t *lock)
{
    char oldval;
    __asm__ __volatile__(
        // xchgb：字节交换，原子指令，把%b0的值(0)和lock->lock交换
        "xchgb %b0,%1"
        // 输出约束：oldval接收交换出来的旧值；lock->lock是内存
        :"=q" (oldval), "=m" (lock->lock)
        // 输入约束：把0放到用于oldval的寄存器%b0
        :"0" (0) : "memory");
    // oldval > 0 也就是 oldval == 1：原来锁是空闲，获取成功
    return oldval > 0;
}

//使用汇编判断等等自旋锁
static inline void spin_lock(spinlock_t *lock)
{
#if SPINLOCK_DEBUG
	__label__ here;
here:
	if (lock->magic != SPINLOCK_MAGIC) {
printk("eip: %p\n", &&here);
		BUG();
	}
#endif
	__asm__ __volatile__(
		spin_lock_string
		:"=m" (lock->lock) : : "memory");
}

//自旋锁
static inline void spin_unlock(spinlock_t *lock)
{
#if SPINLOCK_DEBUG
	if (lock->magic != SPINLOCK_MAGIC)
		BUG();
	if (!spin_is_locked(lock))
		BUG();
#endif
	__asm__ __volatile__(
		spin_unlock_string
		:"=m" (lock->lock) : : "memory");
}

/*
 * Read-write spinlocks, allowing multiple readers
 * but only one writer.
 *
 * NOTE! it is quite common to have readers in interrupts
 * but no interrupt writers. For those circumstances we
 * can "mix" irq-safe locks - any writer needs to get a
 * irq-safe write-lock, but readers can get non-irqsafe
 * read-locks.
 */

//读写锁
// 正数：当前有 N 个读者持有读锁，**无写者**，可以继续加读者
// 负数：写锁被持有（值一般是 `-1`），此时**禁止新来读者、新来写者**
// 拿到写锁之后，计数器的值就是 0
// 写锁一次，减去 RW_LOCK_BIAS
typedef struct {
	volatile unsigned int lock;
#if SPINLOCK_DEBUG
	unsigned magic;
#endif
} rwlock_t;

#define RWLOCK_MAGIC	0xdeaf1eed

#if SPINLOCK_DEBUG
#define RWLOCK_MAGIC_INIT	, RWLOCK_MAGIC
#else
#define RWLOCK_MAGIC_INIT	/* */
#endif

//读写锁没用占用的标记
#define RW_LOCK_UNLOCKED (rwlock_t) { RW_LOCK_BIAS RWLOCK_MAGIC_INIT }

#define rwlock_init(x)	do { *(x) = RW_LOCK_UNLOCKED; } while(0)

/*
 * On x86, we implement read-write locks as a 32-bit counter
 * with the high bit (sign) being the "contended" bit.
 *
 * The inline assembly is non-obvious. Think about it.
 *
 * Changed to use the same technique as rw semaphores.  See
 * semaphore.h for details.  -ben
 */
/* the spinlock helpers are in arch/i386/kernel/semaphore.c */

//读锁
static inline void read_lock(rwlock_t *rw)
{
#if SPINLOCK_DEBUG
	if (rw->magic != RWLOCK_MAGIC)
		BUG();
#endif
	__build_read_lock(rw, "__read_lock_failed");
}

static inline void write_lock(rwlock_t *rw)
{
#if SPINLOCK_DEBUG
	if (rw->magic != RWLOCK_MAGIC)
		BUG();
#endif
	__build_write_lock(rw, "__write_lock_failed");
}

#define read_unlock(rw)		asm volatile("lock ; incl %0" :"=m" ((rw)->lock) : : "memory")
#define write_unlock(rw)	asm volatile("lock ; addl $" RW_LOCK_BIAS_STR ",%0":"=m" ((rw)->lock) : : "memory")

static inline int write_trylock(rwlock_t *lock)
{
	atomic_t *count = (atomic_t *)lock;
	if (atomic_sub_and_test(RW_LOCK_BIAS, count))
		return 1;
	atomic_add(RW_LOCK_BIAS, count);
	return 0;
}

#endif /* __ASM_SPINLOCK_H */
