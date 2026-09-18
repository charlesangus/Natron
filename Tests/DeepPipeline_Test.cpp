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
#include <cstddef>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <SequenceParsing.h>

#include "BaseTest.h"
#include "FlatExrReader.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Deep/DeepMerge.h"
#include "Engine/Nodes/Deep/DeepRead.h"
#include "Engine/Nodes/Deep/DeepRecolor.h"
#include "Engine/Nodes/Deep/DeepToImage.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

// The two deep inputs, Tests/fixtures/deep-scanline.exr and deep-interleaved.exr, as
// Tests/fixtures/make-deep-fixtures.py wrote them, in Natron's bottom-up pixel coordinates (the
// script's rows are in file order, so its row 0 is y == 2 here). Only each sample's alpha and
// depth extent are listed: DeepRecolor overwrites every sample's R, G and B on the way through,
// so nothing the files hold in those channels can reach the output. The full contents are
// transcribed in DeepReadWrite_Test.cpp and in the script's INTERLEAVED table.
//
// deep-interleaved.exr was laid out so that within any one pixel none of its depth ranges
// overlaps one of deep-scanline.exr's, and the one pixel where deep-scanline.exr overlaps
// itself, (3, 2), is led by an opaque point sample at Z=1 that hides everything behind it.
// The merged pixel therefore flattens as a plain front-to-back "over" of the union of the two
// files' samples, which is what expectedOutputPixel() below computes -- with no call into the
// engine's tidying or flattening code.
#define kDeepFixtureWidth 4
#define kDeepFixtureHeight 3

struct FixtureSample {
    float alpha;
    float z;
    float zback;
};

struct FixturePixel {
    int x;
    int y;
    int count;
    FixtureSample samples[3];
};

const FixturePixel kScanlinePixels[] = {
    { 0, 2, 0, { { 0.f, 0.f, 0.f } } },
    { 1, 2, 1, { { 1.f, 3.f, 3.5f } } },
    { 2, 2, 2, { { 0.5f, 5.f, 5.25f }, { 0.25f, 2.f, 2.5f } } },
    { 3, 2, 3, { { 1.f, 1.f, 1.f }, { 0.5f, 4.f, 6.f }, { 0.75f, 5.5f, 5.5f } } },
    { 0, 1, 2, { { 0.5f, 7.f, 7.25f }, { 1.f, 8.f, 8.25f } } },
    { 1, 1, 0, { { 0.f, 0.f, 0.f } } },
    { 2, 1, 1, { { 0.25f, 9.f, 9.f } } },
    { 3, 1, 1, { { 0.125f, 10.f, 12.f } } },
    { 0, 0, 1, { { 0.5f, 13.f, 13.f } } },
    { 1, 0, 2, { { 0.75f, 14.f, 14.5f }, { 1.f, 15.f, 15.5f } } },
    { 2, 0, 0, { { 0.f, 0.f, 0.f } } },
    { 3, 0, 2, { { 1.f, 16.f, 16.25f }, { 0.5f, 17.f, 17.5f } } },
};

const FixturePixel kInterleavedPixels[] = {
    { 0, 2, 1, { { 0.4f, 20.f, 20.f } } },
    { 1, 2, 1, { { 0.375f, 1.f, 2.f } } },
    { 2, 2, 2, { { 0.5f, 3.f, 3.f }, { 0.5f, 10.f, 10.f } } },
    { 3, 2, 1, { { 0.5f, 3.f, 3.f } } },
    { 0, 1, 1, { { 0.25f, 6.f, 6.f } } },
    { 1, 1, 1, { { 0.75f, 3.f, 4.f } } },
    { 2, 1, 2, { { 0.25f, 12.f, 12.f }, { 0.5f, 8.f, 8.f } } },
    { 3, 1, 1, { { 0.5f, 9.f, 9.f } } },
    { 0, 0, 1, { { 0.5f, 14.f, 14.f } } },
    { 1, 0, 1, { { 0.25f, 13.f, 13.f } } },
    { 2, 0, 0, { { 0.f, 0.f, 0.f } } },
    { 3, 0, 1, { { 0.5f, 15.f, 15.f } } },
};

// The Constant feeding DeepRecolor's Color input. Three distinct channel values, so a channel
// swap anywhere in the chain shows; alpha 1, so the Constant's own premultiplication state is
// moot and DeepRecolor's unpremultiplied colour is the colour itself.
const float kColorR = 0.5f;
const float kColorG = 0.25f;
const float kColorB = 0.125f;
const float kColorA = 1.f;

void
appendFixtureSamples(const FixturePixel* pixels,
                     std::size_t pixelCount,
                     int x,
                     int y,
                     std::vector<FixtureSample>* out)
{
    for (std::size_t p = 0; p < pixelCount; ++p) {
        if ((pixels[p].x == x) && (pixels[p].y == y)) {
            out->insert(out->end(), pixels[p].samples, pixels[p].samples + pixels[p].count);
        }
    }
}

bool
frontToBack(const FixtureSample& a,
            const FixtureSample& b)
{
    return a.z < b.z;
}

// What DeepRead x2 -> DeepMerge (combine) -> DeepRecolor -> DeepToImage should produce at (x, y):
// the union of both files' samples, in depth order, each carrying the Constant's unpremultiplied
// colour scaled to its own alpha, composited front-to-back into a premultiplied RGBA pixel.
void
expectedOutputPixel(int x,
                    int y,
                    float* rgba)
{
    std::vector<FixtureSample> samples;

    appendFixtureSamples(kScanlinePixels, sizeof(kScanlinePixels) / sizeof(kScanlinePixels[0]), x, y, &samples);
    appendFixtureSamples(kInterleavedPixels, sizeof(kInterleavedPixels) / sizeof(kInterleavedPixels[0]), x, y, &samples);
    std::stable_sort(samples.begin(), samples.end(), &frontToBack);

    const float color[3] = { kColorR / kColorA, kColorG / kColorA, kColorB / kColorA };
    float transmittance = 1.f;
    for (int c = 0; c < 4; ++c) {
        rgba[c] = 0.f;
    }
    for (std::size_t s = 0; s < samples.size(); ++s) {
        const float alpha = samples[s].alpha;
        for (int c = 0; c < 3; ++c) {
            rgba[c] += transmittance * (color[c] * alpha);
        }
        rgba[3] += transmittance * alpha;
        transmittance *= 1.f - alpha;
    }
}

QString
fixturePath(const char* name)
{
    return QString::fromUtf8(NATRON_TESTS_FIXTURES_DIR "/") + QString::fromUtf8(name);
}

} // namespace

class DeepPipelineTest
    : public BaseTest {
protected:
    NodePtr createDeepRead(const QString& filename)
    {
        NodePtr read = createNode(QString::fromUtf8(PLUGINID_NATRON_DEEPREAD));

        if (!read) {
            return read;
        }
        KnobFile* knob = dynamic_cast<KnobFile*>(read->getKnobByName("filename").get());
        if (!knob) {
            return NodePtr();
        }
        knob->setValue(filename.toStdString());

        return read;
    }
};

// The deep chain end to end, the way a user's render runs it: DeepRead x2 -> DeepMerge ->
// DeepRecolor (Color: Constant) -> DeepToImage -> WriteOIIO, dispatched through the render
// scheduler as a writer render rather than pulled by hand, and checked on what the writer put on
// disk. Every pixel of the frame is compared against the composite computed independently from
// the two fixtures' documented samples, at a tolerance a wrong merge or flatten cannot hide
// under.
TEST_F(DeepPipelineTest, DeepReadMergeRecolorToImageWriteMatchesTheSerialComposite)
{
    NodePtr readA = createDeepRead(fixturePath("deep-scanline.exr"));
    NodePtr readB = createDeepRead(fixturePath("deep-interleaved.exr"));
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_DEEPMERGE));
    NodePtr constant = createNode(QString::fromUtf8(PLUGINID_OFX_CONSTANT));
    NodePtr recolor = createNode(QString::fromUtf8(PLUGINID_NATRON_DEEPRECOLOR));
    NodePtr toImage = createNode(QString::fromUtf8(PLUGINID_NATRON_DEEPTOIMAGE));
    NodePtr writer = createNode(_writeOIIOPluginID);

    ASSERT_TRUE(readA && readB && merge && constant && recolor && toImage && writer);

    Format format(0, 0, kDeepFixtureWidth, kDeepFixtureHeight, "deepPipelineFormat", 1.);
    getApp()->getProject()->setOrAddProjectFormat(format);

    {
        KnobChoice* operation = dynamic_cast<KnobChoice*>(merge->getKnobByName("operation").get());
        ASSERT_TRUE(operation != NULL);
        operation->setValue((int)DeepMerge::eOperationCombine);

        KnobBool* targetInputAlpha = dynamic_cast<KnobBool*>(recolor->getKnobByName("targetInputAlpha").get());
        ASSERT_TRUE(targetInputAlpha != NULL);
        targetInputAlpha->setValue(false);

        KnobColor* color = dynamic_cast<KnobColor*>(constant->getKnobByName("color").get());
        ASSERT_TRUE(color != NULL);
        color->setValue(kColorR, ViewSpec::all(), 0);
        color->setValue(kColorG, ViewSpec::all(), 1);
        color->setValue(kColorB, ViewSpec::all(), 2);
        color->setValue(kColorA, ViewSpec::all(), 3);
    }

    connectNodes(readA, merge, 0, true);
    connectNodes(readB, merge, 1, true);
    connectNodes(merge, recolor, 0, true);
    connectNodes(constant, recolor, 1, true);
    connectNodes(recolor, toImage, 0, true);
    connectNodes(toImage, writer, 0, true);

    {
        KnobChoice* bitDepth = dynamic_cast<KnobChoice*>(writer->getKnobByName("bitDepth").get());
        ASSERT_TRUE(bitDepth != NULL);
        bitDepth->setValueFromID("32f", 0);

        KnobChoice* compression = dynamic_cast<KnobChoice*>(writer->getKnobByName("compression").get());
        ASSERT_TRUE(compression != NULL);
        compression->setValueFromID("none", 0);

        // DeepToImage's output is a front-to-back composite, i.e. premultiplied, and that is what
        // WriteOIIO expects to encode; saying so keeps the writer from re-premultiplying it.
        KnobChoice* inputPremult = dynamic_cast<KnobChoice*>(writer->getKnobByName("inputPremult").get());
        ASSERT_TRUE(inputPremult != NULL);
        inputPremult->setValueFromID("premult", 0);
        ASSERT_EQ(1, inputPremult->getValue());
    }

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string pattern = (tmp.path() + QLatin1String("/deep-pipeline.####.exr")).toStdString();
    writer->setOutputFilesForWriter(pattern);

    OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>(writer->getEffectInstance().get());
    ASSERT_TRUE(writerEffect != NULL);

    const int frame = 1;
    std::list<AppInstance::RenderWork> works;
    works.push_back(AppInstance::RenderWork(writerEffect, frame, frame, 1, false));
    getApp()->startWritersRendering(true, works);

    const std::vector<std::string>& viewNames = getApp()->getProject()->getProjectViewNames();
    const std::string path = SequenceParsing::generateFileNameFromPattern(pattern, viewNames, frame, 0);
    ASSERT_TRUE(QFile::exists(QString::fromStdString(path))) << "the frame was not rendered: " << path;

    FlatExrImage image;
    std::string error;
    ASSERT_TRUE(readFlatExr(path, &image, &error)) << error;
    ASSERT_EQ(kDeepFixtureWidth, image.width);
    ASSERT_EQ(kDeepFixtureHeight, image.height);
    static const char* const kChannels[4] = { "R", "G", "B", "A" };
    for (int c = 0; c < 4; ++c) {
        ASSERT_GE(image.channelIndex(kChannels[c]), 0) << "channel " << kChannels[c] << " is missing from the written file";
    }

    for (int y = 0; y < kDeepFixtureHeight; ++y) {
        for (int x = 0; x < kDeepFixtureWidth; ++x) {
            float expected[4];
            expectedOutputPixel(x, y, expected);

            const int32_t fileRow = image.y1 + (kDeepFixtureHeight - 1 - y);
            const int32_t fileCol = image.x1 + x;
            for (int c = 0; c < 4; ++c) {
                const float actual = image.at(fileCol, fileRow, kChannels[c]);
                EXPECT_NEAR(expected[c], actual, 1e-6f) << "at pixel (" << x << ", " << y << ") channel " << kChannels[c];
            }
        }
    }
} // TEST_F(DeepPipelineTest, DeepReadMergeRecolorToImageWriteMatchesTheSerialComposite)
