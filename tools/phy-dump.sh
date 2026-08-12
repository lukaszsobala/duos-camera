#!/bin/sh
# Dump the SG2000 CSI-2 receiver registers through /dev/mem.
#
# The point of this script is to be runnable on *either* stack: on the vendor
# 5.10 image, where the camera demonstrably works, and on our mainline image,
# where it does not. Run it on both with the sensor streaming and diff the
# output. That replaces guessing at PHY tuning with a list of registers that
# differ from a known-good configuration.
#
# Needs the devmem helper next to it:
#
#	gcc -O2 -Wall -o devmem devmem.c
#	./phy-dump.sh > /tmp/mine.txt
#
# It only reads. Nothing here writes to a register.
#
# On the vendor image, start the camera first (any of their sample apps that
# streams the sensor will do), confirm frames are actually moving, and only then
# run this - a vendor stack sitting idle would give a baseline as unhelpful as
# ours.

DEVMEM=${DEVMEM:-./devmem}

if ! [ -x "$DEVMEM" ]; then
	echo "no devmem helper at $DEVMEM; build it with:" >&2
	echo "  gcc -O2 -Wall -o devmem devmem.c" >&2
	exit 1
fi

dump() {
	# $1 label, $2 address
	printf '%-22s %s  %s\n' "$1" "$2" "$($DEVMEM "$2" | tail -1)"
}

echo "# csi_wrap0 @ 0x0a0d0000 - D-PHY"
dump phy_top_00_power      0x0a0d0000
dump phy_top_04_clksel     0x0a0d0004
dump phy_top_30_mode       0x0a0d0030
dump phy_top_70_d0_d3      0x0a0d0070
dump phy_top_74_d4_d5      0x0a0d0074
dump phy_top_7c_lpout      0x0a0d007c
dump phy_top_80_deskew_en  0x0a0d0080
dump phy_top_84_deskew_lo  0x0a0d0084
dump phy_top_88_deskew_hi  0x0a0d0088
dump phy_4l_00_sensor_mode 0x0a0d0300
dump phy_4l_04_data_sel    0x0a0d0304
dump phy_4l_08_clk_sel     0x0a0d0308
dump phy_4l_0c_lane_en     0x0a0d030c
dump phy_4l_10_hs_settle   0x0a0d0310
dump phy_4l_dbg90_state    0x0a0d0390
dump phy_4l_dbg94_state    0x0a0d0394

echo
echo "# csi_mac0 @ 0x0a0c2000 - CSI-2 MAC"
dump mac_sensor_00         0x0a0c2000
dump mac_csi_00_lane_mode  0x0a0c2400
dump mac_csi_04_intr       0x0a0c2404
dump mac_csi_18_vc_map     0x0a0c2418
dump mac_csi_40_status     0x0a0c2440
dump mac_csi_60_intr_sts   0x0a0c2460
dump mac_csi_70_vs_gen     0x0a0c2470

echo
echo "# vip_sys @ 0x0a0c8000 - subsystem control"
dump vip_resets            0x0a0c8000
dump vip_axi_sw            0x0a0c8010
dump vip_clk_lp            0x0a0c8014
dump vip_clk_ctrl0         0x0a0c8018
dump vip_clk_ctrl1         0x0a0c801c

echo
echo "# csibdg front-end, stage 2 reference"
# Only the first two members of REG_ISP_CSI_BDG_T are at offsets that can be
# read straight off the struct declaration; the rest is a dense run of 32-bit
# registers whose names do not encode their offsets, so they are not guessed at
# here.
dump csibdg0_top_ctrl      0x0a000800
dump csibdg0_intr_ctrl     0x0a000804
