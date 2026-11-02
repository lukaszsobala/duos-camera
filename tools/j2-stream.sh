#!/bin/sh
# Capture 1920x1080 NV12 frames from the camera on J2 (IMX708 or OV5647)
# through the ISP (run on the board).
#   [GAIN=n] [OUT=file] j2-stream.sh [bridge test pattern 0-4] [frames]
# Needs overlays=camera-j2-imx708 or camera-j2-ov5647, and a cv1800-vi with
# the ISP path. Frames land in /root/cap.yuv, or OUT. GAIN is the sensor
# analogue_gain control. BLACK sets the ISP black level (default: IMX708
# 256, OV5647 64). FPS sets the frame rate (default 30).
# frames = 0 streams until Ctrl-C and writes no file unless OUT is set.
# frames = setup configures the pipeline, prints VIDEO=<ISP node> and
# SENSOR=<sensor subdev>, and stops.
# A sensor mounted upside down (rotation 180 in the overlay) is flipped
# upright. Pattern 0 = the sensor. 1-4 = the VI bridge's own generator.
W=1920
H=1080
TP=${1:-0}
N=${2:-8}
M="media-ctl -d ${MEDIA:-/dev/media0}"

# The sensor is whatever feeds the CSI-2 receiver
SNAME=$($M -p | sed -n 's/^.*<- "\([^"]*\)":0 \[ENABLED.*$/\1/p' | grep -v '^cv1800' | head -1)
[ -n "$SNAME" ] || { echo "no sensor in the media graph"; exit 1; }
SENSOR=$($M -e "$SNAME")
VIDEO=$($M -e cv1800-vi-yuv)
[ -n "$VIDEO" ] || { echo "no cv1800-vi-yuv node: this cv1800-vi has no ISP"; exit 1; }
BRIDGE=$($M -e cv1800-vi-bridge)
case $SNAME in
	imx708*) BL=256 ;; ov5647*) BL=64 ;; gc2083*) BL=252 ;; *) BL=0 ;;
esac
BLACK=${BLACK:-$BL}
fmt() { $M --get-v4l2 "\"$SNAME\":0" | sed -n "s/.*fmt:\([A-Z0-9_]*\)\/\([0-9]*\)x\([0-9]*\).*/\\$1/p"; }

# Flip a sensor mounted at 180 degrees; the Bayer order follows the flips
ROT=$(v4l2-ctl -d "$SENSOR" -C camera_sensor_rotation 2>/dev/null | sed -n 's/.*: *//p')
[ "$ROT" = 180 ] && v4l2-ctl -d "$SENSOR" -c horizontal_flip=1,vertical_flip=1
[ -n "$GAIN" ] && v4l2-ctl -d "$SENSOR" -c analogue_gain="$GAIN"

$M -V "\"$SNAME\":0 [fmt:$(fmt 1)/${W}x${H}]" || exit 1
CODE=$(fmt 1) W=$(fmt 2) H=$(fmt 3)
$M -V "\"cv1800-csi2\":0 [fmt:$CODE/${W}x${H}]" || exit 1
$M -V "\"cv1800-vi-bridge\":0 [fmt:$CODE/${W}x${H}]" || exit 1

# The frame rate, after the format: a new mode starts at its fastest rate
RATE=$(v4l2-ctl -d "$SENSOR" -C pixel_rate 2>/dev/null | sed -n 's/.*: *//p')
HB=$(v4l2-ctl -d "$SENSOR" -C horizontal_blanking 2>/dev/null | sed -n 's/.*: *//p')
if [ -n "$RATE" ] && [ -n "$HB" ]; then
	v4l2-ctl -d "$SENSOR" -c vertical_blanking=$((RATE / (${FPS:-30} * (W + HB)) - H))
fi

v4l2-ctl -d "$VIDEO" -c black_level="$BLACK"
v4l2-ctl -d "$BRIDGE" -c test_pattern="$TP"
v4l2-ctl -d "$VIDEO" --set-fmt-video width="$W",height="$H",pixelformat=NV12 || exit 1
echo "$SNAME $CODE ${W}x${H} -> ISP -> NV12, black level $BLACK, rotation ${ROT:-0}, bridge pattern $TP"

if [ "$N" = setup ]; then
	echo "VIDEO=$VIDEO"
	echo "SENSOR=$SENSOR"
	exit 0
fi

if [ "$N" = 0 ]; then
	echo "streaming until Ctrl-C; v4l2-ctl prints the frame rate"
	exec v4l2-ctl -d "$VIDEO" --stream-mmap=4 ${OUT:+--stream-to="$OUT"}
fi
OUT=${OUT:-/root/cap.yuv}

timeout 20 v4l2-ctl -d "$VIDEO" --stream-mmap=4 --stream-skip=3 \
	--stream-count="$N" --stream-to="$OUT" --verbose &
sleep 3
cat /sys/kernel/debug/a0c2000.csi/status
cat /sys/kernel/debug/a000000.video-input/status
wait
ls -l "$OUT"
