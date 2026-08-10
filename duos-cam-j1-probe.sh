#!/bin/sh
# Milestone 0 for the Milk-V Duo S CAM-GC2083 on J1.
#
# Question this answers: with mainline's clk/pinctrl/gpio drivers and nothing
# else, does the sensor show up on I2C? If yes, the board side of the camera
# plan is grounded and only the SoC CSI/ISP driver is missing. If no, we find
# out before anyone writes a driver.
#
# Expected wiring (from the vendor board files, duo-buildroot-sdk-v2):
#   J1 I2C     : i2c-3, GC2083 at 0x37
#   J1 MCLK    : CAM_MCLK0, 24MHz, mainline clock id CLK_CAM0 = 147
#   J1 reset   : pad CAM_RST0 muxed to XGPIOA_2, active low
#
# Run as root on the board. Safe to re-run.

set -u

say() { printf '\n=== %s ===\n' "$1"; }

say "kernel"
uname -a

say "pinmux (U-Boot should already have muxed these; kernel does not touch them)"
if [ -d /sys/kernel/debug/pinctrl ]; then
	grep -i -E 'CAM_MCLK0|CAM_RST0|IIC3' /sys/kernel/debug/pinctrl/*/pinmux-pins 2>/dev/null \
		|| echo "no CAM_/IIC3 lines in pinmux-pins - pads are whatever U-Boot left them as"
	echo "--- pin claims ---"
	grep -i -E 'CAM_MCLK0|CAM_RST0|IIC3' /sys/kernel/debug/pinctrl/*/pins 2>/dev/null | head
else
	echo "debugfs not mounted: mount -t debugfs none /sys/kernel/debug"
fi

say "camera clocks before"
grep -i -E 'clk_cam0|cam0pll|mipimpll' /sys/kernel/debug/clk/clk_summary 2>/dev/null \
	|| echo "no clk_summary"

say "i2c buses"
i2cdetect -l 2>/dev/null || echo "install i2c-tools"

# --- release the sensor from reset -------------------------------------------
# CAM_RST0 is active low, so the sensor runs when GPIOA_2 is driven high.
# sysfs rather than libgpiod: the gpioset syntax differs between libgpiod v1
# and v2, and a sysfs export stays asserted after this script exits, which is
# what we want for the i2cdetect that follows.
say "releasing GPIOA_2 (sensor reset)"
BASE=""
for d in /sys/class/gpio/gpiochip*; do
	[ -e "$d/label" ] || continue
	case "$(cat "$d/label")" in
		*3020000*) BASE="$(cat "$d/base")" ;;
	esac
done

if [ -z "$BASE" ]; then
	echo "could not find the gpio0 (3020000) controller; labels seen:"
	cat /sys/class/gpio/gpiochip*/label 2>/dev/null
else
	N=$((BASE + 2))
	echo "gpio0 base=$BASE, GPIOA_2 = global $N"
	[ -d "/sys/class/gpio/gpio$N" ] || echo "$N" > /sys/class/gpio/export
	echo out > "/sys/class/gpio/gpio$N/direction"
	echo 0   > "/sys/class/gpio/gpio$N/value"   # assert reset
	sleep 0.1
	echo 1   > "/sys/class/gpio/gpio$N/value"   # release
	sleep 0.2
	echo "value now: $(cat /sys/class/gpio/gpio$N/value)"
fi

# --- optional: bring up MCLK --------------------------------------------------
# Nothing in a mainline kernel enables CLK_CAM0, and a Galaxycore sensor very
# likely needs MCLK before its I2C block answers. duos-mclk.ko does exactly one
# thing: clk_set_rate(CLK_CAM0, 24MHz) + clk_prepare_enable.
say "MCLK"
if [ -f ./duos-mclk.ko ] && ! grep -q '^duos_mclk ' /proc/modules; then
	insmod ./duos-mclk.ko && echo "duos-mclk loaded"
elif grep -q '^duos_mclk ' /proc/modules; then
	echo "duos-mclk already loaded"
else
	echo "duos-mclk.ko not present - running without MCLK."
	echo "If the sensor does not answer below, that is the first thing to fix."
fi
sleep 0.2
grep -i 'clk_cam0' /sys/kernel/debug/clk/clk_summary 2>/dev/null

say "scanning i2c-3 for the GC2083 (expect 0x37)"
i2cdetect -y -r 3

say "reading the GC2083 chip id (0x03f0/0x03f1, expect 0x20 0x83)"
# 16-bit register address, so i2cget's plain modes do not apply: write the two
# address bytes, then read one byte back.
if command -v i2ctransfer >/dev/null 2>&1; then
	HI=$(i2ctransfer -y 3 w2@0x37 0x03 0xf0 r1 2>&1)
	LO=$(i2ctransfer -y 3 w2@0x37 0x03 0xf1 r1 2>&1)
	echo "chip_id_high = $HI"
	echo "chip_id_low  = $LO"
else
	echo "install i2c-tools for i2ctransfer"
fi
