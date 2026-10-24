#!/usr/bin/env bash
# Installs a pinned git-lfs release into /usr/local/bin. aswf/ci-vfxall ships
# git without git-lfs and has no reachable distro package repository, so the
# dev image bakes it in with this script, and the jobs that run on the bare
# ASWF image call it before pulling the LFS-tracked test fixtures.
#
# Usage: install-git-lfs.sh

set -euo pipefail

version=3.8.1
sha256=e72cf723a4a660438862282e8aecf3885fc8123e88bfcad534cf6b941a354b75
url="https://github.com/git-lfs/git-lfs/releases/download/v${version}/git-lfs-linux-amd64-v${version}.tar.gz"

work="$(mktemp -d)"
trap 'rm -rf "${work:?}"' EXIT

wget -q -O "${work}/git-lfs.tar.gz" "$url"
echo "${sha256}  ${work}/git-lfs.tar.gz" | sha256sum -c -
tar -xzf "${work}/git-lfs.tar.gz" -C "$work"
install -m 0755 "${work}/git-lfs-${version}/git-lfs" /usr/local/bin/git-lfs
git lfs version
