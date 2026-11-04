// SPDX-License-Identifier: GPL-2.0
/*
 * k618_tlmm: print TLMM pin control registers with their eGPIO bits, which the pinctrl debugfs
 * files leave out on kalama (its SoC data names no egpio function, and msm_gpio_dbg_show only
 * looks at the bits when one is named). Loaded by hand on a running phone:
 *
 *   insmod k618_tlmm.ko pins=0,1,57,66,69,91,182,183
 *   insmod k618_tlmm.ko all=1                          (every pin that is not reserved)
 *   insmod k618_tlmm.ko pins=182,183 claim=182,183    (also set egpio_enable, as 5.15 does)
 *   insmod k618_tlmm.ko pins=102 cfg=102:1:0:0         (pin:function:drive:pull, see below)
 *
 * One line per pin: the raw ctl and io registers and what they hold -- function, drive, pull,
 * output enable, in/out level, egpio_present (bit 11: the pin can belong to the LPASS island)
 * and egpio_enable (bit 12: APPS owns it). A pin with egpio_present and no egpio_enable is the
 * island's: APPS writes land in the register but do not move the pad.
 *
 * Reads only, unless claim= names pins: those get egpio_enable set (read-modify-write of their
 * ctl register), and only when egpio_present says they can take it. cfg= sets a pin's function
 * (the index in its pinctrl-kalama.c PINGROUP, 0 = gpio), drive (0..7 = 2..16 mA) and pull
 * (0 none, 1 down, 2 keeper, 3 up) -- the mux a driver never applied -- and leaves output enable
 * and the eGPIO bits as they are; it refuses a pin the island owns (claim it first). Both only
 * for pins in pins= (or all=1), so the line before and after is in the log.
 *
 * Pins the TLMM node reserves (gpio-reserved-ranges, which gpiolib honours, and
 * qcom,gpios-reserved, which the vendor pinctrl-msm does) are never touched -- what is left is
 * what `cat /sys/kernel/debug/gpio` reads.
 * With the node in the DT the register base is its reg + 0x100000, as pinctrl-kalama.c has it;
 * base= is for QEMU, which has no TLMM (and must match the node's when there is one).
 */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/string.h>

#define TLMM_CTL_OFF	0x100000	/* pinctrl-kalama.c: REG_BASE */
#define TLMM_PIN_SIZE	0x1000		/* pinctrl-kalama.c: REG_SIZE */
#define TLMM_NGPIOS	211
#define EGPIO_PRESENT	BIT(11)
#define EGPIO_ENABLE	BIT(12)

static unsigned long base;
module_param(base, ulong, 0444);
static int pins[32];
static int npins;
module_param_array(pins, int, &npins, 0444);
static bool all;
module_param(all, bool, 0444);
static int claim[32];
static int nclaim;
module_param_array(claim, int, &nclaim, 0444);
static char *cfg = "";
module_param(cfg, charp, 0444);

#define CFG_MAX		16
static int cfg_pin[CFG_MAX];
static u32 cfg_bits[CFG_MAX];	/* func << 2 | drv << 6 | pull */
static int ncfg;
#define CFG_MASK	(0xf << 2 | 7 << 6 | 3)

/* pinctrl-kalama.c's own two (the 13 Pro's node is -v2); not the VM's */
static const struct of_device_id tlmm_of_match[] = {
	{ .compatible = "qcom,kalama-pinctrl" },
	{ .compatible = "qcom,kalama-pinctrl-v2" },
	{}
};

static u32 ranges[2 * 16];	/* gpio-reserved-ranges: <first count> pairs */
static int nranges;
static u32 singles[64];		/* qcom,gpios-reserved: pins */
static int nsingles;

static bool pin_reserved(int pin)
{
	int i;

	for (i = 0; i + 1 < nranges; i += 2)
		if (pin >= ranges[i] && pin < ranges[i] + ranges[i + 1])
			return true;
	for (i = 0; i < nsingles; i++)
		if (pin == singles[i])
			return true;
	return false;
}

/* a property that is there but does not fit is an error: better no dump than one that reads a
 * reserved pin */
static int read_reserved(struct device_node *np, const char *name, u32 *to, int max, int *n)
{
	int len = of_property_count_u32_elems(np, name);

	if (len == -EINVAL)
		return 0;			/* not there */
	if (len < 0 || len > max)
		return -E2BIG;
	*n = len;
	return of_property_read_u32_array(np, name, to, len);
}

static bool in_list(const int *list, int n, int pin)
{
	int i;

	for (i = 0; i < n; i++)
		if (list[i] == pin)
			return true;
	return false;
}

/* "pin:func:drv:pull,..." -- 10 functions per pin (gpio + f1..f9), as pinctrl-kalama.c's nfuncs */
static int parse_cfg(void)
{
	char *buf, *p, *e;
	int pin;
	unsigned int func, drv, pull;

	if (!cfg || !*cfg)
		return 0;
	buf = kstrdup(cfg, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	p = buf;
	while ((e = strsep(&p, ",")) != NULL) {
		if (!*e)
			continue;
		if (ncfg == CFG_MAX || sscanf(e, "%d:%u:%u:%u", &pin, &func, &drv, &pull) != 4 ||
		    func > 9 || drv > 7 || pull > 3 || (!all && !in_list(pins, npins, pin))) {
			pr_err("k618_tlmm: cfg entry '%s' refused\n", e);
			kfree(buf);
			return -EINVAL;
		}
		cfg_pin[ncfg] = pin;
		cfg_bits[ncfg++] = func << 2 | drv << 6 | pull;
	}
	kfree(buf);
	return 0;
}

static void show(int pin, void __iomem *r, const char *what)
{
	u32 ctl = readl(r), io = readl(r + 4);

	pr_info("k618_tlmm: gpio%d%s ctl %#06x io %#x: func %u drv %u pull %u oe %u in %u out %u egpio present %u enable %u%s\n",
		pin, what, ctl, io, (ctl >> 2) & 0xf, (ctl >> 6) & 7, ctl & 3, !!(ctl & BIT(9)),
		io & 1, !!(io & 2), !!(ctl & EGPIO_PRESENT), !!(ctl & EGPIO_ENABLE),
		(ctl & EGPIO_PRESENT) && !(ctl & EGPIO_ENABLE) ? " (island owns it)" : "");
}

static int __init k618_tlmm_init(void)
{
	struct device_node *np;
	struct resource res;
	resource_size_t start, size;
	int i, j, n, ret;

	np = of_find_matching_node(NULL, tlmm_of_match);
	if (np) {
		if (of_address_to_resource(np, 0, &res)) {
			of_node_put(np);
			pr_err("k618_tlmm: TLMM node has no reg\n");
			return -ENODEV;
		}
		ret = read_reserved(np, "gpio-reserved-ranges", ranges, ARRAY_SIZE(ranges), &nranges);
		if (!ret)
			ret = read_reserved(np, "qcom,gpios-reserved", singles, ARRAY_SIZE(singles),
					    &nsingles);
		of_node_put(np);
		if (ret || (nranges & 1)) {
			pr_err("k618_tlmm: cannot read the reserved pins (%d)\n", ret);
			return -EINVAL;
		}
		start = res.start + TLMM_CTL_OFF;
		size = resource_size(&res) > TLMM_CTL_OFF ? resource_size(&res) - TLMM_CTL_OFF : 0;
		if (base && base != start) {
			pr_err("k618_tlmm: base %#lx is not the TLMM's %pa\n", base, &start);
			return -EINVAL;
		}
		pr_info("k618_tlmm: TLMM %pR, pin registers from %pa, %d reserved range(s), %d single reserved pin(s)\n",
			&res, &start, nranges / 2, nsingles);
	} else if (base) {
		start = base;
		size = (resource_size_t)TLMM_NGPIOS * TLMM_PIN_SIZE;
		pr_info("k618_tlmm: no TLMM node, base %#lx as given\n", base);
	} else {
		pr_err("k618_tlmm: no TLMM node and no base=\n");
		return -ENODEV;
	}

	for (i = 0; i < nclaim; i++)
		if (!all && !in_list(pins, npins, claim[i])) {
			pr_err("k618_tlmm: claim=%d is not in pins=\n", claim[i]);
			return -EINVAL;
		}
	ret = parse_cfg();
	if (ret)
		return ret;

	n = all ? TLMM_NGPIOS : npins;
	for (i = 0; i < n; i++) {
		int pin = all ? i : pins[i];
		void __iomem *r;
		u32 ctl;

		if (pin < 0 || pin >= TLMM_NGPIOS ||
		    (resource_size_t)(pin + 1) * TLMM_PIN_SIZE > size) {
			if (!all)
				pr_info("k618_tlmm: gpio%d: out of range, skipped\n", pin);
			continue;
		}
		if (pin_reserved(pin)) {
			pr_info("k618_tlmm: gpio%d: reserved, skipped\n", pin);
			continue;
		}
		r = ioremap(start + (resource_size_t)pin * TLMM_PIN_SIZE, 8);
		if (!r) {
			pr_err("k618_tlmm: gpio%d: ioremap failed\n", pin);
			continue;
		}
		show(pin, r, "");
		if (in_list(claim, nclaim, pin)) {
			ctl = readl(r);
			if (!(ctl & EGPIO_PRESENT))
				pr_info("k618_tlmm: gpio%d: no egpio_present, not claimed\n", pin);
			else if (ctl & EGPIO_ENABLE)
				pr_info("k618_tlmm: gpio%d: APPS owns it already\n", pin);
			else {
				writel(ctl | EGPIO_ENABLE, r);
				show(pin, r, " claimed");
			}
		}
		for (j = 0; j < ncfg; j++) {
			if (cfg_pin[j] != pin)
				continue;
			ctl = readl(r);
			if ((ctl & EGPIO_PRESENT) && !(ctl & EGPIO_ENABLE)) {
				pr_info("k618_tlmm: gpio%d: the island owns it, cfg not applied\n", pin);
				break;
			}
			writel((ctl & ~CFG_MASK) | cfg_bits[j], r);
			show(pin, r, " set");
			break;
		}
		iounmap(r);
	}
	return 0;
}

static void __exit k618_tlmm_exit(void)
{
}

module_init(k618_tlmm_init);
module_exit(k618_tlmm_exit);
MODULE_DESCRIPTION("k618: print TLMM pin registers with their eGPIO bits");
MODULE_LICENSE("GPL");
