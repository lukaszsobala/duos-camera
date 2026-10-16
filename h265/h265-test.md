# H.265 test: the SG2000 WAVE420L encoder

Goal: encode the J1 camera to H.265 with the SoC's H.265 encoder.

The SG2000 H.265 encoder is a Chips&Media WAVE420L. Mainline has no driver
for it. Mainline `wave5` drives the WAVE5 family, which has other commands.
Patch 0926 adds a new driver, `wave420l`: NV12 in, HEVC Main out, constant
QP, an IDR picture every GOP, line buffers in the codec SRAM. The register
sequence follows the vendor driver
(`duo-buildroot-sdk-v2/osdrv/interdrv/cvi_vc_drv/vcodec/vpuapi/wave`) and the
Amlogic SM1 WAVE420L driver (`venc-hevc/w4enc.c` in
github.com/ray38080289/meson-sm1-codecs).

Patch 0925 makes coda give the shared SRAM back to the H.264 core before
each H.264 command. Do not run H.264 and H.265 at the same time: the
cores share the SRAM. Patch 0927 adds the H.265 node to
`overlays=video-codec`.

Status: parked on 2026-10-17. See [README.md](README.md).

## Build

Apply [armbian-build-h265.patch](armbian-build-h265.patch) to the build
fork, on top of the codec branch `duos-coda980`, with `git am`. The build
fork also keeps it as the local branch `duos-h265-parked`. Driver source:
`~/Software/armbian/linux-gc2083`, branch `wave420l`. Build the kernel and
install the image and dtb packages. The last tested kernel: 2026-10-17
00:08, hash part `P7372`, version 7.2.11.

## Board setup

1. Install the image and dtb packages.
2. Copy the vendor firmware to the board, as `/lib/firmware/monet.bin`:

       scp ~/Software/armbian/duo-buildroot-sdk-v2/device/generic/rootfs_overlay/common/usr/share/fw_vcodec/monet.bin root@<board>:/lib/firmware/

   md5 `202043a809fdfa607a2d01d179aa7c3d`. It has no license file.

   Second choice: the StarFive build of the same firmware, with a
   Chips&Media license that allows binary redistribution (no reverse
   engineering). The driver tries `v4l-wave420l-sg2000.bin` first:

       wget -O /lib/firmware/v4l-wave420l-sg2000.bin https://raw.githubusercontent.com/starfive-tech/soft_3rdpart/b60da16be1b36453aa46599889c28310fd148fb8/wave420l/firmware/monet.bin
       md5sum /lib/firmware/v4l-wave420l-sg2000.bin   # dbfaf48e103e1bf7f1101b49c5e3bbde

   Remove that file to go back to the Cvitek build.
3. `/boot/armbianEnv.txt` does not change: `overlays=camera-j1-gc2083
   video-codec`.
4. Copy `cam2h264.c`, `h264-rtsp.c`, `j1-stream.sh`, `j1-h264.sh` and
   `j1-rtsp.sh` to one directory, and build there. Copy all of them again
   after a change: an old `j1-rtsp.sh` starts the server as H.264.

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
   - `instance 0: 960x540, 2 frame buffers, SRAM 19200 bytes`
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
  Report `dmesg` with the log on, with the `hang:` line. Then try without
  the SRAM:

      echo N > /sys/module/wave420l/parameters/sram
      ./cam2h264 -i cap.raw -n 60 -x -o file.h265
- `command 0x8: done without IRQ`: the interrupt number is wrong, but the
  encoder works.
- The stream decodes as 960x536, or the bottom edge is wrong: the unit of
  the conformance window is wrong.

## Not known yet

- The encoding of a picture is untested.
- The unit of the conformance window: the driver gives 4 pixels, like the
  vendor driver.
- Whether the firmware makes each forced IDR picture, and puts VPS, SPS
  and PPS before it.
- The byte count: the driver uses `WR_PTR - RD_PTR`, else
  `RET_ENC_PIC_BYTE`. The log shows both.
- H.264 and H.265 at the same time: not supported. Both cores use the
  SRAM, and each driver takes the SRAM before each picture.

## Report back

The `dmesg` output of steps 1 and 2, with the log on, and the cam2h264
output. Put `file.h265` and `out.h265` in `~/Downloads`.

## Results

### 2026-10-16, kernel P78cc (no SRAM)

- Probe, firmware load and boot work: `WAVE product 0x4201, firmware
  250505, hardware revision 0`.
- SET_PARAM works, with an interrupt: 2 frame buffers, 1 source buffer.
- SET_FRAMEBUF works.
- ENC_PIC hangs: `command 0x8: timeout`. All frames are 0 bytes.
- The register writes match the vendor driver, except the SRAM. The
  vendor always puts the H.265 line buffers in the SRAM
  (`secondary_axi = 0x7` in `cviVEncOpen`).
- The `j1-rtsp.sh` on the board was an old copy: the server started as
  H.264.

Next kernel (Pc2fe): the line buffers go to the SRAM, like in the vendor
driver. A hang prints the VPU state on a `hang:` line.

### 2026-10-16, kernel Pc2fe (SRAM)

- ENC_PIC still hangs. The VPU CPU loops at PC 0x1834-0x1840.
- Later the command ends with fail reason 0x8000,
  `WAVE4_SYSERR_WATCHDOG_TIMEOUT`: the encoder hardware did not finish.
- After the hang, H.264 failed until a reboot.
- Not the cause: control register 0x28 (vendor writes 7), the clocks
  VC_SRC1 and VC_SRC2, the codec MMU (0x40 = 3, off), the slice buffer
  mode, the reset lines. The bus interface (GDI) shows no error.
- The vendor register sequence matches. Found later: a WAVE420L driver for
  the Amlogic SM1 that works on hardware
  (github.com/ray38080289/meson-sm1-codecs, `venc-hevc/w4enc.c`). It
  differs: it clears `VINT_REASON_USR` after each interrupt, uses a custom
  GOP, and asks for VPS/SPS/PPS explicitly.

### 2026-10-16, kernel P1edb (Amlogic sequence)

- ENC_PIC still hangs. At the hang the bus status is 0 and the interrupt
  reasons are clear: the VPU waits on no bus transaction.
- H.264 after the hang: `VIDIOC_QBUF: Connection timed out`. Likely cause:
  coda gave the SRAM back only before each picture, and its SEQ_INIT ran
  while the SRAM switch still pointed to the H.265 core.

Next kernel (P49af): coda takes the SRAM before each command. The
`sram_share` parameter of wave420l sets the switch value;
`h265/tools/w4-sram-sweep.sh` tries 2, 0, 1, 3 and -1 (no write), each with and
without the line buffers in the SRAM.

### 2026-10-16/17, kernel P49af

- H.264 works before and after H.265 hangs: coda's SRAM claim per command
  fixes that.
- SRAM switch: with 2 the firmware boots and ENC_PIC hangs. With 0, 1, 3
  or no write, INIT_VPU times out. The H.265 core needs the SRAM to boot;
  2 is right.
- Codec clocks lowered by hand (0x030020e4, 0x030020ec) to 300/343 MHz and
  225/257 MHz: still hangs. The FSBL sets 450/515 MHz (overdrive, 1.00 V).
- StarFive firmware (firmware version 196485): same hang, same PC.

Next kernel (P7372): 1 MiB code window and 1 MiB temp buffer, like the
Chips&Media package and the Amlogic driver. Sophgo uses 256 KiB and
512 KiB.

### 2026-10-17, kernel P7372 (1 MiB code window)

- Same hang, same PC (0x1838-0x1842), with the StarFive firmware.
- No driver-side difference to the working references is left. Open
  question: does the vendor stack encode H.265 on the SG2000 at all? Test:
  official Milk-V image, `sample_venc -c 265 -w 960 -h 540
  --pixel_format=2 -i cap.raw -o enc -n 10`.
