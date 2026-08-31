#!/usr/bin/env bash
#
# Start -- or exec into, if already running -- a long-lived dev container
# built from natron-dev:2027.0 (see tools/ci/local/Dockerfile). The whole
# point of this script is that container start cost (pulling/creating the
# container, warming caches) is paid once, not on every build/test
# invocation, so this always reuses one running container per repo
# checkout rather than starting a fresh one each time.
#
# Usage:
#   tools/ci/local/devshell.sh                 # interactive shell in the container
#   tools/ci/local/devshell.sh <cmd> [args...]  # run a command, propagate its exit code
#   tools/ci/local/devshell.sh --recreate       # force-recreate the container (e.g. after
#                                                # the image changes), then continue as above
#
# The container name defaults to "natron-dev" and can be overridden with
# NATRON_DEV_CONTAINER, e.g. to give a second git worktree of this repo its
# own container and caches:
#
#   NATRON_DEV_CONTAINER=natron-dev-wt2 tools/ci/local/devshell.sh
#
# To fully tear down a container and its caches (rare -- normally you just
# want --recreate, which keeps the ccache/home volumes):
#   docker rm -f "${NATRON_DEV_CONTAINER:-natron-dev}"
#   docker volume rm "${NATRON_DEV_CONTAINER:-natron-dev}-ccache" "${NATRON_DEV_CONTAINER:-natron-dev}-home"
#
# The container this script creates always has NATRON_IN_CONTAINER=1 set (see
# the `docker run -e NATRON_IN_CONTAINER=1 ...` below). build.sh/test.sh use
# that as the explicit, unambiguous "we are already inside the dev container,
# do not re-exec through this script again" signal -- see their comments for
# why this exists alongside (not instead of) inferring the same thing from
# CI.

set -euo pipefail

IMAGE="natron-dev:2027.0"
CONTAINER_NAME="${NATRON_DEV_CONTAINER:-natron-dev}"
CCACHE_VOLUME="${CONTAINER_NAME}-ccache"
HOME_VOLUME="${CONTAINER_NAME}-home"
CCACHE_MOUNT="/ccache"
HOME_MOUNT="/home/devshell"

# ccache's default max_size is 5 GiB. A cold full build of this repo alone
# produces ~550 cacheable objects that overflow that 5 GiB ceiling well
# before the build finishes, so ccache starts evicting entries mid-build
# (confirmed: 193 cleanups logged by `ccache -s` on a single cold build,
# with a resulting hit rate of 0.18%) -- i.e. it thrashes instead of
# accumulating anything reusable. The whole point of this cache is to pay
# off across branch switches (e.g. Qt6-migration branch <-> base branch),
# where the same translation units get rebuilt repeatedly, so it needs
# enough headroom to hold several full build trees at once. A full debug
# build tree is ~4.1 GB, and the Docker filesystem has 116 GB free, so 40
# GiB (roughly ten build trees' worth) comfortably covers realistic
# branch-switch churn without being unbounded or eating the disk.
# Overridable the same way NATRON_DEV_CONTAINER is, e.g. for a smaller disk:
#   CCACHE_MAXSIZE=10G tools/ci/local/devshell.sh
CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-40G}"

# qtpy pin (M2.P3.T1d). This script never runs under CI (CI's container
# never goes through devshell.sh -- see ci.yml/README.md), so this literal
# default is what an ordinary local invocation actually uses; it matches
# ci.yml's own QTPY_PIN default by convention, same as PYTHON_VERSION/
# OCIO_CONFIG_VERSION above. The value is threaded into the container below
# (`-e QTPY_PIN=...`) so fetch-assets.sh, running inside that container,
# installs/verifies the identical pin rather than an independently
# maintained copy -- see fetch-assets.sh's own fallback default for the one
# path (running outside both scripts) that still needs a hardcoded copy of
# this string.
QTPY_PIN="${QTPY_PIN:-qtpy>=2.0,<3}"

# Resolve repo root from this script's own location, not $PWD, so this
# works the same from any worktree.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." >/dev/null 2>&1 && pwd)"

UID_GID="$(id -u):$(id -g)"

# --- optional forced recreate -----------------------------------------------
if [[ "${1:-}" == "--recreate" ]]; then
    shift
    echo "devshell.sh: recreating container '${CONTAINER_NAME}' (caches are kept)..." >&2
    docker rm -f "${CONTAINER_NAME}" >/dev/null 2>&1 || true
fi

container_state() {
    # Echoes "running", "stopped", or "missing".
    local running
    if ! running="$(docker container inspect -f '{{.State.Running}}' "${CONTAINER_NAME}" 2>/dev/null)"; then
        echo "missing"
    elif [[ "${running}" == "true" ]]; then
        echo "running"
    else
        echo "stopped"
    fi
}

STATE="$(container_state)"

if [[ "${STATE}" == "missing" ]]; then
    if ! docker image inspect "${IMAGE}" >/dev/null 2>&1; then
        echo "devshell.sh: image '${IMAGE}' not found. Build it first:" >&2
        echo "    docker build -t ${IMAGE} tools/ci/local" >&2
        exit 1
    fi

    echo "devshell.sh: creating container '${CONTAINER_NAME}' from ${IMAGE}..." >&2

    # Named volumes for persistent caches. Created fresh they are
    # root-owned, so a one-time chown (as root, against the same volumes)
    # is needed before the unprivileged container below can write to them.
    # This is idempotent: docker volume create is a no-op if the volume
    # already exists, and re-chowning an already-chowned volume is harmless.
    docker volume create "${CCACHE_VOLUME}" >/dev/null
    docker volume create "${HOME_VOLUME}" >/dev/null

    docker run --rm --user 0:0 \
        -v "${CCACHE_VOLUME}:${CCACHE_MOUNT}" \
        -v "${HOME_VOLUME}:${HOME_MOUNT}" \
        "${IMAGE}" \
        chown -R "${UID_GID}" "${CCACHE_MOUNT}" "${HOME_MOUNT}" >/dev/null

    # The repo is bind-mounted at the SAME absolute path it has outside the
    # container, and that path is also the container's working directory,
    # so paths printed by tools inside the container match paths outside it.
    #
    # --cap-add=SYS_PTRACE and --security-opt seccomp=unconfined are
    # required (not optional) for gdb to work inside this container.
    docker run -d \
        --name "${CONTAINER_NAME}" \
        --user "${UID_GID}" \
        --cap-add=SYS_PTRACE \
        --security-opt seccomp=unconfined \
        -v "${REPO_ROOT}:${REPO_ROOT}" \
        -v "${CCACHE_VOLUME}:${CCACHE_MOUNT}" \
        -v "${HOME_VOLUME}:${HOME_MOUNT}" \
        -w "${REPO_ROOT}" \
        -e CI=True \
        -e NATRON_IN_CONTAINER=1 \
        -e PYTHON_VERSION=3.13 \
        -e OCIO_CONFIG_VERSION=2.5 \
        -e QTPY_PIN="${QTPY_PIN}" \
        -e HOME="${HOME_MOUNT}" \
        -e CCACHE_DIR="${CCACHE_MOUNT}" \
        -e CCACHE_MAXSIZE="${CCACHE_MAXSIZE}" \
        "${IMAGE}" \
        sleep infinity >/dev/null

    # ccache persists settings it's told about (via `ccache -M`/env at time
    # of use) into ${CCACHE_MOUNT}/ccache.conf inside the volume, and that
    # file takes precedence over CCACHE_MAXSIZE on later runs -- so if an
    # older ccache.conf with the old 5 GiB default is already sitting in
    # this volume, just setting the env var above would be silently
    # overridden by it. Clear any max_size line so our env var wins; this
    # is idempotent and harmless if the file doesn't exist yet.
    docker run --rm --user "${UID_GID}" \
        -v "${CCACHE_VOLUME}:${CCACHE_MOUNT}" \
        "${IMAGE}" \
        sh -c "sed -i '/^max_size/d' ${CCACHE_MOUNT}/ccache.conf 2>/dev/null || true"

    # One-time (per-container), root-level install of `qtpy` into system
    # site-packages (see M2.P3.T1d). This container runs as uid 1000 (see
    # `--user "${UID_GID}"` above), which cannot write to system
    # site-packages (root-owned 755, verified) -- and `pip install --user`
    # is not a usable workaround here: NatronRenderer's embedded
    # interpreter explicitly disables user-site (see
    # AppManager::initPython()'s PYTHONNOUSERSITE=1, there so Natron's own
    # bundled packages can't be shadowed), so anything installed with
    # --user would be invisible to `import qtpy` inside Natron even though
    # it resolves fine from a bare `python3 -c ...`. `docker exec -u 0:0`
    # overrides the container's default user for just this one exec, which
    # is enough to install into system site-packages without changing how
    # the container runs otherwise. This sits in the STATE=="missing"
    # branch specifically -- not gated behind its own "is qtpy already
    # there" check -- because that branch itself only ever runs once per
    # container lifetime, which already gives idempotency for free: a
    # second `devshell.sh` invocation against a still-running container
    # skips this whole block, doing no network work.
    #
    # Pin comes from QTPY_PIN (set above), the single source shared with
    # fetch-assets.sh's own qtpy install (which handles CI, where the job's
    # container already runs as root and so never needs this escalation) --
    # both this exec and the container's own QTPY_PIN env var (set on the
    # `docker run` above) use the same value, so fetch-assets.sh running
    # inside this container never has to guess it independently.
    echo "devshell.sh: installing qtpy (root, one-time) into '${CONTAINER_NAME}'..." >&2
    docker exec -u 0:0 "${CONTAINER_NAME}" \
        python3 -m pip install --root-user-action=ignore "${QTPY_PIN}" >&2

elif [[ "${STATE}" == "stopped" ]]; then
    echo "devshell.sh: starting existing (stopped) container '${CONTAINER_NAME}'..." >&2
    docker start "${CONTAINER_NAME}" >/dev/null
fi
# else: already running -- nothing to do, just exec below.

# --- run the requested command ----------------------------------------------
# Note: the base image's entrypoint prints a large NVIDIA/CUDA banner, but
# that only runs once, at container creation/start above (it lands in
# `docker logs <container>`, not here). `docker exec` never re-invokes the
# entrypoint, so it never pollutes the output below -- this is also why we
# don't need a TTY for the non-interactive path.
if [[ "$#" -eq 0 ]]; then
    exec docker exec -it "${CONTAINER_NAME}" bash -l
else
    exec docker exec -i "${CONTAINER_NAME}" "$@"
fi
