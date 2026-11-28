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
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "Engine/Geo/GeoFileFormat.h"
#include "Engine/Geo/GeoPacked.h"

NATRON_NAMESPACE_USING

namespace {

constexpr GeoIndex kPoints = 2500;

GeoAttributeDesc
desc(const char* name,
     GeoStorageEnum storage,
     int tupleSize = 1,
     GeoTypeInfoEnum typeInfo = eGeoTypeInfoNone,
     bool isArray = false)
{
    return GeoAttributeDesc { name, storage, tupleSize, typeInfo, isArray };
}

GeoDetail
roundTrip(const GeoDetail& d,
          const GeoFileReadOptions& options = GeoFileReadOptions())
{
    const std::vector<unsigned char> bytes = geoFileEncode(d);

    return geoFileDecode(bytes, "memory", options);
}

GeoDetail
makeRich(bool precision64)
{
    GeoDetailBuilder b;
    b.setPointPrecision64(precision64);

    const std::int32_t seven = 7;
    b.addAttribute(eGeoAttribClassPoint, desc("withDefault", eGeoStorageInt32)).setDefault<std::int32_t>(std::span<const std::int32_t>(&seven, 1));

    const GeoOffset first = b.appendPoints(kPoints);
    for (GeoIndex i = 0; i < kPoints; ++i) {
        b.setPointPosition(first + i, double(i) * 0.25, double(i % 17) + 1.0 / 3.0, -double(i));
    }

    std::vector<std::int32_t> counts;
    std::vector<GeoOffset> points;
    for (GeoIndex i = 0; i + 2 < kPoints; i += 3) {
        counts.push_back(3);
        points.insert(points.end(), { first + i, first + i + 1, first + i + 2 });
    }
    b.appendPrimitives(eGeoPrimitivePolygon, counts, points);
    const std::array<GeoOffset, 4> line { first, first + 5, first + 9, first + 2 };
    b.appendPrimitive(eGeoPrimitivePolyline, line);

    GeoAttribute& i32 = b.addAttribute(eGeoAttribClassPoint, desc("id", eGeoStorageInt32));
    GeoAttribute& i64 = b.addAttribute(eGeoAttribClassPoint, desc("big", eGeoStorageInt64, 2));
    GeoAttribute& cd = b.addAttribute(eGeoAttribClassPoint, desc("Cd", eGeoStorageFloat32, 3, eGeoTypeInfoColour));
    GeoAttribute& f64 = b.addAttribute(eGeoAttribClassPoint, desc("orient", eGeoStorageFloat64, 4, eGeoTypeInfoQuaternion));
    GeoAttribute& name = b.addAttribute(eGeoAttribClassPoint, desc("name", eGeoStorageString));
    GeoAttribute& tags = b.addAttribute(eGeoAttribClassPoint, desc("tags", eGeoStorageString, 2));
    GeoAttribute& flat = b.addAttribute(eGeoAttribClassPoint, desc("flat", eGeoStorageFloat32));
    GeoAttribute& farr = b.addAttribute(eGeoAttribClassPoint, desc("offsets", eGeoStorageFloat32, 3, eGeoTypeInfoVector, true));
    GeoAttribute& iarr = b.addAttribute(eGeoAttribClassPoint, desc("links", eGeoStorageInt64, 1, eGeoTypeInfoNone, true));
    GeoAttribute& sarr = b.addAttribute(eGeoAttribClassPoint, desc("labels", eGeoStorageString, 1, eGeoTypeInfoNone, true));
    const float half = 0.5f;
    flat.fill<float>(0, flat.size(), std::span<const float>(&half, 1));
    for (GeoIndex i = 0; i < kPoints; ++i) {
        const std::size_t o = std::size_t(first + i);
        i32.set<std::int32_t>(o, std::int32_t(i * 3 - 100));
        const std::array<std::int64_t, 2> big { std::int64_t(i) << 40, -std::int64_t(i) };
        i64.setTuple<std::int64_t>(o, big);
        const std::array<float, 3> c { float(i) / kPoints, 0.25f, 1.f - float(i) / kPoints };
        cd.setTuple<float>(o, c);
        const std::array<double, 4> q { 1e-300 * double(i), 0.1, 0.2, 0.3 };
        f64.setTuple<double>(o, q);
        name.setString(o, "piece" + std::to_string(i % 5));
        tags.setString(o, i % 2 ? "odd" : "even", 0);
        tags.setString(o, "t" + std::to_string(i % 3), 1);
        if (i % 4 != 0) {
            std::vector<float> v(std::size_t(3 * (i % 4)), float(i));
            farr.setArray<float>(o, v);
            const std::vector<std::int64_t> l { i, i + 1 };
            iarr.setArray<std::int64_t>(o, l);
            const std::vector<std::string> s { "a", "b" + std::to_string(i % 7) };
            sarr.setStringArray(o, s);
        }
    }

    GeoAttribute& uv = b.addAttribute(eGeoAttribClassVertex, desc("uv", eGeoStorageFloat32, 2));
    for (GeoIndex v = 0; v < b.detail().vertexCount(); ++v) {
        const std::array<float, 2> t { float(v), float(-v) };
        uv.setTuple<float>(std::size_t(v), t);
    }
    GeoAttribute& primId = b.addAttribute(eGeoAttribClassPrimitive, desc("primId", eGeoStorageInt32));
    for (GeoIndex p = 0; p < b.detail().primitiveCount(); ++p) {
        primId.set<std::int32_t>(std::size_t(p), std::int32_t(p * p));
    }
    GeoAttribute& xform = b.addAttribute(eGeoAttribClassDetail, desc("xform", eGeoStorageFloat64, 16, eGeoTypeInfoMatrix));
    std::array<double, 16> m {};
    std::iota(m.begin(), m.end(), 1.0);
    xform.setTuple<double>(0, m);
    b.addAttribute(eGeoAttribClassDetail, desc("author", eGeoStorageString)).setString(0, "cache");

    GeoElementGroup& even = b.addGroup(eGeoAttribClassPoint, "even");
    for (GeoIndex i = 0; i < kPoints; i += 2) {
        even.add(first + i);
    }
    b.addGroup(eGeoAttribClassPoint, "block").addRange(100, 2100);
    b.addGroup(eGeoAttribClassPoint, "none");
    b.addGroup(eGeoAttribClassVertex, "corner").add(GeoOffset(2));
    b.addGroup(eGeoAttribClassPrimitive, "thirds").addRange(0, b.detail().primitiveCount() / 3);
    GeoEdgeGroup& seam = b.addEdgeGroup("seam");
    seam.add(3, 1);
    seam.add(10, 11);

    return b.finish();
}

GeoDetailConstPtr
makeContained(double top)
{
    GeoDetailBuilder b;
    const GeoOffset first = b.appendPoints(3);
    b.setPointPosition(first, 0., 0., 0.);
    b.setPointPosition(first + 1, 1., top, 3.);
    b.setPointPosition(first + 2, 0., 1., 0.);
    const std::array<GeoOffset, 3> tri { first, first + 1, first + 2 };
    b.appendPrimitive(eGeoPrimitivePolygon, tri);

    return std::make_shared<const GeoDetail>(b.finish());
}

GeoMatrix4
translation(double x)
{
    GeoMatrix4 m = geoMatrixIdentity();
    m[3] = x;

    return m;
}

std::string
tempPath(const char* stem)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path();

    return (dir / (std::string(stem) + "-" + std::to_string(::getpid()) + "." + kGeoFileExtension)).string();
}

void
expectFileError(std::span<const unsigned char> bytes,
                const char* expected)
{
    try {
        GeoFileReadOptions options;
        options.verifyContentHash = true;
        (void)geoFileDecode(bytes, "broken.ngeo", options);
        ADD_FAILURE() << "decoding did not fail";
    } catch (const GeoFileError& e) {
        const std::string what = e.what();
        EXPECT_NE(std::string::npos, what.find("broken.ngeo")) << what;
        if (expected) {
            EXPECT_NE(std::string::npos, what.find(expected)) << what;
        }
    }
}

} // namespace

TEST(GeoFileFormat, EmptyDetailRoundTrips)
{
    const GeoDetail empty;
    const GeoDetail back = roundTrip(empty);
    EXPECT_EQ(0, back.pointCount());
    EXPECT_EQ(0, back.primitiveCount());
    EXPECT_EQ(empty.contentHash(), back.contentHash());
}

TEST(GeoFileFormat, EveryAttributeKindRoundTrips)
{
    const GeoDetail src = makeRich(false);
    GeoFileReadOptions options;
    options.verifyContentHash = true;
    const GeoDetail back = roundTrip(src, options);

    EXPECT_EQ(src.contentHash(), back.contentHash());
    EXPECT_EQ(src.pointCount(), back.pointCount());
    EXPECT_EQ(src.vertexCount(), back.vertexCount());
    EXPECT_EQ(src.primitiveCount(), back.primitiveCount());
    EXPECT_EQ(eGeoPrimitivePolyline, back.primitiveType(back.primitiveCount() - 1));
    ASSERT_EQ(src.attributeCount(eGeoAttribClassPoint), back.attributeCount(eGeoAttribClassPoint));
    for (std::size_t i = 0; i < src.attributeCount(eGeoAttribClassPoint); ++i) {
        EXPECT_EQ(src.attribute(eGeoAttribClassPoint, i).name(), back.attribute(eGeoAttribClassPoint, i).name());
    }

    const GeoAttribute* name = back.findAttribute(eGeoAttribClassPoint, "name");
    ASSERT_NE(nullptr, name);
    EXPECT_EQ("piece3", name->getString(8));
    const GeoAttribute* labels = back.findAttribute(eGeoAttribClassPoint, "labels");
    ASSERT_NE(nullptr, labels);
    std::vector<std::string> s;
    labels->getStringArray(13, s);
    ASSERT_EQ(2u, s.size());
    EXPECT_EQ("b6", s[1]);
    const GeoAttribute* withDefault = back.findAttribute(eGeoAttribClassPoint, "withDefault");
    ASSERT_NE(nullptr, withDefault);
    std::int32_t def = 0;
    withDefault->getDefault<std::int32_t>(std::span<std::int32_t>(&def, 1));
    EXPECT_EQ(7, def);
    const GeoAttribute* author = back.findAttribute(eGeoAttribClassDetail, "author");
    ASSERT_NE(nullptr, author);
    EXPECT_EQ("cache", author->getString(0));

    const GeoElementGroup* block = back.findGroup(eGeoAttribClassPoint, "block");
    ASSERT_NE(nullptr, block);
    EXPECT_EQ(2000, block->countLive(back.pointMap()));
    EXPECT_NE(nullptr, back.findGroup(eGeoAttribClassPoint, "none"));
    EXPECT_NE(nullptr, back.findGroup(eGeoAttribClassVertex, "corner"));
    EXPECT_NE(nullptr, back.findGroup(eGeoAttribClassPrimitive, "thirds"));
    const GeoEdgeGroup* seam = back.findEdgeGroup("seam");
    ASSERT_NE(nullptr, seam);
    EXPECT_TRUE(seam->contains(1, 3));
    EXPECT_EQ(2u, seam->size());
}

TEST(GeoFileFormat, Precision64RoundTrips)
{
    const GeoDetail src = makeRich(true);
    const GeoDetail back = roundTrip(src);
    EXPECT_TRUE(back.pointPrecision64());
    EXPECT_EQ(src.contentHash(), back.contentHash());
    EXPECT_EQ(src.pointPosition(1234), back.pointPosition(1234));
}

TEST(GeoFileFormat, PackedPrimitivesRoundTrip)
{
    const GeoDetailConstPtr inner = makeContained(2.);
    GeoDetailConstPtr nested;
    {
        GeoDetailBuilder b;
        const GeoOffset pivot = b.appendPoints(1);
        geoAppendPackedGeometry(b, pivot, inner);
        nested = std::make_shared<const GeoDetail>(b.finish());
    }
    const GeoDetailConstPtr twin = makeContained(2.);

    GeoDetailBuilder b;
    const GeoOffset first = b.appendPoints(200);
    std::vector<GeoOffset> pivots(100);
    std::iota(pivots.begin(), pivots.end(), first);
    std::vector<GeoMatrix4> xforms;
    for (std::size_t i = 0; i < pivots.size(); ++i) {
        xforms.push_back(translation(double(i)));
    }
    geoAppendPackedGeometries(b, pivots, inner, xforms);
    geoAppendPackedGeometry(b, first + 100, twin, translation(-1.));
    geoAppendPackedGeometry(b, first + 101, nested);
    const GeoBounds bounds { { -1., -1., -1. }, { 1., 1., 1. } };
    geoAppendPackedUSD(b, first + 102, "/World/geo/rock", geoMatrixIdentity(), translation(4.), bounds);
    const GeoDetail src = b.finish();
    ASSERT_EQ(3u, src.sharedDetails().size());

    GeoFileReadOptions options;
    options.verifyContentHash = true;
    const GeoDetail back = roundTrip(src, options);
    EXPECT_EQ(src.contentHash(), back.contentHash());
    ASSERT_EQ(3u, back.sharedDetails().size());
    EXPECT_NE(back.sharedDetails()[0].get(), back.sharedDetails()[1].get());
    EXPECT_EQ(inner->contentHash(), geoPackedDetail(back, 0)->contentHash());
    EXPECT_EQ("/World/geo/rock", std::string(geoPackedUSDPath(back, back.primitiveCount() - 1)));
    EXPECT_EQ(translation(4.), geoPackedTransform(back, back.primitiveCount() - 1));

    const std::vector<unsigned char> bytes = geoFileEncode(src);
    const GeoFileInfo info = [&] {
        const std::string path = tempPath("geofileformat-packed");
        geoFileWrite(src, path);
        const GeoFileInfo i = geoFileReadInfo(path);
        std::filesystem::remove(path);
        return i;
    }();
    EXPECT_EQ(2u + 1u, info.detailCount) << "the twin's content is stored once";
    EXPECT_EQ(src.contentHash(), info.contentHash);
    EXPECT_FALSE(bytes.empty());
}

TEST(GeoFileFormat, TombstonesAreDefragmentedOnWrite)
{
    GeoDetailBuilder b(makeRich(false));
    const std::array<GeoOffset, 3> doomed { 4, 500, 1800 };
    b.deletePoints(doomed);
    const std::array<GeoOffset, 1> prim { 7 };
    b.deletePrimitives(prim);
    const GeoDetail src = b.finish();
    ASSERT_GT(src.pointMap().tombstoneCount(), 0);

    const GeoDetail back = roundTrip(src);
    EXPECT_EQ(src.pointCount(), back.pointCount());
    EXPECT_EQ(src.primitiveCount(), back.primitiveCount());
    EXPECT_EQ(0, back.pointMap().tombstoneCount());
    EXPECT_EQ(geoFileNormalized(src).contentHash(), back.contentHash());
}

TEST(GeoFileFormat, FileWriteReplacesAtomically)
{
    const std::string path = tempPath("geofileformat-write");
    const GeoDetail first = makeRich(false);
    geoFileWrite(first, path);
    const GeoDetail second = makeRich(true);
    geoFileWrite(second, path);

    EXPECT_EQ(second.contentHash(), geoFileRead(path).contentHash());
    const std::filesystem::path dir = std::filesystem::path(path).parent_path();
    const std::string stem = std::filesystem::path(path).filename().string();
    for (const std::filesystem::directory_entry& e : std::filesystem::directory_iterator(dir)) {
        const std::string n = e.path().filename().string();
        EXPECT_FALSE(n != stem && n.rfind(stem, 0) == 0) << "temporary file left behind: " << n;
    }
    std::filesystem::remove(path);

    EXPECT_THROW(geoFileWrite(first, "/nonexistent-dir-for-geo-test/x.ngeo"), GeoFileError);
    EXPECT_THROW(geoFileRead("/nonexistent-dir-for-geo-test/x.ngeo"), GeoFileError);
}

TEST(GeoFileFormat, TruncatedFileFailsCleanly)
{
    const std::vector<unsigned char> bytes = geoFileEncode(makeRich(false));
    for (const std::size_t keep : { std::size_t(0), std::size_t(5), std::size_t(20), std::size_t(40), bytes.size() / 3, bytes.size() / 2, bytes.size() - 1 }) {
        SCOPED_TRACE(keep);
        expectFileError(std::span<const unsigned char>(bytes.data(), keep), nullptr);
    }
}

TEST(GeoFileFormat, CorruptFileFailsCleanly)
{
    const std::vector<unsigned char> clean = geoFileEncode(makeRich(false));
    for (const std::size_t at : { clean.size() / 4, clean.size() / 2, clean.size() * 3 / 4 }) {
        SCOPED_TRACE(at);
        std::vector<unsigned char> bytes = clean;
        bytes[at] ^= 0x5a;
        expectFileError(bytes, nullptr);
    }
    std::vector<unsigned char> notGeo = clean;
    notGeo[0] = 'X';
    expectFileError(notGeo, "not a native geometry file");
}

TEST(GeoFileFormat, NewerVersionFails)
{
    std::vector<unsigned char> bytes = geoFileEncode(makeRich(false));
    const std::uint32_t future = kGeoFileFormatVersion + 1;
    std::memcpy(bytes.data() + 8, &future, sizeof(future));
    const std::string expected = "version " + std::to_string(future);
    expectFileError(bytes, expected.c_str());
}

TEST(GeoFileFormat, CorruptRawPageFailsNamingItsSection)
{
    GeoDetailBuilder b;
    const GeoOffset first = b.appendPoints(kPoints);
    GeoAttribute& noise = b.addAttribute(eGeoAttribClassPoint, desc("noise", eGeoStorageInt64));
    // An LCG's output does not compress, so every page of the column is stored raw.
    std::uint64_t state = 0x9e3779b97f4a7c15ull;
    for (GeoIndex i = 0; i < kPoints; ++i) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        noise.set<std::int64_t>(std::size_t(first + i), std::int64_t(state));
    }
    std::vector<unsigned char> bytes = geoFileEncode(b.finish());

    const std::string name = "noise";
    const auto at = std::search(bytes.begin(), bytes.end(), name.begin(), name.end());
    ASSERT_TRUE(at != bytes.end());
    // Past the attribute's description, default and page headers, inside its first page.
    const std::size_t inPage = std::size_t(at - bytes.begin()) + name.size() + 256;
    ASSERT_LT(inPage, bytes.size());
    bytes[inPage] ^= 0x01;
    expectFileError(bytes, "attribute 'noise'");
    expectFileError(bytes, "checksum");
}

TEST(GeoFileFormat, CorruptConstantPageFailsNamingItsAttributeAndPage)
{
    GeoDetailBuilder b;
    const GeoOffset first = b.appendPoints(kPoints);
    GeoAttribute& tag = b.addAttribute(eGeoAttribClassPoint, desc("tag", eGeoStorageInt32));
    const std::int32_t value = 0x13572468;
    for (GeoIndex i = 0; i < kPoints; ++i) {
        tag.set<std::int32_t>(std::size_t(first + i), value);
    }
    std::vector<unsigned char> bytes = geoFileEncode(b.finish());

    const std::string name = "tag";
    const auto at = std::search(bytes.begin(), bytes.end(), name.begin(), name.end());
    ASSERT_TRUE(at != bytes.end());
    unsigned char valueBytes[sizeof(value)];
    std::memcpy(valueBytes, &value, sizeof(value));
    const auto stored = std::search(at, bytes.end(), valueBytes, valueBytes + sizeof(value));
    ASSERT_TRUE(stored != bytes.end());
    *stored ^= 0x01;
    expectFileError(bytes, "attribute 'tag'");
    expectFileError(bytes, "page 0");
    expectFileError(bytes, "checksum");
}
