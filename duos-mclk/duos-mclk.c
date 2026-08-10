// SPDX-License-Identifier: GPL-2.0
/*
 * Throwaway: turn on the Milk-V Duo S camera master clock.
 *
 * A mainline kernel has no consumer for CLK_CAM0/CLK_CAM1, so the SG2000's
 * camera MCLK pads stay dead and a sensor on J1/J2 never sees a clock - which
 * means it will not answer on I2C either. This module takes the clock straight
 * from the provider and enables it, so that i2cdetect can say something
 * meaningful about whether the camera is wired up the way the vendor board
 * files claim.
 *
 * There is no device node to bind to, on purpose: this needs no device tree
 * change, so it can be dropped onto a stock Armbian image. It resolves the
 * clock by building the phandle spec the clock controller would have been
 * given, which is why it pokes at of_clk_get_from_provider() directly. That is
 * not how a real driver should do this, and this file is not a real driver.
 *
 * Defaults are J1: CLK_CAM0 (147) at 24MHz, which is what the SDK's
 * gc2083_rx_attr asks for. For J2 use index=148.
 *
 *   insmod duos-mclk.ko                  # CLK_CAM0 @ 24MHz
 *   insmod duos-mclk.ko index=148        # CLK_CAM1 @ 24MHz
 *   rmmod duos_mclk                      # clock off again
 */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_clk.h>

#define CLK_CAM0	147
#define CLK_CAM1	148

static unsigned int index = CLK_CAM0;
module_param(index, uint, 0444);
MODULE_PARM_DESC(index, "clock id from dt-bindings/clock/sophgo,cv1800.h (147=CAM0, 148=CAM1)");

static unsigned long rate = 24000000;
module_param(rate, ulong, 0444);
MODULE_PARM_DESC(rate, "MCLK rate in Hz (24000000 for GC2083/OV5647, 27000000 for GC4653)");

static struct clk *mclk;

static int __init duos_mclk_init(void)
{
	struct of_phandle_args spec = { };
	struct device_node *np;
	int ret;

	np = of_find_compatible_node(NULL, NULL, "sophgo,sg2000-clk");
	if (!np) {
		pr_err("duos-mclk: no sophgo,sg2000-clk node\n");
		return -ENODEV;
	}

	spec.np = np;
	spec.args_count = 1;
	spec.args[0] = index;

	mclk = of_clk_get_from_provider(&spec);
	of_node_put(np);
	if (IS_ERR(mclk)) {
		ret = PTR_ERR(mclk);
		pr_err("duos-mclk: cannot get clock %u: %d\n", index, ret);
		return ret;
	}

	ret = clk_set_rate(mclk, rate);
	if (ret) {
		pr_err("duos-mclk: set_rate(%lu) failed: %d\n", rate, ret);
		goto err_put;
	}

	ret = clk_prepare_enable(mclk);
	if (ret) {
		pr_err("duos-mclk: enable failed: %d\n", ret);
		goto err_put;
	}

	pr_info("duos-mclk: clock %u enabled, asked %lu Hz, got %lu Hz\n",
		index, rate, clk_get_rate(mclk));
	return 0;

err_put:
	clk_put(mclk);
	mclk = NULL;
	return ret;
}

static void __exit duos_mclk_exit(void)
{
	if (!mclk)
		return;

	clk_disable_unprepare(mclk);
	clk_put(mclk);
	pr_info("duos-mclk: clock %u disabled\n", index);
}

module_init(duos_mclk_init);
module_exit(duos_mclk_exit);

MODULE_DESCRIPTION("Enable the SG2000 camera MCLK for Milk-V Duo S bring-up");
MODULE_LICENSE("GPL");
