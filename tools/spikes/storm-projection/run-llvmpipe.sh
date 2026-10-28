#!/usr/bin/env bash
# Renders the spike scene with usdrecord (UsdImagingGLEngine, Storm) under the build/deeprepro
# GLX-capable Xvfb recipe (llvmpipe), in natron-dev. Output in build/m234-spike/.
set -u
root="$(cd "$(dirname "$0")/../../.." && pwd)"
out="$root/build/m234-spike"
mkdir -p "$out"
# A 16:9 plate whose lower-left quarter is white, to read its orientation.
python3 -c '
import sys, numpy as np, OpenImageIO as oiio
y, x = np.mgrid[0:288, 0:512]
img = np.where(((x // 32 + y // 32) % 2)[..., None] == 0, [1., 0., 0.], [0., 0., 1.]).astype(np.float32)
img[144:, :256] = 1.
out = oiio.ImageOutput.create(sys.argv[1])
out.open(sys.argv[1], oiio.ImageSpec(512, 288, 3, "uint8"))
out.write_image(img)
out.close()' "$out/plate.png"
python3 "$(dirname "$0")/gen.py" "$out" "$out/plate.png" || exit 1
LD_LIBRARY_PATH=/usr/lib64 Xvfb :82 -screen 0 1600x1000x24 +extension GLX > /dev/null 2>&1 &
XPID=$!
sleep 2
export LD_LIBRARY_PATH="/home/bosley/git/Natron/build/deeprepro/gl:/usr/local/lib:/usr/local/lib64"
export DISPLAY=:82 LIBGL_ALWAYS_SOFTWARE=1 __GLX_VENDOR_LIBRARY_NAME=mesa
timeout 90 usdrecord --camera /World/View --frames 1:2 --imageWidth 640 --renderer GL \
    "$out/scene.usda" "$out/projection.#.png"
echo "usdrecord exit=$?"
kill $XPID
