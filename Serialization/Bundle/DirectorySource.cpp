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

#include "Serialization/Bundle/DirectorySource.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <utility>

#include "Serialization/Bundle/ZipStore.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

namespace {

    namespace fs = std::filesystem;

    constexpr const char* kTopLevelMembers[] = { "mimetype", "manifest.yaml", "project.yaml" };
    constexpr std::string_view kBlobFolder = "blobs";

    bool
    isRegularFile(const fs::path& file)
    {
        std::error_code ec;

        return fs::is_regular_file(file, ec);
    }

} // anon namespace

DirectorySource::DirectorySource(const std::string& path)
    : _path(path)
{
    const fs::path root(path);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        throw DirectorySourceError(path + ": not a folder");
    }
    for (const char* name : kTopLevelMembers) {
        const fs::path file = root / name;
        if (isRegularFile(file)) {
            readMember(name, file.string());
        }
    }

    const fs::path blobs = root / kBlobFolder;
    if (!fs::is_directory(blobs, ec)) {
        return;
    }
    fs::recursive_directory_iterator it(blobs, ec);
    if (ec) {
        throw DirectorySourceError(blobs.string() + ": " + ec.message());
    }
    for (const fs::recursive_directory_iterator end; it != end; it.increment(ec)) {
        if (ec) {
            throw DirectorySourceError(blobs.string() + ": " + ec.message());
        }
        if (!isRegularFile(it->path())) {
            continue;
        }
        const std::string relative = it->path().lexically_relative(root).generic_string();
        readMember(relative, it->path().string());
    }
    if (ec) {
        throw DirectorySourceError(blobs.string() + ": " + ec.message());
    }
}

DirectorySource::~DirectorySource() = default;

void
DirectorySource::readMember(const std::string& name,
                            const std::string& file)
{
    std::FILE* stream = std::fopen(file.c_str(), "rb");
    if (!stream) {
        throw DirectorySourceError(file + ": " + std::strerror(errno));
    }
    std::error_code ec;
    const std::uintmax_t declared = fs::file_size(file, ec);
    if (ec) {
        std::fclose(stream);
        throw DirectorySourceError(file + ": " + ec.message());
    }
    const std::size_t size = static_cast<std::size_t>(declared);
    // aligned_alloc needs a non-zero multiple of the alignment.
    const std::size_t rounded = std::max<std::size_t>(1, (size + kZipMemberAlignment - 1) / kZipMemberAlignment) * kZipMemberAlignment;
    Member member;
    member.bytes.reset(static_cast<std::byte*>(std::aligned_alloc(kZipMemberAlignment, rounded)));
    if (!member.bytes) {
        std::fclose(stream);
        throw DirectorySourceError(file + ": cannot allocate " + std::to_string(size) + " bytes");
    }
    const std::size_t read = std::fread(member.bytes.get(), 1, size, stream);
    // A second read past the declared size tells a file that grew since file_size apart from one
    // that matches it.
    char extra;
    const bool longer = std::fread(&extra, 1, 1, stream) != 0;
    const bool failed = std::ferror(stream) != 0;
    std::fclose(stream);
    if (failed || (read != size) || longer) {
        throw DirectorySourceError(file + ": could not be read completely");
    }
    member.size = size;
    _members.insert_or_assign(name, std::move(member));
}

std::vector<std::string>
DirectorySource::memberNames() const
{
    std::vector<std::string> names;
    names.reserve(_members.size());
    for (const auto& member : _members) {
        names.push_back(member.first);
    }

    return names;
}

std::optional<std::span<const std::byte>>
DirectorySource::findMember(std::string_view path) const
{
    const auto it = _members.find(path);
    if (it == _members.end()) {
        return std::nullopt;
    }

    return std::span<const std::byte>(it->second.bytes.get(), it->second.size);
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
