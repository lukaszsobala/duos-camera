#!/bin/sh
# Capture raw frames from the GC2083 on J1 (run on the board).
#   [GAIN=n] [OUT=file] j1-stream.sh [bridge test pattern 0-4] [frames]
# GAIN is the sensor analogue_gain control: 64 = 1x (the default), 1024 = 16x.
# frames = 0 streams until Ctrl-C and writes no file unless OUT is set: use
# it to watch the CPU load in top while the camera runs.
# Pattern 0 = the sensor. 1-4 = the VI bridge's own generator, which
# starts neither the receiver nor the sensor. Frames land in /root/cap.raw
# (packed RAW12, back to back), or OUT. Adapted from tools/stream.sh in
# https://github.com/leafpmpmp/sg200x-camera-dkms.
W=1920
H=1080
TP=${1:-0}
N=${2:-8}
M="media-ctl -d ${MEDIA:-/dev/media0}"

SNAME=$($M -p | sed -n 's/.*entity [0-9]*: \(gc2083 [0-9]*-0037\).*/\1/p' | head -1)
[ -n "$SNAME" ] || { echo "no gc2083 entity in $M -p"; exit 1; }
VIDEO=$($M -e cv1800-vi-raw)
BRIDGE=$($M -e cv1800-vi-bridge)
SENSOR=$($M -e "$SNAME")
[ -n "$GAIN" ] && v4l2-ctl -d "$SENSOR" -c analogue_gain="$GAIN"

$M -V "\"$SNAME\":0 [fmt:SRGGB10_1X10/${W}x${H}]" || exit 1
CODE=$($M --get-v4l2 "\"$SNAME\":0" | sed -n 's/.*fmt:\([A-Z0-9_]*\)\/.*/\1/p')
$M -V "\"cv1800-csi2\":0 [fmt:$CODE/${W}x${H}]" || exit 1
$M -V "\"cv1800-vi-bridge\":0 [fmt:$CODE/${W}x${H}]" || exit 1
case $CODE in
	SBGGR*) PIX=pBCC ;; SGBRG*) PIX=pGCC ;; SGRBG*) PIX=pgCC ;; *) PIX=pRCC ;;
esac
v4l2-ctl -d "$BRIDGE" -c test_pattern="$TP"
v4l2-ctl -d "$VIDEO" --set-fmt-video width=$W,height=$H,pixelformat=$PIX || exit 1
echo "$SNAME $CODE ${W}x${H} -> $PIX, bridge pattern $TP"

if [ "$N" = 0 ]; then
	echo "streaming until Ctrl-C; v4l2-ctl prints the frame rate"
	exec v4l2-ctl -d "$VIDEO" --stream-mmap=4 ${OUT:+--stream-to="$OUT"}
fi
OUT=${OUT:-/root/cap.raw}

timeout 20 v4l2-ctl -d "$VIDEO" --stream-mmap=4 --stream-skip=3 \
	--stream-count="$N" --stream-to="$OUT" --verbose &
sleep 3
cat /sys/kernel/debug/a0c2000.csi/status
cat /sys/kernel/debug/a000000.video-input/status
wait
ls -l "$OUT"
