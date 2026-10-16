#!/bin/sh
# Serve the GC2083 on J1 as H.264, or H.265 with -x, over RTSP (run on the
# board).
#   [GAIN=n] [PORT=n] j1-rtsp.sh [cam2h264 options]
# Needs overlays=camera-j1-gc2083 video-codec, /lib/firmware/coda980.bin
# (H.264) or monet.bin (H.265), and cam2h264 and h264-rtsp next to this
# script. Play rtsp://<board>:8554/ in VLC, ffplay or mpv. Ctrl-C stops.
D=$(dirname "$0")
H=
for a in "$@"; do
	[ "$a" = -x ] && H=-H
done
# The server ignores Ctrl-C: cam2h264 stops, prints its summary, and closes
# the pipe, and then the server exits.
sh "$D/j1-h264.sh" 0 -o - "$@" |
	(trap '' INT; exec "$D/h264-rtsp" $H -p "${PORT:-8554}")
