/*
 *  linux/arch/i386/mm/init.c
 *
 *  Copyright (C) 1995  Linus Torvalds
 *
 *  Support of BIGMEM added by Gerhard Wichert, Siemens AG, July 1999
 */

#include <linux/config.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/ptrace.h>
#include <linux/mman.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/smp.h>
#include <linux/init.h>
#ifdef CONFIG_BLK_DEV_INITRD
#include <linux/blk.h>
#endif
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/bootmem.h>

#include <asm/processor.h>
#include <asm/system.h>
#include <asm/uaccess.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/dma.h>
#include <asm/fixmap.h>
#include <asm/e820.h>
#include <asm/apic.h>

unsigned long highstart_pfn, highend_pfn;
static unsigned long totalram_pages;
static unsigned long totalhigh_pages;

/*
 * BAD_PAGE is the page that is used for page faults when linux
 * is out-of-memory. Older versions of linux just did a
 * do_exit(), but using this instead means there is less risk
 * for a process dying in kernel mode, possibly leaving an inode
 * unused etc..
 *
 * BAD_PAGETABLE is the accompanying page-table: it is initialized
 * to point to BAD_PAGE entries.
 *
 * ZERO_PAGE is a special page that is used for zero-initialized
 * data and COW.
 */

/*
 * These are allocated in head.S so that we get proper page alignment.
 * If you change the size of these then change head.S as well.
 */
extern char empty_bad_page[PAGE_SIZE];
#if CONFIG_X86_PAE
extern pmd_t empty_bad_pmd_table[PTRS_PER_PMD];
#endif
extern pte_t empty_bad_pte_table[PTRS_PER_PTE];

/*
 * We init them before every return and make them writable-shared.
 * This guarantees we get out of the kernel in some more or less sane
 * way.
 */
#if CONFIG_X86_PAE
static pmd_t * get_bad_pmd_table(void)
{
	pmd_t v;
	int i;

	set_pmd(&v, __pmd(_PAGE_TABLE + __pa(empty_bad_pte_table)));

	for (i = 0; i < PAGE_SIZE/sizeof(pmd_t); i++)
		empty_bad_pmd_table[i] = v;

	return empty_bad_pmd_table;
}
#endif

static pte_t * get_bad_pte_table(void)
{
	pte_t v;
	int i;

	v = pte_mkdirty(mk_pte_phys(__pa(empty_bad_page), PAGE_SHARED));

	for (i = 0; i < PAGE_SIZE/sizeof(pte_t); i++)
		empty_bad_pte_table[i] = v;

	return empty_bad_pte_table;
}



void __handle_bad_pmd(pmd_t *pmd)
{
	pmd_ERROR(*pmd);
	set_pmd(pmd, __pmd(_PAGE_TABLE + __pa(get_bad_pte_table())));
}

void __handle_bad_pmd_kernel(pmd_t *pmd)
{
	pmd_ERROR(*pmd);
	set_pmd(pmd, __pmd(_KERNPG_TABLE + __pa(get_bad_pte_table())));
}

pte_t *get_pte_kernel_slow(pmd_t *pmd, unsigned long offset)
{
	pte_t *pte;

	pte = (pte_t *) __get_free_page(GFP_KERNEL);
	if (pmd_none(*pmd)) {
		if (pte) {
			clear_page(pte);
			set_pmd(pmd, __pmd(_KERNPG_TABLE + __pa(pte)));
			return pte + offset;
		}
		set_pmd(pmd, __pmd(_KERNPG_TABLE + __pa(get_bad_pte_table())));
		return NULL;
	}
	free_page((unsigned long)pte);
	if (pmd_bad(*pmd)) {
		__handle_bad_pmd_kernel(pmd);
		return NULL;
	}
	return (pte_t *) pmd_page(*pmd) + offset;
}

pte_t *get_pte_slow(pmd_t *pmd, unsigned long offset)
{
	unsigned long pte;

	pte = (unsigned long) __get_free_page(GFP_KERNEL);
	if (pmd_none(*pmd)) {
		if (pte) {
			clear_page((void *)pte);
			set_pmd(pmd, __pmd(_PAGE_TABLE + __pa(pte)));
			return (pte_t *)pte + offset;
		}
		set_pmd(pmd, __pmd(_PAGE_TABLE + __pa(get_bad_pte_table())));
		return NULL;
	}
	free_page(pte);
	if (pmd_bad(*pmd)) {
		__handle_bad_pmd(pmd);
		return NULL;
	}
	return (pte_t *) pmd_page(*pmd) + offset;
}

int do_check_pgt_cache(int low, int high)
{
	int freed = 0;
	if(pgtable_cache_size > high) {
		do {
			if(pgd_quicklist)
				free_pgd_slow(get_pgd_fast()), freed++;
			if(pmd_quicklist)
				free_pmd_slow(get_pmd_fast()), freed++;
			if(pte_quicklist)
				free_pte_slow(get_pte_fast()), freed++;
		} while(pgtable_cache_size > low);
	}
	return freed;
}

/*
 * NOTE: pagetable_init alloc all the fixmap pagetables contiguous on the
 * physical space so we can cache the place of the first one and move
 * around without checking the pgd every time.
 */

#if CONFIG_HIGHMEM
pte_t *kmap_pte;
pgprot_t kmap_prot;

#define kmap_get_fixmap_pte(vaddr)					\
	pte_offset(pmd_offset(pgd_offset_k(vaddr), (vaddr)), (vaddr))

/**
 * kmap_init - 高端内存临时映射区(kmap)初始化函数
 * __init：初始化函数，内核启动完成后，这段代码内存会被释放
 * 适用：Linux 2.4 x86 32位，配合HIGHMEM使用
 * 调用位置：paging_init() 里面，CONFIG_HIGHMEM开启时才会执行
 */
void __init kmap_init(void)
{
    unsigned long kmap_vstart;

    /* cache the first kmap pte */
    // 拿到FIX_KMAP_BEGIN对应的虚拟地址，kmap固定映射区域的起始虚拟地址
    kmap_vstart = __fix_to_virt(FIX_KMAP_BEGIN);
    // 获取这个虚拟地址对应的页表项pte，缓存到全局变量kmap_pte
    // kmap后续做临时映射，就是修改这一组pte，动态绑定不同的高端物理页
    kmap_pte = kmap_get_fixmap_pte(kmap_vstart);

    // 设置kmap映射的页表权限：PAGE_KERNEL，内核可读可写，无用户权限
    kmap_prot = PAGE_KERNEL;
}
#endif /* CONFIG_HIGHMEM */

void show_mem(void)
{
	int i, total = 0, reserved = 0;
	int shared = 0, cached = 0;
	int highmem = 0;

	printk("Mem-info:\n");
	show_free_areas();
	printk("Free swap:       %6dkB\n",nr_swap_pages<<(PAGE_SHIFT-10));
	i = max_mapnr;
	while (i-- > 0) {
		total++;
		if (PageHighMem(mem_map+i))
			highmem++;
		if (PageReserved(mem_map+i))
			reserved++;
		else if (PageSwapCache(mem_map+i))
			cached++;
		else if (page_count(mem_map+i))
			shared += page_count(mem_map+i) - 1;
	}
	printk("%d pages of RAM\n", total);
	printk("%d pages of HIGHMEM\n",highmem);
	printk("%d reserved pages\n",reserved);
	printk("%d pages shared\n",shared);
	printk("%d pages swap cached\n",cached);
	printk("%ld pages in page table cache\n",pgtable_cache_size);
	show_buffers();
}

/* References to section boundaries */

extern char _text, _etext, _edata, __bss_start, _end;
extern char __init_begin, __init_end;

static inline void set_pte_phys (unsigned long vaddr,
			unsigned long phys, pgprot_t flags)
{
	pgprot_t prot;
	pgd_t *pgd;
	pmd_t *pmd;
	pte_t *pte;

	pgd = swapper_pg_dir + __pgd_offset(vaddr);
	if (pgd_none(*pgd)) {
		printk("PAE BUG #00!\n");
		return;
	}
	pmd = pmd_offset(pgd, vaddr);
	if (pmd_none(*pmd)) {
		printk("PAE BUG #01!\n");
		return;
	}
	pte = pte_offset(pmd, vaddr);
	if (pte_val(*pte))
		pte_ERROR(*pte);
	pgprot_val(prot) = pgprot_val(PAGE_KERNEL) | pgprot_val(flags);
	set_pte(pte, mk_pte_phys(phys, prot));

	/*
	 * It's enough to flush this one mapping.
	 * (PGE mappings get flushed as well)
	 */
	__flush_tlb_one(vaddr);
}

void __set_fixmap (enum fixed_addresses idx, unsigned long phys, pgprot_t flags)
{
	unsigned long address = __fix_to_virt(idx);

	if (idx >= __end_of_fixed_addresses) {
		printk("Invalid __set_fixmap\n");
		return;
	}
	set_pte_phys(address, phys, flags);
}

static void __init fixrange_init (unsigned long start, unsigned long end, pgd_t *pgd_base)
{
	pgd_t *pgd;
	pmd_t *pmd;
	pte_t *pte;
	int i, j;
	unsigned long vaddr;

	vaddr = start;
	i = __pgd_offset(vaddr);
	j = __pmd_offset(vaddr);
	pgd = pgd_base + i;

	for ( ; (i < PTRS_PER_PGD) && (vaddr != end); pgd++, i++) {
#if CONFIG_X86_PAE
		if (pgd_none(*pgd)) {
			pmd = (pmd_t *) alloc_bootmem_low_pages(PAGE_SIZE);
			set_pgd(pgd, __pgd(__pa(pmd) + 0x1));
			if (pmd != pmd_offset(pgd, 0))
				printk("PAE BUG #02!\n");
		}
		pmd = pmd_offset(pgd, vaddr);
#else
		pmd = (pmd_t *)pgd;
#endif
		for (; (j < PTRS_PER_PMD) && (vaddr != end); pmd++, j++) {
			if (pmd_none(*pmd)) {
				pte = (pte_t *) alloc_bootmem_low_pages(PAGE_SIZE);
				set_pmd(pmd, __pmd(_KERNPG_TABLE + __pa(pte)));
				if (pte != pte_offset(pmd, 0))
					BUG();
			}
			vaddr += PMD_SIZE;
		}
		j = 0;
	}
}

/**
 * pagetable_init - 初始化内核主页表 swapper_pg_dir，建立内核线性映射，Linux2.4 x86
 * 调用时机：paging_init 内部，切换cr3之前执行
 * 无返回值
 */
static void __init pagetable_init (void)
{
    unsigned long vaddr, end;
    pgd_t *pgd, *pgd_base;
    int i, j, k;
    pmd_t *pmd;
    pte_t *pte;

    /*
     * This can be zero as well - no problem, in that case we exit
     * the loops anyway due to the PTRS_PER_* conditions.
     */
    // max_low_pfn：低端内存最大页号，转换成虚拟地址，作为线性映射的结束边界
    end = (unsigned long)__va(max_low_pfn*PAGE_SIZE);

    // 内核全局页目录基地址，swapper_pg_dir 是内核初始页表
    pgd_base = swapper_pg_dir;
#if CONFIG_X86_PAE
    // PAE模式：先清空所有pgd表项
    for (i = 0; i < PTRS_PER_PGD; i++) {
        pgd = pgd_base + i;
        __pgd_clear(pgd);
    }
#endif
    // PAGE_OFFSET = 0xC0000000，找到内核起始虚拟地址对应的pgd索引
    i = __pgd_offset(PAGE_OFFSET);
    pgd = pgd_base + i;

    // 遍历内核对应的pgd表项，建立内核线性映射
    for (; i < PTRS_PER_PGD; pgd++, i++) {
        vaddr = i*PGDIR_SIZE;
        // 到达低端内存边界，停止构建映射
        if (end && (vaddr >= end))
            break;
#if CONFIG_X86_PAE
        // PAE：分配pmd页，填入pgd
        pmd = (pmd_t *) alloc_bootmem_low_pages(PAGE_SIZE);
        set_pgd(pgd, __pgd(__pa(pmd) + 0x1));
#else
        // 非PAE（标准二级页表）：pmd和pgd是同一个，pmd层是虚拟的，PTRS_PER_PMD=1
        pmd = (pmd_t *)pgd;
#endif
        if (pmd != pmd_offset(pgd, 0))
            BUG();
        // 遍历pmd表项
        for (j = 0; j < PTRS_PER_PMD; pmd++, j++) {
            vaddr = i*PGDIR_SIZE + j*PMD_SIZE;
            if (end && (vaddr >= end))
                break;
            // CPU支持PSE大页（4M页），优先使用大页映射
            if (cpu_has_pse) {
                unsigned long __pe;

                // 开启CR4的PSE位，允许4M大页
                set_in_cr4(X86_CR4_PSE);
                boot_cpu_data.wp_works_ok = 1;
                // 构造pmd表项：内核属性 + PSE标志 + 物理地址（线性映射 vaddr == __pa(vaddr)）
                __pe = _KERNPG_TABLE + _PAGE_PSE + __pa(vaddr);
                // 支持PGE全局页标记，则加上G位，TLB刷新时不刷这个表项
                if (cpu_has_pge) {
                    set_in_cr4(X86_CR4_PGE);
                    __pe += _PAGE_GLOBAL;
                }
                set_pmd(pmd, __pmd(__pe));
                continue;
            }

            // 不支持PSE，分配pte页，建立4K细粒度页表
            pte = (pte_t *) alloc_bootmem_low_pages(PAGE_SIZE);
            set_pmd(pmd, __pmd(_KERNPG_TABLE + __pa(pte)));

            if (pte != pte_offset(pmd, 0))
                BUG();

            // 填充每一个pte，虚拟地址直接映射到相同物理地址（内核线性映射）
            for (k = 0; k < PTRS_PER_PTE; pte++, k++) {
                vaddr = i*PGDIR_SIZE + j*PMD_SIZE + k*PAGE_SIZE;
                if (end && (vaddr >= end))
                    break;
                // 构造pte：虚拟地址对应的物理页，权限PAGE_KERNEL（内核读写）
                *pte = mk_pte_phys(__pa(vaddr), PAGE_KERNEL);
            }
        }
    }

    /*
     * Fixed mappings, only the page table structure has to be
     * created - mappings will be set by set_fixmap():
     */
    // 固定映射区（fixmap）：预先分配页表骨架，后续set_fixmap再填充pte
    vaddr = __fix_to_virt(__end_of_fixed_addresses - 1) & PMD_MASK;
    fixrange_init(vaddr, 0, pgd_base);

#if CONFIG_HIGHMEM
    /*
     * Permanent kmaps:
     */
    // 高端内存永久映射区PKMAP，预先建好页表框架
    vaddr = PKMAP_BASE;
    fixrange_init(vaddr, vaddr + PAGE_SIZE*LAST_PKMAP, pgd_base);

    // 保存pkmap页表起始地址，kmap()会使用这个pte数组
    pgd = swapper_pg_dir + __pgd_offset(vaddr);
    pmd = pmd_offset(pgd, vaddr);
    pte = pte_offset(pmd, vaddr);
    pkmap_page_table = pte;
#endif

#if CONFIG_X86_PAE
    /*
     * Add low memory identity-mappings - SMP needs it when
     * starting up on an AP from real-mode. In the non-PAE
     * case we already have these mappings through head.S.
     * All user-space mappings are explicitly cleared after
     * SMP startup.
     */
    // PAE模式：给AP启动准备低端物理地址恒等映射（虚拟地址=物理地址）
    pgd_base[0] = pgd_base[USER_PTRS_PER_PGD];
#endif
}


void __init zap_low_mappings (void)
{
	int i;
	/*
	 * Zap initial low-memory mappings.
	 *
	 * Note that "pgd_clear()" doesn't do it for
	 * us in this case, because pgd_clear() is a
	 * no-op in the 2-level case (pmd_clear() is
	 * the thing that clears the page-tables in
	 * that case).
	 */
	for (i = 0; i < USER_PTRS_PER_PGD; i++)
#if CONFIG_X86_PAE
		pgd_clear(swapper_pg_dir+i);
#else
		set_pgd(swapper_pg_dir+i, __pgd(0));
#endif
	flush_tlb_all();
}

/**
 * 
 物理内存的初始化：
 start_kernel()
    ↓
setup_arch()          //【第一步：识别物理内存】
    ↓
paging_init()         //【第二步：页表 + mem_map + zone + 伙伴位图初始化】
    ↓
free_area_init()
    ↓
free_area_init_core() // 分配mem_map数组、初始化struct page、zone水位、free_area位图
    ↓
mem_init()            //【第三步：把空闲物理页释放进伙伴系统buddy】
    ↓
free_all_bootmem()
    ↓
do_basic_setup()      // 驱动、子系统初始化（你之前看的device_init、chr_dev_init都在这里）

 */
/*
 * paging_init() sets up the page tables - note that the first 8MB are
 * already mapped by head.S.
 *
 * This routines also unmaps the page at virtual kernel address 0, so
 * that we can trap those pesky NULL-reference errors in the kernel.
 */
/**
 * paging_init - x86 32位页表初始化，建立内核主页表，初始化zone与mem_map，Linux2.4
 * 调用时机：start_kernel -> setup_arch -> paging_init
 * 无返回值
 */
void __init paging_init(void)
{
    // 初始化swapper_pg_dir内核主页目录（内核初始页表）
    pagetable_init();

    // 把swapper_pg_dir物理地址写入cr3，切换到内核页表
    __asm__( "movl %%ecx,%%cr3\n" ::"c"(__pa(swapper_pg_dir)));

#if CONFIG_X86_PAE
    /*
     * We will bail out later - printk doesnt work right now so
     * the user would just see a hanging kernel.
     */
    // 如果CPU支持PAE，开启CR4的PAE位，启用36位物理地址扩展
    if (cpu_has_pae)
        set_in_cr4(X86_CR4_PAE);
#endif

    // 刷新全部TLB，使新页表生效
    __flush_tlb_all();

#ifdef CONFIG_HIGHMEM
    // 高端内存初始化：kmap/kunmap、pkmap页表
    kmap_init();
#endif
    {
        // 各个zone的页框数量数组：ZONE_DMA、ZONE_NORMAL、ZONE_HIGHMEM
        unsigned long zones_size[MAX_NR_ZONES] = {0, 0, 0};
        unsigned int max_dma, high, low;

        // MAX_DMA_ADDRESS对应的物理页号，ZONE_DMA上限
        max_dma = virt_to_phys((char *)MAX_DMA_ADDRESS) >> PAGE_SHIFT;
        // low：ZONE_NORMAL的末尾pfn（低于__PAGE_OFFSET的最大页号）
        low = max_low_pfn;
        // high：整个物理内存的最大pfn
        high = highend_pfn;

        // 内存全部在DMA区域内
        if (low < max_dma)
            zones_size[ZONE_DMA] = low;
        else {
            // DMA区域：0 ~ max_dma
            zones_size[ZONE_DMA] = max_dma;
            // NORMAL区域：max_dma ~ max_low_pfn
            zones_size[ZONE_NORMAL] = low - max_dma;
#ifdef CONFIG_HIGHMEM
            // HIGHMEM区域：max_low_pfn ~ highend_pfn（没有线性映射）
            zones_size[ZONE_HIGHMEM] = high - low;
#endif
        }
        // 核心：初始化zone、分配mem_map数组、初始化所有struct page
        free_area_init(zones_size);
    }
    return;
}


/*
 * Test if the WP bit works in supervisor mode. It isn't supported on 386's
 * and also on some strange 486's (NexGen etc.). All 586+'s are OK. The jumps
 * before and after the test are here to work-around some nasty CPU bugs.
 */

/*
 * This function cannot be __init, since exceptions don't work in that
 * section.
 */
static int do_test_wp_bit(unsigned long vaddr);

void __init test_wp_bit(void)
{
/*
 * Ok, all PSE-capable CPUs are definitely handling the WP bit right.
 */
	const unsigned long vaddr = PAGE_OFFSET;
	pgd_t *pgd;
	pmd_t *pmd;
	pte_t *pte, old_pte;

	printk("Checking if this processor honours the WP bit even in supervisor mode... ");

	pgd = swapper_pg_dir + __pgd_offset(vaddr);
	pmd = pmd_offset(pgd, vaddr);
	pte = pte_offset(pmd, vaddr);
	old_pte = *pte;
	*pte = mk_pte_phys(0, PAGE_READONLY);
	local_flush_tlb();

	boot_cpu_data.wp_works_ok = do_test_wp_bit(vaddr);

	*pte = old_pte;
	local_flush_tlb();

	if (!boot_cpu_data.wp_works_ok) {
		printk("No.\n");
#ifdef CONFIG_X86_WP_WORKS_OK
		panic("This kernel doesn't support CPU's with broken WP. Recompile it for a 386!");
#endif
	} else {
		printk("Ok.\n");
	}
}

static inline int page_is_ram (unsigned long pagenr)
{
	int i;

	for (i = 0; i < e820.nr_map; i++) {
		unsigned long addr, end;

		if (e820.map[i].type != E820_RAM)	/* not usable memory */
			continue;
		/*
		 *	!!!FIXME!!! Some BIOSen report areas as RAM that
		 *	are not. Notably the 640->1Mb area. We need a sanity
		 *	check here.
		 */
		addr = (e820.map[i].addr+PAGE_SIZE-1) >> PAGE_SHIFT;
		end = (e820.map[i].addr+e820.map[i].size) >> PAGE_SHIFT;
		if  ((pagenr >= addr) && (pagenr < end))
			return 1;
	}
	return 0;
}

/**
 * mem_init - 内存管理初始化收尾函数
 * __init：初始化函数，执行完成后该段代码占用内存后续可以释放
 * 调用时机：start_kernel() 中，paging_init、free_area_init 之后
 * 核心作用：
 * 1. 设定全局内存边界变量
 * 2. 清零零页 empty_zero_page
 * 3. 把 bootmem 管理的低端内存释放到伙伴系统 free_area
 * 4. 处理高端内存 page 标记、释放进伙伴系统
 * 5. 统计内核代码/数据/init段大小、打印内存信息
 * 6. 可选：销毁低端页表映射、检测CPU WP位、PAE合法性检查
 *
 * 时序重点：
 * free_area_init_core 只是搭建zone、伙伴系统链表框架；
 * mem_init 才真正把空闲物理页交到伙伴系统，此后alloc_pages才可以正常分配内存
 */
void __init mem_init(void)
{
    int codesize, reservedpages, datasize, initsize;
    int tmp;

    // 合法性检查：mem_map 必须已经分配好，是所有物理页struct page数组
    if (!mem_map)
        BUG();

#ifdef CONFIG_HIGHMEM
    // 开启高端内存：highmem_start_page 指向高端内存第一个page描述符
    highmem_start_page = mem_map + highstart_pfn;
    // max_mapnr：最大有效页帧号；num_physpages：整机物理总页数；highend_pfn是最高pfn
    max_mapnr = num_physpages = highend_pfn;
#else
    // 无HIGHMEM，全部是低端内存，上限就是max_low_pfn
    max_mapnr = num_physpages = max_low_pfn;
#endif
    // high_memory：低端内存线性映射区的虚拟地址上限（__va转换，896M边界）
    high_memory = (void *) __va(max_low_pfn * PAGE_SIZE);

    /* clear the zero-page */
    // 清零全局零页，匿名缺页、COW时会用到ZERO_PAGE
    memset(empty_zero_page, 0, PAGE_SIZE);

    /* this will put all low memory onto the freelists */
    // free_all_bootmem：遍历bootmem位图，把bootmem里标记空闲的低端页释放进伙伴系统
    // 返回释放的页数，累加到总可用内存 totalram_pages
    totalram_pages += free_all_bootmem();

    reservedpages = 0;
    // 遍历全部低端内存pfn，统计【属于RAM、并且标记为Reserved保留】的页面数量
    // Reserved页：内核镜像、内核数据、BIOS、设备占用等，不允许伙伴系统分配
    for (tmp = 0; tmp < max_low_pfn; tmp++)
        /*
         * Only count reserved RAM pages
         */
        if (page_is_ram(tmp) && PageReserved(mem_map+tmp))
            reservedpages++;

#ifdef CONFIG_HIGHMEM
    // 遍历所有高端内存pfn
    for (tmp = highstart_pfn; tmp < highend_pfn; tmp++) {
        struct page *page = mem_map + tmp;

        // 不是有效RAM的页，标记为保留页，不参与分配
        if (!page_is_ram(tmp)) {
            SetPageReserved(page);
            continue;
        }
        // 有效RAM：清除保留标记
        ClearPageReserved(page);
        // 打上PG_highmem标志，标记这是高端内存页
        set_bit(PG_highmem, &page->flags);
        // 引用计数初始化为1
        atomic_set(&page->count, 1);
        // 把高端页释放进伙伴系统free_area
        __free_page(page);
        // 高端内存页数统计
        totalhigh_pages++;
    }
    // 高端页计入总内存
    totalram_pages += totalhigh_pages;
#endif

    // 计算内核各段大小（虚拟地址，链接脚本符号）
    codesize =  (unsigned long) &_etext - (unsigned long) &_text;    // .text 代码段大小
    datasize =  (unsigned long) &_edata - (unsigned long) &_etext;    // .data 初始化数据段大小
    initsize =  (unsigned long) &__init_end - (unsigned long) &__init_begin; // __init段大小

    // 打印内核启动内存信息，就是dmesg里看到的Memory: xxx available
    printk("Memory: %luk/%luk available (%dk kernel code, %dk reserved, %dk data, %dk init, %ldk highmem)\n",
        (unsigned long) nr_free_pages() << (PAGE_SHIFT-10),    // 当前空闲页数
        max_mapnr << (PAGE_SHIFT-10),                         // 整机物理内存总大小
        codesize >> 10,                                       // 内核代码 KB
        reservedpages << (PAGE_SHIFT-10),                     // 保留页 KB
        datasize >> 10,                                       // data段 KB
        initsize >> 10,                                       // init段 KB
        (unsigned long) (totalhigh_pages << (PAGE_SHIFT-10))  // highmem 总大小 KB
        );

#if CONFIG_X86_PAE
    // 开启PAE内核，但是CPU不支持PAE，直接panic崩溃
    if (!cpu_has_pae)
        panic("cannot execute a PAE-enabled kernel on a PAE-less CPU!");
#endif
    // 测试CPU的WP写保护位是否正常
    if (boot_cpu_data.wp_works_ok < 0)
        test_wp_bit();

    /*
     * Subtle. SMP is doing it's boot stuff late (because it has to
     * fork idle threads) - but it also needs low mappings for the
     * protected-mode entry to work. We zap these entries only after
     * the WP-bit has been tested.
     */
#ifndef CONFIG_SMP
    // 非SMP：销毁低端临时页表映射，节省页表资源；SMP需要保留低端映射给AP启动，所以不执行
    zap_low_mappings();
#endif

}

/* Put this after the callers, so that it cannot be inlined */
static int do_test_wp_bit(unsigned long vaddr)
{
	char tmp_reg;
	int flag;

	__asm__ __volatile__(
		"	movb %0,%1	\n"
		"1:	movb %1,%0	\n"
		"	xorl %2,%2	\n"
		"2:			\n"
		".section __ex_table,\"a\"\n"
		"	.align 4	\n"
		"	.long 1b,2b	\n"
		".previous		\n"
		:"=m" (*(char *) vaddr),
		 "=q" (tmp_reg),
		 "=r" (flag)
		:"2" (1)
		:"memory");
	
	return flag;
}

void free_initmem(void)
{
	unsigned long addr;

	addr = (unsigned long)(&__init_begin);
	for (; addr < (unsigned long)(&__init_end); addr += PAGE_SIZE) {
		ClearPageReserved(virt_to_page(addr));
		set_page_count(virt_to_page(addr), 1);
		free_page(addr);
		totalram_pages++;
	}
	printk ("Freeing unused kernel memory: %dk freed\n", (&__init_end - &__init_begin) >> 10);
}

#ifdef CONFIG_BLK_DEV_INITRD
void free_initrd_mem(unsigned long start, unsigned long end)
{
	if (start < end)
		printk ("Freeing initrd memory: %ldk freed\n", (end - start) >> 10);
	for (; start < end; start += PAGE_SIZE) {
		ClearPageReserved(virt_to_page(start));
		set_page_count(virt_to_page(start), 1);
		free_page(start);
		totalram_pages++;
	}
}
#endif

void si_meminfo(struct sysinfo *val)
{
	val->totalram = totalram_pages;
	val->sharedram = 0;
	val->freeram = nr_free_pages();
	val->bufferram = atomic_read(&buffermem_pages);
	val->totalhigh = totalhigh_pages;
	val->freehigh = nr_free_highpages();
	val->mem_unit = PAGE_SIZE;
	return;
}
