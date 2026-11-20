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

#ifndef Serialization_Sha256_h
#define Serialization_Sha256_h

#include "Global/Macros.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief The SHA-256 digest of bytes, as 64 lowercase hex characters.
 **/
std::string sha256Hex(std::span<const std::byte> bytes);
std::string sha256Hex(std::string_view text);

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Sha256_h
