#!/bin/sh
#
# Turn the Milk-V Duo S blue LED off (or on).
#
# The blue LED is the only LED on this board software can touch. It hangs off
# XGPIOA[29] - pad IIC0_SDA, mux function 3 - through a MOSFET, and Milk-V's own
# blink.sh drives it as /sys/class/gpio/gpio509 (their kernel's numbering, not
# ours). The red LED is a power indicator wired straight to the 3.3V rail; no
# amount of software turns that one off.
#
# Nothing in mainline claims XGPIOA[29], so under Armbian the pin comes out of
# U-Boot muxed as a GPIO but never driven, and the pad's default pull leaves the
# LED lit. Driving it low turns it off.
#
# Usage:
#     ./duos-led.sh            # off
#     ./duos-led.sh off
#     ./duos-led.sh on
#     ./duos-led.sh status
#
# Run as root. The effect lasts until reboot - see README.md in this directory
# for the device tree overlay that makes it stick.
#
# Environment overrides:
#     LED_OFFSET=29        line offset within the XGPIOA bank
#     LED_ACTIVE_LOW=1     if driving low turns the LED *on* rather than off

set -eu

# porta (XGPIOA) is gpio@3020000 in the board device tree. The sysfs base is
# assigned dynamically, so match the controller by name rather than assuming it.
PORTA_MATCH=3020000
LED_OFFSET=${LED_OFFSET:-29}
LED_ACTIVE_LOW=${LED_ACTIVE_LOW:-0}

action=${1:-off}

die() {
	echo "duos-led: $*" >&2
	exit 1
}

case "$action" in
off | on | status) ;;
-h | --help | help)
	sed -n '3,26p' "$0" | sed 's/^# \{0,1\}//'
	exit 0
	;;
*) die "unknown action '$action' (expected off, on or status)" ;;
esac

[ "$(id -u)" = 0 ] || die "must run as root"

# ---------------------------------------------------------------------------
# Path 1: a gpio-leds node exists, so the LED subsystem owns the line.
# ---------------------------------------------------------------------------
#
# If the overlay from this directory is applied, XGPIOA[29] is claimed by
# leds-gpio and the sysfs GPIO interface will refuse to export it. Drive it
# through /sys/class/leds instead, which is the better interface anyway.

led_class=
for d in /sys/class/leds/*; do
	[ -d "$d" ] || continue
	case "${d##*/}" in
	*blue* | *:status | *duos*) led_class=$d; break ;;
	esac
done

if [ -n "$led_class" ]; then
	name=${led_class##*/}

	if [ "$action" = status ]; then
		trigger=$(sed -n 's/.*\[\([^]]*\)\].*/\1/p' "$led_class/trigger" 2>/dev/null || true)
		echo "LED class device: $name"
		echo "  brightness: $(cat "$led_class/brightness")" \
			"(max $(cat "$led_class/max_brightness"))"
		echo "  trigger:    ${trigger:-unknown}"
		exit 0
	fi

	# A trigger left in place would immediately overwrite whatever we write.
	if [ -w "$led_class/trigger" ]; then
		echo none > "$led_class/trigger"
	fi

	if [ "$action" = off ]; then
		echo 0 > "$led_class/brightness"
	else
		cat "$led_class/max_brightness" > "$led_class/brightness"
	fi

	echo "duos-led: $name -> $action"
	exit 0
fi

# ---------------------------------------------------------------------------
# Path 2: no LED node, so drive the GPIO line directly.
# ---------------------------------------------------------------------------

[ -d /sys/class/gpio ] || die "/sys/class/gpio missing (CONFIG_GPIO_SYSFS not set?)"

base=
for chip in /sys/class/gpio/gpiochip*; do
	[ -r "$chip/base" ] || continue
	label=$(cat "$chip/label" 2>/dev/null || true)
	dev=$(readlink -f "$chip/device" 2>/dev/null || true)
	case "$label $dev" in
	*"$PORTA_MATCH"*)
		base=$(cat "$chip/base")
		break
		;;
	esac
done

[ -n "$base" ] || die "no gpiochip matching $PORTA_MATCH (XGPIOA); is this a Duo S?"

pin=$((base + LED_OFFSET))
gpio=/sys/class/gpio/gpio$pin

if [ ! -d "$gpio" ]; then
	if ! echo "$pin" > /sys/class/gpio/export 2>/dev/null; then
		die "cannot export gpio$pin - the line is claimed by a driver;" \
			"check /sys/class/leds"
	fi
fi

if [ "$action" = status ]; then
	echo "XGPIOA[$LED_OFFSET] = gpio$pin (gpiochip base $base)"
	echo "  direction: $(cat "$gpio/direction")"
	echo "  value:     $(cat "$gpio/value")"
	exit 0
fi

# "low"/"high" set direction and level in one write, so the pin never glitches
# through the opposite state on the way to being an output.
if [ "$action" = off ]; then
	level=low
else
	level=high
fi

if [ "$LED_ACTIVE_LOW" != 0 ]; then
	case "$level" in
	low) level=high ;;
	high) level=low ;;
	esac
fi

echo "$level" > "$gpio/direction"

echo "duos-led: XGPIOA[$LED_OFFSET] (gpio$pin) driven $level -> blue LED $action"
echo "duos-led: the red LED is a power indicator on the 3.3V rail and cannot be" \
	"switched off"
