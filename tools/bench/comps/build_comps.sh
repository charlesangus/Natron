#!/usr/bin/env bash
# Regenerates the .ntp comps in this directory inside the natron-dev container.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
for comp in keying_grade cg_multipass defocus_retime; do
    docker exec -e COMP="$comp" -e REPO="$REPO" -e OFX_PLUGIN_PATH="$REPO/build/assets/Plugins" natron-dev bash -lc \
        'cd "$REPO" && xvfb-run --auto-servernum build/release/Renderer/NatronRenderer -b tools/bench/comps/build_comps.py' 2>&1 \
        | grep -E 'Traceback|Error|rror:' || true
done
ls "$REPO"/tools/bench/comps/*.ntp
