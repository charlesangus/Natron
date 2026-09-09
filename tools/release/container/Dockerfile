# Packages here correspond to tools/release/excludelist.txt entries the
# bundle expects the host to provide (GL/X11, fontconfig, glib, dbus) --
# nothing from the VFX stack is installed. A new excludelist entry with no
# matching package here will surface as a missing-soname failure, not
# silent staleness.
FROM debian:12

RUN apt-get update && apt-get install -y --no-install-recommends \
    xvfb \
    libgl1 \
    libgl1-mesa-dri \
    libglx-mesa0 \
    libopengl0 \
    libxi6 \
    libfontconfig1 \
    libglib2.0-0 \
    libxtst6 \
    libxrender1 \
    libxrandr2 \
    libxinerama1 \
    libxdamage1 \
    libxcursor1 \
    libxcomposite1 \
    libdbus-1-3 \
    && rm -rf /var/lib/apt/lists/*
