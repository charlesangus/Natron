# 2026-08-30 — Sealed package network: use the aswf image as-is

This development environment reaches `github.com` and the container registries
(`registry-1.docker.io`, `auth.docker.io`, `mirror.gcr.io`, `ghcr.io`,
`quay.io`) but no distro package repository: `mirrors.rockylinux.org`,
`mirrors.fedoraproject.org`, `dl.fedoraproject.org`,
`developer.download.nvidia.com`, Docker Hub's blob CDN
(`production.cloudfront.docker.com`), and every EPEL mirror probed are all
unreachable. Egress is an allowlist rather than a blocklist.

Two consequences hold for any local tooling in this project, not just M7:

1. **Pull Docker images through `mirror.gcr.io` and retag.** `docker pull
   aswf/ci-baseqt:2027.0` authenticates and then dies on the first blob.
   `docker pull mirror.gcr.io/aswf/ci-baseqt:2027.0` followed by `docker tag
   mirror.gcr.io/aswf/ci-baseqt:2027.0 aswf/ci-baseqt:2027.0` gives the same
   image — verified digest
   `sha256:9df58e9cc6831773bad261596a317ea6019006423ad73ed91652e1762a5a68f7` —
   and leaves `FROM` lines pinned to the tag CI uses.
2. **`dnf install` cannot run in an image build.** The chosen response is to use
   `aswf/ci-baseqt:2027.0` as the build engine as-is rather than to reconstruct
   CI's package step by other means. This is nearly free: the image already
   ships every package `.github/workflows/ci.yml` installs, with `clang` and
   `ccache` under `/usr/local/bin` from Conan. `extra-cmake-modules` is the sole
   exception, and installing it from source was implemented, verified, and then
   deliberately cut — see M7's `## Decisions` for the resulting
   Wayland-support divergence and how to close it.

Widening the egress allowlist would remove both workarounds and is worth
revisiting, but neither blocks the work, so it is not tracked as an open
question.
