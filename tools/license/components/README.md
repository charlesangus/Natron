Natron binaries include components from the following libraries:

# Main components

* Python (Modified BSD) https://www.python.org/download/releases/2.7/license/
* PySide (LGPL 2.1) https://wiki.qt.io/About_PySide
* libshiboken (LGPL 2.1) https://github.com/qtproject/pyside-shiboken
* Qt (LGPL 2.1 with Qt LGPL Exception) http://doc.qt.io/qt-4.8/lgpl.html
* Boost (Boost Software License) http://www.boost.org/users/license.html
* Cairo (LGPL 2.1) http://cairographics.org/
* GLAD (MIT) http://glad.dav1d.de/
* GLFW (libpng/zlib license) http://www.glfw.org/
* Lucide icons (ISC; Feather-derived glyphs MIT) https://github.com/lucide-icons/lucide
* openfx (Modified BSD) https://github.com/ofxa/openfx
* openfx-io (GPLv2) https://github.com/NatronGitHub/openfx-io
* openfx-misc (GPLv2) https://github.com/NatronGitHub/openfx-misc
* openfx-arena (GPLv2) https://github.com/NatronGitHub/openfx-arena
* openfx-gmic (CeCILL-C, LGPL-like) https://github.com/NatronGitHub/openfx-gmic
* Eigen 3 (MPL 2.0) http://eigen.tuxfamily.org/
* Ceres Solver (New BSD) http://ceres-solver.org/
* gflags (New BSD) http://gflags.github.io/gflags/
* glog (New BSD) https://github.com/google/glog
* libmv (BSD) https://developer.blender.org/project/profile/59/
* openMVG (MPL 2.0) http://imagine.enpc.fr/~moulonp/openMVG/
* qhttpserver (BSD) https://github.com/nikhilm/qhttpserver
* hoedown (BSD) https://github.com/hoedown/hoedown
* libtess (SGI Free Software License B, X11-like) ftp://ftp.freedesktop.org/pub/mesa/glu/
* rawspeed (LGPL 2.1) https://github.com/darktable-org/rawspeed -- the camera raw decoder, linked statically into Natron at commit c835b05aecfacb7343f7c424abd620aa12116c3f (the revision darktable release-5.6.2 pins); its camera database, cameras.xml, ships under Resources/rawspeed
* pugixml (MIT) https://pugixml.org (used by Natron via rawspeed, to read cameras.xml)
* darktable (GPL 3.0 or later) https://www.darktable.org -- the raw processing nodes are ported from darktable release-5.6.2 (commit 20891f6c6fa7be995e5bf9dff9d1ee5062051773) and keep darktable's copyright headers
* darktable white balance presets (GPL 3.0 or later; values originally from UFRaw, Copyright 2004-2013 Udi Fuchs, GPL 2.0 or later) https://github.com/darktable-org/darktable/blob/release-5.6.2/data/wb_presets.json -- data/wb_presets.json of darktable release-5.6.2, copied unchanged to Engine/Nodes/Raw/data/wb_presets.json and shipped under Resources/darktable; RawTemperature's camera presets
* yaml-cpp (MIT) https://github.com/jbeder/yaml-cpp
* FastNoise2 (MIT) https://github.com/Auburn/FastNoise2 -- the noise generators in libs/noise (CPU) and Engine/Gpu/Kernels/noisecore.slang (GPU) are ported from FastNoise2 at commit 721400f9560831be2fc9790be9b72c44bee7d33f
* ONNX Runtime (MIT) https://github.com/microsoft/onnxruntime -- the official Linux x64 CPU release 1.31.1, shipped unmodified under lib/onnxruntime and loaded at run time by the AI nodes; its ThirdPartyNotices.txt, listing the components ONNX Runtime itself includes and their licences, ships beside it

Natron itself is GPL 2.0 or later. Because the raw nodes ported from darktable are GPL 3.0 or
later, the Natron binaries as a whole are distributed under the terms of the GPL 3.0
(LICENSE-GPL-3.0.txt); rawspeed's LGPL 2.1 and pugixml's MIT licence are both compatible with that.

# Additional dependencies

## openfx-misc

* Mesa (MIT) http://www.mesa3d.org
* TinyThread++ (libpng/zlib License) http://tinythreadpp.bitsnbites.eu/
* CImg (CeCILL-C, LGPL-like) http://cimg.eu/

## openfx-io

* OpenImageIO (Modified BSD) https://openimageio.org
* OpenColorIO (Modified BSD) http://opencolorio.org/License.html
* FFmpeg (LGPL 2.1) https://www.ffmpeg.org/legal.html
* SeExpr (modified Apache 2.0) http://www.disneyanimation.com/technology/seexpr.html
* pstream (Boost Software License) http://pstreams.sourceforge.net

### Video codecs and formats

* x264 (GPLv2) http://www.videolan.org/developers/x264.html (used by openfx-io via FFmpeg) 
* x265 (GPLv2) http://x265.org/ (used by openfx-io via FFmpeg) 
* libogg (3-clause BSD) https://xiph.org/ogg/ (used by openfx-io via FFmpeg)
* libvorbis (3-clause BSD) https://xiph.org/vorbis/ (used by openfx-io via FFmpeg)
* libtheora (3-clause BSD) http://www.theora.org/ (used by openfx-io via FFmpeg)
* libspeex (3-clause BSD) http://www.speex.org/ (used by openfx-io via FFmpeg)
* libopus (3-clause BSD) http://opus-codec.org/ (used by openfx-io via FFmpeg)
* libmodplug (Public Domain) http://modplug-xmms.sourceforge.net/ (used by openfx-io via FFmpeg) 
* libmp3lame (LGPL 2.1) http://lame.sourceforge.net/ (used by openfx-io via FFmpeg) 
* openh264 (BSD) http://www.openh264.org/ (used by openfx-io via FFmpeg)
* orc (BSD) http://code.entropywave.com/orc/ (used by openfx-io via FFmpeg and schroedinger) 
* Snappy (3-clause BSD) http://diracvideo.org/ (used by openfx-io via FFmpeg) 

### Image formats

* OpenEXR & IlmBase (Modified BSD)  http://www.openexr.com/license.html (used by openfx-io via OpenImageIO) 
* libtiff (Modified BSD) http://www.remotesensing.org/libtiff (used by openfx-io via OpenImageIO) 
* libpng (libpng/zlib License) http://www.libpng.org (used by openfx-io via OpenImageIO and openfx-arena via ImageMagick)
* libjpeg (Modified BSD) http://ijg.org (used by openfx-io via OpenImageIO, and by Natron via rawspeed for lossy DNG) 
* openjpeg (2-clauses BSD) http://www.openjpeg.org (used by openfx-io via OpenImageIO) 
* webp (Modified BSD) https://developers.google.com/speed/webp (used by openfx-io via OpenImageIO) 
* HDF5 (Modified BSD) https://www.hdfgroup.org/HDF5 (used by openfx-io via OpenImageIO) 
* Field3D (Modified BSD) https://github.com/imageworks/Field3D (used by openfx-io via OpenImageIO) 
* GIFLIB (X Consortium-like licence) http://giflib.sourceforge.net (used by openfx-io via OpenImageIO) 
* LibRaw (LGPL 2.1 or CDDL 1.0 license) http://www.libraw.org (used by openfx-io via OpenImageIO) 
* JasPer (MIT) http://www.ece.uvic.ca/~frodo/jasper/ (used by openfx-io via LibRaw) 

## openfx-arena

* ImageMagick (Apache 2.0 with attribution clause) http://www.imagemagick.org/script/license.php
* fontconfig (Modified BSD) http://www.freedesktop.org/wiki/Software/fontconfig/
* Pango (LGPL 2.1) http://www.pango.org/
* Little CMS (MIT) http://www.littlecms.com/
* libzip (BSD) http://www.nih.at/libzip/
* lodepng (zlib) http://lodev.org/lodepng/
* Cairo (LGPL 2.1) http://cairographics.org/
* libxml (MIT) http://www.xmlsoft.org/

The SVG, PDF and CorelDRAW readers are built off, so the bundle links none
of librsvg (LGPL 2.1), poppler (GPL 2.0), librevenge (MPL 2.0) or libcdr
(MPL 2.0). Poppler in particular is excluded deliberately, not incidentally:
linking it would make Arena.ofx GPL-2.0 outright. Re-enabling any of those
readers means restoring its entry here.

## openfx-gmic

* G'MIC (CeCILL-C, LGPL-like) http://gmic.eu/
* CImg (CeCILL-C, LGPL-like) http://cimg.eu/
* FFTW (GPLv2) http://www.fftw.org/

## Indirect dependencies

* TinyXML (libpng/zlib license) http://www.sourceforge.net/projects/tinyxml (used by openfx-io via OpenColorIO and openfx-arena via OpenColorIO)
* FreeType (Modified BSD) http://git.savannah.gnu.org/cgit/freetype/freetype2.git/plain/docs/FTL.TXT (used by Natron, openfx-io and openfx-arena via Cairo, ImageMagick and OpenImageIO) 
* Expat (MITX) http://expat.sourceforge.net/ (used by openfx) 
* Pixman (MIT) http://www.pixman.org/ (used by Natron and openfx-arena via Cairo)
* libmng (libpng/zlib License) http://www.libpng.org/pub/mng/ (used by Qt)
* OpenSSL (Apache 1.0 or 4-clause BSD) http://www.openssl.org (used by Qt and Python)
* gdk_pixbuf (LGPL 2.1) https://developer.gnome.org/gdk-pixbuf/stable/ (used by openfx-arena via librsvg)
* Little CMS (MIT) http://www.littlecms.com/ (used by Qt and openfx-io via libmng and LibRaw)
* libcroco (LGPL 2.1) https://github.com/GNOME/libcroco (used by openfx-arena via librsvg)
* GLib (LGPL 2.1) http://www.gtk.org (used by Qt and openfx-arena via libcroco, harfbuzz, Pango, librsvg)
* HarfBuzz (MIT) http://www.freedesktop.org/wiki/Software/HarfBuzz/ (used by Qt and openfx-arena via Pango)
* libffi (MIT) https://sourceware.org/libffi/ (used Python and openfx-arena via GLib)
* ICU (MIT) http://www.icu-project.org/ (used by Boost and openfx-arena via libcdr)
* Graphite (LGPL 2.1) http://graphite.sil.org (used by openfx-arena via HarfBuzz)
* libedit (BSD) http://thrysoee.dk/editline/ (used by Python) 
* Berkeley DB (AGPLv3) http://www.oracle.com/us/products/database/berkeley-db (used by Python) 
* SQLite (Public Domain) https://www.sqlite.org/ (used by Python)
* bzip2 (Modified BSD) http://www.bzip.org (used by Natron, openfx-io and openfx-arena via FreeType and FFmpeg) 
* liblzma (Public Domain) http://tukaani.org/xz/ (used by openfx-io and openfx-arena via FFmpeg and libxml) 
* zlib (libpng/zlib License) http://www.zlib.net/ (used by Natron via rawspeed for deflate DNG, among others)
* libiconv (LGPL 2.1) https://www.gnu.org/software/libiconv/
* libintl (LGPL 2.1) https://www.gnu.org/software/gettext/
* ncurses (LGPL 2.1) https://www.gnu.org/software/ncurses/

