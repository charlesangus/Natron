#!/usr/bin/env bash
# Compute the next sequential v0.1.0-betaN tag from the current repo's tags.
# Pure and side-effect-free: prints the next tag to stdout, creates or pushes
# nothing. Caller is responsible for actually tagging and pushing.
set -euo pipefail

if [[ $# -ne 0 ]]; then
    echo "Usage: next-beta-tag.sh" >&2
    exit 1
fi

max=0
while IFS= read -r tag; do
    [[ -z "$tag" ]] && continue
    if [[ "$tag" =~ ^v0\.1\.0-beta([0-9]+)$ ]]; then
        n="${BASH_REMATCH[1]}"
        if (( n > max )); then
            max="$n"
        fi
    fi
done < <(git tag -l 'v0.1.0-beta*')

echo "v0.1.0-beta$((max + 1))"
