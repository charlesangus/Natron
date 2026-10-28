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
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "RawSpeed-API.h"

#include "Engine/Nodes/IO/RawDecodeSupport.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/IO/RawspeedRuntime.h"
#include "Engine/RectI.h"

#include "RawFixtures.h"

NATRON_NAMESPACE_USING

namespace {
// rawspeed's own decode of a file, set up as RawDecodeSupport sets it up, for the expected values.
struct Reference {
    std::unique_ptr<rawspeed::RawDecoder> decoder;
    std::optional<rawspeed::RawImage> image;
};

bool
referenceDecode(const std::string& path,
                Reference* ref,
                std::string* error)
{
    const rawspeed::CameraMetaData* cameras = RawspeedRuntime::cameraMetaData(error);

    if (!cameras) {
        return false;
    }
    try {
        auto [storage, buffer] = rawspeed::FileReader(path.c_str()).readFile();
        rawspeed::RawParser parser(buffer);
        ref->decoder = parser.getDecoder(cameras);
        ref->decoder->failOnUnknown = true;
        ref->decoder->checkSupport(cameras);
        ref->decoder->decodeRaw();
        ref->decoder->decodeMetaData(cameras);
        ref->image = ref->decoder->mRaw;
        ref->decoder.reset();
    } catch (const std::exception& e) {
        *error = e.what();

        return false;
    }

    return true;
}

// The value of channel c of Natron pixel (x, y) in rawspeed's buffer, whose rows run down from the
// top of the uncropped sensor.
float
referenceValue(rawspeed::RawImageData& raw,
               int x,
               int y,
               int c)
{
    const int row = raw.getUncroppedDim().y - 1 - y;
    const int col = x * (int)raw.getCpp() + c;

    if (raw.getDataType() == rawspeed::RawImageType::F32) {
        return raw.getF32DataAsUncroppedArray2DRef()(row, col);
    }

    return (float)raw.getU16DataAsUncroppedArray2DRef()(row, col);
}

RawCfaColorEnum
referenceColour(rawspeed::RawImageData& raw,
                int x,
                int y)
{
    const rawspeed::iPoint2D crop = raw.getCropOffset();
    const int fileRow = raw.getUncroppedDim().y - 1 - y;
    rawspeed::CFAColor colour;

    if (raw.cfa.getDcrawFilter() == 9) {
        colour = raw.cfa.getColorAt(x, fileRow);
    } else {
        colour = raw.cfa.getColorAt(x - crop.x, fileRow - crop.y);
    }
    switch (colour) {
    case rawspeed::CFAColor::RED:
        return eRawCfaColorRed;
    case rawspeed::CFAColor::GREEN:
    case rawspeed::CFAColor::FUJI_GREEN:
        return eRawCfaColorGreen;
    case rawspeed::CFAColor::BLUE:
        return eRawCfaColorBlue;
    default:
        return eRawCfaColorNone;
    }
}

std::string
temporaryCopy(const QTemporaryDir& dir,
              const std::string& name,
              const std::vector<char>& bytes)
{
    const std::string path = (dir.path() + QString::fromUtf8("/") + QString::fromStdString(name)).toStdString();

    EXPECT_TRUE(writeFixtureBytes(path, bytes.data(), bytes.size())) << path;

    return path;
}
} // namespace

class RawDecodeSupportTest
    : public ::testing::Test {
protected:
    virtual void SetUp() OVERRIDE
    {
        RawDecodeSupport::clearCaches();
        RawDecodeSupport::setDecodeCacheCapacityForTests(std::nullopt);
    }

    virtual void TearDown() OVERRIDE
    {
        RawDecodeSupport::setDecodedHookForTests(std::function<void(const std::string&)>());
        RawDecodeSupport::setDecodeCacheCapacityForTests(std::nullopt);
        RawDecodeSupport::clearCaches();
    }
};

TEST_F(RawDecodeSupportTest, HeaderMatchesRawspeedForEachFixture)
{
    for (const RawFixture& fixture : rawFixtures()) {
        SCOPED_TRACE(fixture.file);
        std::string error;
        const std::shared_ptr<const RawDecodeSupport::Header> header = RawDecodeSupport::readHeader(fixture.path(), &error);
        ASSERT_TRUE(bool(header)) << error;

        Reference ref;
        ASSERT_TRUE(referenceDecode(fixture.path(), &ref, &error)) << error;
        rawspeed::RawImageData& raw = **ref.image;
        const rawspeed::iPoint2D uncropped = raw.getUncroppedDim();
        const rawspeed::iPoint2D crop = raw.getCropOffset();

        EXPECT_EQ(RectI(0, 0, uncropped.x, uncropped.y), header->format);
        EXPECT_EQ(raw.dim.x, header->window.width());
        EXPECT_EQ(raw.dim.y, header->window.height());
        EXPECT_EQ(crop.x, header->window.x1);
        EXPECT_EQ(uncropped.y - crop.y, header->window.y2);
        EXPECT_EQ(fixture.channels, header->channels);
        EXPECT_EQ(raw.isCFA ? 1 : (int)raw.getCpp(), header->channels);

        const ImageMetadata& metadata = header->metadata;
        EXPECT_EQ(std::optional<RectI>(header->window), RawMetadata::getCrop(metadata));
        EXPECT_EQ(std::optional<RectI>(header->format), RawMetadata::getSensor(metadata));
        EXPECT_EQ(std::optional<RawStageEnum>(eRawStageRaw), RawMetadata::getStage(metadata));
        if (raw.whitePoint) {
            EXPECT_EQ(std::optional<double>((double)*raw.whitePoint), RawMetadata::getWhiteLevel(metadata));
        }
        if (raw.metadata.isoSpeed > 0) {
            EXPECT_EQ(std::optional<double>((double)raw.metadata.isoSpeed), RawMetadata::getIso(metadata));
        }
        if (raw.metadata.wbCoeffs && (*raw.metadata.wbCoeffs)[0] > 0.f && (*raw.metadata.wbCoeffs)[1] > 0.f && (*raw.metadata.wbCoeffs)[2] > 0.f) {
            const std::optional<std::array<double, 3>> wb = RawMetadata::getWbCoeffs(metadata);
            ASSERT_TRUE(wb.has_value());
            for (std::size_t i = 0; i < 3; ++i) {
                EXPECT_EQ((double)(*raw.metadata.wbCoeffs)[i], (*wb)[i]);
            }
        }
        EXPECT_EQ(raw.metadata.colorMatrix.size() >= 9, RawMetadata::getCamToXyz(metadata).has_value());

        if (!raw.blackLevelSeparate) {
            raw.calculateBlackAreas();
        }
        ASSERT_TRUE(bool(raw.blackLevelSeparate));
        const rawspeed::Array1DRef<int> separate = *raw.blackLevelSeparate->getAsArray1DRef();
        const std::optional<std::vector<double>> black = RawMetadata::getBlackLevels(metadata);
        ASSERT_TRUE(black.has_value());

        const std::optional<RawLayout> layout = RawLayout::fromMetadata(metadata);
        ASSERT_TRUE(layout.has_value());
        EXPECT_EQ(fixture.period, layout->period());
        if (fixture.period == 0) {
            EXPECT_FALSE(raw.isCFA);
            EXPECT_EQ(std::optional<std::string>(std::string(kRawCfaPatternNone)), metadata.getString(kRawMetadataCfaPattern));
            ASSERT_EQ((std::size_t)header->channels, black->size());
            for (int c = 0; c < header->channels; ++c) {
                EXPECT_EQ((double)separate(std::min(c, 3)), (*black)[(std::size_t)c]);
            }
            continue;
        }

        ASSERT_TRUE(raw.isCFA);
        // Every photosite of a block several periods wide, at the window's bottom-left and at an
        // odd offset inside it, has rawspeed's colour and black level.
        const RectI& w = header->window;
        ASSERT_EQ(4u, black->size());
        for (const std::pair<int, int>& at : { std::make_pair(w.x1, w.y1), std::make_pair(w.x1 + 101, w.y1 + 57), std::make_pair(w.x2 - 13, w.y2 - 13) }) {
            for (int y = at.second; y < at.second + 13; ++y) {
                for (int x = at.first; x < at.first + 13; ++x) {
                    EXPECT_EQ(referenceColour(raw, x, y), layout->colourAt(x, y)) << x << "," << y;
                    const int fileRow = uncropped.y - 1 - y;
                    const int cell = (((y - layout->originY()) & 1) * 2) + ((x - layout->originX()) & 1);
                    EXPECT_EQ((double)separate(2 * (fileRow & 1) + (x & 1)), (*black)[(std::size_t)cell]) << x << "," << y;
                }
            }
        }
    }
}

// darktable keys its white balance presets (data/wb_presets.json: maker "Canon", model "EOS 5D
// Mark III"; maker "Fujifilm", model "X-Pro2") and its colour matrix table (src/common/
// colormatrices.c: "Canon EOS 5D Mark III") on rawspeed's canonical names.
TEST_F(RawDecodeSupportTest, CameraNamesAreRawspeedsCanonicalNames)
{
    struct Expected {
        const RawFixture* fixture;
        const char* make;
        const char* model;
    };
    const Expected expected[] = {
        { &bayerFixture(), "Canon", "EOS 5D Mark III" },
        { &xtransFixture(), "Fujifilm", "X-Pro2" },
    };

    for (const Expected& e : expected) {
        SCOPED_TRACE(e.fixture->file);
        std::string error;
        const std::shared_ptr<const RawDecodeSupport::Header> header = RawDecodeSupport::readHeader(e.fixture->path(), &error);
        ASSERT_TRUE(bool(header)) << error;

        EXPECT_EQ(std::optional<std::string>(e.make), RawMetadata::getMake(header->metadata));
        EXPECT_EQ(std::optional<std::string>(e.model), RawMetadata::getModel(header->metadata));
    }

    std::string error;
    const std::shared_ptr<const RawDecodeSupport::Header> canon = RawDecodeSupport::readHeader(bayerFixture().path(), &error);
    ASSERT_TRUE(bool(canon)) << error;
    EXPECT_EQ("Canon EOS 5D Mark III", *RawMetadata::getMake(canon->metadata) + " " + *RawMetadata::getModel(canon->metadata));

    const std::shared_ptr<const RawDecodeSupport::Header> dng = RawDecodeSupport::readHeader(linearFixture().path(), &error);
    ASSERT_TRUE(bool(dng)) << error;
    EXPECT_FALSE(RawMetadata::getMake(dng->metadata).value_or("").empty());
    EXPECT_FALSE(RawMetadata::getModel(dng->metadata).value_or("").empty());
}

// The values exiv2 reports for each fixture. None of them records a SubjectDistance.
TEST_F(RawDecodeSupportTest, LensExifKeysComeFromTheFilesExif)
{
    struct Expected {
        const RawFixture* fixture;
        const char* make;
        const char* model;
        const char* lensModel;
        double focalLength;
        double fNumber;
    };
    const Expected expected[] = {
        { &bayerFixture(), "Canon", "Canon EOS 5D Mark III", "EF70-200mm f/2.8L IS II USM", 70., 2.8 },
        { &xtransFixture(), "FUJIFILM", "X-Pro2", "XF50mmF2 R WR", 50., 2. },
        { &linearFixture(), "Canon", "Canon EOS 5D Mark III", "EF70-200mm f/2.8L IS II USM", 70., 2.8 },
    };

    for (const Expected& e : expected) {
        SCOPED_TRACE(e.fixture->file);
        std::string error;
        const std::shared_ptr<const RawDecodeSupport::Header> header = RawDecodeSupport::readHeader(e.fixture->path(), &error);
        ASSERT_TRUE(bool(header)) << error;
        const ImageMetadata& metadata = header->metadata;

        EXPECT_EQ(std::optional<std::string>(e.make), metadata.getString(kExifMetadataMake));
        EXPECT_EQ(std::optional<std::string>(e.model), metadata.getString(kExifMetadataModel));
        EXPECT_EQ(std::optional<std::string>(e.lensModel), metadata.getString(kExifMetadataLensModel));
        ASSERT_TRUE(metadata.getDouble(kExifMetadataFocalLength).has_value());
        EXPECT_NEAR(e.focalLength, *metadata.getDouble(kExifMetadataFocalLength), 1e-6);
        ASSERT_TRUE(metadata.getDouble(kExifMetadataFNumber).has_value());
        EXPECT_NEAR(e.fNumber, *metadata.getDouble(kExifMetadataFNumber), 1e-6);
        EXPECT_FALSE(metadata.contains(kExifMetadataSubjectDistance));
    }
}

TEST_F(RawDecodeSupportTest, CameraFilesHaveNoDngMatrixAndALinearResponseLimitOfOne)
{
    for (const RawFixture* fixture : { &bayerFixture(), &xtransFixture() }) {
        SCOPED_TRACE(fixture->file);
        std::string error;
        const std::shared_ptr<const RawDecodeSupport::Header> header = RawDecodeSupport::readHeader(fixture->path(), &error);
        ASSERT_TRUE(bool(header)) << error;

        EXPECT_FALSE(RawMetadata::getDngD65Matrix(header->metadata).has_value());
        EXPECT_FALSE(header->metadata.contains(kRawMetadataLinearResponseLimit));
        EXPECT_EQ(1., RawMetadata::getLinearResponseLimit(header->metadata));
    }
}

// The linear DNG carries ColorMatrix1/2 for illuminants A (17) and D65 (21), CameraCalibration1/2
// and an AnalogBalance. darktable picks the D65 entry and its Bradford adaptation to D65 is the
// identity, so its d65_color_matrix is AnalogBalance * CameraCalibration2 * ColorMatrix2. The
// tag values below were read from the file with a plain TIFF parser; darktable's logged matrix
// for this file comes with the reference regeneration.
TEST_F(RawDecodeSupportTest, LinearDngYieldsTheD65MatrixFromItsEmbeddedTags)
{
    std::string error;
    const std::shared_ptr<const RawDecodeSupport::Header> header = RawDecodeSupport::readHeader(linearFixture().path(), &error);
    ASSERT_TRUE(bool(header)) << error;

    const std::array<double, 9> colorMatrix2 = { 0.6722, -0.0635, -0.0963, -0.4287, 1.246, 0.2028, -0.0908, 0.2162, 0.5668 };
    const std::array<double, 9> cameraCalibration2 = { 1.002, 0., 0., 0., 1., 0., 0., 0., 1.0062 };
    const std::array<double, 3> analogBalance = { 1.399522, 1., 2.677346 };

    const std::optional<std::array<double, 9>> matrix = RawMetadata::getDngD65Matrix(header->metadata);
    ASSERT_TRUE(matrix.has_value());
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            double expected = 0.;
            for (int k = 0; k < 3; ++k) {
                expected += analogBalance[(std::size_t)row] * cameraCalibration2[(std::size_t)(3 * row + k)] * colorMatrix2[(std::size_t)(3 * k + col)];
            }
            EXPECT_NEAR(expected, (*matrix)[(std::size_t)(3 * row + col)], 1e-5) << "row " << row << " col " << col;
        }
    }
    EXPECT_EQ(1., RawMetadata::getLinearResponseLimit(header->metadata));
}

TEST_F(RawDecodeSupportTest, FullDecodeMatchesRawspeedAndWindowsMatchTheFullDecode)
{
    for (const RawFixture& fixture : rawFixtures()) {
        SCOPED_TRACE(fixture.file);
        std::string error;
        const std::shared_ptr<const RawDecodeSupport::Header> header = RawDecodeSupport::readHeader(fixture.path(), &error);
        ASSERT_TRUE(bool(header)) << error;
        const RectI full = header->window;
        const std::size_t nComps = (std::size_t)header->channels;
        const std::size_t stride = (std::size_t)full.width() * nComps;

        std::vector<float> all(stride * (std::size_t)full.height());
        ASSERT_TRUE(RawDecodeSupport::decode(fixture.path(), full, all.data(), stride, &error)) << error;

        Reference ref;
        ASSERT_TRUE(referenceDecode(fixture.path(), &ref, &error)) << error;
        rawspeed::RawImageData& raw = **ref.image;
        std::size_t mismatches = 0;
        for (int y = full.y1; y < full.y2; ++y) {
            const float* row = all.data() + (std::size_t)(y - full.y1) * stride;
            for (int x = full.x1; x < full.x2; ++x) {
                for (std::size_t c = 0; c < nComps; ++c) {
                    if (row[(std::size_t)(x - full.x1) * nComps + c] != referenceValue(raw, x, y, (int)c)) {
                        ++mismatches;
                    }
                }
            }
        }
        EXPECT_EQ(0u, mismatches);

        const RectI windows[] = {
            RectI(full.x1, full.y1, full.x1 + 1, full.y1 + 1),
            RectI(full.x1 + 17, full.y1 + 31, full.x1 + 17 + 333, full.y1 + 31 + 129),
            RectI(full.x2 - 64, full.y2 - 7, full.x2, full.y2),
            RectI(full.x1 + 1, full.y1 + 1, full.x2 - 1, full.y1 + 3),
        };
        for (const RectI& window : windows) {
            // A padded stride, to check rows land where the stride says.
            const std::size_t windowStride = (std::size_t)window.width() * nComps + 5;
            std::vector<float> part(windowStride * (std::size_t)window.height(), -1.f);
            ASSERT_TRUE(RawDecodeSupport::decode(fixture.path(), window, part.data(), windowStride, &error)) << error;
            for (int y = window.y1; y < window.y2; ++y) {
                const float* got = part.data() + (std::size_t)(y - window.y1) * windowStride;
                const float* want = all.data() + (std::size_t)(y - full.y1) * stride + (std::size_t)(window.x1 - full.x1) * nComps;
                EXPECT_EQ(0, std::memcmp(got, want, (std::size_t)window.width() * nComps * sizeof(float))) << "row " << y;
            }
        }

        std::vector<float> outside(4 * nComps);
        EXPECT_FALSE(RawDecodeSupport::decode(fixture.path(), RectI(full.x1 - 1, full.y1, full.x1 + 1, full.y1 + 1), outside.data(), 4 * nComps, &error));
    }
}

TEST_F(RawDecodeSupportTest, TruncatedFileFailsCleanly)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::vector<char> bytes = readFixtureBytes(bayerFixture().path());
    ASSERT_GT(bytes.size(), 1024u);

    for (const std::size_t keep : { bytes.size() / 2, (std::size_t)4096, (std::size_t)16 }) {
        SCOPED_TRACE(keep);
        const std::string path = temporaryCopy(dir, "truncated-" + std::to_string(keep) + ".cr2", std::vector<char>(bytes.begin(), bytes.begin() + (std::ptrdiff_t)keep));
        std::string error;
        EXPECT_FALSE(bool(RawDecodeSupport::readHeader(path, &error)));
        EXPECT_FALSE(error.empty());
        float pixel = 0.f;
        error.clear();
        EXPECT_FALSE(RawDecodeSupport::decode(path, RectI(0, 0, 1, 1), &pixel, 1, &error));
        EXPECT_FALSE(error.empty());
    }

    std::string error;
    EXPECT_FALSE(bool(RawDecodeSupport::readHeader(dir.path().toStdString() + "/missing.cr2", &error)));
    EXPECT_FALSE(error.empty());
}

TEST_F(RawDecodeSupportTest, UnsupportedCameraNamesTheCameraAndRawspeedVersion)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    std::vector<char> bytes = readFixtureBytes(bayerFixture().path());
    // Same length, so every TIFF offset and count still holds; rawspeed's database has no such
    // camera and the decoder is not allowed to guess.
    const std::string real = "Canon EOS 5D Mark III";
    const std::string stub = "Canon EOS 5D Mark XII";
    ASSERT_EQ(real.size(), stub.size());
    std::size_t patched = 0;
    for (std::vector<char>::iterator it = bytes.begin(); (it = std::search(it, bytes.end(), real.begin(), real.end())) != bytes.end(); it += (std::ptrdiff_t)real.size()) {
        std::copy(stub.begin(), stub.end(), it);
        ++patched;
    }
    ASSERT_GT(patched, 0u);
    const std::string path = temporaryCopy(dir, "unsupported.cr2", bytes);

    std::string error;
    EXPECT_FALSE(bool(RawDecodeSupport::readHeader(path, &error)));
    EXPECT_NE(std::string::npos, error.find("Canon EOS 5D Mark XII")) << error;
    EXPECT_NE(std::string::npos, error.find(RawspeedRuntime::version())) << error;
    EXPECT_NE(std::string::npos, error.find("not supported")) << error;
    EXPECT_EQ(0u, error.find(RawDecodeSupport::unsupportedCameraMessage("Canon", "Canon EOS 5D Mark XII", std::string()).substr(0, 30))) << error;
}

TEST_F(RawDecodeSupportTest, ConcurrentFirstReadsDecodeOnce)
{
    std::atomic<int> decodes(0);
    RawDecodeSupport::setDecodedHookForTests([&decodes](const std::string&) { ++decodes; });

    const std::string path = bayerFixture().path();
    std::atomic<int> failures(0);
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&path, &failures]() {
            std::string error;
            if (!RawDecodeSupport::readHeader(path, &error)) {
                ++failures;
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    EXPECT_EQ(0, failures.load());
    EXPECT_EQ(1, decodes.load());

    std::string error;
    float pixel = 0.f;
    const std::shared_ptr<const RawDecodeSupport::Header> header = RawDecodeSupport::readHeader(path, &error);
    ASSERT_TRUE(bool(header)) << error;
    const RectI first(header->window.x1, header->window.y1, header->window.x1 + 1, header->window.y1 + 1);
    EXPECT_TRUE(RawDecodeSupport::decode(path, first, &pixel, 1, &error)) << error;
    EXPECT_EQ(1, decodes.load());
}

TEST_F(RawDecodeSupportTest, DecodeCacheIsBoundedButKeepsTheLatestFile)
{
    EXPECT_GT(RawDecodeSupport::decodeCacheCapacity(), 0u);

    std::atomic<int> decodes(0);
    RawDecodeSupport::setDecodedHookForTests([&decodes](const std::string&) { ++decodes; });
    RawDecodeSupport::setDecodeCacheCapacityForTests((std::size_t)1);

    std::string error;
    ASSERT_TRUE(bool(RawDecodeSupport::readHeader(bayerFixture().path(), &error))) << error;
    const std::size_t oneFile = RawDecodeSupport::decodeCacheBytes();
    EXPECT_GT(oneFile, 0u);
    ASSERT_TRUE(bool(RawDecodeSupport::readHeader(xtransFixture().path(), &error))) << error;
    EXPECT_EQ(2, decodes.load());
    EXPECT_NE(oneFile, RawDecodeSupport::decodeCacheBytes());

    // The header outlives the evicted image, but the pixels need a new decode.
    ASSERT_TRUE(bool(RawDecodeSupport::readHeader(bayerFixture().path(), &error))) << error;
    EXPECT_EQ(2, decodes.load());
    const std::shared_ptr<const RawDecodeSupport::Header> header = RawDecodeSupport::readHeader(bayerFixture().path(), &error);
    ASSERT_TRUE(bool(header));
    float pixel = 0.f;
    const RectI first(header->window.x1, header->window.y1, header->window.x1 + 1, header->window.y1 + 1);
    ASSERT_TRUE(RawDecodeSupport::decode(bayerFixture().path(), first, &pixel, 1, &error)) << error;
    EXPECT_EQ(3, decodes.load());
    EXPECT_EQ(oneFile, RawDecodeSupport::decodeCacheBytes());

    RawDecodeSupport::setDecodeCacheCapacityForTests(std::nullopt);
    const std::shared_ptr<const RawDecodeSupport::Header> xtrans = RawDecodeSupport::readHeader(xtransFixture().path(), &error);
    ASSERT_TRUE(bool(xtrans)) << error;
    const RectI xtransFirst(xtrans->window.x1, xtrans->window.y1, xtrans->window.x1 + 1, xtrans->window.y1 + 1);
    ASSERT_TRUE(RawDecodeSupport::decode(xtransFixture().path(), xtransFirst, &pixel, 1, &error)) << error;
    EXPECT_EQ(4, decodes.load());
    ASSERT_TRUE(RawDecodeSupport::decode(bayerFixture().path(), first, &pixel, 1, &error)) << error;
    EXPECT_EQ(4, decodes.load());

    RawDecodeSupport::evict({ bayerFixture().path() });
    ASSERT_TRUE(bool(RawDecodeSupport::readHeader(bayerFixture().path(), &error))) << error;
    EXPECT_EQ(5, decodes.load());
}
