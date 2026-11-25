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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Geo/GeoOpenVDB.h"

#include "Engine/Geo/GeoPacked.h"
#include "Engine/Geo/GeoSpreadsheet.h"
#include "Engine/Geo/GeoVDB.h"

NATRON_NAMESPACE_USING

namespace {

std::vector<std::string>
columnNames(const GeoSpreadsheetSource& s,
            GeoAttribClassEnum cls)
{
    std::vector<std::string> names;
    for (const GeoSpreadsheetColumn& c : s.columns(cls)) {
        names.push_back(c.name);
    }

    return names;
}

int
columnIndex(const GeoSpreadsheetSource& s,
            GeoAttribClassEnum cls,
            const std::string& name)
{
    const std::vector<GeoSpreadsheetColumn>& cols = s.columns(cls);
    for (std::size_t i = 0; i < cols.size(); ++i) {
        if (cols[i].name == name) {
            return int(i);
        }
    }

    return -1;
}

std::string
cell(const GeoSpreadsheetSource& s,
     GeoAttribClassEnum cls,
     std::int64_t row,
     const std::string& column)
{
    const int c = columnIndex(s, cls, column);
    EXPECT_GE(c, 0) << column;
    std::vector<std::string> out;
    const int cols[] = { c };
    s.readCells(cls, row, 1, cols, out);

    return out.empty() ? std::string() : out[0];
}

GeoMatrix4
translation(double x,
            double y,
            double z)
{
    GeoMatrix4 m = geoMatrixIdentity();
    m[3] = x;
    m[7] = y;
    m[11] = z;

    return m;
}

// Six points in a row and two triangles over them.
GeoDetail
makeGrid(bool precision64 = false)
{
    GeoDetailBuilder b;
    b.setPointPrecision64(precision64);
    const GeoOffset first = b.appendPoints(6);
    for (int i = 0; i < 6; ++i) {
        b.setPointPosition(first + i, double(i) + 0.5, double(i) * 2., 0.);
    }
    GeoAttributeDesc cd;
    cd.name = "Cd";
    cd.tupleSize = 3;
    cd.typeInfo = eGeoTypeInfoColour;
    GeoAttribute& cdAttr = b.addAttribute(eGeoAttribClassPoint, cd);
    for (int i = 0; i < 6; ++i) {
        const float rgb[3] = { 0.25f * float(i), 0.5f, 1.f };
        cdAttr.setTuple<float>(std::size_t(first + i), rgb);
    }
    GeoAttributeDesc name;
    name.name = "name";
    name.storage = eGeoStorageString;
    GeoAttribute& nameAttr = b.addAttribute(eGeoAttribClassPoint, name);
    for (int i = 0; i < 6; ++i) {
        nameAttr.setString(std::size_t(first + i), "pt" + std::to_string(i));
    }
    GeoAttributeDesc ids;
    ids.name = "ids";
    ids.storage = eGeoStorageInt32;
    ids.isArray = true;
    GeoAttribute& idsAttr = b.addAttribute(eGeoAttribClassPoint, ids);
    for (int i = 0; i < 6; ++i) {
        std::vector<std::int32_t> v;
        v.resize(std::size_t(i));
        for (int k = 0; k < i; ++k) {
            v[std::size_t(k)] = 10 * i + k;
        }
        idsAttr.setArray<std::int32_t>(std::size_t(first + i), v);
    }
    const GeoOffset tri0[] = { first, first + 1, first + 2 };
    const GeoOffset tri1[] = { first + 3, first + 4, first + 5 };
    b.appendPrimitive(eGeoPrimitivePolygon, tri0);
    b.appendPrimitive(eGeoPrimitivePolygon, tri1);

    return b.finish();
}

} // namespace

TEST(GeoSpreadsheetTest, ColumnNamesAndCellText)
{
    const GeoDetailSpreadsheetSource src(std::make_shared<const GeoDetail>(makeGrid()));
    EXPECT_EQ(src.rowCount(eGeoAttribClassPoint), 6);
    EXPECT_EQ(src.rowCount(eGeoAttribClassVertex), 6);
    EXPECT_EQ(src.rowCount(eGeoAttribClassPrimitive), 2);
    EXPECT_EQ(src.rowCount(eGeoAttribClassDetail), 1);
    EXPECT_EQ(columnNames(src, eGeoAttribClassPoint),
              (std::vector<std::string> { "P[x]", "P[y]", "P[z]", "Cd[r]", "Cd[g]", "Cd[b]", "name", "ids" }));

    EXPECT_EQ(cell(src, eGeoAttribClassPoint, 3, "P[x]"), "3.5");
    EXPECT_EQ(cell(src, eGeoAttribClassPoint, 3, "P[y]"), "6");
    EXPECT_EQ(cell(src, eGeoAttribClassPoint, 2, "Cd[r]"), "0.5");
    EXPECT_EQ(cell(src, eGeoAttribClassPoint, 4, "name"), "pt4");
    EXPECT_EQ(cell(src, eGeoAttribClassPoint, 2, "ids"), "(2) [20, 21]");
    EXPECT_EQ(cell(src, eGeoAttribClassPoint, 5, "ids"), "(5) [50, 51, 52, \xE2\x80\xA6]");
    EXPECT_EQ(src.fullValue(eGeoAttribClassPoint, columnIndex(src, eGeoAttribClassPoint, "ids"), 5), "(5) [50, 51, 52, 53, 54]");
    EXPECT_TRUE(src.columns(eGeoAttribClassPoint)[std::size_t(columnIndex(src, eGeoAttribClassPoint, "ids"))].hasFullValue);
}

TEST(GeoSpreadsheetTest, BlockReadIsRowMajor)
{
    const GeoDetailSpreadsheetSource src(std::make_shared<const GeoDetail>(makeGrid()));
    const int cols[] = { columnIndex(src, eGeoAttribClassPoint, "name"), columnIndex(src, eGeoAttribClassPoint, "P[x]") };
    std::vector<std::string> out;
    src.readCells(eGeoAttribClassPoint, 4, 10, cols, out);
    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(out[0], "pt4");
    EXPECT_EQ(out[1], "4.5");
    EXPECT_EQ(out[2], "pt5");
    EXPECT_EQ(out[3], "5.5");
}

TEST(GeoSpreadsheetTest, SortKeys)
{
    const GeoDetailSpreadsheetSource src(std::make_shared<const GeoDetail>(makeGrid()));
    const GeoAttribClassEnum pt = eGeoAttribClassPoint;
    EXPECT_EQ(std::get<double>(src.sortKey(pt, columnIndex(src, pt, "P[y]"), 3)), 6.);
    EXPECT_EQ(std::get<std::string>(src.sortKey(pt, columnIndex(src, pt, "name"), 3)), "pt3");
    EXPECT_EQ(std::get<GeoSpreadsheetArrayLength>(src.sortKey(pt, columnIndex(src, pt, "ids"), 4)).length, 4);
}

TEST(GeoSpreadsheetTest, RowsAreDenseAfterDelete)
{
    GeoDetailBuilder b(makeGrid());
    const GeoOffset dead[] = { 2, 3, 4 };
    // Both triangles use a doomed point, so keeping referenced points would delete nothing.
    b.deletePoints(dead, eGeoPointDeleteDegenerate);
    const GeoDetail d = b.finish();
    ASSERT_GT(d.pointMap().tombstoneCount(), 0);
    const GeoDetailSpreadsheetSource src(std::make_shared<const GeoDetail>(d));
    EXPECT_EQ(src.rowCount(eGeoAttribClassPoint), 6 - GeoIndex(d.pointMap().tombstoneCount()));
    EXPECT_EQ(cell(src, eGeoAttribClassPoint, 2, "name"), "pt5");
    EXPECT_EQ(cell(src, eGeoAttribClassPoint, 1, "name"), "pt1");

    const int cols[] = { columnIndex(src, eGeoAttribClassPoint, "name") };
    std::vector<std::string> out;
    src.readCells(eGeoAttribClassPoint, 0, 100, cols, out);
    EXPECT_EQ(out, (std::vector<std::string> { "pt0", "pt1", "pt5" }));
}

TEST(GeoSpreadsheetTest, Float64PositionKeepsFullPrecision)
{
    GeoDetailBuilder b;
    b.setPointPrecision64(true);
    const GeoOffset p = b.appendPoints(1);
    b.setPointPosition(p, 0.1 + 0.2, 1. / 3., 1e-300);
    const GeoDetailSpreadsheetSource src(std::make_shared<const GeoDetail>(b.finish()));
    EXPECT_EQ(std::stod(cell(src, eGeoAttribClassPoint, 0, "P[x]")), 0.1 + 0.2);
    EXPECT_EQ(std::stod(cell(src, eGeoAttribClassPoint, 0, "P[y]")), 1. / 3.);
    EXPECT_EQ(std::stod(cell(src, eGeoAttribClassPoint, 0, "P[z]")), 1e-300);
}

TEST(GeoSpreadsheetTest, VertexPointColumnFollowsTopology)
{
    GeoDetailBuilder b(makeGrid());
    const GeoOffset dead[] = { 0 };
    b.deletePoints(dead, eGeoPointDeleteKeepReferenced);
    const GeoOffset oneTri[] = { 5, 4, 1 };
    b.appendPrimitive(eGeoPrimitivePolygon, oneTri);
    const GeoDetail d = b.finish();
    const GeoDetailSpreadsheetSource src(std::make_shared<const GeoDetail>(d));
    EXPECT_EQ(columnNames(src, eGeoAttribClassVertex).front(), "Point");
    ASSERT_EQ(src.rowCount(eGeoAttribClassVertex), d.vertexCount());
    std::int64_t row = 0;
    d.vertexMap().forEachLive([&](GeoIndex, GeoOffset vertex) {
        const GeoIndex point = d.pointMap().indexFromOffset(d.vertexPoint(vertex));
        EXPECT_EQ(cell(src, eGeoAttribClassVertex, row, "Point"), std::to_string(point));
        ++row;
    });
}

TEST(GeoSpreadsheetTest, PackedPrimitivesShowIntrinsics)
{
    GeoDetailBuilder inner;
    const GeoOffset ip = inner.appendPoints(2);
    inner.setPointPosition(ip, 0., 0., 0.);
    inner.setPointPosition(ip + 1, 1., 2., 3.);
    const GeoDetailConstPtr contained = std::make_shared<const GeoDetail>(inner.finish());

    GeoDetailBuilder b;
    const GeoOffset p = b.appendPoints(2);
    geoAppendPackedGeometry(b, p, contained, translation(1., 2., 3.));
    geoAppendPackedUSD(b, p + 1, "/World/Chair", translation(5., 0., 0.), translation(4., 5., 6.), GeoBounds { { -1., -2., -3. }, { 1., 2., 3. } });
    const GeoDetailSpreadsheetSource src(std::make_shared<const GeoDetail>(b.finish()));
    const GeoAttribClassEnum prim = eGeoAttribClassPrimitive;

    EXPECT_EQ(columnNames(src, prim),
              (std::vector<std::string> { "intrinsic:kind", "intrinsic:transform", "intrinsic:bounds", "intrinsic:sourcePath" }));
    EXPECT_EQ(cell(src, prim, 0, "intrinsic:kind"), "packedGeometry");
    EXPECT_EQ(cell(src, prim, 1, "intrinsic:kind"), "packedUSD");
    EXPECT_EQ(cell(src, prim, 0, "intrinsic:transform"), "T(1, 2, 3)");
    EXPECT_EQ(cell(src, prim, 1, "intrinsic:transform"), "T(4, 5, 6)");
    EXPECT_EQ(cell(src, prim, 0, "intrinsic:bounds"), "(0, 0, 0) - (1, 2, 3)");
    EXPECT_EQ(cell(src, prim, 1, "intrinsic:bounds"), "(-1, -2, -3) - (1, 2, 3)");
    EXPECT_EQ(cell(src, prim, 0, "intrinsic:sourcePath"), "");
    EXPECT_EQ(cell(src, prim, 1, "intrinsic:sourcePath"), "/World/Chair");
    EXPECT_EQ(src.fullValue(prim, columnIndex(src, prim, "intrinsic:transform"), 0), "1 0 0 1\n0 1 0 2\n0 0 1 3\n0 0 0 1");
}

TEST(GeoSpreadsheetTest, DetailClassHasOneRow)
{
    GeoDetailBuilder b(makeGrid());
    GeoAttributeDesc d;
    d.name = "label";
    d.storage = eGeoStorageString;
    b.addAttribute(eGeoAttribClassDetail, d).setString(0, "hello");
    const GeoDetailSpreadsheetSource src(std::make_shared<const GeoDetail>(b.finish()));
    EXPECT_EQ(src.rowCount(eGeoAttribClassDetail), 1);
    EXPECT_EQ(cell(src, eGeoAttribClassDetail, 0, "label"), "hello");
}

TEST(GeoSpreadsheetTest, BlockNearEndOfHugeDetailReadsOnlyItsPages)
{
    constexpr GeoIndex kPoints = 10000000;
    GeoDetailBuilder b;
    b.appendPoints(kPoints);
    for (GeoOffset i = kPoints - 60; i < kPoints; ++i) {
        b.setPointPosition(i, double(i), 0., 0.);
    }
    const GeoDetailSpreadsheetSource src(std::make_shared<const GeoDetail>(b.finish()));
    ASSERT_EQ(src.rowCount(eGeoAttribClassPoint), kPoints);

    const int cols[] = { 0, 1, 2 };
    std::vector<std::string> out;
    geoSpreadsheetResetPagesRead();
    src.readCells(eGeoAttribClassPoint, kPoints - 50, 50, cols, out);
    const std::uint64_t pages = geoSpreadsheetPagesRead();
    ASSERT_EQ(out.size(), 150u);
    EXPECT_EQ(out[0], std::to_string(kPoints - 50));
    EXPECT_GT(pages, 0u);
    EXPECT_LE(pages, 3u * 2u);
}

namespace {

class FakeSource : public GeoSpreadsheetSource {
public:
    typedef std::function<GeoSpreadsheetSortKey(std::int64_t)> KeyFn;

    FakeSource(std::int64_t rows,
               KeyFn key)
        : _rows(rows)
        , _key(std::move(key))
        , _columns(1)
    {
    }

    std::int64_t rowCount(GeoAttribClassEnum) const override
    {
        return _rows;
    }

    const std::vector<GeoSpreadsheetColumn>& columns(GeoAttribClassEnum) const override
    {
        return _columns;
    }

    void readCells(GeoAttribClassEnum,
                   std::int64_t,
                   std::int64_t,
                   std::span<const int>,
                   std::vector<std::string>& out) const override
    {
        out.clear();
    }

    GeoSpreadsheetSortKey sortKey(GeoAttribClassEnum,
                                  int,
                                  std::int64_t row) const override
    {
        return _key(row);
    }

    std::string fullValue(GeoAttribClassEnum,
                          int,
                          std::int64_t) const override
    {
        return std::string();
    }

private:
    std::int64_t _rows;
    KeyFn _key;
    std::vector<GeoSpreadsheetColumn> _columns;
};

std::vector<GeoIndex>
buildOrder(const GeoSpreadsheetSource& src,
           const GeoElementMask* mask,
           int column,
           bool descending)
{
    const std::atomic<bool> cancelled(false);
    const auto order = geoSpreadsheetBuildOrder(src, eGeoAttribClassPoint, mask, column, descending, cancelled);
    EXPECT_NE(order, nullptr);

    return order ? *order : std::vector<GeoIndex>();
}

std::vector<GeoIndex>
sorted(const std::vector<GeoIndex>& elements,
       const std::function<int(GeoIndex, GeoIndex)>& cmp)
{
    std::vector<GeoIndex> out = elements;
    std::stable_sort(out.begin(), out.end(), [&](GeoIndex a, GeoIndex b) { return cmp(a, b) < 0; });

    return out;
}

std::vector<GeoIndex>
elementRange(std::int64_t n)
{
    std::vector<GeoIndex> v;
    v.resize(std::size_t(n));
    for (std::int64_t i = 0; i < n; ++i) {
        v[std::size_t(i)] = i;
    }

    return v;
}

// More rows than one sort chunk, so the merge passes run, with many ties.
constexpr std::int64_t kOrderRows = 200000;

} // namespace

TEST(GeoSpreadsheetOrderTest, FloatColumnAscendingAndDescendingKeepTiesInElementOrder)
{
    auto key = [](std::int64_t r) { return double((r * 7919) % 1000) * 0.5; };
    const FakeSource src(kOrderRows, [&](std::int64_t r) { return GeoSpreadsheetSortKey(key(r)); });
    const std::vector<GeoIndex> asc = buildOrder(src, nullptr, 0, false);
    const std::vector<GeoIndex> desc = buildOrder(src, nullptr, 0, true);
    EXPECT_EQ(asc, sorted(elementRange(kOrderRows), [&](GeoIndex a, GeoIndex b) { return key(a) < key(b) ? -1 : (key(a) > key(b) ? 1 : 0); }));
    EXPECT_EQ(desc, sorted(elementRange(kOrderRows), [&](GeoIndex a, GeoIndex b) { return key(a) > key(b) ? -1 : (key(a) < key(b) ? 1 : 0); }));
}

TEST(GeoSpreadsheetOrderTest, IntegerColumnIsOrderedByValue)
{
    const FakeSource src(kOrderRows, [](std::int64_t r) { return GeoSpreadsheetSortKey(double(-(r % 97))); });
    const std::vector<GeoIndex> asc = buildOrder(src, nullptr, 0, false);
    ASSERT_EQ(asc.size(), std::size_t(kOrderRows));
    for (std::size_t i = 1; i < asc.size(); ++i) {
        const double a = -double(asc[i - 1] % 97);
        const double b = -double(asc[i] % 97);
        ASSERT_TRUE(a < b || (a == b && asc[i - 1] < asc[i])) << i;
    }
}

TEST(GeoSpreadsheetOrderTest, StringColumnAscendingAndDescending)
{
    auto key = [](std::int64_t r) { return std::string("s") + std::to_string((r * 31) % 500); };
    const FakeSource src(kOrderRows, [&](std::int64_t r) { return GeoSpreadsheetSortKey(key(r)); });
    auto cmp = [&](GeoIndex a, GeoIndex b) { return key(a).compare(key(b)); };
    EXPECT_EQ(buildOrder(src, nullptr, 0, false), sorted(elementRange(kOrderRows), cmp));
    EXPECT_EQ(buildOrder(src, nullptr, 0, true), sorted(elementRange(kOrderRows), [&](GeoIndex a, GeoIndex b) { return cmp(b, a); }));
}

TEST(GeoSpreadsheetOrderTest, ArrayLengthColumnOrdersByLength)
{
    auto len = [](std::int64_t r) { return (r * 13) % 50; };
    const FakeSource src(1000, [&](std::int64_t r) { return GeoSpreadsheetSortKey(GeoSpreadsheetArrayLength { len(r) }); });
    auto cmp = [&](GeoIndex a, GeoIndex b) { return len(a) < len(b) ? -1 : (len(a) > len(b) ? 1 : 0); };
    EXPECT_EQ(buildOrder(src, nullptr, 0, false), sorted(elementRange(1000), cmp));
    EXPECT_EQ(buildOrder(src, nullptr, 0, true), sorted(elementRange(1000), [&](GeoIndex a, GeoIndex b) { return cmp(b, a); }));
}

TEST(GeoSpreadsheetOrderTest, MixedKindsOrderByKindThenValue)
{
    auto key = [](std::int64_t r) {
        switch (r % 3) {
        case 0:
            return GeoSpreadsheetSortKey(std::string(1, char('a' + r % 7)));
        case 1:
            return GeoSpreadsheetSortKey(double(r % 5));
        default:
            return GeoSpreadsheetSortKey(GeoSpreadsheetArrayLength { r % 4 });
        }
    };
    const FakeSource src(300, key);
    const std::vector<GeoIndex> asc = buildOrder(src, nullptr, 0, false);
    ASSERT_EQ(asc.size(), 300u);
    for (std::size_t i = 1; i < asc.size(); ++i) {
        const GeoSpreadsheetSortKey a = key(asc[i - 1]);
        const GeoSpreadsheetSortKey b = key(asc[i]);
        ASSERT_LE(a.index(), b.index()) << i;
        if (a == b) {
            ASSERT_LT(asc[i - 1], asc[i]) << i;
        }
    }
}

TEST(GeoSpreadsheetOrderTest, MaskKeepsOnlyItsElementsAndNoSortKeepsElementOrder)
{
    const FakeSource src(1000, [](std::int64_t r) { return GeoSpreadsheetSortKey(double(1000 - r)); });
    GeoElementMask mask(1000, 0);
    std::vector<GeoIndex> kept;
    for (std::size_t i = 0; i < mask.size(); i += 7) {
        mask[i] = 1;
        kept.push_back(GeoIndex(i));
    }
    EXPECT_EQ(buildOrder(src, &mask, -1, false), kept);
    std::vector<GeoIndex> reversed(kept.rbegin(), kept.rend());
    EXPECT_EQ(buildOrder(src, &mask, 0, false), reversed);
    EXPECT_EQ(buildOrder(src, nullptr, -1, false), elementRange(1000));
}

TEST(GeoSpreadsheetOrderTest, CancellingMidSortReturnsPromptly)
{
    constexpr std::int64_t kRows = 10000000;
    const FakeSource src(kRows, [](std::int64_t r) { return GeoSpreadsheetSortKey(double((r * 2654435761u) % 1000003)); });
    std::atomic<bool> cancelled(false);
    std::atomic<std::int64_t> setAt(0);
    std::thread canceller([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        setAt = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        cancelled = true;
    });
    const auto order = geoSpreadsheetBuildOrder(src, eGeoAttribClassPoint, nullptr, 0, false, cancelled);
    const std::int64_t doneAt = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    canceller.join();
    EXPECT_FALSE(order);
    EXPECT_LT(doneAt - setAt.load(), 100000);
}

TEST(GeoSpreadsheetOrderTest, NaNSortsLastAscendingAndDescending)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double values[] = { 2., nan, 1., 3., nan };
    const FakeSource numbers(5, [&](std::int64_t r) { return GeoSpreadsheetSortKey(values[r]); });
    EXPECT_EQ(buildOrder(numbers, nullptr, 0, false), (std::vector<GeoIndex> { 2, 0, 3, 1, 4 }));
    EXPECT_EQ(buildOrder(numbers, nullptr, 0, true), (std::vector<GeoIndex> { 3, 0, 2, 1, 4 }));

    // A string in the column takes the mixed-kind path, which keeps NaN last among the numbers too.
    const FakeSource mixed(6, [&](std::int64_t r) { return r == 5 ? GeoSpreadsheetSortKey(std::string("a")) : GeoSpreadsheetSortKey(values[r]); });
    EXPECT_EQ(buildOrder(mixed, nullptr, 0, false), (std::vector<GeoIndex> { 2, 0, 3, 1, 4, 5 }));
    EXPECT_EQ(buildOrder(mixed, nullptr, 0, true), (std::vector<GeoIndex> { 5, 3, 0, 2, 1, 4 }));
}

TEST(GeoSpreadsheetTest, RowMaskAndRowCellsFollowTheLiveRows)
{
    GeoDetailBuilder b(makeGrid());
    const GeoOffset dead[] = { 2, 3, 4 };
    b.deletePoints(dead, eGeoPointDeleteDegenerate);
    const GeoDetail d = b.finish();
    ASSERT_EQ(3, d.pointMap().tombstoneCount());

    const GeoElementMask offsets = { 0, 1, 1, 1, 1, 1 };
    EXPECT_EQ(geoSpreadsheetRowMask(d.pointMap(), offsets), (GeoElementMask { 0, 1, 1 }));
    EXPECT_EQ(geoSpreadsheetRowMask(d.pointMap(), GeoElementMask { 1 }), (GeoElementMask { 1, 0, 0 }));

    const GeoDetailSpreadsheetSource src(std::make_shared<const GeoDetail>(d));
    const int cols[] = { columnIndex(src, eGeoAttribClassPoint, "name") };
    const GeoIndex rows[] = { 2, 0, 7, 1 };
    std::vector<std::string> out;
    src.readRowCells(eGeoAttribClassPoint, rows, cols, out);
    EXPECT_EQ(out, (std::vector<std::string> { "pt5", "pt0", "", "pt1" }));
}

TEST(GeoSpreadsheetTest, PackedPrimitivesKeepUserAttributesNamedLikePacked)
{
    GeoDetailBuilder b;
    const GeoOffset p = b.appendPoints(1);
    geoAppendPackedUSD(b, p, "/World/Chair", translation(0., 0., 0.), translation(0., 0., 0.), GeoBounds { { 0., 0., 0. }, { 1., 1., 1. } });
    GeoAttributeDesc desc;
    desc.name = "packedness";
    b.addAttribute(eGeoAttribClassPrimitive, desc);
    const GeoDetailSpreadsheetSource src(std::make_shared<const GeoDetail>(b.finish()));

    const std::vector<std::string> names = columnNames(src, eGeoAttribClassPrimitive);
    EXPECT_NE(std::find(names.begin(), names.end(), "packedness"), names.end());
    for (const char* internal : { kGeoPackedXformAttr, kGeoPackedBoundsAttr, kGeoPackedUSDPathAttr, kGeoPackedUSDParentXformAttr }) {
        EXPECT_EQ(std::find(names.begin(), names.end(), internal), names.end()) << internal;
    }
}

TEST(GeoSpreadsheetTest, VDBPrimitivesNameTheirKind)
{
    geoInitializeOpenVDB();
    GeoDetailBuilder b;
    const GeoOffset p = b.appendPoints(1);
    geoAppendPackedUSD(b, p, "/World/Chair", translation(0., 0., 0.), translation(0., 0., 0.), GeoBounds { { -1., -1., -1. }, { 1., 1., 1. } });
    geoAppendVDB(b, GeoVDBGridBaseConstPtr(openvdb::FloatGrid::create(0.f)), "density");
    const GeoDetailSpreadsheetSource src(std::make_shared<const GeoDetail>(b.finish()));

    EXPECT_EQ(cell(src, eGeoAttribClassPrimitive, 1, "intrinsic:kind"), "vdb");
}
