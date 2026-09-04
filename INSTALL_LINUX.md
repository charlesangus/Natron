# Installing Natron on Linux

This covers the pre-built Linux release artifacts published on the [Releases page](https://github.com/charlesangus/Natron/releases). If you'd rather build from source, see [tools/ci/local/README.md](tools/ci/local/README.md).

## Which artifact to take

Two artifacts are published per release:

- **AppImage** (`Natron-<version>-x86_64.AppImage`) — for workstations. A single self-contained file with desktop integration (menu entry, icon, MIME types) via its bundled AppStream metadata. Make it executable and run it directly:

  ```
  chmod +x Natron-<version>-x86_64.AppImage
  ./Natron-<version>-x86_64.AppImage
  ```

  The AppImage runtime needs FUSE2 to mount itself. If FUSE2 isn't available on your system, run it with `--appimage-extract-and-run` instead:

  ```
  ./Natron-<version>-x86_64.AppImage --appimage-extract-and-run
  ```

- **Tarball** (`Natron-<version>-linux-x86_64.tar.xz`) — for render farms or shared network storage. Extract it and run the binaries directly; there is no installer:

  ```
  tar -xJf Natron-<version>-linux-x86_64.tar.xz
  ```

  For farm rendering, invoke `bin/NatronRenderer`, not `bin/Natron -b`. `NatronRenderer` exits with a non-zero status on render failure, which farm schedulers rely on to detect failed jobs; `Natron -b` does not reliably do this.

## System requirements

- x86_64 Linux
- glibc 2.34 or later. The artifacts are built on Rocky Linux 9 (EL9), which sets this floor. In practice this means Ubuntu 22.04 or later, Debian 12 or later, Fedora 36 or later, and RHEL/Rocky/Alma 9 or later. Ubuntu 20.04 and other older distributions are not supported.
- OpenGL 2.0 or later for GPU-accelerated rendering

## Headless / render farm caveat

On a machine with no X server or display available, `NatronRenderer` will still start, but GPU-accelerated rendering is not available — it falls back to CPU-only rendering. This is a limitation in how the renderer initializes its OpenGL context (it requires `XOpenDisplay` to succeed), not a packaging issue, and it is not fixed by using a different artifact.

As a workaround, you can force Mesa's software rasterizer:

```
LIBGL_ALWAYS_SOFTWARE=1
```

This lets rendering proceed without a display, but it is a software fallback and will be slower than GPU-accelerated rendering.

## Verifying the download

Each artifact ships with a `.sha256` sidecar file. Verify the download before running it:

```
sha256sum -c Natron-<version>-x86_64.AppImage.sha256
sha256sum -c Natron-<version>-linux-x86_64.tar.xz.sha256
```

## Environment notes

Both artifacts carry their own OCIO config, OFX plugins, fontconfig configuration, and Python runtime. Do not set `OCIO`, `OFX_PLUGIN_PATH`, `PYTHONHOME`, or `PYTHONPATH` in your environment before running Natron — the bundled defaults are correct for the bundled version, and external overrides can point Natron at incompatible configs or plugins and break it.
