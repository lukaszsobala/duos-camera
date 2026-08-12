# GC2083 upstream RFC v1

Generated from `~/Software/armbian/linux-gc2083`, branch `gc2083`, based on
mainline **v7.2-rc7** (`db2ddb871435`). That tree is a `git clone --shared` of
Armbian's bare kernel cache, so its objects live in
`build/cache/git-bare/kernel` — if Armbian ever garbage-collects that repo the
clone breaks. `git repack -a -d` inside `linux-gc2083` detaches it at the cost
of a few GB.

These are *not* the Armbian build patches. `../0058-*` and `../0059-*` are the
Armbian-numbered copies and are left alone.

## Checks run

| check | how | result |
| --- | --- | --- |
| `dt_binding_check` | `PATH=$PWD/.dtschema-venv/bin:$PATH make dt_binding_check DT_SCHEMA_FILES=galaxycore,gc2083.yaml` | clean |
| `checkpatch --strict` | `./scripts/checkpatch.pl --strict --git db2ddb871435..HEAD` | clean apart from the "does MAINTAINERS need updating?" false positive on 1/2 — MAINTAINERS is updated in 2/2, which is the usual split |
| build | `make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- W=1 drivers/media/i2c/gc2083.o` off `allmodconfig` | no warnings |

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

- `devm_v4l2_sensor_clk_get_legacy()` → `devm_v4l2_sensor_clk_get()`. The legacy
  helper's own kernel-doc says it "must not be used in any new driver", and on
  an OF platform it silently calls `clk_set_rate(24MHz)` and ignores what the
  clock actually landed on — which is exactly how the 23.76MHz on this board
  went unnoticed. The non-legacy helper leaves the rate to
  `assigned-clock-rates` in DT.
- Probe now reads `clk_get_rate()` and warns if it is not 24MHz.
- The binding's `clocks` description says the timings assume 24MHz rather than
  asserting the clock is 24MHz.
