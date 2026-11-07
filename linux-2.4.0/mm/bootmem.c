/*
 *  linux/mm/initmem.c
 *
 *  Copyright (C) 1999 Ingo Molnar
 *  Discontiguous memory support, Kanoj Sarcar, SGI, Nov 1999
 *
 *  simple boot-time physical memory area allocator and
 *  free memory collector. It's used to deal with reserved
 *  system memory and memory holes as well.
 */

#include <linux/mm.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/swapctl.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/bootmem.h>
#include <linux/mmzone.h>
#include <asm/dma.h>

/*
 * Access to this subsystem has to be serialized externally. (this is
 * true for the boot process anyway)
 */
/**
 * min_low_pfn：低端内存最小页帧号（low memory 起始pfn）
 * max_low_pfn：低端内存最大页帧号（low memory 末尾pfn + 1）
 * 单位：pfn（page frame number，页帧号，物理地址 >> PAGE_SHIFT）
 * 适用：Linux2.4 x86 32位，区分 ZONE_NORMAL / ZONE_HIGHMEM
 * 
低端内存 Low Memory（ZONE_DMA + ZONE_NORMAL）
物理地址范围：**0 ~ 896MB**，对应 pfn：`min_low_pfn ~ max_low_pfn - 1`
为什么叫 “低端”：物理地址在低地址区间，在内核的永久线性映射窗口里面。

高端内存 High Memory（ZONE_HIGHMEM）
物理地址：**大于 896MB 的物理内存**，pfn >= max_low_pfn
❌ 特点：
- **没有永久线性映射！**
内核那 896M 的虚拟窗口已经用完了，没法一一固定映射到这部分物理内存
- 内核不能直接 `__va()` 访问，直接转换会地址越界、出错
- 如果要访问里面的内容，必须**临时映射**：`kmap() / kmap_atomic()`
  - kmap：从剩下 128M 内核虚拟地址里拿一个空位，临时绑定这个物理页
  - 用完要 kunmap 释放这个虚拟槽位，不然映射槽耗尽
- 只有 32 位系统才有 HIGHMEM 概念；64 位内核虚拟地址极大，不存在高端内存
 */
unsigned long max_low_pfn;
unsigned long min_low_pfn;

/* return the number of _pages_ that will be allocated for the boot bitmap */
unsigned long __init bootmem_bootmap_pages (unsigned long pages)
{
	unsigned long mapsize;

	mapsize = (pages+7)/8;
	mapsize = (mapsize + ~PAGE_MASK) & PAGE_MASK;
	mapsize >>= PAGE_SHIFT;

	return mapsize;
}

/*
 * Called once to set up the allocator itself.
 */
/**
 * init_bootmem_core
 * @pgdat: 待初始化的NUMA/UMA节点描述符 pg_data_t
 * @mapstart: bootmem位图本身存放的起始pfn
 * @start: 本bootmem管理区间起始pfn
 * @end: 本bootmem管理区间结束pfn（开区间）
 * __init: 初始化函数，内核启动完毕后这段代码可以被回收
 * 返回值：bootmem位图占用的字节大小
 *
 * 功能：
 * 1. 将当前pgdat节点头插入全局pgdat_list链表（头插法）
 * 2. 计算bootmem位图需要多大内存
 * 3. 填充bootmem_data_t的管理信息
 * 4. 位图初始全部置0xff（所有bit=1，默认所有页面标记为【已占用/保留】）
 *
 * 重要约定：
 * 刚初始化完位图时全部标记为占用；后续必须显式调用 free_bootmem()
 * 把真正空闲的RAM区域对应的bit清零，才代表页面空闲。
 */
static unsigned long __init init_bootmem_core (pg_data_t *pgdat,
    unsigned long mapstart, unsigned long start, unsigned long end)
{
    bootmem_data_t *bdata = pgdat->bdata;
    // 总pfn数量：end - start；每一页占用1bit，+7是向上按8bit对齐（凑整字节）
    unsigned long mapsize = ((end - start)+7)/8;

    // ========= 挂载pgdat到全局pgdat_list链表（头插法）=========
    pgdat->node_next = pgdat_list;
    pgdat_list = pgdat;
    // ==========================================================

    // 按 unsigned long 对齐位图大小，保证位图按机器字长访问安全
    mapsize = (mapsize + (sizeof(long) - 1UL)) & ~(sizeof(long) - 1UL);

    // mapstart是pfn，左移PAGE_SHIFT转为物理地址，再转为内核虚拟地址，赋值位图指针
    bdata->node_bootmem_map = phys_to_virt(mapstart << PAGE_SHIFT);
    // node_boot_start：bootmem管理区间起始【物理地址】
    bdata->node_boot_start = (start << PAGE_SHIFT);
    // node_low_pfn：bootmem管理区间结束pfn（bootmem只管低端内存）
    bdata->node_low_pfn = end;

    /*
     * Initially all pages are reserved - setup_arch() has to
     * register free RAM areas explicitly.
     * 初始全部字节填0xff → 所有bit=1：默认全部页面标记为保留、已占用
     * 之后setup_arch解析E820，识别出空闲RAM，调用free_bootmem把对应bit清0
     */
    memset(bdata->node_bootmem_map, 0xff, mapsize);

    return mapsize;
}

/*
 * Marks a particular physical memory range as unallocatable. Usable RAM
 * might be used for boot-time allocations - or it might get added
 * to the free page pool later on.
 */
static void __init reserve_bootmem_core(bootmem_data_t *bdata, unsigned long addr, unsigned long size)
{
	unsigned long i;
	/*
	 * round up, partially reserved pages are considered
	 * fully reserved.
	 */
	unsigned long sidx = (addr - bdata->node_boot_start)/PAGE_SIZE;
	unsigned long eidx = (addr + size - bdata->node_boot_start + 
							PAGE_SIZE-1)/PAGE_SIZE;
	unsigned long end = (addr + size + PAGE_SIZE-1)/PAGE_SIZE;

	if (!size) BUG();

	if (end > bdata->node_low_pfn)
		BUG();
	for (i = sidx; i < eidx; i++)
		if (test_and_set_bit(i, bdata->node_bootmem_map))
			printk("hm, page %08lx reserved twice.\n", i*PAGE_SIZE);
}

static void __init free_bootmem_core(bootmem_data_t *bdata, unsigned long addr, unsigned long size)
{
	unsigned long i;
	unsigned long start;
	/*
	 * round down end of usable mem, partially free pages are
	 * considered reserved.
	 */
	unsigned long sidx;
	unsigned long eidx = (addr + size - bdata->node_boot_start)/PAGE_SIZE;
	unsigned long end = (addr + size)/PAGE_SIZE;

	if (!size) BUG();
	if (end > bdata->node_low_pfn)
		BUG();

	/*
	 * Round up the beginning of the address.
	 */
	start = (addr + PAGE_SIZE-1) / PAGE_SIZE;
	sidx = start - (bdata->node_boot_start/PAGE_SIZE);

	for (i = sidx; i < eidx; i++) {
		if (!test_and_clear_bit(i, bdata->node_bootmem_map))
			BUG();
	}
}

/*
 * We 'merge' subsequent allocations to save space. We might 'lose'
 * some fraction of a page if allocations cannot be satisfied due to
 * size constraints on boxes where there is physical RAM space
 * fragmentation - in these cases * (mostly large memory boxes) this
 * is not a problem.
 *
 * On low memory boxes we get it right in 100% of the cases.
 */

/*
 * alignment has to be a power of 2 value.
 */
static void * __init __alloc_bootmem_core (bootmem_data_t *bdata, 
	unsigned long size, unsigned long align, unsigned long goal)
{
	unsigned long i, start = 0;
	void *ret;
	unsigned long offset, remaining_size;
	unsigned long areasize, preferred, incr;
	unsigned long eidx = bdata->node_low_pfn - (bdata->node_boot_start >>
							PAGE_SHIFT);

	if (!size) BUG();

	/*
	 * We try to allocate bootmem pages above 'goal'
	 * first, then we try to allocate lower pages.
	 */
	if (goal && (goal >= bdata->node_boot_start) && 
			((goal >> PAGE_SHIFT) < bdata->node_low_pfn)) {
		preferred = goal - bdata->node_boot_start;
	} else
		preferred = 0;

	preferred = ((preferred + align - 1) & ~(align - 1)) >> PAGE_SHIFT;
	areasize = (size+PAGE_SIZE-1)/PAGE_SIZE;
	incr = align >> PAGE_SHIFT ? : 1;

restart_scan:
	for (i = preferred; i < eidx; i += incr) {
		unsigned long j;
		if (test_bit(i, bdata->node_bootmem_map))
			continue;
		for (j = i + 1; j < i + areasize; ++j) {
			if (j >= eidx)
				goto fail_block;
			if (test_bit (j, bdata->node_bootmem_map))
				goto fail_block;
		}
		start = i;
		goto found;
	fail_block:;
	}
	if (preferred) {
		preferred = 0;
		goto restart_scan;
	}
found:
	if (start >= eidx)
		BUG();

	/*
	 * Is the next page of the previous allocation-end the start
	 * of this allocation's buffer? If yes then we can 'merge'
	 * the previous partial page with this allocation.
	 */
	if (align <= PAGE_SIZE
	    && bdata->last_offset && bdata->last_pos+1 == start) {
		offset = (bdata->last_offset+align-1) & ~(align-1);
		if (offset > PAGE_SIZE)
			BUG();
		remaining_size = PAGE_SIZE-offset;
		if (size < remaining_size) {
			areasize = 0;
			// last_pos unchanged
			bdata->last_offset = offset+size;
			ret = phys_to_virt(bdata->last_pos*PAGE_SIZE + offset +
						bdata->node_boot_start);
		} else {
			remaining_size = size - remaining_size;
			areasize = (remaining_size+PAGE_SIZE-1)/PAGE_SIZE;
			ret = phys_to_virt(bdata->last_pos*PAGE_SIZE + offset +
						bdata->node_boot_start);
			bdata->last_pos = start+areasize-1;
			bdata->last_offset = remaining_size;
		}
		bdata->last_offset &= ~PAGE_MASK;
	} else {
		bdata->last_pos = start + areasize - 1;
		bdata->last_offset = size & ~PAGE_MASK;
		ret = phys_to_virt(start * PAGE_SIZE + bdata->node_boot_start);
	}
	/*
	 * Reserve the area now:
	 */
	for (i = start; i < start+areasize; i++)
		if (test_and_set_bit(i, bdata->node_bootmem_map))
			BUG();
	memset(ret, 0, size);
	return ret;
}
/**
 * free_all_bootmem_core
 * @pgdat：内存节点描述符（UMA就是&contig_page_data）
 * __init：初始化函数，跑完之后代码可以释放
 * 功能：扫描bootmem位图，把bootmem管理的空闲低端页移交伙伴系统；
 *       最后把bootmem位图本身占用的页面也释放还给伙伴系统
 * 返回值：一共释放了多少页（包含业务空闲页 + bootmem位图占用页）
 */
static unsigned long __init free_all_bootmem_core(pg_data_t *pgdat)
{
    // 拿到本节点的page数组起始、bootmem控制块
    struct page *page = pgdat->node_mem_map;
    bootmem_data_t *bdata = pgdat->bdata;
    unsigned long i, count, total = 0;
    unsigned long idx;

    // 合法性检查：bootmem位图必须存在
    if (!bdata->node_bootmem_map) BUG();

    count = 0;
    // idx：bootmem一共管理多少个pfn（位图总bit数量）
    // node_boot_start：bootmem管理区间起始物理地址
    // node_low_pfn：bootmem管理区间结束pfn
    idx = bdata->node_low_pfn - (bdata->node_boot_start >> PAGE_SHIFT);

    // 遍历bootmem位图的每一位
    for (i = 0; i < idx; i++, page++) {
        // test_bit(i, 位图)：bit=0 代表该页空闲，没有被bootmem分配出去
        if (!test_bit(i, bdata->node_bootmem_map)) {
            count++;
            // 清除保留标记：不再属于内核预留，伙伴系统可以分配
            ClearPageReserved(page);
            // 引用计数初始化为1
            set_page_count(page, 1);
            // 核心：把这个page释放进伙伴系统free_area链表，触发伙伴合并逻辑
            __free_page(page);
        }
    }
    total += count;

    /*
     * Now free the allocator bitmap itself, it's not
     * needed anymore:
     */
    // 上面把内存移交完之后，bootmem位图本身已经没用了，把位图占用的页面也释放
    page = virt_to_page(bdata->node_bootmem_map);
    count = 0;
    // 计算位图一共占用多少页
    for (i = 0; i < ((bdata->node_low_pfn-(bdata->node_boot_start >> PAGE_SHIFT))/8 + PAGE_SIZE-1)/PAGE_SIZE; i++,page++) {
        count++;
        ClearPageReserved(page);
        set_page_count(page, 1);
        __free_page(page);
    }
    total += count;
    // 置空指针，标记bootmem位图失效，bootmem分配器退役
    bdata->node_bootmem_map = NULL;

    return total;
}

unsigned long __init init_bootmem_node (pg_data_t *pgdat, unsigned long freepfn, unsigned long startpfn, unsigned long endpfn)
{
	return(init_bootmem_core(pgdat, freepfn, startpfn, endpfn));
}

void __init reserve_bootmem_node (pg_data_t *pgdat, unsigned long physaddr, unsigned long size)
{
	reserve_bootmem_core(pgdat->bdata, physaddr, size);
}

void __init free_bootmem_node (pg_data_t *pgdat, unsigned long physaddr, unsigned long size)
{
	return(free_bootmem_core(pgdat->bdata, physaddr, size));
}

unsigned long __init free_all_bootmem_node (pg_data_t *pgdat)
{
	return(free_all_bootmem_core(pgdat));
}

unsigned long __init init_bootmem (unsigned long start, unsigned long pages)
{
	max_low_pfn = pages;
	min_low_pfn = start;
	return(init_bootmem_core(&contig_page_data, start, 0, pages));
}

void __init reserve_bootmem (unsigned long addr, unsigned long size)
{
	reserve_bootmem_core(contig_page_data.bdata, addr, size);
}

void __init free_bootmem (unsigned long addr, unsigned long size)
{
	return(free_bootmem_core(contig_page_data.bdata, addr, size));
}

/**
 * free_all_bootmem - UMA平台的包装函数
 * __init：初始化函数，启动完成后代码内存会被释放
 * 功能：把当前UMA节点bootmem管理的空闲页全部移交伙伴系统
 * 返回值：成功释放、加入伙伴系统的页面总数量
 */
unsigned long __init free_all_bootmem (void)
{
    // UMA只有唯一节点 contig_page_data，直接传给核心实现
    return(free_all_bootmem_core(&contig_page_data));
}

void * __init __alloc_bootmem (unsigned long size, unsigned long align, unsigned long goal)
{
	pg_data_t *pgdat = pgdat_list;
	void *ptr;

	while (pgdat) {
		if ((ptr = __alloc_bootmem_core(pgdat->bdata, size,
						align, goal)))
			return(ptr);
		pgdat = pgdat->node_next;
	}
	/*
	 * Whoops, we cannot satisfy the allocation request.
	 */
	BUG();
	return NULL;
}

void * __init __alloc_bootmem_node (pg_data_t *pgdat, unsigned long size, unsigned long align, unsigned long goal)
{
	void *ptr;

	ptr = __alloc_bootmem_core(pgdat->bdata, size, align, goal);
	if (ptr)
		return (ptr);

	/*
	 * Whoops, we cannot satisfy the allocation request.
	 */
	BUG();
	return NULL;
}

