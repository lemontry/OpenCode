#ifndef _LINUX_PTRACE_H
#define _LINUX_PTRACE_H
/* ptrace.h */
/* structs and defines to help the user use the ptrace system call. */

/* has the defines to get at the registers. */

#define PTRACE_TRACEME         0   // 当前进程让父进程跟踪自己，子进程主动请求被trace
#define PTRACE_PEEKTEXT        1   // 读取被跟踪进程 TEXT 段（代码段）一字长数据
#define PTRACE_PEEKDATA        2   // 读取被跟踪进程 DATA 段（数据段）一字长数据
#define PTRACE_PEEKUSR         3   // 读取被跟踪进程 pt_regs、usr区域（寄存器）
#define PTRACE_POKETEXT        4   // 向被跟踪进程 TEXT 段写入一字长数据
#define PTRACE_POKEDATA        5   // 向被跟踪进程 DATA 段写入一字长数据
#define PTRACE_POKEUSR         6   // 向被跟踪进程 USR区域写入（修改寄存器）
#define PTRACE_CONT            7   // 恢复被跟踪进程继续运行
#define PTRACE_KILL            8   // 杀掉被跟踪进程
#define PTRACE_SINGLESTEP      9   // 单步执行一条指令，然后停下给tracer通知

#define PTRACE_ATTACH       0x10   // tracer 附加到一个已经存在的目标进程（主动attach）
#define PTRACE_DETACH       0x11   // 解除跟踪，目标进程恢复正常运行

#define PTRACE_SYSCALL        24   // 系统调用跟踪：进入/退出syscall时都会停下通知tracer

#include <asm/ptrace.h>

#endif
