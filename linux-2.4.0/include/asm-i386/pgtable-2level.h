#ifndef _I386_PGTABLE_2LEVEL_H
#define _I386_PGTABLE_2LEVEL_H

/**
 * 
 [31 22]  [21 12]  [11 0]
 PGD索引  PTE索引  页内偏移
 */
/*
 * traditional i386 two-level paging structure:
 */
// x86 32 Linux2.4 二级页表宏定义
// PGDIR_SHIFT：页全局目录项偏移，一个PGD项覆盖的地址比特数
#define PGDIR_SHIFT 22

// 每个PGD表里面有多少个表项
#define PTRS_PER_PGD    1024

/*
 * the i386 is two-level, so we don't really have any
 * PMD directory physically.
 * i386是二级页表，物理上不存在独立PMD中间页目录
 */
#define PMD_SHIFT   22
#define PTRS_PER_PMD    1

#define PTRS_PER_PTE	1024

#define pte_ERROR(e) \
	printk("%s:%d: bad pte %08lx.\n", __FILE__, __LINE__, (e).pte_low)
#define pmd_ERROR(e) \
	printk("%s:%d: bad pmd %08lx.\n", __FILE__, __LINE__, pmd_val(e))
#define pgd_ERROR(e) \
	printk("%s:%d: bad pgd %08lx.\n", __FILE__, __LINE__, pgd_val(e))

/*
 * The "pgd_xxx()" functions here are trivial for a folded two-level
 * setup: the pgd is never bad, and a pmd always exists (as it's folded
 * into the pgd entry)
 */
extern inline int pgd_none(pgd_t pgd)		{ return 0; }
extern inline int pgd_bad(pgd_t pgd)		{ return 0; }
extern inline int pgd_present(pgd_t pgd)	{ return 1; }
#define pgd_clear(xp)				do { } while (0)

/*
 * Certain architectures need to do special things when PTEs
 * within a page table are directly modified.  Thus, the following
 * hook is made available.
 */
#define set_pte(pteptr, pteval) (*(pteptr) = pteval)
/*
 * (pmds are folded into pgds so this doesnt get actually called,
 * but the define is needed for a generic inline function.)
 */
#define set_pmd(pmdptr, pmdval) (*(pmdptr) = pmdval)
#define set_pgd(pgdptr, pgdval) (*(pgdptr) = pgdval)

#define pgd_page(pgd) \
((unsigned long) __va(pgd_val(pgd) & PAGE_MASK))

extern inline pmd_t * pmd_offset(pgd_t * dir, unsigned long address)
{
	return (pmd_t *) dir;
}
/**
 * pte 基础操作宏，非PAE x86-32，对pte_t进行读取、比较、构造、清零
 */
// 原子读取并清零pte表项
#define ptep_get_and_clear(xp)  __pte(xchg(&(xp)->pte_low, 0))
// 判断两个pte是否相等
#define pte_same(a, b)      ((a).pte_low == (b).pte_low)
// 根据pte得到对应的struct page指针
#define pte_page(x)     (mem_map+((unsigned long)(((x).pte_low >> PAGE_SHIFT))))
// 判断pte为空（没有映射）
#define pte_none(x)     (!(x).pte_low)
// 根据page编号 + 保护属性，构造pte_t
#define __mk_pte(page_nr,pgprot) __pte(((page_nr) << PAGE_SHIFT) | pgprot_val(pgprot))


#endif /* _I386_PGTABLE_2LEVEL_H */
