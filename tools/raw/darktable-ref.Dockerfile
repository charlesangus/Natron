# darktable-cli at one pinned revision (DARKTABLE_REF or DARKTABLE_MASTER_REF), for
# tools/raw/make-reference.sh; options a revision lacks are ignored. Only what the
# command-line export of a raw file to TIFF needs is built; every optional feature is off.
FROM debian:trixie

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ca-certificates cmake git ninja-build pkg-config \
        gettext intltool xsltproc libxml2-utils python3-jsonschema perl \
        libglib2.0-dev libgtk-3-dev libcairo2-dev libpango1.0-dev libgdk-pixbuf-2.0-dev \
        libexiv2-dev liblcms2-dev libpugixml-dev libjson-glib-dev libsqlite3-dev \
        libcurl4-gnutls-dev librsvg2-dev libtiff-dev libpng-dev libjpeg-dev zlib1g-dev \
        libxml2-dev libarchive-dev libicu-dev liblensfun-dev libpotrace-dev libomp-dev \
        python3-numpy python3-tifffile \
    && rm -rf /var/lib/apt/lists/*

ARG DARKTABLE_REPO
ARG DARKTABLE_REF
ARG JOBS=4
RUN git init /src/darktable \
    && cd /src/darktable \
    && git remote add origin "${DARKTABLE_REPO}" \
    && git fetch --depth 1 origin "${DARKTABLE_REF}" \
    && git checkout --quiet FETCH_HEAD \
    && git -c submodule.src/tests/integration.update=none -c submodule.src/external/lua-scripts.update=none \
           submodule update --init --depth 1 --jobs 4

# BINARY_PACKAGE_BUILD keeps the code generic rather than tuned to the build host's CPU, so the
# references do not depend on the machine they were made on.
RUN cmake -S /src/darktable -B /src/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/darktable \
        -DBINARY_PACKAGE_BUILD=ON -DUSE_OPENCL=OFF -DUSE_LUA=OFF -DUSE_MAP=OFF \
        -DUSE_CAMERA_SUPPORT=OFF -DUSE_COLORD=OFF -DUSE_KWALLET=OFF -DUSE_LIBSECRET=OFF \
        -DUSE_GRAPHICSMAGICK=OFF -DUSE_IMAGEMAGICK=OFF -DUSE_PORTMIDI=OFF -DUSE_OPENJPEG=OFF \
        -DUSE_JXL=OFF -DUSE_WEBP=OFF -DUSE_AVIF=OFF -DUSE_HEIF=OFF -DUSE_XCF=OFF \
        -DUSE_OPENEXR=OFF -DUSE_GMIC=OFF -DUSE_SDL2=OFF -DUSE_AI=OFF -DUSE_MCP=OFF -DUSE_OPENCV=OFF \
        -DBUILD_PRINT=OFF -DBUILD_CMSTEST=OFF -DBUILD_RS_IDENTIFY=OFF -DUSE_XMLLINT=OFF \
        -DBUILD_TESTING=OFF \
    && cmake --build /src/build -j "${JOBS}" \
    && cmake --install /src/build \
    && rm -rf /src/build

ENV PATH="/opt/darktable/bin:${PATH}"
