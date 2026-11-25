#!/usr/bin/env bash
# Prints `ccache -s`, then one line that reads the remote numbers correctly and
# the config that decides whether two runs share remote keys.
#
# ccache counts a remote lookup only after a local miss, so "Remote storage:
# Hits x/y" in `ccache -s` is x of the y local misses, and only the remote
# misses were actually compiled.
{ set +x; } 2>/dev/null
set -uo pipefail

ccache -s

stats="$(ccache --print-stats 2>/dev/null)" || stats=""
stat() {
  awk -v k="$1" '$1 == k { print $2; found = 1 } END { if (!found) print 0 }' <<<"${stats}"
}
local_hit="$(stat local_storage_hit)"
local_miss="$(stat local_storage_miss)"
remote_hit="$(stat remote_storage_hit)"
remote_miss="$(stat remote_storage_miss)"
remote_error="$(stat remote_storage_error)"
remote_timeout="$(stat remote_storage_timeout)"

echo
if [[ -n "${CCACHE_REMOTE_STORAGE:-}" ]]; then
  pct=0
  if [[ "${local_miss}" -gt 0 ]]; then
    pct=$((100 * remote_hit / local_miss))
  fi
  echo "ccache summary: local hits ${local_hit}; remote hits ${remote_hit}/${local_miss} local misses (${pct}%); true compiles ${remote_miss} (remote misses); remote errors ${remote_error}, timeouts ${remote_timeout}"
else
  echo "ccache summary: local hits ${local_hit}; remote off; true compiles ${local_miss} (local misses)"
fi

# The URL and most attributes can carry credentials, so only the scheme, the
# attribute names and the layout value are shown.
ccache -p 2>/dev/null | while IFS= read -r line; do
  case "${line}" in
    *" remote_storage = "*)
      value="${line#* remote_storage = }"
      if [[ -z "${value}" ]]; then
        echo "${line}"
      else
        attrs=""
        rest="${value#*|}"
        if [[ "${rest}" != "${value}" ]]; then
          attrs="$(tr '|' '\n' <<<"${rest}" | sed -E '/^@?layout=/!s/=.*//' | paste -sd '|')"
        fi
        echo "${line%% remote_storage = *} remote_storage = ${value%%://*}://<masked>${attrs:+|${attrs}}"
      fi
      ;;
    *" base_dir = "* | *" hash_dir = "* | *" compiler_check = "*)
      echo "${line}"
      ;;
  esac
done
