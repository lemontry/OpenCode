#ifndef _LINUX_TIMES_H
#define _LINUX_TIMES_H

// 进程CPU时间统计结构，times()系统调用返回该结构数据
struct tms {
    // 本进程用户态CPU耗时（clock_t时钟滴答）
    clock_t tms_utime;
    // 本进程内核态CPU耗时
    clock_t tms_stime;
    // 已终止子进程的用户态CPU总耗时
    clock_t tms_cutime;
    // 已终止子进程的内核态CPU总耗时
    clock_t tms_cstime;
};


#endif
