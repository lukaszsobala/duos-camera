# H.264 and JPEG test: the SG2000 codecs with the mainline coda driver

Goal: encode the J1 camera to H.264 and to motion JPEG with the SoC's
encoders.

The SG2000 H.264 encoder is a Chips&Media CODA980. Mainline's `coda` driver
supports the CODA960, which has the same command interface. Patch 0920 adds
the CODA980 as a CODA960 variant: H.264 encoder only, constant QP, no SRAM.
The register sequence follows the vendor driver
(`duo-buildroot-sdk-v2/osdrv/interdrv/cvi_vc_drv/vcodec/vpuapi/coda9`).

The SG2000 also has a standalone Chips&Media JPEG unit (JPU). It has the
registers of the CODA960 JPEG engine, at offset 0, and no firmware. Patch
0923 adds it to the same driver as a JPEG encoder. The only real change:
the JPU takes the frame addresses in its own registers, where the CODA960
uses its GDI. Patch 0924 adds it to the same overlay.

The camera gives raw Bayer, and the encoders need YUV. The SG2000 ISP has no
mainline driver, so `tools/cam2h264` converts on the CPU, at half size
(960x540).

## Build

Build fork, local branch `duos-coda980`: PR #10936 (leafpmpmp `duos-camera`,
at 9e284db7) plus two commits.
They add 0920 (H.264 driver), 0921/0922 (overlay, riscv/arm64), 0923 (JPEG
driver), 0924 (JPEG node in the overlay), and `CONFIG_VIDEO_CODA=m`. Build
the riscv64 edge kernel as for the camera.

The driver source is `~/Software/armbian/linux-gc2083`, branch `coda980`, on
v7.2-rc7. The coda files are the same in 7.2.11, 7.3-rc6 and 7.3-rc7, so
one 0920 serves both patch directories.

## Board setup

1. Install the image and dtb packages.
2. Copy the vendor firmware to the board, as `/lib/firmware/coda980.bin`:

       scp ~/Software/armbian/duo-buildroot-sdk-v2/device/generic/rootfs_overlay/common/usr/share/fw_vcodec/coda980.bin root@<board>:/lib/firmware/

   md5 `02d773cb7b1ef926c7f1e30e5b7918f8`. It has no license file.
3. In `/boot/armbianEnv.txt`:

       overlays=camera-j1-gc2083 video-codec

4. Copy `cam2h264.c`, `mjpeg-http.c`, `j1-stream.sh`, `j1-h264.sh` and
   `j1-http.sh` to one directory, and build the two programs there (see
   [tools/README.md](tools/README.md)). Copy a raw capture too, for example
   `cap.raw`.
5. Reboot.

## Steps

1. Check that the encoder starts:

       dmesg | grep -i coda

   Expected:
   - `Hardware product code 0x9800, firmware 0x...`
   - `Initialized CODA980, firmware product 0x...., version ...`
   - `coda-video-encoder registered as /dev/videoN`
   - `coda-jpeg-encoder registered as /dev/videoM`, from `b000000.jpeg`

   The JPEG encoder does not need the H.264 firmware. If only the JPEG
   line is there, continue with step 4.

2. Encode a raw capture, without the camera:

       ./cam2h264 -i cap.raw -n 60 -o file.h264

   Expected: 60 frames, a size for frame 0 and frame 30, then a summary.
   Copy `file.h264` to the PC and play it: `ffplay -f h264 file.h264`.

3. Encode from the camera:

       GAIN=256 ./j1-h264.sh 300

   Expected: `out.h264`, 10 s of video. Note the frame rate and the times
   per frame in the summary.

4. JPEG from a raw capture, then motion JPEG from the camera:

       ./cam2h264 -i cap.raw -n 1 -j -o a.jpg
       GAIN=256 ./j1-h264.sh 300 -j

   Expected: `a.jpg` opens in an image viewer, and `out.mjpeg` plays with
   `ffplay -f mjpeg out.mjpeg`.

5. Live view over HTTP (copy `mjpeg-http` and `j1-http.sh` too):

       GAIN=256 ./j1-http.sh

   Open `http://<board>:8080/` in a browser on the PC. Expected: a live
   picture at about 30 fps.

## If a step fails

- Step 1, `firmware request failed`: the file name or place is wrong. After
  you copy the file, reload the driver: `modprobe -r coda_vpu; modprobe
  coda_vpu`. Or reboot.
- Step 1, `firmware get command error`: the BIT processor does not start.
  Report the whole `dmesg | grep -i -e coda -e vpu`.
- Step 2 hangs or fails: turn on the driver's debug output, then run step 2
  again and report `dmesg`:

      echo 1 > /sys/module/coda_vpu/parameters/coda_debug

- Step 2 gives a stream with wrong colours, but `-y a.nv12` gives correct
  frames: the encoder reads the frames wrongly. The frame memory control
  (endian, interleave) is the first suspect.

## Not known yet

- The product code that the firmware reports. The driver prints it and
  continues.
- Whether the CODA980 encodes without the SRAM. The SRAM is in the codec, at
  VPU address 0, 0xE400 bytes. The driver gives it to the H.264 core but
  does not use it yet.
- The frame endian bit (0x10). The vendor sets it; the driver does the same.
- JPEG: whether the JPU INIT command is a sufficient reset after each frame.
  The CODA960 resets the whole codec there.

## Report back

The output of each step. For steps 2 to 4, the `.h264`, `.jpg` and
`.mjpeg` files. For step 5, the frame rate in the browser.

## Results

None yet.
