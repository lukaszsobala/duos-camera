# J1 test: GC2083 on the cv1800-csi2 / cv1800-vi drivers

Goal: get a frame from the GC2083 on J1 through the SG2000 receiver and capture
drivers from armbian/build PR #10936. Those drivers stream on J2. This test
replaces the `duos-csi` stage 1 work.

## What changed from stage 1

- **Lane mapping.** The Duo S board file `sensor_cfg_GC2083.ini` sets
  `lane_id = 2, 0, 1`: clock on PHY lane 2, data on lanes 0 and 1. `duos-csi`
  used `1, 0, 2`, the default in the vendor's `gc2083_cmos_param.h`. The ini
  replaces that default at runtime (`gc2083_cmos.c`, `as16LaneId`). The J2
  entry in the same set of files, `5, 3, 4`, is what the J2 overlay uses.
- **Receiver steps that `duos-csi` did not do:** clear the MIPI pad pulls,
  sample on the falling clock edge (`PHY_AD_CLK_INV`), set the CSI MAC clock
  divider, and derive `T_HS_SETTLE` from the link frequency.

The stage 1 observations agree with the new mapping. Lane 2 read `lp 0/0`
all the time: that is a continuous HS clock. Lanes 0 and 1 toggled LP: those
are data lanes.

## Build

Branch `milkv-duos-camera` in the build fork. It has the PR #10936 commits,
then the GC2083 patches (0900/0901) and the J1 overlay (0902 riscv, 0903
arm64).

    ./compile.sh kernel BOARD=milkv-duos-riscv BRANCH=edge

Install the image and dtb packages on the board. In `/boot/armbianEnv.txt`:

    overlays=camera-j1-gc2083

Remove `duos-cam-csi` and `duos-cam` from `user_overlays`. Do not load
`duos-csi.ko`, `duos-cam-stub.ko` or `duos-mclk.ko`. Reboot.

## Steps

Copy `tools/j1-stream.sh` to the board. Install `v4l-utils`.

1. **Probe.**

       dmesg | grep -i -e gc2083 -e cv1800 -e csi
       media-ctl -p

   Expect four entities: `gc2083 3-0037`, `cv1800-csi2`, `cv1800-vi-bridge`
   and the video node `cv1800-vi-raw`.

2. **Capture path alone.** The bridge pattern starts neither the receiver nor
   the sensor.

       ./j1-stream.sh 1 4

   Expect a `/root/cap.raw` of 4 frames. If this fails, the problem is in the
   VI, not on J1.

3. **Idle lanes.** Power the receiver without a stream:

       echo 1 > /sys/kernel/debug/a0c2000.csi/power
       cat /sys/kernel/debug/a0c2000.csi/status
       echo 0 > /sys/kernel/debug/a0c2000.csi/power

   The sensor is powered off outside a stream, so expect `00/00`. The
   LP-11 check happens in step 4.

4. **Stream.**

       ./j1-stream.sh 0 8

   A good result: `decoded data type 0x2b` (RAW10), the error counters at 0,
   and a `/root/cap.raw` of 8 frames.

## If step 4 fails

Run `tools/j1-diag.sh` first. It samples the lanes during a stream and
dumps the sensor registers.

- No HS on any lane: check that `gc2083` has `stream_on_restart=0` (the
  default). Then sweep `hs_settle` of `cv1800_csi2` from 5 to 20.
- ECC or CRC errors, wrong data type: swap the data lanes in the overlay
  (`data-lanes = <1 0>`), then try P/N swap with `lane-polarities`.
- Frames of the wrong size: the VI resets itself and continues. Check the
  `a000000.video-input/status` file for the measured size.

## Report back

The output of steps 1-4, both debugfs `status` files, and `dmesg`.

## Results

**Run 3, 2026-10-15, riscv64 edge 7.2.10: frames.** 8 frames of 1920x1080 at
29.71 fps (30 x 23.76/24, the MCLK ratio). The lanes alternate between all-HS
and data-lanes-LP-11 with the clock in HS: a continuous clock. Sensor PLL
registers hold the mode table values; `0x023e` reads back `0x99`. One ECC
error; the status snapshot showed data type `0x08`, not `0x2b`. Frames drop
after the first 7, likely the SD card at 90 MB/s. Image content not yet checked:
convert on the host with `tools/raw12-to-png.py cap.raw`.

Runs 1 and 2 used a build with the old driver by mistake (a cherry-pick dropped
the start-sequence fix): all lanes stayed in LP-11.

**Run 4, 2026-10-15: a correct image.** A lit room at `GAIN=256` and
`GAIN=1024`: sharp, colours right (green plants, brown wood), so the Bayer
order RGGB is confirmed. At 16x gain the mean is about 30% of full scale. At
the default 1x gain a dim scene stays at black level (run 3). The camera was held
upside down for this capture, so the orientation needs no change.
