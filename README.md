# Milk-V Duo S camera work

Working files for getting the CAM-GC2083 on the J1 connector running under
Armbian.

**Current state (2026-10-14):** the GC2083 now runs on the SG2000 receiver and
capture drivers from armbian/build PR #10936, which stream on J2. Our own
receiver, `duos-csi`, is retired. Branch `milkv-duos-camera` in the build fork
carries the PR, the GC2083 patches and a J1 overlay. It produces
correct 1920x1080 images since 2026-10-15, see [j1-test.md](j1-test.md). Stage 1 also had the wrong lane mapping, see
that file.

Two things live here, and they are testable independently:

1. **`duos-cam-j1-probe.sh` + `duos-mclk/`** - a smoke test that answers "does
   the sensor answer on I2C at all". Needs nothing rebuilt. Do this first. It
   **passes**: `0x37` answers and the chip ID reads `0x20 0x83`.
2. **`gc2083.c` + `galaxycore,gc2083.yaml` + `patches/`** - a mainline-style
   V4L2 sensor driver. Needs a kernel rebuild, and can only be partly tested
   until there is a CSI-2 receiver driver for the SG2000.

`tools/` holds one thing the probe wants and the image may not have: a 40-line
`/dev/mem` peek/poke, for boards whose busybox was built without the `devmem`
applet.

`led/` is unrelated to the camera - it turns the blue user LED off. It is here
only because this is where Duo S odds and ends that are not in the build live.
See `led/README.md`.

## Board facts

Extracted from Milk-V's own board files in `../duo-buildroot-sdk-v2`
(`build/boards/cv181x/sg2000_milkv_duos_glibc_arm64_sd/u-boot/cvi_board_init.c`
and `device/generic/rootfs_overlay/duos/mnt/data/sensor_cfg_*.ini`):

|              | J1 (16-pin, CAM-GC2083)        | J2 (15-pin, RPi pinout)       |
|--------------|--------------------------------|-------------------------------|
| I2C          | i2c-3, GC2083 at 0x37          | i2c-2, OV5647 at 0x36         |
| MCLK         | `CAM_MCLK0`, 24MHz             | `CAM_MCLK1`                   |
| Reset        | `CAM_RST0` -> GPIOA_2, low     | `CAM_PD1` -> GPIOA_4          |
| MIPI pads    | clk `MIPIRX2`, d0 `MIPIRX0`, d1 `MIPIRX1` | clk `MIPIRX5`, d0 `MIPIRX3`, d1 `MIPIRX4` |

U-Boot already applies all of that pinmux, so the kernel inherits it. Mainline
enables i2c3 with no `pinctrl-0` of its own, which is why that matters.

---

# Test 1: does the sensor answer? (no rebuild needed)

`duos-cam-j1-probe.sh` dumps pinmux and clock state, releases GPIOA_2, loads the
MCLK module if it finds it, scans i2c-3, and reads the chip ID at
0x03f0/0x03f1.

`duos-mclk/` is a throwaway module that enables `CLK_CAM0` at 24MHz. Nothing in
a mainline kernel consumes that clock, so without it the MCLK pad is dead - and
a GalaxyCore sensor very likely will not answer on I2C without MCLK. It binds to
no device node, so it needs no device tree change and no rebuilt image.

## Before you start

- Camera connected to **J1** (the 16-pin 0.5mm connector), board powered off
  while you connect it.
- On the board: `apt install i2c-tools`. Without it the scan and the chip-ID
  read both no-op with a message.
- Everything below runs **as root**.
- Check which build the board is running - `uname -m` gives `aarch64` or
  `riscv64`. The prebuilt `duos-mclk.ko` here is **riscv64**, for
  `7.2.0-rc4-bleedingedge-sophgo-sg200x-riscv64`. If the board is arm64, or on
  any other kernel version, rebuild it (below) or the `insmod` will be rejected.

## Rebuilding duos-mclk.ko

On the host, against the kernel tree Armbian already built. For arm64 you need
`apt install gcc-aarch64-linux-gnu` first.

    cd duos-mclk
    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
         KDIR=~/Software/armbian/build/cache/sources/linux-kernel-worktree/7.2__sophgo-sg200x-arm64__arm64

    # or riscv64:
    make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- \
         KDIR=~/Software/armbian/build/cache/sources/linux-kernel-worktree/7.2__sophgo-sg200x-riscv64__riscv64

`modinfo duos-mclk.ko` should print a `vermagic:` matching `uname -r` on the
board. The worktree is root-owned, so if make complains about permissions run it
with `sudo -E`. A gcc version mismatch warning against the kernel's gcc is
expected and harmless for a module this small; if `insmod` rejects it anyway,
build on the board instead - see "Building on the board itself" below, which is
the shorter route to a module that is certain to match.

## Running it

Both files must land in the **same directory** - the script looks for
`./duos-mclk.ko` next to itself.

    scp duos-cam-j1-probe.sh duos-mclk/duos-mclk.ko root@duos:/root/
    ssh root@duos 'cd /root && sh duos-cam-j1-probe.sh'

If `/sys/kernel/debug` is empty the script says so; `mount -t debugfs none
/sys/kernel/debug` and re-run. Re-running is safe.

## Reading the result

The line that matters is the i2c-3 scan, and then the chip ID, which should read
`0x20` and `0x83`.

- **0x37 appears, chip ID reads 0x20/0x83** - everything on the board side is
  confirmed: mainline pinctrl, the clock driver's `CLK_CAM0` mux, the GPIO, the
  i2c alias, and the wiring table above. The camera plan is grounded; the only
  missing piece is the SoC-side CSI/ISP driver.
- **0x37 appears only with reset asserted, or only in one of the two sweep
  passes** - the polarity in the wiring table above is wrong.
- **Nothing on i2c-3, and the idle-level read is not `1 1`** - the bus is stuck
  or shorted, and no sensor could answer on it whatever else is right.

Note this only exercises the control path. It says nothing about whether the
MIPI lanes work.

## Result: it passes

Run against a CAM-GC2083 on J1 of a Duo S, riscv64 7.2.0-rc7: `0x37` answers and
the chip ID reads `0x20 0x83`. Every line of the wiring table above is now
measured rather than inferred, and so is a bit more:

- **The pinmux survives U-Boot.** `CAM_MCLK0`, `IIC3_SCL` and `IIC3_SDA` read back
  mux 0 and `CAM_RST0` reads mux 3, which is exactly `CAM_MCLK0__CAM_MCLK0`,
  `IIC3_SCL__IIC3_SCL`, `IIC3_SDA__IIC3_SDA` and `CAM_RST0__XGPIOA_2` from the
  vendor's own `cv181x_pinlist_swconfig.h`. `&i2c3` does **not** need a
  `pinctrl-0` for the mux. `UNCLAIMED` in `pinmux-pins` is not evidence of
  anything - it only means no driver took a pinctrl reference.
- **`CLK_CAM0` is the right clock and 23.76MHz is not a bug.** The vendor asks
  for 24MHz off the same `cam0pll` in `_cif_enable_snsr_clk()` and lands on the
  same number.
- **`duos-mclk.ko` is not missing anything.** The vendor's
  `CVI_MIPI_ENABLE_SENSOR_CLOCK` ioctl reduces, on the common-clk path, to
  `clk_set_rate` + `clk_prepare_enable` and nothing else - no MIPI-RX wrapper
  register, and its `PINMUX_CONFIG(CAM_MCLK0, CAM_MCLK0)` is commented out
  because it relies on U-Boot too.
- **Reset is `XGPIOA_2` and it is active low**, confirmed the direct way: `0x37`
  is present with `GPIOA_2` high, gone with it low, back when it goes high again.
- **Nothing is cross-wired.** i2c-1, i2c-2 and i2c-4 are empty, so `bus_id = 3`
  from the vendor's `sensor_cfg_GC2083.ini` is the whole story.
- **No power-down line is stuck asserted.** Every other CV181x/SG200x board in
  the vendor tree muxes `CAM_PD0` to `XGPIOA_1`; the Duo S board file
  deliberately does not.

### What it cost, so the next person does not repeat it

The first several runs found nothing, and the cause was **the FPC in upside
down**. Every measurement along the way was a true negative, and two of them are
worth remembering because they look like faults and are not:

- The pads' reset-default bias *is* pull-down rather than pull-up (`conf 0x48`,
  `PIN_IO_PULLDOWN`), because `PINMUX_CONFIG()` in U-Boot writes only the fmux
  register and mainline gives `&i2c3` no `pinctrl-0` to set the rest. It looks
  like a bug and it is not: the board's own pull-ups win, and `&i2c3` needs no
  `pinctrl-0` for the bias either. Do not "fix" this. The proof is in the errno
  and it was there in the very first failing run: the DesignWare driver returns
  `-EREMOTEIO` for a NACK and `-EAGAIN` for arbitration loss
  (`i2c-designware-common.c`), so `Remote I/O error` at `conf 0x48` means the
  controller sampled SDA released high at the ACK bit. A line genuinely held
  down by that pull-down could not have produced it.
- The Duo S carries its own SDA/SCL pull-ups, so **the bus idles high with no
  camera attached at all**. A `1 1` from the idle-level read says nothing about
  whether the module has power, and a board with a reversed cable is
  indistinguishable, line for line, from a board with an empty connector. That
  cost the most time of anything here.

If a future run comes up empty, check the cable orientation before anything else:
open the J1 latch, look at which wall of the slot the contact fingers are on, and
put the FPC's gold fingers against it.

### One thing still unmeasured

**Does the sensor answer without MCLK?** The passing run had `duos-mclk.ko`
loaded before the scan, so this is untested. Reboot and scan before loading it.
It is cheap, and it is the last thing that would change what the driver's device
tree node has to guarantee before its first register read.

## Undoing it

The script leaves GPIOA_2 exported and driven high, and the module loaded, on
purpose - so you can poke at the sensor by hand afterwards. To clean up:

    rmmod duos_mclk
    echo N > /sys/class/gpio/unexport     # N as printed by the script

Or just reboot.

---

# Test 2: the sensor driver

If you only want the steps in order, skip to "Protocol: gc2083.ko on the board"
below. This section is the reasoning behind them.

## What can actually be tested right now

There is no CSI-2 receiver driver for the SG2000, so no `/dev/video0`, no
frames, and no `/dev/v4l-subdev*` (the subdev devnode only appears once a media
device completes the async binding). What *can* be tested is everything up to
that point, which is most of the driver:

- that it builds and links inside a real Armbian kernel build - **this is the
  one check that has not been done**, see State below;
- that `modprobe gc2083` loads;
- with a device tree node for the sensor, that it probes: powers on, enables
  MCLK at 24MHz, releases reset, reads the chip ID, and registers a subdev.

That last one is a real test of the power sequencing, the clock handling, the
reset polarity and the CCI register access, and it does not need a receiver -
though it does need something on the far end of the endpoint, see below.

## Building it into an image

1. Copy the patches in:

       cp patches/*.patch ~/Software/armbian/build/patch/kernel/archive/sophgo-sg200x-7.2/

2. Add **three** config symbols:

       CONFIG_VIDEO_DEV=m
       CONFIG_VIDEO_CAMERA_SENSOR=y
       CONFIG_VIDEO_GC2083=m

   in `config/kernel/linux-sophgo-sg200x-<arch>-bleedingedge.config` for
   whichever arch you are building. Remember the framework rewrites these files
   during the build - see the note about diffing configs.

   The middle one is the easy one to miss, and missing it fails *silently*.
   Every sensor driver in `drivers/media/i2c/Kconfig` sits inside
   `if VIDEO_CAMERA_SENSOR`, and all four Armbian configs for this family ship
   with `# CONFIG_VIDEO_CAMERA_SENSOR is not set`. Append `CONFIG_VIDEO_GC2083=m`
   under that and `olddefconfig` simply drops it - the symbol is invisible while
   its enclosing `if` is false. No warning, no error, and the build succeeds; you
   only find out afterwards, when the image has no `gc2083.ko` in it and the
   headers still do not declare the CCI functions.

   With `VIDEO_CAMERA_SENSOR=y` the rest does follow automatically: it is a
   `menuconfig` that selects `MEDIA_CONTROLLER`, `V4L2_FWNODE` and
   `VIDEO_V4L2_SUBDEV_API`, and the GC2083 entry itself selects `V4L2_CCI_I2C`.
   It is `default y`, so its only real requirement is that
   `CONFIG_MEDIA_CAMERA_SUPPORT=y`, which these configs already set.

   Do not try to set `CONFIG_V4L2_CCI_I2C=m` or
   `CONFIG_VIDEO_V4L2_SUBDEV_API=y` by hand as a substitute. Neither has a
   Kconfig prompt, so neither is user-settable, and `olddefconfig` discards both
   exactly the way it discards an orphaned `VIDEO_GC2083`. They can only arrive
   via `select`, which means via a driver that selects them.

3. Build as usual and flash. `modprobe gc2083` should succeed; with no device
   tree node it will register the i2c driver and then sit there doing nothing,
   which is the expected result at this stage.

## Getting it to probe

The driver needs a DT node with an endpoint before it will do anything, and the
endpoint needs a remote:

    &i2c3 {
        status = "okay";

        gc2083: sensor@37 {
            compatible = "galaxycore,gc2083";
            reg = <0x37>;
            clocks = <&clk CLK_CAM0>;
            assigned-clocks = <&clk CLK_CAM0>;
            assigned-clock-rates = <24000000>;
            reset-gpios = <&porta 2 GPIO_ACTIVE_LOW>;
            post-init-providers = <&csi_sink>;

            port {
                gc2083_out: endpoint {
                    remote-endpoint = <&csi_sink_in>;
                    data-lanes = <1 2>;
                    link-frequencies = /bits/ 64 <185625000>;
                };
            };
        };
    };

    &{/} {
        csi_sink: csi-sink {
            compatible = "milkv,duos-cam-stub";

            port {
                csi_sink_in: endpoint {
                    remote-endpoint = <&gc2083_out>;
                    data-lanes = <1 2>;
                };
            };
        };
    };

The `csi-sink` stub is not optional, and its absence is the mistake that cost a
boot. `gc2083_parse_fwnode()` calls `fwnode_graph_get_endpoint_by_id()` without
`FWNODE_GRAPH_DEVICE_DISABLED`, so the helper runs each candidate endpoint
through `fwnode_graph_remote_available()` first: it follows `remote-endpoint`
up to the remote's parent device node and asks whether that node is available.
With no `remote-endpoint` the walk yields NULL, `fwnode_device_is_available()`
of NULL is false, the only endpoint is skipped, and the driver reports
`missing endpoint node` and fails with `-EINVAL` - which reads like the node is
absent when in fact it is present and merely dangling. Note what the check does
*not* require: `of_fwnode_device_is_available()` reads the node's `status`
property, not whether a driver has claimed it, so the sensor probes whether or
not `duos-cam-stub.ko` is loaded. The `compatible` is there for the stub driver
to bind to (see Test 3), not for the sensor's benefit.

`post-init-providers` is the second half of the same problem, and it cost
another boot. fw_devlink reads that `remote-endpoint` too, and draws exactly the
conclusion the driver does not want: `parse_remote_endpoint` reduces it to the
remote's port parent, so the sensor is recorded as a consumer of `/csi-sink`,
and `device_links_check_suppliers()` refuses to let a device probe while it has
an unresolved *fwnode* supplier. The version of the overlay that first hit this
had no `compatible` on `/csi-sink`, so no struct device was ever created for it,
the fwnode link was never converted or purged, and the sensor waited forever -
`dmesg` said nothing at all, but `/sys/kernel/debug/devices_deferred` said
`3-0037  i2c: wait for supplier /csi-sink`. Note that this is *not* the same
mechanism that eventually gives up on a supplier device with no driver:
`fw_devlink_no_driver()` relaxes device links, and there was no device link
there to relax.

`post-init-providers` is the generic escape hatch for that, and it needs no
kernel change. Its entry in `of_supplier_bindings` carries
`.fwlink_flags = FWLINK_FLAG_IGNORE`, `__fwnode_link_add()` ORs the flags of a
duplicate link rather than creating a second one, and
`fwnode_links_check_suppliers()` skips any link carrying `IGNORE`. The two
properties have to resolve to the same consumer/supplier pair for the OR to land
on the right link, and they do: the graph parser reduces the endpoint to
`/csi-sink` and the consumer to the sensor node, which is what
`post-init-providers = <&csi_sink>` names directly. Order does not matter.

Keep it even now that `/csi-sink` has a `compatible` and a driver. Without it
the sensor waits on the stub, and the stub is an out-of-tree module that may
never be loaded; worse, the dependency is backwards. It is the stub that waits
for the sensor - asynchronously, through the V4L2 async framework, which is
built for exactly this - and the sensor needs nothing from the stub at all.

The other way out, for the record, is `fw_devlink=permissive` in `extraargs`. It
short-circuits `fwnode_links_check_suppliers()` for the whole system: a fine
one-off test, a bad thing to ship.

Put it in `arch/<arch>/boot/dts/sophgo/overlay/` as a `.dtso` following the
pattern of the existing `sg2000-*.dtso` files, enable it with
`overlays=<name>` in `/boot/armbianEnv.txt`, and watch dmesg. Or skip the
rebuild entirely and compile it on the board - see "Building on the board
itself" below, which has a version of this overlay that `dtc` accepts as-is.
A successful probe is silent; a failure names its reason (`missing endpoint
node`, `chip id mismatch`, `failed to get xclk`). Note that if this works it also supersedes
Test 1 entirely - the driver enables MCLK itself, so `duos-mclk.ko` becomes
redundant.

## State

- Compiles clean at `W=1` against 7.2, `checkpatch --strict` clean, both patches
  apply to plain v7.2-rc7 - see "The upstream series" below, which is where the
  postable version lives now.
- **The driver probes on hardware.** With `CONFIG_VIDEO_CAMERA_SENSOR=y` added,
  `gc2083.ko` builds, loads, binds at `3-0037`, and `probe()` returns 0. Since
  `probe()` powers the sensor on and identifies it before returning, that is a
  real test of the whole bring-up path: MCLK, the reset polarity, and a 16-bit
  CCI read of `0x03f0` returning `0x2083`. Everything below the chip ID -
  streaming, formats, controls - is still untested, and cannot be tested until
  something can open the subdev.

  It took three boots, and the two failures are worth knowing about because
  neither is the driver's fault and only one of them says anything at all:

  1. `missing endpoint node` - the overlay's endpoint had no `remote-endpoint`;
     see the `csi-sink` stub above.
  2. total silence - fw_devlink held the device in deferred probe waiting on
     `/csi-sink`, visible only in `/sys/kernel/debug/devices_deferred`; fixed
     with `post-init-providers`.

  The three `supply ... not found, using dummy regulator` lines that remain are
  expected: the module's rails are hardwired on the board and there is nothing
  in DT to describe.
- **Runtime PM verified, and the power-off is real.** Reading the chip ID
  register directly over i2c returns `Remote I/O error` while the device is
  suspended and `0x20 0x83` after `echo on > power/control`, so the suspend
  genuinely drops the sensor rather than only updating bookkeeping. Five
  `rmmod`/`modprobe` cycles and five unbind/rebind cycles leave `clk_cam0` at
  enable count 0 with no warning from the PM or clk cores, so nothing leaks
  along either teardown path. Note that the enable count reads 1 if you check it
  within the 1000ms autosuspend delay of the last bind - that is the timer, not
  a leak; a leak would accumulate one count per iteration.
- **The stub receiver works.** `duos-cam-stub.ko` binds to the `csi-sink` node,
  the async notifier completes, and `/dev/v4l-subdev0` appears with
  `sensor` reading `gc2083 3-0037`. Everything above the chip ID is now
  reachable from userspace.
- **Formats and controls check out against the vendor timings.** Not just "no
  error" - the numbers are consistent: `hblank` 280 = HTS 2200 - 1920 and is
  read-only, `vblank` 45 = VTS 1125 - 1080 with a max of 15303 that puts VTS at
  the register's 14-bit ceiling, `exposure` max 1117 = VTS - 8, `analogue_gain`
  64-9474 in 1/64 units = 1.0x-148.0x, and `pixel_rate` 74250000 =
  185625000 x 2 lanes x 2 (DDR) / 10 bits. The default format is 1920x1080
  `SRGGB10_1X10`, `ENUM_MBUS_CODE` returns the single code matching the current
  flip state, and setting `hflip` moves it to `0x300a` `SGRBG10_1X10` - index 1
  of the flip table, so the table is wired to the right control. What that does
  *not* prove is that RGGB is the right base; only optics can. Streaming is
  still untested.
- **Streaming register writes work.** `echo 1 > stream` on the stub returns 0,
  which means the whole init table, the control-handler sync and the stream-on
  sequence were ACKed by the sensor. `PLL_CTRL` (`0x03f7`) reads back `0x11`,
  and two control values can be read straight out of the chip afterwards:
  exposure at `0x0d03/04` is `0x045d` = 1117 and VTS at `0x0d41/42` is `0x0465`
  = 1125, both exactly the control defaults. Nothing observes the lanes, so what
  is proven is that the sensor accepted every write and started its PLL - not
  that a frame exists.
- **Register readback is per-register, not per-page, and mostly absent.** Do not
  use `MIPI_EN` (`0x003e`) to check stream state: it reads `0x00` after being
  written `0x91`, as does `0x00d7` after the init table writes `0x29` to it. The
  `+0x0d00` "shadow" is not a general rule either - `0x0d3e` and `0x0dd7` read
  `0x00` as well, even though `0x0d03` and `0x0d41` read back perfectly. Nor is
  it a page rule: `0x0d15` and `0x0d41` are in the same page and only one of
  them reads. The flip pair `0x0015`/`0x0d15` both read `0x00` with the device
  forced active, `hflip` confirmed at 1 by `--get-ctrl` and no error from the
  write, so that is the register and not the driver. Two
  further traps: multi-byte reads do not auto-increment the address in the
  `0x0dxx` page, though they do at `0x03f0`, so read one byte at a time; and
  `gc2083_set_ctrl()` silently skips the register write when the device is
  runtime-suspended, so force `power/control` to `on` before expecting a
  control write to land.
- **MCLK is 23.76MHz, not 24.** `clk_cam0` is a mux over
  `clk_cam0pll` / `_d2` / `_d3` / `clk_mipimpll_d3` with a 6-bit divider, and
  with `clk_cam0pll` at its default 1188MHz the closest it can get to 24MHz is
  1188/50. `assigned-clock-rates = <24000000>` therefore silently lands 1% low,
  and every sensor timing derived from it - frame rate, and the 185.625MHz link
  frequency the overlay declares - is 1% low with it. It is close enough that
  the sensor works, and it did not stop the chip ID read. **We are keeping it.**

  Getting it exact would mean reprogramming the PLL as well, e.g.
  `assigned-clocks = <&clk CLK_CAM0PLL>, <&clk CLK_CAM0>` at `<1200000000>,
  <24000000>`, which is 24 x 50 exactly. **Do not put that in the overlay.**
  Setting any rate on a `CV1800_FACTIONAL_PLL` hangs the machine:
  `fpll_determine_rate()` and `fpll_set_rate()` in
  `drivers/clk/sophgo/clk-cv18xx-pll.c` both call
  `fpll_find_rate(pll, &pll->pll_limit[2], ...)`, but every fractional PLL in
  `clk-cv1800.c` is declared with a **two**-element `pll_limits[]`. The read is
  one past the end, and the nested `for_each_pll_limit_range()` loops then run
  on whatever `.rodata` follows - one CPU pegged in kernel context, no signal
  check, holding the clk framework's `prepare_lock`. Confirmed by trying it:
  `insmod duos-mclk.ko index=6 rate=1200000000` wedged the board and needed a
  sysrq reboot. Through `assigned-clock-rates` it would have run from
  `of_clk_set_defaults()` during boot instead, i.e. before there was any way in.

  That is an upstream bug affecting all six CV1800 fractional PLLs
  (`clk_mpll`, `clk_tpll`, `clk_a0pll`, `clk_disppll`, `clk_cam0pll`,
  `clk_cam1pll`) and is worth reporting separately.
- An earlier attempt at closing the build gap
  did not close it. `CONFIG_VIDEO_DEV=m` and `CONFIG_VIDEO_GC2083=m` were added
  to `config/kernel/linux-sophgo-sg200x-riscv64-bleedingedge.config`, but that
  file also carries `# CONFIG_VIDEO_CAMERA_SENSOR is not set`, so
  `VIDEO_GC2083` was dropped by `olddefconfig` and the image built from it has
  no driver and no CCI symbols - confirmed on hardware, where an out-of-tree
  build against its headers still fails at `v4l2_subdev_get_fmt undeclared`.
  The worktree `.config` shows the same: `VIDEO_CAMERA_SENSOR` unset, no
  `V4L2_CCI_I2C`, no `VIDEO_V4L2_SUBDEV_API`. Compilation was only ever checked
  with those symbols forced on by hand. Adding `CONFIG_VIDEO_CAMERA_SENSOR=y`
  per step 2 above is what should actually close it.
- The `MODULE_AUTHOR`, binding maintainer and MAINTAINERS entry all name
  lukaszsobala - change them if that is not what you want.

## The upstream series

`patches/upstream-rfc-v1/` holds the linux-media posting: cover letter, binding,
driver, and a `NOTES.md` covering what was checked and how to send it. It is
generated from a separate clean tree, `~/Software/armbian/linux-gc2083`, branch
`gc2083`, based on mainline **v7.2-rc7** - not on the Armbian worktree, which
carries a few hundred Armbian patches and cannot produce a postable diff.

`dt_binding_check` and `checkpatch --strict` both pass there, and the driver
builds warning-free at `W=1` for riscv. Those were the two checks listed here as
never run; they have now been run. `CHECK_DTBS` still has nothing to check,
because the series deliberately adds no DTS: a sensor node with nothing at the
far end of its graph does not probe, so an in-tree node has to wait for an
SG2000 receiver driver to exist.

It goes out as **RFC**, for one reason: no frame has ever come out of this
driver, and the cover letter says so in its second paragraph. Everything that
has been verified was verified over I2C. The three open questions - Bayer order,
whether the flip writes take effect, which parts of the vendor init matter - are
in the cover letter as the things review is wanted on, rather than left for a
reviewer to find.

Two changes were made to the driver for upstream, and **neither has been
retested on hardware**:

- `devm_v4l2_sensor_clk_get_legacy()` became `devm_v4l2_sensor_clk_get()`. The
  legacy helper's own kernel-doc says it "must not be used in any new driver",
  and on a DT platform it calls `clk_set_rate(24MHz)` and then ignores what the
  clock actually landed on - which is precisely how 23.76MHz went unnoticed
  here. The non-legacy helper does not touch the rate at all, leaving it to
  `assigned-clock-rates` in the overlay, which is already there and already
  produces the same 23.76MHz. Behaviour on this board should be unchanged.
- Probe now reads `clk_get_rate()` and `dev_warn`s when it is not 24MHz, so the
  next person sees the 1% immediately instead of deriving it from `clk_summary`.

We are keeping 23.76MHz. The alternative was reprogramming `clk_cam0pll`, and
the attempt to test that hung the board: `fpll_determine_rate()` and
`fpll_set_rate()` in `drivers/clk/sophgo/clk-cv18xx-pll.c` both index
`pll->pll_limit[2]` on a two-element array, and the garbage bounds make
`fpll_find_rate()`'s nested loops spin forever. That is an upstream bug in the
sophgo clock driver, worth reporting on its own, and unrelated to this series.
Testing it as a module rather than through the overlay is what kept it from
being an unbootable board.

## What the driver supports

1920x1080 RAW10 at 30fps, two lanes. Exposure, analogue gain, VBLANK/HBLANK,
hflip/vflip, link frequency, pixel rate. Register sequences come from the vendor
SDK's userspace sensor driver
(`cvi_mpi/component/isp/sensor/cv181x/gcore_gc2083/`), which is the only public
source of GalaxyCore's reference settings for this part.

Analogue gain is a ladder of 29 steps about 1.2x apart, each selected by writing
thirteen unrelated registers. The driver exposes one continuous
`V4L2_CID_ANALOGUE_GAIN` over the whole 1x-148x range, picks the step at or
below what was asked, and trims the remainder in digital fine gain - which is
what the vendor's own AE loop does with the same table.

## Things taken on faith

- **Bayer order.** RGGB, because Sophgo's per-sensor table maps
  `GCORE_GC2083_MIPI_2M_30FPS_10BIT` to `BAYER_RGGB`. First thing to check
  against a real frame; if it is wrong the flip-derived codes are wrong too.
- **Link frequency**, 185.625MHz, derived from the vendor's own HTS 2200 x VTS
  1125 x 30fps over two RAW10 lanes rather than read off a datasheet.
- **The stream on/off sequences** and the 0x031d hold/release around the gain
  writes are reproduced verbatim from the vendor; which parts matter is unknown.
- **That the flip writes do anything.** Both `0x0015` and its supposed shadow
  `0x0d15` are write-only, so there is no way to confirm from the bus that
  either took - unlike exposure and VTS, which read back exactly. The V4L2 side
  is verified (the control moves the mbus code through the flip table); the
  sensor side needs a frame. The same applies to the comment on
  `GC2083_REG_FLIP_SHADOW`, which asserts more than can currently be shown.
- **No test pattern control.** The vendor init writes 0x07/0xff to 0x008c/0x008d,
  which on the related GC05A2 are the test-pattern registers - so a pattern
  generator probably exists and would be genuinely useful for bringing up the
  CSI receiver without optics. Not implemented because the semantics could not
  be confirmed rather than because it is not wanted.

---

# Test 3: the stub receiver

Test 2 ends at a wall. The sensor probes, identifies itself and manages its own
power, but there is no `/dev/v4l-subdev0`, so nothing in userspace can open it -
and formats, controls, the vendor init table and the stream-on sequence, which
is most of the driver by line count, stay untested.

The wall is not the hardware. The sensor is on I2C and answers. What is missing
is that a subdev device node is only created by
`v4l2_device_register_subdev_nodes()`, which a *bridge* calls when its async
notifier completes, and the SG2000 has no mainline CSI-2 receiver or ISP driver
to be that bridge.

`duos-cam-stub/` is a bridge that receives nothing. It registers a
`v4l2_device`, adds an async connection for the remote of its own DT endpoint,
and calls `v4l2_device_register_subdev_nodes()` when the sensor binds. It reads
and writes no receiver registers, because there are none it knows about; the
MIPI lanes go nowhere. It is about 250 lines and it exists solely to make the
sensor reachable.

Two sysfs files on the platform device:

- `sensor` - the name of the bound subdev, or empty.
- `stream` - write 1 and it calls `s_stream(1)` on the sensor, which writes the
  full init table, syncs the control handler, waits the vendor's 80ms and runs
  the stream-on sequence. Nothing receives the result. A clean return means the
  sensor accepted every write and started its PLL, not that a frame exists.

## Running it

The overlay already has what it needs - `compatible = "milkv,duos-cam-stub"` on
the `csi-sink` node - so if you are on the current `duos-cam.dtbo` there is no
DT change. Build and load it the same way as the sensor:

    scp -r duos-cam-stub root@duos:/root/
    ssh root@duos 'cd /root/duos-cam-stub && make && insmod duos-cam-stub.ko'

    ls -l /dev/v4l-subdev0
    cat /sys/bus/platform/drivers/duos-cam-stub/csi-sink/sensor   # "gc2083 3-0037"

If `/dev/v4l-subdev0` does not appear, the notifier never completed: check
`dmesg` for `bound sensor`, and check that `gc2083.ko` is loaded and bound
first - the async framework matches in either order, but a sensor that failed to
probe never registers a subdev to match.

## What it makes testable

    apt install v4l-utils

    v4l2-ctl -d /dev/v4l-subdev0 --list-ctrls
    v4l2-ctl -d /dev/v4l-subdev0 --get-subdev-fmt pad=0
    v4l2-ctl -d /dev/v4l-subdev0 --list-subdev-mbus-codes pad=0
    v4l2-ctl -d /dev/v4l-subdev0 --list-subdev-framesizes pad=0,code=0x300f

The flip-derived Bayer order is checkable from here without a frame: set
`hflip`, re-read the mbus code, and confirm it walks the
`SRGGB10/SGRBG10/SGBRG10/SBGGR10` table the way the driver claims. That does not
prove RGGB is the right base - only optics can - but it does prove the table is
wired to the right control.

Then the part that has never run:

    D=/sys/bus/platform/drivers/duos-cam-stub/csi-sink
    echo 1 > $D/stream
    cat /sys/bus/i2c/devices/3-0037/power/runtime_status    # expect "active"
    i2ctransfer -f -y 3 w2@0x37 0x03 0xf7 r1                # PLL_CTRL, expect 0x11
    i2ctransfer -f -y 3 w2@0x37 0x0d 0x03 r1                # exposure hi, 0x04
    i2ctransfer -f -y 3 w2@0x37 0x0d 0x04 r1                # exposure lo, 0x5d
    i2ctransfer -f -y 3 w2@0x37 0x0d 0x41 r1                # VTS hi, 0x04
    i2ctransfer -f -y 3 w2@0x37 0x0d 0x42 r1                # VTS lo, 0x65
    echo 0 > $D/stream
    sleep 2
    cat /sys/bus/i2c/devices/3-0037/power/runtime_status    # "suspended"

One byte per transfer: the address does not auto-increment in the `0x0dxx`
page, so a `r2` read returns the first byte twice. It does auto-increment at
`0x03f0`, which is why the chip ID can be read in one go.

A non-zero write to `stream` returns the errno from `s_stream` - an I2C NAK
partway through the init table shows up as `-EREMOTEIO` from `echo`, which is
the failure worth catching. Reading back a few registers is the only
confirmation available that the sequence took effect, since nothing downstream
can see the lanes - and only some registers read back at all. `MIPI_EN`
(`0x003e`) is not one of them; see the readback note under State.

---

# Test 4: the real receiver

> **Superseded.** Use [j1-test.md](j1-test.md): the PR #10936 receiver
> replaces `duos-csi`, and the lane mapping below is wrong.

The stub's wall - no receiver, so nothing observes the lanes - is what
`duos-csi/` starts to take down. It is the same V4L2 plumbing as the stub plus
the D-PHY and CSI-2 MAC programming, so the sensor's lanes are terminated and
decoded and the MAC can be asked whether it is receiving valid CSI-2. Still no
frames: that is the next stage.

It supersedes `duos-cam-stub.ko`, and `duos-cam-csi.dts` supersedes
`duos-cam.dts`. Load one pair or the other, not both. The stub stays useful for
testing the sensor with the receiver deliberately absent.

**To run it, follow [stage1-test.md](stage1-test.md)** - the procedure, the pass
conditions, and what to do about each way it can fail. **The design and the
register map are in [receiver-plan.md](receiver-plan.md)**, including where every
constant came from. Everything the plan needed from the vendor SDK is recorded
there, so none of it has to be re-derived from
`~/Software/armbian/duo-buildroot-sdk-v2`.

---

# Building on the board itself

Everything above builds on a host and copies the result across. Most of it
builds just as well on the Duo S itself, and it is worth knowing which parts:

|                          | On the board?                                            |
|--------------------------|----------------------------------------------------------|
| `duos-mclk.ko`           | yes, and it is the easy way to get the vermagic right    |
| the device tree overlay  | yes, and it needs no kernel build at all, host or board  |
| `gc2083.ko`              | only against an image with the V4L2 core - see below     |
| the kernel itself        | no, in any practical sense                               |

The reason to bother is that a module built on the board cannot have the wrong
`vermagic`, the wrong arch or the wrong gcc: there is one kernel and one
toolchain and they are both the right ones. No cross compiler, no root-owned
worktree, no `sudo -E`.

## Getting a toolchain and headers

    apt install build-essential
    apt install linux-headers-bleedingedge-sophgo-sg200x-arm64   # see below

The headers package is named `linux-headers-<branch>-sophgo-sg200x-<arch>` -
`bleedingedge` or `edge`, `arm64` or `riscv64`, matching the image.
`armbian-config` under System does the same if you would rather not guess.
If the image came out of your own build rather than an Armbian download, the
`.deb` is already sitting in `output/debs/` next to the image; copy that one
over and `dpkg -i` it, because its version has to match `uname -r` exactly.

Two things are worth knowing before you start it on a board running from an SD
card. It is not small - about 110MB to fetch and 85MB in 26,000 files unpacked
into `/usr/src`. And installing it is not just an unpack: the postinst runs
`make olddefconfig`, `make scripts`, `make M=scripts/mod` and
`tools/bpf/resolve_btfids` on the board, which takes a few minutes on this SoC.
It pulls in `make`, `gcc`, `libc6-dev`, `bison`, `flex`, `libssl-dev`,
`libelf-dev` and `pahole` as dependencies, so `build-essential` is belt and
braces rather than strictly required.

What you get at the end is `/lib/modules/$(uname -r)/build` pointing at
`/usr/src/linux-headers-<version>`. Every Makefile here defaults `KDIR` to
exactly that, so once the symlink exists nothing needs an argument.

## duos-mclk.ko

    scp -r duos-mclk root@duos:/root/
    ssh root@duos 'cd /root/duos-mclk && make'

That is the whole thing - no `ARCH`, no `CROSS_COMPILE`, no `KDIR`. Then move
the `.ko` next to the probe script, since the script looks for `./duos-mclk.ko`
beside itself:

    ssh root@duos 'cp /root/duos-mclk/duos-mclk.ko /root/ && cd /root && sh duos-cam-j1-probe.sh'

This is the answer whenever the prebuilt `duos-mclk.ko` here is rejected - wrong
arch, wrong kernel version, or `insmod` complaining about the module format. It
also costs nothing to redo after every kernel upgrade, which the prebuilt one
does not survive.

## The device tree overlay

This one is the real win, because it needs no kernel build anywhere. Armbian's
boot script for this family loads two sets of overlays: `overlays=` from the
kernel's own `/boot/dtb/sophgo/overlay/`, and `user_overlays=` from
`/boot/overlay-user/`. The second is yours, and `dtc` on the board is enough to
fill it:

    apt install device-tree-compiler
    dtc -@ -I dts -O dtb -o /boot/overlay-user/duos-cam.dtbo duos-cam.dts

then `user_overlays=duos-cam` in `/boot/armbianEnv.txt` and reboot.
`armbian-add-overlay duos-cam.dts` does both steps if the image has it.

The catch is that neither `dtc` nor `armbian-add-overlay` runs the C
preprocessor, so the version under "Getting it to probe" above will not compile
as written - `CLK_CAM0` and `GPIO_ACTIVE_LOW` come from headers that are not
there. Written out numerically it becomes:

    /dts-v1/;
    /plugin/;

    &i2c3 {
        status = "okay";
        #address-cells = <1>;
        #size-cells = <0>;

        gc2083: sensor@37 {
            compatible = "galaxycore,gc2083";
            reg = <0x37>;
            clocks = <&clk 147>;                /* CLK_CAM0 */
            assigned-clocks = <&clk 147>;
            assigned-clock-rates = <24000000>;
            reset-gpios = <&porta 2 1>;         /* GPIO_ACTIVE_LOW */
            post-init-providers = <&csi_sink>;

            port {
                gc2083_out: endpoint {
                    remote-endpoint = <&csi_sink_in>;
                    data-lanes = <1 2>;
                    link-frequencies = /bits/ 64 <185625000>;
                };
            };
        };
    };

    &{/} {
        csi_sink: csi-sink {
            compatible = "milkv,duos-cam-stub";

            port {
                csi_sink_in: endpoint {
                    remote-endpoint = <&gc2083_out>;
                    data-lanes = <1 2>;
                };
            };
        };
    };

`&{/}` is dtc's path-reference syntax; a bare `/ { ... }` in a `/plugin/` file
is the overlay's own root, not a fragment against the base tree, so its contents
would be silently ignored.

`147` is `CLK_CAM0` from `include/dt-bindings/clock/sophgo,cv1800.h` and `1` is
`GPIO_ACTIVE_LOW`; both are ABI, so writing them out is ugly rather than
fragile. The two cell counts are not decoration - a fragment has no parent
context, so without them `dtc` reads `reg = <0x37>` against the default
`#address-cells = <2>` and warns that it is the wrong length.

That much has been checked: it compiles with `dtc -@`, and `fdtoverlay` merges
it into the built `sg2000-milkv-duo-s.dtb` with `&i2c3`, `&clk` and `&porta` all
resolving, `sensor@37` landing under `/soc/i2c@4030000`, and the two
`remote-endpoint` phandles resolving to each other across the two fragments.
The base dtb carries a `__symbols__` node, which is what U-Boot's `fdt apply`
needs for the same thing to work at boot.

The name matters twice: `user_overlays=` in `armbianEnv.txt` has to match the
`.dtbo` basename in `/boot/overlay-user/` exactly, and a mismatch is silent -
the boot script just does not find a file, and you get a kernel with the driver
loaded, no i2c device, and nothing in dmesg. That is indistinguishable at a
glance from "the overlay loaded and the driver ignored it", so check
`/sys/bus/i2c/devices/` for a `3-0037` before believing anything else.

None of which does anything on a stock image - the node will just sit there
unbound, because there is no `gc2083` driver to claim it. It is only useful
once the driver is in the image, and then it saves rebuilding to change the
node.

## Protocol: gc2083.ko on the board

This is the one procedure. Steps 0-2 happen on the host and are one-time; step 3
onwards is the loop you actually iterate in.

**Step 0 - do not use `gcc` directly.** `gcc gc2083.c -o gc2083.ko` cannot work
for any kernel module, and it fails in a way that looks like a missing package
but is not:

    gc2083.c:17:10: fatal error: linux/array_size.h: No such file or directory

`linux/array_size.h` is a kernel header, not a libc one. Bare `gcc` searches
`/usr/include`, never `/lib/modules/$(uname -r)/build/include`, so it dies on
the first `#include <linux/...>` it meets and would die on the next thirty. Even
if the include path were fixed by hand the result would not be a loadable
module: `.ko` files need kbuild's flags, `MODULE_*` section handling, modpost
and the vermagic stamp. The repo has a `Makefile` for this; use `make`.

**Step 1 - build an image whose kernel exports the V4L2 CCI symbols.** There is
no way around this one, and it is the actual reason a board build fails. Two
symbols the driver needs are conditional:

    gc2083.c:840: error: 'v4l2_subdev_get_fmt' undeclared here
    gc2083.c:1018: error: implicit declaration of function 'devm_cci_regmap_init_i2c'

They are declared behind `CONFIG_VIDEO_V4L2_SUBDEV_API` and
`CONFIG_V4L2_CCI_I2C`. A stock Armbian SG200x image sets neither, so the
*headers* on the board do not offer them. Force the declarations on by hand and
the compile is clean but modpost stops instead, because the modules that export
them were never built:

    ERROR: modpost: "cci_read" [gc2083.ko] undefined!
    ERROR: modpost: "cci_write" [gc2083.ko] undefined!
    ERROR: modpost: "cci_multi_reg_write" [gc2083.ko] undefined!
    ERROR: modpost: "devm_cci_regmap_init_i2c" [gc2083.ko] undefined!
    ERROR: modpost: "v4l2_subdev_get_fmt" [gc2083.ko] undefined!

Neither symbol can be turned on from the board. `VIDEO_V4L2_SUBDEV_API` is a
`bool` and `v4l2_subdev_get_fmt` is compiled into `videodev.ko` itself, so
reaching it means rebuilding `videodev`; `V4L2_CCI` builds a `v4l2-cci.ko` that
is not in the image at all. There is no out-of-tree escape from either, and
`KBUILD_MODPOST_WARN=1` is not one - it downgrades the modpost errors to
warnings and hands you a `.ko` that `insmod` rejects with `Unknown symbol`.

So building on the board is not a shortcut past Test 2's image build; it is what
makes every attempt *after* it cheap. Follow "Building it into an image" above,
and note in particular the `CONFIG_VIDEO_CAMERA_SENSOR=y` line - without it
`CONFIG_VIDEO_GC2083=m` is silently discarded and you get an image that looks
built but changes nothing here.

**Step 2 - headers on the board.** See "Getting a toolchain and headers" above.
The `.deb` has to be the one from your own `output/debs/`, matching `uname -r`
exactly, since the image is your own build. When it is in,
`/lib/modules/$(uname -r)/build` exists and the Makefile needs no arguments.

**Step 3 - the overlay, once.** Needed for the driver to bind to anything, not
to build or load it. Without a DT node `insmod gc2083.ko` registers the i2c
driver and stops - no probe, no chip-ID read, nothing in dmesg. Take the
numeric overlay from "The device tree overlay" above, then reboot:

    apt install device-tree-compiler
    dtc -@ -I dts -O dtb -o /boot/overlay-user/duos-cam.dtbo duos-cam.dts
    # user_overlays=duos-cam in /boot/armbianEnv.txt

Nothing needs preparing on the pin side. U-Boot applies the whole J1 pinmux
before the kernel starts and Test 1 measured it surviving; `&i2c3` needs no
`pinctrl-0`. The driver takes `CLK_CAM0` and `XGPIOA_2` from the overlay and
sequences both itself. Do not export GPIOA_2 by hand and do not load
`duos-mclk.ko` - both are Test 1 scaffolding, and a held GPIO or a second
claimant on the clock is a way to make the probe fail for no reason. If the
probe script left them behind, reboot before step 4.

**Step 4 - the loop.**

    scp gc2083.c Makefile root@duos:/root/gc2083/
    ssh root@duos 'cd /root/gc2083 && make && (rmmod gc2083; insmod gc2083.ko) && dmesg -w'

The in-image module and the out-of-tree one are the same driver, so `rmmod` the
one that autoloaded first. A successful probe is silent; a failure names its
reason (`missing endpoint node`, `chip id mismatch`, `failed to get xclk`). No
reflash between attempts - for a driver that has never been run against
hardware, that is the difference between an afternoon and a week.

## The kernel itself

Not seriously. Half a gigabyte of RAM, no swap by default beyond zram, and the
rootfs on an SD card, against a build that wants an hour on a laptop. It can be
made to work with swap on USB storage and enough patience, but there is no
reason to: the cross build on the host is the supported path and it is what
every instruction above assumes. The point of this section is the two things
that do not need it.
