// SPDX-License-Identifier: GPL-2.0
/* Device compatibility. */
#define pr_fmt(fmt) "miwill_genl: " fmt

#include <linux/module.h>
#include <linux/netdevice.h>
#include <net/genetlink.h>

enum miwill_attr {
	MIWILL_ATTR_UNSPEC,
	MIWILL_ATTR_MIW,	/* string: the miwill device's name */
	MIWILL_ATTR_ENABLE,	/* u8 */
	MIWILL_ATTR_MODE,	/* string */
	MIWILL_ATTR_GW_ADDR,	/* binary: the gateway's MAC */
	MIWILL_ATTR_PORT_5G,	/* string */
	MIWILL_ATTR_PORT_24G,	/* string */
	MIWILL_ATTR_IP,		/* u32: sent with command 3 */
	MIWILL_ATTR_RESET,	/* u8: sent with command 3 */
	MIWILL_ATTR_APP,	/* minet's app blob */
	__MIWILL_ATTR_AFTER_LAST,
};
#define MIWILL_ATTR_MAX		(__MIWILL_ATTR_AFTER_LAST - 1)	/* 9, as stock */

enum miwill_cmd {
	MIWILL_CMD_UNSPEC,
	MIWILL_CMD_OPTIONS_SET	= 1,
	MIWILL_CMD_OPTIONS_GET	= 2,
	MIWILL_CMD_EVENT	= 3,	/* kernel -> user */
	MIWILL_CMD_MINET_SET	= 4,
};

static const struct nla_policy miwill_nl_policy[MIWILL_ATTR_MAX + 1] = {
	[MIWILL_ATTR_MIW]	= { .type = NLA_STRING },
	[MIWILL_ATTR_ENABLE]	= { .type = NLA_U8 },
	[MIWILL_ATTR_MODE]	= { .type = NLA_STRING },
	[MIWILL_ATTR_GW_ADDR]	= { .type = NLA_BINARY },
	[MIWILL_ATTR_PORT_5G]	= { .type = NLA_STRING },
	[MIWILL_ATTR_PORT_24G]	= { .type = NLA_STRING },
	[MIWILL_ATTR_IP]	= { .type = NLA_U32 },
	[MIWILL_ATTR_RESET]	= { .type = NLA_U8 },
	[MIWILL_ATTR_APP]	= { .type = NLA_UNSPEC },
};

static atomic_t miwill_set_calls = ATOMIC_INIT(0);
static atomic_t minet_set_calls = ATOMIC_INIT(0);

static int miwill_nl_cmd_options_set(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr **tb = info->attrs;
	char name[IFNAMSIZ];

	if (!tb[MIWILL_ATTR_MIW]) {
		pr_debug("%s miw: NULL\n", __func__);
		return -EPERM;
	}
	/*
	 * k618: NLA_STRING without a .len (stock's policy, kept so the same messages are accepted)
	 * is not checked for a terminator, so none of these is handed to a %s -- the name is copied
	 * with a length, the rest printed with the attribute's own length.
	 */
	if (nla_strscpy(name, tb[MIWILL_ATTR_MIW], sizeof(name)) < 0)
		return -EPERM;
	pr_debug("%s miw: %s\n", __func__, name);

	if (tb[MIWILL_ATTR_ENABLE])
		pr_debug("%s enable: %d\n", __func__, nla_get_u8(tb[MIWILL_ATTR_ENABLE]));
	if (tb[MIWILL_ATTR_MODE])
		pr_debug("%s mode: %.*s\n", __func__, nla_len(tb[MIWILL_ATTR_MODE]),
			 (char *)nla_data(tb[MIWILL_ATTR_MODE]));
	if (tb[MIWILL_ATTR_GW_ADDR] && nla_len(tb[MIWILL_ATTR_GW_ADDR]) >= ETH_ALEN)
		pr_debug("%s gw_addr: %pM\n", __func__, nla_data(tb[MIWILL_ATTR_GW_ADDR]));
	if (tb[MIWILL_ATTR_PORT_5G])
		pr_debug("%s port_5g: %.*s\n", __func__, nla_len(tb[MIWILL_ATTR_PORT_5G]),
			 (char *)nla_data(tb[MIWILL_ATTR_PORT_5G]));
	if (tb[MIWILL_ATTR_PORT_24G])
		pr_debug("%s port_24g: %.*s\n", __func__, nla_len(tb[MIWILL_ATTR_PORT_24G]),
			 (char *)nla_data(tb[MIWILL_ATTR_PORT_24G]));

	/*
	 * No miwill device: stock returns -1 here as well (miwill_name_to_dev() fails), because
	 * nothing on this phone creates one. Said once, so a daemon that keeps asking is quiet.
	 */
	if (atomic_inc_return(&miwill_set_calls) == 1)
		pr_info("k618: no miwill device (%s): the aggregation is not implemented, only this interface\n",
			name);
	return -EPERM;
}

static int miwill_nl_cmd_options_get(struct sk_buff *skb, struct genl_info *info)
{
	/* stock only logs and returns 0 */
	pr_debug("%s\n", __func__);
	return 0;
}

static int minet_nl_cmd_set(struct sk_buff *skb, struct genl_info *info)
{
	if (!info->attrs[MIWILL_ATTR_APP])
		return 0;
	/* stock hands this to its own per-flow accounting, which this module does not do */
	if (atomic_inc_return(&minet_set_calls) == 1)
		pr_info("k618: minet app list taken, flow accounting is not implemented\n");
	return 0;
}

static const struct genl_ops miwill_nl_ops[] = {
	{
		.cmd		= MIWILL_CMD_OPTIONS_SET,
		.doit		= miwill_nl_cmd_options_set,
		.flags		= GENL_ADMIN_PERM,
		.validate	= GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
	},
	{
		.cmd		= MIWILL_CMD_OPTIONS_GET,
		.doit		= miwill_nl_cmd_options_get,
		.flags		= GENL_ADMIN_PERM,
		.validate	= GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
	},
	{
		.cmd		= MIWILL_CMD_MINET_SET,
		.doit		= minet_nl_cmd_set,
		.flags		= GENL_ADMIN_PERM,
		.validate	= GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
	},
};

static const struct genl_multicast_group miwill_nl_mcgrps[] = {
	{ .name = "miwill_event" },
};

static struct genl_family miwill_nl_family __ro_after_init = {
	.name		= "miwill_genl",
	.version	= 1,
	.maxattr	= MIWILL_ATTR_MAX,
	.netnsok	= true,
	/* Device compatibility. */
	.resv_start_op	= MIWILL_CMD_MINET_SET + 1,
	.policy		= miwill_nl_policy,
	.ops		= miwill_nl_ops,
	.n_ops		= ARRAY_SIZE(miwill_nl_ops),
	.mcgrps		= miwill_nl_mcgrps,
	.n_mcgrps	= ARRAY_SIZE(miwill_nl_mcgrps),
	.module		= THIS_MODULE,
};

static int __init miwill_init(void)
{
	int ret = genl_register_family(&miwill_nl_family);

	if (ret)
		pr_err("%s genl_register_family:%d\n", __func__, ret);
	return ret;
}

static void __exit miwill_exit(void)
{
	genl_unregister_family(&miwill_nl_family);
}

module_init(miwill_init);
module_exit(miwill_exit);
MODULE_DESCRIPTION("driver for MiWiLL-capable device (k618: the netlink interface only)");
MODULE_LICENSE("GPL");
