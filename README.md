# Milk-V Duo S camera work

Working files for the CAM-GC2083 on the J1 connector of the Milk-V Duo S,
under Armbian.

## Current state (2026-10-18)

- **The camera works.** The GC2083 gives correct 1920x1080 frames at 29.7 fps
  on J1, through the SG2000 receiver and capture drivers (`cv1800-csi2`,
  `cv1800-vi`).
- **The camera support is in Armbian main.** armbian/build merged PR #10936
  on 2026-10-18 (`0931fc56`). The receiver, capture and sensor drivers and
  the camera overlays are no longer kernel patches. They live in
  [leafpmpmp/sg200x-camera-dkms](https://github.com/leafpmpmp/sg200x-camera-dkms),
  under `linux/`. The Armbian extension `sophgo-sg200x-camera` copies a
  pinned commit of that repository into the kernel tree. Both Duo S boards
  enable the extension. The overlay is still `overlays=camera-j1-gc2083`.
- **GC2083 changes go to sg200x-camera-dkms.** Make a commit there, then
  move the `SG200X_CAMERA_REF` pin in `extensions/sophgo-sg200x-camera.sh`.
  The pin `2c1ebd54` carries our `gc2083.c` and binding byte for byte. The
  same repository also builds as a DKMS package for other kernels.
- **Hardware encoders.** The build branch `duos-coda980` (fork
  github.com/lukaszsobala/build) adds the SG2000 H.264 encoder (CODA980)
  and JPEG unit to the mainline `coda` driver: patches 0920-0924, on top
  of Armbian main. They stay kernel patches, because they change a driver
  that mainline already has. Both encoders work on hardware: H.264 at
  29.6 fps from the camera, live over RTSP and MJPEG over HTTP. See
  [h264-test.md](h264-test.md).
- **libcamera works** with the software ISP, at about 5 fps. The startup
  auto exposure overshoots. See [libcamera/README.md](libcamera/README.md).
- **H.265 encoder: parked.** Our `wave420l` driver boots the WAVE420L,
  but the encoder hardware never finishes a picture. The patches, the
  results and the resume steps are in [h265/](h265/README.md).
- **Upstream.** Nothing has been posted yet. The first posting to
  linux-media is ready in [patches/upstream/](patches/upstream/NOTES.md).

## Where to start

| Task                                   | File                                   |
|----------------------------------------|----------------------------------------|
| Test the camera on a new kernel        | [j1-test.md](j1-test.md)               |
| Take a picture, convert it to PNG      | [tools/README.md](tools/README.md)     |
| Encode H.264 or MJPEG, live view (HTTP) | [h264-test.md](h264-test.md), [tools/README.md](tools/README.md) |
| H.265 encoder (parked)                 | [h265/README.md](h265/README.md)       |
| What the driver does and assumes       | "The sensor driver" below              |

## Files

Paths in these files use three placeholders:

- `<armbian-build>`: a checkout of the Armbian build framework,
  github.com/armbian/build, or the fork github.com/lukaszsobala/build for
  the codec branch.
- `<duo-buildroot-sdk-v2>`: Milk-V's vendor SDK,
  github.com/milkv-duo/duo-buildroot-sdk-v2.
- `<linux-gc2083>`: the mainline kernel tree where the drivers were written.
  It is not published. The patches in this repository are its commits.

Current:

- `gc2083.c`, `galaxycore,gc2083.yaml`: the sensor driver and its binding.
  This is the source. The copies in sg200x-camera-dkms
  (`linux/drivers/media/i2c/sg200x-camera/gc2083.c` and
  `linux/Documentation/devicetree/bindings/media/i2c/galaxycore,gc2083.yaml`)
  must stay byte-identical.
- `patches/`: the Armbian copies of the codec patches 0920-0924, from the
  7.2 patch directory. The 7.3 copies differ only in the `index` line of
  0921. `patches/upstream/`: the linux-media series. The GC2083
  patches 0910-0913 are gone: sg200x-camera-dkms holds the driver and the
  J1 overlay now. Git history keeps the old patches.
- `libcamera/`: the libcamera patch for `cv1800-vi` and the GC2083, and a
  cross build.
- `tools/`: capture, conversion, encoding and live view. See
  [tools/README.md](tools/README.md).
- `j1-test.md`, `h264-test.md`: runbooks and results.
- `h265/`: the parked H.265 work: patches, runbook, results, tools.
- `Makefile`: builds `gc2083.ko` out of tree.

`led/` is not camera work. It turns the blue user LED off. See
[led/README.md](led/README.md).

The bring-up research (MCLK smoke test, stub receiver, our own receiver)
was removed on 2026-10-18. Git history keeps it, see "History" below.

## Board facts

Extracted from Milk-V's own board files in the vendor SDK,
`<duo-buildroot-sdk-v2>`
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

# The sensor driver

## What it supports

1920x1080 RAW10, two lanes. The register tables come from the vendor SDK's
userspace sensor driver
(`cvi_mpi/component/isp/sensor/cv181x/gcore_gc2083/`).

- **Link frequency** follows the MCLK rate: 247.5 MHz at 23.76 MHz (the Duo
  S), 250 MHz at 24 MHz. The overlay must list the right one. Probe fails
  with a message for any other MCLK rate.
- **Timing:** HTS 2962, VTS 1125. 29.7 fps on the Duo S, 30 fps at 24 MHz.
- **Controls:** exposure, analogue gain, VBLANK (frame rate), HBLANK (read
  only), hflip/vflip, link frequency and pixel rate (read only).
- **Flips** change the Bayer order, so they return EBUSY while streaming.
- **Analogue gain** is a ladder of 29 steps, each set by 13 registers. The
  driver exposes one control over 1x-148x. It picks the step at or below the
  value, and sets the rest with digital fine gain, as the vendor's AE does.
- **Stream on** is the mode table, then the controls, then the MIPI output
  enable (`0x023e = 0x99`). The vendor's `gc2083_restart()` is a resume
  from standby. Applied on a cold start it leaves the lanes in LP-11.

Confirmed on hardware: the Bayer order RGGB (run 4 in
[j1-test.md](j1-test.md)), the link frequency and frame rate (run 5), and
runtime PM. Suspend really powers the sensor off: I2C reads NAK until it
resumes.

## Things taken on faith

- **The stream on/off sequences** and the 0x031d hold/release around the
  gain writes are copied from the vendor. Which parts matter is unknown.
- **That the flips change the image.** `0x0015` and `0x0d15` are
  write-only. The flip controls work on the V4L2 side, but no frame has been
  checked with a flip set.
- **No test pattern control.** The vendor init writes 0x07/0xff to
  0x008c/0x008d. On the related GC05A2 these are the test-pattern
  registers. Not implemented: their meaning is not confirmed.

## Hardware traps

- **Cable orientation.** The FPC in upside down looks exactly like an empty
  connector: the Duo S has its own I2C pull-ups, so the bus idles high
  either way, and nothing answers at 0x37. Check the cable first. Open the
  J1 latch, see which wall of the slot has the contacts, and put the gold
  fingers against it.
- **MCLK is 23.76 MHz, not 24.** `clk_cam0` divides `clk_cam0pll`
  (1188 MHz), and 1188/50 is the closest it gets. The driver handles that
  rate. **Do not set a rate on `clk_cam0pll`** to get 24 MHz: setting a rate
  on any CV1800 fractional PLL hangs the kernel. `fpll_determine_rate()` and
  `fpll_set_rate()` in `drivers/clk/sophgo/clk-cv18xx-pll.c` read
  `pll_limit[2]` of a two-element array and then loop on whatever follows,
  holding `prepare_lock`. Tried once, it needed a sysrq reboot. This is an
  upstream bug in the Sophgo clock driver, worth reporting on its own.
- **Most registers do not read back.** Do not check `MIPI_EN` (`0x003e`):
  it reads `0x00` after a write of `0x91`. Exposure (`0x0d03/04`) and VTS
  (`0x0d41/42`) do read back. Read one byte at a time: the address does not
  auto-increment in the `0x0dxx` page (it does at `0x03f0`, the chip ID).
- **Control writes need the sensor powered.** `gc2083_set_ctrl()` skips the
  register write while the device is runtime-suspended. Set
  `power/control` to `on` before checking a register by hand.

## Building gc2083.ko on the board

Armbian kernels with the camera extension already build `gc2083.ko`. For a
quick driver change without a kernel build:

    apt install build-essential linux-headers-<branch>-sophgo-sg200x-<arch>
    scp gc2083.c Makefile root@duos:/root/gc2083/
    ssh root@duos 'cd /root/gc2083 && make && (rmmod gc2083; insmod gc2083.ko)'

The headers package must match `uname -r` exactly. For your own image, take
the `.deb` from `output/debs/`. Its postinst builds the kernel scripts on
the board, which takes a few minutes. Do not run `gcc gc2083.c` directly:
a module needs kbuild, which the `Makefile` drives.

## Upstream

`patches/upstream/` holds the first linux-media posting, not sent yet:
cover letter, binding, driver, and [NOTES.md](patches/upstream/NOTES.md)
with the checks and the steps before sending. The patches come from `<linux-gc2083>`, branch `gc2083`, which
is byte-identical to `gc2083.c` and the binding here.

## History

The bring-up log (the I2C smoke test with `duos-mclk`, the stub receiver,
our own `duos-csi` receiver and its stage 1 tests, the old overlays) was
removed on 2026-10-18. It is in the git history of this repository, up to
commit `47b54e9`.
