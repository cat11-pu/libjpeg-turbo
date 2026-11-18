#!/bin/bash
# Reproduce all region-decode consistency data.
# Requires a configured libjpeg-turbo build at ../build (see README.md).
set -eu

cd "$(dirname "$0")"
B=../build

make


mkdir -p jpgs
SRC=../testimages/vgl_6548_0026a.bmp
$B/cjpeg -grayscale              -outfile jpgs/gray.jpg    "$SRC"
$B/cjpeg -sample 2x2             -outfile jpgs/420.jpg     "$SRC"
$B/cjpeg -sample 2x1             -outfile jpgs/422.jpg     "$SRC"
$B/cjpeg -sample 1x2             -outfile jpgs/440.jpg     "$SRC"
$B/cjpeg -sample 1x1             -outfile jpgs/444.jpg     "$SRC"
$B/cjpeg -progressive -sample 2x2 -outfile jpgs/420prog.jpg "$SRC"

# A wider image so iMCU-aligned decode strips can have BOTH edges internal
# to the image (needed to exercise the documented fancy-upsampling edge case).
python3 - "$SRC" <<'PY'
import sys
w, h = 512, 384
with open("/tmp/wide.ppm", "wb") as f:
    f.write(f"P6\n{w} {h}\n255\n".encode())
    for y in range(h):
        row = bytearray()
        for x in range(w):
            row += bytes(((x * 7 + y * 3) & 255,
                          (x * 5 + y * 11) & 255,
                          (x * 13 + y * 17) & 255))
        f.write(row)
PY
$B/cjpeg -sample 2x2 -outfile jpgs/wide420.jpg /tmp/wide.ppm
$B/cjpeg -sample 2x1 -outfile jpgs/wide422.jpg /tmp/wide.ppm

./rdec_test jpgs/gray.jpg jpgs/420.jpg jpgs/422.jpg jpgs/440.jpg \
            jpgs/444.jpg jpgs/420prog.jpg jpgs/wide420.jpg \
            jpgs/wide422.jpg | tee test_results.txt

echo
echo "Cross-check against djpeg -crop (exact window vs. iMCU strip):"
./rdec -n -s 4 -r 30x20+17+9 jpgs/wide420.jpg /tmp/rdec.ppm
$B/djpeg -nosmooth -scale 4/8 -crop 30x20+17+9 -rgb \
         -outfile /tmp/djpeg.ppm jpgs/wide420.jpg
python3 - <<'PY'
import sys
a = open("/tmp/rdec.ppm", "rb").read()
b = open("/tmp/djpeg.ppm", "rb").read()
ha, hb = b"P6\n31 20\n255\n", b"P6\n30 20\n255\n"
pa, pb = a[len(hb):], b[len(ha):]
diffs = sum(pa[y * 90:(y + 1) * 90] !=
            pb[y * 93:(y + 1) * 93][3:] for y in range(20))
print(f"djpeg cross-check rows differing: {diffs}")
sys.exit(1 if diffs else 0)
PY
