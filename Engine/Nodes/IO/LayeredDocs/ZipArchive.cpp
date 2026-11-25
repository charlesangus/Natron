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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "ZipArchive.h"

#include <fstream>
#include <span>

#include <OpenImageIO/filesystem.h>

#include "Engine/Nodes/IO/LayeredDocs/LayeredDocument.h"

NATRON_NAMESPACE_ENTER

namespace LayeredDocs {
namespace {
    bool
    readWholeFile(const std::string& path,
                  std::vector<std::byte>* bytes)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);

        if (!file) {
            return false;
        }
        const std::streamoff size = file.tellg();
        if (size < 0) {
            return false;
        }
        bytes->resize((std::size_t)size);
        file.seekg(0);

        return size == 0 || (bool)file.read(reinterpret_cast<char*>(bytes->data()), size);
    }

    // OIIO keeps a PNG's alpha associated, as when it reads a PNG file, unless asked otherwise.
    OIIO::ImageInput::unique_ptr
    openPng(const std::string& member,
            OIIO::Filesystem::IOMemReader* reader,
            std::string* error)
    {
        OIIO::ImageInput::unique_ptr input = OIIO::ImageInput::open(member, nullptr, reader);

        if (!input) {
            const std::string detail = OIIO::geterror();
            *error = "Could not decode " + member + (detail.empty() ? std::string() : ": " + detail);
        }
        return input;
    }
} // anonymous namespace

bool
readArchive(const std::string& path,
            Archive* archive,
            std::string* error,
            std::string* zipError)
{
    if (!readWholeFile(path, &archive->bytes)) {
        *error = "Could not read " + path;

        return false;
    }
    try {
        archive->zip = std::make_unique<Serialization::ZipReader>(std::span<const std::byte>(archive->bytes));
    } catch (const Serialization::ZipError& e) {
        *zipError = e.what();

        return false;
    }

    return true;
}

bool
pngInfoOf(const Archive& archive,
          const std::string& member,
          PngInfo* info,
          std::string* error)
{
    try {
        const std::span<const std::byte> data = archive.zip->data(member);
        OIIO::Filesystem::IOMemReader reader(data.data(), data.size());
        OIIO::ImageInput::unique_ptr input = openPng(member, &reader, error);
        if (!input) {
            return false;
        }
        const OIIO::ImageSpec& spec = input->spec();
        info->width = spec.width;
        info->height = spec.height;
        info->channels = spec.channelnames;
        info->type = spec.format;
        input->close();
    } catch (const Serialization::ZipError& e) {
        *error = e.what();

        return false;
    }

    return true;
}

bool
decodePng(const Archive& archive,
          const std::string& member,
          const PngInfo& info,
          bool rgba,
          std::vector<unsigned char>* pixels,
          std::string* error)
{
    try {
        const std::span<const std::byte> data = archive.zip->data(member);
        OIIO::Filesystem::IOMemReader reader(data.data(), data.size());
        OIIO::ImageInput::unique_ptr input = openPng(member, &reader, error);
        if (!input) {
            return false;
        }
        const OIIO::ImageSpec& spec = input->spec();
        if (spec.width != info.width || spec.height != info.height || spec.channelnames != info.channels) {
            *error = member + " changed while it was read";

            return false;
        }
        const std::size_t count = (std::size_t)spec.width * (std::size_t)spec.height;
        std::vector<float> values(count * (std::size_t)spec.nchannels);
        if (!input->read_image(0, 0, 0, spec.nchannels, OIIO::TypeDesc::FLOAT, values.data())) {
            *error = "Could not decode " + member + ": " + input->geterror();

            return false;
        }
        input->close();

        std::vector<float> out;
        if (rgba) {
            out.resize(count * 4);
            toRgba(values.data(), count, info.channels, out.data());
        } else {
            out.swap(values);
        }
        pixels->resize(out.size() * info.type.size());
        if (!convertValues(OIIO::TypeDesc::FLOAT, out.data(), info.type, pixels->data(), out.size())) {
            *error = "Could not convert the pixels of " + member;

            return false;
        }
    } catch (const Serialization::ZipError& e) {
        *error = e.what();

        return false;
    }

    return true;
}
} // namespace LayeredDocs

NATRON_NAMESPACE_EXIT
