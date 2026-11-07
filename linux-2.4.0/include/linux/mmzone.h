#ifndef _LINUX_MMZONE_H
#define _LINUX_MMZONE_H

#ifdef __KERNEL__
#ifndef __ASSEMBLY__

#include <linux/config.h>
#include <linux/spinlock.h>
#include <linux/list.h>

/*
 * Free memory management - zoned buddy allocator.
 */

#define MAX_ORDER 10

/**
 * free_area_struct - 伙伴系统中同一阶的空闲页管理结构
 */
typedef struct free_area_struct {
    // 该阶空闲页块链表
    struct list_head    free_list;
    // 伙伴位图，记录伙伴块状态
    unsigned int        *map;
} free_area_t;

struct pglist_data;

/**
 * zone_struct - 内存域描述符，Linux2.4，ZONE_DMA / ZONE_NORMAL / ZONE_HIGHMEM
 */
typedef struct zone_struct {
    /*
     * Commonly accessed fields:
     */
    // zone保护自旋锁
    spinlock_t      lock;
    // 本zone在全局mem_map里的页偏移
    unsigned long       offset;
    // 当前zone空闲页总数
    unsigned long       free_pages;
    // 非活跃干净页计数
    unsigned long       inactive_clean_pages;
    // 非活跃脏页计数
    unsigned long       inactive_dirty_pages;
    // 内存水位：min / low / high
    unsigned long       pages_min, pages_low, pages_high;

    /*
     * free areas of different sizes
     */
    // 非活跃干净页链表
    struct list_head    inactive_clean_list;
    // 伙伴系统，0阶~MAX_ORDER-1阶空闲页
    free_area_t     free_area[MAX_ORDER];

    /*
     * rarely used fields:
     */
    // zone名字，"DMA" / "Normal" / "HighMem"
    char            *name;
    // zone总页数
    unsigned long       size;
    /*
     * Discontig memory support fields.
     */
    // 所属内存节点pgdat
    struct pglist_data  *zone_pgdat;
    // zone起始物理地址
    unsigned long       zone_start_paddr;
    // zone起始页帧号
    unsigned long       zone_start_mapnr;
    // 本zone的page数组起始
    struct page     *zone_mem_map;
} zone_t;

// DMA内存域，ISA设备只能访问低于16MB内存
// 老 ISA 设备，天生硬件缺陷：它的地址线只有 20 根，最多只能寻址 0~16MB 的物理内存。
// 你给它分配一个 20MB 位置的物理页，这个设备根本看不见、没法读写。
#define ZONE_DMA        0
// 普通内存域，可以直接内核线性映射
#define ZONE_NORMAL     1
// 高端内存域，32位内核无法直接线性映射，需要kmap临时映射
#define ZONE_HIGHMEM        2
// 内存域总数
#define MAX_NR_ZONES        3


/*
 * One allocation request operates on a zonelist. A zonelist
 * is a list of zones, the first one is the 'goal' of the
 * allocation, the other zones are fallback zones, in decreasing
 * priority.
 *
 * Right now a zonelist takes up less than a cacheline. We never
 * modify it apart from boot-up, and only a few indices are used,
 * so despite the zonelist table being relatively big, the cache
 * footprint of this construct is very small.
 */
/**
 * zonelist_t - 内存分配优先级链表，描述一次GFP分配尝试zone的顺序
 * 不同gfp_mask对应不同的zonelist，分配时按数组顺序依次尝试zone
 */
typedef struct zonelist_struct {
    // zone指针数组，以NULL作为结束标记
    zone_t * zones [MAX_NR_ZONES+1]; // NULL delimited
    // 该zonelist对应的GFP掩码
    int gfp_mask;
} zonelist_t;

#define NR_GFPINDEX		0x100

struct bootmem_data;
/**
 * pglist_data - NUMA内存节点描述符，Linux2.4
 */
typedef struct pglist_data {
    // 该节点包含的内存域：ZONE_DMA ZONE_NORMAL ZONE_HIGHMEM
    zone_t node_zones[MAX_NR_ZONES];
    // 内存分配优先级链表，GFP不同类型对应不同zonelist
    zonelist_t node_zonelists[NR_GFPINDEX];
    // 当前节点的page数组起始
    struct page *node_mem_map;
    // 有效物理页位图，标记该节点哪些地址有内存
    unsigned long *valid_addr_bitmap;
    // bootmem管理结构体，内核启动阶段内存分配器
    struct bootmem_data *bdata;
    // 节点起始物理地址
    unsigned long node_start_paddr;
    // 节点起始页帧号
    unsigned long node_start_mapnr;
    // 节点总页数
    unsigned long node_size;
    // NUMA节点ID
    int node_id;
    // 下一个节点，构成单向链表
    struct pglist_data *node_next;
} pg_data_t;

// 系统总的NUMA节点数量
extern int numnodes;
// NUMA节点链表头
extern pg_data_t *pgdat_list;


#define memclass(pgzone, tzone)	(((pgzone)->zone_pgdat == (tzone)->zone_pgdat) \
			&& (((pgzone) - (pgzone)->zone_pgdat->node_zones) <= \
			((tzone) - (pgzone)->zone_pgdat->node_zones)))

/*
 * The following two are not meant for general usage. They are here as
 * prototypes for the discontig memory code.
 */
struct page;
extern void show_free_areas_core(pg_data_t *pgdat);
extern void free_area_init_core(int nid, pg_data_t *pgdat, struct page **gmap,
  unsigned long *zones_size, unsigned long paddr, unsigned long *zholes_size,
  struct page *pmap);

extern pg_data_t contig_page_data;

#ifndef CONFIG_DISCONTIGMEM

#define NODE_DATA(nid)		(&contig_page_data)
#define NODE_MEM_MAP(nid)	mem_map

#else /* !CONFIG_DISCONTIGMEM */

#include <asm/mmzone.h>

#endif /* !CONFIG_DISCONTIGMEM */

#define MAP_ALIGN(x)	((((x) % sizeof(mem_map_t)) == 0) ? (x) : ((x) + \
		sizeof(mem_map_t) - ((x) % sizeof(mem_map_t))))

#endif /* !__ASSEMBLY__ */
#endif /* __KERNEL__ */
#endif /* _LINUX_MMZONE_H */
