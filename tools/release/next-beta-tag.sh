#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 0 ]]; then
    echo "Usage: next-beta-tag.sh" >&2
    exit 1
fi

tags="$(git tag -l 'v0.1.0-beta*')"

max=0
while IFS= read -r tag; do
    [[ -z "$tag" ]] && continue
    if [[ "$tag" =~ ^v0\.1\.0-beta([0-9]+)$ ]]; then
        # 10# forces decimal: an unprefixed "08" is invalid octal and aborts the script.
        n=$((10#${BASH_REMATCH[1]}))
        if (( n > max )); then
            max="$n"
        fi
    fi
done <<< "$tags"

echo "v0.1.0-beta$((max + 1))"
