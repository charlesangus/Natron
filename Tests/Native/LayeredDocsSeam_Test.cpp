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
