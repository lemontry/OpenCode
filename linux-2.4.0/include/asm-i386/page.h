#ifndef _I386_PAGE_H
#define _I386_PAGE_H

/* PAGE_SHIFT determines the page size */
#define PAGE_SHIFT	12
#define PAGE_SIZE	(1UL << PAGE_SHIFT)
#define PAGE_MASK	(~(PAGE_SIZE-1))

#ifdef __KERNEL__
#ifndef __ASSEMBLY__

#include <linux/config.h>

#ifdef CONFIG_X86_USE_3DNOW

#include <asm/mmx.h>

#define clear_page(page)	mmx_clear_page(page)
#define copy_page(to,from)	mmx_copy_page(to,from)

#else

/*
 *	On older X86 processors its not a win to use MMX here it seems.
 *	Maybe the K6-III ?
 */
 
#define clear_page(page)	memset((void *)(page), 0, PAGE_SIZE)
#define copy_page(to,from)	memcpy((void *)(to), (void *)(from), PAGE_SIZE)

#endif

#define clear_user_page(page, vaddr)	clear_page(page)
#define copy_user_page(to, from, vaddr)	copy_page(to, from)

/*
 * These are used to make use of C type-checking..
 */
/**
 * 页表项类型定义，区分是否开启PAE物理地址扩展
 * PAE：Physical Address Extension，x86 32位用来支持超过4GB物理内存
 */
#if CONFIG_X86_PAE
// PAE开启：pte拆成低32位、高32位，合计64bit
typedef struct { unsigned long pte_low, pte_high; } pte_t;
// pmd、pgd直接64bit
typedef struct { unsigned long long pmd; } pmd_t;
typedef struct { unsigned long long pgd; } pgd_t;
// 取出pte原始数值，拼接高低32位
#define pte_val(x)  ((x).pte_low | ((unsigned long long)(x).pte_high << 32))
#else
// PAE关闭（普通32位非PAE）：pte仅32bit
typedef struct { unsigned long pte_low; } pte_t;
typedef struct { unsigned long pmd; } pmd_t;
typedef struct { unsigned long pgd; } pgd_t;
// 直接取出32位pte值
#define pte_val(x)  ((x).pte_low)
#endif

/**
 * PTE_MASK：页表项中页物理地址掩码，等于PAGE_MASK
 * pgprot_t：页保护属性封装类型，用于隔离页权限标志
 */
#define PTE_MASK    PAGE_MASK

// pgprot_t：页保护属性封装类型
typedef struct { unsigned long pgprot; } pgprot_t;

// 取出pmd_t原始数值
#define pmd_val(x)  ((x).pmd)
// 取出pgd_t原始数值
#define pgd_val(x)  ((x).pgd)
// 取出pgprot_t原始属性值
#define pgprot_val(x)   ((x).pgprot)

// 构造pte_t页表项
#define __pte(x) ((pte_t) { (x) } )
// 构造pmd_t中间页目录项
#define __pmd(x) ((pmd_t) { (x) } )
// 构造pgd_t页全局目录项
#define __pgd(x) ((pgd_t) { (x) } )
// 构造pgprot_t页保护属性
#define __pgprot(x) ((pgprot_t) { (x) } )

#endif /* !__ASSEMBLY__ */

/* to align the pointer to the (next) page boundary */
#define PAGE_ALIGN(addr)	(((addr)+PAGE_SIZE-1)&PAGE_MASK)

/*
 * This handles the memory map.. We could make this a config
 * option, but too many people screw it up, and too few need
 * it.
 *
 * A __PAGE_OFFSET of 0xC0000000 means that the kernel has
 * a virtual address space of one gigabyte, which limits the
 * amount of physical memory you can use to about 950MB. 
 *
 * If you want more physical memory than this then see the CONFIG_HIGHMEM4G
 * and CONFIG_HIGHMEM64G options in the kernel configuration.
 */

// x86 32位 Linux 2.4 内核
// __PAGE_OFFSET：内核虚拟地址空间起始偏移
/**
 * 32 位虚拟地址：`0 ~ 0xFFFFFFFF`，总共 4GB

- **0x00000000 ~ 0xBFFFFFFF（3GB）：用户态虚拟地址空间**
- **0xC0000000 ~ 0xFFFFFFFF（1GB）：内核虚拟地址空间**

`__PAGE_OFFSET = 0xC0000000` 就是**内核空间的起始地址**。
 */
#define __PAGE_OFFSET        (0xC0000000)

#ifndef __ASSEMBLY__

/*
 * Tell the user there is some problem. Beep too, so we can
 * see^H^H^Hhear bugs in early bootup as well!
 */
#define BUG() do { \
	printk("kernel BUG at %s:%d!\n", __FILE__, __LINE__); \
	__asm__ __volatile__(".byte 0x0f,0x0b"); \
} while (0)

#define PAGE_BUG(page) do { \
	BUG(); \
} while (0)

/* Pure 2^n version of get_order */
extern __inline__ int get_order(unsigned long size)
{
	int order;

	size = (size-1) >> (PAGE_SHIFT-1);
	order = -1;
	do {
		size >>= 1;
		order++;
	} while (size);
	return order;
}

#endif /* __ASSEMBLY__ */

#define PAGE_OFFSET		((unsigned long)__PAGE_OFFSET)

// 物理地址 → 内核线性地址（恒等映射区）
/**
 * `__va`：物理地址 + 0xC0000000 → 得到内核虚拟地址
 * `__pa`：内核虚拟地址 - 0xC0000000 → 得到物理地址
 */
#define __pa(x)  ((unsigned long)(x) - __PAGE_OFFSET)
#define __va(x)  ((void *)((unsigned long)(x) + __PAGE_OFFSET))

#define virt_to_page(kaddr)	(mem_map + (__pa(kaddr) >> PAGE_SHIFT))
#define VALID_PAGE(page)	((page - mem_map) < max_mapnr)


#endif /* __KERNEL__ */

#endif /* _I386_PAGE_H */
