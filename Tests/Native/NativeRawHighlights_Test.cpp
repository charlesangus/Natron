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
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QString>

#include <ofxImageEffect.h>
#include <ofxNatron.h>

#include "BaseTest.h"
#include "NativePluginIdentity.h"
#include "RawTestSource.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Raw/RawHighlights.h"
#include "Engine/Nodes/Raw/RawPrepare.h"
#include "Engine/Nodes/Raw/RawTemperature.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const int kWidth = 48;
const int kHeight = 42;
const float kCoeffs[3] = { 2.f, 1.f, 1.5f };

const char* const kXTransPattern = "GGRGGB"
                                   "GGBGGR"
                                   "BRGRBG"
                                   "GGBGGR"
                                   "GGRGGB"
                                   "RBGBRG";

/**
 * @brief A white-balanced mosaic: a smooth scene at 0.3 to 0.5 of the white level times each
 * colour's multiplier, and a disc where the sensor clipped, every photosite at the white level
 * times its multiplier, with a rim where only green clipped.
 **/
RawTestSource::ValueFn
clippedScene(const RawLayout& layout)
{
    return [layout](int x, int y, int) {
        const int colour = layout.colourAt(x, y);
        const float d = std::hypot((float)x - 24.f, (float)y - 20.f);
        float raw = 0.3f + 0.2f * (float)x / (float)kWidth;
        if (d < 8.f) {
            raw = 1.f;
        } else if ((d < 11.f) && (colour == 1)) {
            raw = 1.f;
        }

        return raw * kCoeffs[colour];
    };
}

ImageMetadata
whiteBalancedMetadata(const std::string& pattern)
{
    ImageMetadata metadata = rawTestMetadata(pattern, 0, 0, eRawStagePrepared);

    RawMetadata::setWhiteLevel(&metadata, 1.);
    RawMetadata::setWbApplied(&metadata, { kCoeffs[0], kCoeffs[1], kCoeffs[2] });
    RawMetadata::setProcessedMaximum(&metadata, { kCoeffs[0], kCoeffs[1], kCoeffs[2] });

    return metadata;
}

// Windows a tile render may ask for: inside, on the image's edges, and single pixels.
const RectI kTiles[] = {
    RectI(0, 0, kWidth, kHeight),
    RectI(10, 9, 31, 28),
    RectI(0, 0, 7, 5),
    RectI(kWidth - 9, kHeight - 4, kWidth, kHeight),
    RectI(24, 20, 25, 21),
    RectI(17, 13, 18, 14),
    RectI(30, 0, 48, 13),
};
bool
readReferenceMosaic(const std::string& path,
                    std::vector<float>* pixels,
                    int* width,
                    int* height,
                    std::string* error)
{
    OIIO::ImageInput::unique_ptr input = OIIO::ImageInput::open(path);

    if (!input) {
        *error = "cannot open " + path + ": " + OIIO::geterror();

        return false;
    }
    const OIIO::ImageSpec spec = input->spec();
    pixels->resize((std::size_t)spec.width * spec.height);
    const bool ok = (spec.nchannels == 1) && input->read_image(0, 0, 0, 1, OIIO::TypeDesc::FLOAT, pixels->data());
    input->close();
    if (!ok) {
        *error = "cannot read " + path + " as one float channel";

        return false;
    }
    *width = spec.width;
    *height = spec.height;

    return true;
}
} // namespace

class NativeRawHighlightsTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
    }

    NodePtr createChain(const std::string& pattern,
                        RawHighlightsModeEnum mode,
                        NodePtr* sourceOut = nullptr)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWHIGHLIGHTS));

        EXPECT_TRUE(source && node);
        if (!source || !node) {
            return NodePtr();
        }
        const ImageMetadata metadata = whiteBalancedMetadata(pattern);
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(RectI(0, 0, kWidth, kHeight), 1, clippedScene(*RawLayout::fromMetadata(metadata)), metadata);
        connectNodes(source, node, 0, true);
        node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamMode)->setValue((int)mode);
        if (sourceOut) {
            *sourceOut = source;
        }

        return node;
    }

    // Every tile of kTiles renders the same pixels as the whole frame.
    void expectTilesMatchTheFullFrame(const NodePtr& node,
                                      const char* what)
    {
        RenderedPlane full;
        std::string error;
        ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &full, &error)) << what << ": " << error;
        for (const RectI& tile : kTiles) {
            RenderedPlane plane;
            ASSERT_TRUE(renderRawTestPlane(node, tile, &plane, &error)) << what << ": " << error;
            for (int y = tile.y1; y < tile.y2; ++y) {
                for (int x = tile.x1; x < tile.x2; ++x) {
                    ASSERT_EQ(rawTestPlaneValue(full, x, y, 0), rawTestPlaneValue(plane, x, y, 0)) << what << " at " << x << "," << y << " in tile " << tile.x1 << "," << tile.y1 << "-" << tile.x2 << "," << tile.y2;
                }
            }
        }
    }
};

TEST_F(NativeRawHighlightsTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_RAWHIGHLIGHTS, PLUGIN_MAJOR_NATRON_RAWHIGHLIGHTS, "RawHighlights");

    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWHIGHLIGHTS));
    ASSERT_TRUE(bool(node));
    EXPECT_EQ(eDataKindRaw, node->getEffectInstance()->getInputDataKind(0));
    EXPECT_EQ(eDataKindRaw, node->getEffectInstance()->getOutputDataKind());
}

// Clip mode clips every photosite to the threshold times the white level, whatever its colour,
// which turns the clipped disc white once demosaiced.
TEST_F(NativeRawHighlightsTest, ClipModeClipsToTheThreshold)
{
    NodePtr node = createChain("RGGB", eRawHighlightsModeClip);
    ASSERT_TRUE(bool(node));
    const RawTestSource::ValueFn scene = clippedScene(*RawLayout::fromPattern("RGGB"));

    node->getEffectInstance()->getKnobByNameAndType<KnobBool>(kRawHighlightsParamClipOverride)->setValue(true);
    for (double threshold : { 1., 0.8 }) {
        node->getEffectInstance()->getKnobByNameAndType<KnobDouble>(kRawHighlightsParamClip)->setValue(threshold);
        RenderedPlane plane;
        std::string error;
        ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error)) << error;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                ASSERT_EQ(std::fmin(scene(x, y, 0), (float)threshold), rawTestPlaneValue(plane, x, y, 0)) << x << "," << y << " at threshold " << threshold;
            }
        }
    }
}

TEST_F(NativeRawHighlightsTest, UnclippedLeavesTheData)
{
    NodePtr node = createChain("RGGB", eRawHighlightsModeUnclipped);
    ASSERT_TRUE(bool(node));
    const RawTestSource::ValueFn scene = clippedScene(*RawLayout::fromPattern("RGGB"));

    RenderedPlane plane;
    std::string error;
    ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error)) << error;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            ASSERT_EQ(scene(x, y, 0), rawTestPlaneValue(plane, x, y, 0)) << x << "," << y;
        }
    }
}

// Inpaint Opposed only ever raises a photosite at or above its colour's clipping level, and
// leaves every other one alone; its chrominance is the whole image's, so tiles agree.
TEST_F(NativeRawHighlightsTest, InpaintOpposedOnlyRaisesClippedPhotosites)
{
    for (const char* pattern : { "RGGB", kXTransPattern }) {
        NodePtr node = createChain(pattern, eRawHighlightsModeOpposed);
        ASSERT_TRUE(bool(node));
        const RawLayout layout = *RawLayout::fromPattern(pattern);
        const RawTestSource::ValueFn scene = clippedScene(layout);

        RenderedPlane plane;
        std::string error;
        ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error)) << error;
        int raised = 0;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const float in = scene(x, y, 0);
                const float out = rawTestPlaneValue(plane, x, y, 0);
                const float clip = 0.987f * kCoeffs[layout.colourAt(x, y)];
                if (in < clip) {
                    ASSERT_EQ(in, out) << pattern << " " << x << "," << y;
                } else {
                    ASSERT_GE(out, in) << pattern << " " << x << "," << y;
                    raised += (out > in) ? 1 : 0;
                }
            }
        }
        EXPECT_GT(raised, 0) << pattern;
        expectTilesMatchTheFullFrame(node, pattern);
    }
}

TEST_F(NativeRawHighlightsTest, LChTilesMatchTheFullFrame)
{
    for (const char* pattern : { "GBRG", kXTransPattern }) {
        NodePtr node = createChain(pattern, eRawHighlightsModeLCh);
        ASSERT_TRUE(bool(node));
        const RawTestSource::ValueFn scene = clippedScene(*RawLayout::fromPattern(pattern));

        RenderedPlane plane;
        std::string error;
        ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error)) << error;
        // Far from the disc nothing clips, so nothing changes.
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                ASSERT_EQ(scene(x, y, 0), rawTestPlaneValue(plane, x, y, 0)) << pattern << " " << x << "," << y;
            }
        }
        expectTilesMatchTheFullFrame(node, pattern);
    }
}

TEST_F(NativeRawHighlightsTest, ClipModeOnLinearData)
{
    NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWHIGHLIGHTS));
    ASSERT_TRUE(source && node);
    const RawTestSource::ValueFn linear = [](int x, int y, int c) {
        return 0.1f * (float)(c + 1) + 0.04f * (float)x + 0.01f * (float)y;
    };
    dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(RectI(0, 0, kWidth, kHeight), 3, linear, rawTestMetadata(kRawCfaPatternNone, 0, 0, eRawStagePrepared));
    connectNodes(source, node, 0, true);
    node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamMode)->setValue((int)eRawHighlightsModeClip);

    RenderedPlane plane;
    std::string error;
    ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error)) << error;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            for (int c = 0; c < 3; ++c) {
                ASSERT_EQ(std::fmin(linear(x, y, c), 1.f), rawTestPlaneValue(plane, x, y, c)) << x << "," << y << " channel " << c;
            }
        }
    }
}

// At 26x26 the 3x3 block map has no rounding slack, and its last row and column of photosites
// map one block past it: the reads there must stay inside the mask.
TEST_F(NativeRawHighlightsTest, InpaintOpposedOnASizeWithoutMaskPadding)
{
    for (const char* pattern : { "RGGB", kXTransPattern }) {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWHIGHLIGHTS));
        ASSERT_TRUE(source && node);
        const ImageMetadata metadata = whiteBalancedMetadata(pattern);
        const RawLayout layout = *RawLayout::fromMetadata(metadata);
        const RawTestSource::ValueFn scene = [layout](int x, int y, int) {
            const int colour = layout.colourAt(x, y);
            const float raw = (std::hypot((float)x - 13.f, (float)y - 13.f) < 5.f) ? 1.f : 0.4f + 0.01f * (float)x;

            return raw * kCoeffs[colour];
        };
        const RectI rod(0, 0, 26, 26);
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(rod, 1, scene, metadata);
        connectNodes(source, node, 0, true);
        node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamMode)->setValue((int)eRawHighlightsModeOpposed);

        RenderedPlane plane;
        std::string error;
        ASSERT_TRUE(renderRawTestPlane(node, rod, &plane, &error)) << pattern << ": " << error;
        for (int y = rod.y1; y < rod.y2; ++y) {
            for (int x = rod.x1; x < rod.x2; ++x) {
                ASSERT_GE(rawTestPlaneValue(plane, x, y, 0), scene(x, y, 0)) << pattern << " " << x << "," << y;
            }
        }
    }
}

// darktable's highlights module on the synthetic clipped mosaics tools/raw/make-reference.sh
// writes as DNGs, with each mode's result read back through its photosite-colour demosaic. The
// same DNG goes through the native Read, RawPrepare and RawTemperature here; see
// Tests/fixtures/raw/README.md for the tolerances.
TEST_F(NativeRawHighlightsTest, MatchesTheDarktableReference)
{
    struct ModeCase {
        RawHighlightsModeEnum mode;
        const char* name;
        double tolerance;
    };
    const ModeCase modes[] = {
        { eRawHighlightsModeClip, "clip", 1e-5 },
        { eRawHighlightsModeUnclipped, "unclipped", 1e-5 },
        { eRawHighlightsModeOpposed, "opposed", 2e-3 },
        { eRawHighlightsModeLCh, "lch", 1e-3 },
    };
    const std::string dir = std::string(NATRON_TESTS_FIXTURES_DIR "/raw/reference/");

    for (const char* layout : { "bayer", "xtrans" }) {
        for (const ModeCase& m : modes) {
            const std::string what = std::string(layout) + " " + m.name;
            std::vector<float> reference;
            int refWidth = 0;
            int refHeight = 0;
            std::string error;
            ASSERT_TRUE(readReferenceMosaic(dir + "highlights-" + layout + "-" + m.name + ".tif", &reference, &refWidth, &refHeight, &error)) << what << ": " << error;

            NodePtr read = createNode(QString::fromUtf8(PLUGINID_NATRON_READ));
            ASSERT_TRUE(bool(read));
            read->getEffectInstance()->getKnobByNameAndType<KnobFile>(kOfxImageEffectFileParamName)->setValue(dir + "highlights-" + layout + ".dng");
            NodePtr prepare = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWPREPARE));
            NodePtr temperature = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWTEMPERATURE));
            NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWHIGHLIGHTS));
            ASSERT_TRUE(prepare && temperature && node);
            connectNodes(read, prepare, 0, true);
            connectNodes(prepare, temperature, 0, true);
            connectNodes(temperature, node, 0, true);
            node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamMode)->setValue((int)m.mode);

            const RectI window(0, 0, refWidth, refHeight);
            RenderedPlane plane;
            ASSERT_TRUE(renderRawTestPlane(node, window, &plane, &error)) << what << ": " << error;
            double worst = 0.;
            double sum = 0.;
            for (int row = 0; row < refHeight; ++row) {
                for (int x = 0; x < refWidth; ++x) {
                    // Natron's rows count from the bottom.
                    const int y = refHeight - 1 - row;
                    const double expected = reference[(std::size_t)row * refWidth + x];
                    const double diff = std::fabs((double)rawTestPlaneValue(plane, x, y, 0) - expected);
                    sum += diff;
                    worst = std::max(worst, diff);
                    EXPECT_LE(diff, m.tolerance) << what << " at " << x << "," << y << ": darktable " << expected << ", Natron " << rawTestPlaneValue(plane, x, y, 0);
                }
            }
            std::printf("highlights %s against darktable: mean error %g, max error %g\n", what.c_str(), sum / ((double)refWidth * refHeight), worst);
            getApp()->getProject()->reset(false, true);
        }
    }
}

TEST_F(NativeRawHighlightsTest, UnpreparedDataRaisesTheStageError)
{
    NodePtr source;
    NodePtr node = createChain("RGGB", eRawHighlightsModeClip, &source);
    ASSERT_TRUE(source && node);
    const ImageMetadata metadata = rawTestMetadata("RGGB", 0, 0, eRawStageRaw);
    dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setMetadata(metadata);

    RenderedPlane plane;
    std::string error;
    EXPECT_FALSE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error));
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_EQ(QString::fromUtf8(RawMetadata::requireStage(metadata, eRawStagePrepared, node->getLabel()).value_or(std::string()).c_str()), message);
}

// Each mode's whole-frame result is kept across renders of the frame, and Clear Caches drops it,
// so the next render computes it again.
TEST_F(NativeRawHighlightsTest, ClearCachesDropsTheWholeFrameResults)
{
    struct Case {
        RawHighlightsModeEnum mode;
        std::vector<const char*> frames;
    };
    const Case cases[] = {
        { eRawHighlightsModeOpposed, { kRawHighlightsFrameOpposedChroma } },
        { eRawHighlightsModeInpaint, { kRawHighlightsFrameReconstructColor } },
        { eRawHighlightsModeSegments, { kRawHighlightsFrameSegments, kRawHighlightsFrameOpposedChroma } },
        { eRawHighlightsModeLaplacian, { kRawHighlightsFrameLaplacian } },
    };
    for (const Case& k : cases) {
        NodePtr node = createChain("RGGB", k.mode);
        ASSERT_TRUE(bool(node)) << k.mode;
        const FrameComputedCounter computed;
        // Emptying the node cache makes each window render, and leaves the node's own results.
        const auto renderWindow = [&node, &k](const RectI& window) {
            appPTR->clearNodeCache();
            RenderedPlane plane;
            std::string error;
            EXPECT_TRUE(renderRawTestPlane(node, window, &plane, &error, false)) << k.mode << ": " << error;
        };
        renderWindow(RectI(0, 0, 7, 5));
        renderWindow(RectI(30, 0, 48, 13));
        for (const char* what : k.frames) {
            EXPECT_EQ(1, computed.count(what)) << k.mode << ": " << what << " before Clear Caches";
        }
        node->purgeAllInstancesCaches();
        renderWindow(RectI(10, 9, 31, 28));
        for (const char* what : k.frames) {
            EXPECT_EQ(2, computed.count(what)) << k.mode << ": " << what << " after Clear Caches";
        }
        EXPECT_EQ(k.frames.size(), computed.counts().size()) << k.mode;
    }
}
