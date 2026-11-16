#!/usr/bin/env bash
# Builds and runs rs_probe in the published dev image, which carries hdCycles; the long-running
# natron-dev container may predate it. Output goes to spike/m255/rendersettings/out/.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
image="${NATRON_DEV_IMAGE:-ghcr.io/charlesangus/natron-dev:6c97a956d295d586}"
mkdir -p "${here}/out"

docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "${here}:${here}" -w "${here}" \
    --entrypoint bash "$image" -c '
set -euo pipefail
py_inc="$(python3 -c "import sysconfig; print(sysconfig.get_paths()[\"include\"])")"
/opt/rh/gcc-toolset-14/root/usr/bin/g++ -std=gnu++20 -O1 -w -pthread -I/usr/local/include -I"$py_inc" rs_probe.cpp -o out/rs_probe \
    -L/usr/local/lib -Wl,-rpath,/usr/local/lib \
    -lusd_usdImagingGL -lusd_usdImaging -lusd_hdx -lusd_hd -lusd_hgi -lusd_cameraUtil \
    -lusd_usdRender -lusd_usdLux -lusd_usdShade -lusd_usdGeom -lusd_usd -lusd_sdf -lusd_vt -lusd_gf -lusd_tf \
    -lusd_plug -lusd_arch -lusd_python -ltbb -lpython3.13
./out/rs_probe "$@" > out/probe.log 2> out/probe.stderr.log
' probe "$@"
