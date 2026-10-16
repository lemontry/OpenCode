#include <linux/config.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/init.h>

extern struct proc_dir_entry *proc_sys_root;

#ifdef CONFIG_SYSCTL
EXPORT_SYMBOL(proc_sys_root);
#endif
EXPORT_SYMBOL(proc_symlink);
EXPORT_SYMBOL(proc_mknod);
EXPORT_SYMBOL(proc_mkdir);
EXPORT_SYMBOL(create_proc_entry);
EXPORT_SYMBOL(remove_proc_entry);
EXPORT_SYMBOL(proc_root);
EXPORT_SYMBOL(proc_root_fs);
EXPORT_SYMBOL(proc_net);
EXPORT_SYMBOL(proc_bus);
EXPORT_SYMBOL(proc_root_driver);

static DECLARE_FSTYPE(proc_fs_type, "proc", proc_read_super, FS_SINGLE);

/**
 * __init init_proc_fs - proc文件系统模块初始化入口（2.4内核）
 * 返回值：0成功；负数错误码失败
 * __init：标记函数，内核启动完成后该函数所占内存可以被释放
 */
static int __init init_proc_fs(void)
{
	// 1. 向内核全局file_systems链表注册proc文件系统类型
	int err = register_filesystem(&proc_fs_type);
	if (!err) {
		// 2. kern_mount：内核内部挂载，不是用户态mount系统调用；
        //    直接创建proc的super_block + vfsmount，返回vfsmount指针
		proc_mnt = kern_mount(&proc_fs_type);

		// 取出错误码（如果是错误指针）
		err = PTR_ERR(proc_mnt);
		if (IS_ERR(proc_mnt))
			// 挂载失败：回滚，把刚才注册的文件系统类型注销
			unregister_filesystem(&proc_fs_type);
		else
			err = 0;
	}
	return err;
}

static void __exit exit_proc_fs(void)
{
	unregister_filesystem(&proc_fs_type);
	kern_umount(proc_mnt);
}

module_init(init_proc_fs)
module_exit(exit_proc_fs)
