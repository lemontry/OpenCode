/*
 * linux/arch/i386/mm/extable.c
 */

#include <linux/config.h>
#include <linux/module.h>
#include <asm/uaccess.h>

//异常修复表
/**
 * 在这个表中
 *   __start___ex_table = .;
  __ex_table : { *(__ex_table) }
  __stop___ex_table = .;

  一般在汇编代码里面通过 section __ex_table 汇编定义段数据
 * 
 */
extern const struct exception_table_entry __start___ex_table[];
extern const struct exception_table_entry __stop___ex_table[];

//查找修复表
static inline unsigned long
search_one_table(const struct exception_table_entry *first,
		 const struct exception_table_entry *last,
		 unsigned long value)
{
        while (first <= last) {
		const struct exception_table_entry *mid;
		long diff;

		mid = (last - first) / 2 + first;
		diff = mid->insn - value;
                if (diff == 0)
                        return mid->fixup;
                else if (diff < 0)
                        first = mid+1;
                else
                        last = mid-1;
        }
        return 0;
}

/**
 * search_exception_table - 在内核异常修复表(exception table)查找异常地址对应的修复地址
 * @addr: 触发异常的指令eip地址
 * 返回：成功返回修复跳转地址；找不到返回0
 *
 * Linux2.4 arch/i386/mm/exception.c
 * 作用：内核态发生异常时，查询预先构建的异常修复表，找到异常发生后要跳转的修复地址
 */
unsigned long
search_exception_table(unsigned long addr)
{
    unsigned long ret;
#ifndef CONFIG_MODULES
    /* 未开启模块支持：只有内核本体一张异常表 */
    ret = search_one_table(__start___ex_table, __stop___ex_table-1, addr);
    if (ret) return ret;
#else
    /* 开启内核模块：遍历内核 + 所有加载的模块，每个模块都自带独立异常表 */
    struct module *mp;
    for (mp = module_list; mp != NULL; mp = mp->next) {
        if (mp->ex_table_start == NULL)
            continue;
        ret = search_one_table(mp->ex_table_start,
                    mp->ex_table_end - 1, addr);
        if (ret) return ret;
    }
#endif
    return 0;
}
