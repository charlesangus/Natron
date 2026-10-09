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

#include "ImageMetadata.h"

#include <cstdint>
#include <cstring>

#include "Engine/Hash64.h"

NATRON_NAMESPACE_ENTER

namespace {
const ImageMetadata::Map&
emptyMap()
{
    static const ImageMetadata::Map s;

    return s;
}

template <typename T>
const T*
getAs(const ImageMetadata::Value* v)
{
    return v ? std::get_if<T>(v) : nullptr;
}

void
putU64(std::vector<unsigned char>& out,
       std::uint64_t v)
{
    // Explicit little-endian so the byte stream does not depend on the host.
    for (int i = 0; i < 8; ++i) {
        out.push_back((unsigned char)((v >> (8 * i)) & 0xff));
    }
}

void
putI32(std::vector<unsigned char>& out,
       int v)
{
    const std::uint32_t u = (std::uint32_t)v;
    for (int i = 0; i < 4; ++i) {
        out.push_back((unsigned char)((u >> (8 * i)) & 0xff));
    }
}

std::uint64_t
doubleBits(double d)
{
    std::uint64_t u;

    std::memcpy(&u, &d, sizeof(u));

    return u;
}

void
putDouble(std::vector<unsigned char>& out,
          double d)
{
    putU64(out, doubleBits(d));
}

void
putString(std::vector<unsigned char>& out,
          const std::string& s)
{
    putU64(out, s.size());
    out.insert(out.end(), s.begin(), s.end());
}

bool
valueEqual(const ImageMetadata::Value& a,
           const ImageMetadata::Value& b)
{
    if (a.index() != b.index()) {
        return false;
    }
    switch (a.index()) {
    case 1:

        return doubleBits(std::get<double>(a)) == doubleBits(std::get<double>(b));
    case 4: {
        const std::vector<double>& x = std::get<std::vector<double>>(a);
        const std::vector<double>& y = std::get<std::vector<double>>(b);
        if (x.size() != y.size()) {
            return false;
        }
        for (std::size_t i = 0; i < x.size(); ++i) {
            if (doubleBits(x[i]) != doubleBits(y[i])) {
                return false;
            }
        }

        return true;
    }
    default:

        return a == b;
    }
}
} // anon

bool
ImageMetadata::empty() const
{
    return !_data || _data->empty();
}

std::size_t
ImageMetadata::size() const
{
    return _data ? _data->size() : 0;
}

ImageMetadata::const_iterator
ImageMetadata::begin() const
{
    return _data ? _data->begin() : emptyMap().begin();
}

ImageMetadata::const_iterator
ImageMetadata::end() const
{
    return _data ? _data->end() : emptyMap().end();
}

bool
ImageMetadata::contains(const std::string& key) const
{
    return find(key) != nullptr;
}

const ImageMetadata::Value*
ImageMetadata::find(const std::string& key) const
{
    if (!_data) {
        return nullptr;
    }
    Map::const_iterator it = _data->find(key);

    return it == _data->end() ? nullptr : &it->second;
}

std::optional<ImageMetadataType>
ImageMetadata::typeOf(const std::string& key) const
{
    const Value* v = find(key);
    if (!v) {
        return std::nullopt;
    }
    switch (v->index()) {
    case 0:

        return ImageMetadataType::eInt;
    case 1:

        return ImageMetadataType::eDouble;
    case 2:

        return ImageMetadataType::eString;
    case 3:

        return ImageMetadataType::eIntVector;
    default:

        return ImageMetadataType::eDoubleVector;
    }
}

std::optional<int>
ImageMetadata::getInt(const std::string& key) const
{
    const int* p = getAs<int>(find(key));

    return p ? std::optional<int>(*p) : std::nullopt;
}

std::optional<double>
ImageMetadata::getDouble(const std::string& key) const
{
    const double* p = getAs<double>(find(key));

    return p ? std::optional<double>(*p) : std::nullopt;
}

std::optional<std::string>
ImageMetadata::getString(const std::string& key) const
{
    const std::string* p = getAs<std::string>(find(key));

    return p ? std::optional<std::string>(*p) : std::nullopt;
}

std::optional<std::vector<int>>
ImageMetadata::getIntVector(const std::string& key) const
{
    const std::vector<int>* p = getAs<std::vector<int>>(find(key));

    return p ? std::optional<std::vector<int>>(*p) : std::nullopt;
}

std::optional<std::vector<double>>
ImageMetadata::getDoubleVector(const std::string& key) const
{
    const std::vector<double>* p = getAs<std::vector<double>>(find(key));

    return p ? std::optional<std::vector<double>>(*p) : std::nullopt;
}

ImageMetadata::Map&
ImageMetadata::detach()
{
    if (!_data) {
        _data = std::make_shared<Map>();
    } else if (_data.use_count() > 1) {
        _data = std::make_shared<Map>(*_data);
    }

    return *_data;
}

void
ImageMetadata::set(const std::string& key,
                   Value value)
{
    detach()[key] = std::move(value);
}

void
ImageMetadata::setInt(const std::string& key,
                      int value)
{
    set(key, Value(std::in_place_type<int>, value));
}

void
ImageMetadata::setDouble(const std::string& key,
                         double value)
{
    set(key, Value(std::in_place_type<double>, value));
}

void
ImageMetadata::setString(const std::string& key,
                         const std::string& value)
{
    set(key, Value(std::in_place_type<std::string>, value));
}

void
ImageMetadata::setIntVector(const std::string& key,
                            const std::vector<int>& value)
{
    set(key, Value(std::in_place_type<std::vector<int>>, value));
}

void
ImageMetadata::setDoubleVector(const std::string& key,
                               const std::vector<double>& value)
{
    set(key, Value(std::in_place_type<std::vector<double>>, value));
}

bool
ImageMetadata::remove(const std::string& key)
{
    if (!contains(key)) {
        return false;
    }
    detach().erase(key);

    return true;
}

void
ImageMetadata::clear()
{
    _data.reset();
}

void
ImageMetadata::merge(const ImageMetadata& other,
                     MergePrecedence precedence)
{
    if (other.empty() || (_data == other._data)) {
        return;
    }
    if (empty()) {
        _data = other._data;

        return;
    }
    Map& dst = detach();
    for (const_iterator it = other.begin(); it != other.end(); ++it) {
        if (precedence == eMergePreferOther) {
            dst[it->first] = it->second;
        } else {
            dst.emplace(it->first, it->second);
        }
    }
}

bool
ImageMetadata::operator==(const ImageMetadata& other) const
{
    if (_data == other._data) {
        return true;
    }
    if (size() != other.size()) {
        return false;
    }
    const_iterator a = begin();
    const_iterator b = other.begin();
    for (; a != end(); ++a, ++b) {
        if ((a->first != b->first) || !valueEqual(a->second, b->second)) {
            return false;
        }
    }

    return true;
}

U64
ImageMetadata::hash() const
{
    if (empty()) {
        return 0;
    }
    std::vector<unsigned char> bytes;
    putU64(bytes, _data->size());
    for (const_iterator it = begin(); it != end(); ++it) {
        const Value& v = it->second;
        bytes.push_back((unsigned char)v.index());
        putString(bytes, it->first);
        switch (v.index()) {
        case 0:
            putI32(bytes, std::get<int>(v));
            break;
        case 1:
            putDouble(bytes, std::get<double>(v));
            break;
        case 2:
            putString(bytes, std::get<std::string>(v));
            break;
        case 3: {
            const std::vector<int>& vec = std::get<std::vector<int>>(v);
            putU64(bytes, vec.size());
            for (std::size_t i = 0; i < vec.size(); ++i) {
                putI32(bytes, vec[i]);
            }
            break;
        }
        default: {
            const std::vector<double>& vec = std::get<std::vector<double>>(v);
            putU64(bytes, vec.size());
            for (std::size_t i = 0; i < vec.size(); ++i) {
                putDouble(bytes, vec[i]);
            }
            break;
        }
        }
    }
    while (bytes.size() % 8 != 0) {
        bytes.push_back(0);
    }

    Hash64 h;
    for (std::size_t i = 0; i < bytes.size(); i += 8) {
        std::uint64_t word = 0;
        for (int b = 0; b < 8; ++b) {
            word |= (std::uint64_t)bytes[i + b] << (8 * b);
        }
        h.append<U64>(word);
    }
    h.computeHash();

    return h.value();
}

bool
ImageMetadata::sharesStorageWith(const ImageMetadata& other) const
{
    return _data == other._data;
}

NATRON_NAMESPACE_EXIT
