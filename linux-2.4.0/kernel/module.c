#include <linux/config.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <asm/module.h>
#include <asm/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/smp_lock.h>
#include <asm/pgalloc.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/kmod.h>

/*
 * Originally by Anonymous (as far as I know...)
 * Linux version by Bas Laarhoven <bas@vimec.nl>
 * 0.99.14 version by Jon Tombs <jon@gtex02.us.es>,
 * Heavily modified by Bjorn Ekwall <bj0rn@blox.se> May 1994 (C)
 * Rewritten by Richard Henderson <rth@tamu.edu> Dec 1996
 * Add MOD_INITIALIZING Keith Owens <kaos@ocs.com.au> Nov 1999
 * Add kallsyms support, Keith Owens <kaos@ocs.com.au> Apr 2000
 * Add asm/module support, IA64 has special requirements.  Keith Owens <kaos@ocs.com.au> Sep 2000
 * Fix assorted bugs in module verification.  Keith Owens <kaos@ocs.com.au> Sep 2000
 * Fix sys_init_module race, Andrew Morton <andrewm@uow.edu.au> Oct 2000
 *     http://www.uwsg.iu.edu/hypermail/linux/kernel/0008.3/0379.html
 * Replace xxx_module_symbol with inter_module_xxx.  Keith Owens <kaos@ocs.com.au> Oct 2000
 *
 * This source is covered by the GNU GPL, the same as all kernel sources.
 */

#if defined(CONFIG_MODULES) || defined(CONFIG_KALLSYMS)

/*
 * __start___ksymtab / __stop___ksymtab
 * 链接脚本定义的边界符号，标记 __ksymtab section 的起止
 * 注意：这里声明写的是 struct module_symbol[]，但是内核本体实际存放的是 struct kernel_sym！
 * 这是2.4源码里一个经典“类型伪装”写法
 * 
 * __start___ksymtab = .;
*(___ksymtab)
__stop___ksymtab = .;

通过 EXPORT_SYMBOL 声明一个符号  __ksymtab_XXXX，放在__ksymtab段中

vmlinux.lds 中定义了段： __ksymtab，范围为[__start___ksymtab, __stop___ksymtab)
 */
extern struct module_symbol __start___ksymtab[];
extern struct module_symbol __stop___ksymtab[];

extern const struct exception_table_entry __start___ex_table[];
extern const struct exception_table_entry __stop___ex_table[];

/**
 * 
 * **是内核编译 Makefile 配套的脚本（genkallsyms），编译 vmlinux 后自动跑：**
时序：

1. 所有目标文件链接成初步 `vmlinux`
2. 脚本调用 `nm vmlinux`，读出**全部符号**（static、全局、导出，全部）
3. 脚本生成一个临时 `.o` 文件，这个临时目标文件里面，把所有符号整理成一张表，放到它自己的 `__kallsyms` section
4. 再把这个临时 `.o` 和 vmlinux 二次链接
5. ld 链接脚本收拢所有 `*(__kallsyms)` → 拼成整张全局 kallsyms 表
 * 
 */
extern const char __start___kallsyms[] __attribute__ ((weak));
extern const char __stop___kallsyms[] __attribute__ ((weak));

/**
 * kernel_module - 伪模块，用来代表内核本体，给sys_query_module使用
 *
 * 2.4内核里，sys_query_module传name=NULL的时候，就用这个结构体
 * 内核导出符号表、异常表都挂靠在这个“假模块”上，统一复用模块查询逻辑
 * 它不是动态加载的模块，是内核镜像本身
 */
static struct module kernel_module =
{
    // size_of_struct：当前module结构体本身大小，用于版本/兼容性校验
    size_of_struct:     sizeof(struct module),
    // 模块名字为空字符串，标识这是内核自身
    name:               "",
    // uc：usecount 引用计数，初始化为1，永不释放
    uc:                 {ATOMIC_INIT(1)},
    // flags：标记状态，MOD_RUNNING 表示正在运行
    flags:              MOD_RUNNING,
    // syms：导出符号表起始地址，__start___ksymtab 是链接脚本定义的符号
    // 也就是EXPORT_SYMBOL()导出的符号，insmod通过QM_SYMBOLS从这里读取
    syms:               __start___ksymtab,
    // ex_table_start / ex_table_end：内核异常表（fixup表）范围
    // 用于内核页异常修复，比如copy_from_user出错回溯
    ex_table_start:     __start___ex_table,
    ex_table_end:       __stop___ex_table,
    // kallsyms：全内核符号表（kallsyms，非导出也能查到，供调试）
    kallsyms_start:     __start___kallsyms,
    kallsyms_end:       __stop___kallsyms,
};


struct module *module_list = &kernel_module;

#endif	/* defined(CONFIG_MODULES) || defined(CONFIG_KALLSYMS) */

/* inter_module functions are always available, even when the kernel is
 * compiled without modules.  Consumers of inter_module_xxx routines
 * will always work, even when both are built into the kernel, this
 * approach removes lots of #ifdefs in mainline code.
 */

static struct list_head ime_list = LIST_HEAD_INIT(ime_list);
static spinlock_t ime_lock = SPIN_LOCK_UNLOCKED;
static int kmalloc_failed;

/**
 * inter_module_register - register a new set of inter module data.
 * @im_name: an arbitrary string to identify the data, must be unique
 * @owner: module that is registering the data, always use THIS_MODULE
 * @userdata: pointer to arbitrary userdata to be registered
 *
 * Description: Check that the im_name has not already been registered,
 * complain if it has.  For new data, add it to the inter_module_entry
 * list.
 */
void inter_module_register(const char *im_name, struct module *owner, const void *userdata)
{
	struct list_head *tmp;
	struct inter_module_entry *ime, *ime_new;

	if (!(ime_new = kmalloc(sizeof(*ime), GFP_KERNEL))) {
		/* Overloaded kernel, not fatal */
		printk(KERN_ERR
			"Aiee, inter_module_register: cannot kmalloc entry for '%s'\n",
			im_name);
		kmalloc_failed = 1;
		return;
	}
	memset(ime_new, 0, sizeof(*ime_new));
	ime_new->im_name = im_name;
	ime_new->owner = owner;
	ime_new->userdata = userdata;

	spin_lock(&ime_lock);
	list_for_each(tmp, &ime_list) {
		ime = list_entry(tmp, struct inter_module_entry, list);
		if (strcmp(ime->im_name, im_name) == 0) {
			spin_unlock(&ime_lock);
			kfree(ime_new);
			/* Program logic error, fatal */
			printk(KERN_ERR "inter_module_register: duplicate im_name '%s'", im_name);
			BUG();
		}
	}
	list_add(&(ime_new->list), &ime_list);
	spin_unlock(&ime_lock);
}

/**
 * inter_module_unregister - unregister a set of inter module data.
 * @im_name: an arbitrary string to identify the data, must be unique
 *
 * Description: Check that the im_name has been registered, complain if
 * it has not.  For existing data, remove it from the
 * inter_module_entry list.
 */
void inter_module_unregister(const char *im_name)
{
	struct list_head *tmp;
	struct inter_module_entry *ime;

	spin_lock(&ime_lock);
	list_for_each(tmp, &ime_list) {
		ime = list_entry(tmp, struct inter_module_entry, list);
		if (strcmp(ime->im_name, im_name) == 0) {
			list_del(&(ime->list));
			spin_unlock(&ime_lock);
			kfree(ime);
			return;
		}
	}
	spin_unlock(&ime_lock);
	if (kmalloc_failed) {
		printk(KERN_ERR
			"inter_module_unregister: no entry for '%s', "
			"probably caused by previous kmalloc failure\n",
			im_name);
		return;
	}
	else {
		/* Program logic error, fatal */
		printk(KERN_ERR "inter_module_unregister: no entry for '%s'", im_name);
		BUG();
	}
}

/**
 * inter_module_get - return arbitrary userdata from another module.
 * @im_name: an arbitrary string to identify the data, must be unique
 *
 * Description: If the im_name has not been registered, return NULL.
 * Try to increment the use count on the owning module, if that fails
 * then return NULL.  Otherwise return the userdata.
 */
const void *inter_module_get(const char *im_name)
{
	struct list_head *tmp;
	struct inter_module_entry *ime;
	const void *result = NULL;

	spin_lock(&ime_lock);
	list_for_each(tmp, &ime_list) {
		ime = list_entry(tmp, struct inter_module_entry, list);
		if (strcmp(ime->im_name, im_name) == 0) {
			if (try_inc_mod_count(ime->owner))
				result = ime->userdata;
			break;
		}
	}
	spin_unlock(&ime_lock);
	return(result);
}

/**
 * inter_module_get_request - im get with automatic request_module.
 * @im_name: an arbitrary string to identify the data, must be unique
 * @modname: module that is expected to register im_name
 *
 * Description: If inter_module_get fails, do request_module then retry.
 */
const void *inter_module_get_request(const char *im_name, const char *modname)
{
	const void *result = inter_module_get(im_name);
	if (!result) {
		request_module(modname);
		result = inter_module_get(im_name);
	}
	return(result);
}

/**
 * inter_module_put - release use of data from another module.
 * @im_name: an arbitrary string to identify the data, must be unique
 *
 * Description: If the im_name has not been registered, complain,
 * otherwise decrement the use count on the owning module.
 */
void inter_module_put(const char *im_name)
{
	struct list_head *tmp;
	struct inter_module_entry *ime;

	spin_lock(&ime_lock);
	list_for_each(tmp, &ime_list) {
		ime = list_entry(tmp, struct inter_module_entry, list);
		if (strcmp(ime->im_name, im_name) == 0) {
			if (ime->owner)
				__MOD_DEC_USE_COUNT(ime->owner);
			spin_unlock(&ime_lock);
			return;
		}
	}
	spin_unlock(&ime_lock);
	printk(KERN_ERR "inter_module_put: no entry for '%s'", im_name);
	BUG();
}


#if defined(CONFIG_MODULES)	/* The rest of the source */

static long get_mod_name(const char *user_name, char **buf);
static void put_mod_name(char *buf);
struct module *find_module(const char *name);
void free_module(struct module *, int tag_freed);


/*
 * Called at boot time
 */

void __init init_modules(void)
{
	kernel_module.nsyms = __stop___ksymtab - __start___ksymtab;

#ifdef __alpha__
	__asm__("stq $29,%0" : "=m"(kernel_module.gp));
#endif
}

/*
 * Copy the name of a module from user space.
 */

static inline long
get_mod_name(const char *user_name, char **buf)
{
	unsigned long page;
	long retval;

	page = __get_free_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;

	retval = strncpy_from_user((char *)page, user_name, PAGE_SIZE);
	if (retval > 0) {
		if (retval < PAGE_SIZE) {
			*buf = (char *)page;
			return retval;
		}
		retval = -ENAMETOOLONG;
	} else if (!retval)
		retval = -EINVAL;

	free_page(page);
	return retval;
}

static inline void
put_mod_name(char *buf)
{
	free_page((unsigned long)buf);
}

/*
1. 打开模块 `.o` 文件，读入 ELF 目标文件
2. 解析 ELF：section、符号表、重定位表
3. `query_module()` 系统调用，向内核查询内核导出符号（`register_chrdev`等）的虚拟地址
4. **用户态完成重定位**：把所有未解析符号，修补成内核真实地址（2.4 最核心特点）
5. 调用 `create_module(name, size)` → 内核分配一块内核空间，返回内核虚拟地址 `base`
6. insmod 把修补好的镜像拷贝到这个内核地址
7. 调用 `init_module(name, module_struct)`，通知内核启动模块
*/

/*
 * Allocate space for a module.
 */

/**
 * sys_create_module - 2.4内核创建动态模块的系统调用
 * @name_user: 用户态传入模块名字字符串
 * @size: 需要为模块分配的总内存大小（包含struct module + 模块名 + 代码/数据/符号表等）
 *
 * 返回值：成功返回内核虚拟地址（struct module *指针，用户态后续传给sys_init_module）
 * 失败返回负错误码：-EPERM / -EINVAL / -EEXIST / -ENOMEM
 *
 * 整体分工：
 * sys_create_module：分配一块内核内存、初始化module头部、挂入全局module_list
 * 用户态insmod：在这块内核内存里填充syms、deps、refs、init、cleanup、ex_table等所有字段
 * sys_init_module：最后做校验、调用init入口
 */
asmlinkage unsigned long
sys_create_module(const char *name_user, size_t size)
{
    char *name;
    long namelen, error;
    struct module *mod;

    // 权限校验：必须拥有 CAP_SYS_MODULE 权限
    if (!capable(CAP_SYS_MODULE))
        return -EPERM;

    // 拿到BKL大锁，保护全局module_list链表
    lock_kernel();

    // 从用户空间拷贝模块名字到内核临时缓冲区name
    if ((namelen = get_mod_name(name_user, &name)) < 0) {
        error = namelen;
        goto err0;
    }

    // 最小size校验：总大小至少要能放下 struct module + 模块名字符串（含'\0'）
    if (size < sizeof(struct module)+namelen) {
        error = -EINVAL;
        goto err1;
    }

    // 检查同名模块是否已经存在，存在返回EEXIST
    if (find_module(name) != NULL) {
        error = -EEXIST;
        goto err1;
    }

    // module_map：内核分配一块size大小的vmalloc内存（模块镜像空间）
    if ((mod = (struct module *)module_map(size)) == NULL) {
        error = -ENOMEM;
        goto err1;
    }

    // module结构体头部清零
    memset(mod, 0, sizeof(*mod));
    // 标记结构体大小，用于modutils版本兼容校验
    mod->size_of_struct = sizeof(*mod);
    // 头插法加入链表：新模块next指向原来的module_list
    mod->next = module_list;
    /*
     * 内存布局重点：
     * mod指向这块内存起始 = struct module
     * mod+1 紧跟在结构体后面，用来存放模块名字字符串
     */
    mod->name = (char *)(mod + 1);
    mod->size = size;
    // 把模块名字复制到结构体后方的内存区域
    memcpy((char*)(mod+1), name, namelen+1);

    // 释放get_mod_name拿到的临时名字缓存
    put_mod_name(name);

    // 更新全局链表头，新模块成为链表第一个节点
    module_list = mod;  /* link it in */

    // 成功：返回mod指针给用户态insmod（unsigned long强转）
    error = (long) mod;
    goto err0;

err1:
    // 失败分支，释放临时名字缓存
    put_mod_name(name);
err0:
    // 释放BKL锁
    unlock_kernel();
    return error;
}

/*
 * Initialize a module.
 */

/**
 * sys_init_module - 2.4内核模块初始化系统调用
 * @name_user: 用户传入模块名称
 * @mod_user: 用户态传入的module虚拟地址（是sys_create_module返回的内核地址）
 *
 * 前置流程：
 * 1. insmod -> sys_create_module 分配整块内核内存，创建module节点挂入链表
 * 2. insmod 在用户态解析ko、重定位，填充整块内存里的struct module、syms、deps、代码段
 * 3. insmod 调用 sys_init_module，内核做合法性校验、建立反向依赖、执行mod->init
 *
 * 核心特点：2.4是【用户态完成ELF解析+重定位】，内核只负责校验、依赖登记、调用init入口
 */
asmlinkage long
sys_init_module(const char *name_user, struct module *mod_user)
{
    struct module mod_tmp, *mod;
    char *name, *n_name, *name_tmp = NULL;
    long namelen, n_namelen, i, error;
    unsigned long mod_user_size;
    struct module_ref *dep;

    // 权限检查：需要 CAP_SYS_MODULE
    if (!capable(CAP_SYS_MODULE))
        return -EPERM;

    // 获取BKL大锁，保护全局module链表
    lock_kernel();

    // 拷贝模块名到内核临时缓冲区
    if ((namelen = get_mod_name(name_user, &name)) < 0) {
        error = namelen;
        goto err0;
    }

    // 根据名字查找之前 sys_create_module 创建好的模块
    if ((mod = find_module(name)) == NULL) {
        error = -ENOENT;
        goto err1;
    }

    /*
     * 校验 module 头部结构体大小
     * 兼容新旧modutils：允许少量富余，防止modutils版本和内核不匹配
     */
    if ((error = get_user(mod_user_size, &mod_user->size_of_struct)) != 0)
        goto err1;
    if (mod_user_size < (unsigned long)&((struct module *)0L)->persist_start
        || mod_user_size > sizeof(struct module) + 16*sizeof(void*)) {
        printk(KERN_ERR "init_module: Invalid module header size.\n"
               KERN_ERR "A new version of the modutils is likely "
               "needed.\n");
        error = -EINVAL;
        goto err1;
    }

    /*
     * 备份原来的module头部信息
     * 如果后面校验失败，用mod_tmp恢复现场，保证链表上的模块不变坏
     */
    mod_tmp = *mod;
    name_tmp = kmalloc(strlen(mod->name) + 1, GFP_KERNEL);  /* Where's kstrdup()? */
    if (name_tmp == NULL) {
        error = -ENOMEM;
        goto err1;
    }
    strcpy(name_tmp, mod->name);

    // 第一步拷贝：拷贝struct module头部（mod_user_size长度）
    error = copy_from_user(mod, mod_user, mod_user_size);
    if (error) {
        error = -EFAULT;
        goto err2;
    }

    // 合法性校验：模块总大小不能超过当初create_module分配的size
    error = -EINVAL;
    if (mod->size > mod_tmp.size) {
        printk(KERN_ERR "init_module: Size of initialized module "
                "exceeds size of created module.\n");
        goto err2;
    }

    /*
     * mod_bound(p, count, mod) 辅助宏：
     * 判断指针p指向的内存，是否落在这块模块分配的内存区间内
     * 防止用户态传入野指针，越界到其他内核内存
     */
    if (!mod_bound(mod->name, namelen, mod)) {
        printk(KERN_ERR "init_module: mod->name out of bounds.\n");
        goto err2;
    }
    if (mod->nsyms && !mod_bound(mod->syms, mod->nsyms, mod)) {
        printk(KERN_ERR "init_module: mod->syms out of bounds.\n");
        goto err2;
    }
    if (mod->ndeps && !mod_bound(mod->deps, mod->ndeps, mod)) {
        printk(KERN_ERR "init_module: mod->deps out of bounds.\n");
        goto err2;
    }
    if (mod->init && !mod_bound(mod->init, 0, mod)) {
        printk(KERN_ERR "init_module: mod->init out of bounds.\n");
        goto err2;
    }
    if (mod->cleanup && !mod_bound(mod->cleanup, 0, mod)) {
        printk(KERN_ERR "init_module: mod->cleanup out of bounds.\n");
        goto err2;
    }

    // 校验异常表ex_table起止地址、对齐
    if (mod->ex_table_start > mod->ex_table_end
        || (mod->ex_table_start &&
        !((unsigned long)mod->ex_table_start >= ((unsigned long)mod + mod->size_of_struct)
          && ((unsigned long)mod->ex_table_end
              < (unsigned long)mod + mod->size)))
        || (((unsigned long)mod->ex_table_start
         - (unsigned long)mod->ex_table_end)
        % sizeof(struct exception_table_entry))) {
        printk(KERN_ERR "init_module: mod->ex_table_* invalid.\n");
        goto err2;
    }

    // 标志位只允许 MOD_AUTOCLEAN，其他非法flag拒绝
    if (mod->flags & ~MOD_AUTOCLEAN) {
        printk(KERN_ERR "init_module: mod->flags invalid.\n");
        goto err2;
    }

#ifdef __alpha__
    if (!mod_bound(mod->gp - 0x8000, 0, mod)) {
        printk(KERN_ERR "init_module: mod->gp out of bounds.\n");
        goto err2;
    }
#endif

    // mod_member_present：判断当前内核版本是否存在该扩展字段
    if (mod_member_present(mod, can_unload)
        && mod->can_unload && !mod_bound(mod->can_unload, 0, mod)) {
        printk(KERN_ERR "init_module: mod->can_unload out of bounds.\n");
        goto err2;
    }
    if (mod_member_present(mod, kallsyms_end)) {
        if (mod->kallsyms_end &&
            (!mod_bound(mod->kallsyms_start, 0, mod) ||
             !mod_bound(mod->kallsyms_end, 0, mod))) {
            printk(KERN_ERR "init_module: mod->kallsyms out of bounds.\n");
            goto err2;
        }
        if (mod->kallsyms_start > mod->kallsyms_end) {
            printk(KERN_ERR "init_module: mod->kallsyms invalid.\n");
            goto err2;
        }
    }
    if (mod_member_present(mod, archdata_end)) {
        if (mod->archdata_end &&
        (!mod_bound(mod->archdata_start, 0, mod) ||
         !mod_bound(mod->archdata_end, 0, mod))) {
            printk(KERN_ERR "init_module: mod->archdata out of bounds.\n");
            goto err2;
        }
        if (mod->archdata_start > mod->archdata_end) {
            printk(KERN_ERR "init_module: mod->archdata invalid.\n");
            goto err2;
        }
    }
    if (mod_member_present(mod, kernel_data) && mod->kernel_data) {
        printk(KERN_ERR "init_module: mod->kernel_data must be zero.\n");
        goto err2;
    }

    // 校验：用户不能偷偷修改模块名字（create和init阶段名字必须一致）
    if ((n_namelen = get_mod_name(mod->name - (unsigned long)mod
                + (unsigned long)mod_user,
                &n_name)) < 0) {
        printk(KERN_ERR "init_module: get_mod_name failure.\n");
        error = n_namelen;
        goto err2;
    }
    if (namelen != n_namelen || strcmp(n_name, mod_tmp.name) != 0) {
        printk(KERN_ERR "init_module: changed module name to "
                "`%s' from `%s'\n",
               n_name, mod_tmp.name);
        goto err3;
    }

    // 第二步拷贝：拷贝头部之后剩余的模块镜像（代码、数据、符号、ex_table等）
    if (copy_from_user((char *)mod+mod_user_size,
               (char *)mod_user+mod_user_size,
               mod->size-mod_user_size)) {
        error = -EFAULT;
        goto err3;
    }

    // 架构相关初始化
    if (module_arch_init(mod))
        goto err3;

    // 刷新指令cache，保证I/D cache一致性，CPU可以正常执行模块里的新代码
    flush_icache_range((unsigned long)mod, (unsigned long)mod + mod->size);

    // 恢复next指针；refs反向引用链表初始化为NULL
    mod->next = mod_tmp.next;
    mod->refs = NULL;

    /*
     * 遍历依赖列表，校验依赖的模块是真实存在、已加载的模块
     * 禁止自依赖；依赖项必须在全局链表中找到
     */
    for (i = 0, dep = mod->deps; i < mod->ndeps; ++i, ++dep) {
        struct module *o, *d = dep->dep;

        if (d == mod) {
            printk(KERN_ERR "init_module: self-referential "
                    "dependency in mod->deps.\n");
            goto err3;
        }

        for (o = module_list; o != &kernel_module && o != d; o = o->next)
            ;

        if (o != d) {
            printk(KERN_ERR "init_module: found dependency that is "
                    "(no longer?) a module.\n");
            goto err3;
        }
    }

    /*
     * 建立反向引用链表（重点！支撑QM_REFS / rmmod卸载检查）
     * dep->ref = 当前模块（依赖方）
     * 把这个dep节点头插到被依赖模块d的refs链表上
     */
    for (i = 0, dep = mod->deps; i < mod->ndeps; ++i, ++dep) {
        struct module *d = dep->dep;

        dep->ref = mod;
        dep->next_ref = d->refs;
        d->refs = dep;
        d->flags |= MOD_USED_ONCE;
    }

    // 释放临时名字缓存
    put_mod_name(n_name);
    put_mod_name(name);

    // 标记正在初始化；引用计数置1，防止初始化过程中被卸载
    mod->flags |= MOD_INITIALIZING;
    atomic_set(&mod->uc.usecount,1);
    // 调用模块的init入口函数（module_init）
    if (mod->init && (error = mod->init()) != 0) {
        atomic_set(&mod->uc.usecount,0);
        mod->flags &= ~MOD_INITIALIZING;
        if (error > 0)  /* Buggy module */
            error = -EBUSY;
        goto err0;
    }
    atomic_dec(&mod->uc.usecount);

    // 初始化成功，标记模块为运行态，清除INITIALIZING标记
    mod->flags = (mod->flags | MOD_RUNNING) & ~MOD_INITIALIZING;
    error = 0;
    goto err0;

err3:
    put_mod_name(n_name);
err2:
    // 校验失败：恢复原来的module结构体和名字，回滚现场
    *mod = mod_tmp;
    strcpy((char *)mod->name, name_tmp);
err1:
    put_mod_name(name);
err0:
    unlock_kernel();
    kfree(name_tmp);
    return error;
}

static spinlock_t unload_lock = SPIN_LOCK_UNLOCKED;
int try_inc_mod_count(struct module *mod)
{
	int res = 1;
	if (mod) {
		spin_lock(&unload_lock);
		if (mod->flags & MOD_DELETED)
			res = 0;
		else
			__MOD_INC_USE_COUNT(mod);
		spin_unlock(&unload_lock);
	}
	return res;
}

/**
 * sys_delete_module - 2.4内核 rmmod 对应的系统调用
 * @name_user: 待删除模块名字；传 NULL 表示自动收割 AUTOCLEAN 模块（kmod自动卸载）
 *
 * 两条分支：
 * 1. name_user != NULL：用户手动 rmmod 【指定模块】
 * 2. name_user == NULL：自动扫描链表，收割标记 MOD_AUTOCLEAN 的闲置模块
 *
 * 依赖前置：sys_init_module 已经建好反向 refs 链表，用来判断有没有别的模块依赖它
 */
asmlinkage long
sys_delete_module(const char *name_user)
{
    struct module *mod, *next;
    char *name;
    long error;
    int something_changed;

    // 权限校验：需要 CAP_SYS_MODULE
    if (!capable(CAP_SYS_MODULE))
        return -EPERM;

    // 拿BKL大锁，保护 module_list 全局链表
    lock_kernel();

    // ========== 分支一：传入模块名，手动卸载指定模块（rmmod xxx） ==========
    if (name_user) {
        // 从用户态拷贝模块名到内核临时缓冲区
        if ((error = get_mod_name(name_user, &name)) < 0)
            goto out;
        // 空名字，非法
        if (error == 0) {
            error = -EINVAL;
            put_mod_name(name);
            goto out;
        }
        // 根据名字查找存活模块（find_module 自动跳过 MOD_DELETED）
        error = -ENOENT;
        if ((mod = find_module(name)) == NULL) {
            put_mod_name(name);
            goto out;
        }
        put_mod_name(name);

        // 【关键检查】mod->refs != NULL：还有其他模块依赖本模块，不能卸载
        error = -EBUSY;
        if (mod->refs != NULL)
            goto out;

        // unload_lock 自旋锁：保护模块引用计数usecount和flags
        spin_lock(&unload_lock);
        // __MOD_IN_USE：判断模块引用计数是否 > 0（还被内核/进程使用）
        if (!__MOD_IN_USE(mod)) {
            // 打上 MOD_DELETED 标记，后续 find_module 看不见这个模块
            mod->flags |= MOD_DELETED;
            spin_unlock(&unload_lock);
            // free_module：执行 cleanup、拆除依赖链表、释放内存
            free_module(mod, 0);
            error = 0;
        } else {
            // 引用计数不为0，忙，返回-EBUSY
            spin_unlock(&unload_lock);
        }
        goto out;
    }

    // ========== 分支二：name_user == NULL，自动收割（kmod 自动清理） ==========
restart:
    something_changed = 0;
    // 遍历模块链表，哨兵 &kernel_module
    for (mod = module_list; mod != &kernel_module; mod = next) {
        next = mod->next;  // 提前缓存next，防止free_module之后节点失效
        spin_lock(&unload_lock);

        // 自动卸载的全部条件（全部满足才可以删）
        if (mod->refs == NULL                // 没有其他模块依赖它
            && (mod->flags & MOD_AUTOCLEAN)   // 开启自动清理标记
            && (mod->flags & MOD_RUNNING)     // 模块正常运行中
            && !(mod->flags & MOD_DELETED)     // 还没有标记删除
            && (mod->flags & MOD_USED_ONCE)    // 曾经被使用过一次
            && !__MOD_IN_USE(mod)) {           // 当前引用计数为0，没人在用

            if ((mod->flags & MOD_VISITED)
                && !(mod->flags & MOD_JUST_FREED)) {
                // 已经一轮访问过，本轮直接跳过，清除VISITED标记
                spin_unlock(&unload_lock);
                mod->flags &= ~MOD_VISITED;
            } else {
                // 打上删除标记，调用free_module释放
                mod->flags |= MOD_DELETED;
                spin_unlock(&unload_lock);
                free_module(mod, 1);
                something_changed = 1;
            }
        } else {
            // 任意条件不满足，跳过
            spin_unlock(&unload_lock);
        }
    }
    // 如果本轮删掉了模块，链表发生变化，重新从头扫描（restart）
    if (something_changed)
        goto restart;

    // 一轮稳定扫描结束，清除所有 MOD_JUST_FREED 标记
    for (mod = module_list; mod != &kernel_module; mod = mod->next)
        mod->flags &= ~MOD_JUST_FREED;

    error = 0;
out:
    unlock_kernel();
    return error;
}


/* Query various bits about modules.  */

/**
 * qm_modules - QM_MODULES 对应的处理函数，收集所有动态模块名字
 * @buf: 用户态缓冲区，用来存放各个模块名字（字符串以'\0'分隔）
 * @bufsize: 用户buf总大小
 * @ret: 用户态指针，回填总共需要多少字节
 *
 * 调用来源：sys_query_module(..., QM_MODULES, ...)
 * 返回：0 全部拷贝成功；-ENOSPC 缓冲区太小；-EFAULT 用户地址非法
 *
 * 重要：遍历终止条件 mod != &kernel_module
 * kernel_module 伪模块不在 module_list 链表内，作为链表尾哨兵
 */
static int
qm_modules(char *buf, size_t bufsize, size_t *ret)
{
    struct module *mod;
    size_t nmod, space, len;

    nmod = space = 0;

    // 第一轮遍历：尝试往用户buf拷贝模块名
    for (mod=module_list; mod != &kernel_module; mod=mod->next, ++nmod) {
        // +1 包含字符串结束符 '\0'
        len = strlen(mod->name)+1;

        // 当前这个模块名字已经塞不下剩余buf，进入【只计算所需空间】分支
        if (len > bufsize)
            goto calc_space_needed;

        // 拷贝模块名字串到用户空间
        if (copy_to_user(buf, mod->name, len))
            return -EFAULT;

        // 缓冲区指针后移、剩余容量扣减、累计已写入字节
        buf += len;
        bufsize -= len;
        space += len;
    }

    // 全部模块拷贝完成，把总数量nmod写入用户ret（2.4这里ret同时承载总字节需求）
    if (put_user(nmod, ret))
        return -EFAULT;
    else
        return 0;

calc_space_needed:
    // buf空间不足，不再拷贝，只统计全部模块一共需要多少字节
    space += len;
    while ((mod = mod->next) != &kernel_module)
        space += strlen(mod->name)+1;

    // 将需要的总字节数回填给用户，返回缓冲区不足标记
    if (put_user(space, ret))
        return -EFAULT;
    else
        return -ENOSPC;
}

/**
 * qm_deps - QM_DEPS 分支处理函数，查询指定模块的依赖模块列表
 * @mod: 目标模块指针
 * @buf: 用户态缓冲区，存放依赖模块名字串，'\0'分隔
 * @bufsize: 用户缓冲区字节大小
 * @ret: 用户指针，回填结果信息
 *
 * 调用入口：sys_query_module(..., QM_DEPS, ...)
 * 返回值：
 *    0：拷贝成功，*ret = 依赖模块数量
 *   -ENOSPC：缓冲区太小，*ret = 所需总字节
 *   -EINVAL：查询内核伪模块kernel_module（内核无依赖）
 *   -EFAULT：copy_to_user/put_user访问用户地址失败
 */
static int
qm_deps(struct module *mod, char *buf, size_t bufsize, size_t *ret)
{
    size_t i, space, len;

    // kernel_module是内核本体，不存在依赖模块，直接非法
    if (mod == &kernel_module)
        return -EINVAL;

    // MOD_CAN_QUERY：判断模块是否允许被查询
    if (!MOD_CAN_QUERY(mod))
        if (put_user(0, ret))
            return -EFAULT;
        else
            return 0;

    space = 0;
    // 遍历本模块的deps数组：mod->ndeps 依赖项个数
    for (i = 0; i < mod->ndeps; ++i) {
        // dep：被依赖的模块，取出它的名字
        const char *dep_name = mod->deps[i].dep->name;

        // +1 包含字符串结束符'\0'
        len = strlen(dep_name)+1;

        // 剩余缓冲区放不下当前名字，跳转到只统计空间的分支
        if (len > bufsize)
            goto calc_space_needed;

        // 把依赖模块名字拷贝到用户空间
        if (copy_to_user(buf, dep_name, len))
            return -EFAULT;

        buf += len;
        bufsize -= len;
        space += len;
    }

    // 全部依赖拷贝成功：*ret 填入依赖的数量 i（等于ndeps）
    if (put_user(i, ret))
        return -EFAULT;
    else
        return 0;

calc_space_needed:
    // 当前这条放不下，先加上当前这条长度
    space += len;
    // 继续遍历剩下所有依赖，累加全部需要的字节
    while (++i < mod->ndeps)
        space += strlen(mod->deps[i].dep->name)+1;

    // *ret填入总共需要的字节，返回缓冲区不足标记
    if (put_user(space, ret))
        return -EFAULT;
    else
        return -ENOSPC;
}


static int
qm_refs(struct module *mod, char *buf, size_t bufsize, size_t *ret)
{
	size_t nrefs, space, len;
	struct module_ref *ref;

	if (mod == &kernel_module)
		return -EINVAL;
	if (!MOD_CAN_QUERY(mod))
		if (put_user(0, ret))
			return -EFAULT;
		else
			return 0;

	space = 0;
	for (nrefs = 0, ref = mod->refs; ref ; ++nrefs, ref = ref->next_ref) {
		const char *ref_name = ref->ref->name;

		len = strlen(ref_name)+1;
		if (len > bufsize)
			goto calc_space_needed;
		if (copy_to_user(buf, ref_name, len))
			return -EFAULT;
		buf += len;
		bufsize -= len;
		space += len;
	}

	if (put_user(nrefs, ret))
		return -EFAULT;
	else
		return 0;

calc_space_needed:
	space += len;
	while ((ref = ref->next_ref) != NULL)
		space += strlen(ref->ref->name)+1;

	if (put_user(space, ret))
		return -EFAULT;
	else
		return -ENOSPC;
}

static int
qm_symbols(struct module *mod, char *buf, size_t bufsize, size_t *ret)
{
	size_t i, space, len;
	struct module_symbol *s;
	char *strings;
	unsigned long *vals;

	if (!MOD_CAN_QUERY(mod))
		if (put_user(0, ret))
			return -EFAULT;
		else
			return 0;

	space = mod->nsyms * 2*sizeof(void *);

	i = len = 0;
	s = mod->syms;

	if (space > bufsize)
		goto calc_space_needed;

	if (!access_ok(VERIFY_WRITE, buf, space))
		return -EFAULT;

	bufsize -= space;
	vals = (unsigned long *)buf;
	strings = buf+space;

	for (; i < mod->nsyms ; ++i, ++s, vals += 2) {
		len = strlen(s->name)+1;
		if (len > bufsize)
			goto calc_space_needed;

		if (copy_to_user(strings, s->name, len)
		    || __put_user(s->value, vals+0)
		    || __put_user(space, vals+1))
			return -EFAULT;

		strings += len;
		bufsize -= len;
		space += len;
	}

	if (put_user(i, ret))
		return -EFAULT;
	else
		return 0;

calc_space_needed:
	for (; i < mod->nsyms; ++i, ++s)
		space += strlen(s->name)+1;

	if (put_user(space, ret))
		return -EFAULT;
	else
		return -ENOSPC;
}

static int
qm_info(struct module *mod, char *buf, size_t bufsize, size_t *ret)
{
	int error = 0;

	if (mod == &kernel_module)
		return -EINVAL;

	if (sizeof(struct module_info) <= bufsize) {
		struct module_info info;
		info.addr = (unsigned long)mod;
		info.size = mod->size;
		info.flags = mod->flags;
		info.usecount = (mod_member_present(mod, can_unload)
				 && mod->can_unload ? -1 : atomic_read(&mod->uc.usecount));

		if (copy_to_user(buf, &info, sizeof(struct module_info)))
			return -EFAULT;
	} else
		error = -ENOSPC;

	if (put_user(sizeof(struct module_info), ret))
		return -EFAULT;

	return error;
}

/*
1. 打开模块 `.o` 文件，读入 ELF 目标文件
2. 解析 ELF：section、符号表、重定位表
3. `query_module()` 系统调用，向内核查询内核导出符号（`register_chrdev`等）的虚拟地址
4. **用户态完成重定位**：把所有未解析符号，修补成内核真实地址（2.4 最核心特点）
5. 调用 `create_module(name, size)` → 内核分配一块内核空间，返回内核虚拟地址 `base`
6. insmod 把修补好的镜像拷贝到这个内核地址
7. 调用 `init_module(name, module_struct)`，通知内核启动模块
*/
/**
 * sys_query_module - 2.4内核模块查询系统调用，供用户态insmod/lsmod使用
 * @name_user: 用户态传入模块名字符串，NULL代表查询内核本身(kernel_module)
 * @which: 查询子命令类型 QM_xxx，控制本次要查什么内容
 * @buf: 用户态缓冲区，内核把结果拷贝到这里返回给应用
 * @bufsize: 用户缓冲区长度，防止越界
 * @ret: 用户态指针，内核回填【实际需要的字节数】
 *
 * 返回值：0成功；负数错误码
 *
 * 核心背景：2.4的insmod靠这个系统调用拿导出符号地址，在用户侧完成重定位
 */
asmlinkage long
sys_query_module(const char *name_user, int which, char *buf, size_t bufsize,
         size_t *ret)
{
    struct module *mod;
    int err;

    // 2.4内核大锁BKL，保护module_list和模块结构体遍历
    lock_kernel();

    // name_user == NULL：指定查询内核本身（内核导出符号）
    if (name_user == NULL)
        mod = &kernel_module;
    else {
        long namelen;
        char *name;

        // 从用户空间拷贝模块名字到内核缓冲区name
        if ((namelen = get_mod_name(name_user, &name)) < 0) {
            err = namelen;
            goto out;
        }
        err = -ENOENT;
        // 名字是空串，同样指向内核本身
        if (namelen == 0)
            mod = &kernel_module;
        // 根据模块名遍历全局module_list，查找对应模块
        else if ((mod = find_module(name)) == NULL) {
            put_mod_name(name);
            goto out;
        }
        // 释放get_mod_name分配/拿到的名字缓存
        put_mod_name(name);
    }

    // which：查询类型分发
    switch (which)
    {
    case 0:
        err = 0;
        break;
    case QM_MODULES:
        // QM_MODULES：获取当前所有已经加载的模块名称列表
        err = qm_modules(buf, bufsize, ret);
        break;
    case QM_DEPS:
        // QM_DEPS：获取该模块依赖的其他模块
        err = qm_deps(mod, buf, bufsize, ret);
        break;
    case QM_REFS:
        // QM_REFS：获取引用了本模块的其他模块（被谁依赖）
        err = qm_refs(mod, buf, bufsize, ret);
        break;
    case QM_SYMBOLS:
        // QM_SYMBOLS：【insmod核心】获取该模块导出符号表：符号名 + 符号内核虚拟地址
        // insmod就是用这个拿到 register_chrdev 这类内核符号地址，用户态做重定位修补
        err = qm_symbols(mod, buf, bufsize, ret);
        break;
    case QM_INFO:
        // QM_INFO：获取模块基础信息（size、usecount、flags等）
        err = qm_info(mod, buf, bufsize, ret);
        break;
    default:
        // which参数非法
        err = -EINVAL;
        break;
    }
out:
    unlock_kernel();
    return err;
}

/*
 * Copy the kernel symbol table to user space.  If the argument is
 * NULL, just return the size of the table.
 *
 * This call is obsolete.  New programs should use query_module+QM_SYMBOLS
 * which does not arbitrarily limit the length of symbols.
 */

asmlinkage long
sys_get_kernel_syms(struct kernel_sym *table)
{
	struct module *mod;
	int i;
	struct kernel_sym ksym;

	lock_kernel();
	for (mod = module_list, i = 0; mod; mod = mod->next) {
		/* include the count for the module name! */
		i += mod->nsyms + 1;
	}

	if (table == NULL)
		goto out;

	/* So that we don't give the user our stack content */
	memset (&ksym, 0, sizeof (ksym));

	for (mod = module_list, i = 0; mod; mod = mod->next) {
		struct module_symbol *msym;
		unsigned int j;

		if (!MOD_CAN_QUERY(mod))
			continue;

		/* magic: write module info as a pseudo symbol */
		ksym.value = (unsigned long)mod;
		ksym.name[0] = '#';
		strncpy(ksym.name+1, mod->name, sizeof(ksym.name)-1);
		ksym.name[sizeof(ksym.name)-1] = '\0';

		if (copy_to_user(table, &ksym, sizeof(ksym)) != 0)
			goto out;
		++i, ++table;

		if (mod->nsyms == 0)
			continue;

		for (j = 0, msym = mod->syms; j < mod->nsyms; ++j, ++msym) {
			ksym.value = msym->value;
			strncpy(ksym.name, msym->name, sizeof(ksym.name));
			ksym.name[sizeof(ksym.name)-1] = '\0';

			if (copy_to_user(table, &ksym, sizeof(ksym)) != 0)
				goto out;
			++i, ++table;
		}
	}
out:
	unlock_kernel();
	return i;
}

/*
 * Look for a module by name, ignoring modules marked for deletion.
 */

/**
 * find_module - 根据模块名字在全局模块链表中查找存活模块
 * @name: 待查找的模块名字字符串
 *
 * 返回值：找到返回对应struct module指针；找不到返回NULL
 * 注意：调用者需要提前持有BKL（lock_kernel）保护module_list链表
 */
struct module *
find_module(const char *name)
{
    struct module *mod;

    // 从全局链表头 module_list 开始遍历单向链表
    for (mod = module_list; mod ; mod = mod->next) {
        // MOD_DELETED：标记已经被标记删除、正在清理中的模块，直接跳过
        if (mod->flags & MOD_DELETED)
            continue;
        // 名字匹配，跳出循环
        if (!strcmp(mod->name, name))
            break;
    }

    // 两种情况返回：
    // 1. 找到：mod 指向目标模块
    // 2. 遍历到链表末尾（mod == NULL）：没有匹配模块
    return mod;
}

/*
 * Free the given module.
 */

/**
 * free_module - 模块真正清理释放函数
 * @mod: 待释放模块
 * @tag_freed: 1=kmod自动卸载；0=用户手动rmmod，用来控制MOD_JUST_FREED标记
 *
 * 调用来源：sys_delete_module（手动卸载 / 自动收割分支）
 * 职责：执行cleanup、摘除反向依赖refs链表、摘除全局module_list、释放vmalloc内存
 */
void
free_module(struct module *mod, int tag_freed)
{
    struct module_ref *dep;
    unsigned i;

    /* Let the module clean up. */
    // 只有模块处于RUNNING状态，才执行反初始化cleanup，MOD_RUNNING说明执行了init函数了
    if (mod->flags & MOD_RUNNING)
    {
		//调用驱动模块的卸载函数
        if(mod->cleanup)
            mod->cleanup();
        // 清除运行标记，cleanup执行完模块不再对外提供服务
        mod->flags &= ~MOD_RUNNING;
    }

    /* Remove the module from the dependency lists. */
    // 遍历本模块所有正向依赖deps：我依赖了哪些模块
    for (i = 0, dep = mod->deps; i < mod->ndeps; ++i, ++dep) {
        struct module_ref **pp;
        // 在【被依赖模块d->refs反向链表】找到当前dep节点，摘除
        for (pp = &dep->dep->refs; *pp != dep; pp = &(*pp)->next_ref)
            continue;
        // 链表摘除：跳过当前dep
        *pp = dep->next_ref;
        // 如果是自动卸载模式，并且被依赖模块的refs链表变空了，打上MOD_JUST_FREED
        if (tag_freed && dep->dep->refs == NULL)
            dep->dep->flags |= MOD_JUST_FREED;
    }

    /* And from the main module list. */
    // 把mod从全局module_list单向链表摘除
    if (mod == module_list) {
        // 是链表头节点，直接移动头指针
        module_list = mod->next;
    } else {
        // 不是表头，从头遍历找到前驱节点p
        struct module *p;
        for (p = module_list; p->next != mod; p = p->next)
            continue;
        p->next = mod->next;
    }

    /* And free the memory. */
    // 释放当初sys_create_module通过module_map分配的整块vmalloc内存
    module_unmap(mod);
}

/*
 * Called by the /proc file system to return a current list of modules.
 */

int get_module_list(char *p)
{
	size_t left = PAGE_SIZE;
	struct module *mod;
	char tmpstr[64];
	struct module_ref *ref;

	for (mod = module_list; mod != &kernel_module; mod = mod->next) {
		long len;
		const char *q;

#define safe_copy_str(str, len)						\
		do {							\
			if (left < len)					\
				goto fini;				\
			memcpy(p, str, len); p += len, left -= len;	\
		} while (0)
#define safe_copy_cstr(str)	safe_copy_str(str, sizeof(str)-1)

		len = strlen(mod->name);
		safe_copy_str(mod->name, len);

		if ((len = 20 - len) > 0) {
			if (left < len)
				goto fini;
			memset(p, ' ', len);
			p += len;
			left -= len;
		}

		len = sprintf(tmpstr, "%8lu", mod->size);
		safe_copy_str(tmpstr, len);

		if (mod->flags & MOD_RUNNING) {
			len = sprintf(tmpstr, "%4ld",
				      (mod_member_present(mod, can_unload)
				       && mod->can_unload
				       ? -1L : (long)atomic_read(&mod->uc.usecount)));
			safe_copy_str(tmpstr, len);
		}

		if (mod->flags & MOD_DELETED)
			safe_copy_cstr(" (deleted)");
		else if (mod->flags & MOD_RUNNING) {
			if (mod->flags & MOD_AUTOCLEAN)
				safe_copy_cstr(" (autoclean)");
			if (!(mod->flags & MOD_USED_ONCE))
				safe_copy_cstr(" (unused)");
		}
		else if (mod->flags & MOD_INITIALIZING)
			safe_copy_cstr(" (initializing)");
		else
			safe_copy_cstr(" (uninitialized)");

		if ((ref = mod->refs) != NULL) {
			safe_copy_cstr(" [");
			while (1) {
				q = ref->ref->name;
				len = strlen(q);
				safe_copy_str(q, len);

				if ((ref = ref->next_ref) != NULL)
					safe_copy_cstr(" ");
				else
					break;
			}
			safe_copy_cstr("]");
		}
		safe_copy_cstr("\n");

#undef safe_copy_str
#undef safe_copy_cstr
	}

fini:
	return PAGE_SIZE - left;
}

/*
 * Called by the /proc file system to return a current list of ksyms.
 */

int
get_ksyms_list(char *buf, char **start, off_t offset, int length)
{
	struct module *mod;
	char *p = buf;
	int len     = 0;	/* code from  net/ipv4/proc.c */
	off_t pos   = 0;
	off_t begin = 0;

	for (mod = module_list; mod; mod = mod->next) {
		unsigned i;
		struct module_symbol *sym;

		if (!MOD_CAN_QUERY(mod))
			continue;

		for (i = mod->nsyms, sym = mod->syms; i > 0; --i, ++sym) {
			p = buf + len;
			if (*mod->name) {
				len += sprintf(p, "%0*lx %s\t[%s]\n",
					       (int)(2*sizeof(void*)),
					       sym->value, sym->name,
					       mod->name);
			} else {
				len += sprintf(p, "%0*lx %s\n",
					       (int)(2*sizeof(void*)),
					       sym->value, sym->name);
			}
			pos = begin + len;
			if (pos < offset) {
				len = 0;
				begin = pos;
			}
			pos = begin + len;
			if (pos > offset+length)
				goto leave_the_loop;
		}
	}
leave_the_loop:
	*start = buf + (offset - begin);
	len -= (offset - begin);
	if (len > length)
		len = length;
	return len;
}

#else		/* CONFIG_MODULES */

/* Dummy syscalls for people who don't want modules */

asmlinkage unsigned long
sys_create_module(const char *name_user, size_t size)
{
	return -ENOSYS;
}

asmlinkage long
sys_init_module(const char *name_user, struct module *mod_user)
{
	return -ENOSYS;
}

asmlinkage long
sys_delete_module(const char *name_user)
{
	return -ENOSYS;
}

asmlinkage long
sys_query_module(const char *name_user, int which, char *buf, size_t bufsize,
		 size_t *ret)
{
	/* Let the program know about the new interface.  Not that
	   it'll do them much good.  */
	if (which == 0)
		return 0;

	return -ENOSYS;
}

asmlinkage long
sys_get_kernel_syms(struct kernel_sym *table)
{
	return -ENOSYS;
}

int try_inc_mod_count(struct module *mod)
{
	return 1;
}

#endif	/* CONFIG_MODULES */
