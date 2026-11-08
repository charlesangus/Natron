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

#include "Global/Macros.h"

#include <cstddef>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Geo/GeoAttribute.h"

NATRON_NAMESPACE_USING

namespace {

GeoAttributeDesc
makeDesc(const std::string& name,
         GeoStorageEnum storage,
         int tupleSize = 1,
         bool isArray = false)
{
    GeoAttributeDesc desc;
    desc.name = name;
    desc.storage = storage;
    desc.tupleSize = tupleSize;
    desc.isArray = isArray;

    return desc;
}

// Every page dense with a distinct value per element, so nothing is constant or shared.
GeoAttribute
makeDenseFloat(std::size_t size)
{
    GeoAttribute attr(makeDesc("P", eGeoStorageFloat32), size);
    attr.visitPagesForWriting<float>([](const GeoMutablePageView<float>& page) {
        for (std::size_t i = 0; i < page.count; ++i) {
            page.values[i] = static_cast<float>(page.firstOffset + i);
        }
    });

    return attr;
}

template <typename T>
T
valueFor(std::size_t element,
         int component)
{
    return static_cast<T>(static_cast<std::int64_t>(element) * 17 + component * 3 - 40);
}

template <typename T>
void
roundTripAllTupleSizes(GeoStorageEnum storage)
{
    const std::size_t size = 2 * kGeoPageSize + 37;
    for (int tupleSize = 1; tupleSize <= GeoAttribute::kMaxTupleSize; ++tupleSize) {
        SCOPED_TRACE(tupleSize);
        GeoAttribute attr(makeDesc("a", storage, tupleSize), size);
        std::vector<T> tuple(tupleSize);
        for (std::size_t e = 0; e < size; e += 7) {
            for (int c = 0; c < tupleSize; ++c) {
                tuple[c] = valueFor<T>(e, c);
            }
            attr.setTuple<T>(e, tuple);
        }
        std::vector<T> out(tupleSize);
        for (std::size_t e = 0; e < size; ++e) {
            attr.getTuple<T>(e, out);
            for (int c = 0; c < tupleSize; ++c) {
                const T expected = e % 7 == 0 ? valueFor<T>(e, c) : T(0);
                ASSERT_EQ(out[c], expected);
                ASSERT_EQ(attr.get<T>(e, c), expected);
            }
        }

        std::vector<T> dense(size * tupleSize);
        attr.copyDense<T>(0, size, dense);
        for (T& v : dense) {
            v = v + T(1);
        }
        GeoAttribute other(makeDesc("a", storage, tupleSize), size);
        other.scatterDense<T>(0, size, dense);
        for (std::size_t e = 0; e < size; e += 5) {
            for (int c = 0; c < tupleSize; ++c) {
                ASSERT_EQ(other.get<T>(e, c), attr.get<T>(e, c) + T(1));
            }
        }

        attr.set<T>(3, T(99), tupleSize - 1);
        EXPECT_EQ(attr.get<T>(3, tupleSize - 1), T(99));
    }
}

template <typename T>
void
roundTripArrays(GeoStorageEnum storage)
{
    for (int tupleSize : { 1, 3, 16 }) {
        SCOPED_TRACE(tupleSize);
        GeoAttribute attr(makeDesc("arr", storage, tupleSize, true), 3000);
        EXPECT_EQ(attr.arrayLength(10), 0u);
        for (std::size_t e = 0; e < 3000; e += 11) {
            std::vector<T> values((e % 5) * tupleSize);
            for (std::size_t k = 0; k < values.size(); ++k) {
                values[k] = valueFor<T>(e, static_cast<int>(k));
            }
            attr.setArray<T>(e, values);
        }
        attr.setArray<T>(22, std::vector<T>(4 * tupleSize, T(5)));

        const auto check = [&](const GeoAttribute& a) {
            std::vector<T> out;
            for (std::size_t e = 0; e < 3000; ++e) {
                a.getArray<T>(e, out);
                if (e == 22) {
                    ASSERT_EQ(out, std::vector<T>(4 * tupleSize, T(5)));
                    continue;
                }
                const std::size_t expected = e % 11 == 0 ? (e % 5) * tupleSize : 0;
                ASSERT_EQ(out.size(), expected);
                ASSERT_EQ(a.arrayLength(e) * tupleSize, expected);
                for (std::size_t k = 0; k < out.size(); ++k) {
                    ASSERT_EQ(out[k], valueFor<T>(e, static_cast<int>(k)));
                }
            }
        };
        check(attr);
        GeoAttribute compacted = attr;
        compacted.compactArrayPool();
        check(compacted);
        check(attr);

        if (tupleSize > 1) {
            EXPECT_THROW(attr.setArray<T>(1, std::vector<T>(tupleSize + 1)), std::invalid_argument);
        }
    }
}

} // namespace

TEST(GeoAttribute, CopySharesEveryPage)
{
    const GeoAttribute attr = makeDenseFloat(10 * kGeoPageSize + 5);
    const GeoAttribute copy = attr;

    ASSERT_EQ(copy.pageCount(), attr.pageCount());
    for (std::size_t i = 0; i < attr.pageCount(); ++i) {
        EXPECT_EQ(copy.values().page(i).get(), attr.values().page(i).get());
    }
    EXPECT_EQ(copy.contentHash(), attr.contentHash());
}

TEST(GeoAttribute, SparseWriteDetachesOnlyTouchedPages)
{
    const std::size_t size = 10000000;
    const GeoAttribute source = makeDenseFloat(size);
    GeoAttribute edited = source;

    const std::vector<std::size_t> offsets = { 0, 1, 3000000, 5000000, size - 1 };
    for (std::size_t offset : offsets) {
        edited.set<float>(offset, -1.f);
    }
    std::set<std::size_t> touched;
    for (std::size_t offset : offsets) {
        touched.insert(offset >> kGeoPageBits);
    }
    ASSERT_EQ(touched.size(), 4u);

    std::size_t detached = 0;
    for (std::size_t i = 0; i < source.pageCount(); ++i) {
        const bool shared = edited.values().page(i).get() == source.values().page(i).get();
        EXPECT_EQ(shared, touched.count(i) == 0) << "page " << i;
        detached += shared ? 0 : 1;
    }
    EXPECT_EQ(detached, touched.size());
    for (std::size_t offset : offsets) {
        EXPECT_EQ(edited.get<float>(offset), -1.f);
        EXPECT_EQ(source.get<float>(offset), static_cast<float>(offset));
    }
    EXPECT_EQ(edited.get<float>(2), 2.f);
}

TEST(GeoAttribute, ConstantPageStaysOneValueUntilWritten)
{
    const std::size_t size = 5 * kGeoPageSize;
    GeoAttribute attr(makeDesc("Cd", eGeoStorageFloat32, 3), size);
    for (std::size_t i = 0; i < attr.pageCount(); ++i) {
        EXPECT_TRUE(attr.values().page(i)->isConstant());
        EXPECT_EQ(attr.values().page(i).get(), attr.values().defaultPage().get());
    }

    const std::vector<float> red = { 1.f, 0.f, 0.f };
    attr.fill<float>(0, size, red);
    const GeoPage* redPage = attr.values().page(0).get();
    for (std::size_t i = 0; i < attr.pageCount(); ++i) {
        EXPECT_TRUE(attr.values().page(i)->isConstant());
        EXPECT_EQ(attr.values().page(i).get(), redPage);
        EXPECT_EQ(attr.values().page(i)->storedBytes(), 3 * sizeof(float));
    }

    attr.setTuple<float>(10, red);
    attr.set<float>(kGeoPageSize + 4, 1.f, 0);
    EXPECT_EQ(attr.values().page(0).get(), redPage);
    EXPECT_EQ(attr.values().page(1).get(), redPage);

    attr.set<float>(kGeoPageSize + 4, 0.5f, 1);
    EXPECT_FALSE(attr.values().page(1)->isConstant());
    for (std::size_t i = 0; i < attr.pageCount(); ++i) {
        if (i != 1) {
            EXPECT_EQ(attr.values().page(i).get(), redPage);
        }
    }
    std::vector<float> out(3);
    attr.getTuple<float>(kGeoPageSize + 4, out);
    EXPECT_EQ(out, (std::vector<float> { 1.f, 0.5f, 0.f }));
    attr.getTuple<float>(kGeoPageSize + 5, out);
    EXPECT_EQ(out, red);

    const U64 hashWithEdit = attr.contentHash();
    attr.set<float>(kGeoPageSize + 4, 0.f, 1);
    EXPECT_NE(attr.contentHash(), hashWithEdit);
    EXPECT_FALSE(attr.values().page(1)->isConstant());
    const U64 hashDense = attr.contentHash();
    attr.compactConstantPages();
    EXPECT_TRUE(attr.values().page(1)->isConstant());
    EXPECT_EQ(attr.contentHash(), hashDense);
}

TEST(GeoAttribute, ScatterOfUnchangedDataKeepsSharing)
{
    const std::size_t size = 4 * kGeoPageSize + 100;
    const GeoAttribute source = makeDenseFloat(size);
    GeoAttribute edited = source;

    std::vector<float> dense(size);
    edited.copyDense<float>(0, size, dense);
    dense[2 * kGeoPageSize + 7] = -3.f;
    edited.scatterDense<float>(0, size, dense);

    for (std::size_t i = 0; i < source.pageCount(); ++i) {
        EXPECT_EQ(edited.values().page(i).get() == source.values().page(i).get(), i != 2) << "page " << i;
    }
    EXPECT_EQ(edited.get<float>(2 * kGeoPageSize + 7), -3.f);

    const std::vector<std::size_t> offsets = { 5, 3 * kGeoPageSize + 1 };
    std::vector<float> gatheredValues(2);
    edited.copyDense<float>(offsets, gatheredValues);
    EXPECT_EQ(gatheredValues, (std::vector<float> { 5.f, static_cast<float>(3 * kGeoPageSize + 1) }));
    gatheredValues[1] = 0.25f;
    edited.scatterDense<float>(offsets, std::span<const float>(gatheredValues));
    EXPECT_EQ(edited.values().page(0).get(), source.values().page(0).get());
    EXPECT_NE(edited.values().page(3).get(), source.values().page(3).get());
    EXPECT_EQ(edited.get<float>(3 * kGeoPageSize + 1), 0.25f);
}

TEST(GeoAttribute, NumericRoundTrips)
{
    roundTripAllTupleSizes<std::int32_t>(eGeoStorageInt32);
    roundTripAllTupleSizes<std::int64_t>(eGeoStorageInt64);
    roundTripAllTupleSizes<float>(eGeoStorageFloat32);
    roundTripAllTupleSizes<double>(eGeoStorageFloat64);
}

TEST(GeoAttribute, Int64KeepsFullRange)
{
    GeoAttribute attr(makeDesc("id", eGeoStorageInt64), 3);
    const std::int64_t big = (std::int64_t(1) << 62) + 12345;
    attr.set<std::int64_t>(1, big);
    EXPECT_EQ(attr.get<std::int64_t>(1), big);
}

TEST(GeoAttribute, DefaultValue)
{
    GeoAttribute attr(makeDesc("pscale", eGeoStorageFloat32));
    attr.setDefault<float>(std::vector<float> { 1.f });
    attr.resize(kGeoPageSize + 3);
    EXPECT_EQ(attr.get<float>(kGeoPageSize + 2), 1.f);
    EXPECT_THROW(attr.setDefault<float>(std::vector<float> { 2.f }), std::logic_error);
}

TEST(GeoAttribute, StringRoundTripsAndTableSharing)
{
    GeoAttribute attr(makeDesc("path", eGeoStorageString, 2), 2 * kGeoPageSize);
    EXPECT_EQ(attr.getString(5, 1), "");
    attr.setString(5, "/geo/a", 0);
    attr.setString(5, "/geo/b", 1);
    attr.setString(kGeoPageSize + 1, "/geo/a", 1);
    EXPECT_EQ(attr.getString(5, 0), "/geo/a");
    EXPECT_EQ(attr.getString(5, 1), "/geo/b");
    EXPECT_EQ(attr.getString(kGeoPageSize + 1, 1), "/geo/a");
    EXPECT_EQ(attr.get<std::int32_t>(5, 0), attr.get<std::int32_t>(kGeoPageSize + 1, 1));
    EXPECT_EQ(attr.stringTable().size(), 3u);

    GeoAttribute copy = attr;
    EXPECT_EQ(&copy.stringTable(), &attr.stringTable());
    copy.setString(6, "/geo/a", 0);
    EXPECT_EQ(&copy.stringTable(), &attr.stringTable());
    copy.setString(7, "/geo/c", 0);
    EXPECT_NE(&copy.stringTable(), &attr.stringTable());
    EXPECT_EQ(attr.stringTable().find("/geo/c"), -1);
    EXPECT_EQ(copy.getString(7, 0), "/geo/c");
    EXPECT_EQ(attr.getString(7, 0), "");

    EXPECT_THROW(attr.set<std::int32_t>(0, 42), std::out_of_range);
    EXPECT_THROW(attr.get<float>(0), std::logic_error);
}

TEST(GeoAttribute, ArrayRoundTrips)
{
    roundTripArrays<std::int32_t>(eGeoStorageInt32);
    roundTripArrays<std::int64_t>(eGeoStorageInt64);
    roundTripArrays<float>(eGeoStorageFloat32);
    roundTripArrays<double>(eGeoStorageFloat64);

    GeoAttribute names(makeDesc("tags", eGeoStorageString, 1, true), 10);
    const std::vector<std::string> tags = { "a", "", "bb", "a" };
    names.setStringArray(3, tags);
    std::vector<std::string> out;
    names.getStringArray(3, out);
    EXPECT_EQ(out, tags);
    names.getStringArray(4, out);
    EXPECT_TRUE(out.empty());

    GeoAttribute copy = names;
    EXPECT_EQ(copy.arrayPool().page(0).get(), names.arrayPool().page(0).get());
    copy.setStringArray(4, std::vector<std::string> { "z" });
    names.getStringArray(4, out);
    EXPECT_TRUE(out.empty());
    copy.getStringArray(4, out);
    EXPECT_EQ(out, std::vector<std::string> { "z" });

    EXPECT_THROW(names.get<std::int32_t>(0), std::logic_error);
}

TEST(GeoAttribute, GatherSharesPagesThatDoNotMove)
{
    const std::size_t size = 4 * kGeoPageSize + 10;
    const GeoAttribute source = makeDenseFloat(size);

    std::vector<std::size_t> identity(size);
    for (std::size_t i = 0; i < size; ++i) {
        identity[i] = i;
    }
    const GeoAttribute same = source.gathered(identity);
    for (std::size_t i = 0; i < source.pageCount(); ++i) {
        EXPECT_EQ(same.values().page(i).get(), source.values().page(i).get());
    }

    std::vector<std::size_t> compacted = identity;
    compacted.erase(compacted.begin() + 2 * kGeoPageSize + 3);
    const GeoAttribute moved = source.gathered(compacted);
    ASSERT_EQ(moved.size(), size - 1);
    EXPECT_EQ(moved.values().page(0).get(), source.values().page(0).get());
    EXPECT_EQ(moved.values().page(1).get(), source.values().page(1).get());
    EXPECT_NE(moved.values().page(2).get(), source.values().page(2).get());
    for (std::size_t i = 0; i < moved.size(); ++i) {
        ASSERT_EQ(moved.get<float>(i), static_cast<float>(compacted[i]));
    }

    std::vector<std::size_t> truncated(identity.begin(), identity.begin() + kGeoPageSize + 5);
    const GeoAttribute shorter = source.gathered(truncated);
    EXPECT_EQ(shorter.values().page(0).get(), source.values().page(0).get());
    EXPECT_NE(shorter.values().page(1).get(), source.values().page(1).get());
    GeoAttribute grown = shorter;
    grown.resize(kGeoPageSize + 10);
    EXPECT_EQ(grown.get<float>(kGeoPageSize + 7), 0.f);
}

TEST(GeoAttribute, ShrinkThenGrowRestoresDefaults)
{
    GeoAttribute attr = makeDenseFloat(2 * kGeoPageSize);
    attr.resize(kGeoPageSize + 100);
    attr.resize(2 * kGeoPageSize);
    EXPECT_EQ(attr.get<float>(kGeoPageSize + 99), static_cast<float>(kGeoPageSize + 99));
    EXPECT_EQ(attr.get<float>(kGeoPageSize + 100), 0.f);
    EXPECT_EQ(attr.get<float>(2 * kGeoPageSize - 1), 0.f);
}

TEST(GeoAttribute, EqualContentHashesEqual)
{
    const std::size_t size = 3 * kGeoPageSize;
    GeoAttribute filled(makeDesc("v", eGeoStorageFloat64, 2), size);
    filled.fill<double>(0, size, std::vector<double> { 1.0, 2.0 });

    GeoAttribute written(makeDesc("v", eGeoStorageFloat64, 2), size);
    for (std::size_t e = 0; e < size; ++e) {
        written.setTuple<double>(e, std::vector<double> { 1.0, 2.0 });
    }
    EXPECT_FALSE(written.values().page(0)->isConstant());
    EXPECT_EQ(filled.contentHash(), written.contentHash());

    written.set<double>(17, 3.0, 1);
    EXPECT_NE(filled.contentHash(), written.contentHash());
}

TEST(GeoAttribute, RejectsBadDescriptionsAndAccess)
{
    EXPECT_THROW(GeoAttribute(makeDesc("x", eGeoStorageFloat32, 0)), std::invalid_argument);
    EXPECT_THROW(GeoAttribute(makeDesc("x", eGeoStorageFloat32, 17)), std::invalid_argument);

    GeoAttribute attr(makeDesc("x", eGeoStorageFloat32, 3), 4);
    EXPECT_THROW(attr.get<double>(0), std::logic_error);
    EXPECT_THROW(attr.get<float>(4), std::out_of_range);
    EXPECT_THROW(attr.get<float>(0, 3), std::out_of_range);
    EXPECT_THROW(attr.setTuple<float>(0, std::vector<float> { 1.f }), std::invalid_argument);
    std::vector<float> wrong(5);
    EXPECT_THROW(attr.copyDense<float>(0, 4, wrong), std::invalid_argument);
    EXPECT_THROW(attr.arrayLength(0), std::logic_error);
}

TEST(GeoAttribute, ParallelWritersOnDistinctPages)
{
    const std::size_t pages = 64;
    const GeoAttribute source = makeDenseFloat(pages * kGeoPageSize);
    GeoAttribute edited = source;
    edited.makePageTableUnique();

    const unsigned threadCount = 8;
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < threadCount; ++t) {
        threads.emplace_back([&edited, t, pages, threadCount]() {
            for (std::size_t p = t; p < pages; p += threadCount) {
                if (p % 2 == 0) {
                    edited.pageForWriting<float>(p)[0] = -static_cast<float>(p);
                }
            }
        });
    }
    // Further copies of the source taken and written while the writers run must not see them.
    std::vector<std::thread> copiers;
    for (unsigned t = 0; t < 4; ++t) {
        copiers.emplace_back([&source, t]() {
            GeoAttribute local = source;
            local.set<float>(t * kGeoPageSize, 1000.f);
            EXPECT_EQ(local.get<float>(t * kGeoPageSize), 1000.f);
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    for (std::thread& t : copiers) {
        t.join();
    }

    for (std::size_t p = 0; p < pages; ++p) {
        const std::size_t first = p * kGeoPageSize;
        EXPECT_EQ(source.get<float>(first), static_cast<float>(first));
        if (p % 2 == 0) {
            EXPECT_EQ(edited.get<float>(first), -static_cast<float>(p));
            EXPECT_NE(edited.values().page(p).get(), source.values().page(p).get());
        } else {
            EXPECT_EQ(edited.values().page(p).get(), source.values().page(p).get());
        }
    }
}
