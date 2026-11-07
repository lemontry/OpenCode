/*
 * i386 semaphore implementation.
 *
 * (C) Copyright 1999 Linus Torvalds
 *
 * Portions Copyright 1999 Red Hat, Inc.
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 *
 * rw semaphores implemented November 1999 by Benjamin LaHaise <bcrl@redhat.com>
 */
#include <linux/config.h>
#include <linux/sched.h>

#include <asm/semaphore.h>

/*
 * Semaphores are implemented using a two-way counter:
 * The "count" variable is decremented for each process
 * that tries to acquire the semaphore, while the "sleeping"
 * variable is a count of such acquires.
 *
 * Notably, the inline "up()" and "down()" functions can
 * efficiently test if they need to do any extra work (up
 * needs to do something only if count was negative before
 * the increment operation.
 *
 * "sleeping" and the contention routine ordering is
 * protected by the semaphore spinlock.
 *
 * Note that these functions are only called when there is
 * contention on the lock, and as such all this is the
 * "non-critical" part of the whole semaphore business. The
 * critical part is the inline stuff in <asm/semaphore.h>
 * where we want to avoid any extra jumps and calls.
 */

/*
 * Logic:
 *  - only on a boundary condition do we need to care. When we go
 *    from a negative count to a non-negative, we wake people up.
 *  - when we go from a non-negative count to a negative do we
 *    (a) synchronize with the "sleeper" count and (b) make sure
 *    that we're on the wakeup list before we synchronize so that
 *    we cannot lose wakeup events.
 */

void __up(struct semaphore *sem)
{
	wake_up(&sem->wait);
}

static spinlock_t semaphore_lock = SPIN_LOCK_UNLOCKED;
/**
 * __down - 信号量down失败后的睡眠等待逻辑，不可中断 (Linux2.4)
 * @sem: 目标信号量
 * 被汇编down里面的 __down_failed 调用；排他等待，TASK_UNINTERRUPTIBLE，信号不能打断
 */
void __down(struct semaphore * sem)
{
    // 获取当前进程描述符
    struct task_struct *tsk = current;
    // 栈上定义并初始化等待节点，绑定当前进程
    DECLARE_WAITQUEUE(wait, tsk);
    // 设置进程状态：不可中断睡眠
    tsk->state = TASK_UNINTERRUPTIBLE;
    // 添加排他等待节点，避免惊群，一次wake只唤醒一个等待者
    add_wait_queue_exclusive(&sem->wait, &wait);
    // 获取信号量全局自旋锁，关本地中断，保护sleepers、count临界区
    spin_lock_irq(&semaphore_lock);
    // sleepers计数+1，表示又多一个等待睡眠的进程
    sem->sleepers++;
    for (;;) {
        int sleepers = sem->sleepers;
        /*
         * Add "everybody else" into it. They aren't
         * playing, because we own the spinlock.
         */
        // atomic_add_negative：把(sleepers - 1)加到sem->count，返回相加后结果是否为负数
        if (!atomic_add_negative(sleepers - 1, &sem->count)) {
            // count >= 0：拿到信号量资源
            sem->sleepers = 0;
            break;
        }
        // 资源还是不够，sleepers置1，代表当前这个进程是等待者
        sem->sleepers = 1;
        // 解锁，开中断，让出CPU
        spin_unlock_irq(&semaphore_lock);
        // 调度，进入睡眠，等待up唤醒
        schedule();
        // 被唤醒回来，重新设置状态，准备再次检查条件
        tsk->state = TASK_UNINTERRUPTIBLE;
        // 再次拿锁，进入下一轮循环判断
        spin_lock_irq(&semaphore_lock);
    }
    // 拿到资源，退出循环，释放自旋锁
    spin_unlock_irq(&semaphore_lock);
    // 将等待节点从等待队列摘除
    remove_wait_queue(&sem->wait, &wait);
    // 恢复进程为运行态
    tsk->state = TASK_RUNNING;
    // 唤醒下一个等待在信号量上的进程（传递唤醒）
    wake_up(&sem->wait);
}

/**
 * __down_interruptible - 可中断版本信号量获取的睡眠逻辑（Linux2.4）
 * @sem: 目标信号量
 * @retval: 返回值，0成功；-EINTR代表被信号打断
 * 特点：TASK_INTERRUPTIBLE，可以被信号唤醒打断，是down_interruptible底层C实现
 */
int __down_interruptible(struct semaphore * sem)
{
    // 返回值初始化为0，默认成功拿到信号量
    int retval = 0;
    // 获取当前进程描述符
    struct task_struct *tsk = current;
    // 在栈上定义并初始化排他等待节点，绑定当前进程
    DECLARE_WAITQUEUE(wait, tsk);
    // 设置进程状态：可中断睡眠，可以被信号唤醒
    tsk->state = TASK_INTERRUPTIBLE;
    // 添加排他等待节点，避免惊群，一次wake只唤醒一个等待进程
    add_wait_queue_exclusive(&sem->wait, &wait);
    // 获取全局信号量自旋锁，关闭本地中断，保护sleepers、count临界区
    spin_lock_irq(&semaphore_lock);
    // 等待进程计数+1
    sem->sleepers ++;
    for (;;) {
        int sleepers = sem->sleepers;
        /*
         * With signals pending, this turns into
         * the trylock failure case - we won't be
         * sleeping, and we* can't get the lock as
         * it has contention. Just correct the count
         * and exit.
         */
        // 检查当前进程是否有挂起信号
        if (signal_pending(current)) {
            // 标记返回值：被信号中断
            retval = -EINTR;
            sem->sleepers = 0;
            // 修复count：把sleepers加回去，恢复信号量计数，相当于本次down没有执行
            atomic_add(sleepers, &sem->count);
            break;
        }
        /*
         * Add "everybody else" into it. They aren't
         * playing, because we own the spinlock. The
         * "-1" is because we're still hoping to get
         * the lock.
         */
        // atomic_add_negative：sem->count += (sleepers - 1)，返回相加后是否为负数
		// 返回 sem->count 是否小于0
        if (!atomic_add_negative(sleepers - 1, &sem->count)) {
            // count >= 0，资源充足，成功拿到信号量
            sem->sleepers = 0;
            break;
        }
        // 资源仍然不足，sleepers置1，代表只有本进程继续等待
        sem->sleepers = 1;
        // 解锁、恢复中断
        spin_unlock_irq(&semaphore_lock);
        // 让出CPU睡眠，等待up唤醒或者信号唤醒
        schedule();
        // 唤醒返回，重新设置为可中断睡眠状态
        tsk->state = TASK_INTERRUPTIBLE;
        // 重新拿锁，进入循环判断
        spin_lock_irq(&semaphore_lock);
    }
    // 退出循环，释放全局自旋锁
    spin_unlock_irq(&semaphore_lock);
    // 恢复进程状态为运行态
    tsk->state = TASK_RUNNING;
    // 将等待节点从等待队列摘除
    remove_wait_queue(&sem->wait, &wait);
    // 传递唤醒：唤醒下一个排他等待的进程
    wake_up(&sem->wait);
    return retval;
}

/*
 * Trylock failed - make sure we correct for
 * having decremented the count.
 *
 * We could have done the trylock with a
 * single "cmpxchg" without failure cases,
 * but then it wouldn't work on a 386.
 */
int __down_trylock(struct semaphore * sem)
{
	int sleepers;
	unsigned long flags;

	spin_lock_irqsave(&semaphore_lock, flags);
	sleepers = sem->sleepers + 1;
	sem->sleepers = 0;

	/*
	 * Add "everybody else" and us into it. They aren't
	 * playing, because we own the spinlock.
	 */
	if (!atomic_add_negative(sleepers, &sem->count))
		wake_up(&sem->wait);

	spin_unlock_irqrestore(&semaphore_lock, flags);
	return 1;
}


/*
 * The semaphore operations have a special calling sequence that
 * allow us to do a simpler in-line version of them. These routines
 * need to convert that sequence back into the C sequence when
 * there is contention on the semaphore.
 *
 * %ecx contains the semaphore pointer on entry. Save the C-clobbered
 * registers (%eax, %edx and %ecx) except %eax when used as a return
 * value..
 */
asm(
".align 4\n"
".globl __down_failed\n"
"__down_failed:\n\t"
	"pushl %eax\n\t"
	"pushl %edx\n\t"
	"pushl %ecx\n\t"
	"call __down\n\t"
	"popl %ecx\n\t"
	"popl %edx\n\t"
	"popl %eax\n\t"
	"ret"
);

asm(
".align 4\n"
".globl __down_failed_interruptible\n"
"__down_failed_interruptible:\n\t"
	"pushl %edx\n\t"
	"pushl %ecx\n\t"
	"call __down_interruptible\n\t"
	"popl %ecx\n\t"
	"popl %edx\n\t"
	"ret"
);

asm(
".align 4\n"
".globl __down_failed_trylock\n"
"__down_failed_trylock:\n\t"
	"pushl %edx\n\t"
	"pushl %ecx\n\t"
	"call __down_trylock\n\t"
	"popl %ecx\n\t"
	"popl %edx\n\t"
	"ret"
);

asm(
".align 4\n"
".globl __up_wakeup\n"
"__up_wakeup:\n\t"
	"pushl %eax\n\t"
	"pushl %edx\n\t"
	"pushl %ecx\n\t"
	"call __up\n\t"
	"popl %ecx\n\t"
	"popl %edx\n\t"
	"popl %eax\n\t"
	"ret"
);

asm(
"
.align 4
.globl __down_read_failed
__down_read_failed:
	pushl	%edx
	pushl	%ecx
	jnc	2f

3:	call	down_read_failed_biased

1:	popl	%ecx
	popl	%edx
	ret

2:	call	down_read_failed
	" LOCK "subl	$1,(%eax)
	jns	1b
	jnc	2b
	jmp	3b
"
);

asm(
"
.align 4
.globl __down_write_failed
__down_write_failed:
	pushl	%edx
	pushl	%ecx
	jnc	2f

3:	call	down_write_failed_biased

1:	popl	%ecx
	popl	%edx
	ret

2:	call	down_write_failed
	" LOCK "subl	$" RW_LOCK_BIAS_STR ",(%eax)
	jz	1b
	jnc	2b
	jmp	3b
"
);

struct rw_semaphore *FASTCALL(rwsem_wake_readers(struct rw_semaphore *sem));
struct rw_semaphore *FASTCALL(rwsem_wake_writer(struct rw_semaphore *sem));

struct rw_semaphore *FASTCALL(down_read_failed_biased(struct rw_semaphore *sem));
struct rw_semaphore *FASTCALL(down_write_failed_biased(struct rw_semaphore *sem));
struct rw_semaphore *FASTCALL(down_read_failed(struct rw_semaphore *sem));
struct rw_semaphore *FASTCALL(down_write_failed(struct rw_semaphore *sem));

struct rw_semaphore *down_read_failed_biased(struct rw_semaphore *sem)
{
	struct task_struct *tsk = current;
	DECLARE_WAITQUEUE(wait, tsk);

	add_wait_queue(&sem->wait, &wait);	/* put ourselves at the head of the list */

	for (;;) {
		if (sem->read_bias_granted && xchg(&sem->read_bias_granted, 0))
			break;
		set_task_state(tsk, TASK_UNINTERRUPTIBLE);
		if (!sem->read_bias_granted)
			schedule();
	}

	remove_wait_queue(&sem->wait, &wait);
	tsk->state = TASK_RUNNING;

	return sem;
}

struct rw_semaphore *down_write_failed_biased(struct rw_semaphore *sem)
{
	struct task_struct *tsk = current;
	DECLARE_WAITQUEUE(wait, tsk);

	add_wait_queue_exclusive(&sem->write_bias_wait, &wait);	/* put ourselves at the end of the list */

	for (;;) {
		if (sem->write_bias_granted && xchg(&sem->write_bias_granted, 0))
			break;
		set_task_state(tsk, TASK_UNINTERRUPTIBLE);
		if (!sem->write_bias_granted)
			schedule();
	}

	remove_wait_queue(&sem->write_bias_wait, &wait);
	tsk->state = TASK_RUNNING;

	/* if the lock is currently unbiased, awaken the sleepers
	 * FIXME: this wakes up the readers early in a bit of a
	 * stampede -> bad!
	 */
	if (atomic_read(&sem->count) >= 0)
		wake_up(&sem->wait);

	return sem;
}

/* Wait for the lock to become unbiased.  Readers
 * are non-exclusive. =)
 */
struct rw_semaphore *down_read_failed(struct rw_semaphore *sem)
{
	struct task_struct *tsk = current;
	DECLARE_WAITQUEUE(wait, tsk);

	__up_read(sem);	/* this takes care of granting the lock */

	add_wait_queue(&sem->wait, &wait);

	while (atomic_read(&sem->count) < 0) {
		set_task_state(tsk, TASK_UNINTERRUPTIBLE);
		if (atomic_read(&sem->count) >= 0)
			break;
		schedule();
	}

	remove_wait_queue(&sem->wait, &wait);
	tsk->state = TASK_RUNNING;

	return sem;
}

/* Wait for the lock to become unbiased. Since we're
 * a writer, we'll make ourselves exclusive.
 */
struct rw_semaphore *down_write_failed(struct rw_semaphore *sem)
{
	struct task_struct *tsk = current;
	DECLARE_WAITQUEUE(wait, tsk);

	__up_write(sem);	/* this takes care of granting the lock */

	add_wait_queue_exclusive(&sem->wait, &wait);

	while (atomic_read(&sem->count) < 0) {
		set_task_state(tsk, TASK_UNINTERRUPTIBLE);
		if (atomic_read(&sem->count) >= 0)
			break;	/* we must attempt to acquire or bias the lock */
		schedule();
	}

	remove_wait_queue(&sem->wait, &wait);
	tsk->state = TASK_RUNNING;

	return sem;
}

asm(
"
.align 4
.globl __rwsem_wake
__rwsem_wake:
	pushl	%edx
	pushl	%ecx

	jz	1f
	call	rwsem_wake_readers
	jmp	2f

1:	call	rwsem_wake_writer

2:	popl	%ecx
	popl	%edx
	ret
"
);

/* Called when someone has done an up that transitioned from
 * negative to non-negative, meaning that the lock has been
 * granted to whomever owned the bias.
 */
struct rw_semaphore *rwsem_wake_readers(struct rw_semaphore *sem)
{
	if (xchg(&sem->read_bias_granted, 1))
		BUG();
	wake_up(&sem->wait);
	return sem;
}

struct rw_semaphore *rwsem_wake_writer(struct rw_semaphore *sem)
{
	if (xchg(&sem->write_bias_granted, 1))
		BUG();
	wake_up(&sem->write_bias_wait);
	return sem;
}

#if defined(CONFIG_SMP)
asm(
"
.align	4
.globl	__write_lock_failed
__write_lock_failed:
	" LOCK "addl	$" RW_LOCK_BIAS_STR ",(%eax)
1:	cmpl	$" RW_LOCK_BIAS_STR ",(%eax)
	jne	1b

	" LOCK "subl	$" RW_LOCK_BIAS_STR ",(%eax)
	jnz	__write_lock_failed
	ret


.align	4
.globl	__read_lock_failed
__read_lock_failed:
	lock ; incl	(%eax)
1:	cmpl	$1,(%eax)
	js	1b

	lock ; decl	(%eax)
	js	__read_lock_failed
	ret
"
);
#endif

