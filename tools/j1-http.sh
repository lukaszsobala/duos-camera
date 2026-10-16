#!/bin/sh
# Serve the GC2083 on J1 as motion JPEG over HTTP (run on the board).
#   [GAIN=n] [PORT=n] j1-http.sh [cam2h264 options]
# Needs overlays=camera-j1-gc2083 video-codec, and cam2h264 and mjpeg-http
# next to this script. Open http://<board>:8080/ in a browser. Ctrl-C stops.
D=$(dirname "$0")
sh "$D/j1-h264.sh" 0 -j -o - "$@" | "$D/mjpeg-http" -p "${PORT:-8080}"
