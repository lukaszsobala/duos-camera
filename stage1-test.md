# Stage 1 test: is the CSI-2 link actually receiving?

A runbook for `duos-csi.ko` + `duos-cam-csi.dtbo` on a Milk-V Duo S with the
GC2083 on J1. Background, register map and the reasoning behind every constant
are in [receiver-plan.md](receiver-plan.md); this file is just the procedure.

**What is being tested:** that the D-PHY locks to the sensor's clock lane and the
CSI-2 decoder reports RAW10 without errors. That proves MCLK, the sensor's init
table, the ribbon wiring and this driver's lane mapping are all right, together,
on real silicon. **No frames are captured. There is no `/dev/video0`.** That is
stage 2.

## Before you boot

Two things must be true, and both are easy to get wrong.

**1. Load the new overlay, not the old one.** `duos-cam-csi` replaces
`duos-cam`. They both define a node at the same place with the same sensor, so
loading both is a conflict, not a belt-and-braces.

    dtc -@ -I dts -O dtb -o /boot/overlay-user/duos-cam-csi.dtbo duos-cam-csi.dts

Then in `/boot/armbianEnv.txt`:

    user_overlays=duos-cam-csi

If that line currently says `duos-cam`, change it - do not append.

**2. The module must match the running kernel.** The prebuilt
`duos-csi/duos-csi.ko` in this repo was built against
`7.2.0-rc7-bleedingedge-sophgo-sg200x-riscv64`. If `uname -r` differs, build it
on the board instead - that is the reliable way to get vermagic right:

    cd /root/duos-csi && make

Building on the board needs the matching `linux-headers` package, same as
`gc2083.ko`.

## The test

    # 0. Sanity: the overlay applied and the sensor is there.
    ls /proc/device-tree/soc/csi-rx@a0c2000/    # should exist
    modprobe gc2083
    dmesg | tail -20

    # 1. Load the receiver. Order does not matter - the async framework matches
    #    either way - but the sensor has to have probed successfully.
    insmod /root/duos-csi/duos-csi.ko
    dmesg | tail -20                            # expect "bound sensor gc2083 3-0037"

    D=$(echo /sys/bus/platform/drivers/duos-csi/*csi-rx*)
    cat $D/sensor                               # expect "gc2083 3-0037"
    ls -l /dev/v4l-subdev0                      # should exist

    # 2. Look at the link with the sensor still idle. This is the baseline.
    cat $D/phy_state                            # expect clk hs 0
    cat $D/csi_status

    # 3. Start the sensor. This brings the receiver up first, then writes the
    #    sensor's init table and stream-on sequence.
    echo 1 > $D/stream

    # 4. Clear the startup transient, wait, then read the real state.
    echo 1 > $D/clear_errors
    sleep 1
    cat $D/phy_state
    cat $D/csi_status

    # 5. Stop.
    echo 0 > $D/stream

## What a pass looks like

From `phy_state`, the one line that matters:

    clk hs 1 ulps 0 stop 0 err 0

`clk hs 1` means the clock lane is in high-speed mode - the sensor is driving a
real DDR clock and the PHY has locked to it.

From `csi_status`:

    decode_format 0x08 (RAW10)
    ecc_no_error 1 ecc_corrected 0 ecc_error 0
    crc_error 0 wc_error 0 fifo_full 0

`0x08` is bit 3, and it should be the only bit set. The error flags are sticky,
which is why step 4 clears them and waits: flags that come back after a clear are
errors happening *now*, not leftovers from PHY startup.

Please capture the full output of both files, plus `dmesg`, whatever the result -
the interesting failures are the ones where some of this passes.

## If it fails

**The module will not probe.** `dmesg` names the reason. "failed to get clocks"
or "failed to get phy reset" means a clock index or the reset indices 70/71 in
the overlay are wrong for this SoC - those are the constants taken from the
vendor's headers rather than from mainline, so they are the most likely thing to
be off. "missing endpoint node" means the overlay's graph did not resolve.

**The sensor never binds.** Check `/sys/kernel/debug/devices_deferred`. This is
the failure mode that cost three boots last time: silence in `dmesg` with the
device sitting in deferred probe. `post-init-providers` in the overlay is what
prevents it, and it is present.

**`clk hs 0` with `stop 1`.** The lane is parked in LP-11: the sensor is not
transmitting. Look at the sensor, not this driver - confirm `echo 1 > $D/stream`
returned 0, and that the runtime status is active:

    cat /sys/bus/i2c/devices/3-0037/power/runtime_status   # expect "active"
    i2ctransfer -f -y 3 w2@0x37 0x03 0xf7 r1               # PLL_CTRL, expect 0x11

**`clk hs 0` with `err 1`, or no lock at all.** PHY tuning. All three knobs are
module parameters, so no rebuild - sweep in this order:

    rmmod duos-csi
    insmod duos-csi.ko deskew_data=0     # then 1..8
    # then, only if that gets nowhere:
    insmod duos-csi.ko hs_settle=8       # any non-zero overrides the default

`deskew_data` defaults to 3 and `deskew_clk` to 0, both from the vendor's own
`lane_phase[]` table. `hs_settle` defaults to 0 meaning "leave the hardware
default", which is what the vendor does for this sensor - a non-zero value is
going beyond them, not fixing an omission.

**`clk hs 1` but the decode format is wrong, or ECC errors stream in.** The PHY
is locked and the packets are garbage, which points at the lane mapping rather
than the timing. The experiment is swapping the two data lanes:
`DUOS_DATA_LANE0` and `DUOS_DATA_LANE1` in `duos-csi.c`, currently PHY lanes 0
and 2. That needs a rebuild.

**`fifo_full 1`.** The MAC cannot drain fast enough. This points at the MAC clock
rather than the PHY: the vendor asks for 200MHz but the `clk_set_rate` that would
apply it is commented out in their driver, so the rate is whatever
`CLK_SRC_VIP_SYS_2` happens to be. Worth measuring
(`/sys/kernel/debug/clk/clk_summary`) before doing anything about it.

**The board wedges.** A ramoops region is reserved (kernel patches 0056/0057), so
after a sysrq reboot the trace should be in `/sys/fs/pstore/`. The known way to
hang this SoC from a camera overlay is setting a rate on a fractional PLL, which
neither this driver nor this overlay does - but if it happens, that is where to
look first.

## What this does not tell you

That a frame exists, or that the image is any good. Nothing in stage 1 writes a
pixel to memory. A clean pass means the sensor and the receiver agree on a valid
RAW10 CSI-2 stream at the link level, which is exactly the precondition stage 2
needs before there is any point programming the CSIBDG write-DMA.
