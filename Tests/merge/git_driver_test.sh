#!/usr/bin/env bash
# Merges .ntpz projects through git with natron-merge as the merge driver and textconv: a merge of
# independent edits succeeds, a merge of clashing edits fails with the conflicts in the bundle, a
# later merge keeps those conflicts, and two branches that each add the file merge.
#
# usage: git_driver_test.sh <natron-merge> <merge_scenario_writer>
set -euo pipefail

natron_merge=$(readlink -f "$1")
writer=$(readlink -f "$2")
work=$(mktemp -d "${TMPDIR:-/tmp}/natron-merge-git.XXXXXX")
trap 'rm -rf -- "${work:?}"' EXIT

export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
"$writer" "$work"

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

members() {
    if command -v unzip >/dev/null 2>&1; then
        unzip -Z1 "$1"
    else
        python3 -c 'import sys, zipfile; print("\n".join(zipfile.ZipFile(sys.argv[1]).namelist()))' "$1"
    fi
}

# Makes a repository whose main branch holds <case>-base then <case>-mine, and whose theirs
# branch holds <case>-base then <case>-theirs, then merges theirs into main. Prints the repo path.
setup() {
    local name=$1 repo="$work/$1"
    git init -q -b main "$repo"
    git -C "$repo" config user.name "natron-merge test"
    git -C "$repo" config user.email "natron-merge-test@localhost"
    git -C "$repo" config merge.natron.name "Natron project merge"
    git -C "$repo" config merge.natron.driver "\"$natron_merge\" git-driver %O %A %B"
    git -C "$repo" config diff.natron.textconv "\"$natron_merge\" textconv"
    printf '*.ntpz -text merge=natron diff=natron\n' > "$repo/.gitattributes"
    cp "$work/$name-base.ntpz" "$repo/shot.ntpz"
    git -C "$repo" add .gitattributes shot.ntpz
    git -C "$repo" commit -q -m base
    git -C "$repo" checkout -q -b theirs
    cp "$work/$name-theirs.ntpz" "$repo/shot.ntpz"
    git -C "$repo" commit -q -am theirs
    git -C "$repo" checkout -q main
    cp "$work/$name-mine.ntpz" "$repo/shot.ntpz"
    git -C "$repo" commit -q -am mine
}

setup clean
repo="$work/clean"
git -C "$repo" merge -q --no-edit theirs || fail "the clean merge failed"
diff_text=$("$natron_merge" diff "$work/clean-base.ntpz" "$repo/shot.ntpz" || true)
grep -qF "~ KeyGrade (renamed from Grade1)" <<<"$diff_text" || fail "the merge lost mine's rename: $diff_text"
grep -qF "multiply: default → 1.5" <<<"$diff_text" || fail "the merge lost mine's edit: $diff_text"
grep -qF "size: (2, 2) → (4, 4)" <<<"$diff_text" || grep -qF "size: 2 → 4" <<<"$diff_text" || fail "the merge lost theirs' edit: $diff_text"
members "$repo/shot.ntpz" | grep -E '(^|/)merge/conflicts\.yaml$' >/dev/null && fail "a clean merge recorded conflicts"
git -C "$repo" log -1 --format=%s | grep -q "Merge branch 'theirs'" || fail "the clean merge made no merge commit"
git -C "$repo" show --textconv HEAD:shot.ntpz | grep -F "KeyGrade" >/dev/null || fail "textconv showed no node text"

setup conflict
repo="$work/conflict"
if git -C "$repo" merge -q --no-edit theirs >/dev/null 2>&1; then
    fail "a clashing merge succeeded"
fi
git -C "$repo" ls-files -u -- shot.ntpz | grep -q . || fail "git did not mark shot.ntpz conflicted"
members "$repo/shot.ntpz" | grep -E '(^|/)merge/conflicts\.yaml$' >/dev/null || fail "the conflicted bundle has no merge/conflicts.yaml"

# A later merge keeps the conflicts the earlier one saved while their nodes are still there.
git -C "$repo" commit -q -am "keep the merge"
git -C "$repo" checkout -q theirs
cp "$work/conflict-more.ntpz" "$repo/shot.ntpz"
git -C "$repo" commit -q -am more
git -C "$repo" checkout -q main
git -C "$repo" merge -q --no-edit theirs || fail "a merge of an unrelated edit after a conflicted merge failed"
members "$repo/shot.ntpz" | grep -E '(^|/)merge/conflicts\.yaml$' >/dev/null || fail "the second merge dropped the saved conflicts"
diff_text=$("$natron_merge" diff "$work/conflict-base.ntpz" "$repo/shot.ntpz" || true)
grep -qF "multiply: default → 2" <<<"$diff_text" || fail "the second merge lost theirs' edit: $diff_text"

# Both branches add the file, so git hands the driver an empty ancestor.
repo="$work/addadd"
git init -q -b main "$repo"
git -C "$repo" config user.name "natron-merge test"
git -C "$repo" config user.email "natron-merge-test@localhost"
git -C "$repo" config merge.natron.name "Natron project merge"
git -C "$repo" config merge.natron.driver "\"$natron_merge\" git-driver %O %A %B"
printf '*.ntpz -text merge=natron diff=natron\n' > "$repo/.gitattributes"
git -C "$repo" add .gitattributes
git -C "$repo" commit -q -m attributes
git -C "$repo" checkout -q -b theirs
cp "$work/addadd-theirs.ntpz" "$repo/shot.ntpz"
git -C "$repo" add shot.ntpz
git -C "$repo" commit -q -m theirs
git -C "$repo" checkout -q main
cp "$work/addadd-mine.ntpz" "$repo/shot.ntpz"
git -C "$repo" add shot.ntpz
git -C "$repo" commit -q -m mine
git -C "$repo" merge -q --no-edit theirs || fail "the add/add merge failed"
diff_text=$("$natron_merge" diff "$work/addadd-mine.ntpz" "$repo/shot.ntpz" || true)
grep -qF "+ TheirBlur (Blur)" <<<"$diff_text" || fail "the add/add merge lost theirs' node: $diff_text"
grep -qF "MineBlur" <<<"$diff_text" && fail "the add/add merge changed mine's node: $diff_text"

echo "PASS"
