/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>

#include <OpenImageIO/imageio.h>
#include <OpenImageIO/plugin.h>

#include "LiveTextureFile.h"

OIIO_PLUGIN_NAMESPACE_BEGIN

namespace {
const int kTileSize = 64;

class ScopedFile {
public:
    explicit ScopedFile(const std::string& path)
        : _fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC))
    {
    }

    ~ScopedFile()
    {
        if (_fd >= 0) {
            ::close(_fd);
        }
    }

    ScopedFile(const ScopedFile&) = delete;
    ScopedFile& operator=(const ScopedFile&) = delete;

    int get() const
    {
        return _fd;
    }

private:
    int _fd;
};

bool
readFully(int fd,
          void* data,
          std::size_t size,
          off_t offset)
{
    char* out = static_cast<char*>(data);

    while (size > 0) {
        const ssize_t got = ::pread(fd, out, size, offset);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }

            return false;
        }
        if (got == 0) {
            return false;
        }
        out += got;
        offset += got;
        size -= std::size_t(got);
    }

    return true;
}

bool
readDimensions(const std::string& path,
               int* width,
               int* height)
{
    const ScopedFile file(path);
    struct stat info;
    char header[NatronLiveTexture::kHeaderSize];

    if ((file.get() < 0) || (::fstat(file.get(), &info) != 0) || !readFully(file.get(), header, sizeof(header), 0)
        || !NatronLiveTexture::readHeader(header, sizeof(header), width, height)) {
        return false;
    }

    return std::size_t(info.st_size) == NatronLiveTexture::fileSize(*width, *height);
}

/*
 * Holds no descriptor or mapping between reads. OIIO keeps a closed-over handle open until it passes
 * its open-files limit, which is 128 in Cycles, and either would pin the pages of a file Natron has
 * since removed: 128 frames of a plate sequence, in RAM, past the store's budget.
 */
class NatronInput final : public ImageInput {
public:
    const char* format_name() const override
    {
        return NatronLiveTexture::kFormatName;
    }

    bool valid_file(const std::string& name) const override
    {
        int width = 0;
        int height = 0;

        return readDimensions(name, &width, &height);
    }

    bool open(const std::string& name,
              ImageSpec& spec) override
    {
        if (!readDimensions(name, &_width, &_height)) {
            errorfmt("{} is not a live texture Natron is serving", name);

            return false;
        }
        _path = name;
        setLevel(0);
        spec = m_spec;

        return true;
    }

    bool close() override
    {
        _path.clear();

        return true;
    }

    int current_subimage() const override
    {
        return 0;
    }

    int current_miplevel() const override
    {
        return _level;
    }

    /*
     * OIIO seeks from every thread that reads this input, under its lock or not, and rebuilding the
     * spec at the level it is already on frees strings another thread is reading, which corrupts the
     * heap in a multi-frame Cycles render.
     */
    bool seek_subimage(int subimage,
                       int miplevel) override
    {
        if ((subimage != 0) || (miplevel < 0) || (miplevel >= NatronLiveTexture::levelCount(_width, _height))) {
            return false;
        }
        if (miplevel != _level) {
            setLevel(miplevel);
        }

        return true;
    }

    bool read_native_scanline(int /*subimage*/,
                              int /*miplevel*/,
                              int /*y*/,
                              int /*z*/,
                              void* /*data*/) override
    {
        errorfmt("a live texture is read by tiles");

        return false;
    }

    bool read_native_tile(int subimage,
                          int miplevel,
                          int x,
                          int y,
                          int /*z*/,
                          void* data) override
    {
        // OIIO calls this from many threads on one input, with no lock of its own.
        lock_guard guard(*this);

        if (!seek_subimage(subimage, miplevel)) {
            return false;
        }
        const NatronLiveTexture::Level level = NatronLiveTexture::levelAt(_width, _height, miplevel);
        if ((x < 0) || (y < 0) || (x >= level.width) || (y >= level.height)) {
            errorfmt("tile {},{} is outside level {} of {}", x, y, miplevel, _path);

            return false;
        }
        const ScopedFile file(_path);
        if (file.get() < 0) {
            errorfmt("cannot open {}: {}", _path, std::strerror(errno));

            return false;
        }
        const std::size_t tileRowBytes = std::size_t(kTileSize) * NatronLiveTexture::kTexelBytes;
        char* out = static_cast<char*>(data);
        std::memset(out, 0, tileRowBytes * std::size_t(kTileSize));
        const int columns = std::min(kTileSize, level.width - x);
        const int rows = std::min(kTileSize, level.height - y);
        const std::size_t rowBytes = std::size_t(level.width) * NatronLiveTexture::kTexelBytes;
        for (int j = 0; j < rows; ++j) {
            // OIIO's row 0 is the top of the image, the file's the bottom.
            const std::size_t fileRow = std::size_t(level.height - 1 - (y + j));
            const off_t offset = off_t(level.offset + fileRow * rowBytes + std::size_t(x) * NatronLiveTexture::kTexelBytes);
            if (!readFully(file.get(), out + std::size_t(j) * tileRowBytes, std::size_t(columns) * NatronLiveTexture::kTexelBytes, offset)) {
                errorfmt("cannot read {}: {}", _path, std::strerror(errno));

                return false;
            }
        }

        return true;
    }

private:
    void setLevel(int index)
    {
        const NatronLiveTexture::Level level = NatronLiveTexture::levelAt(_width, _height, index);

        _level = index;
        m_spec = ImageSpec(level.width, level.height, 4, TypeDesc::FLOAT);
        m_spec.tile_width = kTileSize;
        m_spec.tile_height = kTileSize;
        m_spec.tile_depth = 1;
        m_spec.channelnames = { "R", "G", "B", "A" };
        m_spec.alpha_channel = 3;
        // No oiio:ColorSpace: the pixels are in the project's working space, which the file does not
        // name, and neither delegate converts a live texture from the space a file claims.
        m_spec.attribute("oiio:UnassociatedAlpha", 1);
        m_spec.attribute("textureformat", "Plain Texture");
        m_spec.attribute("wrapmodes", "black,black");
    }

    std::string _path;
    int _width = 0;
    int _height = 0;
    int _level = 0;
};
} // namespace

OIIO_PLUGIN_EXPORTS_BEGIN
OIIO_EXPORT int natronlive_imageio_version = OIIO_PLUGIN_VERSION;
OIIO_EXPORT const char*
natronlive_imageio_library_version()
{
    return nullptr;
}

OIIO_EXPORT ImageInput*
natronlive_input_imageio_create()
{
    return new NatronInput;
}

OIIO_EXPORT const char* natronlive_input_extensions[] = { NatronLiveTexture::kFileExtension, nullptr };
OIIO_PLUGIN_EXPORTS_END

OIIO_PLUGIN_NAMESPACE_END
