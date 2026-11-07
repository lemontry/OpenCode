/*
 * NET		An implementation of the SOCKET network access protocol.
 *		This is the master header file for the Linux NET layer,
 *		or, in plain English: the networking handling part of the
 *		kernel.
 *
 * Version:	@(#)net.h	1.0.3	05/25/93
 *
 * Authors:	Orest Zborowski, <obz@Kodak.COM>
 *		Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#ifndef _LINUX_NET_H
#define _LINUX_NET_H

#include <linux/config.h>
#include <linux/socket.h>
#include <linux/wait.h>

struct poll_table_struct;

#define NPROTO		32		/* should be enough for now..	*/


#define SYS_SOCKET	1		/* sys_socket(2)		*/
#define SYS_BIND	2		/* sys_bind(2)			*/
#define SYS_CONNECT	3		/* sys_connect(2)		*/
#define SYS_LISTEN	4		/* sys_listen(2)		*/
#define SYS_ACCEPT	5		/* sys_accept(2)		*/
#define SYS_GETSOCKNAME	6		/* sys_getsockname(2)		*/
#define SYS_GETPEERNAME	7		/* sys_getpeername(2)		*/
#define SYS_SOCKETPAIR	8		/* sys_socketpair(2)		*/
#define SYS_SEND	9		/* sys_send(2)			*/
#define SYS_RECV	10		/* sys_recv(2)			*/
#define SYS_SENDTO	11		/* sys_sendto(2)		*/
#define SYS_RECVFROM	12		/* sys_recvfrom(2)		*/
#define SYS_SHUTDOWN	13		/* sys_shutdown(2)		*/
#define SYS_SETSOCKOPT	14		/* sys_setsockopt(2)		*/
#define SYS_GETSOCKOPT	15		/* sys_getsockopt(2)		*/
#define SYS_SENDMSG	16		/* sys_sendmsg(2)		*/
#define SYS_RECVMSG	17		/* sys_recvmsg(2)		*/


typedef enum {
  SS_FREE = 0,				/* not allocated		*/
  SS_UNCONNECTED,			/* unconnected to any socket	*/
  SS_CONNECTING,			/* in process of connecting	*/
  SS_CONNECTED,				/* connected to socket		*/
  SS_DISCONNECTING			/* in process of disconnecting	*/
} socket_state;

#define __SO_ACCEPTCON	(1<<16)		/* performed a listen		*/

#ifdef __KERNEL__

#define SOCK_ASYNC_NOSPACE	0
#define SOCK_ASYNC_WAITDATA	1
#define SOCK_NOSPACE		2

/**
 * struct socket - BSD套接字层描述符（Linux2.4）
 * 属于VFS上层的socket抽象，对应用户态操作的socket文件；
 * 和下层传输控制块 struct sock(sk) 一一绑定
 */
struct socket
{
    // socket连接状态：SS_UNCONNECTED / SS_CONNECTED / SS_CONNECTING / SS_DISCONNECTING
    socket_state        state;

    // socket标志位
    unsigned long       flags;
    // 套接字操作函数集：bind/connect/accept/recvmsg/sendmsg等（inet_stream_ops、inet_dgram_ops）
    struct proto_ops    *ops;
    // 关联的VFS匿名inode，socket靠inode接入VFS文件系统模型
    struct inode        *inode;
    // 异步通知链表，fcntl(FASYNC)使用，信号异步唤醒进程
    struct fasync_struct    *fasync_list;   /* Asynchronous wake up list    */
    // 反向指向绑定的file结构体，sock_map_fd之后赋值，用于垃圾回收
    struct file     *file;      /* File back pointer for gc */
    // 底层传输层控制块，TCP/UDP/RAW的真正内核控制结构
    struct sock     *sk;
    // 等待队列头，阻塞读、阻塞写、阻塞accept时进程在此睡眠等待
    wait_queue_head_t   wait;

    // 套接字类型：SOCK_STREAM / SOCK_DGRAM / SOCK_RAW
    short               type;
    // 是否开启传递凭证（passcred），控制辅助消息携带pid/uid/gid
    unsigned char       passcred;
};


#define SOCK_INODE(S)	((S)->inode)

struct scm_cookie;
struct vm_area_struct;

/**
 * struct proto_ops - BSD socket层操作函数表（Linux2.4）
 * 对应用户态socket系统调用的回调集合，属于socket抽象层接口；
 * 不同协议/类型套接字拥有各自实例，如inet_stream_ops(TCP)、inet_dgram_ops(UDP)
 */
struct proto_ops {
    // 协议族：PF_INET / PF_UNIX 等
    int   family;

    // 关闭释放socket资源，close最终走到这里
    int   (*release)  (struct socket *sock);
    // bind系统调用：绑定本地地址端口
    int   (*bind)     (struct socket *sock, struct sockaddr *umyaddr,
                 int sockaddr_len);
    // connect系统调用：发起连接（TCP）或指定远端地址（UDP）
    int   (*connect)  (struct socket *sock, struct sockaddr *uservaddr,
                 int sockaddr_len, int flags);
    // socketpair系统调用：创建一对相互连通的套接字（本地管道式socket）
    int   (*socketpair)   (struct socket *sock1, struct socket *sock2);
    // accept系统调用：TCP接受新连接，产出newsock新套接字
    int   (*accept)   (struct socket *sock, struct socket *newsock,
                 int flags);
    // getsockname/getpeername：获取本地地址/对端地址，peer=1表示取对端
    int   (*getname)  (struct socket *sock, struct sockaddr *uaddr,
                 int *usockaddr_len, int peer);
    // poll/select轮询回调，检测可读、可写、异常事件
    unsigned int (*poll)  (struct file *file, struct socket *sock, struct poll_table_struct *wait);
    // ioctl对socket执行控制命令
    int   (*ioctl)    (struct socket *sock, unsigned int cmd,
                 unsigned long arg);
    // listen系统调用：TCP开启监听，设置backlog队列长度
    int   (*listen)   (struct socket *sock, int len);
    // shutdown系统调用：半关闭，关闭读方向/写方向
    int   (*shutdown) (struct socket *sock, int flags);
    // setsockopt系统调用：设置socket选项
    int   (*setsockopt)   (struct socket *sock, int level, int optname,
                 char *optval, int optlen);
    // getsockopt系统调用：读取socket选项
    int   (*getsockopt)   (struct socket *sock, int level, int optname,
                 char *optval, int *optlen);
    // sendmsg系统调用：通用发送消息接口，send/sendto底层都走它
    int   (*sendmsg)  (struct socket *sock, struct msghdr *m, int total_len, struct scm_cookie *scm);
    // recvmsg系统调用：通用接收消息接口，recv/recvfrom底层都走它
    int   (*recvmsg)  (struct socket *sock, struct msghdr *m, int total_len, int flags, struct scm_cookie *scm);
    // mmap：socket支持内存映射（少数套接字类型实现）
    int   (*mmap)     (struct file *file, struct socket *sock, struct vm_area_struct * vma);
};

struct net_proto_family 
{
	int	family;
	int	(*create)(struct socket *sock, int protocol);
	/* These are counters for the number of different methods of
	   each we support */
	short	authentication;
	short	encryption;
	short	encrypt_net;
};

struct net_proto 
{
	const char *name;		/* Protocol name */
	void (*init_func)(struct net_proto *);	/* Bootstrap */
};

extern int	sock_wake_async(struct socket *sk, int how, int band);
extern int	sock_register(struct net_proto_family *fam);
extern int	sock_unregister(int family);
extern struct socket *sock_alloc(void);
extern int	sock_create(int family, int type, int proto, struct socket **);
extern void	sock_release(struct socket *);
extern int   	sock_sendmsg(struct socket *, struct msghdr *m, int len);
extern int	sock_recvmsg(struct socket *, struct msghdr *m, int len, int flags);
extern int	sock_readv_writev(int type, struct inode * inode, struct file * file,
				  const struct iovec * iov, long count, long size);

extern int	net_ratelimit(void);
extern unsigned long net_random(void);
extern void net_srandom(unsigned long);

#ifndef CONFIG_SMP
#define SOCKOPS_WRAPPED(name) name
#define SOCKOPS_WRAP(name, fam)
#else

#define SOCKOPS_WRAPPED(name) __unlocked_##name

#define SOCKCALL_WRAP(name, call, parms, args)		\
static int __lock_##name##_##call  parms		\
{							\
	int ret;					\
	lock_kernel();					\
	ret = __unlocked_##name##_ops.call  args ;\
	unlock_kernel();				\
	return ret;					\
}

#define SOCKCALL_UWRAP(name, call, parms, args)		\
static unsigned int __lock_##name##_##call  parms	\
{							\
	int ret;					\
	lock_kernel();					\
	ret = __unlocked_##name##_ops.call  args ;\
	unlock_kernel();				\
	return ret;					\
}


#define SOCKOPS_WRAP(name, fam)					\
SOCKCALL_WRAP(name, release, (struct socket *sock), (sock))	\
SOCKCALL_WRAP(name, bind, (struct socket *sock, struct sockaddr *uaddr, int addr_len), \
	      (sock, uaddr, addr_len))				\
SOCKCALL_WRAP(name, connect, (struct socket *sock, struct sockaddr * uaddr, \
			      int addr_len, int flags), 	\
	      (sock, uaddr, addr_len, flags))			\
SOCKCALL_WRAP(name, socketpair, (struct socket *sock1, struct socket *sock2), \
	      (sock1, sock2))					\
SOCKCALL_WRAP(name, accept, (struct socket *sock, struct socket *newsock, \
			 int flags), (sock, newsock, flags)) \
SOCKCALL_WRAP(name, getname, (struct socket *sock, struct sockaddr *uaddr, \
			 int *addr_len, int peer), (sock, uaddr, addr_len, peer)) \
SOCKCALL_UWRAP(name, poll, (struct file *file, struct socket *sock, struct poll_table_struct *wait), \
	      (file, sock, wait)) \
SOCKCALL_WRAP(name, ioctl, (struct socket *sock, unsigned int cmd, \
			 unsigned long arg), (sock, cmd, arg)) \
SOCKCALL_WRAP(name, listen, (struct socket *sock, int len), (sock, len)) \
SOCKCALL_WRAP(name, shutdown, (struct socket *sock, int flags), (sock, flags)) \
SOCKCALL_WRAP(name, setsockopt, (struct socket *sock, int level, int optname, \
			 char *optval, int optlen), (sock, level, optname, optval, optlen)) \
SOCKCALL_WRAP(name, getsockopt, (struct socket *sock, int level, int optname, \
			 char *optval, int *optlen), (sock, level, optname, optval, optlen)) \
SOCKCALL_WRAP(name, sendmsg, (struct socket *sock, struct msghdr *m, int len, struct scm_cookie *scm), \
	      (sock, m, len, scm)) \
SOCKCALL_WRAP(name, recvmsg, (struct socket *sock, struct msghdr *m, int len, int flags, struct scm_cookie *scm), \
	      (sock, m, len, flags, scm)) \
SOCKCALL_WRAP(name, mmap, (struct file *file, struct socket *sock, struct vm_area_struct *vma), \
	      (file, sock, vma)) \
	      \
static struct proto_ops name##_ops = {			\
	family:		fam,				\
							\
	release:	__lock_##name##_release,	\
	bind:		__lock_##name##_bind,		\
	connect:	__lock_##name##_connect,	\
	socketpair:	__lock_##name##_socketpair,	\
	accept:		__lock_##name##_accept,		\
	getname:	__lock_##name##_getname,	\
	poll:		__lock_##name##_poll,		\
	ioctl:		__lock_##name##_ioctl,		\
	listen:		__lock_##name##_listen,		\
	shutdown:	__lock_##name##_shutdown,	\
	setsockopt:	__lock_##name##_setsockopt,	\
	getsockopt:	__lock_##name##_getsockopt,	\
	sendmsg:	__lock_##name##_sendmsg,	\
	recvmsg:	__lock_##name##_recvmsg,	\
	mmap:		__lock_##name##_mmap,		\
};
#endif


#endif /* __KERNEL__ */
#endif	/* _LINUX_NET_H */
