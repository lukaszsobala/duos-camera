// SPDX-License-Identifier: GPL-2.0
/*
 * Stage 1 of a CSI-2 receiver for the SG2000: bring the MIPI link up and report
 * what the PHY and the CSI-2 decoder see. No DMA, no buffers, no /dev/video0.
 *
 * This replaces duos-cam-stub.ko. It keeps the stub's entire reason for
 * existing - registering a v4l2_device so the sensor's subdev gets a device node
 * and the sensor can be told to stream - and adds the part the stub explicitly
 * did not do: it programs the D-PHY and the CSI-2 MAC so that the lanes are
 * actually terminated and decoded.
 *
 * What that buys is a measurement. With the link up and the sensor streaming,
 * the MAC will tell us whether it is receiving valid CSI-2:
 *
 *   cat /sys/bus/platform/drivers/duos-csi/<dev>/phy_state
 *   cat /sys/bus/platform/drivers/duos-csi/<dev>/csi_status
 *
 * A PHY in HS mode on the clock lane, a decode format of RAW10, and error
 * counters that do not climb, together prove the whole transmit chain - MCLK,
 * the sensor's init table, the lane wiring, the lane mapping in this driver -
 * without a single frame buffer existing anywhere. That is the point of doing
 * this before touching the CSIBDG write-DMA.
 *
 * Everything here is derived from the vendor 5.10 SDK, which ships full
 * register-field headers for these two blocks. See receiver-plan.md for the
 * register map, the interrupt numbers and where each constant came from. The
 * relevant vendor sources are:
 *
 *   osdrv/interdrv/cif/chip/cv181x/cif.c          - _cif_mipi_config()
 *   osdrv/interdrv/cif/chip/cv181x/drv/cif_drv.c  - _cif_csi_config(), the
 *                                                   lane/deskew/streaming helpers
 *   osdrv/interdrv/cif/chip/cv181x/drv/inc/reg_fields_csi_{mac,wrap}.h
 *
 * The vendor driver is generic across CV180x/CV181x, five input formats and two
 * links. This is one board, one link, CSI-2 RAW10 only, with the Duo S lane
 * mapping resolved to constants rather than carried in a config struct.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/reset.h>

#include <media/v4l2-async.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>

/*
 * Register offsets. The vendor headers describe these blocks as C structs of
 * bitfield unions, padded so that each member's name is its own offset - REG_70
 * really does live at 0x70 - so the offsets below are the names transcribed.
 *
 * Within the csi_mac0 window (0x0a0c2000) there are two blocks we touch:
 * "sensor mac" at 0x000 and "csi_ctrl_top" at 0x400. Within the csi_wrap0
 * window (0x0a0d0000): "phy top" at 0x000 and the 4-lane PHY at 0x300.
 */
#define MAC_TOP				0x000
#define MAC_CSI				0x400

#define MAC_SENSOR_MAC_00		(MAC_TOP + 0x00)
#define  SENSOR_MAC_MODE		GENMASK(2, 0)
#define  CSI_CTRL_ENABLE		BIT(4)
#define  CSI_VS_INV			BIT(5)
#define  CSI_HS_INV			BIT(6)

#define MAC_CSI_00			(MAC_CSI + 0x00)
#define  CSI_LANE_MODE			GENMASK(2, 0)

#define MAC_CSI_04			(MAC_CSI + 0x04)
#define  CSI_INTR_MASK			GENMASK(7, 0)
#define  CSI_INTR_CLR			GENMASK(15, 8)

#define MAC_CSI_18			(MAC_CSI + 0x18)
#define  CSI_VC_MAP_CH00		GENMASK(3, 0)
#define  CSI_VC_MAP_CH01		GENMASK(7, 4)
#define  CSI_VC_MAP_CH10		GENMASK(11, 8)
#define  CSI_VC_MAP_CH11		GENMASK(15, 12)

/* Status. Read-only, and the error bits are sticky until CSI_INTR_CLR. */
#define MAC_CSI_40			(MAC_CSI + 0x40)
#define  CSI_ECC_NO_ERROR		BIT(0)
#define  CSI_ECC_CORRECTED_ERROR	BIT(1)
#define  CSI_ECC_ERROR			BIT(2)
#define  CSI_CRC_ERROR			BIT(4)
#define  CSI_WC_ERROR			BIT(5)
#define  CSI_FIFO_FULL			BIT(8)
#define  CSI_DECODE_FORMAT		GENMASK(21, 16)

#define MAC_CSI_60			(MAC_CSI + 0x60)
#define  CSI_INTR_STATUS		GENMASK(7, 0)

#define MAC_CSI_70			(MAC_CSI + 0x70)
#define  CSI_VS_GEN_MODE		GENMASK(1, 0)

#define WRAP_TOP			0x000
#define WRAP_4L				0x300

#define WRAP_PHY_TOP_00			(WRAP_TOP + 0x00)
#define  MIPIRX_PD_IBIAS		BIT(14)
#define  MIPIRX_PD_RXLP			GENMASK(21, 16)

#define WRAP_PHY_TOP_04			(WRAP_TOP + 0x04)
#define  MIPIRX_SEL_CLK_P0TOP1		BIT(6)
#define  MIPIRX_SEL_CLK_P1TOP0		BIT(7)
#define  MIPIRX_SEL_CLK_CHANNEL		GENMASK(21, 16)

#define WRAP_PHY_TOP_80			(WRAP_TOP + 0x80)
#define  FORCE_DESKEW_CODE(n)		BIT(16 + (n))

#define WRAP_PHY_TOP_84			(WRAP_TOP + 0x84)
#define WRAP_PHY_TOP_88			(WRAP_TOP + 0x88)

#define WRAP_4L_00			(WRAP_4L + 0x00)
#define  PHY_SENSOR_MODE		GENMASK(1, 0)

#define WRAP_4L_04			(WRAP_4L + 0x04)
#define  CSI_LANE_D0_SEL		GENMASK(2, 0)
#define  CSI_LANE_D1_SEL		GENMASK(6, 4)
#define  CSI_LANE_D2_SEL		GENMASK(10, 8)
#define  CSI_LANE_D3_SEL		GENMASK(14, 12)

#define WRAP_4L_08			(WRAP_4L + 0x08)
#define  CSI_LANE_CK_SEL		GENMASK(2, 0)
#define  CSI_LANE_CK_PNSWAP		BIT(4)
#define  CSI_LANE_D0_PNSWAP		BIT(8)
#define  CSI_LANE_D1_PNSWAP		BIT(9)
#define  CSI_LANE_D2_PNSWAP		BIT(10)
#define  CSI_LANE_D3_PNSWAP		BIT(11)

#define WRAP_4L_0C			(WRAP_4L + 0x0c)
#define  DESKEW_LANE_EN			GENMASK(3, 0)

#define WRAP_4L_10			(WRAP_4L + 0x10)
#define  T_HS_SETTLE			GENMASK(7, 0)
#define  AUTO_IGNORE			BIT(16)
#define  AUTO_SYNC			BIT(17)

/* PHY state, reported as two 16-bit halves. See duos_csi_phy_state(). */
#define WRAP_4L_DBG_90			(WRAP_4L + 0x90)
#define WRAP_4L_DBG_94			(WRAP_4L + 0x94)

/* vip_sys, 0x0a0c8000. Setting a bit asserts that block's reset. */
#define VIP_SYS_RESETS			0x00
#define  VIP_RESET_CSI_MAC0		BIT(12)

/*
 * A note on what is deliberately missing: the vendor clears the pull-up and
 * pull-down on each MIPI pad before bringing the link up, at
 * PSD_CTRL_OFFSET(n) = 0x03001c30 + (5 - n) * 8, one 8-byte pair per PHY lane.
 *
 * We do not, because those are not our registers. 0x03001c30 lands inside the
 * pinctrl block's "sys" window (0x03001000 + 0x1000), which pinctrl requests
 * exclusively - asking for it fails probe with -EBUSY, which is how this was
 * found. The vendor gets away with it only because its bring-up predates having
 * a pinctrl driver at all.
 *
 * The mapping is exact, so the proper fix is cheap when it is needed: the
 * vendor's offset for lane n is the pinctrl conf register of MIPIRX<n>N, and
 * offset + 4 is MIPIRX<n>P. From pinctrl-sg2000.c, MIPIRX0N is at 0xc58 and
 * MIPIRX0P at 0xc5c, which is 0x03001c58/0x03001c5c - i.e. (5 - 0) * 8 = 40
 * past 0x03001c30. Lanes 1 and 2 line up the same way (0xc50, 0xc48).
 *
 * Expressed properly that is a pinctrl group on this node with
 * bias-pull-up = <0> and bias-pull-down = <0> on MIPIRX0N/0P, MIPIRX1N/1P and
 * MIPIRX2N/2P. It is not done yet for one reason: the binding requires both
 * "pinmux" and "power-source" in a pin group, so it cannot be config-only, and
 * asserting a mux value for these pads without knowing the right one risks
 * breaking pads that currently work.
 *
 * Leaving the pads at their reset default is a defensible starting point - these
 * are dedicated differential D-PHY pads in their own 1.8V VDD18A_MIPI domain,
 * where an enabled pull would be surprising - and it is self-checking: if the
 * PHY locks, the question was moot. If it does not, this is on the list.
 */

/*
 * The Duo S wiring, from the vendor's own sensor parameters
 * (cvi_mpi/component/isp/sensor/sg200x/gcore_gc2083/gc2083_cmos_param.h:
 * lane_id = {1, 0, 2, -1, -1}, pn_swap all zero).
 *
 * Clock on PHY lane 1, data on PHY lanes 0 and 2. All three are on port 0,
 * which is what selects the free-running clock direction and the "cross" deskew
 * phases below.
 */
#define DUOS_CLK_LANE			1
#define DUOS_DATA_LANE0			0
#define DUOS_DATA_LANE1			2
#define DUOS_NUM_DATA_LANES		2

/* Unused PHY lanes still get parked on a distinct select value. */
#define DUOS_PARK_LANE0			3
#define DUOS_PARK_LANE1			4

/*
 * Deskew phases, from the vendor's lane_phase[] table
 * {0x00, 0x03, 0x08, 0x00, 0x03} indexed by LANE_SKEW_CROSS_CLK and
 * LANE_SKEW_CROSS_DATA_NEAR. A zero phase leaves FORCE_DESKEW_CODE clear, which
 * is why the clock lane is not forced.
 *
 * Both are module parameters for the same reason the vendor made them one: if
 * the PHY will not lock, this is the first thing to sweep.
 */
static u8 deskew_clk;
module_param(deskew_clk, byte, 0644);
MODULE_PARM_DESC(deskew_clk, "clock lane deskew phase (default 0)");

static u8 deskew_data = 0x03;
module_param(deskew_data, byte, 0644);
MODULE_PARM_DESC(deskew_data, "data lane deskew phase (default 3)");

/*
 * T_HS_SETTLE. The vendor never programs this for the GC2083 - its
 * combo_dev_attr leaves .dphy.enable clear, so the reset default stands - so
 * the default here is "do not touch". Non-zero writes the register.
 */
static u8 hs_settle;
module_param(hs_settle, byte, 0644);
MODULE_PARM_DESC(hs_settle, "T_HS_SETTLE override, 0 to leave at reset default");

/*
 * The decoder's format report. This is CSI_DECODE_FORMAT's bit order, which is
 * the vendor's enum csi_decode_fmt_e (cif_drv.h) - note it starts at YUV, so a
 * correctly received GC2083 stream sets bit 3 and nothing else.
 *
 * There is no matching "program the expected data type" register on this path:
 * the vendor computes a CSI-2 data type (0x2b for RAW10) but only ever writes it
 * as CSI_HDR_DT_FORMAT in the HDR configuration, which we do not use. For a
 * plain single-exposure stream the decoder reports what it finds and is told
 * nothing, so this array is for reading, not writing.
 */
static const char * const duos_csi_decode_fmt[] = {
	"YUV422-8", "YUV422-10", "RAW8", "RAW10", "RAW12",
};

struct duos_csi {
	struct v4l2_device v4l2_dev;
	struct v4l2_async_notifier notifier;

	void __iomem *mac;
	void __iomem *wrap;
	void __iomem *vip_sys;

	struct clk_bulk_data *clks;
	int num_clks;
	struct reset_control *phy_reset;
	struct reset_control *phy_apb_reset;

	/* Guards @sensor and the register state against unbind racing sysfs. */
	struct mutex lock;
	struct v4l2_subdev *sensor;
	bool streaming;
};

static void duos_csi_update(void __iomem *base, u32 reg, u32 mask, u32 val)
{
	u32 tmp = readl(base + reg);

	tmp &= ~mask;
	tmp |= val & mask;
	writel(tmp, base + reg);
}

/*
 * Park one PHY lane's select field. The vendor walks a bitmask of unused lanes
 * and assigns each a select value; skipping that leaves two data-lane selects
 * pointing at lanes we are also using, which is a mux conflict.
 */
static void duos_csi_set_lane_selects(struct duos_csi *csi)
{
	duos_csi_update(csi->wrap, WRAP_4L_08, CSI_LANE_CK_SEL,
			FIELD_PREP(CSI_LANE_CK_SEL, DUOS_CLK_LANE));
	duos_csi_update(csi->wrap, WRAP_4L_04,
			CSI_LANE_D0_SEL | CSI_LANE_D1_SEL |
			CSI_LANE_D2_SEL | CSI_LANE_D3_SEL,
			FIELD_PREP(CSI_LANE_D0_SEL, DUOS_DATA_LANE0) |
			FIELD_PREP(CSI_LANE_D1_SEL, DUOS_DATA_LANE1) |
			FIELD_PREP(CSI_LANE_D2_SEL, DUOS_PARK_LANE0) |
			FIELD_PREP(CSI_LANE_D3_SEL, DUOS_PARK_LANE1));

	/* No P/N swap anywhere on this board. */
	duos_csi_update(csi->wrap, WRAP_4L_08,
			CSI_LANE_CK_PNSWAP | CSI_LANE_D0_PNSWAP |
			CSI_LANE_D1_PNSWAP | CSI_LANE_D2_PNSWAP |
			CSI_LANE_D3_PNSWAP, 0);

	/* PHYA clock channel select is a bitmask, not an index. */
	duos_csi_update(csi->wrap, WRAP_PHY_TOP_04, MIPIRX_SEL_CLK_CHANNEL,
			FIELD_PREP(MIPIRX_SEL_CLK_CHANNEL,
				   BIT(DUOS_CLK_LANE)));
}

static void duos_csi_set_deskew(struct duos_csi *csi)
{
	static const u8 data_lanes[] = { DUOS_DATA_LANE0, DUOS_DATA_LANE1 };
	unsigned int i;

	/*
	 * DESKEW_CODE0..3 are the four bytes of PHY_TOP_84 and 4..5 the low two
	 * of PHY_TOP_88, so a lane's code is a byte field indexed by lane
	 * number. Every lane this board uses is below 4.
	 */
	duos_csi_update(csi->wrap, WRAP_PHY_TOP_84,
			0xffU << (8 * DUOS_CLK_LANE),
			(u32)deskew_clk << (8 * DUOS_CLK_LANE));
	duos_csi_update(csi->wrap, WRAP_PHY_TOP_80,
			FORCE_DESKEW_CODE(DUOS_CLK_LANE),
			deskew_clk ? FORCE_DESKEW_CODE(DUOS_CLK_LANE) : 0);

	for (i = 0; i < ARRAY_SIZE(data_lanes); i++) {
		u8 lane = data_lanes[i];

		duos_csi_update(csi->wrap, WRAP_PHY_TOP_84,
				0xffU << (8 * lane),
				(u32)deskew_data << (8 * lane));
		duos_csi_update(csi->wrap, WRAP_PHY_TOP_80,
				FORCE_DESKEW_CODE(lane),
				deskew_data ? FORCE_DESKEW_CODE(lane) : 0);
	}
}

/*
 * The MAC and PHY format configuration, i.e. the vendor's _cif_csi_config()
 * with our constants substituted.
 */
static void duos_csi_configure(struct duos_csi *csi)
{
	/*
	 * Sensor mode 1 is CSI-2. The HS/VS inversions are what the vendor
	 * unconditionally programs for this input type; they are not a Duo S
	 * quirk.
	 */
	duos_csi_update(csi->mac, MAC_SENSOR_MAC_00,
			SENSOR_MAC_MODE | CSI_CTRL_ENABLE |
			CSI_VS_INV | CSI_HS_INV,
			FIELD_PREP(SENSOR_MAC_MODE, 1) | CSI_CTRL_ENABLE |
			CSI_VS_INV | CSI_HS_INV);

	/* Lane mode is the data lane count minus one. */
	duos_csi_update(csi->mac, MAC_CSI_00, CSI_LANE_MODE,
			FIELD_PREP(CSI_LANE_MODE, DUOS_NUM_DATA_LANES - 1));

	duos_csi_update(csi->mac, MAC_CSI_70, CSI_VS_GEN_MODE,
			FIELD_PREP(CSI_VS_GEN_MODE, 0));

	/* Identity virtual-channel mapping; we only ever use VC0. */
	duos_csi_update(csi->mac, MAC_CSI_18,
			CSI_VC_MAP_CH00 | CSI_VC_MAP_CH01 |
			CSI_VC_MAP_CH10 | CSI_VC_MAP_CH11,
			FIELD_PREP(CSI_VC_MAP_CH00, 0) |
			FIELD_PREP(CSI_VC_MAP_CH01, 1) |
			FIELD_PREP(CSI_VC_MAP_CH10, 2) |
			FIELD_PREP(CSI_VC_MAP_CH11, 3));

	/*
	 * PHY digital: CSI-2 mode, and auto-ignore/auto-sync off. The vendor
	 * turns both off "by default" and never turns them back on.
	 */
	duos_csi_update(csi->wrap, WRAP_4L_00, PHY_SENSOR_MODE, 0);
	duos_csi_update(csi->wrap, WRAP_4L_10, AUTO_IGNORE | AUTO_SYNC, 0);

	if (hs_settle)
		duos_csi_update(csi->wrap, WRAP_4L_10, T_HS_SETTLE,
				FIELD_PREP(T_HS_SETTLE, hs_settle));
}

/*
 * PHY power and lane enable. This is the vendor's cif_stream_enable() for the
 * CSI-2 case, in its exact order: the analogue bias and low-power receivers are
 * powered down and the lanes disabled first even when turning on, and the 20us
 * between powering the receivers up and enabling the lanes is theirs.
 */
static void duos_csi_phy_enable(struct duos_csi *csi, bool on)
{
	duos_csi_update(csi->wrap, WRAP_PHY_TOP_00,
			MIPIRX_PD_IBIAS | MIPIRX_PD_RXLP,
			MIPIRX_PD_IBIAS | FIELD_PREP(MIPIRX_PD_RXLP, 0x3f));

	if (on)
		duos_csi_update(csi->wrap, WRAP_PHY_TOP_00,
				MIPIRX_PD_IBIAS, 0);

	duos_csi_update(csi->wrap, WRAP_4L_0C, DESKEW_LANE_EN, 0);

	if (on) {
		duos_csi_update(csi->wrap, WRAP_PHY_TOP_00, MIPIRX_PD_RXLP, 0);
		usleep_range(20, 40);
		duos_csi_update(csi->wrap, WRAP_4L_0C, DESKEW_LANE_EN,
				FIELD_PREP(DESKEW_LANE_EN,
					   BIT(DUOS_NUM_DATA_LANES) - 1));
	}
}

/* Toggle one block's reset bit in vip_sys, the vendor's vip_toggle_reset(). */
static void duos_csi_vip_reset(struct duos_csi *csi, u32 mask)
{
	duos_csi_update(csi->vip_sys, VIP_SYS_RESETS, mask, mask);
	usleep_range(20, 40);
	duos_csi_update(csi->vip_sys, VIP_SYS_RESETS, mask, 0);
}

static void duos_csi_reset(struct duos_csi *csi)
{
	/* Mask the MAC's error interrupts while it is in reset. */
	duos_csi_update(csi->mac, MAC_CSI_04, CSI_INTR_MASK,
			FIELD_PREP(CSI_INTR_MASK, 0xff));

	reset_control_assert(csi->phy_reset);
	reset_control_assert(csi->phy_apb_reset);
	udelay(5);
	reset_control_deassert(csi->phy_reset);
	reset_control_deassert(csi->phy_apb_reset);

	duos_csi_vip_reset(csi, VIP_RESET_CSI_MAC0);
}

static void duos_csi_clear_errors(struct duos_csi *csi)
{
	duos_csi_update(csi->mac, MAC_CSI_04, CSI_INTR_CLR,
			FIELD_PREP(CSI_INTR_CLR, 0xff));
}

/* Bring the link up. Caller holds the lock. */
static void duos_csi_link_up(struct duos_csi *csi)
{
	duos_csi_reset(csi);

	duos_csi_set_lane_selects(csi);

	/*
	 * Free-running clock direction. The vendor picks this whenever the
	 * clock lane is on port 0 (lanes 0-2) in a 1-clock-2-data setup, which
	 * is exactly this board.
	 */
	duos_csi_update(csi->wrap, WRAP_PHY_TOP_04,
			MIPIRX_SEL_CLK_P0TOP1 | MIPIRX_SEL_CLK_P1TOP0, 0);

	duos_csi_set_deskew(csi);

	duos_csi_phy_enable(csi, false);
	duos_csi_configure(csi);
	duos_csi_phy_enable(csi, true);

	duos_csi_clear_errors(csi);

	/* Unmask so CSI_INTR_STATUS becomes meaningful. We take no interrupt. */
	duos_csi_update(csi->mac, MAC_CSI_04, CSI_INTR_MASK, 0);
}

static void duos_csi_link_down(struct duos_csi *csi)
{
	duos_csi_update(csi->mac, MAC_CSI_04, CSI_INTR_MASK,
			FIELD_PREP(CSI_INTR_MASK, 0xff));
	duos_csi_phy_enable(csi, false);
	duos_csi_update(csi->mac, MAC_SENSOR_MAC_00, CSI_CTRL_ENABLE, 0);
}

static int duos_csi_bound(struct v4l2_async_notifier *notifier,
			  struct v4l2_subdev *sd,
			  struct v4l2_async_connection *asc)
{
	struct duos_csi *csi = container_of(notifier, struct duos_csi, notifier);

	guard(mutex)(&csi->lock);
	csi->sensor = sd;

	dev_info(csi->v4l2_dev.dev, "bound sensor %s\n", sd->name);

	return 0;
}

static void duos_csi_unbind(struct v4l2_async_notifier *notifier,
			    struct v4l2_subdev *sd,
			    struct v4l2_async_connection *asc)
{
	struct duos_csi *csi = container_of(notifier, struct duos_csi, notifier);

	guard(mutex)(&csi->lock);

	/*
	 * The sensor is going away underneath us; its own remove() stops
	 * streaming. Drop the link so the PHY is not left powered on a
	 * transmitter that no longer exists.
	 */
	if (csi->streaming)
		duos_csi_link_down(csi);

	csi->sensor = NULL;
	csi->streaming = false;
}

static int duos_csi_complete(struct v4l2_async_notifier *notifier)
{
	struct duos_csi *csi = container_of(notifier, struct duos_csi, notifier);

	return v4l2_device_register_subdev_nodes(&csi->v4l2_dev);
}

static const struct v4l2_async_notifier_operations duos_csi_notifier_ops = {
	.bound = duos_csi_bound,
	.unbind = duos_csi_unbind,
	.complete = duos_csi_complete,
};

static ssize_t sensor_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct duos_csi *csi = dev_get_drvdata(dev);

	guard(mutex)(&csi->lock);

	return sysfs_emit(buf, "%s\n", csi->sensor ? csi->sensor->name : "");
}
static DEVICE_ATTR_RO(sensor);

static ssize_t stream_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct duos_csi *csi = dev_get_drvdata(dev);

	guard(mutex)(&csi->lock);

	return sysfs_emit(buf, "%d\n", csi->streaming);
}

static ssize_t stream_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t len)
{
	struct duos_csi *csi = dev_get_drvdata(dev);
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;

	guard(mutex)(&csi->lock);

	if (!csi->sensor)
		return -ENODEV;

	if (enable == csi->streaming)
		return len;

	if (enable) {
		/*
		 * Receiver first. A CSI-2 receiver that starts after the
		 * transmitter has to recover sync mid-stream, and there is no
		 * reason to ask it to.
		 */
		duos_csi_link_up(csi);

		ret = v4l2_subdev_call(csi->sensor, video, s_stream, 1);
		if (ret) {
			duos_csi_link_down(csi);
			return ret;
		}
	} else {
		ret = v4l2_subdev_call(csi->sensor, video, s_stream, 0);
		duos_csi_link_down(csi);

		/*
		 * The link is down either way, so record that even if the
		 * sensor complained on the way out. Reporting failure while
		 * leaving @streaming set would make the next "echo 0" a no-op
		 * against hardware that is already stopped.
		 */
		csi->streaming = false;

		return ret ? ret : len;
	}

	csi->streaming = enable;

	return len;
}
static DEVICE_ATTR_RW(stream);

/*
 * The PHY's own view of the link. The vendor assembles this from two 16-bit
 * debug registers into one word; the field layout is union mipi_phy_state in
 * cif_drv.h.
 *
 * "clk hs" is the one that matters: it means the clock lane is in high-speed
 * mode, i.e. the sensor is driving a real DDR clock and the PHY has locked to
 * it. Each data lane reports a 3-bit state rather than a flag.
 */
static ssize_t phy_state_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct duos_csi *csi = dev_get_drvdata(dev);
	u32 state;

	guard(mutex)(&csi->lock);

	state = readl(csi->wrap + WRAP_4L_DBG_90) |
		readl(csi->wrap + WRAP_4L_DBG_94) << 16;

	return sysfs_emit(buf,
			  "raw 0x%08x\n"
			  "clk hs %u ulps %u stop %u err %u\n"
			  "d0 0x%x d1 0x%x d2 0x%x d3 0x%x\n"
			  "deskew 0x%x\n",
			  state,
			  !!(state & BIT(0)), !!(state & BIT(1)),
			  !!(state & BIT(2)), !!(state & BIT(3)),
			  (state >> 8) & 0x7, (state >> 12) & 0x7,
			  (state >> 16) & 0x7, (state >> 20) & 0x7,
			  (state >> 24) & 0x3);
}
static DEVICE_ATTR_RO(phy_state);

/*
 * The CSI-2 decoder's view. decode_format is a bitmask of formats seen, so a
 * lone RAW10 bit is what a correctly received GC2083 stream looks like; more
 * than one bit set means the decoder is finding packet headers it should not.
 */
static ssize_t csi_status_show(struct device *dev, struct device_attribute *attr,
			       char *buf)
{
	struct duos_csi *csi = dev_get_drvdata(dev);
	u32 status, fmt, intr;
	int len;

	guard(mutex)(&csi->lock);

	status = readl(csi->mac + MAC_CSI_40);
	intr = readl(csi->mac + MAC_CSI_60);
	fmt = FIELD_GET(CSI_DECODE_FORMAT, status);

	len = sysfs_emit(buf,
			 "raw 0x%08x intr 0x%02x\n"
			 "decode_format 0x%02x", status,
			 (u32)FIELD_GET(CSI_INTR_STATUS, intr), fmt);

	/*
	 * Name it only when exactly one format bit is set; anything else is a
	 * number the reader should look at rather than a label to trust.
	 */
	if (hweight32(fmt) == 1 && __ffs(fmt) < ARRAY_SIZE(duos_csi_decode_fmt))
		len += sysfs_emit_at(buf, len, " (%s)",
				     duos_csi_decode_fmt[__ffs(fmt)]);

	len += sysfs_emit_at(buf, len,
			     "\necc_no_error %u ecc_corrected %u ecc_error %u\n"
			     "crc_error %u wc_error %u fifo_full %u\n",
			     !!(status & CSI_ECC_NO_ERROR),
			     !!(status & CSI_ECC_CORRECTED_ERROR),
			     !!(status & CSI_ECC_ERROR),
			     !!(status & CSI_CRC_ERROR),
			     !!(status & CSI_WC_ERROR),
			     !!(status & CSI_FIFO_FULL));

	return len;
}
static DEVICE_ATTR_RO(csi_status);

static ssize_t clear_errors_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t len)
{
	struct duos_csi *csi = dev_get_drvdata(dev);

	guard(mutex)(&csi->lock);
	duos_csi_clear_errors(csi);

	return len;
}
static DEVICE_ATTR_WO(clear_errors);

static struct attribute *duos_csi_attrs[] = {
	&dev_attr_sensor.attr,
	&dev_attr_stream.attr,
	&dev_attr_phy_state.attr,
	&dev_attr_csi_status.attr,
	&dev_attr_clear_errors.attr,
	NULL,
};
ATTRIBUTE_GROUPS(duos_csi);

/*
 * The VIP subsystem's clocks. Without the AXI and config-register clocks the
 * MAC and PHY windows do not respond at all, so these are taken in probe and
 * held for the life of the driver rather than gated around streaming - stage 1
 * is a measurement tool, and a register read returning zero because a clock is
 * off would be indistinguishable from a link that is not working.
 */
static const char * const duos_csi_clk_names[] = {
	"axi", "cfg_reg", "csi_mac0", "csi0_rx", "pad_vi", "vip_sys2",
};

static int duos_csi_get_resources(struct platform_device *pdev,
				  struct duos_csi *csi)
{
	struct device *dev = &pdev->dev;
	int ret, i;

	csi->mac = devm_platform_ioremap_resource_byname(pdev, "csi_mac0");
	if (IS_ERR(csi->mac))
		return PTR_ERR(csi->mac);

	csi->wrap = devm_platform_ioremap_resource_byname(pdev, "csi_wrap0");
	if (IS_ERR(csi->wrap))
		return PTR_ERR(csi->wrap);

	csi->vip_sys = devm_platform_ioremap_resource_byname(pdev, "vip_sys");
	if (IS_ERR(csi->vip_sys))
		return PTR_ERR(csi->vip_sys);

	csi->num_clks = ARRAY_SIZE(duos_csi_clk_names);
	csi->clks = devm_kcalloc(dev, csi->num_clks, sizeof(*csi->clks),
				 GFP_KERNEL);
	if (!csi->clks)
		return -ENOMEM;

	for (i = 0; i < csi->num_clks; i++)
		csi->clks[i].id = duos_csi_clk_names[i];

	ret = devm_clk_bulk_get(dev, csi->num_clks, csi->clks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get clocks\n");

	csi->phy_reset = devm_reset_control_get_exclusive(dev, "phy");
	if (IS_ERR(csi->phy_reset))
		return dev_err_probe(dev, PTR_ERR(csi->phy_reset),
				     "failed to get phy reset\n");

	csi->phy_apb_reset = devm_reset_control_get_exclusive(dev, "phy-apb");
	if (IS_ERR(csi->phy_apb_reset))
		return dev_err_probe(dev, PTR_ERR(csi->phy_apb_reset),
				     "failed to get phy apb reset\n");

	return 0;
}

static int duos_csi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct v4l2_async_connection *asc;
	struct fwnode_handle *ep;
	struct duos_csi *csi;
	int ret;

	csi = devm_kzalloc(dev, sizeof(*csi), GFP_KERNEL);
	if (!csi)
		return -ENOMEM;

	ret = devm_mutex_init(dev, &csi->lock);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, csi);

	ret = duos_csi_get_resources(pdev, csi);
	if (ret)
		return ret;

	ret = clk_bulk_prepare_enable(csi->num_clks, csi->clks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable clocks\n");

	/*
	 * Leave the hardware quiet until something asks for a stream, but do
	 * reset it once here: the vendor bootloader may have left the MAC
	 * configured, and a half-configured MAC reports plausible nonsense.
	 */
	duos_csi_reset(csi);
	duos_csi_link_down(csi);

	/*
	 * As in the stub, none of the V4L2 objects can be devm-managed: the
	 * notifier and the v4l2_device have to come down in a defined order
	 * relative to each other and before the struct they live in is freed.
	 */
	ret = v4l2_device_register(dev, &csi->v4l2_dev);
	if (ret) {
		dev_err_probe(dev, ret, "failed to register v4l2 device\n");
		goto err_clk_disable;
	}

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(dev), 0, 0,
					     FWNODE_GRAPH_ENDPOINT_NEXT);
	if (!ep) {
		ret = dev_err_probe(dev, -EINVAL, "missing endpoint node\n");
		goto err_v4l2_unregister;
	}

	v4l2_async_nf_init(&csi->notifier, &csi->v4l2_dev);
	csi->notifier.ops = &duos_csi_notifier_ops;

	asc = v4l2_async_nf_add_fwnode_remote(&csi->notifier, ep,
					      struct v4l2_async_connection);
	fwnode_handle_put(ep);
	if (IS_ERR(asc)) {
		ret = dev_err_probe(dev, PTR_ERR(asc),
				    "failed to add async connection\n");
		goto err_nf_cleanup;
	}

	ret = v4l2_async_nf_register(&csi->notifier);
	if (ret) {
		dev_err_probe(dev, ret, "failed to register notifier\n");
		goto err_nf_cleanup;
	}

	return 0;

err_nf_cleanup:
	v4l2_async_nf_cleanup(&csi->notifier);
err_v4l2_unregister:
	v4l2_device_unregister(&csi->v4l2_dev);
err_clk_disable:
	clk_bulk_disable_unprepare(csi->num_clks, csi->clks);

	return ret;
}

static void duos_csi_remove(struct platform_device *pdev)
{
	struct duos_csi *csi = platform_get_drvdata(pdev);

	scoped_guard(mutex, &csi->lock) {
		if (csi->streaming) {
			if (csi->sensor)
				v4l2_subdev_call(csi->sensor, video, s_stream, 0);
			duos_csi_link_down(csi);
		}
		csi->streaming = false;
	}

	v4l2_async_nf_unregister(&csi->notifier);
	v4l2_async_nf_cleanup(&csi->notifier);
	v4l2_device_unregister(&csi->v4l2_dev);

	clk_bulk_disable_unprepare(csi->num_clks, csi->clks);
}

static const struct of_device_id duos_csi_of_match[] = {
	{ .compatible = "milkv,duos-csi" },
	{ }
};
MODULE_DEVICE_TABLE(of, duos_csi_of_match);

static struct platform_driver duos_csi_driver = {
	.driver = {
		.name = "duos-csi",
		.of_match_table = duos_csi_of_match,
		.dev_groups = duos_csi_groups,
	},
	.probe = duos_csi_probe,
	.remove = duos_csi_remove,
};
module_platform_driver(duos_csi_driver);

MODULE_DESCRIPTION("SG2000 CSI-2 receiver bring-up for Milk-V Duo S");
MODULE_LICENSE("GPL");
