/*
 * This is the 1999 rewrite of IP Firewalling, aiming for kernel 2.3.x.
 *
 * Copyright (C) 1999 Paul `Rusty' Russell & Michael J. Neuling
 */
#include <linux/module.h>
#include <linux/netfilter_ipv4/ip_tables.h>

#define FILTER_VALID_HOOKS ((1 << NF_IP_LOCAL_IN) | (1 << NF_IP_FORWARD) | (1 << NF_IP_LOCAL_OUT))

/* Standard entry. */
struct ipt_standard
{
	struct ipt_entry entry;
	struct ipt_standard_target target;
};

struct ipt_error_target
{
	struct ipt_entry_target target;
	char errorname[IPT_FUNCTION_MAXNAMELEN];
};

struct ipt_error
{
	struct ipt_entry entry;
	struct ipt_error_target target;
};

static struct
{
	struct ipt_replace repl;
	struct ipt_standard entries[3];
	struct ipt_error term;
} initial_table __initdata
= { { "filter", FILTER_VALID_HOOKS, 4,
      sizeof(struct ipt_standard) * 3 + sizeof(struct ipt_error),
      { [NF_IP_LOCAL_IN] 0,
	[NF_IP_FORWARD] sizeof(struct ipt_standard),
	[NF_IP_LOCAL_OUT] sizeof(struct ipt_standard) * 2 },
      { [NF_IP_LOCAL_IN] 0,
	[NF_IP_FORWARD] sizeof(struct ipt_standard),
	[NF_IP_LOCAL_OUT] sizeof(struct ipt_standard) * 2 },
      0, NULL, { } },
    {
	    /* LOCAL_IN */
	    { { { { 0 }, { 0 }, { 0 }, { 0 }, "", "", { 0 }, { 0 }, 0, 0, 0 },
		0,
		sizeof(struct ipt_entry),
		sizeof(struct ipt_standard),
		0, { 0, 0 }, { } },
	      { { { { IPT_ALIGN(sizeof(struct ipt_standard_target)), "" } }, { } },
		-NF_ACCEPT - 1 } },
	    /* FORWARD */
	    { { { { 0 }, { 0 }, { 0 }, { 0 }, "", "", { 0 }, { 0 }, 0, 0, 0 },
		0,
		sizeof(struct ipt_entry),
		sizeof(struct ipt_standard),
		0, { 0, 0 }, { } },
	      { { { { IPT_ALIGN(sizeof(struct ipt_standard_target)), "" } }, { } },
		-NF_ACCEPT - 1 } },
	    /* LOCAL_OUT */
	    { { { { 0 }, { 0 }, { 0 }, { 0 }, "", "", { 0 }, { 0 }, 0, 0, 0 },
		0,
		sizeof(struct ipt_entry),
		sizeof(struct ipt_standard),
		0, { 0, 0 }, { } },
	      { { { { IPT_ALIGN(sizeof(struct ipt_standard_target)), "" } }, { } },
		-NF_ACCEPT - 1 } }
    },
    /* ERROR */
    { { { { 0 }, { 0 }, { 0 }, { 0 }, "", "", { 0 }, { 0 }, 0, 0, 0 },
	0,
	sizeof(struct ipt_entry),
	sizeof(struct ipt_error),
	0, { 0, 0 }, { } },
      { { { { IPT_ALIGN(sizeof(struct ipt_error_target)), IPT_ERROR_TARGET } },
	  { } },
	"ERROR"
      }
    }
};

static struct ipt_table packet_filter
= { { NULL, NULL }, "filter", &initial_table.repl,
    FILTER_VALID_HOOKS, RW_LOCK_UNLOCKED, NULL };

/* The work comes in here from netfilter.c. */
static unsigned int
ipt_hook(unsigned int hook,
	 struct sk_buff **pskb,
	 const struct net_device *in,
	 const struct net_device *out,
	 int (*okfn)(struct sk_buff *))
{
	return ipt_do_table(pskb, hook, in, out, &packet_filter, NULL);
}

static unsigned int
ipt_local_out_hook(unsigned int hook,
		   struct sk_buff **pskb,
		   const struct net_device *in,
		   const struct net_device *out,
		   int (*okfn)(struct sk_buff *))
{
	/* root is playing with raw sockets. */
	if ((*pskb)->len < sizeof(struct iphdr)
	    || (*pskb)->nh.iph->ihl * 4 < sizeof(struct iphdr)) {
		if (net_ratelimit())
			printk("ipt_hook: happy cracking.\n");
		return NF_ACCEPT;
	}

	return ipt_do_table(pskb, hook, in, out, &packet_filter, NULL);
}

static struct nf_hook_ops ipt_ops[]
= { { { NULL, NULL }, ipt_hook, PF_INET, NF_IP_LOCAL_IN, NF_IP_PRI_FILTER },
    { { NULL, NULL }, ipt_hook, PF_INET, NF_IP_FORWARD, NF_IP_PRI_FILTER },
    { { NULL, NULL }, ipt_local_out_hook, PF_INET, NF_IP_LOCAL_OUT,
		NF_IP_PRI_FILTER }
};

/* Default to forward because I got too much mail already. */
static int forward = NF_ACCEPT;
MODULE_PARM(forward, "i");

/**
 * init - iptables filter表模块初始化函数（__init，内核启动initcall调用）
 *
 * 返回值：成功返回0；失败返回负错误码，自动回滚已注册资源
 * 说明：filter表是iptables默认过滤表；设置forward默认策略，注册filter表，然后注册INPUT/FORWARD/OUTPUT三个netfilter钩子
 */
static int __init init(void)
{
    int ret;

    // 校验forward内核参数，只允许0或1
    if (forward < 0 || forward > NF_MAX_VERDICT) {
        printk("iptables forward must be 0 or 1\n");
        return -EINVAL;
    }

    /* Entry 1 is the FORWARD hook */
    // 下标1对应FORWARD规则项，设置默认判决
    // verdict = -forward -1：netfilter约定负数代表内置判决，-1=ACCEPT，-2=DROP
    initial_table.entries[1].target.verdict = -forward - 1;

    /* Register table */
    // 向iptables核心注册filter表（packet_filter就是filter表描述符）
    ret = ipt_register_table(&packet_filter);
    if (ret < 0)
        return ret;

    /* Register hooks */
    // 注册第0个钩子：INPUT钩子
    ret = nf_register_hook(&ipt_ops[0]);
    if (ret < 0)
        goto cleanup_table;

    // 注册第1个钩子：FORWARD钩子
    ret = nf_register_hook(&ipt_ops[1]);
    if (ret < 0)
        goto cleanup_hook0;

    // 注册第2个钩子：OUTPUT钩子
    ret = nf_register_hook(&ipt_ops[2]);
    if (ret < 0)
        goto cleanup_hook1;

    return ret;

// 出错回滚：逆序卸载钩子、卸载表，防止资源泄漏
 cleanup_hook1:
    nf_unregister_hook(&ipt_ops[1]);
 cleanup_hook0:
    nf_unregister_hook(&ipt_ops[0]);
 cleanup_table:
    ipt_unregister_table(&packet_filter);

    return ret;
}

static void __exit fini(void)
{
	unsigned int i;

	for (i = 0; i < sizeof(ipt_ops)/sizeof(struct nf_hook_ops); i++)
		nf_unregister_hook(&ipt_ops[i]);

	ipt_unregister_table(&packet_filter);
}

module_init(init);
module_exit(fini);
