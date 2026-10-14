#!/usr/bin/env python3
"""Convert frames from cv1800-vi (packed RAW12) to PNG, on the host.

  raw12-to-png.py cap.raw [width height bayer ccm]
  (default 1920 1080 RGGB 3915)

Writes cap-<n>.png per frame: a half-size colour image (one RGB pixel per
2x2 Bayer cell), black level removed, gray-world white balance, the vendor's
colour correction matrix for the colour temperature `ccm` in kelvin (0 for
none), stretched so the 99.5th percentile is white, gamma 2.2. Prints per-channel means of the
raw data, so a flat or corrupt frame shows without opening the PNGs.
"""
import sys

import numpy as np
from PIL import Image

BLACK = 64 << 2  # GC2083 black level, 10-bit 64 in the 12-bit container

# Colour correction matrices by colour temperature, Q10, from the vendor's
# tuning file cvi_sdr_bin_GC2083 (offset 0x1b21c, 20 bytes per entry)
CCM = {
    7221: [2222, -834, -364, -353, 1797, -420, 3, -731, 1752],
    5975: [2308, -759, -525, -365, 1828, -439, 3, -806, 1827],
    4786: [2274, -669, -581, -382, 1848, -442, 16, -1004, 2012],
    3915: [2111, -409, -678, -513, 1910, -373, -74, -808, 1906],
    2667: [2332, -657, -651, -538, 1888, -326, -66, -1826, 2916],
}


def main():
    path = sys.argv[1]
    w = int(sys.argv[2]) if len(sys.argv) > 2 else 1920
    h = int(sys.argv[3]) if len(sys.argv) > 3 else 1080
    bayer = sys.argv[4].upper() if len(sys.argv) > 4 else "RGGB"
    kelvin = int(sys.argv[5]) if len(sys.argv) > 5 else 3915
    ccm = np.array(CCM[kelvin]).reshape(3, 3) / 1024 if kelvin else np.eye(3)

    stride = (w * 3 // 2 + 15) // 16 * 16
    frame_bytes = stride * h
    data = np.fromfile(path, dtype=np.uint8)
    frames = len(data) // frame_bytes
    print(f"{frames} frames of {w}x{h}, stride {stride}")

    # Index of each colour in a 2x2 cell, row-major
    pos = {c: [i for i, b in enumerate(bayer) if b == c] for c in "RGB"}

    for n in range(frames):
        raw = data[n * frame_bytes:(n + 1) * frame_bytes]
        raw = raw.reshape(h, stride)[:, :w * 3 // 2].reshape(h, w // 2, 3)
        b0, b1, b2 = (raw[..., i].astype(np.uint16) for i in range(3))
        # MIPI RAW12: two MSB bytes, then a byte with both low nibbles
        px = np.empty((h, w), dtype=np.uint16)
        px[:, 0::2] = (b0 << 4) | (b2 & 0x0f)
        px[:, 1::2] = (b1 << 4) | (b2 >> 4)

        cells = [px[0::2, 0::2], px[0::2, 1::2], px[1::2, 0::2], px[1::2, 1::2]]
        rgb = np.stack([np.mean([cells[i] for i in pos[c]], axis=0)
                        for c in "RGB"], axis=-1) / 4095.0
        means = rgb.reshape(-1, 3).mean(axis=0) * 4095
        print(f"frame {n}: mean R {means[0]:.0f} G {means[1]:.0f} "
              f"B {means[2]:.0f} (of 4095), max {px.max()}")

        lin = np.clip(rgb - BLACK / 4095.0, 0, None)
        lin *= lin.reshape(-1, 3).mean(axis=0).mean() / np.maximum(
            lin.reshape(-1, 3).mean(axis=0), 1e-9)
        lin = np.clip(lin @ ccm.T, 0, None)
        lin /= max(np.percentile(lin, 99.5), 1e-9)
        img = (np.clip(lin, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)
        Image.fromarray(img).save(f"{path.rsplit('.', 1)[0]}-{n}.png")


if __name__ == "__main__":
    main()
