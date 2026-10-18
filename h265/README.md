# H.265 encoder (WAVE420L): parked

Status, 2026-10-17: parked. The `wave420l` driver boots the firmware and the
firmware accepts the stream settings. But the encoder hardware never
finishes a picture. The H.264 encoder and the JPEG unit work and ship
without this work.

## What is here

- [h265-test.md](h265-test.md): the runbook, and every test result.
- `patches/`: the Armbian kernel patches, from the 7.2 patch directory. The
  7.3 copies are identical.
  - 0925: coda gives the shared SRAM back to the H.264 core before each
    command.
  - 0926: the `wave420l` driver.
  - 0927: the H.265 node in `overlays=video-codec` (RISC-V only).
- [armbian-build-h265.patch](armbian-build-h265.patch): the build fork
  commit. It adds 0925-0927 to both patch directories and
  `CONFIG_VIDEO_WAVE420L=m` to the four sophgo-sg200x kernel configs.
- `tools/w4-gdi-dump.sh`: dumps the encoder's bus interface registers after
  a hang.
- `tools/w4-sram-sweep.sh`: tries each value of the SRAM switch.

`cam2h264 -x` and `h264-rtsp -H` in `../tools/` already support H.265.

## Other places

- Driver source: `<linux-gc2083>`, branch `wave420l`, on top
  of branch `coda980`.
- Build fork: `armbian-build-h265.patch` applies unchanged on top of the
  codec branch `duos-coda980` at `2b37f195` (Armbian main since PR
  #10936). The local branch `duos-h265-parked` holds the result; it is not
  pushed.
- Firmware: the Cvitek `monet.bin` from the vendor SDK (no license), or the
  StarFive build (Chips&Media license: binary redistribution allowed, no
  reverse engineering). See [h265-test.md](h265-test.md).
- A WAVE420L driver that works, on the Amlogic SM1:
  github.com/ray38080289/meson-sm1-codecs, `venc-hevc/w4enc.c`.

## What is known

- Boot, GET_FW_VERSION, CREATE_INSTANCE, SET_PARAM (with its interrupt) and
  SET_FRAMEBUF work.
- ENC_PIC hangs. The firmware CPU loops at PC 0x1834-0x1842 with both
  firmware builds. The bitstream pointers do not move. The bus interface is
  idle. Later the command ends with fail reason 0x8000,
  `WAVE4_SYSERR_WATCHDOG_TIMEOUT`.
- The SRAM switch, 0x0b030024, must be 2. With any other value INIT_VPU
  times out.
- No effect: line buffers in the SRAM or not, codec clocks down to
  225/257 MHz, clocks VC_SRC1 and VC_SRC2, control register 0x28, the codec
  MMU, the slice buffer manager, the reset lines, the firmware build, GOP
  preset 2 or custom, implicit or explicit headers, the interrupt
  acknowledge, a 256 KiB or 1 MiB code window.

## Resume

1. Find out if the vendor stack encodes H.265 on the SG2000. Boot the
   official Milk-V image and run:

       sample_venc -c 265 -w 960 -h 540 --pixel_format=2 -i cap.raw -o enc -n 10

   If H.265 fails there too, the SG2000 H.265 encoder is not usable.
2. If it works, dump the encoder registers (0x0b020000-0x0b0201fc), the
   codec control block (0x0b030000-0x0b03004c) and the bus interface
   (`tools/w4-gdi-dump.sh`) during the vendor encode. Compare them with the
   dumps in [h265-test.md](h265-test.md).
3. Apply the build commit in the build fork, on top of the current codec
   branch: `git am armbian-build-h265.patch`. If the coda patches changed,
   regenerate 0925 from the `wave420l` branch of `linux-gc2083`.
