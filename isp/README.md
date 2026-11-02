# ISP: YUV frames from the SG2000 hardware ISP

Goal: 1920x1080 NV21 or NV12 frames from the GC2083 at 29.7 fps, with no CPU
debayer. Status (2026-11-02): works on the Duo S (RISC-V). 1080p H.264 over
RTSP with auto exposure and white balance, correct colours, no ISP stalls.
Not tested on the Arm core.

## Where the code is

- Branch `cv1800-isp` of github.com/lukaszsobala/sg200x-camera-dkms (fork of
  leafpmpmp's repository), commit `094d89b` on master `4db02e6`. No AI
  attribution in that repository.
- The same driver in `<linux-gc2083>`, branch `cv1800-isp` on v7.2-rc7:
  import commit, then the ISP commit. Build check there:
  `make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- M=drivers/media/platform/sophgo CONFIG_VIDEO_SOPHGO_CV1800_VI=m KBUILD_MODPOST_WARN=1 W=1 modules`.
- Test modules for 7.2.15 kernels without the ISP path (built from the
  headers packages in `<armbian-build>/output/debs/`):
  `build/cv1800-vi-7.2.15-riscv64.ko`, `build/cv1800-vi-7.2.15-arm64.ko`.
  Not in git. Not needed with a kernel built from the `cv1800-isp` commit.

## What the driver does

The `cv1800-vi` module gets a second capture node, `cv1800-vi-yuv`. The raw
node (`cv1800-vi-raw`) stays. One node streams at a time.

The front end writes each raw frame to one of three internal buffers. The
back end reads the frame back and runs the raw, RGB and YUV stages. The YUV
stage writes 4:2:0 to the capture buffer. The vendor calls this the offline
back end mode. The register sequence follows the vendor's GPL kernel driver,
`<duo-buildroot-sdk-v2>/osdrv/interdrv/vi/chip/cv181x/`.

Active: black level, white balance gains, demosaic, colour matrix, sRGB
gamma, BT.601 RGB to YUV matrix. Off: lens shading, defect pixels, noise
reduction, sharpening, tone mapping, all statistics.

Controls on `cv1800-vi-yuv`: `red_balance`, `blue_balance` (1024 = 1.0),
`black_level` (12-bit units; GC2083: 252, from the vendor's
`gc2083_cmos_param.h`), `gamma` (1 = sRGB, 0 = linear),
`colour_correction_matrix` (3x3, rows R G B, 1024 = 1.0, default identity).
`cam2h264` loads a saturation matrix (`-s`, default 1.5). The vendor keeps
its GC2083 matrix in binary tuning files only, so a calibrated matrix needs
a colour chart.

## Test

Run on the board as root, with `overlays=camera-j1-gc2083 video-codec`.
The steps use the tools of this repository, installed with `make install`
(see [tools/README.md](../tools/README.md)):

    cd /root/duos-camera && git pull && make -C tools install

1. Check that the kernel has the ISP path:

       media-ctl -p | grep -A4 cv1800-vi-yuv
       dmesg | grep -i 'ISP revision'

   Expected: an entity `cv1800-vi-yuv` and `ISP revision 0x3166`. Without
   them, build the kernel with
   `SG200X_CAMERA_REPO=https://github.com/lukaszsobala/sg200x-camera-dkms`
   and `SG200X_CAMERA_REF=commit:094d89b6b8103a2ba732e25d4a9014b3f441878f`.
   For a 7.2.15 kernel without the ISP path, the test modules in `build/`
   work instead until the next reboot:
   `rmmod cv1800_vi && insmod cv1800-vi-7.2.15-riscv64.ko`.

2. Colour bars from the bridge through the ISP. This needs no light:

       BLACK=0 ISP=1 OUT=/dev/shm/bars.yuv j1-stream.sh 1 4

   Expected: in the status lines, `isp triggers` equal to `done` (or one
   more), and `stalls 0`. Write to `/dev/shm` (RAM): on the SD card, the
   writes fall behind after a few frames, and the ISP drops frames for want
   of a free buffer (`dropped` in the status, gaps in `seq`).

3. The camera, with 16x sensor gain for indoor light:

       GAIN=1024 ISP=1 OUT=/dev/shm/cam.yuv j1-stream.sh 0 8

4. Convert on a PC. `j1-stream.sh` captures NV12, the format of the
   encoder, in BT.601 limited range:

       scp root@<board>:/dev/shm/{bars,cam}.yuv .
       ffmpeg -f rawvideo -pix_fmt nv12 -s 1920x1080 -i bars.yuv -frames 1 bars.png
       ffmpeg -f rawvideo -pix_fmt nv12 -s 1920x1080 -i cam.yuv -vf 'select=eq(n\,7)' -frames 1 cam.png

   A driver from before the range fix gives full range: add
   `scale=in_range=full` to the filters.

   Expected: vertical colour bars in `bars.png`, and a correct image with a
   colour cast in `cam.png`. If red and blue are swapped, stop here: the
   chroma order is wrong, and the white balance of step 6 runs to its
   limits. To remove the cast by hand, set the gains (1024 = 1.0) and
   repeat step 3:

       v4l2-ctl -d "$(media-ctl -e cv1800-vi-yuv)" -c red_balance=1800,blue_balance=2000

5. The raw node still works:

       j1-stream.sh 0 8

6. H.264 at 1920x1080 with auto exposure and white balance, after step 4
   works. This needs the encoder firmware as
   `/lib/firmware/v4l-coda980-sg2000.bin` (see
   [h264-test.md](../h264-test.md)). `dmesg | grep -i coda` must show
   `Firmware version: 3.0.1`.

       ISP=1 j1-h264.sh 300
       ISP=1 j1-h264.sh 300 -K

   Expected: 30 fps, and in the lines every 30 frames, the luminance near
   0.18 and the ISP gains settling within a second. Compare the last line,
   `CPU per frame`, of the two runs: `-K` turns the cache hints off.

7. The stream:

       ISP=1 j1-rtsp.sh

   On the PC:

       ffplay -rtsp_transport tcp -fflags nobuffer rtsp://<board>:8554/

## Report

- The output of steps 1, 2 and 3, with the full status lines. The line
  `csc after reset` gives the matrix format, which is not known yet.
- `bars.png` and `cam.png`.
- Steps 6 and 7: the `cam2h264` output with and without `-K`, and a few
  seconds of `out.h264`.
- On a stall or a hang: `dmesg | grep -i cv1800` and
  `cat /sys/kernel/debug/a000000.video-input/status`.

## Not known yet

- Done, 2026-11-02: NV12 as the driver labels it is correct (colour bars
  in step 4). NV21 is not checked.
- Done, 2026-11-02: the RGB to YUV matrix at reset is full-range BT.601,
  rows Y, Cb, Cr, coefficients 1024 = 1.0, offsets 0/512/512 (10-bit
  units). The vendor's scaler code calls this "601 Limited": its labels
  are swapped. coda's H.264 SPS has no VUI to signal full range, so the
  driver now gives limited range, and full range when the application
  sets `V4L2_PIX_FMT_FLAG_SET_CSC` with `V4L2_QUANTIZATION_FULL_RANGE`
  (cam2h264 does for JPEG). The status file shows `isp output limited
  range` or `full range`. Tested 2026-11-02: limited range, correct
  colours and contrast over RTSP.
- Whether the output DMA accepts software-set strides. The vendor lets the
  hardware set them. The driver writes the same values the vendor
  computes, so for 1920 wide frames both give the same result.
- Sizes other than 1920x1080; the ISP size fields stop at 4096.

## Auto exposure and white balance

`cam2h264` does both in ISP mode, from the NV12 frames (see
[tools/README.md](../tools/README.md)). A host simulation of the loops
(scene under a warm light, the sensor and ISP modelled, settings applied 3
frames late) settles in 8 to 20 frames: luminance 0.17 to 0.19, white
balance within 3% of the light's ideal gains, no oscillation. Two
assumptions come from the open questions above: limited-range YUV, and
NV12 as the driver labels it. If the chroma order is swapped, the white
balance loop runs to its limits; check step 4 first.

## Next steps

1. Fix what the first test shows.
2. Colour matrix control, and an explicit RGB to YUV matrix.
3. Statistics (AE histogram, AWB grid) to a metadata node, and a params
   node in the v4l2-isp format (`include/uapi/linux/media/v4l2-isp.h`).
4. A libcamera pipeline handler with exposure and white balance loops.
