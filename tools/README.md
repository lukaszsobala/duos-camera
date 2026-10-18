# Tools

## Capture and view an image

Capture raw frames on the board, then convert them to PNG on a PC.

### 1. Capture (on the board)

Needs `v4l-utils`, a kernel with the camera drivers, and
`overlays=camera-j1-gc2083` in `/boot/armbianEnv.txt` (see
[../j1-test.md](../j1-test.md)).

    GAIN=256 OUT=/dev/shm/cap.raw ./j1-stream.sh 0 4

- `GAIN`: sensor analogue gain. 64 is 1x (the default), 256 is 4x, 1024 is
  16x. A raw capture has no auto exposure: use about 256 in a lit room,
  1024 in dim light. Too much gain makes the bright areas white. (`cam2h264`
  below sets the exposure itself.)
- `OUT`: the output file. Use `/dev/shm`, which is in RAM. The SD card is too
  slow for 90 MB/s and drops frames.
- First argument: 0 for the sensor. 1-4 select the VI test patterns.
- Second argument: number of frames. Each frame is 3,110,400 bytes.

### 2. Convert (on the PC)

Needs Python 3 with NumPy and Pillow (`apt install python3-numpy
python3-pil`).

    scp root@<board>:/dev/shm/cap.raw .
    python3 raw12-to-png.py cap.raw

The result is `cap-0.png`, `cap-1.png` and so on, one per frame, at 960x540.
The script prints the mean of each colour per frame. Values near 256 (the
black level) mean that the frame is dark: increase `GAIN`.

Optional arguments: `width height bayer ccm`, default `1920 1080 RGGB 3915`.
`ccm` is the colour temperature of the light in kelvin. It selects one of the
vendor's colour correction matrices: 7221, 5975, 4786, 3915 or 2667. Use 0
for raw colours without correction.

The conversion is simple: half resolution, gray-world white balance, no lens
shading correction, gamma 2.2. It shows whether the camera works, not the best
possible image.

## Watch the CPU load while streaming (on the board)

Stream without saving, until Ctrl-C:

    GAIN=256 ./j1-stream.sh 0 0

v4l2-ctl prints the frame rate. In a second `screen` window, run `top -d 1`.
The frames go to memory by DMA, so the CPU load should stay low. To count the
frame interrupts, compare `grep -e csi -e video-input /proc/interrupts` twice.

## Encode to H.264, H.265 or motion JPEG (on the board)

`cam2h264` converts each raw frame to 960x540 NV12 on the CPU and encodes
it with the SG2000's CODA980 (H.264), with `-x` its WAVE420L (H.265), or
with `-j` its JPEG unit (motion JPEG). See [../h264-test.md](../h264-test.md)
for the kernel, overlay and firmware. H.265 is parked: the driver is not in
the kernel. See [../h265/README.md](../h265/README.md).

Build it on the board:

    apt install gcc libc6-dev
    gcc -O3 -mcpu=thead-c906 -o cam2h264 cam2h264.c -lm

`-mcpu=thead-c906` (gcc 13 or later) adds the C906's T-Head extensions.
With an older gcc, leave it out.

Do not cross-build on Ubuntu 25.10 or later. Its `riscv64-linux-gnu-gcc` and
static libc target RVA23 (vector 1.0, Zba/Zbb). The SG2000's C906 is
rv64gc.

Put `cam2h264`, `j1-stream.sh` and `j1-h264.sh` in one directory on the
board. Then:

    ./j1-h264.sh 300                     # 300 frames to out.h264
    ./cam2h264 -i cap.raw -n 60          # from a raw capture, no camera
    ./cam2h264 -i cap.raw -n 4 -y a.nv12 # conversion only, no encoder
    ./j1-h264.sh 300 -j                  # motion JPEG to out.mjpeg
    ./cam2h264 -i cap.raw -n 1 -j -o a.jpg  # one JPEG
    GAIN=256 ./j1-h264.sh 300            # fixed gain 4x, no auto exposure

Options: `-q` QP (default 30, lower is better quality), `-g` GOP size,
`-j` motion JPEG, `-Q` JPEG quality (default 85), `-s` saturation, `-o`
output file (`-` for stdout), `-b` encoder output buffer size in bytes
(default 12 bits per pixel; a small value forces a stall, for testing). At
the end it prints the frame rate and the time per frame for input,
conversion and encoding.

Auto exposure (from the camera only): `cam2h264` finds the `gc2083` subdev
and sets the sensor exposure time first, then the analogue gain, for a mean
green of 18% of white. Software gain (up to 8x) comes in only when the
sensor is at its limits. Long exposures go in steps of the mains flicker
period (10 ms at 50 Hz), against banding under lamps. Every 30 frames it
prints the exposure in lines (1125 lines = 33 ms), the analogue gain and the
software gain. Options: `-M` no auto exposure (the scripts add it when
`GAIN` is set), `-F 60` for 60 Hz mains, `-F 0` for no flicker steps, `-G`
the highest analogue gain (default 32x), `-S` the sensor subdev.

Play the stream on the PC:

    ffplay -f h264 out.h264
    ffmpeg -framerate 30 -i out.h264 -c copy out.mp4
    ffplay -f mjpeg out.mjpeg
    ffmpeg -framerate 30 -f mjpeg -i out.mjpeg -c copy out.mkv

The conversion uses the top 8 bits of each sample, one pixel per 2x2 Bayer
cell, gray-world white balance, and no colour matrix.

## mjpeg-http: live view in a browser

`mjpeg-http` reads motion JPEG on stdin and serves it over HTTP as
`multipart/x-mixed-replace`, the usual IP camera format. Build it on the
board, like `cam2h264`:

    gcc -O2 -o mjpeg-http mjpeg-http.c

Put it next to `cam2h264`, `j1-stream.sh` and `j1-h264.sh`, with
`j1-http.sh`. On the board:

    ./j1-http.sh                     # PORT=n to change port 8080

Then on the PC:

    http://<board>:8080/              page with the live picture
    http://<board>:8080/stream        the stream: ffplay, VLC, Home Assistant
    http://<board>:8080/snapshot.jpg  the latest frame

A slow client skips frames, and does not slow the camera. The server
counts the bytes still in the socket buffer, so the delay stays short on a
slow link. The server has no authentication: everyone on the network can
watch.

## h264-rtsp: H.264 and H.265 live view over RTSP

`h264-rtsp` reads H.264, or H.265 with `-H`, on stdin and serves it over
RTSP, the usual IP camera protocol. It uses about a fifth of the MJPEG bandwidth. Browsers do
not play RTSP: use VLC, ffplay, mpv, or Home Assistant. Build it on the
board:

    gcc -O2 -o h264-rtsp h264-rtsp.c

Put it next to `cam2h264`, `j1-stream.sh` and `j1-h264.sh`, with
`j1-rtsp.sh`. On the board:

    ./j1-rtsp.sh                     # PORT=n to change port 8554

Then on the PC:

    ffplay -rtsp_transport tcp -fflags nobuffer rtsp://<board>:8554/
    vlc rtsp://<board>:8554/
    mpv --profile=low-latency rtsp://<board>:8554/

RTP goes over the RTSP connection (TCP). A client that asks for UDP gets
"461 Unsupported Transport" and retries over TCP. A client starts at the
next IDR picture, at most 1 s with the default GOP of 30. A slow client
skips to the next IDR picture. No authentication.

## Other tools

- `j1-diag.sh` (board): samples the CSI-2 lane state during a stream and dumps
  the sensor registers. Use it when a capture gives no frames.
- `sensor-dump.sh` (board): reads the GC2083 registers over I2C. Needs
  `i2c-tools`.
- `devmem.c` (board): reads and writes a register through `/dev/mem`. The
  H.265 tools use it.
