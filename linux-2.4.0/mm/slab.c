/*
 * linux/mm/slab.c
 * Written by Mark Hemment, 1996/97.
 * (markhe@nextd.demon.co.uk)
 *
 * kmem_cache_destroy() + some cleanup - 1999 Andrea Arcangeli
 *
 * Major cleanup, different bufctl logic, per-cpu arrays
 *	(c) 2000 Manfred Spraul
 *
 * An implementation of the Slab Allocator as described in outline in;
 *	UNIX Internals: The New Frontiers by Uresh Vahalia
 *	Pub: Prentice Hall	ISBN 0-13-101908-2
 * or with a little more detail in;
 *	The Slab Allocator: An Object-Caching Kernel Memory Allocator
 *	Jeff Bonwick (Sun Microsystems).
 *	Presented at: USENIX Summer 1994 Technical Conference
 *
 *
 * The memory is organized in caches, one cache for each object type.
 * (e.g. inode_cache, dentry_cache, buffer_head, vm_area_struct)
 * Each cache consists out of many slabs (they are small (usually one
 * page long) and always contiguous), and each slab contains multiple
 * initialized objects.
 *
 * Each cache can only support one memory type (GFP_DMA, GFP_HIGHMEM,
 * normal). If you need a special memory type, then must create a new
 * cache for that memory type.
 *
 * In order to reduce fragmentation, the slabs are sorted in 3 groups:
 *   full slabs with 0 free objects
 *   partial slabs
 *   empty slabs with no allocated objects
 *
 * If partial slabs exist, then new allocations come from these slabs,
 * otherwise from empty slabs or new slabs are allocated.
 *
 * kmem_cache_destroy() CAN CRASH if you try to allocate from the cache
 * during kmem_cache_destroy(). The caller must prevent concurrent allocs.
 *
 * On SMP systems, each cache has a short per-cpu head array, most allocs
 * and frees go into that array, and if that array overflows, then 1/2
 * of the entries in the array are given back into the global cache.
 * This reduces the number of spinlock operations.
 *
 * The c_cpuarray may not be read with enabled local interrupts.
 *
 * SMP synchronization:
 *  constructors and destructors are called without any locking.
 *  Several members in kmem_cache_t and slab_t never change, they
 *	are accessed without any locking.
 *  The per-cpu arrays are never accessed from the wrong cpu, no locking.
 *  The non-constant members are protected with a per-cache irq spinlock.
 *
 * Further notes from the original documentation:
 *
 * 11 April '97.  Started multi-threading - markhe
 *	The global cache-chain is protected by the semaphore 'cache_chain_sem'.
 *	The sem is only needed when accessing/extending the cache-chain, which
 *	can never happen inside an interrupt (kmem_cache_create(),
 *	kmem_cache_shrink() and kmem_cache_reap()).
 *
 *	To prevent kmem_cache_shrink() trying to shrink a 'growing' cache (which
 *	maybe be sleeping and therefore not holding the semaphore/lock), the
 *	growing field is used.  This also prevents reaping from a cache.
 *
 *	At present, each engine can be growing a cache.  This should be blocked.
 *
 */

#include	<linux/config.h>
#include	<linux/slab.h>
#include	<linux/interrupt.h>
#include	<linux/init.h>
#include	<asm/uaccess.h>

/*
 * DEBUG	- 1 for kmem_cache_create() to honour; SLAB_DEBUG_INITIAL,
 *		  SLAB_RED_ZONE & SLAB_POISON.
 *		  0 for faster, smaller code (especially in the critical paths).
 *
 * STATS	- 1 to collect stats for /proc/slabinfo.
 *		  0 for faster, smaller code (especially in the critical paths).
 *
 * FORCED_DEBUG	- 1 enables SLAB_RED_ZONE and SLAB_POISON (if possible)
 */

#define	DEBUG		0
#define	STATS		0
#define	FORCED_DEBUG	0

/*
 * Parameters for kmem_cache_reap
 */
#define REAP_SCANLEN	10
#define REAP_PERFECT	10

/* Shouldn't this be in a header file somewhere? */
#define	BYTES_PER_WORD		sizeof(void *)

/* Legal flag mask for kmem_cache_create(). */
#if DEBUG
# define CREATE_MASK	(SLAB_DEBUG_INITIAL | SLAB_RED_ZONE | \
			 SLAB_POISON | SLAB_HWCACHE_ALIGN | \
			 SLAB_NO_REAP | SLAB_CACHE_DMA)
#else
# define CREATE_MASK	(SLAB_HWCACHE_ALIGN | SLAB_NO_REAP | SLAB_CACHE_DMA)
#endif

/*
 * kmem_bufctl_t:
 *
 * Bufctl's are used for linking objs within a slab
 * linked offsets.
 *
 * This implementaion relies on "struct page" for locating the cache &
 * slab an object belongs to.
 * This allows the bufctl structure to be small (one int), but limits
 * the number of objects a slab (not a cache) can contain when off-slab
 * bufctls are used. The limit is the size of the largest general cache
 * that does not use off-slab slabs.
 * For 32bit archs with 4 kB pages, is this 56.
 * This is not serious, as it is only for large objects, when it is unwise
 * to have too many per slab.
 * Note: This limit can be raised by introducing a general cache whose size
 * is less than 512 (PAGE_SIZE<<3), but greater than 256.
 */

#define BUFCTL_END 0xffffFFFF
#define	SLAB_LIMIT 0xffffFFFE
typedef unsigned int kmem_bufctl_t;

/* Max number of objs-per-slab for caches which use off-slab slabs.
 * Needed to avoid a possible looping condition in kmem_cache_grow().
 */
static unsigned long offslab_limit;

/*
 * slab_t
 *
 * Manages the objs in a slab. Placed either at the beginning of mem allocated
 * for a slab, or allocated from an general cache.
 * Slabs are chained into one ordered list: fully used, partial, then fully
 * free slabs.
 */
/**
 * slab_t - 描述一个slab块（Linux2.4 SLAB）
 * 一个slab由连续物理页组成，里面存放多个同规格对象
 */
typedef struct slab_s {
    // 挂载到 kmem_cache_t 的 slabs 链表
    struct list_head    list;
    // slab着色偏移，用来错开对象起始地址，降低CPU cache伪共享
    unsigned long       colouroff;
    // slab内第一个对象起始地址（已经加上colouroff着色偏移）
    void                *s_mem;     /* including colour offset */
    // 当前已经分配出去、正在使用的对象数量
    unsigned int        inuse;      /* num of objs active in slab */
    // 空闲对象链表索引，bufctl数组的下标，链式记录空闲对象
    kmem_bufctl_t       free;
} slab_t;


#define slab_bufctl(slabp) \
	((kmem_bufctl_t *)(((slab_t*)slabp)+1))

/*
 * cpucache_t
 *
 * Per cpu structures
 * The limit is stored in the per-cpu structure to reduce the data cache
 * footprint.
 */
typedef struct cpucache_s {
	unsigned int avail;
	unsigned int limit;
} cpucache_t;

#define cc_entry(cpucache) \
	((void **)(((cpucache_t*)cpucache)+1))
#define cc_data(cachep) \
	((cachep)->cpudata[smp_processor_id()])
/*
 * kmem_cache_t
 *
 * manages a cache.
 */

#define CACHE_NAMELEN	20	/* max name length for a slab cache */
/**
 * struct kmem_cache_s （别名 kmem_cache_t）
 * Linux 2.4 SLAB分配器的缓存描述符
 * 每一个slab缓存（cache）对应一个该结构体实例；
 * 例如 cache_cache、kmalloc-32、kmalloc-64、DMA版kmalloc缓存，都各有一个 kmem_cache_t
 */
struct kmem_cache_s {
/* 1) each alloc & free 分配、释放路径高频访问字段 */
    /* full, partial first, then free */
    // slabs：本缓存下所有slab块链表
    // 链表上顺序：满slab → 部分空闲slab → 完全空闲slab
    struct list_head    slabs;
    // firstnotfull：指向第一个【非满】的slab（partial或者free），加速分配查找
    struct list_head    *firstnotfull;
    // objsize：缓存中单个对象的大小（不含slab管理元数据，就是用户申请的大小）
    unsigned int        objsize;
    // flags：静态固定标志位，创建缓存时确定，运行期不变
    unsigned int        flags;
    // num：一块slab里面最多能存放多少个对象
    unsigned int        num;
    // spinlock：保护这个cache的slab链表操作锁
    spinlock_t      spinlock;
#ifdef CONFIG_SMP
    // batchcount：SMP下per-cpu缓存批量移入/移出的数量
    unsigned int        batchcount;
#endif

/* 2) slab additions /removals  slab扩容、回收相关 */
    /* order of pgs per slab (2^n) */
    // gfporder：每个slab占用连续页的阶，页数 = 1 << gfporder
    unsigned int        gfporder;

    /* force GFP flags, e.g. GFP_DMA */
    // gfpflags：分配slab物理页时强制带上的GFP标志，比如GFP_DMA就是DMA专用缓存
    unsigned int        gfpflags;

    // colour：着色总数，一共有多少种着色偏移
    size_t          colour;
    // colour_off：着色步进（偏移单位）
    unsigned int        colour_off;
    // colour_next：下一次新建slab使用的着色编号，轮询
    unsigned int        colour_next;
    // slabp_cache：slab描述符（struct slab）本身从哪个slab缓存分配
    kmem_cache_t        *slabp_cache;
    // growing：标记当前是否正在扩容新建slab，防止并发重复扩容
    unsigned int        growing;
    // dflags：动态标志，运行时变化
    unsigned int        dflags;

    /* constructor func */
    // ctor：对象构造回调函数，新建对象时调用（初始化）
    void (*ctor)(void *, kmem_cache_t *, unsigned long);

    /* de-constructor func */
    // dtor：对象析构回调，回收对象时调用
    void (*dtor)(void *, kmem_cache_t *, unsigned long);

    // failures：分配失败计数
    unsigned long       failures;

/* 3) cache creation/removal 缓存全局管理 */
    // name：缓存名字，比如 "kmem_cache"、"kmalloc-64"
    char            name[CACHE_NAMELEN];
    // next：挂入全局 cache_chain 链表
    struct list_head    next;

#ifdef CONFIG_SMP
/* 4) per-cpu data SMP每CPU本地缓存 */
    // cpudata：每个CPU私有缓存（cpucache_t），提升SMP并发分配性能，减少全局锁争抢
    cpucache_t      *cpudata[NR_CPUS];
#endif

#if STATS
    // 下面都是统计项，打开STATS宏才编译，用于调试观察
    unsigned long       num_active;      // 当前活跃对象
    unsigned long       num_allocations; // 总分配次数
    unsigned long       high_mark;       // 历史最高对象水位
    unsigned long       grown;           // 扩容新建slab次数
    unsigned long       reaped;          // 回收slab次数
    unsigned long       errors;          // 错误次数
#ifdef CONFIG_SMP
    atomic_t        allochit;    // per-cpu缓存分配命中
    atomic_t        allocmiss;   // per-cpu缓存分配未命中
    atomic_t        freehit;     // per-cpu缓存释放命中
    atomic_t        freemiss;    // per-cpu缓存释放未命中
#endif
#endif
};

/* internal c_flags */
#define	CFLGS_OFF_SLAB	0x010000UL	/* slab management in own cache */
#define	CFLGS_OPTIMIZE	0x020000UL	/* optimized slab lookup */

/* c_dflags (dynamic flags). Need to hold the spinlock to access this member */
#define	DFLGS_GROWN	0x000001UL	/* don't reap a recently grown */

#define	OFF_SLAB(x)	((x)->flags & CFLGS_OFF_SLAB)
#define	OPTIMIZE(x)	((x)->flags & CFLGS_OPTIMIZE)
#define	GROWN(x)	((x)->dlags & DFLGS_GROWN)

#if STATS
#define	STATS_INC_ACTIVE(x)	((x)->num_active++)
#define	STATS_DEC_ACTIVE(x)	((x)->num_active--)
#define	STATS_INC_ALLOCED(x)	((x)->num_allocations++)
#define	STATS_INC_GROWN(x)	((x)->grown++)
#define	STATS_INC_REAPED(x)	((x)->reaped++)
#define	STATS_SET_HIGH(x)	do { if ((x)->num_active > (x)->high_mark) \
					(x)->high_mark = (x)->num_active; \
				} while (0)
#define	STATS_INC_ERR(x)	((x)->errors++)
#else
#define	STATS_INC_ACTIVE(x)	do { } while (0)
#define	STATS_DEC_ACTIVE(x)	do { } while (0)
#define	STATS_INC_ALLOCED(x)	do { } while (0)
#define	STATS_INC_GROWN(x)	do { } while (0)
#define	STATS_INC_REAPED(x)	do { } while (0)
#define	STATS_SET_HIGH(x)	do { } while (0)
#define	STATS_INC_ERR(x)	do { } while (0)
#endif

#if STATS && defined(CONFIG_SMP)
#define STATS_INC_ALLOCHIT(x)	atomic_inc(&(x)->allochit)
#define STATS_INC_ALLOCMISS(x)	atomic_inc(&(x)->allocmiss)
#define STATS_INC_FREEHIT(x)	atomic_inc(&(x)->freehit)
#define STATS_INC_FREEMISS(x)	atomic_inc(&(x)->freemiss)
#else
#define STATS_INC_ALLOCHIT(x)	do { } while (0)
#define STATS_INC_ALLOCMISS(x)	do { } while (0)
#define STATS_INC_FREEHIT(x)	do { } while (0)
#define STATS_INC_FREEMISS(x)	do { } while (0)
#endif

#if DEBUG
/* Magic nums for obj red zoning.
 * Placed in the first word before and the first word after an obj.
 */
#define	RED_MAGIC1	0x5A2CF071UL	/* when obj is active */
#define	RED_MAGIC2	0x170FC2A5UL	/* when obj is inactive */

/* ...and for poisoning */
#define	POISON_BYTE	0x5a		/* byte value for poisoning */
#define	POISON_END	0xa5		/* end-byte of poisoning */

#endif

/* maximum size of an obj (in 2^order pages) */
#define	MAX_OBJ_ORDER	5	/* 32 pages */

/*
 * Do not go above this order unless 0 objects fit into the slab.
 */
#define	BREAK_GFP_ORDER_HI	2
#define	BREAK_GFP_ORDER_LO	1
static int slab_break_gfp_order = BREAK_GFP_ORDER_LO;

/*
 * Absolute limit for the gfp order
 */
#define	MAX_GFP_ORDER	5	/* 32 pages */


/* Macros for storing/retrieving the cachep and or slab from the
 * global 'mem_map'. These are used to find the slab an obj belongs to.
 * With kfree(), these are used to find the cache which an obj belongs to.
 */
#define	SET_PAGE_CACHE(pg,x)  ((pg)->list.next = (struct list_head *)(x))
#define	GET_PAGE_CACHE(pg)    ((kmem_cache_t *)(pg)->list.next)
#define	SET_PAGE_SLAB(pg,x)   ((pg)->list.prev = (struct list_head *)(x))
#define	GET_PAGE_SLAB(pg)     ((slab_t *)(pg)->list.prev)

/* Size description struct for general caches. */
/**
 * cache_sizes_t - 预定义slab缓存规格条目，管理一组固定大小的slab缓存，Linux2.4
 */
typedef struct cache_sizes {
	// 该缓存条目对应的对象大小
    size_t          cs_size;
	// 普通内存域(ZONE_NORMAL)的slab缓存描述符
    kmem_cache_t    *cs_cachep;
	//  DMA内存域(ZONE_DMA)的slab缓存描述符
    kmem_cache_t    *cs_dmacachep;
} cache_sizes_t;

/**
 * cache_sizes - 全局数组，kmalloc内置固定大小slab缓存表
 * 数组最后一项cs_size=0作为结束标记
 * PAGE_SIZE=4096时额外增加32字节规格
 */
static cache_sizes_t cache_sizes[] = {
#if PAGE_SIZE == 4096
    {    32,    NULL, NULL},
#endif
    {    64,    NULL, NULL},
    {   128,    NULL, NULL},
    {   256,    NULL, NULL},
    {   512,    NULL, NULL},
    {  1024,    NULL, NULL},
    {  2048,    NULL, NULL},
    {  4096,    NULL, NULL},
    {  8192,    NULL, NULL},
    { 16384,    NULL, NULL},
    { 32768,    NULL, NULL},
    { 65536,    NULL, NULL},
    {131072,    NULL, NULL},
    {     0,    NULL, NULL}
};

/* internal cache of cache description objs */
/**
 * cache_cache：SLAB分配器的【元缓存 / 自举缓存】静态实例
 * 类型：kmem_cache_t
 * 作用：专门用来分配 kmem_cache_t 结构体本身
 * 鸡生蛋问题：要创建其他slab缓存，需要kmem_cache_create；
 * kmem_cache_create内部要分配kmem_cache_t，只能预先静态定义这个cache_cache
 */
static kmem_cache_t cache_cache = {
    // slabs：该缓存下所有slab块的链表头，静态初始化空链表
    slabs:        LIST_HEAD_INIT(cache_cache.slabs),
    // firstnotfull：初始指向slabs链表，代表第一个非满slab的位置；
    // 链表为空时直接指向链表头，分配逻辑以此做判断
    firstnotfull:   &cache_cache.slabs,
    // objsize：这个缓存里面存放的对象大小 = kmem_cache_t结构体本身大小
    objsize:    sizeof(kmem_cache_t),
    // flags：SLAB_NO_REAP 标记：这个缓存不允许kswapd回收slab页面
    // 重要：cache_cache是SLAB根基，一旦回收，所有slab缓存都会瘫痪，禁止回收
    flags:       SLAB_NO_REAP,
    // spinlock：缓存的自旋锁，初始化为未上锁状态
    spinlock:   SPIN_LOCK_UNLOCKED,
    // colour_off：着色偏移步长 = L1_CACHE_BYTES（L1 cache line大小）
    // 着色的单位，每次切换着色就偏移一个cache line，避免硬件cache冲突
    colour_off: L1_CACHE_BYTES,
    // name：缓存名称，dmesg、proc文件系统查看时显示
    name:       "kmem_cache",
};

/* Guard access to the cache-chain. */
static struct semaphore	cache_chain_sem;

/* Place maintainer for reaping. */
static kmem_cache_t *clock_searchp = &cache_cache;

#define cache_chain (cache_cache.next)

#ifdef CONFIG_SMP
/*
 * chicken and egg problem: delay the per-cpu array allocation
 * until the general caches are up.
 */
static int g_cpucache_up;

static void enable_cpucache (kmem_cache_t *cachep);
static void enable_all_cpucaches (void);
#endif

/* Cal the num objs, wastage, and bytes left over for a given slab size. */
static void kmem_cache_estimate (unsigned long gfporder, size_t size,
		 int flags, size_t *left_over, unsigned int *num)
{
	int i;
	size_t wastage = PAGE_SIZE<<gfporder;
	size_t extra = 0;
	size_t base = 0;

	if (!(flags & CFLGS_OFF_SLAB)) {
		base = sizeof(slab_t);
		extra = sizeof(kmem_bufctl_t);
	}
	i = 0;
	while (i*size + L1_CACHE_ALIGN(base+i*extra) <= wastage)
		i++;
	if (i > 0)
		i--;

	if (i > SLAB_LIMIT)
		i = SLAB_LIMIT;

	*num = i;
	wastage -= i*size;
	wastage -= L1_CACHE_ALIGN(base+i*extra);
	*left_over = wastage;
}

/* Initialisation - setup the `cache' cache. */
/**
 * kmem_cache_init - SLAB分配器第一阶段自举初始化
 * __init：初始化函数，启动完成后该段内存可以被回收
 * 调用位置：start_kernel()，setup_arch() 执行完毕之后，mem_init() 之前
 * 前提：当前还在 bootmem 分配器阶段，伙伴系统尚未就绪
 * 核心目标：初始化 cache_cache（专门用来分配 kmem_cache_t 自身的slab缓存），完成slab的自举
 * 说明：cache_cache 是SLAB的“元缓存”——没有它，就无法调用 kmem_cache_create 创建其他缓存
 */
void __init kmem_cache_init(void)
{
    size_t left_over;

    // 初始化互斥锁 cache_chain_sem，保护全局缓存链表 cache_chain
    init_MUTEX(&cache_chain_sem);
    // 初始化全局链表头 cache_chain，系统所有slab缓存（kmem_cache_t）都会挂在这个链表上
    INIT_LIST_HEAD(&cache_chain);

    // kmem_cache_estimate：根据传入的对象大小，估算一个slab块里面最多能放多少个对象
    // 参数：(gfp, objsize, align, &剩余空间left_over, &输出每个slab的对象个数num)
    // cache_cache.objsize = sizeof(kmem_cache_t)，也就是这个缓存用来存放kmem_cache_t结构体
    kmem_cache_estimate(0, cache_cache.objsize, 0,
            &left_over, &cache_cache.num);
    // 估算出来每个slab能容纳的对象数量不能为0，为0直接崩溃
    if (!cache_cache.num)
        BUG();

    // colour：着色数量，slab着色（cache coloring），用来降低硬件cache冲突
    // left_over：一个slab扣除所有对象之后剩下的空闲字节
    // colour_off：每个着色步长
    cache_cache.colour = left_over/cache_cache.colour_off;
    // colour_next：下一次分配使用的着色偏移，轮询使用不同着色
    cache_cache.colour_next = 0;
}

/* Initialisation - setup remaining internal and general caches.
 * Called after the gfp() functions have been enabled, and before smp_init().
 */
/**
 * kmem_cache_sizes_init
 * __init：初始化函数，内核启动完成后该函数所在内存可以被释放
 * 调用时机：start_kernel → mem_init() 之后
 * 作用：遍历全局 cache_sizes[] 数组，批量创建 kmalloc 配套的一系列通用slab缓存
 * 每一个尺寸创建两套缓存：普通缓存、DMA缓存（GFP_DMA分配使用）
 */
void __init kmem_cache_sizes_init(void)
{
    cache_sizes_t *sizes = cache_sizes;
    char name[20];
    /*
     * Fragmentation resistance on low memory - only use bigger
     * page orders on machines with more than 32MB of memory.
     * 碎片控制策略：
     * 物理内存大于32MB时，slab_break_gfp_order = BREAK_GFP_ORDER_HI，允许slab使用更高阶连续页；
     * 小内存机器维持低阶，避免大连续页分配失败、加剧内存碎片
     */
    if (num_physpages > (32 << 20) >> PAGE_SHIFT)
        slab_break_gfp_order = BREAK_GFP_ORDER_HI;

    do {
        /* For performance, all the general caches are L1 aligned.
         * This should be particularly beneficial on SMP boxes, as it
         * eliminates "false sharing".
         * Note for systems short on memory removing the alignment will
         * allow tighter packing of the smaller caches.
         * 所有kmalloc通用缓存开启硬件cache对齐 SLAB_HWCACHE_ALIGN
         * 好处：SMP场景减少cache伪共享（false sharing）；
         * 代价：对象之间有空隙，内存利用率稍微降低
         */
        sprintf(name,"size-%Zd",sizes->cs_size);
        // 创建普通slab缓存，标志 SLAB_HWCACHE_ALIGN
        if (!(sizes->cs_cachep =
            kmem_cache_create(name, sizes->cs_size,
                    0, SLAB_HWCACHE_ALIGN, NULL, NULL))) {
            BUG();
        }

        /* Inc off-slab bufctl limit until the ceiling is hit. */
        // OFF_SLAB：判断这个缓存是否使用【外部管理描述符】（bufctl放在slab页面之外）
        // 如果不是off-slab（bufctl放在slab内部），更新offslab_limit阈值
        // offslab_limit：判断什么时候切换为off-slab模式的阈值
        if (!(OFF_SLAB(sizes->cs_cachep))) {
            offslab_limit = sizes->cs_size-sizeof(slab_t);
            offslab_limit /= 2;
        }

        sprintf(name, "size-%Zd(DMA)",sizes->cs_size);
        // 创建DMA专用缓存，增加 SLAB_CACHE_DMA 标志
        // 分配slab物理页时自动带上 GFP_DMA，从 ZONE_DMA 分配内存
        sizes->cs_dmacachep = kmem_cache_create(name, sizes->cs_size, 0,
                SLAB_CACHE_DMA|SLAB_HWCACHE_ALIGN, NULL, NULL);
        if (!sizes->cs_dmacachep)
            BUG();

        sizes++;
    } while (sizes->cs_size); // 循环直到cs_size为0（数组结束哨兵）
}

int __init kmem_cpucache_init(void)
{
#ifdef CONFIG_SMP
	g_cpucache_up = 1;
	enable_all_cpucaches();
#endif
	return 0;
}

__initcall(kmem_cpucache_init);

/* Interface to system's page allocator. No need to hold the cache-lock.
 */
static inline void * kmem_getpages (kmem_cache_t *cachep, unsigned long flags)
{
	void	*addr;

	/*
	 * If we requested dmaable memory, we will get it. Even if we
	 * did not request dmaable memory, we might get it, but that
	 * would be relatively rare and ignorable.
	 */
	flags |= cachep->gfpflags;
	addr = (void*) __get_free_pages(flags, cachep->gfporder);
	/* Assume that now we have the pages no one else can legally
	 * messes with the 'struct page's.
	 * However vm_scan() might try to test the structure to see if
	 * it is a named-page or buffer-page.  The members it tests are
	 * of no interest here.....
	 */
	return addr;
}

/* Interface to system's page release. */
static inline void kmem_freepages (kmem_cache_t *cachep, void *addr)
{
	unsigned long i = (1<<cachep->gfporder);
	struct page *page = virt_to_page(addr);

	/* free_pages() does not clear the type bit - we do that.
	 * The pages have been unlinked from their cache-slab,
	 * but their 'struct page's might be accessed in
	 * vm_scan(). Shouldn't be a worry.
	 */
	while (i--) {
		PageClearSlab(page);
		page++;
	}
	free_pages((unsigned long)addr, cachep->gfporder);
}

#if DEBUG
static inline void kmem_poison_obj (kmem_cache_t *cachep, void *addr)
{
	int size = cachep->objsize;
	if (cachep->flags & SLAB_RED_ZONE) {
		addr += BYTES_PER_WORD;
		size -= 2*BYTES_PER_WORD;
	}
	memset(addr, POISON_BYTE, size);
	*(unsigned char *)(addr+size-1) = POISON_END;
}

static inline int kmem_check_poison_obj (kmem_cache_t *cachep, void *addr)
{
	int size = cachep->objsize;
	void *end;
	if (cachep->flags & SLAB_RED_ZONE) {
		addr += BYTES_PER_WORD;
		size -= 2*BYTES_PER_WORD;
	}
	end = memchr(addr, POISON_END, size);
	if (end != (addr+size-1))
		return 1;
	return 0;
}
#endif

/* Destroy all the objs in a slab, and release the mem back to the system.
 * Before calling the slab must have been unlinked from the cache.
 * The cache-lock is not held/needed.
 */
static void kmem_slab_destroy (kmem_cache_t *cachep, slab_t *slabp)
{
	if (cachep->dtor
#if DEBUG
		|| cachep->flags & (SLAB_POISON | SLAB_RED_ZONE)
#endif
	) {
		int i;
		for (i = 0; i < cachep->num; i++) {
			void* objp = slabp->s_mem+cachep->objsize*i;
#if DEBUG
			if (cachep->flags & SLAB_RED_ZONE) {
				if (*((unsigned long*)(objp)) != RED_MAGIC1)
					BUG();
				if (*((unsigned long*)(objp + cachep->objsize
						-BYTES_PER_WORD)) != RED_MAGIC1)
					BUG();
				objp += BYTES_PER_WORD;
			}
#endif
			if (cachep->dtor)
				(cachep->dtor)(objp, cachep, 0);
#if DEBUG
			if (cachep->flags & SLAB_RED_ZONE) {
				objp -= BYTES_PER_WORD;
			}	
			if ((cachep->flags & SLAB_POISON)  &&
				kmem_check_poison_obj(cachep, objp))
				BUG();
#endif
		}
	}

	kmem_freepages(cachep, slabp->s_mem-slabp->colouroff);
	if (OFF_SLAB(cachep))
		kmem_cache_free(cachep->slabp_cache, slabp);
}

/**
 * kmem_cache_create - Create a cache.
 * @name: A string which is used in /proc/slabinfo to identify this cache.
 * @size: The size of objects to be created in this cache.
 * @offset: The offset to use within the page.
 * @flags: SLAB flags
 * @ctor: A constructor for the objects.
 * @dtor: A destructor for the objects.
 *
 * Returns a ptr to the cache on success, NULL on failure.
 * Cannot be called within a int, but can be interrupted.
 * The @ctor is run when new pages are allocated by the cache
 * and the @dtor is run before the pages are handed back.
 * The flags are
 *
 * %SLAB_POISON - Poison the slab with a known test pattern (a5a5a5a5)
 * to catch references to uninitialised memory.
 *
 * %SLAB_RED_ZONE - Insert `Red' zones around the allocated memory to check
 * for buffer overruns.
 *
 * %SLAB_NO_REAP - Don't automatically reap this cache when we're under
 * memory pressure.
 *
 * %SLAB_HWCACHE_ALIGN - Align the objects in this cache to a hardware
 * cacheline.  This can be beneficial if you're counting cycles as closely
 * as davem.
 */
/**
 * kmem_cache_create - 创建一个新的SLAB缓存（Linux2.4）
 * @name: 缓存名称字符串，用于调试查看
 * @size: 单个对象的大小
 * @offset: 对象着色偏移基准值（colour_off）
 * @flags: SLAB控制标记，如 SLAB_HWCACHE_ALIGN、SLAB_CACHE_DMA、SLAB_POISON 等
 * @ctor: 对象构造函数，对象首次分配时调用
 * @dtor: 对象析构函数，对象销毁回收时调用
 *
 * 返回值：成功返回 kmem_cache_t* 缓存描述符；失败返回 NULL
 *
 * 核心逻辑：参数合法性校验 → 从cache_cache元缓存分配kmem_cache_t描述符
 * 对齐处理、调试标记适配 → 计算gfporder、每个slab容纳对象数量、碎片评估
 * 决定on-slab / off-slab管理模式 → 初始化缓存各个字段、SMP开启cpucache
 * 加入全局cache_chain链表，同时检查重名缓存防止重复创建
 */
kmem_cache_t *
kmem_cache_create (const char *name, size_t size, size_t offset,
    unsigned long flags, void (*ctor)(void*, kmem_cache_t *, unsigned long),
    void (*dtor)(void*, kmem_cache_t *, unsigned long))
{
    const char *func_nm = KERN_ERR "kmem_create: ";
    size_t left_over, align, slab_size;
    kmem_cache_t *cachep = NULL;

    /*
     * Sanity checks... these are all serious usage bugs.
     */
    // 基础合法性校验，非法参数直接BUG
    if ((!name) ||
        ((strlen(name) >= CACHE_NAMELEN - 1)) ||
        in_interrupt() ||
        (size < BYTES_PER_WORD) ||
        (size > (1<<MAX_OBJ_ORDER)*PAGE_SIZE) ||
        (dtor && !ctor) ||
        (offset < 0 || offset > size))
            BUG();

#if DEBUG
    // 调试标记兼容处理：开启初始化检查但无构造函数，关闭该标记
    if ((flags & SLAB_DEBUG_INITIAL) && !ctor) {
        /* No constructor, but inital state check requested */
        printk("%sNo con, but init state check requested - %s\n", func_nm, name);
        flags &= ~SLAB_DEBUG_INITIAL;
    }

    // 开启毒化标记但提供构造函数，二者冲突，关闭毒化
    if ((flags & SLAB_POISON) && ctor) {
        /* request for poisoning, but we can't do that with a constructor */
        printk("%sPoisoning requested, but con given - %s\n", func_nm, name);
        flags &= ~SLAB_POISON;
    }
#if FORCED_DEBUG
    // 强制调试：小对象开启红区检测；无构造函数自动开启毒化
    if (size < (PAGE_SIZE>>3))
        /*
         * do not red zone large object, causes severe
         * fragmentation.
         */
        flags |= SLAB_RED_ZONE;
    if (!ctor)
        flags |= SLAB_POISON;
#endif
#endif

    /*
     * Always checks flags, a caller might be expecting debug
     * support which isn't available.
     */
    // 过滤不支持的flag，非法标记直接BUG
    if (flags & ~CREATE_MASK)
        BUG();

    // 从元缓存cache_cache分配kmem_cache_t自身描述符（SLAB自举）
    /* Get cache's description obj. */
    cachep = (kmem_cache_t *) kmem_cache_alloc(&cache_cache, SLAB_KERNEL);
    if (!cachep)
        goto opps;
    memset(cachep, 0, sizeof(kmem_cache_t));

    /* Check that size is in terms of words.  This is needed to avoid
     * unaligned accesses for some archs when redzoning is used, and makes
     * sure any on-slab bufctl's are also correctly aligned.
     */
    // 强制对象大小按字对齐，避免非对齐访问、保证bufctl对齐
    if (size & (BYTES_PER_WORD-1)) {
        size += (BYTES_PER_WORD-1);
        size &= ~(BYTES_PER_WORD-1);
        printk("%sForcing size word alignment - %s\n", func_nm, name);
    }
    
#if DEBUG
    // 红区检测：关闭硬件缓存对齐，对象首尾增加2个字的红区
    if (flags & SLAB_RED_ZONE) {
        /*
         * There is no point trying to honour cache alignment
         * when redzoning.
         */
        flags &= ~SLAB_HWCACHE_ALIGN;
        size += 2*BYTES_PER_WORD;   /* words for redzone */
    }
#endif
    // 对齐基准：默认字对齐；开启硬件缓存对齐则使用L1缓存行大小
    align = BYTES_PER_WORD;
    if (flags & SLAB_HWCACHE_ALIGN)
        align = L1_CACHE_BYTES;

    /* Determine if the slab management is 'on' or 'off' slab. */
    // 大对象默认off-slab：slab_t、bufctl管理结构放在slab页外面
    if (size >= (PAGE_SIZE>>3))
        /*
         * Size is large, assume best to place the slab management obj
         * off-slab (should allow better packing of objs).
         */
        flags |= CFLGS_OFF_SLAB;

    if (flags & SLAB_HWCACHE_ALIGN) {
        /* Need to adjust size so that objs are cache aligned. */
        /* Small obj size, can get at least two per cache line. */
        /* FIXME: only power of 2 supported, was better */
        // 硬件缓存对齐：缩小对齐粒度，保证缓存行内可以放下多个对象
        while (size < align/2)
            align /= 2;
        size = (size+align-1)&(~(align-1));
    }

    /* Cal size (in pages) of slabs, and the num of objs per slab.
     * This could be made much more intelligent.  For now, try to avoid
     * using high page-orders for slabs.  When the gfp() funcs are more
     * friendly towards high-order requests, this should be changed.
     */
    // 循环计算合适的gfporder、每个slab容纳对象数量，评估内部碎片
    do {
        unsigned int break_flag = 0;
cal_wastage:
        // 估算：当前order下，一页可以放多少对象、剩余空间left_over
        kmem_cache_estimate(cachep->gfporder, size, flags,
                        &left_over, &cachep->num);
        if (break_flag)
            break;
        if (cachep->gfporder >= MAX_GFP_ORDER)
            break;
        if (!cachep->num)
            goto next;
        // off-slab模式，对象数量超过上限，降低order重新计算
        if (flags & CFLGS_OFF_SLAB && cachep->num > offslab_limit) {
            /* Oops, this num of objs will cause problems. */
            cachep->gfporder--;
            break_flag++;
            goto cal_wastage;
        }

        /*
         * Large num of objs is good, but v. large slabs are currently
         * bad for the gfp()s.
         */
        // 到达预设order上限，不再提升order
        if (cachep->gfporder >= slab_break_gfp_order)
            break;

        // 碎片比例可接受，停止提升order
        if ((left_over*8) <= (PAGE_SIZE<<cachep->gfporder))
            break;  /* Acceptable internal fragmentation. */
next:
        // 碎片太大，提升order，尝试更大连续页块
        cachep->gfporder++;
    } while (1);

    // 算不出可用对象数量，创建失败，释放cachep
    if (!cachep->num) {
        printk("kmem_cache_create: couldn't create cache %s.\n", name);
        kmem_cache_free(&cache_cache, cachep);
        cachep = NULL;
        goto opps;
    }
    // 计算slab管理结构（slab_t + bufctl数组）占用大小，按L1缓存对齐
    slab_size = L1_CACHE_ALIGN(cachep->num*sizeof(kmem_bufctl_t)+sizeof(slab_t));

    /*
     * If the slab has been placed off-slab, and we have enough space then
     * move it on-slab. This is at the expense of any extra colouring.
     */
    // 原本off-slab，但slab内剩余空间足够放下管理结构，改成on-slab节省内存
    if (flags & CFLGS_OFF_SLAB && left_over >= slab_size) {
        flags &= ~CFLGS_OFF_SLAB;
        left_over -= slab_size;
    }

    // 着色偏移向上对齐；offset为0时默认L1_CACHE_BYTES
    offset += (align-1);
    offset &= ~(align-1);
    if (!offset)
        offset = L1_CACHE_BYTES;
    cachep->colour_off = offset;
    // 着色数量：剩余空间 / 着色步长，用来生成不同colouroff，减少伪共享
    cachep->colour = left_over/offset;

    /* init remaining fields */
    // 单页 + on-slab 开启优化标记
    if (!cachep->gfporder && !(flags & CFLGS_OFF_SLAB))
        flags |= CFLGS_OPTIMIZE;

    cachep->flags = flags;
    cachep->gfpflags = 0;
    // DMA标记：分配底层页时使用GFP_DMA
    if (flags & SLAB_CACHE_DMA)
        cachep->gfpflags |= GFP_DMA;
    spin_lock_init(&cachep->spinlock);
    cachep->objsize = size;
    // 初始化slab链表头
    INIT_LIST_HEAD(&cachep->slabs);
    cachep->firstnotfull = &cachep->slabs;

    // off-slab模式：管理结构从通用kmalloc缓存分配
    if (flags & CFLGS_OFF_SLAB)
        cachep->slabp_cache = kmem_find_general_cachep(slab_size,0);
    cachep->ctor = ctor;
    cachep->dtor = dtor;
    // 拷贝缓存名字，防止模块卸载后名字指针失效
    strcpy(cachep->name, name);

#ifdef CONFIG_SMP
    // SMP架构：开启per-cpu缓存cpucache，批量预取对象减少锁竞争
    if (g_cpucache_up)
        enable_cpucache(cachep);
#endif
    // 加全局信号量，遍历全局缓存链表，检查重名缓存
    down(&cache_chain_sem);
    {
        struct list_head *p;

        list_for_each(p, &cache_chain) {
            kmem_cache_t *pc = list_entry(p, kmem_cache_t, next);

            /* The name field is constant - no lock needed. */
            if (!strcmp(pc->name, name))
                BUG();
        }
    }

    /* There is no reason to lock our new cache before we
     * link it in - no one knows about it yet...
     */
    // 把新缓存挂入全局cache_chain链表
    list_add(&cachep->next, &cache_chain);
    up(&cache_chain_sem);
opps:
    return cachep;
}

/*
 * This check if the kmem_cache_t pointer is chained in the cache_cache
 * list. -arca
 */
static int is_chained_kmem_cache(kmem_cache_t * cachep)
{
	struct list_head *p;
	int ret = 0;

	/* Find the cache in the chain of caches. */
	down(&cache_chain_sem);
	list_for_each(p, &cache_chain) {
		if (p == &cachep->next) {
			ret = 1;
			break;
		}
	}
	up(&cache_chain_sem);

	return ret;
}

#ifdef CONFIG_SMP
/*
 * Waits for all CPUs to execute func().
 */
static void smp_call_function_all_cpus(void (*func) (void *arg), void *arg)
{
	local_irq_disable();
	func(arg);
	local_irq_enable();

	if (smp_call_function(func, arg, 1, 1))
		BUG();
}
typedef struct ccupdate_struct_s
{
	kmem_cache_t *cachep;
	cpucache_t *new[NR_CPUS];
} ccupdate_struct_t;

static void do_ccupdate_local(void *info)
{
	ccupdate_struct_t *new = (ccupdate_struct_t *)info;
	cpucache_t *old = cc_data(new->cachep);
	
	cc_data(new->cachep) = new->new[smp_processor_id()];
	new->new[smp_processor_id()] = old;
}

static void free_block (kmem_cache_t* cachep, void** objpp, int len);

static void drain_cpu_caches(kmem_cache_t *cachep)
{
	ccupdate_struct_t new;
	int i;

	memset(&new.new,0,sizeof(new.new));

	new.cachep = cachep;

	down(&cache_chain_sem);
	smp_call_function_all_cpus(do_ccupdate_local, (void *)&new);

	for (i = 0; i < smp_num_cpus; i++) {
		cpucache_t* ccold = new.new[cpu_logical_map(i)];
		if (!ccold || (ccold->avail == 0))
			continue;
		local_irq_disable();
		free_block(cachep, cc_entry(ccold), ccold->avail);
		local_irq_enable();
		ccold->avail = 0;
	}
	smp_call_function_all_cpus(do_ccupdate_local, (void *)&new);
	up(&cache_chain_sem);
}

#else
#define drain_cpu_caches(cachep)	do { } while (0)
#endif

static int __kmem_cache_shrink(kmem_cache_t *cachep)
{
	slab_t *slabp;
	int ret;

	drain_cpu_caches(cachep);

	spin_lock_irq(&cachep->spinlock);

	/* If the cache is growing, stop shrinking. */
	while (!cachep->growing) {
		struct list_head *p;

		p = cachep->slabs.prev;
		if (p == &cachep->slabs)
			break;

		slabp = list_entry(cachep->slabs.prev, slab_t, list);
		if (slabp->inuse)
			break;

		list_del(&slabp->list);
		if (cachep->firstnotfull == &slabp->list)
			cachep->firstnotfull = &cachep->slabs;

		spin_unlock_irq(&cachep->spinlock);
		kmem_slab_destroy(cachep, slabp);
		spin_lock_irq(&cachep->spinlock);
	}
	ret = !list_empty(&cachep->slabs);
	spin_unlock_irq(&cachep->spinlock);
	return ret;
}

/**
 * kmem_cache_shrink - Shrink a cache.
 * @cachep: The cache to shrink.
 *
 * Releases as many slabs as possible for a cache.
 * To help debugging, a zero exit status indicates all slabs were released.
 */
int kmem_cache_shrink(kmem_cache_t *cachep)
{
	if (!cachep || in_interrupt() || !is_chained_kmem_cache(cachep))
		BUG();

	return __kmem_cache_shrink(cachep);
}

/**
 * kmem_cache_destroy - delete a cache
 * @cachep: the cache to destroy
 *
 * Remove a kmem_cache_t object from the slab cache.
 * Returns 0 on success.
 *
 * It is expected this function will be called by a module when it is
 * unloaded.  This will remove the cache completely, and avoid a duplicate
 * cache being allocated each time a module is loaded and unloaded, if the
 * module doesn't have persistent in-kernel storage across loads and unloads.
 *
 * The caller must guarantee that noone will allocate memory from the cache
 * during the kmem_cache_destroy().
 */
int kmem_cache_destroy (kmem_cache_t * cachep)
{
	if (!cachep || in_interrupt() || cachep->growing)
		BUG();

	/* Find the cache in the chain of caches. */
	down(&cache_chain_sem);
	/* the chain is never empty, cache_cache is never destroyed */
	if (clock_searchp == cachep)
		clock_searchp = list_entry(cachep->next.next,
						kmem_cache_t, next);
	list_del(&cachep->next);
	up(&cache_chain_sem);

	if (__kmem_cache_shrink(cachep)) {
		printk(KERN_ERR "kmem_cache_destroy: Can't free all objects %p\n",
		       cachep);
		down(&cache_chain_sem);
		list_add(&cachep->next,&cache_chain);
		up(&cache_chain_sem);
		return 1;
	}
#ifdef CONFIG_SMP
	{
		int i;
		for (i = 0; i < NR_CPUS; i++)
			kfree(cachep->cpudata[i]);
	}
#endif
	kmem_cache_free(&cache_cache, cachep);

	return 0;
}

/* Get the memory for a slab management obj. */
static inline slab_t * kmem_cache_slabmgmt (kmem_cache_t *cachep,
			void *objp, int colour_off, int local_flags)
{
	slab_t *slabp;
	
	if (OFF_SLAB(cachep)) {
		/* Slab management obj is off-slab. */
		slabp = kmem_cache_alloc(cachep->slabp_cache, local_flags);
		if (!slabp)
			return NULL;
	} else {
		/* FIXME: change to
			slabp = objp
		 * if you enable OPTIMIZE
		 */
		slabp = objp+colour_off;
		colour_off += L1_CACHE_ALIGN(cachep->num *
				sizeof(kmem_bufctl_t) + sizeof(slab_t));
	}
	slabp->inuse = 0;
	slabp->colouroff = colour_off;
	slabp->s_mem = objp+colour_off;

	return slabp;
}

static inline void kmem_cache_init_objs (kmem_cache_t * cachep,
			slab_t * slabp, unsigned long ctor_flags)
{
	int i;

	for (i = 0; i < cachep->num; i++) {
		void* objp = slabp->s_mem+cachep->objsize*i;
#if DEBUG
		if (cachep->flags & SLAB_RED_ZONE) {
			*((unsigned long*)(objp)) = RED_MAGIC1;
			*((unsigned long*)(objp + cachep->objsize -
					BYTES_PER_WORD)) = RED_MAGIC1;
			objp += BYTES_PER_WORD;
		}
#endif

		/*
		 * Constructors are not allowed to allocate memory from
		 * the same cache which they are a constructor for.
		 * Otherwise, deadlock. They must also be threaded.
		 */
		if (cachep->ctor)
			cachep->ctor(objp, cachep, ctor_flags);
#if DEBUG
		if (cachep->flags & SLAB_RED_ZONE)
			objp -= BYTES_PER_WORD;
		if (cachep->flags & SLAB_POISON)
			/* need to poison the objs */
			kmem_poison_obj(cachep, objp);
		if (cachep->flags & SLAB_RED_ZONE) {
			if (*((unsigned long*)(objp)) != RED_MAGIC1)
				BUG();
			if (*((unsigned long*)(objp + cachep->objsize -
					BYTES_PER_WORD)) != RED_MAGIC1)
				BUG();
		}
#endif
		slab_bufctl(slabp)[i] = i+1;
	}
	slab_bufctl(slabp)[i-1] = BUFCTL_END;
	slabp->free = 0;
}

/*
 * Grow (by 1) the number of slabs within a cache.  This is called by
 * kmem_cache_alloc() when there are no active objs left in a cache.
 */
static int kmem_cache_grow (kmem_cache_t * cachep, int flags)
{
	slab_t	*slabp;
	struct page	*page;
	void		*objp;
	size_t		 offset;
	unsigned int	 i, local_flags;
	unsigned long	 ctor_flags;
	unsigned long	 save_flags;

	/* Be lazy and only check for valid flags here,
 	 * keeping it out of the critical path in kmem_cache_alloc().
	 */
	if (flags & ~(SLAB_DMA|SLAB_LEVEL_MASK|SLAB_NO_GROW))
		BUG();
	if (flags & SLAB_NO_GROW)
		return 0;

	/*
	 * The test for missing atomic flag is performed here, rather than
	 * the more obvious place, simply to reduce the critical path length
	 * in kmem_cache_alloc(). If a caller is seriously mis-behaving they
	 * will eventually be caught here (where it matters).
	 */
	if (in_interrupt() && (flags & SLAB_LEVEL_MASK) != SLAB_ATOMIC)
		BUG();

	ctor_flags = SLAB_CTOR_CONSTRUCTOR;
	local_flags = (flags & SLAB_LEVEL_MASK);
	if (local_flags == SLAB_ATOMIC)
		/*
		 * Not allowed to sleep.  Need to tell a constructor about
		 * this - it might need to know...
		 */
		ctor_flags |= SLAB_CTOR_ATOMIC;

	/* About to mess with non-constant members - lock. */
	spin_lock_irqsave(&cachep->spinlock, save_flags);

	/* Get colour for the slab, and cal the next value. */
	offset = cachep->colour_next;
	cachep->colour_next++;
	if (cachep->colour_next >= cachep->colour)
		cachep->colour_next = 0;
	offset *= cachep->colour_off;
	cachep->dflags |= DFLGS_GROWN;

	cachep->growing++;
	spin_unlock_irqrestore(&cachep->spinlock, save_flags);

	/* A series of memory allocations for a new slab.
	 * Neither the cache-chain semaphore, or cache-lock, are
	 * held, but the incrementing c_growing prevents this
	 * cache from being reaped or shrunk.
	 * Note: The cache could be selected in for reaping in
	 * kmem_cache_reap(), but when the final test is made the
	 * growing value will be seen.
	 */

	/* Get mem for the objs. */
	if (!(objp = kmem_getpages(cachep, flags)))
		goto failed;

	/* Get slab management. */
	if (!(slabp = kmem_cache_slabmgmt(cachep, objp, offset, local_flags)))
		goto opps1;

	/* Nasty!!!!!! I hope this is OK. */
	i = 1 << cachep->gfporder;
	page = virt_to_page(objp);
	do {
		SET_PAGE_CACHE(page, cachep);
		SET_PAGE_SLAB(page, slabp);
		PageSetSlab(page);
		page++;
	} while (--i);

	kmem_cache_init_objs(cachep, slabp, ctor_flags);

	spin_lock_irqsave(&cachep->spinlock, save_flags);
	cachep->growing--;

	/* Make slab active. */
	list_add_tail(&slabp->list,&cachep->slabs);
	if (cachep->firstnotfull == &cachep->slabs)
		cachep->firstnotfull = &slabp->list;
	STATS_INC_GROWN(cachep);
	cachep->failures = 0;

	spin_unlock_irqrestore(&cachep->spinlock, save_flags);
	return 1;
opps1:
	kmem_freepages(cachep, objp);
failed:
	spin_lock_irqsave(&cachep->spinlock, save_flags);
	cachep->growing--;
	spin_unlock_irqrestore(&cachep->spinlock, save_flags);
	return 0;
}

/*
 * Perform extra freeing checks:
 * - detect double free
 * - detect bad pointers.
 * Called with the cache-lock held.
 */

#if DEBUG
static int kmem_extra_free_checks (kmem_cache_t * cachep,
			slab_t *slabp, void * objp)
{
	int i;
	unsigned int objnr = (objp-slabp->s_mem)/cachep->objsize;

	if (objnr >= cachep->num)
		BUG();
	if (objp != slabp->s_mem + objnr*cachep->objsize)
		BUG();

	/* Check slab's freelist to see if this obj is there. */
	for (i = slabp->free; i != BUFCTL_END; i = slab_bufctl(slabp)[i]) {
		if (i == objnr)
			BUG();
	}
	return 0;
}
#endif

static inline void kmem_cache_alloc_head(kmem_cache_t *cachep, int flags)
{
#if DEBUG
	if (flags & SLAB_DMA) {
		if (!(cachep->gfpflags & GFP_DMA))
			BUG();
	} else {
		if (cachep->gfpflags & GFP_DMA)
			BUG();
	}
#endif
}

static inline void * kmem_cache_alloc_one_tail (kmem_cache_t *cachep,
							 slab_t *slabp)
{
	void *objp;

	STATS_INC_ALLOCED(cachep);
	STATS_INC_ACTIVE(cachep);
	STATS_SET_HIGH(cachep);

	/* get obj pointer */
	slabp->inuse++;
	objp = slabp->s_mem + slabp->free*cachep->objsize;
	slabp->free=slab_bufctl(slabp)[slabp->free];

	if (slabp->free == BUFCTL_END)
		/* slab now full: move to next slab for next alloc */
		cachep->firstnotfull = slabp->list.next;
#if DEBUG
	if (cachep->flags & SLAB_POISON)
		if (kmem_check_poison_obj(cachep, objp))
			BUG();
	if (cachep->flags & SLAB_RED_ZONE) {
		/* Set alloc red-zone, and check old one. */
		if (xchg((unsigned long *)objp, RED_MAGIC2) !=
							 RED_MAGIC1)
			BUG();
		if (xchg((unsigned long *)(objp+cachep->objsize -
			  BYTES_PER_WORD), RED_MAGIC2) != RED_MAGIC1)
			BUG();
		objp += BYTES_PER_WORD;
	}
#endif
	return objp;
}

/*
 * Returns a ptr to an obj in the given cache.
 * caller must guarantee synchronization
 * #define for the goto optimization 8-)
 */
#define kmem_cache_alloc_one(cachep)				\
({								\
	slab_t	*slabp;					\
								\
	/* Get slab alloc is to come from. */			\
	{							\
		struct list_head* p = cachep->firstnotfull;	\
		if (p == &cachep->slabs)			\
			goto alloc_new_slab;			\
		slabp = list_entry(p,slab_t, list);	\
	}							\
	kmem_cache_alloc_one_tail(cachep, slabp);		\
})

#ifdef CONFIG_SMP
/**
 * kmem_cache_alloc_batch - 批量从slab缓存拿对象，填充当前CPU本地cpucache缓存
 * @cachep: slab缓存描述符
 * @flags: GFP分配标志
 *
 * 返回值：成功返回一个对象指针；cpucache填充后无可用对象返回NULL
 * 说明：SMP架构优化，一次性批量取一批对象放到per-cpu缓存，减少频繁争抢全局cache自旋锁
 */
void* kmem_cache_alloc_batch(kmem_cache_t* cachep, int flags)
{
    // 单次批量搬运数量，取自缓存的batchcount
    int batchcount = cachep->batchcount;
    // 获取当前CPU的私有cpucache
    cpucache_t* cc = cc_data(cachep);

    // 加全局slab缓存自旋锁，保护slab链表
    spin_lock(&cachep->spinlock);
    while (batchcount--) {
        /* Get slab alloc is to come from. */
        // 取第一个还有空闲对象的slab
        struct list_head *p = cachep->firstnotfull;
        slab_t *slabp;

        // 没有半满slab，直接退出批量循环
        if (p == &cachep->slabs)
            break;
        slabp = list_entry(p,slab_t, list);
        // 从这个slab取出一个对象，存入cpu本地缓存数组，avail计数+1
        cc_entry(cc)[cc->avail++] =
                kmem_cache_alloc_one_tail(cachep, slabp);
    }
    // 释放全局自旋锁
    spin_unlock(&cachep->spinlock);

    // 如果本地缓存已经拿到对象，返回其中一个
    if (cc->avail)
        return cc_entry(cc)[--cc->avail];
    return NULL;
}
#endif

/**
 * __kmem_cache_alloc
 * @cachep: 目标slab缓存描述符 kmem_cache_t *
 * @flags: GFP分配标志（GFP_KERNEL / GFP_ATOMIC等）
 * static inline，slab分配器核心分配入口
 * 返回值：成功返回空闲对象地址；失败返回NULL
 *
 * 分配优先路径（SMP）：Per-CPU本地缓存 → 批量填充本地缓存 → 全局slab链表取对象 → 新建slab
 */
static inline void * __kmem_cache_alloc (kmem_cache_t *cachep, int flags)
{
    unsigned long save_flags;
    void* objp;

    // 预检查：必要时触发slab的预分配/收缩、内存压力判断
    kmem_cache_alloc_head(cachep, flags);
try_again:
    // 关本地中断，防止中断里并发修改per-cpu缓存
    local_irq_save(save_flags);
#ifdef CONFIG_SMP
    {
        // 获取当前CPU对应的per-cpu缓存 cpucache_t
        cpucache_t *cc = cc_data(cachep);

        if (cc) {
            // per-cpu缓存存在
            if (cc->avail) {
                // ✅ 命中：本地缓存还有空闲对象，直接拿
                STATS_INC_ALLOCHIT(cachep);
                // avail 是当前缓存里空闲对象计数，先减计数再取对象
                objp = cc_entry(cc)[--cc->avail];
            } else {
                // ❌ 本地缓存空，未命中
                STATS_INC_ALLOCMISS(cachep);
                // 从全局slab链表批量拿一批对象填充到当前CPU的cpucache
                objp = kmem_cache_alloc_batch(cachep,flags);
                if (!objp)
                    // 批量拿不到对象，需要新建slab，跳转到新建分支
                    goto alloc_new_slab_nolock;
            }
        } else {
            // 当前CPU没有分配per-cpu缓存，直接加全局锁，从slab链表取单个对象
            spin_lock(&cachep->spinlock);
            objp = kmem_cache_alloc_one(cachep);
            spin_unlock(&cachep->spinlock);
        }
    }
#else
    // UP单处理器：没有per-cpu缓存，直接从全局slab链表分配
    objp = kmem_cache_alloc_one(cachep);
#endif
    // 恢复本地中断
    local_irq_restore(save_flags);
    return objp;

alloc_new_slab:
#ifdef CONFIG_SMP
    spin_unlock(&cachep->spinlock);
alloc_new_slab_nolock:
#endif
    // 恢复中断，再去新建slab（新建slab会调用伙伴系统alloc_pages，不能在关中断、持锁上下文）
    local_irq_restore(save_flags);
    // kmem_cache_grow：向伙伴系统申请物理页，构建一块新的slab
    if (kmem_cache_grow(cachep, flags))
        /*
         * 注释含义：grow成功，新slab已经加入cache的slabs链表；
         * 有可能别的CPU抢先把新slab里对象拿走了，没关系，循环重试一次分配
         */
        goto try_again;
    // grow失败，内存不足，返回NULL
    return NULL;
}

/*
 * Release an obj back to its cache. If the obj has a constructed
 * state, it should be in this state _before_ it is released.
 * - caller is responsible for the synchronization
 */

#if DEBUG
# define CHECK_NR(pg)						\
	do {							\
		if (!VALID_PAGE(pg)) {				\
			printk(KERN_ERR "kfree: out of range ptr %lxh.\n", \
				(unsigned long)objp);		\
			BUG();					\
		} \
	} while (0)
# define CHECK_PAGE(page)					\
	do {							\
		CHECK_NR(page);					\
		if (!PageSlab(page)) {				\
			printk(KERN_ERR "kfree: bad ptr %lxh.\n", \
				(unsigned long)objp);		\
			BUG();					\
		}						\
	} while (0)

#else
# define CHECK_PAGE(pg)	do { } while (0)
#endif

static inline void kmem_cache_free_one(kmem_cache_t *cachep, void *objp)
{
	slab_t* slabp;

	CHECK_PAGE(virt_to_page(objp));
	/* reduces memory footprint
	 *
	if (OPTIMIZE(cachep))
		slabp = (void*)((unsigned long)objp&(~(PAGE_SIZE-1)));
	 else
	 */
	slabp = GET_PAGE_SLAB(virt_to_page(objp));

#if DEBUG
	if (cachep->flags & SLAB_DEBUG_INITIAL)
		/* Need to call the slab's constructor so the
		 * caller can perform a verify of its state (debugging).
		 * Called without the cache-lock held.
		 */
		cachep->ctor(objp, cachep, SLAB_CTOR_CONSTRUCTOR|SLAB_CTOR_VERIFY);

	if (cachep->flags & SLAB_RED_ZONE) {
		objp -= BYTES_PER_WORD;
		if (xchg((unsigned long *)objp, RED_MAGIC1) != RED_MAGIC2)
			/* Either write before start, or a double free. */
			BUG();
		if (xchg((unsigned long *)(objp+cachep->objsize -
				BYTES_PER_WORD), RED_MAGIC1) != RED_MAGIC2)
			/* Either write past end, or a double free. */
			BUG();
	}
	if (cachep->flags & SLAB_POISON)
		kmem_poison_obj(cachep, objp);
	if (kmem_extra_free_checks(cachep, slabp, objp))
		return;
#endif
	{
		unsigned int objnr = (objp-slabp->s_mem)/cachep->objsize;

		slab_bufctl(slabp)[objnr] = slabp->free;
		slabp->free = objnr;
	}
	STATS_DEC_ACTIVE(cachep);
	
	/* fixup slab chain */
	if (slabp->inuse-- == cachep->num)
		goto moveslab_partial;
	if (!slabp->inuse)
		goto moveslab_free;
	return;

moveslab_partial:
    	/* was full.
	 * Even if the page is now empty, we can set c_firstnotfull to
	 * slabp: there are no partial slabs in this case
	 */
	{
		struct list_head *t = cachep->firstnotfull;

		cachep->firstnotfull = &slabp->list;
		if (slabp->list.next == t)
			return;
		list_del(&slabp->list);
		list_add_tail(&slabp->list, t);
		return;
	}
moveslab_free:
	/*
	 * was partial, now empty.
	 * c_firstnotfull might point to slabp
	 * FIXME: optimize
	 */
	{
		struct list_head *t = cachep->firstnotfull->prev;

		list_del(&slabp->list);
		list_add_tail(&slabp->list, &cachep->slabs);
		if (cachep->firstnotfull == &slabp->list)
			cachep->firstnotfull = t->next;
		return;
	}
}

#ifdef CONFIG_SMP
static inline void __free_block (kmem_cache_t* cachep,
							void** objpp, int len)
{
	for ( ; len > 0; len--, objpp++)
		kmem_cache_free_one(cachep, *objpp);
}

static void free_block (kmem_cache_t* cachep, void** objpp, int len)
{
	spin_lock(&cachep->spinlock);
	__free_block(cachep, objpp, len);
	spin_unlock(&cachep->spinlock);
}
#endif

/*
 * __kmem_cache_free
 * called with disabled ints
 */
static inline void __kmem_cache_free (kmem_cache_t *cachep, void* objp)
{
#ifdef CONFIG_SMP
	cpucache_t *cc = cc_data(cachep);

	CHECK_PAGE(virt_to_page(objp));
	if (cc) {
		int batchcount;
		if (cc->avail < cc->limit) {
			STATS_INC_FREEHIT(cachep);
			cc_entry(cc)[cc->avail++] = objp;
			return;
		}
		STATS_INC_FREEMISS(cachep);
		batchcount = cachep->batchcount;
		cc->avail -= batchcount;
		free_block(cachep,
					&cc_entry(cc)[cc->avail],batchcount);
		cc_entry(cc)[cc->avail++] = objp;
		return;
	} else {
		free_block(cachep, &objp, 1);
	}
#else
	kmem_cache_free_one(cachep, objp);
#endif
}

/**
 * kmem_cache_alloc - Allocate an object
 * @cachep: The cache to allocate from.
 * @flags: See kmalloc().
 *
 * Allocate an object from this cache.  The flags are only relevant
 * if the cache has no available objects.
 */
void * kmem_cache_alloc (kmem_cache_t *cachep, int flags)
{
	return __kmem_cache_alloc(cachep, flags);
}

/**
 * kmalloc - allocate memory
 * @size: how many bytes of memory are required.
 * @flags: the type of memory to allocate.
 *
 * kmalloc is the normal method of allocating memory
 * in the kernel.  The @flags argument may be one of:
 *
 * %GFP_BUFFER - XXX
 *
 * %GFP_ATOMIC - allocation will not sleep.  Use inside interrupt handlers.
 *
 * %GFP_USER - allocate memory on behalf of user.  May sleep.
 *
 * %GFP_KERNEL - allocate normal kernel ram.  May sleep.
 *
 * %GFP_NFS - has a slightly lower probability of sleeping than %GFP_KERNEL.
 * Don't use unless you're in the NFS code.
 *
 * %GFP_KSWAPD - Don't use unless you're modifying kswapd.
 */
void * kmalloc (size_t size, int flags)
{
	cache_sizes_t *csizep = cache_sizes;

	for (; csizep->cs_size; csizep++) {
		if (size > csizep->cs_size)
			continue;
		return __kmem_cache_alloc(flags & GFP_DMA ?
			 csizep->cs_dmacachep : csizep->cs_cachep, flags);
	}
	BUG(); // too big size
	return NULL;
}

/**
 * kmem_cache_free - Deallocate an object
 * @cachep: The cache the allocation was from.
 * @objp: The previously allocated object.
 *
 * Free an object which was previously allocated from this
 * cache.
 */
void kmem_cache_free (kmem_cache_t *cachep, void *objp)
{
	unsigned long flags;
#if DEBUG
	CHECK_PAGE(virt_to_page(objp));
	if (cachep != GET_PAGE_CACHE(virt_to_page(objp)))
		BUG();
#endif

	local_irq_save(flags);
	__kmem_cache_free(cachep, objp);
	local_irq_restore(flags);
}

/**
 * kfree - free previously allocated memory
 * @objp: pointer returned by kmalloc.
 *
 * Don't free memory not originally allocated by kmalloc()
 * or you will run into trouble.
 */
void kfree (const void *objp)
{
	kmem_cache_t *c;
	unsigned long flags;

	if (!objp)
		return;
	local_irq_save(flags);
	CHECK_PAGE(virt_to_page(objp));
	c = GET_PAGE_CACHE(virt_to_page(objp));
	__kmem_cache_free(c, (void*)objp);
	local_irq_restore(flags);
}

kmem_cache_t * kmem_find_general_cachep (size_t size, int gfpflags)
{
	cache_sizes_t *csizep = cache_sizes;

	/* This function could be moved to the header file, and
	 * made inline so consumers can quickly determine what
	 * cache pointer they require.
	 */
	for ( ; csizep->cs_size; csizep++) {
		if (size > csizep->cs_size)
			continue;
		break;
	}
	return (gfpflags & GFP_DMA) ? csizep->cs_dmacachep : csizep->cs_cachep;
}

#ifdef CONFIG_SMP

/* called with cache_chain_sem acquired.  */
static int kmem_tune_cpucache (kmem_cache_t* cachep, int limit, int batchcount)
{
	ccupdate_struct_t new;
	int i;

	/*
	 * These are admin-provided, so we are more graceful.
	 */
	if (limit < 0)
		return -EINVAL;
	if (batchcount < 0)
		return -EINVAL;
	if (batchcount > limit)
		return -EINVAL;
	if (limit != 0 && !batchcount)
		return -EINVAL;

	memset(&new.new,0,sizeof(new.new));
	if (limit) {
		for (i = 0; i< smp_num_cpus; i++) {
			cpucache_t* ccnew;

			ccnew = kmalloc(sizeof(void*)*limit+
					sizeof(cpucache_t), GFP_KERNEL);
			if (!ccnew)
				goto oom;
			ccnew->limit = limit;
			ccnew->avail = 0;
			new.new[cpu_logical_map(i)] = ccnew;
		}
	}
	new.cachep = cachep;
	spin_lock_irq(&cachep->spinlock);
	cachep->batchcount = batchcount;
	spin_unlock_irq(&cachep->spinlock);

	smp_call_function_all_cpus(do_ccupdate_local, (void *)&new);

	for (i = 0; i < smp_num_cpus; i++) {
		cpucache_t* ccold = new.new[cpu_logical_map(i)];
		if (!ccold)
			continue;
		local_irq_disable();
		free_block(cachep, cc_entry(ccold), ccold->avail);
		local_irq_enable();
		kfree(ccold);
	}
	return 0;
oom:
	for (i--; i >= 0; i--)
		kfree(new.new[cpu_logical_map(i)]);
	return -ENOMEM;
}

static void enable_cpucache (kmem_cache_t *cachep)
{
	int err;
	int limit;

	/* FIXME: optimize */
	if (cachep->objsize > PAGE_SIZE)
		return;
	if (cachep->objsize > 1024)
		limit = 60;
	else if (cachep->objsize > 256)
		limit = 124;
	else
		limit = 252;

	err = kmem_tune_cpucache(cachep, limit, limit/2);
	if (err)
		printk(KERN_ERR "enable_cpucache failed for %s, error %d.\n",
					cachep->name, -err);
}

static void enable_all_cpucaches (void)
{
	struct list_head* p;

	down(&cache_chain_sem);

	p = &cache_cache.next;
	do {
		kmem_cache_t* cachep = list_entry(p, kmem_cache_t, next);

		enable_cpucache(cachep);
		p = cachep->next.next;
	} while (p != &cache_cache.next);

	up(&cache_chain_sem);
}
#endif

/**
 * kmem_cache_reap - Reclaim memory from caches.
 * @gfp_mask: the type of memory required.
 *
 * Called from try_to_free_page().
 */
void kmem_cache_reap (int gfp_mask)
{
	slab_t *slabp;
	kmem_cache_t *searchp;
	kmem_cache_t *best_cachep;
	unsigned int best_pages;
	unsigned int best_len;
	unsigned int scan;

	if (gfp_mask & __GFP_WAIT)
		down(&cache_chain_sem);
	else
		if (down_trylock(&cache_chain_sem))
			return;

	scan = REAP_SCANLEN;
	best_len = 0;
	best_pages = 0;
	best_cachep = NULL;
	searchp = clock_searchp;
	do {
		unsigned int pages;
		struct list_head* p;
		unsigned int full_free;

		/* It's safe to test this without holding the cache-lock. */
		if (searchp->flags & SLAB_NO_REAP)
			goto next;
		spin_lock_irq(&searchp->spinlock);
		if (searchp->growing)
			goto next_unlock;
		if (searchp->dflags & DFLGS_GROWN) {
			searchp->dflags &= ~DFLGS_GROWN;
			goto next_unlock;
		}
#ifdef CONFIG_SMP
		{
			cpucache_t *cc = cc_data(searchp);
			if (cc && cc->avail) {
				__free_block(searchp, cc_entry(cc), cc->avail);
				cc->avail = 0;
			}
		}
#endif

		full_free = 0;
		p = searchp->slabs.prev;
		while (p != &searchp->slabs) {
			slabp = list_entry(p, slab_t, list);
			if (slabp->inuse)
				break;
			full_free++;
			p = p->prev;
		}

		/*
		 * Try to avoid slabs with constructors and/or
		 * more than one page per slab (as it can be difficult
		 * to get high orders from gfp()).
		 */
		pages = full_free * (1<<searchp->gfporder);
		if (searchp->ctor)
			pages = (pages*4+1)/5;
		if (searchp->gfporder)
			pages = (pages*4+1)/5;
		if (pages > best_pages) {
			best_cachep = searchp;
			best_len = full_free;
			best_pages = pages;
			if (full_free >= REAP_PERFECT) {
				clock_searchp = list_entry(searchp->next.next,
							kmem_cache_t,next);
				goto perfect;
			}
		}
next_unlock:
		spin_unlock_irq(&searchp->spinlock);
next:
		searchp = list_entry(searchp->next.next,kmem_cache_t,next);
	} while (--scan && searchp != clock_searchp);

	clock_searchp = searchp;

	if (!best_cachep)
		/* couldn't find anything to reap */
		goto out;

	spin_lock_irq(&best_cachep->spinlock);
perfect:
	/* free only 80% of the free slabs */
	best_len = (best_len*4 + 1)/5;
	for (scan = 0; scan < best_len; scan++) {
		struct list_head *p;

		if (best_cachep->growing)
			break;
		p = best_cachep->slabs.prev;
		if (p == &best_cachep->slabs)
			break;
		slabp = list_entry(p,slab_t,list);
		if (slabp->inuse)
			break;
		list_del(&slabp->list);
		if (best_cachep->firstnotfull == &slabp->list)
			best_cachep->firstnotfull = &best_cachep->slabs;
		STATS_INC_REAPED(best_cachep);

		/* Safe to drop the lock. The slab is no longer linked to the
		 * cache.
		 */
		spin_unlock_irq(&best_cachep->spinlock);
		kmem_slab_destroy(best_cachep, slabp);
		spin_lock_irq(&best_cachep->spinlock);
	}
	spin_unlock_irq(&best_cachep->spinlock);
out:
	up(&cache_chain_sem);
	return;
}

#ifdef CONFIG_PROC_FS
/* /proc/slabinfo
 *	cache-name num-active-objs total-objs
 *	obj-size num-active-slabs total-slabs
 *	num-pages-per-slab
 */
#define FIXUP(t)				\
	do {					\
		if (len <= off) {		\
			off -= len;		\
			len = 0;		\
		} else {			\
			if (len-off > count)	\
				goto t;		\
		}				\
	} while (0)

static int proc_getdata (char*page, char**start, off_t off, int count)
{
	struct list_head *p;
	int len = 0;

	/* Output format version, so at least we can change it without _too_
	 * many complaints.
	 */
	len += sprintf(page+len, "slabinfo - version: 1.1"
#if STATS
				" (statistics)"
#endif
#ifdef CONFIG_SMP
				" (SMP)"
#endif
				"\n");
	FIXUP(got_data);

	down(&cache_chain_sem);
	p = &cache_cache.next;
	do {
		kmem_cache_t	*cachep;
		struct list_head *q;
		slab_t		*slabp;
		unsigned long	active_objs;
		unsigned long	num_objs;
		unsigned long	active_slabs = 0;
		unsigned long	num_slabs;
		cachep = list_entry(p, kmem_cache_t, next);

		spin_lock_irq(&cachep->spinlock);
		active_objs = 0;
		num_slabs = 0;
		list_for_each(q,&cachep->slabs) {
			slabp = list_entry(q, slab_t, list);
			active_objs += slabp->inuse;
			num_objs += cachep->num;
			if (slabp->inuse)
				active_slabs++;
			else
				num_slabs++;
		}
		num_slabs+=active_slabs;
		num_objs = num_slabs*cachep->num;

		len += sprintf(page+len, "%-17s %6lu %6lu %6u %4lu %4lu %4u",
			cachep->name, active_objs, num_objs, cachep->objsize,
			active_slabs, num_slabs, (1<<cachep->gfporder));

#if STATS
		{
			unsigned long errors = cachep->errors;
			unsigned long high = cachep->high_mark;
			unsigned long grown = cachep->grown;
			unsigned long reaped = cachep->reaped;
			unsigned long allocs = cachep->num_allocations;

			len += sprintf(page+len, " : %6lu %7lu %5lu %4lu %4lu",
					high, allocs, grown, reaped, errors);
		}
#endif
#ifdef CONFIG_SMP
		{
			unsigned int batchcount = cachep->batchcount;
			unsigned int limit;

			if (cc_data(cachep))
				limit = cc_data(cachep)->limit;
			 else
				limit = 0;
			len += sprintf(page+len, " : %4u %4u",
					limit, batchcount);
		}
#endif
#if STATS && defined(CONFIG_SMP)
		{
			unsigned long allochit = atomic_read(&cachep->allochit);
			unsigned long allocmiss = atomic_read(&cachep->allocmiss);
			unsigned long freehit = atomic_read(&cachep->freehit);
			unsigned long freemiss = atomic_read(&cachep->freemiss);
			len += sprintf(page+len, " : %6lu %6lu %6lu %6lu",
					allochit, allocmiss, freehit, freemiss);
		}
#endif
		len += sprintf(page+len,"\n");
		spin_unlock_irq(&cachep->spinlock);
		FIXUP(got_data_up);
		p = cachep->next.next;
	} while (p != &cache_cache.next);
got_data_up:
	up(&cache_chain_sem);

got_data:
	*start = page+off;
	return len;
}

/**
 * slabinfo_read_proc - generates /proc/slabinfo
 * @page: scratch area, one page long
 * @start: pointer to the pointer to the output buffer
 * @off: offset within /proc/slabinfo the caller is interested in
 * @count: requested len in bytes
 * @eof: eof marker
 * @data: unused
 *
 * The contents of the buffer are
 * cache-name
 * num-active-objs
 * total-objs
 * object size
 * num-active-slabs
 * total-slabs
 * num-pages-per-slab
 * + further values on SMP and with statistics enabled
 */
int slabinfo_read_proc (char *page, char **start, off_t off,
				 int count, int *eof, void *data)
{
	int len = proc_getdata(page, start, off, count);
	len -= (*start-page);
	if (len <= count)
		*eof = 1;
	if (len>count) len = count;
	if (len<0) len = 0;
	return len;
}

#define MAX_SLABINFO_WRITE 128
/**
 * slabinfo_write_proc - SMP tuning for the slab allocator
 * @file: unused
 * @buffer: user buffer
 * @count: data len
 * @data: unused
 */
int slabinfo_write_proc (struct file *file, const char *buffer,
				unsigned long count, void *data)
{
#ifdef CONFIG_SMP
	char kbuf[MAX_SLABINFO_WRITE], *tmp;
	int limit, batchcount, res;
	struct list_head *p;
	
	if (count > MAX_SLABINFO_WRITE)
		return -EINVAL;
	if (copy_from_user(&kbuf, buffer, count))
		return -EFAULT;

	tmp = strchr(kbuf, ' ');
	if (!tmp)
		return -EINVAL;
	*tmp = '\0';
	tmp++;
	limit = simple_strtol(tmp, &tmp, 10);
	while (*tmp == ' ')
		tmp++;
	batchcount = simple_strtol(tmp, &tmp, 10);

	/* Find the cache in the chain of caches. */
	down(&cache_chain_sem);
	res = -EINVAL;
	list_for_each(p,&cache_chain) {
		kmem_cache_t *cachep = list_entry(p, kmem_cache_t, next);

		if (!strcmp(cachep->name, kbuf)) {
			res = kmem_tune_cpucache(cachep, limit, batchcount);
			break;
		}
	}
	up(&cache_chain_sem);
	if (res >= 0)
		res = count;
	return res;
#else
	return -EINVAL;
#endif
}
#endif
