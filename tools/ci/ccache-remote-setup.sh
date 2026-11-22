#!/usr/bin/env bash
# Validates CCACHE_REMOTE_STORAGE for a CI job and exports the normalised value
# through $GITHUB_ENV, or clears it when the value is unusable.
{ set +x; } 2>/dev/null
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tools/ci/ccache-remote-layout.sh
source "${here}/ccache-remote-layout.sh"

if [[ -z "${CCACHE_REMOTE_STORAGE:-}" ]]; then
  echo "remote ccache: off"
  exit 0
fi
echo "::add-mask::${CCACHE_REMOTE_STORAGE}"
normalised="$(ccache_remote_with_bazel_layout "${CCACHE_REMOTE_STORAGE}")"
if [[ -z "${normalised}" ]]; then
  echo "remote ccache: unsupported value, off"
  echo "CCACHE_REMOTE_STORAGE=" >>"${GITHUB_ENV}"
  exit 0
fi
echo "::add-mask::${normalised}"
host="$(ccache_remote_host "${normalised}")"
[[ -z "${host}" ]] || echo "::add-mask::${host}"
# ccache parses the remote URL only when it stores or fetches, so probe with a compile.
probe_dir="$(mktemp -d)"
echo 'int natron_ccache_probe;' >"${probe_dir}/probe.c"
if ! (cd "${probe_dir}" && mkdir ccache-probe-dir \
  && CCACHE_DIR="${probe_dir}/ccache-probe-dir" CCACHE_REMOTE_STORAGE="${normalised}" \
    ccache cc -c probe.c -o probe.o) >/dev/null 2>&1; then
  rm -rf -- "${probe_dir:?}"
  echo "remote ccache: config rejected, off"
  echo "CCACHE_REMOTE_STORAGE=" >>"${GITHUB_ENV}"
  exit 0
fi
rm -rf -- "${probe_dir:?}"
echo "CCACHE_REMOTE_STORAGE=${normalised}" >>"${GITHUB_ENV}"
case "${normalised}" in
  https://*) echo "remote ccache: https, layout set" ;;
  *) echo "remote ccache: http, layout set" ;;
esac
