#!/bin/sh
# Serve the camera on J2 (IMX708 or OV5647) as 1920x1080 motion JPEG over
# HTTP, through the ISP (run on the board).
#   [GAIN=n] [PORT=n] j2-http.sh [cam2h264 options]
# Needs overlays=camera-j2-imx708 (or camera-j2-ov5647) video-codec, a
# cv1800-vi with the ISP path, and cam2h264 and mjpeg-http next to this
# script. Open http://<board>:8080/ in a browser. Ctrl-C stops.
D=$(dirname "$0")
# The server ignores Ctrl-C: cam2h264 stops, prints its summary, and closes
# the pipe, and then the server exits.
sh "$D/j2-h264.sh" 0 -j -o - "$@" |
	(trap '' INT; exec "$D/mjpeg-http" -p "${PORT:-8080}")
