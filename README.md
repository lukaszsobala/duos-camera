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
build on the board instead with the linux-headers package installed and
`make KDIR=/lib/modules/$(uname -r)/build`.

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
`overlays=<name>` in `/boot/armbianEnv.txt`, and watch dmesg. A successful
probe is silent; a failure names its reason (`missing endpoint node`, `chip id
mismatch`, `failed to get xclk`). Note that if this works it also supersedes
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
