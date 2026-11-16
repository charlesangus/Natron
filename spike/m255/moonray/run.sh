#!/usr/bin/env bash
# Renders scene.usda through stock hdMoonray over a local Arras session with a deep RenderOutput,
# dumps the mcrt-side deep buffer at frame completion, renders the same RDL scene in batch moonray,
# then compares and sizes the deep data.
#
#   run.sh [--compile-only] [--size N] [--pixel-samples N]
set -u
here="$(cd "$(dirname "$0")" && pwd)"
image="${NATRON_DEV_IMAGE:-ghcr.io/charlesangus/natron-dev:6c97a956d295d586}"
out="$here/out"
size=256
pixel_samples=8
compile_only=0
while [ $# -gt 0 ]; do
    case "$1" in
        --compile-only) compile_only=1 ;;
        --size) size="$2"; shift ;;
        --pixel-samples) pixel_samples="$2"; shift ;;
        *) echo "unknown argument $1"; exit 2 ;;
    esac
    shift
done
mkdir -p "$out/wrap"

in_image() {
    docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "${here}:${here}" -w "$here" \
        --entrypoint bash "$image" -c "$1"
}

finish() {
    echo "MOONRAY DEEP PROBE: $1"
    exit "$2"
}

cxx=/opt/rh/gcc-toolset-14/root/usr/bin/g++
mr_flags="-mavx2 -D__cdecl= -DPLATFORM_UNIX -DPLATFORM_LINUX -DTBB_SUPPRESS_DEPRECATED_MESSAGES -DBOOST_DISABLE_ASSERTS"
in_image "set -e
py_inc=\$(python3 -c 'import sysconfig; print(sysconfig.get_paths()[\"include\"])')
$cxx -std=gnu++20 -O1 -w -pthread -I/usr/local/include -I\"\$py_inc\" mr_probe.cpp -o out/mr_probe \
    -L/usr/local/lib -lusd_hdx -lusd_hd -lusd_usdImaging -lusd_usdGeom -lusd_usdShade -lusd_usd -lusd_sdf \
    -lusd_pcp -lusd_cameraUtil -lusd_hf -lusd_gf -lusd_vt -lusd_tf -lusd_plug -lusd_arch -lusd_trace \
    -lusd_work -lusd_python -ltbb -lpython3.13 -lOpenImageIO -lOpenImageIO_Util
$cxx -std=gnu++20 -O1 -w -fPIC -shared $mr_flags -I/opt/moonray/include -I/usr/local/include mr_shim.cpp \
    -o out/mr_shim.so -L/opt/moonray/lib64 -Wl,-rpath,/opt/moonray/lib64 -lrendering_rndr -lrendering_pbr -lscene_rdl2 -ldl" \
    > "$out/compile.log" 2>&1 || { tail -30 "$out/compile.log"; finish "FAILED (compile)" 1; }
[ "$compile_only" = 1 ] && finish "COMPILED" 0

# Arras spawns execComp from PATH, so a wrapper first on PATH puts the shim into mcrt only.
cat > "$out/wrap/execComp" <<EOF
#!/bin/sh
LD_PRELOAD="$out/mr_shim.so\${LD_PRELOAD:+:\$LD_PRELOAD}" exec /opt/moonray/bin/execComp "\$@"
EOF
chmod +x "$out/wrap/execComp"

for f in progressive_beauty.exr progressive_alpha.exr progressive_scene.rdla progressive.shim.log \
         progressive.shim.done flat_beauty.exr flat_alpha.exr flat_scene.rdla flat.shim.log flat.shim.done \
         batch_deep.exr batch_alpha.exr batch_main.exr; do
    rm -f "${out:?}/$f"
done

moonray_env="export RDL2_DSO_PATH=/opt/moonray/rdl2dso ARRAS_SESSION_PATH=/opt/moonray/sessions \
ARRAS_OS_DISTRIBUTION=linux ARRAS_OS_VERSION=unknown PATH=$out/wrap:/opt/moonray/bin:\$PATH \
PXR_PLUGINPATH_NAME=/opt/moonray/plugin/pxr"

probe() {
    local tag="$1"; shift
    in_image "$moonray_env M255_SHIM_LOG=$out/$tag.shim.log M255_SHIM_DONE=$out/$tag.shim.done
timeout 1200 ./out/mr_probe --scene scene.usda --out $out --tag $tag --size $size \
    --pixel-samples $pixel_samples --shim-done $out/$tag.shim.done $*" > "$out/$tag.log" 2>&1
    local rc=$?
    grep -E '^render |^  (aov|rmse|mcrt|TF_ERROR|TF_FATAL)' "$out/$tag.log"
    [ -f "$out/$tag.shim.log" ] && sed 's/^/  shim: /' "$out/$tag.shim.log"
    return $rc
}

echo "== Arras progressive, deep RO on beauty + flat alpha"
probe progressive || finish "FAILED (progressive render, see out/progressive.log)" 1
echo "== Arras progressive, same AOVs all flat"
probe flat --no-deep || echo "  flat control failed, see out/flat.log"

cat > "$out/batch_override.rdla" <<EOF
RenderOutput("/_outputs/beauty") { ["file_name"] = "$out/batch_deep.exr" }
RenderOutput("/_outputs/alpha") { ["file_name"] = "$out/batch_alpha.exr" }
EOF
# Docker seccomp rejects the mbind() that batch auto-affinity issues.
echo "== batch moonray on the same RDL"
start=$(date +%s)
in_image "$moonray_env
timeout 1200 moonray -in $out/progressive_scene.rdla -in $out/batch_override.rdla -out $out/batch_main.exr \
    -threads \$(nproc) -auto_affinity off -mem_affinity off" > "$out/batch.log" 2>&1 \
    || { tail -20 "$out/batch.log"; finish "FAILED (batch render, see out/batch.log)" 1; }
echo "  batch seconds=$(( $(date +%s) - start ))"

echo "== analysis"
in_image "python3 -I $here/analyze.py $out" 2>&1 | tee "$out/analyze.log" | grep -v -E 'CUDA|NVIDIA|^$|=====|license|Container|GPU|docs.nvidia'
[ "${PIPESTATUS[0]}" = 0 ] || finish "FAILED (analysis)" 1
finish "OK" 0
