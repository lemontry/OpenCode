/* include/asm-i386/rwlock.h
 *
 *	Helpers used by both rw spinlocks and rw semaphores.
 *
 *	Based in part on code from semaphore.h and
 *	spinlock.h Copyright 1996 Linus Torvalds.
 *
 *	Copyright 1999 Red Hat, Inc.
 *
 *	Written by Benjamin LaHaise.
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 */
#ifndef _ASM_I386_RWLOCK_H
#define _ASM_I386_RWLOCK_H

#define RW_LOCK_BIAS		 0x01000000
#define RW_LOCK_BIAS_STR	"0x01000000"

/**
 * __build_read_lock_ptr  2.4 x86-32 rwlock 读锁汇编模板
 * @rw: rwlock_t 指针，放到eax
 * @helper: 自旋等待辅助函数名
 * LOCK: 就是lock前缀，总线锁，保证原子
 * subl $1,(%0)：原子把读写锁引用计数减1
 * js 2f：结果为负，跳转到等待逻辑（写锁持有者已经占用，读锁拿不到）
 * 1: 成功拿到读锁，直接返回
 * .section .text.lock,"ax"：等待代码放到独立section，保护主路径icache
 * 2: call helper 进入自旋等待循环
 *    jmp 1b 等待到锁可用后，回去重新尝试获取读锁
 * .previous：切回原来代码段
 */
#define __build_read_lock_ptr(rw, helper)   \
	asm volatile(LOCK "subl $1,(%0)\n\t" \
		     "js 2f\n" \
		     "1:\n" \
		     ".section .text.lock,\"ax\"\n" \
		     "2:\tcall " helper "\n\t" \
		     "jmp 1b\n" \
		     ".previous" \
		     ::"a" (rw) : "memory")

#define __build_read_lock_const(rw, helper)   \
	asm volatile(LOCK "subl $1,%0\n\t" \
		     "js 2f\n" \
		     "1:\n" \
		     ".section .text.lock,\"ax\"\n" \
		     "2:\tpushl %%eax\n\t" \
		     "leal %0,%%eax\n\t" \
		     "call " helper "\n\t" \
		     "popl %%eax\n\t" \
		     "jmp 1b\n" \
		     ".previous" \
		     :"=m" (*(volatile int *)rw) : : "memory")

#define __build_read_lock(rw, helper)	do { \
						if (__builtin_constant_p(rw)) \
							__build_read_lock_const(rw, helper); \
						else \
							__build_read_lock_ptr(rw, helper); \
					} while (0)

/**
 * __build_write_lock_ptr  2.4 x86-32 rwlock 写锁汇编模板
 * @rw: rwlock_t 指针，放入eax
 * @helper: 写锁自旋等待辅助函数 write_lock_helper
 * LOCK：lock总线前缀，保证sub是原子读改写
 * RW_LOCK_BIAS_STR：RW_LOCK_BIAS 的字符串形式，2.4一般是0x01000000
 * subl $bias,(%0)：原子减去bias，尝试抢占写锁
 * jnz 2f：结果不等于0，说明抢占失败，跳转到等待分支
 * 1: 写锁获取成功，直接返回
 * .section .text.lock,"ax"：慢路径等待代码放到独立section，保护主路径icache
 * 2: call helper 自旋轮询等待，直到条件满足
 *    jmp 1b 等待看到锁空闲后，回去重新原子竞争写锁
 * .previous：切回原来代码段
 */
#define __build_write_lock_ptr(rw, helper) \
	asm volatile(LOCK "subl $" RW_LOCK_BIAS_STR ",(%0)\n\t" \
		     "jnz 2f\n" \
		     "1:\n" \
		     ".section .text.lock,\"ax\"\n" \
		     "2:\tcall " helper "\n\t" \
		     "jmp 1b\n" \
		     ".previous" \
		     ::"a" (rw) : "memory")

#define __build_write_lock_const(rw, helper) \
	asm volatile(LOCK "subl $" RW_LOCK_BIAS_STR ",(%0)\n\t" \
		     "jnz 2f\n" \
		     "1:\n" \
		     ".section .text.lock,\"ax\"\n" \
		     "2:\tpushl %%eax\n\t" \
		     "leal %0,%%eax\n\t" \
		     "call " helper "\n\t" \
		     "popl %%eax\n\t" \
		     "jmp 1b\n" \
		     ".previous" \
		     :"=m" (*(volatile int *)rw) : : "memory")

#define __build_write_lock(rw, helper)	do { \
						if (__builtin_constant_p(rw)) \
							__build_write_lock_const(rw, helper); \
						else \
							__build_write_lock_ptr(rw, helper); \
					} while (0)

#endif
