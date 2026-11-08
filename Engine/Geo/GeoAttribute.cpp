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

#include "GeoAttribute.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

#include <QThreadPool>

#include "Engine/ContentHasher.h"
#include "Engine/PoolParallelFor.h"
#include "Engine/TLSHolder.h"

NATRON_NAMESPACE_ENTER

namespace {

constexpr std::align_val_t kGeoPageAlignment { 64 };

void
replicateElement(unsigned char* dst, const void* value, std::size_t elementBytes, std::size_t count)
{
    if (count == 0) {
        return;
    }
    std::memcpy(dst, value, elementBytes);
    std::size_t done = 1;
    while (done < count) {
        const std::size_t chunk = std::min(done, count - done);
        std::memcpy(dst + done * elementBytes, dst, chunk * elementBytes);
        done += chunk;
    }
}

struct ArrayPair {
    std::int64_t start;
    std::int64_t count;
};

} // namespace

void
geoParallelFor(std::size_t count,
               const std::function<void(std::size_t)>& body,
               std::size_t minParallelCount)
{
    int threads = AppTLS::currentThreadBudget();
    if (threads <= 0) {
        threads = QThreadPool::globalInstance()->maxThreadCount();
    }
    if (count < std::max<std::size_t>(minParallelCount, 2) || threads <= 1) {
        for (std::size_t i = 0; i < count; ++i) {
            body(i);
        }
        return;
    }
    // A few chunks per thread evens out pages that cost more than others (dense vs constant).
    const std::size_t chunks = std::min(count, static_cast<std::size_t>(threads) * 4);
    const std::size_t perChunk = (count + chunks - 1) / chunks;
    const int chunkCount = static_cast<int>((count + perChunk - 1) / perChunk);
    parallelForOnGlobalPool(chunkCount, threads, [&](int c) {
        const std::size_t begin = static_cast<std::size_t>(c) * perChunk;
        const std::size_t end = std::min(count, begin + perChunk);
        for (std::size_t i = begin; i < end; ++i) {
            body(i);
        }
    });
}

// ---------------------------------------------------------------------------------------------
// GeoPage

GeoPage::GeoPage(PrivateTag,
                 std::size_t elementBytes,
                 bool constant)
    : _elementBytes(elementBytes)
    , _constant(constant)
    , _data(static_cast<unsigned char*>(::operator new(storedBytes(), kGeoPageAlignment)))
    , _hash(0)
{
}

GeoPage::~GeoPage()
{
    ::operator delete(_data, kGeoPageAlignment);
}

std::shared_ptr<GeoPage>
GeoPage::makeConstant(std::size_t elementBytes,
                      const void* value)
{
    std::shared_ptr<GeoPage> page = std::make_shared<GeoPage>(PrivateTag(), elementBytes, true);
    std::memcpy(page->_data, value, elementBytes);

    return page;
}

std::shared_ptr<GeoPage>
GeoPage::makeDense(std::size_t elementBytes,
                   const void* fillValue)
{
    std::shared_ptr<GeoPage> page = makeDenseUninitialized(elementBytes);
    replicateElement(page->_data, fillValue, elementBytes, kGeoPageSize);

    return page;
}

std::shared_ptr<GeoPage>
GeoPage::makeDenseUninitialized(std::size_t elementBytes)
{
    return std::make_shared<GeoPage>(PrivateTag(), elementBytes, false);
}

std::shared_ptr<GeoPage>
GeoPage::denseCopy() const
{
    if (_constant) {
        return makeDense(_elementBytes, _data);
    }
    std::shared_ptr<GeoPage> page = makeDenseUninitialized(_elementBytes);
    std::memcpy(page->_data, _data, storedBytes());

    return page;
}

unsigned char*
GeoPage::mutableData() noexcept
{
    _hash.store(0, std::memory_order_relaxed);

    return _data;
}

bool
GeoPage::isUniform() const noexcept
{
    if (_constant) {
        return true;
    }

    // Each element equals its successor exactly when the buffer equals itself shifted by one.
    return std::memcmp(_data, _data + _elementBytes, _elementBytes * (kGeoPageSize - 1)) == 0;
}

U64
GeoPage::contentHash() const
{
    U64 cached = _hash.load(std::memory_order_acquire);
    if (cached != 0) {
        return cached;
    }
    ContentHasher hasher;
    hasher.appendU64(_elementBytes);
    if (_constant) {
        // ContentHasher length-prefixes each append, so the page must go in as one dense buffer
        // to hash equal to a dense page of the same value.
        std::vector<unsigned char> dense(_elementBytes * kGeoPageSize);
        replicateElement(dense.data(), _data, _elementBytes, kGeoPageSize);
        hasher.appendBytes(dense.data(), dense.size());
    } else {
        hasher.appendBytes(_data, storedBytes());
    }
    cached = hasher.finalize();
    _hash.store(cached, std::memory_order_release);

    return cached;
}

// ---------------------------------------------------------------------------------------------
// GeoPagedColumn

GeoPagedColumn::GeoPagedColumn(std::size_t elementBytes,
                               const void* defaultValue,
                               std::size_t size)
    : _elementBytes(elementBytes)
    , _size(0)
    , _pages(std::make_shared<PageTable>())
{
    if (elementBytes == 0) {
        throw std::invalid_argument("GeoPagedColumn: zero element size");
    }
    if (defaultValue) {
        _defaultPage = GeoPage::makeConstant(elementBytes, defaultValue);
    } else {
        const std::vector<unsigned char> zeros(elementBytes, 0);
        _defaultPage = GeoPage::makeConstant(elementBytes, zeros.data());
    }
    resize(size);
}

void
GeoPagedColumn::checkRange(std::size_t begin,
                           std::size_t end) const
{
    if (begin > end || end > _size) {
        throw std::out_of_range("GeoPagedColumn: range outside the column");
    }
}

void
GeoPagedColumn::checkOffset(std::size_t offset) const
{
    if (offset >= _size) {
        throw std::out_of_range("GeoPagedColumn: offset outside the column");
    }
}

bool
GeoPagedColumn::elementIsDefault(const GeoPage& page,
                                 std::size_t indexInPage) const
{
    return std::memcmp(page.element(indexInPage), _defaultPage->data(), _elementBytes) == 0;
}

GeoPageConstPtr
GeoPagedColumn::constantPageFor(const void* value) const
{
    if (std::memcmp(value, _defaultPage->data(), _elementBytes) == 0) {
        return _defaultPage;
    }

    return GeoPage::makeConstant(_elementBytes, value);
}

void
GeoPagedColumn::resize(std::size_t size)
{
    if (size == _size) {
        return;
    }
    if (!_defaultPage) {
        throw std::logic_error("GeoPagedColumn: resize of a column with no element type");
    }
    const std::size_t pages = (size + kGeoPageMask) >> kGeoPageBits;
    makePageTableUnique();
    if (size < _size) {
        _pages->resize(pages);
        _size = size;
        resetTailToDefault();
    } else {
        _pages->resize(pages, _defaultPage);
        _size = size;
    }
}

void
GeoPagedColumn::resetTailToDefault()
{
    const std::size_t tail = _size & kGeoPageMask;
    if (tail == 0 || _pages->empty()) {
        return;
    }
    const std::size_t last = _pages->size() - 1;
    const GeoPage& page = *(*_pages)[last];
    bool clean = true;
    if (page.isConstant()) {
        clean = elementIsDefault(page, 0);
    } else {
        for (std::size_t i = tail; i < kGeoPageSize && clean; ++i) {
            clean = elementIsDefault(page, i);
        }
    }
    if (clean) {
        return;
    }
    unsigned char* data = writablePage(last, true);
    replicateElement(data + tail * _elementBytes, _defaultPage->data(), _elementBytes, kGeoPageSize - tail);
}

void
GeoPagedColumn::makePageTableUnique()
{
    if (!_pages) {
        _pages = std::make_shared<PageTable>();
    } else if (!geoIsSoleOwner(_pages)) {
        _pages = std::make_shared<PageTable>(*_pages);
    }
}

unsigned char*
GeoPagedColumn::writablePage(std::size_t pageIndex,
                             bool preserveContent)
{
    makePageTableUnique();
    GeoPageConstPtr& slot = (*_pages)[pageIndex];
    if (!slot->isConstant() && geoIsSoleOwner(slot)) {
        // Every page is created non-const by GeoPage's factories; the slot only exposes it const.
        return const_cast<GeoPage*>(slot.get())->mutableData();
    }
    std::shared_ptr<GeoPage> fresh = preserveContent ? slot->denseCopy() : GeoPage::makeDenseUninitialized(_elementBytes);
    unsigned char* data = fresh->mutableData();
    slot = std::move(fresh);

    return data;
}

unsigned char*
GeoPagedColumn::pageForWriting(std::size_t pageIndex)
{
    if (pageIndex >= pageCount()) {
        throw std::out_of_range("GeoPagedColumn: page index outside the column");
    }

    return writablePage(pageIndex, true);
}

void
GeoPagedColumn::setElement(std::size_t offset,
                           const void* value)
{
    checkOffset(offset);
    if (std::memcmp(element(offset), value, _elementBytes) == 0) {
        return;
    }
    std::memcpy(writablePage(offset >> kGeoPageBits, true) + (offset & kGeoPageMask) * _elementBytes, value, _elementBytes);
}

unsigned char*
GeoPagedColumn::elementForWriting(std::size_t offset)
{
    checkOffset(offset);

    return writablePage(offset >> kGeoPageBits, true) + (offset & kGeoPageMask) * _elementBytes;
}

void
GeoPagedColumn::fill(std::size_t begin,
                     std::size_t end,
                     const void* value)
{
    checkRange(begin, end);
    if (begin == end) {
        return;
    }
    GeoPageConstPtr constantPage;
    for (std::size_t pi = begin >> kGeoPageBits; pi <= ((end - 1) >> kGeoPageBits); ++pi) {
        const std::size_t pageBegin = pi << kGeoPageBits;
        const std::size_t a = std::max(begin, pageBegin);
        const std::size_t b = std::min(end, pageBegin + kGeoPageSize);
        const GeoPage& current = *(*_pages)[pi];
        if (a == pageBegin && b == pageBegin + kGeoPageSize) {
            if (current.isConstant() && std::memcmp(current.data(), value, _elementBytes) == 0) {
                continue;
            }
            if (!constantPage) {
                constantPage = constantPageFor(value);
            }
            makePageTableUnique();
            (*_pages)[pi] = constantPage;
            continue;
        }
        bool unchanged = true;
        for (std::size_t i = a; i < b && unchanged; ++i) {
            unchanged = std::memcmp(current.element(i - pageBegin), value, _elementBytes) == 0;
        }
        if (unchanged) {
            continue;
        }
        unsigned char* data = writablePage(pi, true);
        replicateElement(data + (a - pageBegin) * _elementBytes, value, _elementBytes, b - a);
    }
}

void
GeoPagedColumn::copyOut(std::size_t begin,
                        std::size_t end,
                        void* out) const
{
    checkRange(begin, end);
    if (begin == end) {
        return;
    }
    unsigned char* dst = static_cast<unsigned char*>(out);
    for (std::size_t pi = begin >> kGeoPageBits; pi <= ((end - 1) >> kGeoPageBits); ++pi) {
        const std::size_t pageBegin = pi << kGeoPageBits;
        const std::size_t a = std::max(begin, pageBegin);
        const std::size_t b = std::min(end, pageBegin + kGeoPageSize);
        const GeoPage& page = *(*_pages)[pi];
        unsigned char* chunk = dst + (a - begin) * _elementBytes;
        if (page.isConstant()) {
            replicateElement(chunk, page.data(), _elementBytes, b - a);
        } else {
            std::memcpy(chunk, page.element(a - pageBegin), (b - a) * _elementBytes);
        }
    }
}

void
GeoPagedColumn::copyOut(std::span<const std::size_t> offsets,
                        void* out) const
{
    unsigned char* dst = static_cast<unsigned char*>(out);
    for (std::size_t k = 0; k < offsets.size(); ++k) {
        checkOffset(offsets[k]);
        std::memcpy(dst + k * _elementBytes, element(offsets[k]), _elementBytes);
    }
}

void
GeoPagedColumn::scatterIn(std::size_t begin,
                          std::size_t end,
                          const void* in)
{
    checkRange(begin, end);
    if (begin == end) {
        return;
    }
    const unsigned char* src = static_cast<const unsigned char*>(in);
    for (std::size_t pi = begin >> kGeoPageBits; pi <= ((end - 1) >> kGeoPageBits); ++pi) {
        const std::size_t pageBegin = pi << kGeoPageBits;
        const std::size_t a = std::max(begin, pageBegin);
        const std::size_t b = std::min(end, pageBegin + kGeoPageSize);
        const unsigned char* chunk = src + (a - begin) * _elementBytes;
        const GeoPage& current = *(*_pages)[pi];
        bool unchanged = true;
        if (current.isConstant()) {
            for (std::size_t i = 0; i < b - a && unchanged; ++i) {
                unchanged = std::memcmp(current.data(), chunk + i * _elementBytes, _elementBytes) == 0;
            }
        } else {
            unchanged = std::memcmp(current.element(a - pageBegin), chunk, (b - a) * _elementBytes) == 0;
        }
        if (unchanged) {
            continue;
        }
        const bool wholePage = a == pageBegin && b == pageBegin + kGeoPageSize;
        unsigned char* data = writablePage(pi, !wholePage);
        std::memcpy(data + (a - pageBegin) * _elementBytes, chunk, (b - a) * _elementBytes);
    }
}

void
GeoPagedColumn::scatterIn(std::span<const std::size_t> offsets,
                          const void* in)
{
    const unsigned char* src = static_cast<const unsigned char*>(in);
    for (std::size_t k = 0; k < offsets.size(); ++k) {
        setElement(offsets[k], src + k * _elementBytes);
    }
}

void
GeoPagedColumn::compactConstantPages()
{
    const std::size_t n = pageCount();
    for (std::size_t pi = 0; pi < n; ++pi) {
        const GeoPage& page = *(*_pages)[pi];
        if (page.isConstant() || !page.isUniform()) {
            continue;
        }
        GeoPageConstPtr replacement = constantPageFor(page.data());
        makePageTableUnique();
        (*_pages)[pi] = std::move(replacement);
    }
}

GeoPagedColumn
GeoPagedColumn::gathered(std::span<const std::size_t> sourceOffsets) const
{
    GeoPagedColumn out;
    out._elementBytes = _elementBytes;
    out._defaultPage = _defaultPage;
    out._size = sourceOffsets.size();
    const std::size_t pages = (out._size + kGeoPageMask) >> kGeoPageBits;
    out._pages = std::make_shared<PageTable>(pages);
    geoParallelFor(pages, [&](std::size_t pi) {
        const std::size_t pageBegin = pi << kGeoPageBits;
        const std::size_t count = std::min(kGeoPageSize, out._size - pageBegin);
        const std::size_t* src = sourceOffsets.data() + pageBegin;

        const std::size_t first = src[0];
        bool shared = (first & kGeoPageMask) == 0 && first + count <= _size;
        for (std::size_t k = 1; k < count && shared; ++k) {
            shared = src[k] == first + k;
        }
        if (shared && count < kGeoPageSize) {
            const GeoPage& page = *(*_pages)[first >> kGeoPageBits];
            if (page.isConstant()) {
                shared = elementIsDefault(page, 0);
            } else {
                for (std::size_t k = count; k < kGeoPageSize && shared; ++k) {
                    shared = elementIsDefault(page, k);
                }
            }
        }
        if (shared) {
            (*out._pages)[pi] = (*_pages)[first >> kGeoPageBits];
            return;
        }

        std::shared_ptr<GeoPage> page = GeoPage::makeDenseUninitialized(_elementBytes);
        unsigned char* data = page->mutableData();
        for (std::size_t k = 0; k < count; ++k) {
            checkOffset(src[k]);
            std::memcpy(data + k * _elementBytes, element(src[k]), _elementBytes);
        }
        replicateElement(data + count * _elementBytes, _defaultPage->data(), _elementBytes, kGeoPageSize - count);
        if (page->isUniform()) {
            (*out._pages)[pi] = constantPageFor(page->data());
        } else {
            (*out._pages)[pi] = std::move(page);
        }
    });

    return out;
}

U64
GeoPagedColumn::contentHash() const
{
    ContentHasher hasher;
    hasher.appendU64(_elementBytes);
    hasher.appendU64(_size);
    const std::size_t n = pageCount();
    for (std::size_t pi = 0; pi < n; ++pi) {
        hasher.appendU64((*_pages)[pi]->contentHash());
    }

    return hasher.finalize();
}

std::size_t
GeoPagedColumn::getSizeInBytes() const
{
    std::size_t bytes = pageCount() * sizeof(GeoPageConstPtr);
    const std::size_t n = pageCount();
    for (std::size_t pi = 0; pi < n; ++pi) {
        bytes += (*_pages)[pi]->storedBytes();
    }

    return bytes;
}

// ---------------------------------------------------------------------------------------------
// GeoStringTable

GeoStringTable::GeoStringTable()
{
    intern(std::string_view());
}

std::int32_t
GeoStringTable::find(std::string_view s) const
{
    const auto it = _index.find(s);

    return it == _index.end() ? -1 : it->second;
}

std::int32_t
GeoStringTable::intern(std::string_view s)
{
    const std::int32_t existing = find(s);
    if (existing >= 0) {
        return existing;
    }
    if (_strings.size() >= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::length_error("GeoStringTable: too many strings");
    }
    const std::int32_t index = static_cast<std::int32_t>(_strings.size());
    _strings.emplace_back(s);
    _index.emplace(_strings.back(), index);

    return index;
}

// ---------------------------------------------------------------------------------------------
// GeoAttribute

std::size_t
geoStorageComponentBytes(GeoStorageEnum storage)
{
    switch (storage) {
    case eGeoStorageInt32:
        return sizeof(std::int32_t);
    case eGeoStorageInt64:
        return sizeof(std::int64_t);
    case eGeoStorageFloat32:
        return sizeof(float);
    case eGeoStorageFloat64:
        return sizeof(double);
    case eGeoStorageString:
        return sizeof(std::int32_t);
    }
    throw std::invalid_argument("geoStorageComponentBytes: unknown storage");
}

GeoAttribute::GeoAttribute(GeoAttributeDesc desc,
                           std::size_t size)
    : _desc(std::move(desc))
{
    if (_desc.tupleSize < 1 || _desc.tupleSize > kMaxTupleSize) {
        throw std::invalid_argument("GeoAttribute: tuple size must be 1-16");
    }
    const std::size_t tupleBytes = geoStorageComponentBytes(_desc.storage) * static_cast<std::size_t>(_desc.tupleSize);
    if (_desc.isArray) {
        _values = GeoPagedColumn(sizeof(ArrayPair), nullptr, size);
        _pool = GeoPagedColumn(tupleBytes, nullptr, 0);
    } else {
        _values = GeoPagedColumn(tupleBytes, nullptr, size);
    }
    if (_desc.storage == eGeoStorageString) {
        _strings = std::make_shared<GeoStringTable>();
    }
}

void
GeoAttribute::throwTypeMismatch() const
{
    throw std::logic_error("GeoAttribute '" + _desc.name + "': value type does not match the storage");
}

void
GeoAttribute::checkArrayFlag(bool array) const
{
    if (_desc.isArray != array) {
        throw std::logic_error("GeoAttribute '" + _desc.name + (array ? "' is not an array attribute" : "' is an array attribute; use getArray/setArray"));
    }
}

void
GeoAttribute::checkComponent(int component) const
{
    if (component < 0 || component >= _desc.tupleSize) {
        throw std::out_of_range("GeoAttribute '" + _desc.name + "': component outside the tuple");
    }
}

void
GeoAttribute::checkTupleSpan(std::size_t count) const
{
    if (count != static_cast<std::size_t>(_desc.tupleSize)) {
        throw std::invalid_argument("GeoAttribute '" + _desc.name + "': value count differs from the tuple size");
    }
}

void
GeoAttribute::checkDenseSpan(std::size_t elements,
                             std::size_t count) const
{
    if (count != elements * static_cast<std::size_t>(_desc.tupleSize)) {
        throw std::invalid_argument("GeoAttribute '" + _desc.name + "': buffer size differs from elements * tuple size");
    }
}

void
GeoAttribute::checkStringIndexValues(const std::int32_t* values,
                                     std::size_t count) const
{
    const std::int32_t tableSize = static_cast<std::int32_t>(_strings->size());
    for (std::size_t i = 0; i < count; ++i) {
        if (values[i] < 0 || values[i] >= tableSize) {
            throw std::out_of_range("GeoAttribute '" + _desc.name + "': string index outside the string table");
        }
    }
}

std::size_t
GeoAttribute::checkedOffset(std::size_t offset) const
{
    if (offset >= _values.size()) {
        throw std::out_of_range("GeoAttribute '" + _desc.name + "': offset outside the attribute");
    }

    return offset;
}

std::string_view
GeoAttribute::getString(std::size_t offset,
                        int component) const
{
    if (_desc.storage != eGeoStorageString) {
        throwTypeMismatch();
    }

    return _strings->at(get<std::int32_t>(offset, component));
}

void
GeoAttribute::setString(std::size_t offset,
                        std::string_view value,
                        int component)
{
    checkScalarAccess<std::int32_t>(false);
    checkComponent(component);
    checkedOffset(offset);
    set<std::int32_t>(offset, internString(value), component);
}

const GeoStringTable&
GeoAttribute::stringTable() const
{
    if (_desc.storage != eGeoStorageString) {
        throwTypeMismatch();
    }

    return *_strings;
}

GeoStringTable&
GeoAttribute::stringTableForWriting()
{
    if (!geoIsSoleOwner(_strings)) {
        _strings = std::make_shared<GeoStringTable>(*_strings);
    }

    return *_strings;
}

std::int32_t
GeoAttribute::internString(std::string_view s)
{
    if (_desc.storage != eGeoStorageString) {
        throwTypeMismatch();
    }
    const std::int32_t existing = _strings->find(s);
    if (existing >= 0) {
        return existing;
    }

    return stringTableForWriting().intern(s);
}

GeoAttribute::ArrayRange
GeoAttribute::arrayRange(std::size_t offset) const
{
    checkArrayFlag(true);
    ArrayPair pair;
    std::memcpy(&pair, _values.element(checkedOffset(offset)), sizeof(pair));

    return ArrayRange { static_cast<std::size_t>(pair.start), static_cast<std::size_t>(pair.count) };
}

std::size_t
GeoAttribute::arrayLength(std::size_t offset) const
{
    return arrayRange(offset).count;
}

void
GeoAttribute::setArrayBytes(std::size_t offset,
                            const void* values,
                            std::size_t componentCount)
{
    const ArrayRange current = arrayRange(offset);
    const std::size_t tupleSize = static_cast<std::size_t>(_desc.tupleSize);
    if (componentCount % tupleSize != 0) {
        throw std::invalid_argument("GeoAttribute '" + _desc.name + "': array length is not a whole number of tuples");
    }
    const std::size_t count = componentCount / tupleSize;
    const std::size_t tupleBytes = _pool.elementBytes();
    const unsigned char* src = static_cast<const unsigned char*>(values);

    if (count == current.count) {
        bool unchanged = true;
        for (std::size_t i = 0; i < count && unchanged; ++i) {
            unchanged = std::memcmp(_pool.element(current.start + i), src + i * tupleBytes, tupleBytes) == 0;
        }
        if (unchanged) {
            return;
        }
    }

    ArrayPair pair { 0, 0 };
    if (count > 0) {
        const std::size_t start = _pool.size();
        _pool.resize(start + count);
        _pool.scatterIn(start, start + count, src);
        pair.start = static_cast<std::int64_t>(start);
        pair.count = static_cast<std::int64_t>(count);
    }
    _values.setElement(offset, &pair);
}

void
GeoAttribute::getStringArray(std::size_t offset,
                             std::vector<std::string>& out) const
{
    if (_desc.storage != eGeoStorageString) {
        throwTypeMismatch();
    }
    std::vector<std::int32_t> indices;
    getArray<std::int32_t>(offset, indices);
    out.clear();
    out.reserve(indices.size());
    for (std::int32_t index : indices) {
        out.push_back(_strings->at(index));
    }
}

void
GeoAttribute::setStringArray(std::size_t offset,
                             std::span<const std::string> values)
{
    checkScalarAccess<std::int32_t>(true);
    checkedOffset(offset);
    std::vector<std::int32_t> indices;
    indices.reserve(values.size());
    for (const std::string& s : values) {
        indices.push_back(internString(s));
    }
    setArray<std::int32_t>(offset, indices);
}

void
GeoAttribute::compactArrayPool()
{
    checkArrayFlag(true);
    const std::size_t n = size();
    std::size_t live = 0;
    bool alreadyCompact = true;
    for (std::size_t i = 0; i < n; ++i) {
        const ArrayRange r = arrayRange(i);
        if (r.count && r.start != live) {
            alreadyCompact = false;
        }
        live += r.count;
    }
    if (alreadyCompact && live == _pool.size()) {
        return;
    }

    GeoPagedColumn pool(_pool.elementBytes(), nullptr, live);
    std::vector<unsigned char> buffer;
    std::size_t next = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const ArrayRange r = arrayRange(i);
        ArrayPair pair { 0, 0 };
        if (r.count) {
            buffer.resize(r.count * _pool.elementBytes());
            _pool.copyOut(r.start, r.start + r.count, buffer.data());
            pool.scatterIn(next, next + r.count, buffer.data());
            pair.start = static_cast<std::int64_t>(next);
            pair.count = static_cast<std::int64_t>(r.count);
            next += r.count;
        }
        _values.setElement(i, &pair);
    }
    _pool = std::move(pool);
}

void
GeoAttribute::copyElement(std::size_t dstOffset,
                          std::size_t srcOffset)
{
    // Writing may replace the page the source element lives in, so it is copied out first.
    unsigned char buffer[kMaxTupleSize * sizeof(std::int64_t)];
    static_assert(sizeof(buffer) >= sizeof(ArrayPair));
    std::memcpy(buffer, _values.element(checkedOffset(srcOffset)), _values.elementBytes());
    _values.setElement(checkedOffset(dstOffset), buffer);
}

GeoAttribute
GeoAttribute::gathered(std::span<const std::size_t> sourceOffsets) const
{
    GeoAttribute out;
    out._desc = _desc;
    out._values = _values.gathered(sourceOffsets);
    out._pool = _pool;
    out._strings = _strings;

    return out;
}

U64
GeoAttribute::contentHash() const
{
    ContentHasher hasher;
    hasher.appendString(_desc.name);
    hasher.appendU64(static_cast<U64>(_desc.storage));
    hasher.appendU64(static_cast<U64>(_desc.tupleSize));
    hasher.appendU64(static_cast<U64>(_desc.typeInfo));
    hasher.appendBool(_desc.isArray);
    hasher.appendU64(_values.defaultValue() ? _values.defaultPage()->contentHash() : 0);
    hasher.appendU64(_values.contentHash());
    if (_desc.isArray) {
        hasher.appendU64(_pool.contentHash());
    }
    if (_strings) {
        hasher.appendU64(_strings->size());
        for (std::size_t i = 0; i < _strings->size(); ++i) {
            hasher.appendString(_strings->at(static_cast<std::int32_t>(i)));
        }
    }

    return hasher.finalize();
}

std::size_t
GeoAttribute::getSizeInBytes() const
{
    std::size_t bytes = _values.getSizeInBytes() + _pool.getSizeInBytes();
    if (_strings) {
        for (std::size_t i = 0; i < _strings->size(); ++i) {
            bytes += sizeof(std::string) + _strings->at(static_cast<std::int32_t>(i)).capacity();
        }
    }

    return bytes;
}

NATRON_NAMESPACE_EXIT
