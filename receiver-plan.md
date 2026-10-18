# The receiver: getting a frame out of the SG2000

> **Superseded on 2026-10-14.** The receiver and capture drivers from
> armbian/build PR #10936 (`cv1800-csi2`, `cv1800-vi`) do this job and stream
> on J2. Test the GC2083 on them with [j1-test.md](j1-test.md). This file
> stays as the record of the vendor register map. Its lane mapping is wrong:
> see "Board wiring" below.

The sensor side is done (see README.md). What is missing is everything that
receives the two MIPI lanes. This file is the hardware map and the staged plan.

Everything here was read out of `<duo-buildroot-sdk-v2>`, the
vendor 5.10 SDK. Paths below are relative to it.

## The one thing that changes the size of this job

An earlier assessment - in `milkv-duos-docs/feature-status.md`, now corrected -
said raw Bayer could only leave through the ISP's write-back path, making a
minimal capture driver impossible without porting a meaningful slice of the ISP.

That is wrong, and the file that disproves it is
`osdrv/interdrv/vi/chip/cv181x/vi_raw_dump.c`. Its 455 lines write frames
straight to DRAM out of the **CSIBDG** block, the front-end bridge immediately
behind the CSI MAC. The whole configuration is three calls:

    ispblk_csibdg_wdma_crop_config(ctx, raw_num, crop, en);  /* frame geometry */
    ispblk_dma_config(ctx, ISP_BLK_ID_DMA_CTL6, raw_num, b->addr);  /* where */
    ispblk_csidbg_dma_wr_en(ctx, raw_num, ISP_FE_CH0, 1);    /* go */

No RGB pipeline, no YUV pipeline, no tuning blocks, no `isp_tuning` blobs. The
data path we need is **CSI MAC -> CSIBDG -> DMA -> DRAM**, and it is about two
register blocks wide.

Vendor register-field headers ship for the CSI MAC and wrap
(`osdrv/interdrv/cif/chip/cv181x/drv/inc/reg_{blocks,fields}_csi_{mac,wrap}.h`),
so this is a port against documentation, not reverse-engineering.

## Register map

Base addresses from `build/boards/default/dts/cv181x/cv181x_base.dtsi`
(`mipi_rx: cif` and `vi` nodes), block offsets from
`osdrv/interdrv/include/chip/cv181x/uapi/linux/isp_reg.h`.

| Block | Address | Size | Role |
| --- | --- | --- | --- |
| `csi_mac0` | `0x0a0c2000` | `0x2000` | CSI-2 MAC, link 0 |
| `csi_wrap0` | `0x0a0d0000` | `0x1000` | D-PHY wrapper, link 0 |
| `csi_mac1` | `0x0a0c4000` | `0x2000` | second link, unused on Duo S |
| `csi_mac2` | `0x0a0c6000` | `0x2000` | DVP/BT, unused |
| `pad_ctrl` | `0x03001c30` | `0x30` | MIPI pad control |
| `vip_sys` | `0x0a0c8000` | `0x20` | VIP subsystem control (`base` node) |
| ISP window | `0x0a000000` | `0x80000` | contains everything below (`vi` node) |
| `CSIBDG0` | `0x0a000800` | | front-end bridge: crop, channel enable, DMA write enable, IRQ status |
| `DMA_CTL6` | `0x0a000b00` | | write-DMA for front-end A channel 0 (`ISP_BLK_ID_DMA_CTL6`) |

`CSIBDG1` is at `0x0a008800` and `DMA_CTL12` follows it, for the second
front-end. Not needed for one camera.

### Interrupts

From `build/boards/default/dts/cv181x_arm64/cv181x_base_arm.dtsi` and its riscv
twin. The vendor arm64 numbers are `GIC_SPI n` and the riscv ones are `n + 16`
on the PLIC, which is exactly mainline's `SOC_PERIPHERAL_IRQ(n)` on both arches
(`arch/arm64/boot/dts/sophgo/sg2000.dtsi`, `arch/riscv/boot/dts/sophgo/sg2002.dtsi`).
So the mainline DT numbers are:

| Name | Vendor arm64 | Vendor riscv | Mainline |
| --- | --- | --- | --- |
| `csi0` | `GIC_SPI 10` | `26` | `SOC_PERIPHERAL_IRQ(10)` |
| `csi1` | `GIC_SPI 11` | `27` | `SOC_PERIPHERAL_IRQ(11)` |
| `isp` | `GIC_SPI 8` | - | `SOC_PERIPHERAL_IRQ(8)` |

The frame-done interrupt we care about is `isp`, read via
`REG_ISP_CSI_BDG_T.INTERRUPT_STATUS_0/1` on the CSIBDG block
(`vi_drv.c:284`). `csi0` is the MAC's own error interrupt.

### Clocks and resets

Clocks are all already in mainline's `clk-cv1800.c`. The CIF node wants
`CLK_CAM0`, `CLK_SRC_VIP_SYS_2`, `CLK_MIPIMPLL`, `CLK_DISPPLL`, `CLK_FPLL`; the
VI node wants `CLK_SRC_VIP_SYS_0..3`, `CLK_AXI_VIP`, `CLK_CSI_BE_VIP`,
`CLK_RAW_VIP`, `CLK_ISP_TOP_VIP`, `CLK_CSI_MAC0_VIP`.

Resets are the gap: the vendor node uses `RST_CSIPHY0`, `RST_CSIPHY1`,
`RST_CSIPHY0RST_APB`, `RST_CSIPHY1RST_APB`, and mainline's
`arch/riscv/boot/dts/sophgo/cv18xx-reset.h` has only `RST_VIPSYS` (6) and
`RST_VIP_CAM0` (99). The CSI PHY reset indices need adding to that header; they
are plain bit numbers in the same reset controller and the vendor's reset header
has them.

Note `assigned-clock-rates` on any `CV1800_FACTIONAL_PLL` hangs the machine -
see the PLL note in README.md. `CLK_MIPIMPLL` and `CLK_DISPPLL` are both
fractional PLLs, so the receiver driver must consume them at whatever rate it
finds them and must never set their rate.

## Board wiring: the lane mapping

> **Correction (2026-10-14).** The mapping below is the driver default, not
> the board value. The Duo S board file
> `device/generic/rootfs_overlay/duos/mnt/data/sensor_cfg_GC2083.ini` sets
> `lane_id = 2, 0, 1`, and `gc2083_cmos.c` copies the ini value over the
> default (`as16LaneId`). So J1 is **clock on PHY lane 2, data on lanes 0
> and 1**. Stage 1 listened for the clock on a data lane.

This is the part I expected to need a schematic for. It is in the SDK, in
`cvi_mpi/component/isp/sensor/sg200x/gcore_gc2083/gc2083_cmos_param.h`:

    struct combo_dev_attr_s gc2083_rx_attr = {
        .input_mode = INPUT_MODE_MIPI,
        .mac_clk    = RX_MAC_CLK_200M,
        .mipi_attr  = {
            .raw_data_type = RAW_DATA_10BIT,
            .lane_id       = {1, 0, 2, -1, -1},
            .pn_swap       = {0, 0, 0, 0, 0},
            .wdr_mode      = CVI_MIPI_WDR_MODE_NONE,
        },
        .mclk = { .cam = 0, .freq = CAMPLL_FREQ_24M },
        .devno = 0,
    };

So: clock lane on PHY lane 1, data lanes on PHY lanes 0 and 2, no P/N swap
anywhere, RAW10, MAC clock 200MHz, MCLK from CAM0 at 24MHz. The lane mapping is
programmed by `cif_set_lane_id()` in
`osdrv/interdrv/cif/chip/cv181x/drv/cif_drv.c:1263`.

That mapping is board data, not SoC data, so in a mainline driver it belongs in
the DT endpoint - `data-lanes` plus a clock-lane property - and not in the
driver. Our overlay currently declares `data-lanes = <1 2>` on both ends, which
is the *sensor's* logical numbering and is correct there; the receiver end needs
the physical mapping above.

## What the CIF driver actually is

`osdrv/interdrv/cif/chip/cv181x/` is 5200 lines, but most of it is for input
formats we do not have: SubLVDS, HiSPi, TTL/BT.656, BT-demux. The CSI-2 subset
is small and well factored:

- `cif_init()`, `cif_reset()` - bring-up
- `cif_config()` -> `_cif_csi_config()` (`cif_drv.c:721`) - the only format path we need
- `cif_set_lane_id()`, `cif_set_hs_settle()`, `cif_set_lane_deskew()` - PHY tuning
- `cif_streaming()` - on/off
- and, importantly, the diagnostics: `cif_get_csi_phy_state()`,
  `cif_check_csi_int_sts()`, `cif_check_csi_fifo_full()`,
  `cif_get_csi_decode_fmt()`

Those diagnostics matter more than they look. They let us prove the lanes are
being received *before* any buffer plumbing exists, which is the natural first
milestone.

## Staged plan

Each stage has a result you can see, and each one is worth stopping at.

**1. CSI MAC bring-up, no DMA.** *Written - see "Stage 1: built" below.* A
platform driver that maps `csi_mac0`,
`csi_wrap0` and `pad_ctrl`, takes the clocks and resets, programs the lane
mapping and RAW10, and enables the link. Then start the sensor (the stub's
`stream` file already does this) and read back `cif_get_csi_phy_state()`,
`cif_get_csi_decode_fmt()` and the error counters.

*Proof:* the PHY reports lock and the decoder reports RAW10 at 1920x1080. That
alone confirms the whole transmit chain - MCLK, sensor init table, lanes,
polarity, our lane mapping - with no frame buffer anywhere. It is also the stage
where the 1% MCLK error and the `hs_settle` value get shaken out.

**2. CSIBDG plus one DMA channel.** Map the ISP window, program CSIBDG geometry
and `DMA_CTL6` with a CMA buffer, enable channel 0 write, take the `isp`
interrupt, and dump one frame to a file from a debugfs or sysfs hook.

*Proof:* a 1920x1080 RAW10 blob whose size is right and which is not all
zeroes. Debayer it on a host - `rawpy`, ImageMagick, or dcraw with explicit
geometry - and look at it. **This is the first image.**

**3. A real V4L2 capture driver.** Replace the stub with a bridge that owns a
`/dev/video0`: videobuf2 dma-contig, `VIDIOC_S_FMT` on `V4L2_PIX_FMT_SRGGB10`
(or the packed variant the hardware actually emits - check CSIBDG's output
packing before assuming), a media graph `gc2083 -> csi -> video`, buffer
recycling on the frame-done interrupt, and correct stream-on ordering (receiver
before sensor).

*Proof:* `v4l2-ctl --stream-mmap --stream-count=100` runs without dropping, and
`--stream-to` gives a file that plays as a sequence.

**4. Upstreamable shape.** Split into a PHY driver under `drivers/phy/sophgo/`
and a capture driver under `drivers/media/platform/sophgo/`, write the DT
bindings, add the CSI PHY reset macros, and put the nodes in `cv181x.dtsi`
rather than an overlay. Post separately from the GC2083 series, which stays
independent.

Stage 4 is a lot of shaping work but no new unknowns. Stages 1 and 2 are where
the risk is, and stage 1 is cheap.

## Stage 1 in detail: the exact sequence for this board

Traced through `cif.c:_cif_mipi_config()` (line 470) and
`cif_drv.c:_cif_csi_config()` (line 721) with the Duo S values substituted, so
the constants below are resolved rather than symbolic. Macros are at
`cif.c:460-465`: `LANE_IS_PORT1(x) = (x > 2)`, `PAD_CTRL_PU = BIT(2)`,
`PAD_CTRL_PD = BIT(3)`, `PSD_CTRL_OFFSET(n) = (5 - n) * 8`.

With `lane_id = {1, 0, 2}` every lane we use is on port 0, so `clk_port = 0`.

0. **Give the MAC a clock source.** `vip_sys + 0x1c` is
   `VIP_SYS_VIP_CLK_CTRL1`; bits [5:4] select CSI MAC0's functional clock and the
   vendor writes `0x2`, "use vip_sys2", in `_cif_set_mac_clk()`.

   **This is not visible to the clock framework and it is not optional.**
   Mainline models `clk_csi_mac0_vip` as a bare `CV1800_GATE`, so enabling that
   clock opens a gate and says nothing about what is behind it; the mux lives in
   the VIP subsystem's own register space with no clk provider wrapping it.

   Missing it cost a test round trip, and the symptom is worth recognising
   because it looks like progress: the PHY's LP comparator is asynchronous, so
   `clk stop` reads 1 and the line state looks plausible, while everything
   synchronous - `clk hs`, the decode format, every packet and error counter -
   stays at exactly zero.

   The same function also sets the vip_sys2 rate, via a vendor-specific clock
   ratio block or, on a non-Cvitek clk driver, a direct write to `0x03002110`.
   That register belongs to mainline's clk driver, so the mainline equivalent is
   `clk_set_rate()` on `CLK_SRC_VIP_SYS_2` - deliberately **not** done yet. It is
   a `CV1800_BYPASS_MUX` with its own divider and no `CLK_SET_RATE_PARENT`, so a
   rate set should stay local rather than propagating into a fractional PLL, but
   "should" is not good enough next to a known machine-hanging PLL bug. The
   `regs` attribute reports both rates instead, so the next run says whether this
   matters at all.

1. **Clocks and resets.** Enable the CIF clocks, deassert `RST_CSIPHY0` and
   `RST_CSIPHY0RST_APB`. Note `cif_init()` and `cif_reset()` in `cif_drv.c` are
   both empty - there is no magic init step being hidden.
2. **Pad pull-up/down clear - skipped, and it cannot be done this way.** The
   vendor reads `0x03001c30 + (5 - lane) * 8`, clears bits 2 and 3, writes it
   back, then the same at `+ 4` for the pair's other pad.

   That address is inside **pinctrl's** "sys" window (`0x03001000 + 0x1000`),
   which pinctrl requests exclusively, so a driver asking for it fails probe with
   `-EBUSY`. Confirmed on hardware: `can't request region for resource [mem
   0x03001c30-0x03001c5f]`. The vendor gets away with it only because its
   bring-up predates there being a pinctrl driver at all.

   The mapping to pinctrl is exact, which makes the proper fix cheap later. The
   vendor's offset for lane n is the conf register of `MIPIRX<n>N`, and `+ 4` is
   `MIPIRX<n>P`. From `pinctrl-sg2000.c`: `MIPIRX0N` at `0xc58`, `MIPIRX0P` at
   `0xc5c` - i.e. `0x03001c58`, which is `(5 - 0) * 8 = 40` past `0x03001c30`.
   Lanes 1 and 2 are `0xc50` and `0xc48`, matching `(5 - 1) * 8` and
   `(5 - 2) * 8`.

   Expressed properly that is a pinctrl group on the receiver node with
   `bias-pull-up = <0>` and `bias-pull-down = <0>` over MIPIRX0N/0P, 1N/1P and
   2N/2P. Note `bias-disable` will **not** work - `pinctrl-cv18xx.c` handles
   `PIN_CONFIG_BIAS_PULL_UP`/`_DOWN` but falls through to `-ENOTSUPP` for
   `PIN_CONFIG_BIAS_DISABLE` - and the group cannot be config-only, because
   `sophgo,cv1800-pinctrl.yaml` requires both `pinmux` and `power-source`. That
   last point is why it is not done yet: it means asserting a mux value for these
   pads, and guessing one risks breaking pads that work today.

   The pads are therefore left at their reset default. That is defensible -
   these are dedicated differential D-PHY pads in their own 1.8V `VDD18A_MIPI`
   domain, where an enabled pull would be surprising - and self-checking: if the
   PHY locks, the question was moot.
3. **Lane mapping.** `cif_set_lane_id(ctx, i, lane_id[i], 0)` for logical
   i = 0,1,2 -> physical 1,0,2, with no P/N swap. Then park the unused PHY
   lanes: the driver walks the leftover bitmask (`tbl` ends up `0x18`) and calls
   `cif_set_lane_id()` for lanes 3 and 4 as well. Do not skip that.
4. **Clock buffer direction.** `mac_num == 0` and `clk_port == 0`, so
   `cif_set_clk_dir(ctx, CIF_CLK_FREERUN)`.
5. **Lane deskew.** Because `clk_port` is 0 rather than positive, this takes the
   *cross* branch even though all lanes are on one port: clock lane gets phase
   `lane_phase[LANE_SKEW_CROSS_CLK] = 0x00`, and both data lanes are on the same
   port as the clock so they get
   `lane_phase[LANE_SKEW_CROSS_DATA_NEAR] = 0x03`. (`lane_phase[]` is a module
   parameter in the vendor driver, `{0x00, 0x03, 0x08, 0x00, 0x03}` - if the PHY
   will not lock, this is the first thing to sweep.)
6. **HS settle.** `gc2083_rx_attr` does not set `.dphy.enable`, so the vendor
   never calls `cif_set_hs_settle()` and the hardware default stands. One fewer
   unknown, but also the second thing to sweep if the PHY misbehaves.
7. **MAC configuration** (`_cif_csi_config`, all on `csi_mac0`):
   - `REG_SENSOR_MAC_T.REG_00`: `SENSOR_MAC_MODE = 1`, `CSI_VS_INV = 1`,
     `CSI_HS_INV = 1`, `CSI_CTRL_ENABLE = 1`
   - `REG_CSI_CTRL_TOP_T.REG_00`: `CSI_LANE_MODE = 1` (that is `lane_num - 1`
     with `lane_num = 2` data lanes)
   - `REG_CSI_CTRL_TOP_T.REG_70`: `CSI_VS_GEN_MODE = vs_gen_mode`
   - `REG_SENSOR_PHY_4L_T.REG_10` on `csi_wrap0`: `AUTO_IGNORE = 0`,
     `AUTO_SYNC = 0`; `REG_00`: `SENSOR_MODE = 0`
   - `REG_CSI_CTRL_TOP_T.REG_18`: `CSI_VC_MAP_CH00..CH11 = 0,1,2,3`
   - format: RAW10 means `CSI_RAW_10` with CSI-2 data type `0x2B`
8. **`cif_streaming(ctx, 1, 0)`** - hdr off.

Then read back, with the sensor streaming:

- `cif_get_csi_phy_state()` - PHY lock and per-lane state
- `cif_get_csi_decode_fmt()` - should report RAW10
- `cif_check_csi_int_sts()` / `cif_clear_csi_int_sts()` - ECC and CRC errors
- `cif_check_csi_fifo_full()` - overflow, which is what you would expect to see
  if the MAC clock is too low

A clean PHY state with no ECC/CRC errors accumulating is the stage 1 pass.

One loose end: `gc2083_rx_attr` asks for `RX_MAC_CLK_200M`, and the vendor
driver's `max_mac_clk` module parameter defaults to 594 (MHz) while the
`clk_set_rate()` that would apply it is **commented out** at `cif.c:1824`. So
the effective MAC clock is whatever `CLK_SRC_VIP_SYS_2` happens to be. Worth
measuring rather than assuming, and a candidate cause if FIFO-full shows up.

## Stage 1: built, not yet run on hardware

`duos-csi/` is the module. It replaces `duos-cam-stub.ko`: it keeps the stub's
whole job - register a `v4l2_device`, bind the sensor subdev, expose `sensor` and
`stream` - and adds the D-PHY and CSI-2 MAC programming the stub deliberately
left out. `duos-cam-csi.dts` is the matching overlay, superseding
`duos-cam.dts`; keep the old one, it is still what you want for testing the
sensor alone.

State: compiles clean at `W=1` for riscv64 against the 7.2 worktree,
`checkpatch --strict` clean, overlay compiles clean with `dtc -@`. **Not yet
loaded on a board.** The arm64 build is unverified only because this host has no
`aarch64-linux-gnu-gcc`; nothing in the module is arch-specific.

Two things it does *not* do, both deliberate: it takes no interrupt (status is
polled through sysfs, so there is nothing to race), and it holds the VIP clocks
from probe to remove rather than gating them around streaming, because a
register read returning zero because a clock was off is indistinguishable from a
link that is not working.

### Running it

    scp -r duos-csi root@duos:/root/
    ssh root@duos 'cd /root/duos-csi && make && insmod duos-csi.ko'

    dtc -@ -I dts -O dtb -o /boot/overlay-user/duos-cam-csi.dtbo duos-cam-csi.dts
    # user_overlays=duos-cam-csi in /boot/armbianEnv.txt, then reboot

Then, with `gc2083.ko` loaded and bound:

    D=/sys/bus/platform/drivers/duos-csi/csi-rx@a0c2000
    cat $D/sensor              # expect "gc2083 3-0037"
    echo 1 > $D/stream
    cat $D/phy_state
    cat $D/csi_status

### Reading the result

`phy_state` is the PHY's own view. **`clk hs 1` is the pass condition**: the
clock lane is in high-speed mode, meaning the sensor is driving a real DDR clock
and the PHY has locked to it. `clk stop 1` with `clk hs 0` means the lane is
parked in LP-11 and the sensor is not transmitting - look at the sensor, not at
this driver. `clk err 1` is a PHY-level error. The `d0`/`d1` fields are 3-bit
state values rather than flags; `d2`/`d3` are the parked lanes and are not
meaningful.

`csi_status` is the CSI-2 decoder's view. The pass condition is
`decode_format 0x08 (RAW10)` - bit 3, and nothing else set. More than one bit
means the decoder is finding packet headers it should not. Then the error flags:
`ecc_no_error 1` with `ecc_error 0`, `crc_error 0`, `wc_error 0` is what a
healthy link looks like. `fifo_full 1` means the MAC cannot drain fast enough,
which points at the MAC clock question above rather than at the PHY.

The error bits are sticky, so clear them and re-read to tell "errored once
during startup" from "erroring continuously":

    echo 1 > $D/clear_errors
    sleep 1
    cat $D/csi_status

### Where stage 1 actually stands

Four hardware runs in. The receiver is configured correctly - every register
reads back as intended - and the link does not come up. What has been ruled out,
so none of it gets re-tried:

- ~~**The lane mapping.**~~ **Wrong, see the correction under "Board
  wiring".** The idle test showed only *which* three pads are in use, not
  which one is the clock. The clock is on lane 2, which explains the `lp 0/0`
  reading below.
- **The MAC clock source.** `VIP_SYS_CLK_CTRL1` bits [5:4] already read `0x2` at
  baseline; the bootloader sets it. Writing it again changes nothing. The theory
  was wrong, and the code is kept only because "the clk framework says the clock
  is enabled" remains a real trap on this SoC.
- **The MIPI pad bias**, which belongs to pinctrl and was never grantable. Left
  at reset default, and nothing suggests it matters.
- **The sensor's start sequence** - this one was a real bug, see the gc2083
  commit. The driver was applying the vendor's resume-from-standby registers on
  a cold start, which held the sensor in LP-11 idle. Fixing it made lanes 0 and 1
  start toggling LP and took the clock lane out of stop state. Necessary, not
  sufficient.
- **Every PHY tuning knob**, via `tools/stage1-sweep.sh`: `T_HS_SETTLE` from 2 to
  128, `MIPIRX_EN_LOW_BAND_RXAFE` 0-3 alone and combined, deskew phases, and P/N
  swap on both lane groups. About thirty configurations, and **the pad state did
  not change in any of them** - not even an error counter moved.

That invariance is the most useful thing we have. If the receive path were the
limiting factor, some setting would have changed something. Nothing did, which
says the receiver is not being given a high-speed burst to fail on.

Two observations that now read differently:

- `data static` is **not** independent evidence. `AD_D<n>_DATA` is sampled on the
  byte clock recovered from the clock lane, so with `clk hs 0` it cannot update
  no matter what the sensor is doing.
- Lane 2 flat at `lp 0/0` is not necessarily a dead lane. Both LP comparators
  read 0 when a pair is driven differentially, so that is also what a lane parked
  in HS-0 looks like. Combined with a clock lane flapping LP without ever
  reaching HS, the shape of it is a data lane driver that is enabled and a MIPI
  PLL that is not producing a usable clock - which is the sensor's side, not
  ours.

One loose thread on that side: the mode table's last write is `0x023e = 0x99`,
and it reads back `0x00`. That is either a write-only register or a write that is
not landing, and we do not know which. Several other registers in the `0x00xx`
range behave the same way, which is why it was originally dismissed.

### The next step is a known-good diff, not another theory

`tools/phy-dump.sh` and `tools/sensor-dump.sh` read the SoC receiver registers
and the sensor registers respectively, and both run on either stack. Taken on the
vendor 5.10 image with the camera actually streaming, they give the register
values of a configuration that demonstrably produces frames, and the diff against
ours is a list of candidates instead of a guess. `tools/devmem.c` already exists
for the `/dev/mem` side; `STRICT_DEVMEM` is unset on these kernels.

Take the sensor dump seriously as the more likely half. Three of the four wrong
turns so far were on the SoC side, and the one real bug was in the sensor driver.

### If the PHY will not lock

In order of likelihood, and all three are module parameters so none needs a
rebuild:

1. `deskew_data` (default 3) and `deskew_clk` (default 0) - the vendor's own
   `lane_phase[]` values, which they also exposed as a tunable. Sweep
   `deskew_data` first.
2. `hs_settle` (default 0, meaning leave the hardware default) - the vendor never
   programs this for the GC2083, so a non-zero value here is going beyond what
   the vendor does, not fixing an omission.
3. The MCLK being 1% low. It did not stop the chip ID read and should not stop
   the PHY, but it shifts every downstream timing.

If `clk hs` is set but the decode format is wrong or ECC errors stream in, the
suspicion moves to the lane mapping: swapping `DUOS_DATA_LANE0` and
`DUOS_DATA_LANE1` is the obvious experiment, since a data-lane swap gives a
locked PHY and garbage packets.

## Config symbols

`CONFIG_VIDEO_GC2083=m` and `MEDIA_SUPPORT=m` already ship in all four
`config/kernel/linux-sophgo-sg200x-{riscv64,arm64}-{edge,bleedingedge}.config`,
and `VIDEO_CAMERA_SENSOR` is `default y` so removing the `is not set` line was
enough. Since the branch bump, `edge` is 7.2 and `bleedingedge` is 7.3, and the
sensor patches sit in both `patch/kernel/archive/sophgo-sg200x-{7.2,7.3}/` as
`0910`/`0911`. `CONFIG_CMA=y` is already set too. Stage 3 will additionally need
`VIDEOBUF2_DMA_CONTIG` and `V4L_PLATFORM_DRIVERS`, both of which come in by
`select` once a platform driver exists in the tree.
