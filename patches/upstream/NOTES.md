# GC2083 upstream series

Nothing has been posted to linux-media yet. This is the first posting:
`[PATCH 0/2]`, no version number.

Generated on 2026-10-18 from `<linux-gc2083>`, branch `gc2083`
(`1f7b5374c1f6`), based on mainline **v7.2-rc7** (`db2ddb871435`):

```
cd <linux-gc2083>
git format-patch --cover-letter --base=db2ddb871435 \
    -o <duos-camera>/patches/upstream db2ddb871435..gc2083
```

The cover letter text was written by hand afterwards. Keep it when
regenerating. Patches 1 and 2 are format-patch output, unedited.

The binding and the driver are byte-identical to `galaxycore,gc2083.yaml` and
`gc2083.c` at the top of this repository, and to the copies in
sg200x-camera-dkms that Armbian builds. Only the commit messages are extra.

`<linux-gc2083>` is a `git clone --shared` of Armbian's bare kernel cache, so
its objects live in `build/cache/git-bare/kernel`. If Armbian ever
garbage-collects that repository the clone breaks. `git repack -a -d` inside
`linux-gc2083` detaches it, at the cost of a few GB.

## Checks run (2026-10-18, on the `gc2083` branch)

| check | how | result |
| --- | --- | --- |
| `dt_binding_check` | `PATH=<linux-gc2083>/.dtschema-venv/bin:$PATH make dt_binding_check DT_SCHEMA_FILES=galaxycore,gc2083.yaml` | clean |
| `checkpatch --strict` | `./scripts/checkpatch.pl --strict --git db2ddb871435..HEAD` | 2/2 clean. 1/2 has only the "does MAINTAINERS need updating?" warning; MAINTAINERS is updated in 2/2, the usual split |
| build | `make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- W=1 drivers/media/i2c/gc2083.o` off `allmodconfig` | no warnings |
| apply | `git am` of the two patches on `db2ddb871435` | applies, tree identical to `gc2083` |

`checkpatch` prints a `ModuleNotFoundError: ply` traceback from
`spdxcheck.py`. That is a missing system package, not a defect: both new files
carry SPDX tags.

`CHECK_DTBS` was not run and cannot be: the series adds no DTS. The SG2000
receiver is not in mainline, and a sensor node with no remote endpoint does
not probe.

## Before sending

1. **Rebase onto `media-committers.git` `next`**, not onto a mainline -rc and
   not onto linux-next. `Documentation/driver-api/media/maintainer-entry-profile.rst`:
   a new driver is a "feature for the next mainline release", and for those
   "baseline shall be the ``media-committers.git next`` branch".

       https://gitlab.freedesktop.org/linux-media/media-committers.git  (branch: next)

   That tree "can be rebased, although only as a last resort", so re-check
   the base shortly before posting. Conflicts, if any, are in
   `drivers/media/i2c/Kconfig` and `Makefile`, where other new sensor drivers
   land. After the rebase, regenerate with the new base, update the
   "Based on" line in the cover letter, and re-run the checks above.
2. **Optional, but it would close an open question:** capture one frame with
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

Also worth adding: `laurent.pinchart@ideasonboard.com`, who reviews most new
sensor drivers. Re-run `get_maintainer.pl` after the rebase.

```
git send-email --to=linux-media@vger.kernel.org \
    --cc=... \
    patches/upstream/*.patch
```
