# H.264 test: the SG2000 CODA980 with the mainline coda driver

Goal: encode the J1 camera to H.264 with the SoC's encoder.

The SG2000 H.264 encoder is a Chips&Media CODA980. Mainline's `coda` driver
supports the CODA960, which has the same command interface. Patch 0920 adds
the CODA980 as a CODA960 variant: H.264 encoder only, constant QP, no SRAM.
The register sequence follows the vendor driver
(`duo-buildroot-sdk-v2/osdrv/interdrv/cvi_vc_drv/vcodec/vpuapi/coda9`).

The camera gives raw Bayer, and the encoder needs YUV. The SG2000 ISP has no
mainline driver, so `tools/cam2h264` converts on the CPU, at half size
(960x540).

## Build

Build fork, local branch `duos-coda980`: `duos-camera-j1` plus one commit.
The commit adds 0920 (driver), 0921/0922 (overlay, riscv/arm64), and
`CONFIG_VIDEO_CODA=m`. Build the riscv64 edge kernel as for the camera.

The driver source is `~/Software/armbian/linux-gc2083`, branch `coda980`, on
v7.2-rc7. The coda files are the same in 7.2.11 and 7.3-rc6, so one 0920
serves both patch directories.

## Board setup

1. Install the image and dtb packages.
2. Copy the vendor firmware to the board, as `/lib/firmware/coda980.bin`:

       scp ~/Software/armbian/duo-buildroot-sdk-v2/device/generic/rootfs_overlay/common/usr/share/fw_vcodec/coda980.bin root@<board>:/lib/firmware/

   md5 `02d773cb7b1ef926c7f1e30e5b7918f8`. It has no license file.
3. In `/boot/armbianEnv.txt`:

       overlays=camera-j1-gc2083 video-codec

4. Copy `cam2h264` (built on the PC, see [tools/README.md](tools/README.md)),
   `j1-stream.sh` and `j1-h264.sh` to one directory. Copy a raw capture
   too, for example `cap.raw`.
5. Reboot.

## Steps

1. Check that the encoder starts:

       dmesg | grep -i coda

   Expected:
   - `Hardware product code 0x9800, firmware 0x...`
   - `Initialized CODA980, firmware product 0x...., version ...`
   - `coda-video-encoder registered as /dev/videoN`

2. Encode a raw capture, without the camera:

       ./cam2h264 -i cap.raw -n 60 -o file.h264

   Expected: 60 frames, a size for frame 0 and frame 30, then a summary.
   Copy `file.h264` to the PC and play it: `ffplay -f h264 file.h264`.

3. Encode from the camera:

       GAIN=256 ./j1-h264.sh 300

   Expected: `out.h264`, 10 s of video. Note the frame rate and the times
   per frame in the summary.

## If a step fails

- Step 1, `firmware request failed`: the file name or place is wrong.
- Step 1, `firmware get command error`: the BIT processor does not start.
  Report the whole `dmesg | grep -i -e coda -e vpu`.
- Step 2 hangs or fails: turn on the driver's debug output, then run step 2
  again and report `dmesg`:

      echo 1 > /sys/module/coda/parameters/coda_debug

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

## Report back

The output of each step. For step 2 and step 3, the `.h264` files.

## Results

None yet.
