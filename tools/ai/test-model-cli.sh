#!/usr/bin/env bash
# Usage: tools/ai/test-model-cli.sh <NatronRenderer binary>
#
# Checks NatronRenderer --list-models and --download-model without the network: the manifest
# (NATRON_AI_MANIFEST) holds one model whose url is a file:// link to
# Tests/fixtures/ai/identity.dtmodel, and models, settings and caches go to a temporary folder.
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="${1:?usage: $0 <NatronRenderer binary>}"
FIXTURE="$ROOT/Tests/fixtures/ai/identity.dtmodel"
WORK="$(mktemp -d)"
trap 'rm -rf -- "${WORK:?}"' EXIT

export XDG_DATA_HOME="$WORK/data"
export XDG_CONFIG_HOME="$WORK/config"
export XDG_CACHE_HOME="$WORK/cache"
export NATRON_AI_MANIFEST="$WORK/manifest.json"

cat > "$NATRON_AI_MANIFEST" <<EOF
{
 "schema": 1,
 "source": {"repository": "test", "tag": "test"},
 "models": [
  {
   "id": "test-identity", "task": "denoise", "name": "Test Identity", "package": "test-identity",
   "files": ["model.onnx"], "url": "file://$FIXTURE",
   "sha256": "eb95dd0d3cfecc9a713799210f0f9fdcb29b1745a8adbf846ddc1b7ad20a92b8", "size": 602,
   "weightsLicence": "MIT", "trainingDataLicence": "none", "osaid": "Open Source AI",
   "cardUrl": "https://example.invalid/card", "default": true
  }
 ]
}
EOF

status=0
fail() {
    echo "FAIL $*"
    status=1
}

out="$("$BIN" --list-models 2>/dev/null)" || fail "--list-models exited with $?"
echo "$out" | grep -q $'^test-identity\tdenoise\tMissing\t602\tMIT\tnone\tOpen Source AI$' \
    || fail "--list-models does not list test-identity as Missing: $out"

"$BIN" --download-model test-identity >/dev/null 2>&1 || fail "--download-model test-identity exited non-zero"
[ -n "$(find "$XDG_DATA_HOME" -path '*/models/test-identity/model.onnx' -print -quit)" ] \
    || fail "the model is not installed under $XDG_DATA_HOME"

out="$("$BIN" --list-models 2>/dev/null)" || fail "--list-models exited with $? after the download"
echo "$out" | grep -q $'^test-identity\tdenoise\tInstalled\t' || fail "test-identity is not Installed after the download: $out"

"$BIN" --download-model no-such-model >/dev/null 2>&1 && fail "--download-model of an unknown id exited 0"
"$BIN" --download-model >/dev/null 2>&1 && fail "--download-model without an id exited 0"

[ "$status" -eq 0 ] && echo "PASS model CLI"
exit "$status"
