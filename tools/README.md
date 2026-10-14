# Tools

## Capture and view an image

Capture raw frames on the board, then convert them to PNG on a PC.

### 1. Capture (on the board)

Needs `v4l-utils`, a kernel with the camera drivers, and
`overlays=camera-j1-gc2083` in `/boot/armbianEnv.txt` (see
[../j1-test.md](../j1-test.md)).

    GAIN=256 OUT=/dev/shm/cap.raw ./j1-stream.sh 0 4

- `GAIN`: sensor analogue gain. 64 is 1x (the default), 256 is 4x, 1024 is
  16x. Nothing sets the exposure automatically: use about 256 in a lit room,
  1024 in dim light. Too much gain makes the bright areas white.
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

## Other tools

- `j1-diag.sh` (board): samples the CSI-2 lane state during a stream and dumps
  the sensor registers. Use it when a capture gives no frames.
- `sensor-dump.sh` (board): reads the GC2083 registers over I2C. Needs
  `i2c-tools`.
- `phy-dump.sh` and `devmem.c` (board): dump the receiver registers through
  `/dev/mem`. From the stage 1 work.
- `stage1-sweep.sh`: for the retired `duos-csi` receiver only.
