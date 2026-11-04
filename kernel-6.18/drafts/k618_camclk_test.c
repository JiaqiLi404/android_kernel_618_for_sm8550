// SPDX-License-Identifier: GPL-2.0
/*
 * k618_camclk_test (test only, not part of the port): when the camera driver has requested the
 * wide or tele MCLK pin as a GPIO -- which muxes it to gpio -- switch it to cam_mclk, as
 * cam-res-mgr's cam_wide_active / cam_tele_active states do on 5.15 (their gpios-shared-pinctrl
 * numbers are 5.15's, base 301, and match nothing on 6.18). A kretprobe on gpio_request_one; the
 * TLMM chip's GPIO base comes from gpio_device_find_by_label(), as the planned fix will take it.
 *   insmod k618_camclk_test.ko                 (phone: label f000000.pinctrl, pins 102/103)
 *   insmod k618_camclk_test.ko label=9030000.pl061    (QEMU: registers, never fires)
 */
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/gpio/driver.h>

static char *label = "f000000.pinctrl";
module_param(label, charp, 0444);

static const struct of_device_id tlmm_of_match[] = {
	{ .compatible = "qcom,kalama-pinctrl" },
	{ .compatible = "qcom,kalama-pinctrl-v2" },
	{}
};

/* pin, drive code (cam_sensor_mclk2_active: 2 mA, mclk3_active: 4 mA) */
static const struct { int pin; u32 drv; } mclk[2] = { { 102, 0 }, { 103, 1 } };
static void __iomem *ctl[2];
static int gbase = -1;
static atomic_t hits = ATOMIC_INIT(0);

struct req {
	unsigned int gpio;
};

static int entry_h(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	((struct req *)ri->data)->gpio = regs->regs[0];
	return 0;
}

static int ret_h(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	unsigned int gpio = ((struct req *)ri->data)->gpio;
	u32 v;
	int i;

	if (regs_return_value(regs))
		return 0;
	for (i = 0; i < 2; i++) {
		if (gpio != gbase + mclk[i].pin)
			continue;
		v = readl(ctl[i]);
		writel((v & ~(0xf << 2 | 7 << 6 | 3)) | 1 << 2 | mclk[i].drv << 6, ctl[i]);
		atomic_inc(&hits);
		pr_info("k618_camclk: gpio%u requested, pin %d: ctl %#x -> %#x (cam_mclk)\n",
			gpio, mclk[i].pin, v, readl(ctl[i]));
	}
	return 0;
}

static struct kretprobe krp = {
	.handler = ret_h,
	.entry_handler = entry_h,
	.data_size = sizeof(struct req),
	.maxactive = 8,
	.kp.symbol_name = "gpio_request_one",
};

static int __init camclk_init(void)
{
	struct gpio_device *gdev;
	struct device_node *np;
	struct resource res;
	int i, ret;

	gdev = gpio_device_find_by_label(label);
	if (!gdev) {
		pr_err("k618_camclk: no gpio chip labelled %s\n", label);
		return -ENODEV;
	}
	gbase = gpio_device_get_base(gdev);
	gpio_device_put(gdev);

	np = of_find_matching_node(NULL, tlmm_of_match);
	if (!np || of_address_to_resource(np, 0, &res)) {
		of_node_put(np);
		pr_err("k618_camclk: no TLMM node\n");
		return -ENODEV;
	}
	of_node_put(np);
	for (i = 0; i < 2; i++) {
		ctl[i] = ioremap(res.start + 0x100000 + mclk[i].pin * 0x1000, 4);
		if (!ctl[i]) {
			while (i--)
				iounmap(ctl[i]);
			return -ENOMEM;
		}
	}
	ret = register_kretprobe(&krp);
	if (ret) {
		pr_err("k618_camclk: kretprobe on gpio_request_one: %d\n", ret);
		for (i = 0; i < 2; i++)
			iounmap(ctl[i]);
		return ret;
	}
	pr_info("k618_camclk: %s base %d: gpio%d/gpio%d get cam_mclk after each request\n",
		label, gbase, gbase + mclk[0].pin, gbase + mclk[1].pin);
	return 0;
}

static void __exit camclk_exit(void)
{
	int i;

	unregister_kretprobe(&krp);
	for (i = 0; i < 2; i++)
		iounmap(ctl[i]);
	pr_info("k618_camclk: %d remux(es), %d missed\n", atomic_read(&hits), krp.nmissed);
}

module_init(camclk_init);
module_exit(camclk_exit);
MODULE_DESCRIPTION("k618 test: wide/tele MCLK pins to cam_mclk after gpio_request_one");
MODULE_LICENSE("GPL");
