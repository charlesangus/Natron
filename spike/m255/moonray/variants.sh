#!/usr/bin/env bash
# Renders transparency variants of the probe scene in batch moonray, each with a deep beauty output
# beside the flat beauty, and tabulates deep samples and flatten(deep) vs flat error per variant.
# Needs out/progressive_scene.rdla from run.sh.
#
#   variants.sh [--size N] [--pixel-samples N] [--prefix /opt/moonray] [--only tag,tag]
set -u
here="$(cd "$(dirname "$0")" && pwd)"
image="${NATRON_DEV_IMAGE:-ghcr.io/charlesangus/natron-dev:6c97a956d295d586}"
size=128
pixel_samples=4
prefix=/opt/moonray
only=""
extra_mounts=()
while [ $# -gt 0 ]; do
    case "$1" in
        --size) size="$2"; shift ;;
        --pixel-samples) pixel_samples="$2"; shift ;;
        --prefix) prefix="$2"; shift ;;
        --only) only="$2"; shift ;;
        *) echo "unknown argument $1"; exit 2 ;;
    esac
    shift
done
base="$here/out/progressive_scene.rdla"
[ -f "$base" ] || { echo "missing $base; run run.sh first"; exit 1; }
vd="$here/out/variants"
mkdir -p "$vd"
case "$prefix" in
    /opt/moonray) ;;
    *) extra_mounts=(-v "$prefix:$prefix:ro") ;;
esac
# MOONRAY_PBR_LIB replaces the image's path integrator library, as build.sh does with a patched one.
if [ -n "${MOONRAY_PBR_LIB:-}" ]; then
    extra_mounts+=(-v "$MOONRAY_PBR_LIB:$prefix/lib64/librendering_pbr.so:ro")
fi

cards=(cardNear cardMid cardFar)

glass() {
    local thin="$1"
    for c in "${cards[@]}"; do
        cat <<EOF
DwaBaseMaterial("/World/mtl/$c/dwa") { ["presence"] = 1, ["transmission"] = 1, ["refractive_index"] = 1.0, ["thin_geometry"] = $thin, ["roughness"] = 0 }
EOF
    done
}

variant_override() {
    case "$1" in
        presence_vec|presence_scalar) ;;
        presence_vec_ztol) echo 'SceneVariables { ["deep_z_tolerance"] = 0.01 }' ;;
        presence_vec_maxdepth0) echo 'SceneVariables { ["max_presence_depth"] = 0 }' ;;
        presence_vec_dcx) echo 'SceneVariables { ["deep_format"] = "opendcx2.0" }' ;;
        glass_thin_vec|glass_thin_scalar) glass true ;;
        glass_solid_vec) glass false ;;
        opaque_vec) for c in "${cards[@]}"; do echo "DwaBaseMaterial(\"/World/mtl/$c/dwa\") { [\"presence\"] = 1 }"; done ;;
    esac
}

variant_mode() {
    case "$1" in
        *_scalar) echo scalar ;;
        *) echo auto ;;
    esac
}

tags=(opaque_vec presence_vec presence_scalar presence_vec_ztol presence_vec_dcx presence_vec_maxdepth0
      glass_thin_vec glass_thin_scalar glass_solid_vec)
[ -n "$only" ] && IFS=, read -r -a tags <<< "$only"

script="set -e
export RDL2_DSO_PATH=$prefix/rdl2dso PATH=$prefix/bin:\$PATH
"
for t in "${tags[@]}"; do
    {
        cat <<EOF
SceneVariables { ["image_width"] = $size, ["image_height"] = $size, ["pixel_samples"] = $pixel_samples }
RenderOutput("/_outputs/beauty") { ["file_name"] = "$vd/${t}_deep.exr" }
RenderOutput("/_outputs/alpha") { ["file_name"] = "$vd/${t}_alpha.exr" }
EOF
        variant_override "$t"
    } > "$vd/$t.rdla"
    rm -f "$vd/${t}_deep.exr" "$vd/${t}_main.exr"
    script+="moonray -in $base -in $vd/$t.rdla -out $vd/${t}_main.exr -exec_mode $(variant_mode "$t") \
-threads \$(nproc) -auto_affinity off -mem_affinity off > $vd/$t.log 2>&1 || echo 'render $t failed'
"
done
script+="python3 -I $here/variant_stats.py $vd ${tags[*]}"

docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "${here}:${here}" "${extra_mounts[@]}" -w "$here" \
    --entrypoint bash "$image" -c "$script" 2>&1 \
    | grep -v -E 'CUDA|NVIDIA|^$|=====|license|Container|GPU|docs.nvidia'
