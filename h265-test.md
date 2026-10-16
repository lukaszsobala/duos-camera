# H.265 test: the SG2000 WAVE420L encoder

Goal: encode the J1 camera to H.265 with the SoC's H.265 encoder.

The SG2000 H.265 encoder is a Chips&Media WAVE420L. Mainline has no driver
for it. Mainline `wave5` drives the WAVE5 family, which has other commands.
Patch 0926 adds a new driver, `wave420l`: NV12 in, HEVC Main out, constant
QP, an IDR picture every GOP, no SRAM. The register sequence follows the
vendor driver (`duo-buildroot-sdk-v2/osdrv/interdrv/cvi_vc_drv/vcodec/vpuapi/wave`).

Patch 0925 makes coda give the shared SRAM back to the H.264 core before
each H.264 picture. Patch 0927 adds the H.265 node to `overlays=video-codec`.

## Build

Build fork, local branch `duos-coda980`, commit "sophgo-sg200x: add the
WAVE420L H.265 encoder". Driver source: `~/Software/armbian/linux-gc2083`,
branch `wave420l`. The kernel debs are in `output/debs/`, built on
2026-10-16: `linux-image-edge-sophgo-sg200x-riscv64_*` and
`linux-dtb-edge-sophgo-sg200x-riscv64_*`, version 7.2.11.

## Board setup

1. Install the image and dtb packages.
2. Copy the vendor firmware to the board, as `/lib/firmware/monet.bin`:

       scp ~/Software/armbian/duo-buildroot-sdk-v2/device/generic/rootfs_overlay/common/usr/share/fw_vcodec/monet.bin root@<board>:/lib/firmware/

   md5 `202043a809fdfa607a2d01d179aa7c3d`. It has no license file.
3. `/boot/armbianEnv.txt` does not change: `overlays=camera-j1-gc2083
   video-codec`.
4. Copy `cam2h264.c`, `h264-rtsp.c`, `j1-stream.sh`, `j1-h264.sh` and
   `j1-rtsp.sh` to one directory, and build there:

       gcc -O3 -mcpu=thead-c906 -o cam2h264 cam2h264.c -lm
       gcc -O2 -o h264-rtsp h264-rtsp.c

5. Reboot.

## Steps

1. Check that the driver probes:

       dmesg | grep -i wave420l

   Expected: `wave420l-encoder registered as /dev/videoN`. The driver loads
   the firmware at the first stream, not at boot.

2. Encode a raw capture, with the driver log on:

       echo 1 > /sys/module/wave420l/parameters/debug
       ./cam2h264 -i cap.raw -n 60 -x -o file.h265
       dmesg | grep -i wave420l | tail -20

   Expected in dmesg:
   - `loaded monet.bin, 147184 bytes`
   - `WAVE... product 0x4201, firmware ..., hardware revision ...`
   - `SET_PARAM: N frame buffers, M source buffers`
   - `frame N: ... bytes (pic ..., rd ..., wr ...), type ..., nut ...`

   cam2h264 prints the frame sizes and the summary line.

3. Encode from the camera, with the log off:

       echo 0 > /sys/module/wave420l/parameters/debug
       GAIN=256 ./j1-h264.sh 300 -x

   Expected: `out.h265`, 10 s of video.

4. Live view over RTSP:

       GAIN=256 ./j1-rtsp.sh -x

   On the PC: `ffplay -rtsp_transport tcp -fflags nobuffer
   rtsp://<board>:8554/`, or VLC.

5. H.264 after H.265: this checks that the SRAM goes back to the H.264
   core.

       ./cam2h264 -i cap.raw -n 60 -o file.h264

## If a step fails

- `firmware not found`: `monet.bin` is not in `/lib/firmware`.
- `product number 0x..., expected 0x4201`: the H.265 core is off or held
  in reset. Report `dmesg`.
- `reset timeout`, `INIT_VPU timeout` or `INIT_VPU failed`: the VPU does
  not boot. Report `dmesg`.
- `SET_PARAM failed: reason 0x...`, or `command 0x10 failed`
  (SET_FRAMEBUF): a parameter or a buffer layout is wrong. Report the
  reason.
- `ENC_PIC: no answer; stop the stream`: the picture does not finish.
  Report `dmesg` with the log on.
- `command 0x8: done without IRQ`: the interrupt number is wrong, but the
  encoder works.
- The stream decodes as 960x536, or the bottom edge is wrong: the unit of
  the conformance window is wrong.

## Not known yet

- Everything after the probe is untested.
- The unit of the conformance window: the driver gives 4 pixels, like the
  vendor driver.
- Whether the firmware makes each forced IDR picture, and puts VPS, SPS
  and PPS before it.
- The byte count: the driver uses `WR_PTR - RD_PTR`, else
  `RET_ENC_PIC_BYTE`. The log shows both.
- H.264 and H.265 at the same time: untested. The cores share the SRAM
  switch.

## Report back

The `dmesg` output of steps 1 and 2, with the log on, and the cam2h264
output. Put `file.h265` and `out.h265` in `~/Downloads`.

## Results

None yet.
