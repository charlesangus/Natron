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

set -euo pipefail

IMAGE="natron-dev:2027.0"
CONTAINER_NAME="${NATRON_DEV_CONTAINER:-natron-dev}"
CCACHE_VOLUME="${CONTAINER_NAME}-ccache"
HOME_VOLUME="${CONTAINER_NAME}-home"
CCACHE_MOUNT="/ccache"
HOME_MOUNT="/home/devshell"

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
        -e PYTHON_VERSION=3.10 \
        -e OCIO_CONFIG_VERSION=2.5 \
        -e HOME="${HOME_MOUNT}" \
        -e CCACHE_DIR="${CCACHE_MOUNT}" \
        "${IMAGE}" \
        sleep infinity >/dev/null

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
