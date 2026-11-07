/*
 *  linux/fs/devices.c
 *
 * (C) 1993 Matthias Urlichs -- collected common code and tables.
 * 
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  Added kerneld support: Jacques Gelinas and Bjorn Ekwall
 *  (changed to kmod)
 */

#include <linux/config.h>
#include <linux/fs.h>
#include <linux/major.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/stat.h>
#include <linux/fcntl.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/smp_lock.h>
#ifdef CONFIG_KMOD
#include <linux/kmod.h>

#include <linux/tty.h>

/* serial module kmod load support */
struct tty_driver *get_tty_driver(kdev_t device);
#define isa_tty_dev(ma)	(ma == TTY_MAJOR || ma == TTYAUX_MAJOR)
#define need_serial(ma,mi) (get_tty_driver(MKDEV(ma,mi)) == NULL)
#endif

/**
 * struct device_struct - Linux 2.4 字符设备全局数组元素类型
 * @name: 驱动名字，用于 /proc/devices 展示
 * @fops: 文件操作方法表 file_operations
 *
 * 这个就是 register_chrdev 操作的数组项，数组名叫 chrdevs[]
 */
struct device_struct {
    const char * name;
    struct file_operations * fops;
};


//字符设备的锁
static rwlock_t chrdevs_lock = RW_LOCK_UNLOCKED;

//设备数组
static struct device_struct chrdevs[MAX_CHRDEV];

extern int get_blkdev_list(char *);

int get_device_list(char * page)
{
	int i;
	int len;

	len = sprintf(page, "Character devices:\n");
	read_lock(&chrdevs_lock);
	for (i = 0; i < MAX_CHRDEV ; i++) {
		if (chrdevs[i].fops) {
			len += sprintf(page+len, "%3d %s\n", i, chrdevs[i].name);
		}
	}
	read_unlock(&chrdevs_lock);
	len += get_blkdev_list(page+len);
	return len;
}

/*
	Return the function table of a device.
	Load the driver if needed.
	Increment the reference count of module in question.
*/
/**
 * get_chrfops - 根据主设备号获取字符设备对应的file_operations指针，支持自动加载模块(kmod)
 * @major: 主设备号
 * @minor: 次设备号，本函数常规查找逻辑不使用，仅isa_tty_dev分支做辅助判断
 * 返回值：成功返回fops指针（引用计数已递增）；失败返回NULL
 */
static struct file_operations * get_chrfops(unsigned int major, unsigned int minor)
{
    struct file_operations *ret = NULL;

    // 主设备号为0或者超出最大字符设备范围，直接返回空
    if (!major || major >= MAX_CHRDEV)
        return NULL;

    // 加读锁保护全局chrdevs数组
    read_lock(&chrdevs_lock);
    // 获取fops并且增加fops的引用计数，防止在使用过程中被释放注销
    ret = fops_get(chrdevs[major].fops);
    read_unlock(&chrdevs_lock);

#ifdef CONFIG_KMOD
    // 如果拿到fops，并且是isa tty类设备，额外做serial模块检查
    if (ret && isa_tty_dev(major)) {
        lock_kernel();
        // 判断是否需要加载serial驱动模块
        if (need_serial(major,minor)) {
            // 放弃当前拿到的fops，置空触发后面的request_module
            fops_put(ret);
            ret = NULL;
        }
        unlock_kernel();
    }

    // 没有拿到有效的fops，尝试自动加载对应主设备号的模块
    if (!ret) {
        char name[20];
        // 拼接模块名字：char-major-xx
        sprintf(name, "char-major-%d", major);
        // 内核kmod机制，调用用户态modprobe加载模块
        request_module(name);

        // 模块加载完成后，再次读锁查询chrdevs数组
        read_lock(&chrdevs_lock);
        ret = fops_get(chrdevs[major].fops);
        read_unlock(&chrdevs_lock);
    }
#endif
    return ret;
}

/**
 * register_chrdev - 2.4内核老式字符设备注册函数（fs/char_dev.c）
 * @major: 想要注册的主设备号；填0表示让内核自动分配空闲主设备号
 * @name: 驱动名称，用于/proc/devices展示
 * @fops: file_operations 操作函数表，VFS打开设备时回调
 * 返回值：成功：固定major号(指定major) / 分配到的major(传0)；失败：负错误码
 */
int register_chrdev(unsigned int major, const char * name, struct file_operations *fops)
{
    // 分支1：major == 0，动态分配主设备号
    if (major == 0) {
        write_lock(&chrdevs_lock);          // 写锁保护全局chrdevs数组
        // 从最大主设备号向前遍历找空位 MAX_CHRDEV一般=256
        for (major = MAX_CHRDEV-1; major > 0; major--) {
            if (chrdevs[major].fops == NULL) {  // 找到未被占用的主设备号
                chrdevs[major].name = name;
                chrdevs[major].fops = fops;
                write_unlock(&chrdevs_lock);
                return major;                   // 返回分配得到的主设备号
            }
        }
        write_unlock(&chrdevs_lock);
        return -EBUSY;  // 全部255个主设备号都占满，返回忙
    }

    // 分支2：用户指定主设备号，先校验合法性
    if (major >= MAX_CHRDEV)
        return -EINVAL; // 主设备号超出最大范围，非法参数

    write_lock(&chrdevs_lock);
    // 判断：该主设备号已经注册，并且注册的fops和本次不一样 → 冲突
    if (chrdevs[major].fops && chrdevs[major].fops != fops) {
        write_unlock(&chrdevs_lock);
        return -EBUSY;
    }
    // 两种可通过场景：
    // 1. chrdevs[major].fops == NULL：未注册，正常填充
    // 2. chrdevs[major].fops == fops：重复注册同一个fops，允许（幂等）
    chrdevs[major].name = name;
    chrdevs[major].fops = fops;
    write_unlock(&chrdevs_lock);
    return 0;
}


int unregister_chrdev(unsigned int major, const char * name)
{
	if (major >= MAX_CHRDEV)
		return -EINVAL;
	write_lock(&chrdevs_lock);
	if (!chrdevs[major].fops || strcmp(chrdevs[major].name, name)) {
		write_unlock(&chrdevs_lock);
		return -EINVAL;
	}
	chrdevs[major].name = NULL;
	chrdevs[major].fops = NULL;
	write_unlock(&chrdevs_lock);
	return 0;
}

/*
 * Called every time a character special file is opened
 */
/**
 * chrdev_open - 字符设备inode默认打开回调（2.4 fs/char_dev.c）
 * @inode: 设备文件对应的inode，i_rdev存放设备号（主+次）
 * @filp: 本次打开生成的file结构体
 * 返回值：0成功；负数错误码
 */
int chrdev_open(struct inode * inode, struct file * filp)
{
    int ret = -ENODEV;

    // 根据主设备号，从全局chrdevs数组取出注册好的file_operations
    filp->f_op = get_chrfops(MAJOR(inode->i_rdev), MINOR(inode->i_rdev));
    if (filp->f_op) {          // 找到了该主设备号对应的fops
        ret = 0;
        if (filp->f_op->open != NULL) { // 驱动提供了open回调
            lock_kernel();     // 2.4大内核锁BKL，保护驱动open里的逻辑
            ret = filp->f_op->open(inode,filp);
            unlock_kernel();
        }
    }
    return ret;
}


/*
 * Dummy default file-operations: the only thing this does
 * is contain the open that then fills in the correct operations
 * depending on the special file...
 */
// 字符设备打开的回调函数
static struct file_operations def_chr_fops = {
	open:		chrdev_open,
};

/*
 * Print device name (in decimal, hexadecimal or symbolic)
 * Note: returns pointer to static data!
 */
const char * kdevname(kdev_t dev)
{
	static char buffer[32];
	sprintf(buffer, "%02x:%02x", MAJOR(dev), MINOR(dev));
	return buffer;
}

const char * cdevname(kdev_t dev)
{
	static char buffer[32];
	const char * name = chrdevs[MAJOR(dev)].name;

	if (!name)
		name = "unknown-char";
	sprintf(buffer, "%s(%d,%d)", name, MAJOR(dev), MINOR(dev));
	return buffer;
}
  
static int sock_no_open(struct inode *irrelevant, struct file *dontcare)
{
	return -ENXIO;
}

static struct file_operations bad_sock_fops = {
	open:		sock_no_open
};

/**
 * init_special_inode - 初始化特殊文件inode（字符设备、块设备、FIFO、socket）(Linux2.4)
 * @inode: 待初始化的inode结构体
 * @mode: 文件类型+权限
 * @rdev: 设备号，FIFO、socket该参数无意义
 */
void init_special_inode(struct inode *inode, umode_t mode, int rdev)
{
    // 设置inode的模式（文件类型、访问权限）
    inode->i_mode = mode;
    if (S_ISCHR(mode)) {
        // 字符设备：绑定字符设备默认文件操作集
        inode->i_fop = &def_chr_fops;
        // 保存设备号，转换成内核kdev_t格式
        inode->i_rdev = to_kdev_t(rdev);
    } else if (S_ISBLK(mode)) {
        // 块设备：绑定块设备默认文件操作集
        inode->i_fop = &def_blk_fops;
        inode->i_rdev = to_kdev_t(rdev);
        // 根据设备号拿到块设备描述符bdev
        inode->i_bdev = bdget(rdev);
    } else if (S_ISFIFO(mode))
        // FIFO有名管道：绑定fifo默认文件操作集 def_fifo_fops
        inode->i_fop = &def_fifo_fops;
    else if (S_ISSOCK(mode))
        // socket文件，默认bad_sock_fops，后续socket层会替换
        inode->i_fop = &bad_sock_fops;
    else
        // 不合法的特殊文件类型，打印调试日志
        printk(KERN_DEBUG "init_special_inode: bogus imode (%o)\n", mode);
}
