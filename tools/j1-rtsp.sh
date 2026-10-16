#!/bin/sh
# Serve the GC2083 on J1 as H.264 over RTSP (run on the board).
#   [GAIN=n] [PORT=n] j1-rtsp.sh [cam2h264 options]
# Needs overlays=camera-j1-gc2083 video-codec, /lib/firmware/coda980.bin,
# and cam2h264 and h264-rtsp next to this script. Play
# rtsp://<board>:8554/ in VLC, ffplay or mpv. Ctrl-C stops.
D=$(dirname "$0")
# The server ignores Ctrl-C: cam2h264 stops, prints its summary, and closes
# the pipe, and then the server exits.
sh "$D/j1-h264.sh" 0 -o - "$@" |
	(trap '' INT; exec "$D/h264-rtsp" -p "${PORT:-8554}")
