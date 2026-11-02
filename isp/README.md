# ISP: YUV frames from the SG2000 hardware ISP

Goal: 1920x1080 NV21 or NV12 frames from the GC2083 at 29.7 fps, with no CPU
debayer. Status (2026-11-02): written and compile-tested, not run on
hardware.

## Where the code is

- Branch `cv1800-isp` of github.com/lukaszsobala/sg200x-camera-dkms (fork of
  leafpmpmp's repository), commit `094d89b` on master `4db02e6`. No AI
  attribution in that repository.
- The same driver in `<linux-gc2083>`, branch `cv1800-isp` on v7.2-rc7:
  import commit, then the ISP commit. Build check there:
  `make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- M=drivers/media/platform/sophgo CONFIG_VIDEO_SOPHGO_CV1800_VI=m KBUILD_MODPOST_WARN=1 W=1 modules`.
- Test modules for the 7.2.15 kernels of 2026-11-01 (built from the headers
  packages in `<armbian-build>/output/debs/`): `build/cv1800-vi-7.2.15-riscv64.ko`,
  `build/cv1800-vi-7.2.15-arm64.ko`. Not in git.

## What the driver does

The `cv1800-vi` module gets a second capture node, `cv1800-vi-yuv`. The raw
node (`cv1800-vi-raw`) stays. One node streams at a time.

The front end writes each raw frame to one of three internal buffers. The
back end reads the frame back and runs the raw, RGB and YUV stages. The YUV
stage writes 4:2:0 to the capture buffer. The vendor calls this the offline
back end mode. The register sequence follows the vendor's GPL kernel driver,
`<duo-buildroot-sdk-v2>/osdrv/interdrv/vi/chip/cv181x/`.

Active: black level, white balance gains, demosaic, sRGB gamma, RGB to YUV
matrix at its reset value. Off: lens shading, colour matrix, defect pixels,
noise reduction, sharpening, tone mapping, all statistics.

Controls on `cv1800-vi-yuv`: `red_balance`, `blue_balance` (1024 = 1.0),
`black_level` (12-bit units; GC2083: 252, from the vendor's
`gc2083_cmos_param.h`), `gamma` (1 = sRGB, 0 = linear).

## Test

Run on the board as root, with `overlays=camera-j1-gc2083` and the 7.2.15
kernel. Check `uname -r` first: `7.2.15-edge-sophgo-sg200x-riscv64` or
`-arm64`.

1. Copy the module of the running architecture and the capture script:

       scp isp/build/cv1800-vi-7.2.15-riscv64.ko root@duos:/root/cv1800-vi.ko
       scp <sg200x-camera-dkms>/tools/stream-yuv.sh root@duos:/root/

2. Replace the module until the next reboot:

       rmmod cv1800_vi && insmod /root/cv1800-vi.ko
       dmesg | tail -5
       media-ctl -p | grep -B1 -A4 'cv1800-vi-yuv'

   Expected: `ISP revision` in dmesg, an entity `cv1800-vi-yuv`, and three
   pads on `cv1800-vi-bridge`. If `rmmod` says the module is in use, stop
   the program that uses the camera.

3. Colour bars from the bridge through the ISP. This needs no light:

       cd /root && PATTERN=1 OUT=bars.yuv ./stream-yuv.sh 1920 1080 4

   Expected: four frames, about 30 fps, and in the status lines
   `isp triggers` equal to `done` with `stalls 0`. On a PC:

       ffmpeg -f rawvideo -pix_fmt nv21 -s 1920x1080 -i bars.yuv -frames 1 bars.png

   Expected: vertical colour bars. If red and blue look swapped, the
   hardware order is NV12, not NV21.

4. The camera, with the black level and 16x gain:

       v4l2-ctl -d "$(media-ctl -e 'gc2083 3-0037')" -c analogue_gain=1024
       BLACK=252 OUT=cam.yuv ./stream-yuv.sh 1920 1080 8

   Convert the last frame on a PC (`-vf select=eq(n\,7)`). Expected: a
   correct image with a colour cast. Try `RED=` and `BLUE=` values from 1024
   to 2500 to remove the cast.

5. NV12, the format the H.264 encoder takes:

       FMT=NV12 BLACK=252 OUT=cam12.yuv ./stream-yuv.sh 1920 1080 8

   Convert with `-pix_fmt nv12`. Expected: the same colours as step 4.

6. The raw node still works: run `tools/j1-stream.sh` from this repository
   as in [j1-test.md](../j1-test.md).

## Report

- The output of steps 2, 3 and 4, with the full status lines. The line
  `csc after reset` gives the matrix format, which is not known yet.
- `bars.png` and one frame of `cam.yuv` as PNG.
- On a stall or a hang: `dmesg | grep -i cv1800` and
  `cat /sys/kernel/debug/a000000.video-input/status`.

To go back: reboot, or `rmmod cv1800_vi && modprobe cv1800_vi`.

## Not known yet

- NV21 or NV12 as the hardware order (step 3).
- Scale, offsets and range of the RGB to YUV matrix at reset. The driver
  reports limited-range BT.601 until that is checked.
- Whether the output DMA accepts software-set strides. The vendor lets the
  hardware set them. The driver writes the same values the vendor
  computes, so for 1920 wide frames both give the same result.
- Sizes other than 1920x1080; the ISP size fields stop at 4096.

## Next steps

1. Fix what the first test shows.
2. Colour matrix control, and an explicit RGB to YUV matrix.
3. Statistics (AE histogram, AWB grid) to a metadata node, and a params
   node in the v4l2-isp format (`include/uapi/linux/media/v4l2-isp.h`).
4. A libcamera pipeline handler with exposure and white balance loops.
5. cam2h264: capture from `cv1800-vi-yuv` as NV12 at full resolution.
