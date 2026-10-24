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

#ifndef Serialization_Bundle_DirectorySource_h
#define Serialization_Bundle_DirectorySource_h

#include "Global/Macros.h"

#include <cstddef>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "Serialization/Yaml/BlobSink.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

class DirectorySourceError
    : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * @brief The members of an unzipped project bundle, read from a folder: `project.yaml`,
 * `mimetype` and `manifest.yaml` when they are regular files there, and every regular file under
 * `blobs/`. Member names are relative to the folder with '/' separators, as in the archive.
 *
 * Everything is read up front into buffers owned by the source and aligned to
 * kZipMemberAlignment, so member bytes can be viewed as typed columns just as a mapped archive's,
 * and the folder may change afterwards without affecting a load in progress.
 **/
class DirectorySource
    : public BlobSource {
public:
    /**
     * @brief Throws DirectorySourceError when path is not a readable folder or a file in it
     * cannot be read.
     **/
    explicit DirectorySource(const std::string& path);
    ~DirectorySource() override;

    DirectorySource(const DirectorySource&) = delete;
    DirectorySource& operator=(const DirectorySource&) = delete;

    const std::string& path() const
    {
        return _path;
    }

    /**
     * @brief Every member name, sorted.
     **/
    std::vector<std::string> memberNames() const;

    std::optional<std::span<const std::byte>> findMember(std::string_view path) const override;

private:
    struct AlignedFree {
        void operator()(std::byte* p) const
        {
            std::free(p);
        }
    };

    struct Member {
        std::unique_ptr<std::byte, AlignedFree> bytes;
        std::size_t size = 0;
    };

    void readMember(const std::string& name, const std::string& file);

    std::string _path;
    std::map<std::string, Member, std::less<>> _members;
};

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Bundle_DirectorySource_h
