/*
 * Dynamic loading of modules into the kernel.
 *
 * Rewritten by Richard Henderson <rth@tamu.edu> Dec 1996
 */

#ifndef _LINUX_MODULE_H
#define _LINUX_MODULE_H

#include <linux/config.h>
#include <linux/spinlock.h>
#include <linux/list.h>

#ifdef __GENKSYMS__
#  define _set_ver(sym) sym
#  undef  MODVERSIONS
#  define MODVERSIONS
#else /* ! __GENKSYMS__ */
# if !defined(MODVERSIONS) && defined(EXPORT_SYMTAB)
#   define _set_ver(sym) sym
#   include <linux/modversions.h>
# endif
#endif /* __GENKSYMS__ */

#include <asm/atomic.h>

/* Don't need to bring in all of uaccess.h just for this decl.  */
struct exception_table_entry;

/* Used by get_kernel_syms, which is obsolete.  */
/**
 * struct kernel_sym
 * 内核原始导出符号条目（链接脚本 __ksymtab 里的原始条目）
 */
struct kernel_sym
{
    unsigned long value;
    char name[60];       /* should have been 64-sizeof(long); oh well */
};

/**
 * struct module_symbol
 * 动态模块自己的导出符号条目，放在module->syms指向的数组
 */
struct module_symbol
{
    unsigned long value;
    const char *name;
};

/**
 * struct module_ref
 * 模块依赖引用节点，用来维护模块之间依赖关系
 */
struct module_ref
{
    struct module *dep; /* "parent" pointer 被依赖模块（我依赖dep） */
    struct module *ref; /* "child" pointer 依赖方模块（ref依赖我） */
    struct module_ref *next_ref;
};


/* TBD */
struct module_persist;
/**
 * struct module - Linux 2.4 内核动态模块描述结构体
 * 每个动态加载ko对应一个；kernel_module是静态实例代表内核本身
 */
struct module
{
    // 结构体自身大小，用于版本兼容校验，防止用户态modutils和内核版本不匹配
    unsigned long size_of_struct;   /* == sizeof(module) */
    // 链表指针，挂入全局 module_list 单向链表
    struct module *next;
    // 模块名字字符串
    const char *name;
    // 模块整体镜像占用字节大小（代码+数据+bss）
    unsigned long size;

    union
    {
        // 模块引用计数：MOD_INC_USE_COUNT / MOD_DEC_USE_COUNT 操作它
        atomic_t usecount;
        // pad 填充，保证联合体整体大小固定，兼容不同对齐、架构
        long pad;
    } uc;   /* Needs to keep its size - so says rth */

    // 模块状态标志位：MOD_RUNNING、AUTOCLEAN 等
    unsigned long flags;            /* AUTOCLEAN et al */

    // nsyms：导出符号的数量
    unsigned nsyms;
    // ndeps：依赖其他模块的数量
    unsigned ndeps;

    // syms：导出符号表数组首地址，struct module_symbol[]
    struct module_symbol *syms;
    // deps：本模块依赖的模块列表
    struct module_ref *deps;
    // refs：依赖本模块的其他模块列表（反向引用）
    struct module_ref *refs;

    // 模块初始化入口函数，sys_init_module 里调用
    int (*init)(void);
    // 模块卸载清理入口函数，sys_delete_module 里调用
    void (*cleanup)(void);

    // 模块的异常修正表起止，处理 copy_from_user 等用户空间访问异常
    const struct exception_table_entry *ex_table_start;
    const struct exception_table_entry *ex_table_end;

#ifdef __alpha__
    // alpha架构gp全局指针，其他架构不使用
    unsigned long gp;
#endif
    /* Members past this point are extensions to the basic
       module support and are optional.  Use mod_member_present()
       to examine them.  */
    // 下面属于扩展字段，老版本modutils可能不支持，用mod_member_present判断是否存在

    // persist：持久化数据段（模块热插拔持久保存，较少使用）
    const struct module_persist *persist_start;
    const struct module_persist *persist_end;

    // can_unload：回调函数，额外判断是否允许卸载模块
    int (*can_unload)(void);
    // runsize：modutils预留字段，内核2.4没有实际使用
    int runsize;                /* In modutils, not currently used */

    // kallsyms 模块全部符号起止，oops、kdb调试打印函数名
    const char *kallsyms_start; /* All symbols for kernel debugging */
    const char *kallsyms_end;

    // 架构相关私有数据段
    const char *archdata_start; /* arch specific data for module */
    const char *archdata_end;

    // 内核保留字段
    const char *kernel_data;    /* Reserved for kernel internal use */
};

struct module_info
{
	unsigned long addr;
	unsigned long size;
	unsigned long flags;
	long usecount;
};

/* Bits of module.flags.  */

#define MOD_UNINITIALIZED	0
#define MOD_RUNNING		1
#define MOD_DELETED		2
#define MOD_AUTOCLEAN		4
#define MOD_VISITED  		8
#define MOD_USED_ONCE		16
#define MOD_JUST_FREED		32
#define MOD_INITIALIZING	64

/* Values for query_module's which.  */

#define QM_MODULES      1   // 所有模块名列表
#define QM_DEPS         2   // 依赖的模块
#define QM_REFS         3   // 引用本模块的模块
#define QM_SYMBOLS      4   // 导出符号表（最重要）
#define QM_INFO         5   // 模块基础信息

/* Can the module be queried? */
#define MOD_CAN_QUERY(mod) (((mod)->flags & (MOD_RUNNING | MOD_INITIALIZING)) && !((mod)->flags & MOD_DELETED))

/* When struct module is extended, we must test whether the new member
   is present in the header received from insmod before we can use it.  
   This function returns true if the member is present.  */

#define mod_member_present(mod,member) 					\
	((unsigned long)(&((struct module *)0L)->member + 1)		\
	 <= (mod)->size_of_struct)

/* Check if an address p with number of entries n is within the body of module m */
#define mod_bound(p, n, m) ((unsigned long)(p) >= ((unsigned long)(m) + ((m)->size_of_struct)) && \
	         (unsigned long)((p)+(n)) <= (unsigned long)(m) + (m)->size)

/* Backwards compatibility definition.  */

#define GET_USE_COUNT(module)	(atomic_read(&(module)->uc.usecount))

/* Poke the use count of a module.  */

#define __MOD_INC_USE_COUNT(mod)					\
	(atomic_inc(&(mod)->uc.usecount), (mod)->flags |= MOD_VISITED|MOD_USED_ONCE)
#define __MOD_DEC_USE_COUNT(mod)					\
	(atomic_dec(&(mod)->uc.usecount), (mod)->flags |= MOD_VISITED)
#define __MOD_IN_USE(mod)						\
	(mod_member_present((mod), can_unload) && (mod)->can_unload	\
	 ? (mod)->can_unload() : atomic_read(&(mod)->uc.usecount))

/* Indirect stringification.  */

#define __MODULE_STRING_1(x)	#x
#define __MODULE_STRING(x)	__MODULE_STRING_1(x)

/* Generic inter module communication.
 *
 * NOTE: This interface is intended for small amounts of data that are
 *       passed between two objects and either or both of the objects
 *       might be compiled as modules.  Do not over use this interface.
 *
 *       If more than two objects need to communicate then you probably
 *       need a specific interface instead of abusing this generic
 *       interface.  If both objects are *always* built into the kernel
 *       then a global extern variable is good enough, you do not need
 *       this interface.
 *
 * Keith Owens <kaos@ocs.com.au> 28 Oct 2000.
 */

#ifdef __KERNEL__
#define HAVE_INTER_MODULE
extern void inter_module_register(const char *, struct module *, const void *);
extern void inter_module_unregister(const char *);
extern const void *inter_module_get(const char *);
extern const void *inter_module_get_request(const char *, const char *);
extern void inter_module_put(const char *);

struct inter_module_entry {
	struct list_head list;
	const char *im_name;
	struct module *owner;
	const void *userdata;
};

extern int try_inc_mod_count(struct module *mod);
#endif /* __KERNEL__ */

#if defined(MODULE) && !defined(__GENKSYMS__)

/* Embedded module documentation macros.  */

/* For documentation purposes only.  */

#define MODULE_AUTHOR(name)						   \
const char __module_author[] __attribute__((section(".modinfo"))) = 	   \
"author=" name

#define MODULE_DESCRIPTION(desc)					   \
const char __module_description[] __attribute__((section(".modinfo"))) =   \
"description=" desc

/* Could potentially be used by kmod...  */

#define MODULE_SUPPORTED_DEVICE(dev)					   \
const char __module_device[] __attribute__((section(".modinfo"))) = 	   \
"device=" dev

/* Used to verify parameters given to the module.  The TYPE arg should
   be a string in the following format:
   	[min[-max]]{b,h,i,l,s}
   The MIN and MAX specifiers delimit the length of the array.  If MAX
   is omitted, it defaults to MIN; if both are omitted, the default is 1.
   The final character is a type specifier:
	b	byte
	h	short
	i	int
	l	long
	s	string
*/

#define MODULE_PARM(var,type)			\
const char __module_parm_##var[]		\
__attribute__((section(".modinfo"))) =		\
"parm_" __MODULE_STRING(var) "=" type

#define MODULE_PARM_DESC(var,desc)		\
const char __module_parm_desc_##var[]		\
__attribute__((section(".modinfo"))) =		\
"parm_desc_" __MODULE_STRING(var) "=" desc

/*
 * MODULE_DEVICE_TABLE exports information about devices
 * currently supported by this module.  A device type, such as PCI,
 * is a C-like identifier passed as the first arg to this macro.
 * The second macro arg is the variable containing the device
 * information being made public.
 *
 * The following is a list of known device types (arg 1),
 * and the C types which are to be passed as arg 2.
 * pci - struct pci_device_id - List of PCI ids supported by this module
 * isapnp - struct isapnp_device_id - List of ISA PnP ids supported by this module
 * usb - struct usb_device_id - List of USB ids supported by this module
 */
#define MODULE_GENERIC_TABLE(gtype,name)	\
static const unsigned long __module_##gtype##_size \
  __attribute__ ((unused)) = sizeof(struct gtype##_id); \
static const struct gtype##_id * __module_##gtype##_table \
  __attribute__ ((unused)) = name
#define MODULE_DEVICE_TABLE(type,name)		\
  MODULE_GENERIC_TABLE(type##_device,name)
/* not put to .modinfo section to avoid section type conflicts */

/* The attributes of a section are set the first time the section is
   seen; we want .modinfo to not be allocated.  */

__asm__(".section .modinfo\n\t.previous");

/* Define the module variable, and usage macros.  */
extern struct module __this_module;

#define THIS_MODULE		(&__this_module)
#define MOD_INC_USE_COUNT	__MOD_INC_USE_COUNT(THIS_MODULE)
#define MOD_DEC_USE_COUNT	__MOD_DEC_USE_COUNT(THIS_MODULE)
#define MOD_IN_USE		__MOD_IN_USE(THIS_MODULE)

#include <linux/version.h>
static const char __module_kernel_version[] __attribute__((section(".modinfo"))) =
"kernel_version=" UTS_RELEASE;
#ifdef MODVERSIONS
static const char __module_using_checksums[] __attribute__((section(".modinfo"))) =
"using_checksums=1";
#endif

#else /* MODULE */

#define MODULE_AUTHOR(name)
#define MODULE_DESCRIPTION(desc)
#define MODULE_SUPPORTED_DEVICE(name)
#define MODULE_PARM(var,type)
#define MODULE_PARM_DESC(var,desc)
#define MODULE_GENERIC_TABLE(gtype,name)
#define MODULE_DEVICE_TABLE(type,name)

#ifndef __GENKSYMS__

#define THIS_MODULE		NULL
#define MOD_INC_USE_COUNT	do { } while (0)
#define MOD_DEC_USE_COUNT	do { } while (0)
#define MOD_IN_USE		1

extern struct module *module_list;

#endif /* !__GENKSYMS__ */

#endif /* MODULE */

/* Export a symbol either from the kernel or a module.

   In the kernel, the symbol is added to the kernel's global symbol table.

   In a module, it controls which variables are exported.  If no
   variables are explicitly exported, the action is controled by the
   insmod -[xX] flags.  Otherwise, only the variables listed are exported.
   This obviates the need for the old register_symtab() function.  */

#if defined(__GENKSYMS__)

/* We want the EXPORT_SYMBOL tag left intact for recognition.  */

#elif !defined(AUTOCONF_INCLUDED)

#define __EXPORT_SYMBOL(sym,str)   error config_must_be_included_before_module
#define EXPORT_SYMBOL(var)	   error config_must_be_included_before_module
#define EXPORT_SYMBOL_NOVERS(var)  error config_must_be_included_before_module

#elif !defined(CONFIG_MODULES)

#define __EXPORT_SYMBOL(sym,str)
#define EXPORT_SYMBOL(var)
#define EXPORT_SYMBOL_NOVERS(var)

#else

#define __EXPORT_SYMBOL(sym, str)			\
const char __kstrtab_##sym[]				\
__attribute__((section(".kstrtab"))) = str;		\
const struct module_symbol __ksymtab_##sym 		\
__attribute__((section("__ksymtab"))) =			\
{ (unsigned long)&sym, __kstrtab_##sym }

//符号是否支持版本变动
#if defined(MODVERSIONS) || !defined(CONFIG_MODVERSIONS)
#define EXPORT_SYMBOL(var)  __EXPORT_SYMBOL(var, __MODULE_STRING(var))
#else
#define EXPORT_SYMBOL(var)  __EXPORT_SYMBOL(var, __MODULE_STRING(__VERSIONED_SYMBOL(var)))
#endif

#define EXPORT_SYMBOL_NOVERS(var)  __EXPORT_SYMBOL(var, __MODULE_STRING(var))

#endif /* __GENKSYMS__ */

#ifdef MODULE
/* Force a module to export no symbols.  */
#define EXPORT_NO_SYMBOLS  __asm__(".section __ksymtab\n.previous")
#else
#define EXPORT_NO_SYMBOLS
#endif /* MODULE */

#ifdef CONFIG_MODULES
#define SET_MODULE_OWNER(some_struct) do { (some_struct)->owner = THIS_MODULE; } while (0)
#else
#define SET_MODULE_OWNER(some_struct) do { } while (0)
#endif

#endif /* _LINUX_MODULE_H */
