#!/bin/sh
# Encode the GC2083 on J1 to H.264 (run on the board).
#   [GAIN=n] j1-h264.sh [frames] [cam2h264 options]
# Needs overlays=camera-j1-gc2083 video-codec, /lib/firmware/coda980.bin,
# and cam2h264 next to this script. frames = 0 runs until Ctrl-C. The output
# is out.h264, 960x540: see README.md to play it.
D=$(dirname "$0")
N=${1:-300}
[ $# -gt 0 ] && shift

VIDEO=$(sh "$D/j1-stream.sh" 0 setup | sed -n 's/^VIDEO=//p')
[ -n "$VIDEO" ] || { echo "pipeline setup failed"; exit 1; }
exec "$D/cam2h264" -c "$VIDEO" -n "$N" "$@"
