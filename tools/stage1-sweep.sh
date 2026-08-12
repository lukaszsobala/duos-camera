#!/bin/sh
# Sweep the CSI-2 receiver's PHY tuning knobs and report what each one does.
#
# One reboot's worth of round trip covers every plausible setting instead of one.
# For each combination it reloads duos-csi.ko, streams, and prints the pad states
# and the PHY status. Run it from the duos-camera directory:
#
#	tools/stage1-sweep.sh 2>&1 | tee /tmp/sweep.txt
#
# It stops early and says so if it ever sees the clock lane reach high speed,
# because at that point the sweep is over and the settings that did it are the
# answer.
#
# Expects the fixed gc2083 already loaded (stream_on_restart off, which is the
# default). It reloads only duos-csi.ko.

MOD=duos-csi/duos-csi.ko
[ -f "$MOD" ] || MOD=./duos-csi.ko
if ! [ -f "$MOD" ]; then
	echo "cannot find duos-csi.ko; run this from the duos-camera directory" >&2
	exit 1
fi

try() {
	# $@ = module parameters
	D=$(echo /sys/bus/platform/drivers/duos-csi/*csi-rx*)
	if [ -d "$D" ]; then
		echo 0 > "$D"/stream 2>/dev/null
	fi
	rmmod duos_csi 2>/dev/null
	sleep 1

	if ! insmod "$MOD" "$@" 2>/dev/null; then
		echo "== $* : insmod failed"
		return
	fi

	D=$(echo /sys/bus/platform/drivers/duos-csi/*csi-rx*)
	if ! [ -d "$D" ]; then
		echo "== $* : device did not appear"
		return
	fi
	if [ "$(cat "$D"/sensor)" = "" ]; then
		echo "== $* : sensor did not bind"
		return
	fi

	echo 1 > "$D"/stream 2>/dev/null
	echo 1 > "$D"/clear_errors 2>/dev/null
	sleep 1

	echo "== $*"
	sed 's/^/   /' "$D"/phy_state
	sed 's/^/   /' "$D"/lane_data
	grep -E 'decode_format|ecc_no_error' "$D"/csi_status | sed 's/^/   /'
	echo

	if grep -q 'clk hs 1' "$D"/phy_state; then
		echo "*** clk hs 1 with: $* ***"
		echo "*** stopping the sweep; this is the answer ***"
		exit 0
	fi
}

echo "### baseline (vendor defaults)"
try

echo "### T_HS_SETTLE sweep - when the HS receiver starts sampling"
for v in 2 4 8 16 24 32 48 64 96 128; do
	try hs_settle=$v
done

echo "### low-band receive AFE - plausible at this link rate (371Mbps)"
for v in 0 1 2 3; do
	try low_band=$v
done

echo "### low band combined with a couple of settle values"
for v in 8 16 32; do
	try low_band=1 hs_settle=$v
	try low_band=3 hs_settle=$v
done

echo "### deskew phases"
for v in 0 1 2 4 8; do
	try deskew_data=$v
done

echo "### P/N swap - unlikely, both pads read LP-11 at idle"
try clk_pn_swap=1
try data_pn_swap=1
try clk_pn_swap=1 data_pn_swap=1

echo "### done, nothing reached clk hs 1"
