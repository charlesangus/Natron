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

#ifndef Engine_Nodes_Metadata_ImageMetadata_h
#define Engine_Nodes_Metadata_ImageMetadata_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "Global/GlobalDefines.h"

NATRON_NAMESPACE_ENTER

enum class ImageMetadataType {
    eInt,
    eDouble,
    eString,
    eIntVector,
    eDoubleVector
};

/**
 * @brief The per-frame key/value metadata of an image. Keys carry the ofx/, exr/, exif/
 * and dpx/ prefixes of ofxMetadata.h and iterate in ascending byte order of the key.
 *
 * Copies are O(1): they share one immutable map and the first mutation of a copy
 * detaches it, so a copy never observes a later change to the original. A single
 * instance must not be mutated from two threads at once; distinct copies may be used
 * from different threads freely.
 *
 * Doubles are compared and hashed by bit pattern, so NaN equals an identical NaN and
 * 0.0 differs from -0.0. This keeps operator== and hash() consistent with each other.
 **/
class ImageMetadata {
public:
    typedef std::variant<int, double, std::string, std::vector<int>, std::vector<double>> Value;
    typedef std::map<std::string, Value> Map;
    typedef Map::const_iterator const_iterator;

    /**
     * @brief Which side wins when both metadata sets hold the same key.
     **/
    enum MergePrecedence {
        eMergePreferThis,
        eMergePreferOther
    };

    ImageMetadata() { }

    bool empty() const;
    std::size_t size() const;

    const_iterator begin() const;
    const_iterator end() const;

    bool contains(const std::string& key) const;

    /**
     * @brief The value for key, or nullptr if absent. Invalidated by any mutation of this object.
     **/
    const Value* find(const std::string& key) const;

    std::optional<ImageMetadataType> typeOf(const std::string& key) const;

    /**
     * @brief Typed getters: nullopt if the key is absent or holds a different type.
     **/
    std::optional<int> getInt(const std::string& key) const;
    std::optional<double> getDouble(const std::string& key) const;
    std::optional<std::string> getString(const std::string& key) const;
    std::optional<std::vector<int>> getIntVector(const std::string& key) const;
    std::optional<std::vector<double>> getDoubleVector(const std::string& key) const;

    /**
     * @brief Setters insert or replace, including a change of type.
     **/
    void set(const std::string& key, Value value);
    void setInt(const std::string& key, int value);
    void setDouble(const std::string& key, double value);
    void setString(const std::string& key, const std::string& value);
    void setIntVector(const std::string& key, const std::vector<int>& value);
    void setDoubleVector(const std::string& key, const std::vector<double>& value);

    /**
     * @brief Returns true if the key existed.
     **/
    bool remove(const std::string& key);
    void clear();

    /**
     * @brief Copies every entry of other into this. On a key present in both, precedence
     * decides which value is kept: eMergePreferThis keeps this object's, eMergePreferOther
     * takes other's. Keys present on one side only are always kept.
     **/
    void merge(const ImageMetadata& other, MergePrecedence precedence);

    bool operator==(const ImageMetadata& other) const;
    bool operator!=(const ImageMetadata& other) const
    {
        return !(*this == other);
    }

    /**
     * @brief A hash of the contents that is independent of insertion order, pointer values
     * and platform string hashing, so it is safe in cache keys. Empty metadata hashes to 0.
     **/
    U64 hash() const;

    /**
     * @brief True if both objects currently reference the same storage (or are both empty).
     **/
    bool sharesStorageWith(const ImageMetadata& other) const;

private:
    Map& detach();

    std::shared_ptr<Map> _data;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Metadata_ImageMetadata_h
