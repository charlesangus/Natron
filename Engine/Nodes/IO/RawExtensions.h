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

#ifndef Engine_Nodes_IO_RawExtensions_h
#define Engine_Nodes_IO_RawExtensions_h

#include "Global/Macros.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>

NATRON_NAMESPACE_ENTER

namespace RawExtensions {
/**
 * @brief The file extensions of the camera raw formats rawspeed decodes, lower case, sorted.
 * Canon CR3, Sigma X3F and ARRI ARI are absent because rawspeed has no decoder for them.
 **/
inline constexpr std::array<std::string_view, 25> kExtensions = {
    "3fr", // Hasselblad
    "arw", // Sony
    "cr2", // Canon
    "crw", // Canon
    "dcr", // Kodak
    "dcs", // Kodak
    "dng", // Adobe Digital Negative
    "erf", // Epson
    "iiq", // Phase One
    "kdc", // Kodak
    "mef", // Mamiya
    "mos", // Leaf
    "mrw", // Minolta
    "nef", // Nikon
    "nrw", // Nikon
    "orf", // Olympus
    "pef", // Pentax
    "raf", // Fujifilm
    "raw", // Panasonic, Leica
    "rw2", // Panasonic
    "rwl", // Leica
    "sr2", // Sony
    "srf", // Sony
    "srw", // Samsung
    "sti", // Sinar
};

static_assert(std::is_sorted(kExtensions.begin(), kExtensions.end()), "isRawExtension() binary-searches kExtensions");

/**
 * @brief Whether `extension` (case-insensitive, with or without a leading dot) is in kExtensions.
 **/
inline bool
isRawExtension(std::string_view extension)
{
    if (!extension.empty() && (extension.front() == '.')) {
        extension.remove_prefix(1);
    }
    std::string lower(extension);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : (char)c;
    });

    return std::binary_search(kExtensions.begin(), kExtensions.end(), std::string_view(lower));
}

/**
 * @brief Whether the extension of `path` -- a file name or a sequence pattern, which need not
 * exist -- is a camera raw one. A dot in a directory name is not an extension.
 **/
inline bool
isRawPath(std::string_view path)
{
    const std::size_t dot = path.find_last_of('.');
    const std::size_t slash = path.find_last_of("/\\");

    if ((dot == std::string_view::npos) || ((slash != std::string_view::npos) && (dot < slash))) {
        return false;
    }

    return isRawExtension(path.substr(dot + 1));
}
} // namespace RawExtensions

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_IO_RawExtensions_h
