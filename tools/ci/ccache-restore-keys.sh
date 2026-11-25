#!/usr/bin/env bash
# Writes the ccache restore-keys for a CI job to $GITHUB_OUTPUT as `keys`.
#
# The usual order is the branch's own newest entry, then main's. Right after
# main is merged into the branch (or the branch is rebased onto main), the
# branch entry was built against the old main and misses nearly every TU the
# merge touched, while main's entry already holds them. That case is detected
# by the branch's merge base with main having moved since the commit the
# branch entry was built from; it then puts main's prefix first. Any lookup
# failure keeps the usual order, so this can only cost a lookup, never a cache.
#
# Environment:
#   FAMILY         key family, e.g. ccache-linux-debug
#   BRANCH         branch name used in the keys
#   HEAD_SHA       commit being built (the PR head, not its merge commit)
#   EXTRA_KEYS     further restore-keys, one per line, appended as given
#   GITHUB_TOKEN   token with actions: read and contents: read
#   GITHUB_REPOSITORY, GITHUB_REF, GITHUB_API_URL, GITHUB_OUTPUT  from Actions
{ set +x; } 2>/dev/null
set -uo pipefail

branch_prefix="${FAMILY}-${BRANCH}-"
main_prefix="${FAMILY}-main-"
first="${branch_prefix}"
second="${main_prefix}"

api() {
  curl -fsS --max-time 20 \
    -H "Authorization: Bearer ${GITHUB_TOKEN}" \
    -H "Accept: application/vnd.github+json" \
    "${GITHUB_API_URL:-https://api.github.com}/repos/${GITHUB_REPOSITORY}/$1"
}

choose_order() {
  if [[ "${BRANCH}" == "main" ]]; then
    echo "ccache order: main run, main's own entries"
    return
  fi
  local prefix_q ref_q entry run_id entry_sha base_now base_then
  prefix_q="$(jq -rn --arg v "${branch_prefix}" '$v|@uri')"
  ref_q="$(jq -rn --arg v "${GITHUB_REF}" '$v|@uri')"
  # Only entries saved under this run's ref are restorable from it.
  entry="$(api "actions/caches?key=${prefix_q}&ref=${ref_q}&sort=created_at&direction=desc&per_page=1" \
    | jq -r '.actions_caches[0].key // empty')" || entry=""
  if [[ -z "${entry}" ]]; then
    echo "ccache order: no branch entry, branch then main"
    return
  fi
  run_id="${entry##*-}"
  if [[ ! "${run_id}" =~ ^[0-9]+$ ]]; then
    echo "ccache order: branch entry ${entry} has no run id, branch then main"
    return
  fi
  entry_sha="$(api "actions/runs/${run_id}" | jq -r '.head_sha // empty')" || entry_sha=""
  if [[ -z "${entry_sha}" ]]; then
    echo "ccache order: run ${run_id} not found, branch then main"
    return
  fi
  if [[ "${entry_sha}" == "${HEAD_SHA}" ]]; then
    echo "ccache order: branch entry is from this commit, branch then main"
    return
  fi
  base_now="$(api "compare/main...${HEAD_SHA}?per_page=1" | jq -r '.merge_base_commit.sha // empty')" || base_now=""
  base_then="$(api "compare/main...${entry_sha}?per_page=1" | jq -r '.merge_base_commit.sha // empty')" || base_then=""
  if [[ -z "${base_now}" || -z "${base_then}" ]]; then
    echo "ccache order: merge base lookup failed, branch then main"
    return
  fi
  if [[ "${base_now}" != "${base_then}" ]]; then
    echo "ccache order: main merged since ${entry} (merge base ${base_then:0:10} -> ${base_now:0:10}), main then branch"
    first="${main_prefix}"
    second="${branch_prefix}"
    return
  fi
  echo "ccache order: ${entry} is on the current merge base, branch then main"
}

choose_order

delim="keys_$(od -An -N8 -tx1 /dev/urandom | tr -d ' \n')"
{
  echo "keys<<${delim}"
  echo "${first}"
  [[ "${second}" == "${first}" ]] || echo "${second}"
  while IFS= read -r line; do
    [[ -n "${line}" ]] && echo "${line}"
  done <<<"${EXTRA_KEYS:-}"
  echo "${delim}"
} >>"${GITHUB_OUTPUT}"
