#!/bin/sh
# Read the GC2083's interesting registers over i2c.
#
# The companion to phy-dump.sh, and the half that matters more right now. Run it
# on the vendor 5.10 stack with the camera streaming to get the register values
# of a sensor that is demonstrably transmitting, then run it on ours and diff.
# Anything that differs in the PLL or MIPI blocks is a candidate for why our
# sensor drives LP transitions but never a coherent high-speed clock.
#
#	./sensor-dump.sh > /tmp/sensor-vendor.txt   # vendor stack, streaming
#	./sensor-dump.sh > /tmp/sensor-ours.txt     # our stack, streaming
#	diff /tmp/sensor-vendor.txt /tmp/sensor-ours.txt
#
# Needs i2ctransfer from i2c-tools and the sensor powered. On our stack that
# means streaming, or at minimum runtime PM forced on:
#
#	echo on > /sys/bus/i2c/devices/3-0037/power/control
#
# Reads are one byte at a time on purpose: the address does not auto-increment
# in the 0x0dxx page, so a multi-byte read there returns the first byte
# repeatedly. It does auto-increment at 0x03f0, which is why the chip ID can be
# read in one go - but there is no reason to rely on that here.
#
# It only reads. Nothing here writes to the sensor.

BUS=${BUS:-3}
ADDR=${ADDR:-0x37}

if ! command -v i2ctransfer >/dev/null 2>&1; then
	echo "i2ctransfer not found; install i2c-tools" >&2
	exit 1
fi

rd() {
	# $1 label, $2 high addr byte, $3 low addr byte
	v=$(i2ctransfer -f -y "$BUS" w2@"$ADDR" "$2" "$3" r1 2>/dev/null)
	printf '%-26s %s%s = %s\n' "$1" "$2" "${3#0x}" "${v:-ERR}"
}

echo "# chip id"
rd chip_id_hi 0x03 0xf0
rd chip_id_lo 0x03 0xf1

echo
echo "# system and PLL - the block our stream-on used to disturb"
rd sys_03f2 0x03 0xf2
rd sys_03f3 0x03 0xf3
rd sys_03f4 0x03 0xf4
rd sys_03f5 0x03 0xf5
rd sys_03f6 0x03 0xf6
rd pll_ctrl_03f7 0x03 0xf7
rd sys_03f8 0x03 0xf8
rd pll_mode_03f9 0x03 0xf9
rd pwr_ctrl_03fc 0x03 0xfc
rd reset_03fe 0x03 0xfe

echo
echo "# MIPI block. The mode table's last write is 0x023e = 0x99, and on our"
echo "# stack it reads back 0x00 - which is either write-only or not landing."
rd mipi_0201 0x02 0x01
rd mipi_0202 0x02 0x02
rd mipi_0203 0x02 0x03
rd mipi_0204 0x02 0x04
rd mipi_0212 0x02 0x12
rd mipi_0213 0x02 0x13
rd mipi_0215 0x02 0x15
rd mipi_0229 0x02 0x29
rd mipi_0237 0x02 0x37
rd mipi_en_023e 0x02 0x3e

echo
echo "# the register the vendor only writes on resume-from-standby"
rd mipi_en_003e 0x00 0x3e

echo
echo "# timing and exposure, as a sanity check that the mode table is present"
rd hts_0d05 0x0d 0x05
rd hts_0d06 0x0d 0x06
rd vts_0d41 0x0d 0x41
rd vts_0d42 0x0d 0x42
rd exp_0d03 0x0d 0x03
rd exp_0d04 0x0d 0x04
rd flip_0d15 0x0d 0x15
rd misc_00d7 0x00 0xd7
