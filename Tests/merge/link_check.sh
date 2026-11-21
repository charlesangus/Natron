#!/usr/bin/env bash
# natron-merge must run where neither Natron nor its runtime is installed: it may need no Qt,
# Python or Natron shared library, and must hold no Engine or Gui code.
#
# usage: link_check.sh <natron-merge>
set -euo pipefail

binary=$1
needed=$(readelf -d "$binary" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
echo "$needed"
if grep -Ei '^lib(Qt|python|shiboken|pyside|Natron)' <<<"$needed"; then
    echo "FAIL: natron-merge needs the libraries above" >&2
    exit 1
fi
if nm -C --defined-only "$binary" 2>/dev/null | grep -E 'Natron::(AppManager|AppInstance|EffectInstance|KnobHelper|NodeGui|Gui)::' >/dev/null; then
    echo "FAIL: natron-merge holds Engine or Gui code" >&2
    exit 1
fi
echo "PASS"
