#!/bin/sh
# Encode the camera on J2 (IMX708 or OV5647) to 1920x1080 H.264 through the
# ISP (run on the board).
#   [GAIN=n] j2-h264.sh [frames] [cam2h264 options]
# Needs overlays=camera-j2-imx708 (or camera-j2-ov5647) video-codec,
# /lib/firmware/v4l-coda980-sg2000.bin, a cv1800-vi with the ISP path, and
# cam2h264 next to this script. frames = 0 runs until Ctrl-C. The output is
# out.h264. cam2h264 sets the exposure, the gain, the white balance and the
# noise reduction. GAIN=n sets a fixed analogue gain instead. BLACK and FPS
# go to j2-stream.sh.
D=$(dirname "$0")
N=${1:-300}
[ $# -gt 0 ] && shift

SETUP=$(sh "$D/j2-stream.sh" 0 setup)
VIDEO=$(echo "$SETUP" | sed -n 's/^VIDEO=//p')
SENSOR=$(echo "$SETUP" | sed -n 's/^SENSOR=//p')
[ -n "$VIDEO" ] || { echo "$SETUP" >&2; echo "pipeline setup failed" >&2; exit 1; }
echo "$SETUP" | grep -v '^VIDEO=\|^SENSOR=' >&2
exec "$D/cam2h264" -c "$VIDEO" -S "$SENSOR" -n "$N" ${GAIN:+-M} ${BLACK:+-B "$BLACK"} "$@"
