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
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QTemporaryDir>

#include "Engine/Nodes/IO/LayeredDocs/LayeredImageInput.h"
#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_USING

namespace {
const char kSeamFormat[] = "natron_seamtest";
const char kSeamExtension[] = "nlseam";
const char kPsdFormat[] = "natron_seampsd";
const char kBuiltinPsdFormat[] = "natron_psd";

const float kComposite[4] = { 0.25f, 0.5f, 0.75f, 1.f };

// The R of the base layer at `col`, `row` of its plane, rows counted down from the top.
unsigned char
baseValue(int col,
          int row)
{
    return (unsigned char)((row * 2 + col + 1) * 10);
}

// A document that holds whatever the file does not: the file only has to exist, and one that
// starts with "bad" fails to read.
std::shared_ptr<LayeredDocs::LayeredDocument>
readSeamDocument(const std::string& path,
                 std::string* error)
{
    std::ifstream file(path);
    std::string start;
    file >> start;
    if (start.rfind("bad", 0) == 0) {
        *error = "seam test document " + path + " is bad";

        return std::shared_ptr<LayeredDocs::LayeredDocument>();
    }

    const std::shared_ptr<LayeredDocs::LayeredDocument> document = std::make_shared<LayeredDocs::LayeredDocument>();
    document->width = 4;
    document->height = 3;

    LayeredDocs::Plane& composite = document->composite;
    composite.width = 4;
    composite.height = 3;
    composite.channels = { "R", "G", "B", "A" };
    composite.type = OIIO::TypeDesc::FLOAT;
    composite.load = [](std::vector<unsigned char>* pixels, std::string*) {
        pixels->resize(4 * 3 * sizeof(kComposite));
        for (int i = 0; i < 4 * 3; ++i) {
            std::memcpy(pixels->data() + i * sizeof(kComposite), kComposite, sizeof(kComposite));
        }

        return true;
    };

    LayeredDocs::Layer base;
    base.name = "Base Layer";
    base.groups = { "Grp" };
    base.blendMode = "multiply";
    base.opacity = 0.5;
    base.plane.x = 1;
    base.plane.y = 1;
    base.plane.width = 2;
    base.plane.height = 2;
    base.plane.channels = { "R", "G", "B", "A" };
    base.plane.type = OIIO::TypeDesc::UINT8;
    base.plane.load = [](std::vector<unsigned char>* pixels, std::string*) {
        pixels->clear();
        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 2; ++col) {
                pixels->insert(pixels->end(), { baseValue(col, row), 0, 0, 255 });
            }
        }

        return true;
    };
    document->layers.push_back(base);

    LayeredDocs::Layer over;
    over.name = "Over";
    over.blendMode = "normal";
    over.visible = false;
    over.plane.x = 3;
    over.plane.y = 0;
    over.plane.width = 3;
    over.plane.height = 1;
    over.plane.channels = { "R", "G", "B", "A" };
    over.plane.type = OIIO::TypeDesc::UINT16;
    over.plane.load = [](std::vector<unsigned char>* pixels, std::string*) {
        const std::uint16_t values[3 * 4] = { 1000, 2000, 3000, 65535, 0, 0, 0, 0, 0, 0, 0, 0 };
        pixels->resize(sizeof(values));
        std::memcpy(pixels->data(), values, sizeof(values));

        return true;
    };
    document->layers.push_back(over);

    if (!document->finalise(error)) {
        return std::shared_ptr<LayeredDocs::LayeredDocument>();
    }

    return document;
}

const char kTiledFormat[] = "natron_seamtiles";
const char kTiledExtension[] = "nltiles";
const char kMisalignedFormat[] = "natron_seammisaligned";
const char kMisalignedExtension[] = "nlmisaligned";

// The R of the tiled layer and of the windowed composite at `col`, `row` of their planes.
unsigned char
regionValue(int col,
            int row)
{
    return (unsigned char)(col * 10 + row + 1);
}

// The regions the windowed decoders of the tiled document were asked for, in plane pixels.
struct Requests {
    std::mutex mutex;
    std::vector<LayeredDocs::Region> layer;
    std::vector<LayeredDocs::Region> composite;

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex);
        layer.clear();
        composite.clear();
    }
};

Requests&
requests()
{
    static Requests instance;

    return instance;
}

bool
fillRegion(const LayeredDocs::Region& region,
           unsigned char* dst,
           std::vector<LayeredDocs::Region>* log)
{
    {
        std::lock_guard<std::mutex> lock(requests().mutex);
        log->push_back(region);
    }
    for (int row = region.y1; row < region.y2; ++row) {
        for (int col = region.x1; col < region.x2; ++col) {
            const unsigned char pixel[4] = { regionValue(col, row), 0, 0, 255 };
            std::memcpy(dst, pixel, sizeof(pixel));
            dst += sizeof(pixel);
        }
    }

    return true;
}

// A 5x3 canvas whose composite decodes by region as scanlines, and one layer, `layerWidth` x 3 at
// `layerX`,0 so cropped to the canvas on its left, that decodes by region in 2x2 tiles.
std::shared_ptr<LayeredDocs::LayeredDocument>
tiledDocument(int layerX,
              int layerWidth,
              std::string* error)
{
    const std::shared_ptr<LayeredDocs::LayeredDocument> document = std::make_shared<LayeredDocs::LayeredDocument>();
    document->width = 5;
    document->height = 3;

    LayeredDocs::Plane& composite = document->composite;
    composite.width = 5;
    composite.height = 3;
    composite.channels = { "R", "G", "B", "A" };
    composite.type = OIIO::TypeDesc::UINT8;
    composite.loadRegion = [](const LayeredDocs::Region& region, unsigned char* dst, std::string*) {
        return fillRegion(region, dst, &requests().composite);
    };

    LayeredDocs::Layer tiles;
    tiles.name = "Tiles";
    tiles.plane.x = layerX;
    tiles.plane.y = 0;
    tiles.plane.width = layerWidth;
    tiles.plane.height = 3;
    tiles.plane.channels = { "R", "G", "B", "A" };
    tiles.plane.type = OIIO::TypeDesc::UINT8;
    tiles.plane.tileWidth = 2;
    tiles.plane.tileHeight = 2;
    tiles.plane.loadRegion = [](const LayeredDocs::Region& region, unsigned char* dst, std::string*) {
        return fillRegion(region, dst, &requests().layer);
    };
    document->layers.push_back(tiles);

    if (!document->finalise(error)) {
        return std::shared_ptr<LayeredDocs::LayeredDocument>();
    }

    return document;
}

// The layer's stored tiles line up with the canvas: its cropped data window starts on a tile.
std::shared_ptr<LayeredDocs::LayeredDocument>
readTiledDocument(const std::string& /*path*/,
                  std::string* error)
{
    return tiledDocument(-2, 7, error);
}

// The layer's stored tiles straddle the canvas's left edge.
std::shared_ptr<LayeredDocs::LayeredDocument>
readMisalignedDocument(const std::string& /*path*/,
                       std::string* error)
{
    return tiledDocument(-1, 6, error);
}

bool
sameRegion(const LayeredDocs::Region& a,
           const LayeredDocs::Region& b)
{
    return a.x1 == b.x1 && a.y1 == b.y1 && a.x2 == b.x2 && a.y2 == b.y2;
}

void
registerSeamFormat(const std::string& name,
                   const std::vector<std::string>& extensions)
{
    LayeredDocs::Format format;
    format.name = name;
    format.extensions = extensions;
    format.read = readSeamDocument;
    std::string error;
    ASSERT_TRUE(LayeredDocs::registerFormat(format, &error)) << error;
}

// Re-registering keeps the format's slot, so it claims its extensions ahead of later formats again.
void
restoreBuiltinPsd()
{
    LayeredDocs::Format format;
    format.name = kBuiltinPsdFormat;
    format.extensions = { "psd", "psb", "pdd" };
    format.read = LayeredDocs::readPsd;
    std::string error;
    EXPECT_TRUE(LayeredDocs::registerFormat(format, &error)) << error;
}

bool
contains(const std::vector<std::string>& values,
         const std::string& value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::string
writeFile(const QTemporaryDir& dir,
          const char* name,
          const char* content)
{
    const std::string path = (dir.path() + QString::fromUtf8("/") + QString::fromUtf8(name)).toStdString();
    std::ofstream file(path);
    file << content;

    return path;
}

std::optional<std::string>
subimageNameOf(const OIIO::ImageSpec& spec)
{
    const OIIO::ParamValue* value = spec.find_attribute("oiio:subimagename", OIIO::TypeDesc::STRING);

    return value ? std::optional<std::string>(value->get_string()) : std::nullopt;
}

class LayeredDocsSeamTest
    : public ::testing::Test {
protected:
    virtual void SetUp() OVERRIDE
    {
        OiioReadSupport::clearHeaderCache();
        LayeredDocs::clearDocumentCache();
        registerSeamFormat(kSeamFormat, { kSeamExtension });
        ASSERT_TRUE(dir.isValid());
    }

    virtual void TearDown() OVERRIDE
    {
        LayeredDocs::unregisterFormat(kSeamFormat);
        LayeredDocs::unregisterFormat(kPsdFormat);
        LayeredDocs::unregisterFormat(kTiledFormat);
        LayeredDocs::unregisterFormat(kMisalignedFormat);
        restoreBuiltinPsd();
        OiioReadSupport::clearHeaderCache();
    }

    QTemporaryDir dir;
};
} // namespace

TEST_F(LayeredDocsSeamTest, InTreeExtensionsAreReadable)
{
    const std::vector<std::string>& readable = OiioReadSupport::readableExtensions();

    EXPECT_TRUE(contains(readable, "ora"));
    EXPECT_TRUE(contains(readable, kSeamExtension));
    EXPECT_TRUE(std::is_sorted(readable.begin(), readable.end()));
    EXPECT_EQ("natron_ora", OiioReadSupport::formatNameForExtension("ORA"));
    EXPECT_EQ(kSeamFormat, OiioReadSupport::formatNameForExtension(std::string(".") + kSeamExtension));
    EXPECT_TRUE(OiioReadSupport::isReadablePath("/a/doc.ORA"));
    EXPECT_TRUE(OiioReadSupport::isReadablePath(std::string("doc.") + kSeamExtension));
}

TEST_F(LayeredDocsSeamTest, AnOiioFormatNameCannotBeRegistered)
{
    LayeredDocs::Format format;
    format.name = "png";
    format.extensions = { "nlpng" };
    format.read = readSeamDocument;
    std::string error;

    EXPECT_FALSE(LayeredDocs::registerFormat(format, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(LayeredDocs::formatForExtension("nlpng").empty());
}

TEST_F(LayeredDocsSeamTest, TheDocumentOpensThroughReadHeaderAsOneSubimagePerLayerAfterTheComposite)
{
    const std::string path = writeFile(dir, "doc.nlseam", "good");
    std::string error;
    const std::shared_ptr<const OiioReadSupport::Header> header = OiioReadSupport::readHeader(path, &error);
    ASSERT_TRUE(bool(header)) << error;
    ASSERT_EQ(3u, header->subimages.size());

    const std::vector<std::string> rgba = { "R", "G", "B", "A" };
    for (const OIIO::ImageSpec& spec : header->subimages) {
        EXPECT_EQ(0, spec.full_x);
        EXPECT_EQ(0, spec.full_y);
        EXPECT_EQ(4, spec.full_width);
        EXPECT_EQ(3, spec.full_height);
        EXPECT_EQ(rgba, spec.channelnames);
        EXPECT_EQ(3, spec.alpha_channel);
    }

    const OIIO::ImageSpec& composite = header->subimages[0];
    EXPECT_FALSE(subimageNameOf(composite));
    EXPECT_EQ(OIIO::TypeDesc::FLOAT, composite.format);
    EXPECT_EQ(0, composite.x);
    EXPECT_EQ(4, composite.width);
    EXPECT_EQ(3, composite.height);

    const OIIO::ImageSpec& base = header->subimages[1];
    EXPECT_EQ(std::optional<std::string>("Grp_Base_Layer"), subimageNameOf(base));
    EXPECT_EQ(OIIO::TypeDesc::UINT8, base.format);
    EXPECT_EQ(1, base.x);
    EXPECT_EQ(1, base.y);
    EXPECT_EQ(2, base.width);
    EXPECT_EQ(2, base.height);

    const OIIO::ImageSpec& over = header->subimages[2];
    EXPECT_EQ(std::optional<std::string>("Over"), subimageNameOf(over));
    EXPECT_EQ(OIIO::TypeDesc::UINT16, over.format);
    EXPECT_EQ(3, over.x);
    EXPECT_EQ(0, over.y);
    EXPECT_EQ(1, over.width);
    EXPECT_EQ(1, over.height);

    std::vector<OiioReadSupport::FileLayer> layers;
    OiioReadSupport::fileLayers(*header, &layers);
    ASSERT_EQ(2u, layers.size());
    EXPECT_EQ("Grp_Base_Layer", layers[0].desc.getLayerID());
    EXPECT_EQ(1, layers[0].subimage);
    EXPECT_EQ(rgba, layers[0].desc.getChannels());
    EXPECT_EQ("Over", layers[1].desc.getLayerID());
    EXPECT_EQ(2, layers[1].subimage);
}

TEST_F(LayeredDocsSeamTest, SubimagePixelsDecodeInNatronRows)
{
    const std::string path = writeFile(dir, "pixels.nlseam", "good");
    std::string error;
    const std::shared_ptr<const OiioReadSupport::Header> header = OiioReadSupport::readHeader(path, &error);
    ASSERT_TRUE(bool(header)) << error;
    ASSERT_EQ(3u, header->subimages.size());

    const RectI baseWindow = OiioReadSupport::dataWindowOf(header->subimages[1]);
    ASSERT_EQ(2, baseWindow.width());
    ASSERT_EQ(2, baseWindow.height());
    std::vector<float> base(2 * 2 * 4);
    ASSERT_TRUE(OiioReadSupport::decode(path, 1, 0, 4, baseWindow, base.data(), 2 * 4, &error)) << error;
    for (int row = 0; row < 2; ++row) {
        const int fileRow = 1 - row;
        for (int col = 0; col < 2; ++col) {
            const float* pixel = &base[(std::size_t)(row * 2 + col) * 4];
            EXPECT_FLOAT_EQ(baseValue(col, fileRow) / 255.f, pixel[0]) << col << "," << row;
            EXPECT_FLOAT_EQ(0.f, pixel[1]);
            EXPECT_FLOAT_EQ(1.f, pixel[3]);
        }
    }

    const RectI overWindow = OiioReadSupport::dataWindowOf(header->subimages[2]);
    std::vector<float> over(4);
    ASSERT_TRUE(OiioReadSupport::decode(path, 2, 0, 4, overWindow, over.data(), 4, &error)) << error;
    EXPECT_FLOAT_EQ(1000.f / 65535.f, over[0]);
    EXPECT_FLOAT_EQ(2000.f / 65535.f, over[1]);
    EXPECT_FLOAT_EQ(3000.f / 65535.f, over[2]);
    EXPECT_FLOAT_EQ(1.f, over[3]);

    const RectI compositeWindow = OiioReadSupport::dataWindowOf(header->subimages[0]);
    std::vector<float> composite(4 * 3 * 4);
    ASSERT_TRUE(OiioReadSupport::decode(path, 0, 0, 4, compositeWindow, composite.data(), 4 * 4, &error)) << error;
    for (std::size_t i = 0; i < composite.size(); ++i) {
        EXPECT_FLOAT_EQ(kComposite[i % 4], composite[i]) << i;
    }
}

TEST_F(LayeredDocsSeamTest, TheCompositeCarriesTheLayerMetadata)
{
    const std::string path = writeFile(dir, "meta.nlseam", "good");
    std::string error;
    const std::shared_ptr<const OiioReadSupport::Header> header = OiioReadSupport::readHeader(path, &error);
    ASSERT_TRUE(bool(header)) << error;
    ASSERT_FALSE(header->subimages.empty());

    const ImageMetadata metadata = OiioReadSupport::attributeMetadata(path, header->subimages[0]);
    EXPECT_EQ(std::optional<std::string>("Base Layer"), metadata.getString("layered/Grp_Base_Layer/name"));
    EXPECT_EQ(std::optional<std::string>("multiply"), metadata.getString("layered/Grp_Base_Layer/blendMode"));
    EXPECT_EQ(std::optional<double>(0.5), metadata.getDouble("layered/Grp_Base_Layer/opacity"));
    EXPECT_EQ(std::optional<int>(1), metadata.getInt("layered/Grp_Base_Layer/visible"));
    EXPECT_EQ(std::optional<int>(0), metadata.getInt("layered/Grp_Base_Layer/clipping"));
    EXPECT_EQ(std::optional<std::string>("Grp"), metadata.getString("layered/Grp_Base_Layer/group"));
    EXPECT_EQ(std::optional<int>(0), metadata.getInt("layered/Over/visible"));
    EXPECT_EQ(std::optional<std::string>("Grp_Base_Layer, Over"), metadata.getString("layered/layers"));
    EXPECT_EQ(std::optional<std::string>("stored"), metadata.getString("layered/composite"));
    EXPECT_FALSE(metadata.contains("layered/skipped"));
}

TEST_F(LayeredDocsSeamTest, AFailedReadReportsTheFormatsError)
{
    const std::string path = writeFile(dir, "broken.nlseam", "bad");
    std::string error;

    EXPECT_FALSE(OiioReadSupport::readHeader(path, &error));
    EXPECT_NE(std::string::npos, error.find("is bad")) << error;
}

TEST_F(LayeredDocsSeamTest, PsdResolvesToTheInTreeFormatThatClaimsIt)
{
    EXPECT_EQ(kBuiltinPsdFormat, OiioReadSupport::formatNameForExtension("psd"));
    LayeredDocs::unregisterFormat(kBuiltinPsdFormat);
    EXPECT_TRUE(OiioReadSupport::formatNameForExtension("psd").empty());
    EXPECT_FALSE(contains(OiioReadSupport::readableExtensions(), "psd"));

    registerSeamFormat(kPsdFormat, { "psd" });
    EXPECT_EQ(kPsdFormat, OiioReadSupport::formatNameForExtension("psd"));
    EXPECT_TRUE(contains(OiioReadSupport::readableExtensions(), "psd"));
    EXPECT_TRUE(OiioReadSupport::isReadablePath("/a/shot.PSD"));

    const std::string path = writeFile(dir, "shot.psd", "good");
    std::string error;
    const std::shared_ptr<const OiioReadSupport::Header> header = OiioReadSupport::readHeader(path, &error);
    ASSERT_TRUE(bool(header)) << error;
    ASSERT_EQ(3u, header->subimages.size());
    EXPECT_EQ(std::optional<std::string>("Grp_Base_Layer"), subimageNameOf(header->subimages[1]));

    OIIO::ImageInput::unique_ptr input = LayeredDocs::open(path, LayeredDocs::formatForExtension("psd"), &error);
    ASSERT_TRUE(bool(input)) << error;
    EXPECT_EQ(std::string(kPsdFormat), input->format_name());
    EXPECT_TRUE(input->seek_subimage(2, 0));
    EXPECT_FALSE(input->seek_subimage(3, 0));
    input->close();

    LayeredDocs::unregisterFormat(kPsdFormat);
    EXPECT_TRUE(OiioReadSupport::formatNameForExtension("psd").empty());
    EXPECT_FALSE(contains(OiioReadSupport::readableExtensions(), "psd"));
    EXPECT_FALSE(OiioReadSupport::isReadablePath(path));
}

TEST_F(LayeredDocsSeamTest, AWindowedPlaneIsAskedForOnlyTheRowsOrTilesAReadCovers)
{
    LayeredDocs::Format format;
    format.name = kTiledFormat;
    format.extensions = { kTiledExtension };
    format.read = readTiledDocument;
    std::string error;
    ASSERT_TRUE(LayeredDocs::registerFormat(format, &error)) << error;
    requests().clear();

    const std::string path = writeFile(dir, "tiles.nltiles", "good");
    const std::shared_ptr<const OiioReadSupport::Header> header = OiioReadSupport::readHeader(path, &error);
    ASSERT_TRUE(bool(header)) << error;
    ASSERT_EQ(2u, header->subimages.size());
    EXPECT_EQ(0, header->subimages[0].tile_width);
    const OIIO::ImageSpec& layer = header->subimages[1];
    EXPECT_EQ(2, layer.tile_width);
    EXPECT_EQ(2, layer.tile_height);
    EXPECT_EQ(0, layer.x);
    EXPECT_EQ(5, layer.width);
    EXPECT_TRUE(requests().layer.empty());
    EXPECT_TRUE(requests().composite.empty());

    // Natron rows count up from the bottom, so Natron row y is file row 2 - y.
    const RectI layerWindow = OiioReadSupport::dataWindowOf(layer);
    std::vector<float> whole(5 * 3 * 4);
    ASSERT_TRUE(OiioReadSupport::decode(path, 1, 0, 4, layerWindow, whole.data(), 5 * 4, &error)) << error;
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 5; ++x) {
            const float* pixel = &whole[(std::size_t)(y * 5 + x) * 4];
            EXPECT_FLOAT_EQ(regionValue(x + 2, 2 - y) / 255.f, pixel[0]) << x << "," << y;
            EXPECT_FLOAT_EQ(1.f, pixel[3]) << x << "," << y;
        }
    }
    for (const LayeredDocs::Region& region : requests().layer) {
        EXPECT_GE(region.x1, 2);
        EXPECT_LE(region.x2, 7);
        EXPECT_GE(region.y1, 0);
        EXPECT_LE(region.y2, 3);
        EXPECT_GT(region.width(), 0);
        EXPECT_GT(region.height(), 0);
        EXPECT_EQ(region.x1 / 2, (region.x2 - 1) / 2) << "a request stays within one stored tile";
        EXPECT_EQ(region.y1 / 2, (region.y2 - 1) / 2) << "a request stays within one stored tile";
    }

    requests().clear();
    std::vector<float> corner(4);
    ASSERT_TRUE(OiioReadSupport::decode(path, 1, 0, 4, RectI(4, 0, 5, 1), corner.data(), 4, &error)) << error;
    EXPECT_FLOAT_EQ(regionValue(6, 2) / 255.f, corner[0]);
    LayeredDocs::Region cornerTile;
    cornerTile.x1 = 6;
    cornerTile.y1 = 2;
    cornerTile.x2 = 7;
    cornerTile.y2 = 3;
    ASSERT_EQ(1u, requests().layer.size());
    EXPECT_TRUE(sameRegion(cornerTile, requests().layer[0]));

    std::vector<float> row(5 * 4);
    ASSERT_TRUE(OiioReadSupport::decode(path, 0, 0, 4, RectI(2, 1, 4, 2), row.data(), 5 * 4, &error)) << error;
    EXPECT_FLOAT_EQ(regionValue(2, 1) / 255.f, row[0]);
    EXPECT_FLOAT_EQ(regionValue(3, 1) / 255.f, row[4]);
    LayeredDocs::Region compositeRow;
    compositeRow.x1 = 0;
    compositeRow.y1 = 1;
    compositeRow.x2 = 5;
    compositeRow.y2 = 2;
    ASSERT_EQ(1u, requests().composite.size());
    EXPECT_TRUE(sameRegion(compositeRow, requests().composite[0]));
}

TEST_F(LayeredDocsSeamTest, APlaneWhoseTilesStraddleTheDataWindowIsReadAsScanlines)
{
    LayeredDocs::Format format;
    format.name = kMisalignedFormat;
    format.extensions = { kMisalignedExtension };
    format.read = readMisalignedDocument;
    std::string error;
    ASSERT_TRUE(LayeredDocs::registerFormat(format, &error)) << error;
    requests().clear();

    const std::string path = writeFile(dir, "tiles.nlmisaligned", "good");
    const std::shared_ptr<const OiioReadSupport::Header> header = OiioReadSupport::readHeader(path, &error);
    ASSERT_TRUE(bool(header)) << error;
    ASSERT_EQ(2u, header->subimages.size());
    const OIIO::ImageSpec& layer = header->subimages[1];
    EXPECT_EQ(0, layer.tile_width);
    EXPECT_EQ(0, layer.tile_height);
    EXPECT_EQ(0, layer.x);
    EXPECT_EQ(5, layer.width);

    const RectI layerWindow = OiioReadSupport::dataWindowOf(layer);
    std::vector<float> whole(5 * 3 * 4);
    ASSERT_TRUE(OiioReadSupport::decode(path, 1, 0, 4, layerWindow, whole.data(), 5 * 4, &error)) << error;
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 5; ++x) {
            const float* pixel = &whole[(std::size_t)(y * 5 + x) * 4];
            EXPECT_FLOAT_EQ(regionValue(x + 1, 2 - y) / 255.f, pixel[0]) << x << "," << y;
            EXPECT_FLOAT_EQ(1.f, pixel[3]) << x << "," << y;
        }
    }
}
