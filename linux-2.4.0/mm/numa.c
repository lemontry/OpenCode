/*
 * Written by Kanoj Sarcar, SGI, Aug 1999
 */
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/init.h>
#include <linux/bootmem.h>
#include <linux/mmzone.h>
#include <linux/spinlock.h>

int numnodes = 1;	/* Initialized for UMA platforms */

/**
 * static bootmem_data_t contig_bootmem_data;
 * UMA平台专用的bootmem描述结构体（单节点）
 * bootmem_data_t：bootmem分配器的控制结构，里面存放bootmem位图、管理区间
 * static：仅当前文件可见，全局静态变量，编译时分配
 */
static bootmem_data_t contig_bootmem_data;

/**
 * pg_data_t contig_page_data = { bdata: &contig_bootmem_data };
 * UMA平台唯一的内存节点pgdat（单node）
 * 初始化：把本节点的bdata成员指向上面的contig_bootmem_data
 * 对应NUMA：每个node都会有自己独立的pg_data_t + 独立bootmem_data_t
 *
 * 用途：
 * 普通PC（UMA）没有多个NUMA节点，直接复用这个预定义好的pgdat
 * 前面free_area_init就是传 &contig_page_data 给free_area_init_core
 */
pg_data_t contig_page_data = { bdata: &contig_bootmem_data };

#ifndef CONFIG_DISCONTIGMEM

/*
 * This is meant to be invoked by platforms whose physical memory starts
 * at a considerably higher value than 0. Examples are Super-H, ARM, m68k.
 * Should be invoked with paramters (0, 0, unsigned long *[], start_paddr).
 */
void __init free_area_init_node(int nid, pg_data_t *pgdat, struct page *pmap,
	unsigned long *zones_size, unsigned long zone_start_paddr, 
	unsigned long *zholes_size)
{
	free_area_init_core(0, &contig_page_data, &mem_map, zones_size, 
				zone_start_paddr, zholes_size, pmap);
}

#endif /* !CONFIG_DISCONTIGMEM */

struct page * alloc_pages_node(int nid, int gfp_mask, unsigned long order)
{
#ifdef CONFIG_NUMA
	return __alloc_pages(NODE_DATA(nid)->node_zonelists + gfp_mask, order);
#else
	return alloc_pages(gfp_mask, order);
#endif
}

#ifdef CONFIG_DISCONTIGMEM

#define LONG_ALIGN(x) (((x)+(sizeof(long))-1)&~((sizeof(long))-1))

static spinlock_t node_lock = SPIN_LOCK_UNLOCKED;

void show_free_areas_node(pg_data_t *pgdat)
{
	unsigned long flags;

	spin_lock_irqsave(&node_lock, flags);
	show_free_areas_core(pgdat);
	spin_unlock_irqrestore(&node_lock, flags);
}

/*
 * Nodes can be initialized parallely, in no particular order.
 */
void __init free_area_init_node(int nid, pg_data_t *pgdat, struct page *pmap,
	unsigned long *zones_size, unsigned long zone_start_paddr, 
	unsigned long *zholes_size)
{
	int i, size = 0;
	struct page *discard;

	if (mem_map == (mem_map_t *)NULL)
		mem_map = (mem_map_t *)PAGE_OFFSET;

	free_area_init_core(nid, pgdat, &discard, zones_size, zone_start_paddr,
					zholes_size, pmap);
	pgdat->node_id = nid;

	/*
	 * Get space for the valid bitmap.
	 */
	for (i = 0; i < MAX_NR_ZONES; i++)
		size += zones_size[i];
	size = LONG_ALIGN((size + 7) >> 3);
	pgdat->valid_addr_bitmap = (unsigned long *)alloc_bootmem_node(pgdat, size);
	memset(pgdat->valid_addr_bitmap, 0, size);
}

static struct page * alloc_pages_pgdat(pg_data_t *pgdat, int gfp_mask,
	unsigned long order)
{
	return __alloc_pages(pgdat->node_zonelists + gfp_mask, order);
}

/*
 * This can be refined. Currently, tries to do round robin, instead
 * should do concentratic circle search, starting from current node.
 */
/**
 * alloc_pages - 分配连续2^order个物理页，伙伴系统入口，Linux2.4
 * @gfp_mask: GFP分配标志，控制分配策略、是否允许阻塞、从哪个zone分配
 * @order: 阶数，分配 2^order 个连续物理页
 * 返回值: 成功返回首page描述符指针；失败返回NULL
 */
struct page * alloc_pages(int gfp_mask, unsigned long order)
{
    struct page *ret = 0;
    pg_data_t *start, *temp;
#ifndef CONFIG_NUMA
    unsigned long flags;
    static pg_data_t *next = 0;
#endif

    // order超过伙伴系统最大阶，直接失败
    if (order >= MAX_ORDER)
        return NULL;
#ifdef CONFIG_NUMA
    // NUMA：优先从当前CPU所在节点分配
    temp = NODE_DATA(numa_node_id());
#else
    // UMA：轮询选节点，node_lock保护静态next指针
    spin_lock_irqsave(&node_lock, flags);
    if (!next) next = pgdat_list;
    temp = next;
    next = next->node_next;
    spin_unlock_irqrestore(&node_lock, flags);
#endif
    start = temp;
    // 第一轮：从选中节点开始，顺着链表向后遍历所有节点尝试分配
    while (temp) {
        if ((ret = alloc_pages_pgdat(temp, gfp_mask, order)))
            return(ret);
        temp = temp->node_next;
    }
    // 第二轮：从头再遍历到起始节点前一个节点，补齐一轮轮询
    temp = pgdat_list;
    while (temp != start) {
        if ((ret = alloc_pages_pgdat(temp, gfp_mask, order)))
            return(ret);
        temp = temp->node_next;
    }
    return(0);
}


#endif /* CONFIG_DISCONTIGMEM */
