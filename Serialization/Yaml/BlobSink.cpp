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

#include "Serialization/Yaml/BlobSink.h"

#include <utility>

#include "Serialization/Yaml/YamlScalars.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

BlobSink::~BlobSink() = default;

BlobSource::~BlobSource() = default;

bool
InlineBlobSink::addMember(const std::string& /*path*/,
                          std::vector<std::byte> /*bytes*/)
{
    return false;
}

std::optional<std::span<const std::byte>>
NoBlobSource::findMember(std::string_view /*path*/) const
{
    return std::nullopt;
}

bool
MemoryBlobStore::addMember(const std::string& path,
                           std::vector<std::byte> bytes)
{
    if (!_members.emplace(path, std::move(bytes)).second) {
        throw YamlError("two blobs were given the path " + path);
    }

    return true;
}

std::optional<std::span<const std::byte>>
MemoryBlobStore::findMember(std::string_view path) const
{
    auto it = _members.find(path);
    if (it == _members.end()) {
        return std::nullopt;
    }

    return std::span<const std::byte>(it->second);
}

void
MemoryBlobStore::removeMember(std::string_view path)
{
    auto it = _members.find(path);
    if (it != _members.end()) {
        _members.erase(it);
    }
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
