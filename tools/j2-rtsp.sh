#!/bin/sh
# Serve the camera on J2 (IMX708 or OV5647) as 1920x1080 H.264 over RTSP,
# through the ISP (run on the board).
#   [GAIN=n] [PORT=n] j2-rtsp.sh [cam2h264 options]
# Needs overlays=camera-j2-imx708 (or camera-j2-ov5647) video-codec,
# /lib/firmware/v4l-coda980-sg2000.bin, a cv1800-vi with the ISP path, and
# cam2h264 and h264-rtsp next to this script. Play rtsp://<board>:8554/ in
# VLC, ffplay or mpv. Ctrl-C stops.
D=$(dirname "$0")
# The server ignores Ctrl-C: cam2h264 stops, prints its summary, and closes
# the pipe, and then the server exits.
sh "$D/j2-h264.sh" 0 -o - "$@" |
	(trap '' INT; exec "$D/h264-rtsp" -p "${PORT:-8554}")
