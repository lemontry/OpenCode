#ifndef _LINUX_TTY_DRIVER_H
#define _LINUX_TTY_DRIVER_H

/*
 * This structure defines the interface between the low-level tty
 * driver and the tty routines.  The following routines can be
 * defined; unless noted otherwise, they are optional, and can be
 * filled in with a null pointer.
 *
 * int  (*open)(struct tty_struct * tty, struct file * filp);
 *
 * 	This routine is called when a particular tty device is opened.
 * 	This routine is mandatory; if this routine is not filled in,
 * 	the attempted open will fail with ENODEV.
 *     
 * void (*close)(struct tty_struct * tty, struct file * filp);
 *
 * 	This routine is called when a particular tty device is closed.
 *
 * int (*write)(struct tty_struct * tty, int from_user,
 * 		 const unsigned char *buf, int count);
 *
 * 	This routine is called by the kernel to write a series of
 * 	characters to the tty device.  The characters may come from
 * 	user space or kernel space.  This routine will return the
 *	number of characters actually accepted for writing.  This
 *	routine is mandatory.
 *
 * void (*put_char)(struct tty_struct *tty, unsigned char ch);
 *
 * 	This routine is called by the kernel to write a single
 * 	character to the tty device.  If the kernel uses this routine,
 * 	it must call the flush_chars() routine (if defined) when it is
 * 	done stuffing characters into the driver.  If there is no room
 * 	in the queue, the character is ignored.
 *
 * void (*flush_chars)(struct tty_struct *tty);
 *
 * 	This routine is called by the kernel after it has written a
 * 	series of characters to the tty device using put_char().  
 * 
 * int  (*write_room)(struct tty_struct *tty);
 *
 * 	This routine returns the numbers of characters the tty driver
 * 	will accept for queuing to be written.  This number is subject
 * 	to change as output buffers get emptied, or if the output flow
 *	control is acted.
 * 
 * int  (*ioctl)(struct tty_struct *tty, struct file * file,
 * 	    unsigned int cmd, unsigned long arg);
 *
 * 	This routine allows the tty driver to implement
 *	device-specific ioctl's.  If the ioctl number passed in cmd
 * 	is not recognized by the driver, it should return ENOIOCTLCMD.
 * 
 * void (*set_termios)(struct tty_struct *tty, struct termios * old);
 *
 * 	This routine allows the tty driver to be notified when
 * 	device's termios settings have changed.  Note that a
 * 	well-designed tty driver should be prepared to accept the case
 * 	where old == NULL, and try to do something rational.
 *
 * void (*set_ldisc)(struct tty_struct *tty);
 *
 * 	This routine allows the tty driver to be notified when the
 * 	device's termios settings have changed.
 * 
 * void (*throttle)(struct tty_struct * tty);
 *
 * 	This routine notifies the tty driver that input buffers for
 * 	the line discipline are close to full, and it should somehow
 * 	signal that no more characters should be sent to the tty.
 * 
 * void (*unthrottle)(struct tty_struct * tty);
 *
 * 	This routine notifies the tty drivers that it should signals
 * 	that characters can now be sent to the tty without fear of
 * 	overrunning the input buffers of the line disciplines.
 * 
 * void (*stop)(struct tty_struct *tty);
 *
 * 	This routine notifies the tty driver that it should stop
 * 	outputting characters to the tty device.  
 * 
 * void (*start)(struct tty_struct *tty);
 *
 * 	This routine notifies the tty driver that it resume sending
 *	characters to the tty device.
 * 
 * void (*hangup)(struct tty_struct *tty);
 *
 * 	This routine notifies the tty driver that it should hangup the
 * 	tty device.
 *
 * void (*break_ctl)(struct tty_stuct *tty, int state);
 *
 * 	This optional routine requests the tty driver to turn on or
 * 	off BREAK status on the RS-232 port.  If state is -1,
 * 	then the BREAK status should be turned on; if state is 0, then
 * 	BREAK should be turned off.
 *
 * 	If this routine is implemented, the high-level tty driver will
 * 	handle the following ioctls: TCSBRK, TCSBRKP, TIOCSBRK,
 * 	TIOCCBRK.  Otherwise, these ioctls will be passed down to the
 * 	driver to handle.
 *
 * void (*wait_until_sent)(struct tty_struct *tty, int timeout);
 * 
 * 	This routine waits until the device has written out all of the
 * 	characters in its transmitter FIFO.
 *
 * void (*send_xchar)(struct tty_struct *tty, char ch);
 *
 * 	This routine is used to send a high-priority XON/XOFF
 * 	character to the device.
 */

#include <linux/fs.h>

/**
 * struct tty_driver - Linux2.4 TTY驱动描述结构体
 * 功能：抽象一类TTY设备驱动，存放驱动元数据、设备数组、底层回调；所有tty_driver挂载到全局双向链表统一管理
 */
struct tty_driver {
    // 魔数，用于结构体合法性校验
    int magic;     /* magic number for this structure */
    // 驱动底层名称，用于内核日志、proc展示
    const char  *driver_name;
    // TTY设备名称前缀，例如 "ttyS"、"pty"
    const char  *name;
    // 设备名字编号偏移基数
    int name_base;  /* offset of printed name */
    // 该驱动对应的主设备号
    short   major;      /* major device number */
    // 次设备号起始编号
    short   minor_start;    /* start of minor device number*/
    // 该驱动支持的TTY设备实例总数量
    short   num;        /* number of devices */
    // TTY驱动类型，串口/pty/控制台等
    short   type;       /* type of tty driver */
    // TTY驱动子类型，对type进一步细分
    short   subtype;    /* subtype of tty driver */
    // TTY初始终端属性配置
    struct termios init_termios; /* Initial termios */
    // TTY驱动标志位，控制TTY层行为
    int flags;      /* tty driver flags */
    // 引用计数，供可加载模块类型的TTY驱动使用
    int *refcount;  /* for loadable tty drivers */
    // proc文件节点指针，用于导出proc接口
    struct proc_dir_entry *proc_entry; /* /proc fs entry */
    // 仅PTY驱动使用，指向配对的另一端tty_driver
    struct tty_driver *other; /* only used for the PTY driver */

    /*
     * Pointer to the tty data structures
     */
    // tty_struct实例指针数组，下标对应次设备号
    struct tty_struct **table;
    // termios属性数组，保存每个tty的常规终端配置
    struct termios **termios;
    // termios属性数组，锁定场景下使用的终端配置
    struct termios **termios_locked;
    // 驱动私有状态指针，仅PTY驱动使用
    void *driver_state; /* only used for the PTY driver */
    
    /*
     * Interface routines from the upper tty layer to the tty
     * driver.
     */
    // 底层驱动open回调，打开单个tty实例
    int  (*open)(struct tty_struct * tty, struct file * filp);
    // 底层驱动close回调，关闭tty实例
    void (*close)(struct tty_struct * tty, struct file * filp);
    // 底层驱动write回调，下发数据到设备
    int  (*write)(struct tty_struct * tty, int from_user,
          const unsigned char *buf, int count);
    // 底层驱动单字符发送回调
    void (*put_char)(struct tty_struct *tty, unsigned char ch);
    // 冲刷待发送字符
    void (*flush_chars)(struct tty_struct *tty);
    // 查询发送缓冲区剩余可写空间
    int  (*write_room)(struct tty_struct *tty);
    // 查询缓冲区中尚未发送的字符数量
    int  (*chars_in_buffer)(struct tty_struct *tty);
    // 底层驱动ioctl回调，处理终端控制命令
    int  (*ioctl)(struct tty_struct *tty, struct file * file,
          unsigned int cmd, unsigned long arg);
    // 修改termios终端属性时的驱动回调
    void (*set_termios)(struct tty_struct *tty, struct termios * old);
    // 输入流控：暂停接收，通知对端停止发送
    void (*throttle)(struct tty_struct * tty);
    // 输入流控：恢复接收，通知对端继续发送
    void (*unthrottle)(struct tty_struct * tty);
    // 停止输出发送
    void (*stop)(struct tty_struct *tty);
    // 恢复输出发送
    void (*start)(struct tty_struct *tty);
    // 线路挂断回调
    void (*hangup)(struct tty_struct *tty);
    // BREAK信号控制回调
    void (*break_ctl)(struct tty_struct *tty, int state);
    // 清空收发缓冲区
    void (*flush_buffer)(struct tty_struct *tty);
    // 线路规程切换回调
    void (*set_ldisc)(struct tty_struct *tty);
    // 阻塞等待所有数据发送完毕
    void (*wait_until_sent)(struct tty_struct *tty, int timeout);
    // 发送特殊控制字符（XON/XOFF）
    void (*send_xchar)(struct tty_struct *tty, char ch);
    // proc读回调函数
    int (*read_proc)(char *page, char **start, off_t off,
          int count, int *eof, void *data);
    // proc写回调函数
    int (*write_proc)(struct file *file, const char *buffer,
          unsigned long count, void *data);

    /*
     * linked list pointers
     */
    // 全局tty_driver双向链表后继节点
    struct tty_driver *next;
    // 全局tty_driver双向链表前驱节点
    struct tty_driver *prev;
};

/* tty driver magic number */
#define TTY_DRIVER_MAGIC		0x5402

/*
 * tty driver flags
 * 
 * TTY_DRIVER_RESET_TERMIOS --- requests the tty layer to reset the
 * 	termios setting when the last process has closed the device.
 * 	Used for PTY's, in particular.
 * 
 * TTY_DRIVER_REAL_RAW --- if set, indicates that the driver will
 * 	guarantee never not to set any special character handling
 * 	flags if ((IGNBRK || (!BRKINT && !PARMRK)) && (IGNPAR ||
 * 	!INPCK)).  That is, if there is no reason for the driver to
 * 	send notifications of parity and break characters up to the
 * 	line driver, it won't do so.  This allows the line driver to
 *	optimize for this case if this flag is set.  (Note that there
 * 	is also a promise, if the above case is true, not to signal
 * 	overruns, either.)
 *
 * TTY_DRIVER_NO_DEVFS --- if set, do not create devfs entries. This
 *	is only used by tty_register_driver().
 *
 */
#define TTY_DRIVER_INSTALLED		0x0001
#define TTY_DRIVER_RESET_TERMIOS	0x0002
#define TTY_DRIVER_REAL_RAW		0x0004
#define TTY_DRIVER_NO_DEVFS		0x0008

/* tty driver types */
#define TTY_DRIVER_TYPE_SYSTEM		0x0001
#define TTY_DRIVER_TYPE_CONSOLE		0x0002
#define TTY_DRIVER_TYPE_SERIAL		0x0003
#define TTY_DRIVER_TYPE_PTY		0x0004
#define TTY_DRIVER_TYPE_SCC		0x0005	/* scc driver */
#define TTY_DRIVER_TYPE_SYSCONS		0x0006

/* system subtypes (magic, used by tty_io.c) */
#define SYSTEM_TYPE_TTY			0x0001
#define SYSTEM_TYPE_CONSOLE		0x0002
#define SYSTEM_TYPE_SYSCONS		0x0003
#define SYSTEM_TYPE_SYSPTMX		0x0004

/* pty subtypes (magic, used by tty_io.c) */
#define PTY_TYPE_MASTER			0x0001
#define PTY_TYPE_SLAVE			0x0002

/* serial subtype definitions */
#define SERIAL_TYPE_NORMAL	1
#define SERIAL_TYPE_CALLOUT	2

#endif /* #ifdef _LINUX_TTY_DRIVER_H */
