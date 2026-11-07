#!/usr/bin/env bash
# Renders gen_placement.py's modes with usdrecord under Xvfb (llvmpipe), in natron-dev, and compares
# each against the one-material-per-card reference. Output in build/m341-spike/.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="$root/build/m341-spike"
mkdir -p "$out"
python3 -c '
import sys, numpy as np, OpenImageIO as oiio
y, x = np.mgrid[0:288, 0:512]
img = np.where(((x // 32 + y // 32) % 2)[..., None] == 0, [1., 0., 0.], [0., 0., 1.]).astype(np.float32)
img[144:, :256] = 1.
o = oiio.ImageOutput.create(sys.argv[1])
o.open(sys.argv[1], oiio.ImageSpec(512, 288, 3, "uint8"))
o.write_image(img)
o.close()' "$out/plate.png"
shader="$root/Engine/Nodes/Scene/shaders/natronProject3D.glslfx"
LD_LIBRARY_PATH=/usr/lib64 Xvfb :91 -screen 0 1600x1000x24 +extension GLX > /dev/null 2>&1 &
XPID=$!
sleep 2
export LD_LIBRARY_PATH="/home/bosley/git/Natron/build/deeprepro/gl:/usr/local/lib:/usr/local/lib64"
export DISPLAY=:91 LIBGL_ALWAYS_SOFTWARE=1 __GLX_VENDOR_LIBRARY_NAME=mesa
for spec in "$@"; do
    IFS=: read -r name mode instances glslfx <<< "$spec"
    glslfx="${glslfx:-$shader}"
    python3 "$here/gen_placement.py" "$out/$name.usda" "$out/plate.png" "$glslfx" "$mode" "$instances" || continue
    echo "== $name"
    timeout 60 usdrecord --camera /World/View --frames 1:2 --imageWidth 640 --renderer GL \
        "$out/$name.usda" "$out/$name.#.png" 2>&1 | grep -vE "^$|Warning: Unused|Recording" | cut -c1-300 | head -20
done
kill $XPID
