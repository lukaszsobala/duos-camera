# GC2083 upstream v2

Generated on 2026-10-18 from `<linux-gc2083>`, branch `gc2083`
(`1f7b5374c1f6`), based on mainline **v7.2-rc7** (`db2ddb871435`):

```
cd <linux-gc2083>
git format-patch -v2 --cover-letter --base=db2ddb871435 \
    -o <duos-camera>/patches/upstream-v2 db2ddb871435..gc2083
```

The cover letter text and the "Changes in v2" notes under the `---` line of
each patch were added by hand afterwards. Keep them when regenerating.

The binding and the driver are byte-identical to `galaxycore,gc2083.yaml` and
`gc2083.c` at the top of this repository, and to the copies in
sg200x-camera-dkms that Armbian builds. Only the commit messages are extra.

`<linux-gc2083>` is a `git clone --shared` of Armbian's bare kernel cache, so
its objects live in `build/cache/git-bare/kernel`. If Armbian ever
garbage-collects that repository the clone breaks. `git repack -a -d` inside
`linux-gc2083` detaches it, at the cost of a few GB.

## PATCH, not RFC

v1 was `[RFC PATCH]` because no frame had come out. The driver now gives
correct images on the Duo S, so v2 is `[PATCH v2]`. The version number
continues from the RFC. To post as RFC anyway, regenerate with `--rfc`.

## Changes since RFC v1

Taken from `git diff 9774494fcb68 1f7b5374c1f6` (v1 to v2):

- Link frequency 250 MHz at 24 MHz MCLK (PLL = 50 x MCLK), not
  185.625 MHz. HTS 2962, not 2200.
- 23.76 MHz MCLK accepted too, link frequency selected from the MCLK rate.
  Probe fails for other rates, or when `link-frequencies` lacks the match.
  v1 only warned.
- MIPI output enable (`0x023e`) moved out of the mode table: written after
  the controls on stream on, cleared first on stream off.
- Flip controls grabbed while streaming.
- `pm_runtime_put_autosuspend()`, autosuspend undone on the probe error
  path and in remove.
- Binding: `$ref` to video-interface-devices, `unevaluatedProperties`,
  example node `camera@37` with 250 MHz, MCLK description.

The old version of this file listed "the vendor's resume-from-standby
sequence is no longer applied on stream-on" as a change. It is not one: v1
did not apply it either. It went into the Armbian copy by mistake for a
while (runs 1 and 2 in `j1-test.md`).

## Checks run (2026-10-18, on the `gc2083` branch)

| check | how | result |
| --- | --- | --- |
| `dt_binding_check` | `PATH=<linux-gc2083>/.dtschema-venv/bin:$PATH make dt_binding_check DT_SCHEMA_FILES=galaxycore,gc2083.yaml` | clean |
| `checkpatch --strict` | `./scripts/checkpatch.pl --strict --git db2ddb871435..HEAD` | 2/2 clean. 1/2 has only the "does MAINTAINERS need updating?" warning; MAINTAINERS is updated in 2/2, the usual split |
| build | `make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- W=1 drivers/media/i2c/gc2083.o` off `allmodconfig` | no warnings |
| apply | `git am` of the two edited patch files on `db2ddb871435` | applies, tree identical to `gc2083` |

`checkpatch` prints a `ModuleNotFoundError: ply` traceback from
`spdxcheck.py`. That is a missing system package, not a defect: both new files
carry SPDX tags.

`CHECK_DTBS` was not run and cannot be: the series adds no DTS. The SG2000
receiver is not in mainline, and a sensor node with no remote endpoint does
not probe.

## Before sending

1. **Fill in the v1 link** in the cover letter
   (`https://lore.kernel.org/linux-media/<v1-message-id>/`). The message ID of
   the v1 posting is not recorded here.
2. **Rebase onto `media-committers.git` `next`**, not onto a mainline -rc and
   not onto linux-next. `Documentation/driver-api/media/maintainer-entry-profile.rst`:
   a new driver is a "feature for the next mainline release", and for those
   "baseline shall be the ``media-committers.git next`` branch".

       https://gitlab.freedesktop.org/linux-media/media-committers.git  (branch: next)

   That tree "can be rebased, although only as a last resort", so re-check
   the base shortly before posting. Conflicts, if any, are in
   `drivers/media/i2c/Kconfig` and `Makefile`, where other new sensor drivers
   land. After the rebase, regenerate with the new base, update the
   "Based on" line in the cover letter, and re-run the checks above.
3. **Optional, but it would close an open question:** capture one frame with
   `hflip` and one with `vflip` set, and check the image. Then drop that item
   from the cover letter.

## Sending

Cc list from `get_maintainer.pl --no-git-fallback`:

```
sakari.ailus@linux.intel.com
mchehab@kernel.org
robh@kernel.org
krzk+dt@kernel.org
conor+dt@kernel.org
chen.wang@linux.dev
inochiama@gmail.com
linux-media@vger.kernel.org
devicetree@vger.kernel.org
sophgo@lists.linux.dev
linux-kernel@vger.kernel.org
```

Also Cc everyone who replied to v1, and `laurent.pinchart@ideasonboard.com`,
who reviews most new sensor drivers. Re-run `get_maintainer.pl` after the
rebase.

```
git send-email --to=linux-media@vger.kernel.org \
    --cc=... \
    patches/upstream-v2/*.patch
```
