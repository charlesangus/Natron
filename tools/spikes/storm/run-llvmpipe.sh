#!/usr/bin/env bash
# Runs the spike under the build/deeprepro GLX-capable Xvfb recipe (llvmpipe).
set -u
cd "$(dirname "$0")/../../.."
LD_LIBRARY_PATH=/usr/lib64 Xvfb :81 -screen 0 1600x1000x24 +extension GLX > /dev/null 2>&1 &
XPID=$!
sleep 2
export LD_LIBRARY_PATH="$(cd "$(dirname "$0")/../../.." && pwd)"/build/deeprepro/gl:/usr/local/lib:/usr/local/lib64
export DISPLAY=:81 LIBGL_ALWAYS_SOFTWARE=1 __GLX_VENDOR_LIBRARY_NAME=mesa
export QT_LOGGING_RULES="qt.qpa.gl=true"
timeout 90 build/spikes/storm/m19spike "$1" "$2" "${@:3}"
echo "spike exit=$?"
kill $XPID
