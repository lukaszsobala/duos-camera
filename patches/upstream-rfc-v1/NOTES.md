# GC2083 upstream RFC v1

Generated from `~/Software/armbian/linux-gc2083`, branch `gc2083`, based on
mainline **v7.2-rc7** (`db2ddb871435`). That tree is a `git clone --shared` of
Armbian's bare kernel cache, so its objects live in
`build/cache/git-bare/kernel` — if Armbian ever garbage-collects that repo the
clone breaks. `git repack -a -d` inside `linux-gc2083` detaches it at the cost
of a few GB.

These are *not* the Armbian build patches. `../0900-*` and `../0901-*` are the
Armbian-numbered copies and are left alone.

## Checks run

| check | how | result |
| --- | --- | --- |
| `dt_binding_check` | `PATH=$PWD/.dtschema-venv/bin:$PATH make dt_binding_check DT_SCHEMA_FILES=galaxycore,gc2083.yaml` | clean |
| `checkpatch --strict` | `./scripts/checkpatch.pl --strict --git db2ddb871435..HEAD` | clean apart from the "does MAINTAINERS need updating?" false positive on 1/2 — MAINTAINERS is updated in 2/2, which is the usual split |
| build | `make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- W=1 drivers/media/i2c/gc2083.o` off `allmodconfig` | no warnings |

All three were **re-run on 2026-09-20** after the start-sequence fix, with the
same results; checkpatch reports the driver patch as "no obvious style problems
and is ready for submission", 0 errors, 0 warnings, 0 checks over 1242 lines.

dtschema lives in a venv at `linux-gc2083/.dtschema-venv` because the system
Python has no pip. `checkpatch` prints a `ModuleNotFoundError: ply` traceback
from `spdxcheck.py`; that is a missing system package, not a defect in the
patches, and both new files carry SPDX tags.

`CHECK_DTBS` was **not** run and cannot be: the series adds no DTS, because a
sensor node with nothing at the far end of its graph does not probe.

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

Worth adding by hand: `laurent.pinchart@ideasonboard.com`, who reviews most new
sensor drivers.

```
git send-email --to=linux-media@vger.kernel.org \
    --cc=... \
    ~/Software/armbian/duos-camera/patches/upstream-rfc-v1/*.patch
```

Marked RFC deliberately — see the cover letter. To post as a plain v1 instead,
regenerate without `--rfc`:

```
cd ~/Software/armbian/linux-gc2083
git format-patch --cover-letter -o <dir> db2ddb871435..HEAD
```

and re-apply the cover letter text.

## Changed from the Armbian copies

The three differences this section used to list — the non-legacy
`devm_v4l2_sensor_clk_get()`, the `clk_get_rate()` warning in probe, and the
binding wording about 24MHz — were all folded back into the Armbian copies by
the refresh, so the two have converged. As of 2026-09-20 the binding is
byte-identical and the driver differs in exactly one way:

- The Armbian copy keeps the old resume-from-standby sequence behind a
  `stream_on_restart` module parameter, defaulting off, so both behaviours can
  be compared on hardware without a rebuild. **This series does not have it**,
  per the rule that bring-up scaffolding does not go upstream. If that parameter
  is ever removed from the Armbian copy, the two files become identical again.

## Still to decide before sending

- **The base is still v7.2-rc7** while mainline has moved on. Rebase onto the
  current -rc before posting, and re-run the three checks above.
- **Version numbering.** These were regenerated in place as `RFC v1`. If the
  original v1 was ever actually posted, this is a v2 and needs `-v2` plus a
  changelog under the `---` line saying the stream-on sequence was dropped.
