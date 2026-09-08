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

#include <cstdint>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "DataKindTestEffect.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/TypedPassthrough.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

struct ExrChannelInfo {
    std::string name;
    int32_t pixelType;
};

bool
readExrCString(std::ifstream& in,
               std::string* out)
{
    out->clear();
    char c;
    while (in.get(c)) {
        if (c == '\0') {
            return true;
        }
        out->push_back(c);
    }

    return false;
}

template <typename T>
bool
readExrPod(std::ifstream& in,
           T* out)
{
    in.read(reinterpret_cast<char*>(out), sizeof(T));

    return bool(in);
}

// Same rationale as RenderRange_Test.cpp's readExrFirstPixelRed, generalized to the whole
// image instead of a single pixel: WriteOIIO is configured below with bitDepth=32f and
// compression=none, giving a single-part, scanline, uncompressed, 32-bit float layout that
// a small hand-rolled reader can parse exactly, rather than linking OpenImageIO/OpenEXR into
// the Tests binary. This is what lets the "renders identically" test below compare every
// pixel of the two renders instead of only their first one.
bool
readExrChannelPlane(const std::string& path,
                    const std::string& channelName,
                    std::vector<float>* outValues,
                    int* outWidth,
                    int* outHeight,
                    std::string* error)
{
    std::ifstream in(path, std::ios::binary);

    if (!in) {
        *error = "cannot open file";

        return false;
    }

    char magic[4];
    in.read(magic, 4);
    static const char kMagic[4] = { 0x76, 0x2f, 0x31, 0x01 };
    if (!in || (std::memcmp(magic, kMagic, 4) != 0)) {
        *error = "bad magic number";

        return false;
    }

    int32_t version = 0;
    if (!readExrPod(in, &version)) {
        *error = "cannot read version field";

        return false;
    }
    if (version & 0x200) {
        *error = "tiled EXR files are not supported by this test's parser";

        return false;
    }
    if (version & 0x1000) {
        *error = "deep EXR files are not supported by this test's parser";

        return false;
    }
    if (version & 0x2000) {
        *error = "multipart EXR files are not supported by this test's parser";

        return false;
    }

    std::vector<ExrChannelInfo> channels;
    int32_t dataWindow[4] = { 0, 0, 0, 0 };
    bool haveDataWindow = false;
    int compression = -1;

    for (;;) {
        std::string name;
        if (!readExrCString(in, &name)) {
            *error = "truncated header (attribute name)";

            return false;
        }
        if (name.empty()) {
            break;
        }
        std::string type;
        if (!readExrCString(in, &type)) {
            *error = "truncated header (attribute type)";

            return false;
        }
        int32_t size = 0;
        if (!readExrPod(in, &size) || (size < 0)) {
            *error = "truncated header (attribute size)";

            return false;
        }
        std::vector<char> data(size);
        if ((size > 0) && !in.read(data.data(), size)) {
            *error = "truncated attribute data for " + name;

            return false;
        }

        if (name == "channels") {
            size_t pos = 0;
            while (pos < data.size()) {
                std::string cname;
                while ((pos < data.size()) && (data[pos] != '\0')) {
                    cname.push_back(data[pos]);
                    ++pos;
                }
                if (pos >= data.size()) {
                    break;
                }
                ++pos; // the channel name's null terminator
                if (cname.empty()) {
                    break; // empty name terminates the channel list
                }
                if (pos + 16 > data.size()) {
                    *error = "malformed channel list entry for " + cname;

                    return false;
                }
                int32_t pixelType = 0;
                std::memcpy(&pixelType, &data[pos], 4);
                pos += 16; // pixelType(4) + pLinear/reserved(4) + xSampling(4) + ySampling(4)
                channels.push_back(ExrChannelInfo { cname, pixelType });
            }
        } else if ((name == "compression") && (size >= 1)) {
            compression = static_cast<unsigned char>(data[0]);
        } else if ((name == "dataWindow") && (size >= 16)) {
            std::memcpy(dataWindow, data.data(), 16);
            haveDataWindow = true;
        }
    }

    if (!haveDataWindow) {
        *error = "no dataWindow attribute found";

        return false;
    }
    if (compression != 0) {
        *error = "expected NO_COMPRESSION, found compression id " + std::to_string(compression);

        return false;
    }

    int channelIndex = -1;
    std::string foundNames;
    for (size_t i = 0; i < channels.size(); ++i) {
        if (!foundNames.empty()) {
            foundNames += ",";
        }
        foundNames += channels[i].name;
        if ((channelIndex < 0) && (channels[i].name == channelName)) {
            channelIndex = static_cast<int>(i);
        }
    }
    if (channelIndex < 0) {
        *error = "no " + channelName + " channel found, channels were: " + foundNames;

        return false;
    }
    if (channels[channelIndex].pixelType != 2 /* FLOAT, per the OpenEXR spec */) {
        *error = channelName + " channel is not 32-bit float";

        return false;
    }

    const int width = dataWindow[2] - dataWindow[0] + 1;
    const int height = dataWindow[3] - dataWindow[1] + 1;

    *outWidth = width;
    *outHeight = height;
    outValues->assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0.f);

    size_t byteOffset = 0;
    for (int i = 0; i < channelIndex; ++i) {
        const int bytesPerSample = (channels[i].pixelType == 1 /* HALF */) ? 2 : 4;
        byteOffset += static_cast<size_t>(width) * bytesPerSample;
    }

    for (int row = 0; row < height; ++row) {
        int64_t offset = 0;
        if (!readExrPod(in, &offset)) {
            *error = "cannot read the scanline offset table";

            return false;
        }

        const std::streampos afterOffsetEntry = in.tellg();

        in.seekg(offset, std::ios::beg);
        int32_t y = 0;
        int32_t chunkDataSize = 0;
        if (!readExrPod(in, &y) || !readExrPod(in, &chunkDataSize) || (chunkDataSize < 0)) {
            *error = "cannot read the scanline chunk header";

            return false;
        }

        std::vector<char> chunk(chunkDataSize);
        if (!in.read(chunk.data(), chunkDataSize)) {
            *error = "truncated scanline pixel data";

            return false;
        }
        if (byteOffset + static_cast<size_t>(width) * 4 > chunk.size()) {
            *error = "scanline data too small to hold the requested channel";

            return false;
        }

        const int rowIndex = y - dataWindow[1];
        if ((rowIndex < 0) || (rowIndex >= height)) {
            *error = "scanline y is outside dataWindow";

            return false;
        }
        std::memcpy(&(*outValues)[static_cast<size_t>(rowIndex) * width], &chunk[byteOffset], static_cast<size_t>(width) * 4);

        in.seekg(afterOffsetEntry);
    }

    return true;
} // readExrChannelPlane

} // namespace

// The checkable form of "appears in the node menu": the plugin is registered with AppManager
// (so the node graph UI would list it) and can be instantiated by its plugin id, and the
// instantiated node reports that same id back.
TEST_F(BaseTest, TypedPassthroughIsRegisteredAndInstantiable)
{
    Plugin* p = appPTR->getPluginBinary(QString::fromUtf8(PLUGINID_NATRON_TYPEDPASSTHROUGH), -1, -1, false);

    ASSERT_TRUE(p != NULL);

    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_TYPEDPASSTHROUGH));

    ASSERT_TRUE(bool(node));
    EXPECT_EQ(std::string(PLUGINID_NATRON_TYPEDPASSTHROUGH), node->getPluginID());
}

// Mirrors DataKind_Test.cpp's DotFedByImageSourceResolvesToImage: a freshly connected
// TypedPassthrough has no declared kind of its own (eDataKindPolymorphic), so its effective
// output kind must resolve structurally to whatever feeds it, here an image-kind generator.
TEST_F(BaseTest, TypedPassthroughFedByImageSourceResolvesToImage)
{
    NodePtr generator = createNode(_generatorPluginID);
    NodePtr proof = createNode(QString::fromUtf8(PLUGINID_NATRON_TYPEDPASSTHROUGH));

    ASSERT_TRUE(generator && proof);

    connectNodes(generator, proof, 0, true);

    EXPECT_EQ(eDataKindImage, proof->getEffectiveOutputDataKind());
}

// Mirrors DataKind_Test.cpp's DisconnectedDotResolvesUnconstrained: with nothing feeding it,
// TypedPassthrough resolves to eDataKindPolymorphic itself -- "no constraint yet", not a
// fallback to image.
TEST_F(BaseTest, DisconnectedTypedPassthroughResolvesUnconstrained)
{
    NodePtr proof = createNode(QString::fromUtf8(PLUGINID_NATRON_TYPEDPASSTHROUGH));

    ASSERT_TRUE(bool(proof));

    EXPECT_EQ(eDataKindPolymorphic, proof->getEffectiveOutputDataKind());
}

// Mirrors DataKind_Test.cpp's DotChainContradictionRejectedFromUpstreamSide: a contradiction
// introduced through a TypedPassthrough chain must be rejected exactly as it would be through
// a Dot, proving the proof node participates in kind resolution with no special-casing.
TEST_F(BaseTest, TypedPassthroughChainContradictionRejected)
{
    NodePtr deepSource = createNode(QString::fromUtf8(kTestPluginIDDataKindDeepSource));
    NodePtr proof = createNode(QString::fromUtf8(PLUGINID_NATRON_TYPEDPASSTHROUGH));
    NodePtr imageSink = createNode(QString::fromUtf8(kTestPluginIDDataKindImageSink));

    ASSERT_TRUE(deepSource && proof && imageSink);

    connectNodes(deepSource, proof, 0, true);
    EXPECT_EQ(eDataKindDeep, proof->getEffectiveOutputDataKind());

    NodePtr conflictingNode;
    Node::CanConnectInputReturnValue ret = imageSink->canConnectInput(proof, 0, &conflictingNode);

    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, ret);
    EXPECT_EQ(proof.get(), conflictingNode.get());
}

// Renders SeNoise -> WriteOIIO twice, once directly and once with TypedPassthrough spliced in
// between generator and writer, and asserts the two renders are pixel-for-pixel identical.
// This is the substantive check that inserting the proof node mid-chain does not alter the
// rendered result: NativeEffectBase gives TypedPassthrough no render() override of its own, so
// this exercises the isIdentity() route (the same one Dot/NoOpBase use), not a pixel copy.
TEST_F(BaseTest, TypedPassthroughMidChainRendersIdentically)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const std::string directPath = (tmp.path() + QLatin1String("/direct.exr")).toStdString();
    const std::string throughProofPath = (tmp.path() + QLatin1String("/through_proof.exr")).toStdString();

    Format f(0, 0, 16, 16, "typedPassthroughRenderFormat", 1.);
    getApp()->getProject()->setOrAddProjectFormat(f);

    // Render 1: generator -> writer directly.
    {
        NodePtr generator = createNode(_generatorPluginID);
        NodePtr writer = createNode(_writeOIIOPluginID);
        ASSERT_TRUE(generator && writer);

        KnobChoice* bitDepth = dynamic_cast<KnobChoice*>(writer->getKnobByName("bitDepth").get());
        ASSERT_TRUE(bitDepth != NULL);
        bitDepth->setValueFromID("32f", 0);

        KnobChoice* compression = dynamic_cast<KnobChoice*>(writer->getKnobByName("compression").get());
        ASSERT_TRUE(compression != NULL);
        compression->setValueFromID("none", 0);

        writer->setOutputFilesForWriter(directPath);
        connectNodes(generator, writer, 0, true);

        OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>(writer->getEffectInstance().get());
        ASSERT_TRUE(writerEffect != NULL);

        std::list<AppInstance::RenderWork> works;
        works.push_back(AppInstance::RenderWork(writerEffect, 1, 1, 1, false));
        getApp()->startWritersRendering(false, works);
    }

    // Render 2: generator -> TypedPassthrough -> writer.
    {
        NodePtr generator = createNode(_generatorPluginID);
        NodePtr proof = createNode(QString::fromUtf8(PLUGINID_NATRON_TYPEDPASSTHROUGH));
        NodePtr writer = createNode(_writeOIIOPluginID);
        ASSERT_TRUE(generator && proof && writer);

        KnobChoice* bitDepth = dynamic_cast<KnobChoice*>(writer->getKnobByName("bitDepth").get());
        ASSERT_TRUE(bitDepth != NULL);
        bitDepth->setValueFromID("32f", 0);

        KnobChoice* compression = dynamic_cast<KnobChoice*>(writer->getKnobByName("compression").get());
        ASSERT_TRUE(compression != NULL);
        compression->setValueFromID("none", 0);

        writer->setOutputFilesForWriter(throughProofPath);
        connectNodes(generator, proof, 0, true);
        connectNodes(proof, writer, 0, true);

        OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>(writer->getEffectInstance().get());
        ASSERT_TRUE(writerEffect != NULL);

        std::list<AppInstance::RenderWork> works;
        works.push_back(AppInstance::RenderWork(writerEffect, 1, 1, 1, false));
        getApp()->startWritersRendering(false, works);
    }

    ASSERT_TRUE(QFile::exists(QString::fromStdString(directPath)));
    ASSERT_TRUE(QFile::exists(QString::fromStdString(throughProofPath)));

    static const char* const kChannels[] = { "R", "G", "B", "A" };
    for (size_t c = 0; c < sizeof(kChannels) / sizeof(kChannels[0]); ++c) {
        std::vector<float> direct, throughProof;
        int directWidth = 0, directHeight = 0, throughProofWidth = 0, throughProofHeight = 0;
        std::string error;

        ASSERT_TRUE(readExrChannelPlane(directPath, kChannels[c], &direct, &directWidth, &directHeight, &error)) << error;
        ASSERT_TRUE(readExrChannelPlane(throughProofPath, kChannels[c], &throughProof, &throughProofWidth, &throughProofHeight, &error)) << error;

        ASSERT_EQ(directWidth, throughProofWidth);
        ASSERT_EQ(directHeight, throughProofHeight);
        ASSERT_EQ(direct.size(), throughProof.size());

        for (size_t i = 0; i < direct.size(); ++i) {
            EXPECT_EQ(direct[i], throughProof[i]) << "channel " << kChannels[c] << " pixel " << i << " differs";
        }

        if (std::string(kChannels[c]) == "R") {
            std::set<float> distinctValues(direct.begin(), direct.end());
            EXPECT_GT(distinctValues.size(), (size_t)1) << "generator produced a degenerate (uniform) image; this test would pass vacuously";
        }
    }

    QFile::remove(QString::fromStdString(directPath));
    QFile::remove(QString::fromStdString(throughProofPath));
} // TEST_F(BaseTest, TypedPassthroughMidChainRendersIdentically)
