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

#ifndef Serialization_Yaml_BlobSink_h
#define Serialization_Yaml_BlobSink_h

#include "Global/Macros.h"

#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief A column table with fewer rows than this is written inline in the YAML document as a
 * flow sequence; from this many rows on it becomes a blob table.
 **/
inline constexpr std::size_t kInlineTableRowThreshold = 64;

/**
 * @brief Where an encoder puts the blob tables of heavy data (roto strokes and shapes, tracker
 * curves). A project bundle keeps each as a member of the archive and the document refers to it
 * by path; a self-contained document (a preset, the clipboard) keeps none, and the encoder then
 * writes the table into the document as base64.
 **/
class BlobSink {
public:
    virtual ~BlobSink();

    /**
     * @brief Takes the bytes of the blob the document will name by path. Returns false when the
     * sink keeps no members, so the encoder inlines the bytes instead.
     **/
    virtual bool addMember(const std::string& path, std::vector<std::byte> bytes) = 0;
};

/**
 * @brief Where a decoder finds the blobs a document names by path.
 **/
class BlobSource {
public:
    virtual ~BlobSource();

    /**
     * @brief The bytes of the member at path, nothing when there is no such member. The bytes
     * stay valid as long as the source does.
     **/
    virtual std::optional<std::span<const std::byte>> findMember(std::string_view path) const = 0;
};

/**
 * @brief A sink that keeps no members, for self-contained documents.
 **/
class InlineBlobSink
    : public BlobSink {
public:
    bool addMember(const std::string& path, std::vector<std::byte> bytes) override;
};

/**
 * @brief A source with no members, for documents that inline every blob.
 **/
class NoBlobSource
    : public BlobSource {
public:
    std::optional<std::span<const std::byte>> findMember(std::string_view path) const override;
};

/**
 * @brief Members held in memory by path, in path order: what a bundle writer collects before
 * writing the archive, and a source for tests.
 **/
class MemoryBlobStore
    : public BlobSink,
      public BlobSource {
public:
    /**
     * @brief Throws YamlError when a member with the same path was already added.
     **/
    bool addMember(const std::string& path, std::vector<std::byte> bytes) override;

    std::optional<std::span<const std::byte>> findMember(std::string_view path) const override;

    const std::map<std::string, std::vector<std::byte>, std::less<>>& members() const
    {
        return _members;
    }

    void removeMember(std::string_view path);

private:
    std::map<std::string, std::vector<std::byte>, std::less<>> _members;
};

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Yaml_BlobSink_h
