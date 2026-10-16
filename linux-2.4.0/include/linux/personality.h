#ifndef _PERSONALITY_H
#define _PERSONALITY_H

#include <linux/linkage.h>
#include <linux/ptrace.h>
#include <asm/current.h>

/* Flags for bug emulation. These occupy the top three bytes. */
#define STICKY_TIMEOUTS		0x4000000
#define WHOLE_SECONDS		0x2000000
#define ADDR_LIMIT_32BIT	0x0800000

/* Personality types. These go in the low byte. Avoid using the top bit,
 * it will conflict with error returns.
 */
#define PER_MASK		(0x00ff)
#define PER_LINUX		(0x0000)
#define PER_LINUX_32BIT		(0x0000 | ADDR_LIMIT_32BIT)
#define PER_SVR4		(0x0001 | STICKY_TIMEOUTS)
#define PER_SVR3		(0x0002 | STICKY_TIMEOUTS)
#define PER_SCOSVR3		(0x0003 | STICKY_TIMEOUTS | WHOLE_SECONDS)
#define PER_WYSEV386		(0x0004 | STICKY_TIMEOUTS)
#define PER_ISCR4		(0x0005 | STICKY_TIMEOUTS)
#define PER_BSD			(0x0006)
#define PER_SUNOS		(PER_BSD | STICKY_TIMEOUTS)
#define PER_XENIX		(0x0007 | STICKY_TIMEOUTS)
#define PER_LINUX32		(0x0008)
#define PER_IRIX32              (0x0009 | STICKY_TIMEOUTS) /* IRIX5 32-bit     */
#define PER_IRIXN32             (0x000a | STICKY_TIMEOUTS) /* IRIX6 new 32-bit */
#define PER_IRIX64              (0x000b | STICKY_TIMEOUTS) /* IRIX6 64-bit     */
#define PER_RISCOS		(0x000c)
#define PER_SOLARIS		(0x000d | STICKY_TIMEOUTS)

/* Prototype for an lcall7 syscall handler. */
typedef void (*lcall7_func)(int, struct pt_regs *);


/* Description of an execution domain - personality range supported,
 * lcall7 syscall handler, start up / shut down functions etc.
 * N.B. The name and lcall7 handler must be where they are since the
 * offset of the handler is hard coded in kernel/sys_call.S.
 */

 /**
 * struct exec_domain - 执行域结构体（Linux2.4，kernel/exec_domain.c）
 * 作用：模拟不同UNIX系统ABI行为，兼容其他Unix（SVR4、Solaris、BSD等）编译的二进制程序；
 * 每个task_struct->exec_domain指针指向此结构，进程的personality（个性/兼容模式）归属某个exec_domain
 *
 * @name: 执行域名字字符串，例如 "Linux", "SVR4", "Solaris"
 * @handler: lcall7_func 函数指针，x86老调用门lcall7系统调用入口，旧ABI使用（非int 0x80）
 * @pers_low: 本执行域覆盖personality编号范围下限
 * @pers_high: 本执行域覆盖personality编号范围上限；
 *             当进程task->personality落在 [pers_low, pers_high]，就使用这个exec_domain
 * @signal_map: 信号映射表：外部OS信号号 → Linux内核信号号
 * @signal_invmap: 反向映射：Linux内核信号号 → 外部OS信号号，给信号返回用户态使用
 * @module: 模块指针；很多exec_domain做成可加载内核模块（SVR4/Solaris），用于模块引用计数
 * @next: 全局exec_domain链表指针；所有注册的执行域串成单向链表exec_domains
 */
struct exec_domain {
	// 执行域名字字符串，例如 "Linux", "SVR4", "Solaris"
	const char *name;

	// lcall7_func 函数指针，x86老调用门lcall7系统调用入口，旧ABI使用（非int 0x80）
	lcall7_func handler;

	// pers_low: 本执行域覆盖personality编号范围下限
	// pers_high: 本执行域覆盖personality编号范围上限；
	unsigned char pers_low, pers_high;

	// 信号映射表：外部OS信号号 → Linux内核信号号
	unsigned long * signal_map;

	//  反向映射：Linux内核信号号 → 外部OS信号号，给信号返回用户态使用
	unsigned long * signal_invmap;

	// 模块指针；很多exec_domain做成可加载内核模块（SVR4/Solaris），用于模块引用计数
	struct module * module;

	// 全局exec_domain链表指针；所有注册的执行域串成单向链表exec_domains
	struct exec_domain *next;
};

extern struct exec_domain default_exec_domain;

extern int register_exec_domain(struct exec_domain *it);
extern int unregister_exec_domain(struct exec_domain *it);
#define put_exec_domain(it) \
	if (it && it->module) __MOD_DEC_USE_COUNT(it->module);
#define get_exec_domain(it) \
	if (it && it->module) __MOD_INC_USE_COUNT(it->module);
extern void __set_personality(unsigned long personality);
#define set_personality(pers) do {	\
	if (current->personality != pers) \
		__set_personality(pers); \
} while (0)
asmlinkage long sys_personality(unsigned long personality);

#endif /* _PERSONALITY_H */
