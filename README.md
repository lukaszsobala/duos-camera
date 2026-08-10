# Milk-V Duo S camera work

Working files for getting the CAM-GC2083 on the J1 connector running under
Armbian. Kept out of the Armbian fork on purpose - nothing here is wired into
the build.

Two things live here, and they are testable independently:

1. **`duos-cam-j1-probe.sh` + `duos-mclk/`** - a smoke test that answers "does
   the sensor answer on I2C at all". Needs nothing rebuilt. Do this first.
2. **`gc2083.c` + `galaxycore,gc2083.yaml` + `patches/`** - a mainline-style
   V4L2 sensor driver. Needs a kernel rebuild, and can only be partly tested
   until there is a CSI-2 receiver driver for the SG2000.

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
- **0x37 appears without the module having loaded** - the sensor does not need
  MCLK to talk. Same conclusion, one less moving part.
- **Nothing on i2c-3, and `clk_summary` shows `clk_cam0` at 0Hz or a wrong
  rate** - the clock is the problem, not the wiring. The fix is in
  `clk-cv1800.c`'s `clk_cam0` mux/divider and it is a bounded one.
- **Nothing on i2c-3, `clk_cam0` looks right, and the pinmux dump shows nothing
  for `IIC3_*`** - U-Boot's pinmux did not survive. `&i2c3` in the base DTS
  needs a `pinctrl-0` of its own. Worth fixing regardless of the camera.

Note this only exercises the control path. It says nothing about whether the
MIPI lanes work.

## Undoing it

The script leaves GPIOA_2 exported and driven high, and the module loaded, on
purpose - so you can poke at the sensor by hand afterwards. To clean up:

    rmmod duos_mclk
    echo N > /sys/class/gpio/unexport     # N as printed by the script

Or just reboot.

---

# Test 2: the sensor driver

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
the endpoint in the overlay can dangle.

## Building it into an image

1. Copy the patches in:

       cp patches/*.patch ~/Software/armbian/build/patch/kernel/archive/sophgo-sg200x-7.2/

2. Add the two config symbols. Everything else (`MEDIA_CONTROLLER`,
   `V4L2_FWNODE`, `VIDEO_V4L2_SUBDEV_API`, `V4L2_CCI_I2C`) is selected
   automatically:

       CONFIG_VIDEO_DEV=m
       CONFIG_VIDEO_GC2083=m

   in `config/kernel/linux-sophgo-sg200x-<arch>-bleedingedge.config` for
   whichever arch you are building. Remember the framework rewrites these files
   during the build - see the note about diffing configs.

3. Build as usual and flash. `modprobe gc2083` should succeed; with no device
   tree node it will register the i2c driver and then sit there doing nothing,
   which is the expected result at this stage.

## Getting it to probe

The driver needs a DT node with an endpoint before it will do anything. An
overlay along these lines - **untested, this has not been compiled or run** -
should be enough; the `remote-endpoint` deliberately points at nothing:

    &i2c3 {
        status = "okay";

        gc2083: sensor@37 {
            compatible = "galaxycore,gc2083";
            reg = <0x37>;
            clocks = <&clk CLK_CAM0>;
            assigned-clocks = <&clk CLK_CAM0>;
            assigned-clock-rates = <24000000>;
            reset-gpios = <&porta 2 GPIO_ACTIVE_LOW>;

            port {
                gc2083_out: endpoint {
                    data-lanes = <1 2>;
                    link-frequencies = /bits/ 64 <185625000>;
                };
            };
        };
    };

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
  dry-run apply to the tree in `cache/sources/linux-kernel-worktree/`.
- **Never run against hardware**, and no image has ever come out of it.
- The full-link build is unverified: the Armbian config has `VIDEO_DEV` unset,
  so the V4L2 core is not built in that worktree and modpost cannot resolve
  `cci_*` or `v4l2_subdev_get_fmt`. Compilation was checked with those symbols
  forced on. Step 2 above is what closes this gap.
- The `MODULE_AUTHOR`, binding maintainer and MAINTAINERS entry all name
  lukaszsobala - change them if that is not what you want.

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
- **No test pattern control.** The vendor init writes 0x07/0xff to 0x008c/0x008d,
  which on the related GC05A2 are the test-pattern registers - so a pattern
  generator probably exists and would be genuinely useful for bringing up the
  CSI receiver without optics. Not implemented because the semantics could not
  be confirmed rather than because it is not wanted.

---

# Building on the board itself

Everything above builds on a host and copies the result across. Most of it
builds just as well on the Duo S itself, and it is worth knowing which parts:

|                          | On the board?                                            |
|--------------------------|----------------------------------------------------------|
| `duos-mclk.ko`           | yes, and it is the easy way to get the vermagic right    |
| the device tree overlay  | yes, and it needs no kernel build at all, host or board  |
| `gc2083.ko`              | only against an image that has the V4L2 core - see below |
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

            port {
                gc2083_out: endpoint {
                    data-lanes = <1 2>;
                    link-frequencies = /bits/ 64 <185625000>;
                };
            };
        };
    };

`147` is `CLK_CAM0` from `include/dt-bindings/clock/sophgo,cv1800.h` and `1` is
`GPIO_ACTIVE_LOW`; both are ABI, so writing them out is ugly rather than
fragile. The two cell counts are not decoration - a fragment has no parent
context, so without them `dtc` reads `reg = <0x37>` against the default
`#address-cells = <2>` and warns that it is the wrong length.

That much has been checked: it compiles with `dtc -@`, and `fdtoverlay` merges
it into the built `sg2000-milkv-duo-s.dtb` with `&i2c3`, `&clk` and `&porta` all
resolving and `sensor@37` landing under `/soc/i2c@4030000`. The base dtb carries
a `__symbols__` node, which is what U-Boot's `fdt apply` needs for the same
thing to work at boot. It has still never been booted.

None of which does anything on a stock image - the node will just sit there
unbound, because there is no `gc2083` driver to claim it. It is only useful
once the driver is in the image, and then it saves rebuilding to change the
node.

## gc2083.ko

The driver is a single self-contained `.c` file, so mechanically it builds out
of tree like anything else - `obj-m += gc2083.o` and the same one-line Makefile
as `duos-mclk/`. Against a stock Armbian kernel it does not get far, and the way
it fails is worth spelling out because it is not a linker problem:

    gc2083.c:840: error: 'v4l2_subdev_get_fmt' undeclared here
    gc2083.c:1018: error: implicit declaration of function 'devm_cci_regmap_init_i2c'

Those two are declared behind `CONFIG_VIDEO_V4L2_SUBDEV_API` and
`CONFIG_V4L2_CCI_I2C`, neither of which is set, so the *headers* do not offer
them. Force both on and the compile is clean, and then modpost stops instead:

    ERROR: modpost: "cci_read" [gc2083.ko] undefined!
    ERROR: modpost: "cci_write" [gc2083.ko] undefined!
    ERROR: modpost: "cci_multi_reg_write" [gc2083.ko] undefined!
    ERROR: modpost: "devm_cci_regmap_init_i2c" [gc2083.ko] undefined!
    ERROR: modpost: "v4l2_subdev_get_fmt" [gc2083.ko] undefined!

because the modules that export them were never built. This is the same gap
described under State, just seen from the other end; it was reproduced against
the `7.2__sophgo-sg200x-riscv64__riscv64` worktree rather than on hardware.

So building on the board is not a way around Test 2's image build. What it is
good for is everything after it. Once an image built with
`CONFIG_VIDEO_GC2083=m` is on the card, its headers declare and export all of
the above - `VIDEO_CAMERA_SENSOR` selects `MEDIA_CONTROLLER`, `V4L2_FWNODE` and
`VIDEO_V4L2_SUBDEV_API`, and the driver's own Kconfig entry selects
`V4L2_CCI_I2C` - and the loop becomes

    make && rmmod gc2083; insmod gc2083.ko && dmesg -w

on the board, with no reflash between attempts. For a driver that has never once
been run against hardware, that loop is the difference between an afternoon and
a week.

One config note if you want the toolchain in place before the driver is: adding
`CONFIG_VIDEO_DEV=m` alone is not enough for an out-of-tree build, because
nothing then selects `V4L2_CCI_I2C`. Add `CONFIG_V4L2_CCI_I2C=m` explicitly.

## The kernel itself

Not seriously. Half a gigabyte of RAM, no swap by default beyond zram, and the
rootfs on an SD card, against a build that wants an hour on a laptop. It can be
made to work with swap on USB storage and enough patience, but there is no
reason to: the cross build on the host is the supported path and it is what
every instruction above assumes. The point of this section is the two things
that do not need it.
