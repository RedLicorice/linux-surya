#!/usr/bin/env python3
# raw2pgm: last RAW10-packed frame of a v4l2-ctl capture -> downscaled 8-bit PGM (high bytes,
# every STEP-th pixel and line), to look at a sensor's raw output without any ISP.
#   usage: raw2pgm.py RAW WIDTH HEIGHT BYTESPERLINE OUT.pgm [STEP]
import sys
raw, w, h, bpl, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
step = int(sys.argv[6]) if len(sys.argv) > 6 else 8
size = bpl * h
with open(raw, 'rb') as f:
    f.seek(-size, 2)
    b = f.read(size)
rows = []
for y in range(0, h, step):
    line = b[y * bpl:(y + 1) * bpl]
    # pixel x lives in byte (x // 4) * 5 + x % 4 (high 8 bits)
    rows.append(bytes(line[(x // 4) * 5 + x % 4] for x in range(0, w, step)))
with open(out, 'wb') as f:
    f.write(b'P5\n%d %d\n255\n' % (len(rows[0]), len(rows)))
    f.write(b''.join(rows))
