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

#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <SequenceParsing.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

struct ExrChannelInfo
{
    std::string name;
    int32_t pixelType;
};

bool
readExrCString(std::ifstream& in,
               std::string* out)
{
    out->clear();
    char c;
    while ( in.get(c) ) {
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

// WriteOIIO is configured below with bitDepth=32f and compression=none, and the OCIO
// input/output colorspaces both resolve to scene_linear for that bit depth (see
// WriteOIIOPlugin::onOutputFileChanged), so the written value is the keyframed value
// with no gamma or premult surprises. That makes it possible to check pixel values with
// a small hand-rolled reader for this one, fully-specified layout (single-part, scanline,
// uncompressed, 32-bit float) instead of linking OpenImageIO/OpenEXR into the Tests binary,
// neither of which is otherwise part of this build.
bool
readExrFirstPixelRed(const std::string& path,
                     float* outValue,
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
    if ( !in || std::memcmp(magic, kMagic, 4) != 0 ) {
        *error = "bad magic number";

        return false;
    }

    int32_t version = 0;
    if ( !readExrPod(in, &version) ) {
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
        if ( !readExrCString(in, &name) ) {
            *error = "truncated header (attribute name)";

            return false;
        }
        if ( name.empty() ) {
            break;
        }
        std::string type;
        if ( !readExrCString(in, &type) ) {
            *error = "truncated header (attribute type)";

            return false;
        }
        int32_t size = 0;
        if ( !readExrPod(in, &size) || (size < 0) ) {
            *error = "truncated header (attribute size)";

            return false;
        }
        std::vector<char> data(size);
        if ( (size > 0) && !in.read(data.data(), size) ) {
            *error = "truncated attribute data for " + name;

            return false;
        }

        if (name == "channels") {
            size_t pos = 0;
            while ( pos < data.size() ) {
                std::string cname;
                while ( (pos < data.size()) && (data[pos] != '\0') ) {
                    cname.push_back(data[pos]);
                    ++pos;
                }
                if ( pos >= data.size() ) {
                    break;
                }
                ++pos; // the channel name's null terminator
                if ( cname.empty() ) {
                    break; // empty name terminates the channel list
                }
                if (pos + 16 > data.size()) {
                    *error = "malformed channel list entry for " + cname;

                    return false;
                }
                int32_t pixelType = 0;
                std::memcpy(&pixelType, &data[pos], 4);
                pos += 16; // pixelType(4) + pLinear/reserved(4) + xSampling(4) + ySampling(4)
                channels.push_back(ExrChannelInfo{ cname, pixelType });
            }
        } else if ( (name == "compression") && (size >= 1) ) {
            compression = static_cast<unsigned char>(data[0]);
        } else if ( (name == "dataWindow") && (size >= 16) ) {
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

    int rChannelIndex = -1;
    std::string foundNames;
    for (size_t i = 0; i < channels.size(); ++i) {
        if ( !foundNames.empty() ) {
            foundNames += ",";
        }
        foundNames += channels[i].name;
        if ( (rChannelIndex < 0) && (channels[i].name == "R") ) {
            rChannelIndex = static_cast<int>(i);
        }
    }
    if (rChannelIndex < 0) {
        *error = "no R channel found, channels were: " + foundNames;

        return false;
    }
    if (channels[rChannelIndex].pixelType != 2 /* FLOAT, per the OpenEXR spec */) {
        *error = "R channel is not 32-bit float (pixelType=" + std::to_string(channels[rChannelIndex].pixelType) + ")";

        return false;
    }

    int64_t firstOffset = 0;
    if ( !readExrPod(in, &firstOffset) ) {
        *error = "cannot read the scanline offset table";

        return false;
    }

    in.seekg(firstOffset, std::ios::beg);
    int32_t y = 0;
    int32_t chunkDataSize = 0;
    if ( !readExrPod(in, &y) || !readExrPod(in, &chunkDataSize) || (chunkDataSize < 0) ) {
        *error = "cannot read the scanline chunk header";

        return false;
    }

    const int width = dataWindow[2] - dataWindow[0] + 1;
    size_t byteOffset = 0;
    for (int i = 0; i < rChannelIndex; ++i) {
        const int bytesPerSample = (channels[i].pixelType == 1 /* HALF */) ? 2 : 4;
        byteOffset += static_cast<size_t>(width) * bytesPerSample;
    }

    std::vector<char> chunk(chunkDataSize);
    if ( !in.read(chunk.data(), chunkDataSize) ) {
        *error = "truncated scanline pixel data";

        return false;
    }
    if (byteOffset + 4 > chunk.size()) {
        *error = "scanline data too small to hold the R channel";

        return false;
    }

    float value = 0.f;
    std::memcpy(&value, &chunk[byteOffset], 4);
    *outValue = value;

    return true;
} // readExrFirstPixelRed

} // namespace

// Renders Constant -> WriteOIIO over a frame range with the Constant's colour keyframed to
// a distinct, exactly-known value per frame, then reads every rendered frame back and checks
// its pixel against the value keyframed for that frame. This catches both a scheduler that
// silently stops dispatching frames (a missing file) and a writer that numbers or overwrites
// frames wrongly (two frames carrying the same value), and it is exact rather than
// approximate because the source image is synthesized at stated values rather than sampled
// from a generator whose output can only be checked approximately.
//
// It does not exercise reader-side frame mapping: there is no reader in this graph.
TEST_F(BaseTest, RenderFrameRangeProducesDistinctFramePixels)
{
    NodePtr generator = createNode( QString::fromUtf8(PLUGINID_OFX_CONSTANT) );
    NodePtr writer = createNode(_writeOIIOPluginID);
    ASSERT_TRUE( bool(generator) && bool(writer) );

    connectNodes(generator, writer, 0, true);

    Format f(0, 0, 8, 8, "renderRangeFormat", 1.);
    generator->getApp()->getProject()->setOrAddProjectFormat(f);

    KnobColor* color = dynamic_cast<KnobColor*>( generator->getKnobByName("color").get() );
    ASSERT_TRUE(color != NULL);

    const int firstFrame = 1;
    const int lastFrame = 5;
    std::map<int, double> valueForFrame;
    for (int frame = firstFrame; frame <= lastFrame; ++frame) {
        const double value = frame * 0.1;
        valueForFrame[frame] = value;
        for (int dimension = 0; dimension < 3; ++dimension) {
            color->setValueAtTime(frame, value, ViewSpec::all(), dimension);
        }
        color->setValueAtTime(frame, 1., ViewSpec::all(), 3); // alpha=1: keeps premult/unpremult a no-op
    }

    KnobChoice* bitDepth = dynamic_cast<KnobChoice*>( writer->getKnobByName("bitDepth").get() );
    ASSERT_TRUE(bitDepth != NULL);
    bitDepth->setValueFromID("32f", 0);

    KnobChoice* compression = dynamic_cast<KnobChoice*>( writer->getKnobByName("compression").get() );
    ASSERT_TRUE(compression != NULL);
    compression->setValueFromID("none", 0);

    QTemporaryDir tmp;
    ASSERT_TRUE( tmp.isValid() );
    const std::string pattern = (tmp.path() + QLatin1String("/render.####.exr")).toStdString();
    writer->setOutputFilesForWriter(pattern);

    OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>( writer->getEffectInstance().get() );
    ASSERT_TRUE(writerEffect != NULL);

    std::list<AppInstance::RenderWork> works;
    works.push_back( AppInstance::RenderWork(writerEffect, firstFrame, lastFrame, 1, false) );
    getApp()->startWritersRendering(false, works);

    const std::vector<std::string>& viewNames = getApp()->getProject()->getProjectViewNames();

    std::set<float> observedValues;
    for (int frame = firstFrame; frame <= lastFrame; ++frame) {
        const std::string path = SequenceParsing::generateFileNameFromPattern(pattern, viewNames, frame, 0);
        const QString qPath = QString::fromStdString(path);
        ASSERT_TRUE( QFile::exists(qPath) ) << "frame " << frame << " was not rendered: " << path;

        float pixel = 0.f;
        std::string error;
        ASSERT_TRUE( readExrFirstPixelRed(path, &pixel, &error) ) << "frame " << frame << ": " << error;

        EXPECT_NEAR(valueForFrame[frame], pixel, 1e-4) << "frame " << frame << " has the wrong pixel value";
        EXPECT_TRUE( observedValues.insert(pixel).second )
            << "frame " << frame << " carries a pixel value (" << pixel << ") already seen on an earlier frame";

        QFile::remove(qPath);
    }
} // TEST_F(BaseTest, RenderFrameRangeProducesDistinctFramePixels)
