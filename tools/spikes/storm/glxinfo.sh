#!/usr/bin/env bash
LD_LIBRARY_PATH=/usr/lib64 Xvfb :82 -screen 0 1600x1000x24 +extension GLX > /dev/null 2>&1 &
XPID=$!
sleep 2
export LD_LIBRARY_PATH="$(cd "$(dirname "$0")/../../.." && pwd)"/build/deeprepro/gl:/usr/local/lib:/usr/local/lib64
export DISPLAY=:82 LIBGL_ALWAYS_SOFTWARE=1 __GLX_VENDOR_LIBRARY_NAME=mesa
glxinfo -B 2>&1 | grep -i "version\|profile\|renderer"
kill $XPID
