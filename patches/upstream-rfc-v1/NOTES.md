# GC2083 upstream RFC v1

Generated from `<linux-gc2083>`, branch `gc2083`, based on
mainline **v7.2-rc7** (`db2ddb871435`). That tree is a `git clone --shared` of
Armbian's bare kernel cache, so its objects live in
`build/cache/git-bare/kernel` — if Armbian ever garbage-collects that repo the
clone breaks. `git repack -a -d` inside `linux-gc2083` detaches it at the cost
of a few GB.

These are *not* the Armbian build patches. Armbian builds the driver from
`leafpmpmp/sg200x-camera-dkms` (since 2026-10-18). The old Armbian copies
`0910-*` and `0911-*` are in the git history of this repository.

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
    patches/upstream-rfc-v1/*.patch
```

Marked RFC deliberately — see the cover letter. To post as a plain v1 instead,
regenerate without `--rfc`:

```
cd <linux-gc2083>
git format-patch --cover-letter -o <dir> db2ddb871435..HEAD
```

and re-apply the cover letter text.

## Changed from the Armbian copies

The three differences this section used to list — the non-legacy
`devm_v4l2_sensor_clk_get()`, the `clk_get_rate()` warning in probe, and the
binding wording about 24MHz — were all folded back into the Armbian copies by
the refresh, so the two have converged. Since 2026-10-15 the Armbian copy has
no `stream_on_restart` parameter either, so the binding and the driver are
byte-identical to this series. Only the commit messages differ: the Armbian
copies carry short ones.

## Baseline while the work continues

**Mainline, and deliberately not the media tree.** Decided 2026-09-20: the
series stays on a mainline base (currently v7.2-rc7) until it is ready to
post. Hardware testing runs the Armbian copy, built against the 7.2 and 7.3
branches, not this tree. This tree only generates the postable series.

Since 2026-10-15 the driver gives correct frames on the PR #10936 receiver,
so the "make it work first" condition is met.

## Before sending

- **This is a v2.** Confirmed 2026-09-20: the original RFC v1 was posted, so the
  next posting is `--subject-prefix="RFC PATCH v2"` with a changelog under the
  `---` line of each patch (and in the cover letter). Changes since v1:
  - the vendor's resume-from-standby sequence is no longer applied on
    stream-on;
  - link frequency 247.5/250 MHz (500 Mbit/s per lane), selected from the
    MCLK rate, not 185.625 MHz; HTS 2962;
  - MIPI output (`0x023e`) written after the controls on stream-on;
  - flips grabbed while streaming; runtime PM puts use autosuspend;
  - binding: `$ref` to video-interface-devices, node name `camera@37`.
  The cover letter must also say that frames now come out (RFC v1 said
  none had), on the Duo S with the PR #10936 receiver.
- **Then, and only then, rebase onto `media-committers.git` `next`** — not onto
  a mainline -rc, and not onto linux-next. The media subsystem states its own
  baseline rule in
  `Documentation/driver-api/media/maintainer-entry-profile.rst`: a new driver is
  a "feature for the next mainline release", and for those "baseline shall be
  the ``media-committers.git next`` branch". The mainline -rc baseline is only
  for bug fixes against the *current* release, which this is not. linux-next is
  not mentioned in that profile at all — it is a daily integration tree for
  testing, not a submission base, and basing on it would hand the maintainer a
  series they cannot apply.

      https://gitlab.freedesktop.org/linux-media/media-committers.git  (branch: next)

  Note the profile warns that tree "can be rebased, although only as a last
  resort", so re-check the base shortly before posting.
- Re-run the three checks above after rebasing. `drivers/media/i2c/Kconfig` and
  `Makefile` are where a conflict would show up, since other new sensor drivers
  land in the same two lists.
