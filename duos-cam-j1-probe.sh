#!/bin/sh
# Milestone 0 for the Milk-V Duo S CAM-GC2083 on J1.
#
# Question this answers: with mainline's clk/pinctrl/gpio drivers and nothing
# else, does the sensor show up on I2C? If yes, the board side of the camera
# plan is grounded and only the SoC CSI/ISP driver is missing. If no, this
# narrows down which of the board-side preconditions is not being met.
#
# Expected wiring (from the vendor board files, duo-buildroot-sdk-v2):
#   J1 I2C     : i2c-3, GC2083 at 0x37
#   J1 MCLK    : CAM_MCLK0, 24MHz, mainline clock id CLK_CAM0 = 147
#   J1 reset   : pad CAM_RST0 muxed to XGPIOA_2, active low
#
# Run as root on the board. Safe to re-run. It restores every register and pad
# it touches except the ones it is deliberately leaving asserted - see the end.

set -u

say() { printf '\n=== %s ===\n' "$1"; }
warn() { printf '!! %s\n' "$1"; }

# --- pinctrl register access --------------------------------------------------
# Everything here lives in the pinctrl SYS block at 0x03001000. The fmux
# (function select) registers are at +0x000.., the pad config registers at
# +0xb00.., both taken from the pin tables in drivers/pinctrl/sophgo/.
#
#	pad        fmux        conf
#	CAM_MCLK0  0x03001000  0x03001b00
#	CAM_RST0   0x03001008  0x03001b08
#	IIC3_SCL   0x03001014  0x03001b14
#	IIC3_SDA   0x03001018  0x03001b18
#
# Reachable from userspace because these kernels have CONFIG_DEVMEM=y with
# STRICT_DEVMEM unset. busybox is not always built with the devmem applet, so
# tools/devmem.c is the fallback: gcc -O2 -o devmem tools/devmem.c.
FMUX_SCL=0x03001014
FMUX_SDA=0x03001018
CONF_SCL=0x03001b14
CONF_SDA=0x03001b18

# conf bits, from PIN_IO_* in drivers/pinctrl/sophgo/pinctrl-cv18xx.c:
#   BIT(2) pull-up, BIT(3) pull-down, GENMASK(7,5) drive strength.
# 0x48 is the reset default the Duo S boots with: pull-down on, pull-up off,
# drive 2. 0x40 is the same drive strength with no bias at all, which is what
# the idle-level read below needs and the only value this script ever writes -
# it puts back whatever it found afterwards.
CONF_NOBIAS=0x40

MEMTOOL=""
if command -v busybox >/dev/null 2>&1 && busybox devmem "$FMUX_SCL" 32 >/dev/null 2>&1; then
	MEMTOOL=busybox
elif [ -x ./devmem ]; then
	MEMTOOL=local
fi

memw() {
	case "$MEMTOOL" in
	busybox) busybox devmem "$1" 32 "$2" ;;
	local)   ./devmem "$1" "$2" >/dev/null ;;
	*)       return 1 ;;
	esac
}

memr() {
	case "$MEMTOOL" in
	busybox) busybox devmem "$1" 32 ;;
	local)   ./devmem "$1" | cut -d' ' -f3 ;;
	*)       return 1 ;;
	esac
}

# --- gpio ---------------------------------------------------------------------
# sysfs rather than libgpiod: the gpioset syntax differs between libgpiod v1 and
# v2, and a sysfs export stays asserted after this script exits, which is what
# we want for poking at the sensor by hand afterwards.
gpio_export() {
	[ -d "/sys/class/gpio/gpio$1" ] || echo "$1" > /sys/class/gpio/export 2>/dev/null
	[ -d "/sys/class/gpio/gpio$1" ]
}

gpio_out() {
	gpio_export "$1" || return 1
	echo out > "/sys/class/gpio/gpio$1/direction" || return 1
	echo "$2" > "/sys/class/gpio/gpio$1/value" || return 1
}

gpio_in() {
	gpio_export "$1" || return 1
	echo in > "/sys/class/gpio/gpio$1/direction" || return 1
	cat "/sys/class/gpio/gpio$1/value"
}

say "kernel"
uname -a

say "pinmux (U-Boot muxes these; the kernel does not touch them)"
# The mux values that matter, checked against the vendor's own fmux table in
# u-boot-2021.10/board/cvitek/cv181x/cv181x_pinlist_swconfig.h:
#   CAM_MCLK0 0 = CAM_MCLK0, IIC3_SCL 0 = IIC3_SCL, IIC3_SDA 0 = IIC3_SDA,
#   CAM_RST0  3 = XGPIOA_2.
# UNCLAIMED in pinmux-pins only means no driver took a pinctrl reference. It
# says nothing about the register, which is what the "pin claims" block shows.
if [ -d /sys/kernel/debug/pinctrl ]; then
	grep -i -E 'CAM_MCLK0|CAM_RST0|IIC3' /sys/kernel/debug/pinctrl/*/pinmux-pins 2>/dev/null \
		|| echo "no CAM_/IIC3 lines in pinmux-pins - pads are whatever U-Boot left them as"
	echo "--- pin claims ---"
	grep -i -E 'CAM_MCLK0|CAM_RST0|IIC3' /sys/kernel/debug/pinctrl/*/pins 2>/dev/null | head
else
	warn "debugfs not mounted: mount -t debugfs none /sys/kernel/debug"
fi

say "i2c buses"
i2cdetect -l 2>/dev/null || warn "install i2c-tools"

# --- find the gpio controller -------------------------------------------------
say "gpio controller"
BASE=""
for d in /sys/class/gpio/gpiochip*; do
	[ -e "$d/label" ] || continue
	case "$(cat "$d/label")" in
		*3020000*) BASE="$(cat "$d/base")" ;;
	esac
done

if [ -z "$BASE" ]; then
	warn "could not find the gpio0 (3020000) controller; labels seen:"
	cat /sys/class/gpio/gpiochip*/label 2>/dev/null
	RST=""
else
	RST=$((BASE + 2))	# CAM_RST0 -> XGPIOA_2
	SCL_GPIO=$((BASE + 5))	# IIC3_SCL -> XGPIOA_5, mux 3
	SDA_GPIO=$((BASE + 6))	# IIC3_SDA -> XGPIOA_6, mux 3
	echo "gpio0 base=$BASE, GPIOA_2 (reset) = $RST"
fi

# --- MCLK, before anything touches reset --------------------------------------
# Order matters and this is the vendor's: SAMPLE_COMM_VI_StartMIPI() enables the
# sensor clock, waits, and only then releases reset. A GalaxyCore part latches
# at the reset edge and its I2C block is clocked from MCLK, so releasing reset
# into a dead pad is not the same experiment.
#
# duos-mclk.ko is what the vendor's cif driver does on the COMMON_CLK path,
# reduced to its two lines: clk_set_rate(CLK_CAM0, 24MHz) + clk_prepare_enable.
# See _cif_enable_snsr_clk() in osdrv/interdrv/cif/chip/cv181x/cif.c. 24MHz off
# cam0pll lands on 23.76MHz here and does the same for the vendor - not a fault.
say "MCLK"
if grep -q '^duos_mclk ' /proc/modules; then
	echo "duos-mclk already loaded"
elif [ -f ./duos-mclk.ko ]; then
	insmod ./duos-mclk.ko && echo "duos-mclk loaded"
elif [ -f ./duos-mclk/duos-mclk.ko ]; then
	insmod ./duos-mclk/duos-mclk.ko && echo "duos-mclk loaded"
else
	warn "duos-mclk.ko not found - running WITHOUT MCLK, which the sensor needs."
fi
sleep 0.2
grep -i 'clk_cam0 ' /sys/kernel/debug/clk/clk_summary 2>/dev/null || echo "no clk_summary"

# --- is the bus electrically alive? -------------------------------------------
# Borrow the pads as GPIOs, with the internal bias off entirely, and read what
# level the bus rests at. Everything is put back before the scan.
#
# The pads' reset default is 0x48 - pull-DOWN, no pull-up - because U-Boot's
# PINMUX_CONFIG() writes only the fmux register and mainline gives &i2c3 no
# pinctrl-0 to set the rest. That looks alarming and does not matter: the Duo S
# carries its own SDA/SCL pull-ups, which win. The proof is in the error code -
# a NACK arrives as -EREMOTEIO, and i2c-designware-common.c only gets there
# after sampling SDA released high at the ACK bit. A line actually held down
# aborts with -EAGAIN from ARB_LOST instead. So do not "fix" the bias; there is
# nothing wrong with it.
#
# Note what a high reading does and does not mean. Those board pull-ups are on
# the board, so the lines read high with no camera attached at all. High proves
# the bus is not shorted or stuck. It does not prove the module is powered, and
# a reversed FPC looks exactly like an empty connector here.
say "i2c-3 bus idle level"
if [ -z "$MEMTOOL" ]; then
	warn "no devmem: install busybox, or gcc -O2 -o devmem tools/devmem.c"
	warn "skipping the idle-level read"
elif [ -z "$BASE" ]; then
	warn "no gpio controller, skipping the idle-level read"
else
	WAS_SCL="$(memr "$CONF_SCL")"
	WAS_SDA="$(memr "$CONF_SDA")"
	echo "conf: SCL $WAS_SCL SDA $WAS_SDA  (0x48 = the pull-down default, and fine)"

	memw "$CONF_SCL" "$CONF_NOBIAS"
	memw "$CONF_SDA" "$CONF_NOBIAS"
	memw "$FMUX_SCL" 3
	memw "$FMUX_SDA" 3
	echo "with no bias, as GPIO inputs: SCL=$(gpio_in "$SCL_GPIO") SDA=$(gpio_in "$SDA_GPIO")  (want 1 1)"
	echo "$SCL_GPIO" > /sys/class/gpio/unexport 2>/dev/null
	echo "$SDA_GPIO" > /sys/class/gpio/unexport 2>/dev/null
	memw "$FMUX_SCL" 0
	memw "$FMUX_SDA" 0

	memw "$CONF_SCL" "$WAS_SCL"
	memw "$CONF_SDA" "$WAS_SDA"
fi

# --- scan, with reset released and again with it asserted ---------------------
# The sweep is here because a polarity or wiring surprise on CAM_RST0 would look
# exactly like a dead sensor. Every value written is read back before the scan:
# an unexported gpio silently swallowing the write is not a null result, it is
# no result, and it is easy to miss in a wall of identical tables.
say "scanning i2c-3 for the GC2083 (expect 0x37), reset released then asserted"
if [ -z "$RST" ]; then
	warn "no reset gpio - scanning once, at whatever state the pad is in"
	i2cdetect -y -r 3
else
	for v in 1 0 1; do
		if ! gpio_out "$RST" "$v"; then
			warn "could not drive gpio$RST - scan below is meaningless"
		fi
		sleep 0.3
		printf -- '--- GPIOA_2 asked %s, reads %s (1 = out of reset) ---\n' \
			"$v" "$(cat "/sys/class/gpio/gpio$RST/value" 2>/dev/null || echo '?')"
		i2cdetect -y -r 3 | grep -v '^     0'
	done
fi

# --- last resort: is it on some other bus? ------------------------------------
# Everything above trusts one number: bus_id = 3, from the vendor's own
# sensor_cfg_GC2083.ini, backed by U-Boot muxing IIC3 for camera0. If the module
# is in J2 rather than J1, or this board is wired differently from the vendor
# board files, the sensor is on a bus nobody has looked at. Cheap to rule out.
say "scanning the other i2c buses (J2/OV5647 would be i2c-2 at 0x36)"
for b in 1 2 4; do
	[ -e "/dev/i2c-$b" ] || continue
	printf -- '--- i2c-%s ---\n' "$b"
	i2cdetect -y -r "$b" | grep -v '^     0'
done

say "reading the GC2083 chip id (0x03f0/0x03f1, expect 0x20 0x83)"
# 16-bit register address, so i2cget's plain modes do not apply: write the two
# address bytes, then read one byte back.
if command -v i2ctransfer >/dev/null 2>&1; then
	echo "chip_id_high = $(i2ctransfer -y 3 w2@0x37 0x03 0xf0 r1 2>&1)"
	echo "chip_id_low  = $(i2ctransfer -y 3 w2@0x37 0x03 0xf1 r1 2>&1)"
else
	warn "install i2c-tools for i2ctransfer"
fi

say "left behind"
# Deliberately not undone, so the sensor can be poked at by hand afterwards:
echo "  duos-mclk.ko loaded, CLK_CAM0 running"
[ -n "$RST" ] && echo "  gpio$RST exported and high (sensor out of reset)"
echo "  every pinctrl register touched has been put back"
echo "  rmmod duos_mclk; echo $RST > /sys/class/gpio/unexport   # or just reboot"
