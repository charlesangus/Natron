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
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include <ofxNatron.h>

#include "BaseTest.h"
#include "CacheMemoryPressureGuard.h"
#include "DeepRenderTestEffect.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/DeepFlatten.h"
#include "Engine/DeepImage.h"
#include "Engine/DeepImageCacheEntry.h"
#include "Engine/DeepImageKey.h"
#include "Engine/DeepPixelOps.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/ImagePlaneDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Deep/DeepCrop.h"
#include "Engine/Nodes/Deep/DeepExpression.h"
#include "Engine/Nodes/Deep/DeepFromImage.h"
#include "Engine/Nodes/Deep/DeepMerge.h"
#include "Engine/Nodes/Deep/DeepRead.h"
#include "Engine/Nodes/Deep/DeepRecolor.h"
#include "Engine/Nodes/Deep/DeepReformat.h"
#include "Engine/Nodes/Deep/DeepToImage.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

#define kDeepFixtureWidth 4
#define kDeepFixtureHeight 3

std::vector<std::string>
rgbaChannelNames()
{
    std::vector<std::string> names;

    names.push_back("R");
    names.push_back("G");
    names.push_back("B");
    names.push_back("A");

    return names;
}

DeepSample
pointSample(float z,
            float r,
            float g,
            float b,
            float a)
{
    std::vector<float> channels;

    channels.push_back(r);
    channels.push_back(g);
    channels.push_back(b);
    channels.push_back(a);

    return DeepSample(z, z, channels);
}

DeepSample
volumeSample(float z,
             float zback,
             float r,
             float g,
             float b,
             float a)
{
    DeepSample sample = pointSample(z, r, g, b, a);

    sample.zback = zback;

    return sample;
}

// The samples one pixel of a hand-built deep image holds, in the order they are stored.
struct SynthPixel {
    int x;
    int y;
    std::vector<DeepSample> samples;

    SynthPixel(int x_,
               int y_)
        : x(x_)
        , y(y_)
        , samples()
    {
    }
};

typedef std::vector<SynthPixel> SynthPixels;

const SynthPixel*
findSynthPixel(const SynthPixels& pixels,
               int x,
               int y)
{
    for (std::size_t p = 0; p < pixels.size(); ++p) {
        if ((pixels[p].x == x) && (pixels[p].y == y)) {
            return &pixels[p];
        }
    }

    return NULL;
}

// A DeepImage holding exactly pixels, every sample's channel values in channelNames' order; the
// samples of a pixel not listed are none.
DeepImagePtr
makeDeepImage(const RectI& bounds,
              const std::vector<std::string>& channelNames,
              const SynthPixels& pixels,
              bool tidy)
{
    DeepImagePtr image = std::make_shared<DeepImage>(bounds, RenderScale::identity, ViewIdx(0));

    SampleTable& table = image->getSampleTableForWriting();
    for (std::size_t p = 0; p < pixels.size(); ++p) {
        std::size_t index;
        EXPECT_TRUE(deepRenderTestPixelIndex(*image, pixels[p].x, pixels[p].y, &index));
        table.setCount(index, (U32)pixels[p].samples.size());
    }
    table.recomputeOffsets();

    float* z = image->getChannelForWriting("Z").dataForWriting();
    float* zback = image->getChannelForWriting("ZBack").dataForWriting();
    std::vector<float*> channels(channelNames.size());
    for (std::size_t c = 0; c < channelNames.size(); ++c) {
        channels[c] = image->getChannelForWriting(channelNames[c]).dataForWriting();
    }

    for (std::size_t p = 0; p < pixels.size(); ++p) {
        std::size_t index;
        EXPECT_TRUE(deepRenderTestPixelIndex(*image, pixels[p].x, pixels[p].y, &index));
        const U64 offset = table.getOffset(index);
        for (std::size_t s = 0; s < pixels[p].samples.size(); ++s) {
            const DeepSample& sample = pixels[p].samples[s];
            z[offset + s] = sample.z;
            zback[offset + s] = sample.zback;
            for (std::size_t c = 0; c < channels.size(); ++c) {
                channels[c][offset + s] = (c < sample.channels.size()) ? sample.channels[c] : 0.f;
            }
        }
    }
    image->setTidy(tidy);

    return image;
}

// One sample as read back out of a DeepImage, its values by channel name so a comparison need
// not care about the name ordering DeepImage keeps its channels in.
struct ReadSample {
    float z;
    float zback;
    std::map<std::string, float> values;

    float value(const std::string& channel) const
    {
        const std::map<std::string, float>::const_iterator found = values.find(channel);

        return (found == values.end()) ? -1.f : found->second;
    }
};

std::vector<ReadSample>
samplesAt(const DeepImage& image,
          int x,
          int y)
{
    std::vector<std::string> channelNames;
    std::vector<DeepSample> samples;
    std::vector<ReadSample> out;

    if (!DeepFlatten::getSamplesAtPixel(image, x, y, &channelNames, &samples)) {
        return out;
    }
    for (std::size_t s = 0; s < samples.size(); ++s) {
        ReadSample sample;
        sample.z = samples[s].z;
        sample.zback = samples[s].zback;
        for (std::size_t c = 0; c < channelNames.size(); ++c) {
            sample.values[channelNames[c]] = samples[s].channels[c];
        }
        out.push_back(sample);
    }

    return out;
}

bool
frontToBack(const ReadSample& a,
            const ReadSample& b)
{
    return a.z < b.z;
}

// The reference the merge is checked against: the front-to-back "over" of samples that never
// overlap one another, in depth order, which is the composite "Interpreting Deep Pixels"
// prescribes once no splitting or merging is called for. Written as the plainest possible loop,
// with no call into DeepPixelOps or DeepFlatten.
std::vector<float>
flattenSerial(std::vector<ReadSample> samples)
{
    const std::vector<std::string> channels = rgbaChannelNames();
    std::vector<float> out(channels.size(), 0.f);
    float transmittance = 1.f;

    std::stable_sort(samples.begin(), samples.end(), &frontToBack);
    for (std::size_t s = 0; s < samples.size(); ++s) {
        for (std::size_t c = 0; c < channels.size(); ++c) {
            out[c] += transmittance * samples[s].value(channels[c]);
        }
        transmittance *= 1.f - samples[s].value("A");
    }

    return out;
}

std::vector<ReadSample>
synthSamplesAt(const SynthPixels& pixels,
               int x,
               int y)
{
    const std::vector<std::string> channels = rgbaChannelNames();
    std::vector<ReadSample> out;
    const SynthPixel* pixel = findSynthPixel(pixels, x, y);

    if (!pixel) {
        return out;
    }
    for (std::size_t s = 0; s < pixel->samples.size(); ++s) {
        ReadSample sample;
        sample.z = pixel->samples[s].z;
        sample.zback = pixel->samples[s].zback;
        for (std::size_t c = 0; c < channels.size(); ++c) {
            sample.values[channels[c]] = pixel->samples[s].channels[c];
        }
        out.push_back(sample);
    }

    return out;
}

void
expectSampleValues(const ReadSample& sample,
                   const DeepSample& expected,
                   float tolerance)
{
    const std::vector<std::string> channels = rgbaChannelNames();

    EXPECT_NEAR(expected.z, sample.z, tolerance);
    EXPECT_NEAR(expected.zback, sample.zback, tolerance);
    for (std::size_t c = 0; c < channels.size() && c < expected.channels.size(); ++c) {
        EXPECT_NEAR(expected.channels[c], sample.value(channels[c]), tolerance) << "channel " << channels[c];
    }
}

const float*
imagePixel(const Image::ReadAccess& access,
           int x,
           int y)
{
    return (const float*)access.pixelAt(x, y);
}

ImagePtr
makeFloatRGBAImage(const RectI& bounds)
{
    const RectD rod(bounds.x1, bounds.y1, bounds.x2, bounds.y2);

    return std::make_shared<Image>(ImagePlaneDesc::getRGBAComponents(), rod, bounds, 0 /*mipmapLevel*/, 1. /*par*/,
                                   eImageBitDepthFloat, eImagePremultiplicationPremultiplied,
                                   eImageFieldingOrderNone, false /*useBitmap*/);
}

QString
fixturePath(const char* name)
{
    return QString::fromUtf8(NATRON_TESTS_FIXTURES_DIR "/") + QString::fromUtf8(name);
}

std::vector<RectI>
cachedDeepEntryBounds(const NodePtr& node,
                      double time)
{
    std::list<DeepImageCacheEntryPtr> found;
    std::vector<RectI> bounds;

    if (!appPTR->getDeepImage(DeepImageKey(node.get(), node->getHashValue(), time, ViewIdx(0), RenderScale::identity), &found)) {
        return bounds;
    }
    for (std::list<DeepImageCacheEntryPtr>::const_iterator it = found.begin(); it != found.end(); ++it) {
        if ((*it)->getDeepImage()) {
            bounds.push_back((*it)->getDeepImage()->getBounds());
        }
    }

    return bounds;
}

// The samples DeepRecolor's tests feed it: pixels with several non-overlapping samples, a sample
// with no alpha, a pixel whose colour has no alpha, one under no colour at all, one covered in
// full and one not covered, so that every branch of the recolour has a pixel to show on.
SynthPixels
recolorPixels()
{
    SynthPixels pixels;

    pixels.push_back(SynthPixel(1, 1));
    pixels.back().samples.push_back(pointSample(1.f, 0.1f, 0.2f, 0.3f, 0.25f));
    pixels.back().samples.push_back(pointSample(2.f, 0.4f, 0.5f, 0.6f, 0.5f));
    pixels.push_back(SynthPixel(3, 0));
    pixels.back().samples.push_back(pointSample(4.f, 0.5f, 0.5f, 0.5f, 0.6f));
    pixels.push_back(SynthPixel(4, 3));
    pixels.back().samples.push_back(pointSample(3.f, 0.f, 0.f, 0.f, 0.f));
    pixels.back().samples.push_back(pointSample(5.f, 0.7f, 0.7f, 0.7f, 0.75f));
    pixels.push_back(SynthPixel(5, 5));
    pixels.back().samples.push_back(volumeSample(1.f, 2.f, 0.3f, 0.3f, 0.3f, 0.3f));
    pixels.back().samples.push_back(pointSample(4.f, 0.9f, 0.9f, 0.9f, 0.9f));
    pixels.push_back(SynthPixel(6, 1));
    pixels.back().samples.push_back(pointSample(1.f, 0.5f, 0.5f, 0.5f, 0.5f));
    pixels.back().samples.push_back(pointSample(2.f, 1.f, 1.f, 1.f, 1.f));
    pixels.push_back(SynthPixel(7, 2));
    pixels.back().samples.push_back(pointSample(1.f, 0.f, 0.f, 0.f, 0.f));
    pixels.push_back(SynthPixel(kImageRenderTestWidth + 1, 3));
    pixels.back().samples.push_back(pointSample(2.f, 0.5f, 0.5f, 0.5f, 0.5f));

    return pixels;
}

float
recolorAlphaAt(int seed,
               int x,
               int y,
               const RectI& colorFrame)
{
    return colorFrame.contains(x, y) ? imageRenderTestValue(seed, x, y, 3) : 0.f;
}

// Checks every sample of pixels that lies within window against what DeepRecolor is meant to
// write: depths untouched, R, G and B the colour image's unpremultiplied colour times the
// sample's output alpha -- zero where the colour has no alpha or does not cover the pixel --
// and, unless the alphas were retargeted, the alpha the sample came in with.
void
expectRecolored(const DeepImage& out,
                const SynthPixels& pixels,
                const RectI& window,
                int seed,
                const RectI& colorFrame,
                bool alphasRetargeted)
{
    for (std::size_t p = 0; p < pixels.size(); ++p) {
        const int x = pixels[p].x;
        const int y = pixels[p].y;

        if (!window.contains(x, y)) {
            continue;
        }
        const std::vector<ReadSample> samples = samplesAt(out, x, y);
        ASSERT_EQ(pixels[p].samples.size(), samples.size()) << "at pixel (" << x << ", " << y << ")";
        const float colorAlpha = recolorAlphaAt(seed, x, y, colorFrame);
        for (std::size_t s = 0; s < samples.size(); ++s) {
            EXPECT_FLOAT_EQ(pixels[p].samples[s].z, samples[s].z) << "at pixel (" << x << ", " << y << ") sample " << s;
            EXPECT_FLOAT_EQ(pixels[p].samples[s].zback, samples[s].zback) << "at pixel (" << x << ", " << y << ") sample " << s;
            if (!alphasRetargeted) {
                EXPECT_FLOAT_EQ(pixels[p].samples[s].channels[3], samples[s].value("A")) << "at pixel (" << x << ", " << y << ") sample " << s;
            }
            for (int c = 0; c < 3; ++c) {
                const float expected = (colorAlpha > 0.f) ? (imageRenderTestValue(seed, x, y, c) / colorAlpha * samples[s].value("A")) : 0.f;
                EXPECT_NEAR(expected, samples[s].value(rgbaChannelNames()[c]), 1e-5f) << "at pixel (" << x << ", " << y << ") sample " << s << " channel " << c;
            }
        }
    }
}

} // namespace

class DeepNodesTest
    : public BaseTest {
protected:
    // Tests here assert that a deep entry rendered earlier is still servable from the app-wide
    // cache; see CacheMemoryPressureGuard.h.
    DisableUnreachableRAMPurging _noPurging;

    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        deepSyntheticSourceImages().clear();
        _nextSlot = 1;
    }

    // Destroyed nodes take their cache entries with them, so the next test's nodes -- which may
    // well get the same script names, and so the same hashes -- cannot be served this test's
    // renders.
    virtual void TearDown() OVERRIDE
    {
        for (std::vector<NodePtr>::reverse_iterator it = _nodes.rbegin(); it != _nodes.rend(); ++it) {
            (*it)->destroyNode(false, false);
        }
        _nodes.clear();
        deepSyntheticSourceImages().clear();
        BaseTest::TearDown();
    }

    NodePtr createTrackedNode(const char* pluginID)
    {
        NodePtr node = createNode(QString::fromUtf8(pluginID));

        if (node) {
            _nodes.push_back(node);
        }

        return node;
    }

    NodePtr createSyntheticSource(const DeepImagePtr& image)
    {
        const int slot = _nextSlot++;

        deepSyntheticSourceImages()[slot] = image;
        NodePtr node = createTrackedNode(kTestPluginIDDeepSyntheticSource);
        if (!node) {
            return node;
        }
        KnobInt* knob = dynamic_cast<KnobInt*>(node->getKnobByName("slot").get());
        if (!knob) {
            return NodePtr();
        }
        knob->setValue(slot);

        return node;
    }

    NodePtr createImageSource(int seed)
    {
        NodePtr node = createTrackedNode(kTestPluginIDImageRenderSource);

        if (!node) {
            return node;
        }
        KnobInt* knob = dynamic_cast<KnobInt*>(node->getKnobByName("seed").get());
        if (!knob) {
            return NodePtr();
        }
        knob->setValue(seed);

        return node;
    }

    NodePtr createDeepRead(const QString& filename)
    {
        NodePtr read = createTrackedNode(PLUGINID_NATRON_DEEPREAD);

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

    NodePtr createDeepMerge(DeepMerge::OperationEnum operation)
    {
        NodePtr merge = createTrackedNode(PLUGINID_NATRON_DEEPMERGE);

        if (!merge) {
            return merge;
        }
        KnobChoice* knob = dynamic_cast<KnobChoice*>(merge->getKnobByName("operation").get());
        if (!knob) {
            return NodePtr();
        }
        knob->setValue((int)operation);

        return merge;
    }

    NodePtr createDeepFromImage(double depth)
    {
        NodePtr node = createTrackedNode(PLUGINID_NATRON_DEEPFROMIMAGE);

        if (!node) {
            return node;
        }
        KnobDouble* knob = dynamic_cast<KnobDouble*>(node->getKnobByName("depth").get());
        if (!knob) {
            return NodePtr();
        }
        knob->setValue(depth);

        return node;
    }

    NodePtr createDeepCrop(double bx,
                           double by,
                           double bw,
                           double bh,
                           bool useBBox,
                           double zmin,
                           double zmax,
                           bool useZRange,
                           bool reformat)
    {
        NodePtr node = createTrackedNode(PLUGINID_NATRON_DEEPCROP);

        if (!node) {
            return node;
        }
        KnobDouble* bbox = dynamic_cast<KnobDouble*>(node->getKnobByName("bbox").get());
        KnobBool* useBBoxKnob = dynamic_cast<KnobBool*>(node->getKnobByName("useBBox").get());
        KnobDouble* zRange = dynamic_cast<KnobDouble*>(node->getKnobByName("zRange").get());
        KnobBool* useZRangeKnob = dynamic_cast<KnobBool*>(node->getKnobByName("useZRange").get());
        KnobBool* reformatKnob = dynamic_cast<KnobBool*>(node->getKnobByName("reformat").get());
        if (!bbox || !useBBoxKnob || !zRange || !useZRangeKnob || !reformatKnob) {
            return NodePtr();
        }
        bbox->setValue(bx, ViewSpec::all(), 0);
        bbox->setValue(by, ViewSpec::all(), 1);
        bbox->setValue(bw, ViewSpec::all(), 2);
        bbox->setValue(bh, ViewSpec::all(), 3);
        useBBoxKnob->setValue(useBBox);
        zRange->setValue(zmin, ViewSpec::all(), 0);
        zRange->setValue(zmax, ViewSpec::all(), 1);
        useZRangeKnob->setValue(useZRange);
        reformatKnob->setValue(reformat);

        return node;
    }

    NodePtr createDeepReformat(int w,
                               int h,
                               bool useCustomSize,
                               bool centre)
    {
        NodePtr node = createTrackedNode(PLUGINID_NATRON_DEEPREFORMAT);

        if (!node) {
            return node;
        }
        KnobBool* useCustomSizeKnob = dynamic_cast<KnobBool*>(node->getKnobByName("useCustomSize").get());
        KnobInt* customSize = dynamic_cast<KnobInt*>(node->getKnobByName("customSize").get());
        KnobBool* centreKnob = dynamic_cast<KnobBool*>(node->getKnobByName("centre").get());
        if (!useCustomSizeKnob || !customSize || !centreKnob) {
            return NodePtr();
        }
        customSize->setValue(w, ViewSpec::all(), 0);
        customSize->setValue(h, ViewSpec::all(), 1);
        useCustomSizeKnob->setValue(useCustomSize);
        centreKnob->setValue(centre);

        return node;
    }

    // The synthetic source declares no format of its own, so its metadata carries the project's,
    // which is no use to a node that positions its input by the input's format. A DeepCrop with
    // Reformat on and a Bbox containing the whole source declares (0, 0, w, h) instead without
    // touching a sample: it is an identity, so a render through it still resolves to the source's
    // own cache entry. Returns that DeepCrop, with the source it wraps in *source.
    NodePtr createDeepSourceWithFormat(const DeepImagePtr& image,
                                       int w,
                                       int h,
                                       NodePtr* source)
    {
        *source = createSyntheticSource(image);

        if (!*source) {
            return NodePtr();
        }
        NodePtr format = createDeepCrop(0., 0., (double)w, (double)h, true, 0., 0., false, true);
        if (!format) {
            return NodePtr();
        }
        connectNodes(*source, format, 0, true);
        (*source)->getEffectInstance()->refreshMetadata_public(false);
        format->getEffectInstance()->refreshMetadata_public(false);

        return format;
    }

    NodePtr createDeepRecolor(bool targetInputAlpha)
    {
        NodePtr node = createTrackedNode(PLUGINID_NATRON_DEEPRECOLOR);

        if (!node) {
            return node;
        }
        KnobBool* knob = dynamic_cast<KnobBool*>(node->getKnobByName("targetInputAlpha").get());
        if (!knob) {
            return NodePtr();
        }
        knob->setValue(targetInputAlpha);

        return node;
    }

    // expressions holds the R, G, B, A, Z and ZBack expressions in that order; an empty one
    // leaves its channel alone.
    NodePtr createDeepExpression(const std::vector<std::string>& expressions)
    {
        static const char* const knobNames[] = { "expressionR", "expressionG", "expressionB", "expressionA", "expressionZ", "expressionZBack" };
        NodePtr node = createTrackedNode(PLUGINID_NATRON_DEEPEXPRESSION);

        if (!node) {
            return node;
        }
        for (std::size_t c = 0; c < expressions.size() && c < 6; ++c) {
            KnobString* knob = dynamic_cast<KnobString*>(node->getKnobByName(knobNames[c]).get());
            if (!knob) {
                return NodePtr();
            }
            knob->setValue(expressions[c]);
        }

        return node;
    }

    // Renders node's deep data the way the scheduler does, under frame args carrying an abort
    // flag for EffectInstance::aborted() to read.
    EffectInstance::RenderRoIRetCode renderDeepFrame(const NodePtr& node,
                                                     double time,
                                                     const RectI& roi,
                                                     DeepImagePtr* outputDeepImage)
    {
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
        ParallelRenderArgsSetter frameRenderArgs(time,
                                                 ViewIdx(0),
                                                 true /*isRenderUserInteraction*/,
                                                 false /*isSequential*/,
                                                 abortInfo,
                                                 node,
                                                 0 /*textureIndex*/,
                                                 getApp()->getTimeLine().get(),
                                                 NodePtr(),
                                                 false /*isAnalysis*/,
                                                 false /*draftMode*/,
                                                 RenderStatsPtr());
        EffectInstance::RenderDeepRoIArgs args(time,
                                               RenderScale::identity,
                                               0 /*mipmapLevel*/,
                                               ViewIdx(0),
                                               false /*byPassCache*/,
                                               roi,
                                               RectD(),
                                               0 /*caller*/,
                                               time);

        return node->getEffectInstance()->renderDeepRoI(args, outputDeepImage);
    }

    // Renders node's float RGBA image through the ordinary image path -- request pass included,
    // the way Node::makePreviewImage() and the scheduler drive it.
    EffectInstance::RenderRoIRetCode renderImageFrame(const NodePtr& node,
                                                      double time,
                                                      const RectI& roi,
                                                      ImagePtr* outputImage)
    {
        outputImage->reset();

        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
        ParallelRenderArgsSetter frameRenderArgs(time,
                                                 ViewIdx(0),
                                                 true /*isRenderUserInteraction*/,
                                                 false /*isSequential*/,
                                                 abortInfo,
                                                 node,
                                                 0 /*textureIndex*/,
                                                 getApp()->getTimeLine().get(),
                                                 NodePtr(),
                                                 false /*isAnalysis*/,
                                                 false /*draftMode*/,
                                                 RenderStatsPtr());
        EffectInstancePtr effect = node->getEffectInstance();

        RectD rod;
        bool isProjectFormat = false;
        if (effect->getRegionOfDefinition_public(node->getHashValue(), time, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat) == eStatusFailed) {
            return EffectInstance::eRenderRoIRetCodeFailed;
        }

        FrameRequestMap request;
        if (EffectInstance::computeRequestPass(time, ViewIdx(0), 0 /*mipmapLevel*/, rod, node, request) == eStatusFailed) {
            return EffectInstance::eRenderRoIRetCodeFailed;
        }
        frameRenderArgs.updateNodesRequest(request);

        std::list<ImagePlaneDesc> components;
        components.push_back(ImagePlaneDesc::getRGBAComponents());
        EffectInstance::RenderRoIArgs args(time,
                                           RenderScale::identity,
                                           0 /*mipmapLevel*/,
                                           ViewIdx(0),
                                           false /*byPassCache*/,
                                           roi,
                                           rod,
                                           components,
                                           eImageBitDepthFloat,
                                           false /*calledFromGetImage*/,
                                           0 /*caller*/,
                                           eStorageModeRAM,
                                           time);
        std::map<ImagePlaneDesc, ImagePtr> planes;
        const EffectInstance::RenderRoIRetCode code = effect->renderRoI(args, &planes);
        if ((code == EffectInstance::eRenderRoIRetCodeOk) && !planes.empty()) {
            *outputImage = planes.begin()->second;
        }

        return code;
    }

    std::vector<NodePtr> _nodes;
    int _nextSlot;
};

TEST_F(DeepNodesTest, AllThreeNodesAreRegisteredAndInstantiable)
{
    NodePtr merge = createTrackedNode(PLUGINID_NATRON_DEEPMERGE);
    NodePtr toImage = createTrackedNode(PLUGINID_NATRON_DEEPTOIMAGE);
    NodePtr fromImage = createTrackedNode(PLUGINID_NATRON_DEEPFROMIMAGE);

    ASSERT_TRUE(merge != NULL);
    ASSERT_TRUE(toImage != NULL);
    ASSERT_TRUE(fromImage != NULL);

    EXPECT_EQ(2, merge->getEffectInstance()->getNInputs());
    EXPECT_EQ(eDataKindDeep, merge->getEffectInstance()->getInputDataKind(0));
    EXPECT_EQ(eDataKindDeep, merge->getEffectInstance()->getInputDataKind(1));
    EXPECT_EQ(eDataKindDeep, merge->getEffectInstance()->getOutputDataKind());
    EXPECT_EQ("A", merge->getEffectInstance()->getInputLabel(0));
    EXPECT_EQ("B", merge->getEffectInstance()->getInputLabel(1));

    EXPECT_EQ(1, toImage->getEffectInstance()->getNInputs());
    EXPECT_EQ(eDataKindDeep, toImage->getEffectInstance()->getInputDataKind(0));
    EXPECT_EQ(eDataKindImage, toImage->getEffectInstance()->getOutputDataKind());

    EXPECT_EQ(2, fromImage->getEffectInstance()->getNInputs());
    EXPECT_EQ(eDataKindImage, fromImage->getEffectInstance()->getInputDataKind(0));
    EXPECT_EQ(eDataKindImage, fromImage->getEffectInstance()->getInputDataKind(1));
    EXPECT_EQ(eDataKindDeep, fromImage->getEffectInstance()->getOutputDataKind());
    EXPECT_FALSE(fromImage->getEffectInstance()->isInputOptional(0));
    EXPECT_TRUE(fromImage->getEffectInstance()->isInputOptional(1));
    EXPECT_EQ("Z", fromImage->getEffectInstance()->getInputLabel(1));

    // All three sit in the same menu as DeepRead and DeepWrite.
    NodePtr nodes[3] = { merge, toImage, fromImage };
    for (int i = 0; i < 3; ++i) {
        std::list<std::string> grouping;
        nodes[i]->getEffectInstance()->getPluginGrouping(&grouping);
        ASSERT_EQ((std::size_t)1, grouping.size());
        EXPECT_EQ(PLUGIN_GROUP_DEEP, grouping.front());
    }

    // The kinds they declare are what decides which edges the graph accepts.
    NodePtr deepSource = createSyntheticSource(makeDeepImage(RectI(0, 0, 2, 2), rgbaChannelNames(), SynthPixels(), true));
    NodePtr imageSource = createImageSource(0);
    ASSERT_TRUE(deepSource && imageSource);
    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, merge->canConnectInput(imageSource, 1));
    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, fromImage->canConnectInput(deepSource, 0));
    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, toImage->canConnectInput(imageSource, 0));
    connectNodes(deepSource, merge, 0, true);
    connectNodes(merge, toImage, 0, true);
    connectNodes(imageSource, fromImage, 0, true);
    connectNodes(fromImage, merge, 1, true);
}

TEST_F(DeepNodesTest, CombineConcatenatesSortsAndFlattensLikeTheSerialReference)
{
    // A and B cover different rectangles, hold samples at interleaved depths, store one pixel's
    // samples back to front, and B carries a channel A lacks.
    const RectI boundsA(0, 0, 4, 3);
    const RectI boundsB(2, 1, 6, 4);
    const RectI unionBounds(0, 0, 6, 4);

    SynthPixels pixelsA;
    pixelsA.push_back(SynthPixel(1, 0));
    pixelsA.back().samples.push_back(pointSample(2.f, 0.2f, 0.1f, 0.05f, 0.5f));
    pixelsA.push_back(SynthPixel(2, 1));
    pixelsA.back().samples.push_back(pointSample(5.f, 0.3f, 0.3f, 0.3f, 0.6f));
    pixelsA.back().samples.push_back(pointSample(1.f, 0.1f, 0.f, 0.2f, 0.25f));
    pixelsA.push_back(SynthPixel(3, 2));
    pixelsA.back().samples.push_back(volumeSample(3.f, 4.f, 0.4f, 0.2f, 0.1f, 0.8f));

    std::vector<std::string> channelsB = rgbaChannelNames();
    channelsB.push_back("AOV");
    SynthPixels pixelsB;
    pixelsB.push_back(SynthPixel(2, 1));
    pixelsB.back().samples.push_back(pointSample(3.f, 0.05f, 0.1f, 0.15f, 0.2f));
    pixelsB.back().samples.back().channels.push_back(7.f);
    pixelsB.push_back(SynthPixel(3, 2));
    pixelsB.back().samples.push_back(pointSample(1.f, 0.5f, 0.5f, 0.5f, 0.5f));
    pixelsB.back().samples.back().channels.push_back(8.f);
    pixelsB.back().samples.push_back(pointSample(6.f, 0.1f, 0.1f, 0.1f, 1.f));
    pixelsB.back().samples.back().channels.push_back(9.f);
    pixelsB.push_back(SynthPixel(5, 3));
    pixelsB.back().samples.push_back(pointSample(2.f, 1.f, 1.f, 1.f, 1.f));
    pixelsB.back().samples.back().channels.push_back(10.f);

    NodePtr sourceA = createSyntheticSource(makeDeepImage(boundsA, rgbaChannelNames(), pixelsA, false));
    NodePtr sourceB = createSyntheticSource(makeDeepImage(boundsB, channelsB, pixelsB, true));
    NodePtr merge = createDeepMerge(DeepMerge::eOperationCombine);
    ASSERT_TRUE(sourceA && sourceB && merge);
    connectNodes(sourceA, merge, 0, true);
    connectNodes(sourceB, merge, 1, true);

    DeepImagePtr merged;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(merge, 1., unionBounds, &merged));
    ASSERT_TRUE(merged != NULL);
    EXPECT_TRUE(unionBounds == merged->getBounds());
    EXPECT_FALSE(merged->isTidy());
    EXPECT_EQ((U64)8, merged->getSampleTable().getTotalSampleCount());
    EXPECT_TRUE(merged->hasChannel("AOV"));

    for (int y = unionBounds.y1; y < unionBounds.y2; ++y) {
        for (int x = unionBounds.x1; x < unionBounds.x2; ++x) {
            std::vector<ReadSample> expected = synthSamplesAt(pixelsA, x, y);
            const std::vector<ReadSample> fromB = synthSamplesAt(pixelsB, x, y);
            expected.insert(expected.end(), fromB.begin(), fromB.end());
            std::stable_sort(expected.begin(), expected.end(), &frontToBack);

            const std::vector<ReadSample> actual = samplesAt(*merged, x, y);
            ASSERT_EQ(expected.size(), actual.size()) << "at pixel (" << x << ", " << y << ")";
            for (std::size_t s = 0; s < expected.size(); ++s) {
                EXPECT_FLOAT_EQ(expected[s].z, actual[s].z) << "at pixel (" << x << ", " << y << ") sample " << s;
                EXPECT_FLOAT_EQ(expected[s].zback, actual[s].zback) << "at pixel (" << x << ", " << y << ") sample " << s;
                const std::vector<std::string> channels = rgbaChannelNames();
                for (std::size_t c = 0; c < channels.size(); ++c) {
                    EXPECT_FLOAT_EQ(expected[s].value(channels[c]), actual[s].value(channels[c])) << "at pixel (" << x << ", " << y << ") sample " << s << " channel " << channels[c];
                }
            }
            // A's samples never had an AOV, so on them it reads as zero; B's keep theirs.
            const SynthPixel* pixelB = findSynthPixel(pixelsB, x, y);
            for (std::size_t s = 0; s < actual.size(); ++s) {
                float expectedAov = 0.f;
                if (pixelB) {
                    for (std::size_t t = 0; t < pixelB->samples.size(); ++t) {
                        if (pixelB->samples[t].z == actual[s].z) {
                            expectedAov = pixelB->samples[t].channels[4];
                        }
                    }
                }
                EXPECT_FLOAT_EQ(expectedAov, actual[s].value("AOV")) << "at pixel (" << x << ", " << y << ") sample " << s;
            }
        }
    }

    NodePtr toImage = createTrackedNode(PLUGINID_NATRON_DEEPTOIMAGE);
    ASSERT_TRUE(toImage != NULL);
    connectNodes(merge, toImage, 0, true);

    ImagePtr flattened;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderImageFrame(toImage, 1., unionBounds, &flattened));
    ASSERT_TRUE(flattened != NULL);
    ASSERT_TRUE(flattened->getBounds().contains(unionBounds));
    ASSERT_EQ(eImageBitDepthFloat, flattened->getBitDepth());
    ASSERT_EQ(4u, flattened->getComponentsCount());

    Image::ReadAccess access = flattened->getReadRights();
    for (int y = unionBounds.y1; y < unionBounds.y2; ++y) {
        for (int x = unionBounds.x1; x < unionBounds.x2; ++x) {
            std::vector<ReadSample> samples = synthSamplesAt(pixelsA, x, y);
            const std::vector<ReadSample> fromB = synthSamplesAt(pixelsB, x, y);
            samples.insert(samples.end(), fromB.begin(), fromB.end());
            const std::vector<float> expected = flattenSerial(samples);
            const float* actual = imagePixel(access, x, y);
            ASSERT_TRUE(actual != NULL);
            for (std::size_t c = 0; c < expected.size(); ++c) {
                EXPECT_NEAR(expected[c], actual[c], 1e-6f) << "at pixel (" << x << ", " << y << ") channel " << c;
            }
        }
    }
} // TEST_F(DeepNodesTest, CombineConcatenatesSortsAndFlattensLikeTheSerialReference)

TEST_F(DeepNodesTest, CombineOfTwoDeepReadsPullsThroughRenderDeepRoIAndCaches)
{
    const RectI fullFrame(0, 0, kDeepFixtureWidth, kDeepFixtureHeight);

    // The two fixtures hold the same samples in a scanline and a tiled part, so the merged pixel
    // is each of them twice over.
    NodePtr readA = createDeepRead(fixturePath("deep-scanline.exr"));
    NodePtr readB = createDeepRead(fixturePath("deep-tiled.exr"));
    NodePtr merge = createDeepMerge(DeepMerge::eOperationCombine);
    ASSERT_TRUE(readA && readB && merge);
    connectNodes(readA, merge, 0, true);
    connectNodes(readB, merge, 1, true);

    DeepImagePtr first;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(merge, 1., fullFrame, &first));
    ASSERT_TRUE(first != NULL);
    EXPECT_TRUE(fullFrame == first->getBounds());
    EXPECT_FALSE(first->isTidy());

    // Both inputs were pulled through the deep pipeline: each has its own cached entry now.
    ASSERT_EQ((std::size_t)1, cachedDeepEntryBounds(readA, 1.).size());
    ASSERT_EQ((std::size_t)1, cachedDeepEntryBounds(readB, 1.).size());
    ASSERT_EQ((std::size_t)1, cachedDeepEntryBounds(merge, 1.).size());
    EXPECT_TRUE(fullFrame == cachedDeepEntryBounds(merge, 1.)[0]);

    DeepImagePtr fixture;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(readA, 1., fullFrame, &fixture));
    ASSERT_TRUE(fixture != NULL);
    ASSERT_GT(fixture->getSampleTable().getTotalSampleCount(), (U64)0);
    EXPECT_EQ(2 * fixture->getSampleTable().getTotalSampleCount(), first->getSampleTable().getTotalSampleCount());

    const std::vector<std::string> channels = rgbaChannelNames();
    for (int y = fullFrame.y1; y < fullFrame.y2; ++y) {
        for (int x = fullFrame.x1; x < fullFrame.x2; ++x) {
            const std::vector<ReadSample> once = samplesAt(*fixture, x, y);
            std::vector<ReadSample> expected(once);
            expected.insert(expected.end(), once.begin(), once.end());
            std::stable_sort(expected.begin(), expected.end(), &frontToBack);

            const std::vector<ReadSample> actual = samplesAt(*first, x, y);
            ASSERT_EQ(expected.size(), actual.size()) << "at pixel (" << x << ", " << y << ")";
            for (std::size_t s = 0; s < expected.size(); ++s) {
                EXPECT_FLOAT_EQ(expected[s].z, actual[s].z) << "at pixel (" << x << ", " << y << ") sample " << s;
                EXPECT_FLOAT_EQ(expected[s].zback, actual[s].zback) << "at pixel (" << x << ", " << y << ") sample " << s;
                for (std::size_t c = 0; c < channels.size(); ++c) {
                    EXPECT_FLOAT_EQ(expected[s].value(channels[c]), actual[s].value(channels[c])) << "at pixel (" << x << ", " << y << ") sample " << s << " channel " << channels[c];
                }
                EXPECT_FLOAT_EQ(expected[s].value("AOV"), actual[s].value("AOV")) << "at pixel (" << x << ", " << y << ") sample " << s;
            }
        }
    }

    // The very same payload comes back out of the deep cache.
    DeepImagePtr second;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(merge, 1., fullFrame, &second));
    ASSERT_TRUE(second != NULL);
    EXPECT_EQ(first.get(), second.get());
    ASSERT_EQ((std::size_t)1, cachedDeepEntryBounds(merge, 1.).size());
}

TEST_F(DeepNodesTest, CombineOfAFixtureWithSamplesBehindItFlattensAsAOverB)
{
    const RectI fullFrame(0, 0, kDeepFixtureWidth, kDeepFixtureHeight);

    // Everything in B lies behind everything in the fixture, so the flattened merge has to be
    // the fixture's flat over B's flat -- and the fixture's own overlapping pixel is handled by
    // the library flatten, not by anything in the node under test.
    SynthPixels pixelsB;
    pixelsB.push_back(SynthPixel(0, 2));
    pixelsB.back().samples.push_back(pointSample(100.f, 0.3f, 0.6f, 0.9f, 1.f));
    pixelsB.push_back(SynthPixel(3, 2));
    pixelsB.back().samples.push_back(pointSample(100.f, 0.2f, 0.1f, 0.4f, 0.5f));
    pixelsB.back().samples.push_back(pointSample(200.f, 0.8f, 0.8f, 0.8f, 1.f));
    pixelsB.push_back(SynthPixel(1, 1));
    pixelsB.back().samples.push_back(volumeSample(100.f, 150.f, 0.1f, 0.2f, 0.3f, 0.4f));
    pixelsB.push_back(SynthPixel(2, 0));
    pixelsB.back().samples.push_back(pointSample(100.f, 0.25f, 0.25f, 0.25f, 0.25f));

    NodePtr read = createDeepRead(fixturePath("deep-scanline.exr"));
    NodePtr sourceB = createSyntheticSource(makeDeepImage(fullFrame, rgbaChannelNames(), pixelsB, true));
    NodePtr merge = createDeepMerge(DeepMerge::eOperationCombine);
    NodePtr toImage = createTrackedNode(PLUGINID_NATRON_DEEPTOIMAGE);
    ASSERT_TRUE(read && sourceB && merge && toImage);
    connectNodes(read, merge, 0, true);
    connectNodes(sourceB, merge, 1, true);
    connectNodes(merge, toImage, 0, true);

    DeepImagePtr fixture;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(read, 1., fullFrame, &fixture));
    ASSERT_TRUE(fixture != NULL);

    ImagePtr flatA = makeFloatRGBAImage(fullFrame);
    {
        DeepPixelScratch scratch;
        DeepTidyWorkspace work;
        ASSERT_EQ(eStatusOK, DeepFlatten::flattenToImage(*fixture, fullFrame, rgbaChannelNames(), 3, &scratch, &work, flatA));
    }

    ImagePtr flattened;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderImageFrame(toImage, 1., fullFrame, &flattened));
    ASSERT_TRUE(flattened != NULL);
    ASSERT_TRUE(flattened->getBounds().contains(fullFrame));

    Image::ReadAccess accessA = flatA->getReadRights();
    Image::ReadAccess access = flattened->getReadRights();
    for (int y = fullFrame.y1; y < fullFrame.y2; ++y) {
        for (int x = fullFrame.x1; x < fullFrame.x2; ++x) {
            const float* a = imagePixel(accessA, x, y);
            const std::vector<float> b = flattenSerial(synthSamplesAt(pixelsB, x, y));
            const float* actual = imagePixel(access, x, y);
            ASSERT_TRUE(a && actual);
            for (int c = 0; c < 4; ++c) {
                EXPECT_NEAR(a[c] + (1.f - a[3]) * b[c], actual[c], 1e-6f) << "at pixel (" << x << ", " << y << ") channel " << c;
            }
        }
    }
}

TEST_F(DeepNodesTest, HoldoutAttenuatesAByWhatOfBLiesInFront)
{
    // The same A sample at every pixel of the first row, held out by a different B each time,
    // plus a volumetric A at x == 6 for the overlap case. B's colour is deliberately loud, and it
    // carries a channel A lacks, so anything of B's reaching the output would show.
    const RectI boundsA(0, 0, 8, 1);
    const RectI boundsB(0, 0, 10, 1);
    const DeepSample aPoint = pointSample(5.f, 0.4f, 0.2f, 0.1f, 0.5f);
    const DeepSample aVolume = volumeSample(1.f, 3.f, 0.6f, 0.3f, 0.15f, 0.75f);

    SynthPixels pixelsA;
    for (int x = 0; x < 8; ++x) {
        pixelsA.push_back(SynthPixel(x, 0));
        pixelsA.back().samples.push_back((x == 6) ? aVolume : aPoint);
    }

    std::vector<std::string> channelsB = rgbaChannelNames();
    channelsB.push_back("AOV");
    SynthPixels pixelsB;
    // x == 0: opaque, in front.
    pixelsB.push_back(SynthPixel(0, 0));
    pixelsB.back().samples.push_back(pointSample(1.f, 1.f, 1.f, 1.f, 1.f));
    // x == 1: opaque, behind.
    pixelsB.push_back(SynthPixel(1, 0));
    pixelsB.back().samples.push_back(pointSample(9.f, 1.f, 1.f, 1.f, 1.f));
    // x == 2: half transparent, in front.
    pixelsB.push_back(SynthPixel(2, 0));
    pixelsB.back().samples.push_back(pointSample(1.f, 0.5f, 0.5f, 0.5f, 0.5f));
    // x == 3: a volume wholly in front.
    pixelsB.push_back(SynthPixel(3, 0));
    pixelsB.back().samples.push_back(volumeSample(1.f, 3.f, 0.75f, 0.75f, 0.75f, 0.75f));
    // x == 4: two half transparent samples in front, stored back to front.
    pixelsB.push_back(SynthPixel(4, 0));
    pixelsB.back().samples.push_back(pointSample(2.f, 0.5f, 0.5f, 0.5f, 0.5f));
    pixelsB.back().samples.push_back(pointSample(1.f, 0.5f, 0.5f, 0.5f, 0.5f));
    // x == 5: a volume straddling A's depth, its front half holding 1 - sqrt(1 - 0.75) of alpha.
    pixelsB.push_back(SynthPixel(5, 0));
    pixelsB.back().samples.push_back(volumeSample(4.f, 6.f, 0.75f, 0.75f, 0.75f, 0.75f));
    // x == 6: a volume overlapping the back half of A's volume.
    pixelsB.push_back(SynthPixel(6, 0));
    pixelsB.back().samples.push_back(volumeSample(2.f, 4.f, 0.75f, 0.75f, 0.75f, 0.75f));
    // x == 7: a point coincident with A's.
    pixelsB.push_back(SynthPixel(7, 0));
    pixelsB.back().samples.push_back(pointSample(5.f, 0.5f, 0.5f, 0.5f, 0.5f));
    for (std::size_t p = 0; p < pixelsB.size(); ++p) {
        for (std::size_t s = 0; s < pixelsB[p].samples.size(); ++s) {
            pixelsB[p].samples[s].channels.push_back(42.f);
        }
    }

    NodePtr sourceA = createSyntheticSource(makeDeepImage(boundsA, rgbaChannelNames(), pixelsA, true));
    NodePtr sourceB = createSyntheticSource(makeDeepImage(boundsB, channelsB, pixelsB, false));
    NodePtr merge = createDeepMerge(DeepMerge::eOperationHoldout);
    ASSERT_TRUE(sourceA && sourceB && merge);
    connectNodes(sourceA, merge, 0, true);
    connectNodes(sourceB, merge, 1, true);

    // Asked over B's wider window, the output still only covers A: B contributes no samples.
    DeepImagePtr held;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(merge, 1., boundsB, &held));
    ASSERT_TRUE(held != NULL);
    EXPECT_TRUE(boundsA == held->getBounds());
    EXPECT_TRUE(held->isTidy());
    EXPECT_FALSE(held->hasChannel("AOV"));
    EXPECT_EQ((U64)9, held->getSampleTable().getTotalSampleCount());

    const float tolerance = 1e-5f;
    std::vector<ReadSample> samples;

    samples = samplesAt(*held, 0, 0);
    ASSERT_EQ((std::size_t)1, samples.size());
    expectSampleValues(samples[0], pointSample(5.f, 0.f, 0.f, 0.f, 0.f), tolerance);

    samples = samplesAt(*held, 1, 0);
    ASSERT_EQ((std::size_t)1, samples.size());
    expectSampleValues(samples[0], aPoint, tolerance);

    samples = samplesAt(*held, 2, 0);
    ASSERT_EQ((std::size_t)1, samples.size());
    expectSampleValues(samples[0], pointSample(5.f, 0.2f, 0.1f, 0.05f, 0.25f), tolerance);

    samples = samplesAt(*held, 3, 0);
    ASSERT_EQ((std::size_t)1, samples.size());
    expectSampleValues(samples[0], pointSample(5.f, 0.1f, 0.05f, 0.025f, 0.125f), tolerance);

    samples = samplesAt(*held, 4, 0);
    ASSERT_EQ((std::size_t)1, samples.size());
    expectSampleValues(samples[0], pointSample(5.f, 0.1f, 0.05f, 0.025f, 0.125f), tolerance);

    samples = samplesAt(*held, 5, 0);
    ASSERT_EQ((std::size_t)1, samples.size());
    expectSampleValues(samples[0], pointSample(5.f, 0.2f, 0.1f, 0.05f, 0.25f), tolerance);

    // A's [1, 3] is cut at B's front boundary, 2, into two halves each holding alpha
    // 1 - sqrt(1 - 0.75) = 0.5 and two thirds of the colour. B's [2, 4] leaves the front half
    // alone; its own front half, [2, 3], holds alpha 0.5 and is coincident with the back half,
    // which it attenuates by 1 - 0.5 / 2.
    samples = samplesAt(*held, 6, 0);
    ASSERT_EQ((std::size_t)2, samples.size());
    expectSampleValues(samples[0], volumeSample(1.f, 2.f, 0.4f, 0.2f, 0.1f, 0.5f), tolerance);
    expectSampleValues(samples[1], volumeSample(2.f, 3.f, 0.3f, 0.15f, 0.075f, 0.375f), tolerance);

    samples = samplesAt(*held, 7, 0);
    ASSERT_EQ((std::size_t)1, samples.size());
    expectSampleValues(samples[0], pointSample(5.f, 0.3f, 0.15f, 0.075f, 0.375f), tolerance);
} // TEST_F(DeepNodesTest, HoldoutAttenuatesAByWhatOfBLiesInFront)

TEST_F(DeepNodesTest, HoldoutWithNothingOnBPassesAThrough)
{
    const RectI bounds(0, 0, 3, 2);

    SynthPixels pixelsA;
    pixelsA.push_back(SynthPixel(1, 1));
    pixelsA.back().samples.push_back(pointSample(7.f, 0.4f, 0.2f, 0.1f, 0.5f));
    pixelsA.back().samples.push_back(volumeSample(2.f, 4.f, 0.6f, 0.3f, 0.15f, 0.75f));
    pixelsA.push_back(SynthPixel(2, 0));
    pixelsA.back().samples.push_back(pointSample(1.f, 1.f, 1.f, 1.f, 1.f));

    NodePtr sourceA = createSyntheticSource(makeDeepImage(bounds, rgbaChannelNames(), pixelsA, false));
    NodePtr merge = createDeepMerge(DeepMerge::eOperationHoldout);
    ASSERT_TRUE(sourceA && merge);
    connectNodes(sourceA, merge, 0, true);

    DeepImagePtr held;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(merge, 1., bounds, &held));
    ASSERT_TRUE(held != NULL);
    EXPECT_TRUE(bounds == held->getBounds());
    EXPECT_FALSE(held->isTidy());
    EXPECT_EQ((U64)3, held->getSampleTable().getTotalSampleCount());

    for (std::size_t p = 0; p < pixelsA.size(); ++p) {
        const std::vector<ReadSample> samples = samplesAt(*held, pixelsA[p].x, pixelsA[p].y);
        ASSERT_EQ(pixelsA[p].samples.size(), samples.size());
        for (std::size_t s = 0; s < samples.size(); ++s) {
            expectSampleValues(samples[s], pixelsA[p].samples[s], 0.f);
        }
    }
}

TEST_F(DeepNodesTest, DeepFromImageOmitsSamplesForFullyTransparentPixels)
{
    const RectI frame(0, 0, kImageRenderTestWidth, kImageRenderTestHeight);
    const int seed = 3;
    const double depth = 12.5;

    NodePtr image = createImageSource(seed);
    NodePtr fromImage = createDeepFromImage(depth);
    ASSERT_TRUE(image && fromImage);
    connectNodes(image, fromImage, 0, true);

    DeepImagePtr deep;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(fromImage, 1., frame, &deep));
    ASSERT_TRUE(deep != NULL);
    EXPECT_TRUE(frame == deep->getBounds());

    U64 expectedSampleCount = 0;
    for (int y = frame.y1; y < frame.y2; ++y) {
        for (int x = frame.x1; x < frame.x2; ++x) {
            const std::vector<ReadSample> samples = samplesAt(*deep, x, y);
            if (imageRenderTestValue(seed, x, y, 3) <= 0.f) {
                EXPECT_EQ((std::size_t)0, samples.size()) << "at pixel (" << x << ", " << y << ")";
            } else {
                EXPECT_EQ((std::size_t)1, samples.size()) << "at pixel (" << x << ", " << y << ")";
                expectedSampleCount += samples.size();
            }
        }
    }
    EXPECT_EQ(expectedSampleCount, deep->getSampleTable().getTotalSampleCount());
}

TEST_F(DeepNodesTest, DeepFromImageThenDeepToImageReproducesTheImageAtAConstantDepth)
{
    const RectI frame(0, 0, kImageRenderTestWidth, kImageRenderTestHeight);
    const int seed = 3;
    const double depth = 12.5;

    NodePtr image = createImageSource(seed);
    NodePtr fromImage = createDeepFromImage(depth);
    ASSERT_TRUE(image && fromImage);
    connectNodes(image, fromImage, 0, true);

    DeepImagePtr deep;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(fromImage, 1., frame, &deep));
    ASSERT_TRUE(deep != NULL);
    EXPECT_TRUE(frame == deep->getBounds());
    EXPECT_TRUE(deep->isTidy());

    const std::vector<std::string> channels = rgbaChannelNames();
    for (int y = frame.y1; y < frame.y2; ++y) {
        for (int x = frame.x1; x < frame.x2; ++x) {
            const std::vector<ReadSample> samples = samplesAt(*deep, x, y);
            if (imageRenderTestValue(seed, x, y, 3) <= 0.f) {
                continue;
            }
            ASSERT_EQ((std::size_t)1, samples.size()) << "at pixel (" << x << ", " << y << ")";
            EXPECT_EQ((float)depth, samples[0].z) << "at pixel (" << x << ", " << y << ")";
            EXPECT_EQ((float)depth, samples[0].zback) << "at pixel (" << x << ", " << y << ")";
            for (std::size_t c = 0; c < channels.size(); ++c) {
                EXPECT_EQ(imageRenderTestValue(seed, x, y, (int)c), samples[0].value(channels[c])) << "at pixel (" << x << ", " << y << ") channel " << channels[c];
            }
        }
    }

    NodePtr toImage = createTrackedNode(PLUGINID_NATRON_DEEPTOIMAGE);
    ASSERT_TRUE(toImage != NULL);
    connectNodes(fromImage, toImage, 0, true);

    ImagePtr roundTrip;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderImageFrame(toImage, 1., frame, &roundTrip));
    ASSERT_TRUE(roundTrip != NULL);
    ASSERT_TRUE(roundTrip->getBounds().contains(frame));
    ASSERT_EQ(eImageBitDepthFloat, roundTrip->getBitDepth());
    ASSERT_EQ(4u, roundTrip->getComponentsCount());

    Image::ReadAccess access = roundTrip->getReadRights();
    for (int y = frame.y1; y < frame.y2; ++y) {
        for (int x = frame.x1; x < frame.x2; ++x) {
            const float* actual = imagePixel(access, x, y);
            ASSERT_TRUE(actual != NULL);
            if (imageRenderTestValue(seed, x, y, 3) <= 0.f) {
                for (int c = 0; c < 4; ++c) {
                    EXPECT_EQ(0.f, actual[c]) << "at pixel (" << x << ", " << y << ") channel " << c;
                }
                continue;
            }
            for (int c = 0; c < 4; ++c) {
                EXPECT_EQ(imageRenderTestValue(seed, x, y, c), actual[c]) << "at pixel (" << x << ", " << y << ") channel " << c;
            }
        }
    }
}

TEST_F(DeepNodesTest, DeepFromImageTakesItsDepthFromTheFirstChannelOfZ)
{
    const RectI frame(0, 0, kImageRenderTestWidth, kImageRenderTestHeight);
    const int seed = 3;
    const int depthSeed = 11;

    NodePtr image = createImageSource(seed);
    NodePtr depthImage = createImageSource(depthSeed);
    // The constant is a decoy here: with Z connected it must never be read.
    NodePtr fromImage = createDeepFromImage(-1.);
    NodePtr toImage = createTrackedNode(PLUGINID_NATRON_DEEPTOIMAGE);
    ASSERT_TRUE(image && depthImage && fromImage && toImage);
    connectNodes(image, fromImage, 0, true);
    connectNodes(depthImage, fromImage, 1, true);
    connectNodes(fromImage, toImage, 0, true);

    DeepImagePtr deep;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(fromImage, 1., frame, &deep));
    ASSERT_TRUE(deep != NULL);
    EXPECT_TRUE(frame == deep->getBounds());

    const std::vector<std::string> channels = rgbaChannelNames();
    for (int y = frame.y1; y < frame.y2; ++y) {
        for (int x = frame.x1; x < frame.x2; ++x) {
            const std::vector<ReadSample> samples = samplesAt(*deep, x, y);
            if (imageRenderTestValue(seed, x, y, 3) <= 0.f) {
                continue;
            }
            ASSERT_EQ((std::size_t)1, samples.size()) << "at pixel (" << x << ", " << y << ")";
            EXPECT_EQ(imageRenderTestValue(depthSeed, x, y, 0), samples[0].z) << "at pixel (" << x << ", " << y << ")";
            EXPECT_EQ(imageRenderTestValue(depthSeed, x, y, 0), samples[0].zback) << "at pixel (" << x << ", " << y << ")";
            for (std::size_t c = 0; c < channels.size(); ++c) {
                EXPECT_EQ(imageRenderTestValue(seed, x, y, (int)c), samples[0].value(channels[c])) << "at pixel (" << x << ", " << y << ") channel " << channels[c];
            }
        }
    }

    ImagePtr roundTrip;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderImageFrame(toImage, 1., frame, &roundTrip));
    ASSERT_TRUE(roundTrip != NULL);
    ASSERT_TRUE(roundTrip->getBounds().contains(frame));

    Image::ReadAccess access = roundTrip->getReadRights();
    for (int y = frame.y1; y < frame.y2; ++y) {
        for (int x = frame.x1; x < frame.x2; ++x) {
            const float* actual = imagePixel(access, x, y);
            ASSERT_TRUE(actual != NULL);
            if (imageRenderTestValue(seed, x, y, 3) <= 0.f) {
                for (int c = 0; c < 4; ++c) {
                    EXPECT_EQ(0.f, actual[c]) << "at pixel (" << x << ", " << y << ") channel " << c;
                }
                continue;
            }
            for (int c = 0; c < 4; ++c) {
                EXPECT_EQ(imageRenderTestValue(seed, x, y, c), actual[c]) << "at pixel (" << x << ", " << y << ") channel " << c;
            }
        }
    }
}

TEST_F(DeepNodesTest, DeepRecolorGivesSamplesTheColourImagesColourAndSharesWhatItDoesNotWrite)
{
    const RectI colorFrame(0, 0, kImageRenderTestWidth, kImageRenderTestHeight);
    const RectI deepBounds(0, 0, kImageRenderTestWidth + 2, kImageRenderTestHeight);
    const int seed = 5;
    const SynthPixels pixels = recolorPixels();

    NodePtr source = createSyntheticSource(makeDeepImage(deepBounds, rgbaChannelNames(), pixels, true));
    NodePtr color = createImageSource(seed);
    NodePtr recolor = createDeepRecolor(false);
    ASSERT_TRUE(source && color && recolor);

    EXPECT_EQ(2, recolor->getEffectInstance()->getNInputs());
    EXPECT_EQ(eDataKindDeep, recolor->getEffectInstance()->getInputDataKind(0));
    EXPECT_EQ(eDataKindImage, recolor->getEffectInstance()->getInputDataKind(1));
    EXPECT_EQ(eDataKindDeep, recolor->getEffectInstance()->getOutputDataKind());
    EXPECT_EQ("A", recolor->getEffectInstance()->getInputLabel(0));
    EXPECT_EQ("Color", recolor->getEffectInstance()->getInputLabel(1));
    {
        std::list<std::string> grouping;
        recolor->getEffectInstance()->getPluginGrouping(&grouping);
        ASSERT_EQ((std::size_t)1, grouping.size());
        EXPECT_EQ(PLUGIN_GROUP_DEEP, grouping.front());
    }
    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, recolor->canConnectInput(color, 0));
    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, recolor->canConnectInput(source, 1));
    connectNodes(source, recolor, 0, true);
    connectNodes(color, recolor, 1, true);

    DeepImagePtr recolored;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(recolor, 1., deepBounds, &recolored));
    ASSERT_TRUE(recolored != NULL);
    EXPECT_TRUE(deepBounds == recolored->getBounds());
    EXPECT_TRUE(recolored->isTidy());
    EXPECT_FALSE(recolor->hasPersistentMessage());

    DeepImagePtr input;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(source, 1., deepBounds, &input));
    ASSERT_TRUE(input != NULL);
    EXPECT_EQ(input->getSampleTable().getTotalSampleCount(), recolored->getSampleTable().getTotalSampleCount());

    // Only the colour was written: the sample table, the depths and the alpha are the input's
    // own storage, and the input's colour is untouched.
    EXPECT_TRUE(recolored->sharesSampleTableWith(*input));
    EXPECT_TRUE(recolored->sharesChannelStorageWith(*input, "Z"));
    EXPECT_TRUE(recolored->sharesChannelStorageWith(*input, "ZBack"));
    EXPECT_TRUE(recolored->sharesChannelStorageWith(*input, "A"));
    EXPECT_FALSE(recolored->sharesChannelStorageWith(*input, "R"));
    EXPECT_FALSE(recolored->sharesChannelStorageWith(*input, "G"));
    EXPECT_FALSE(recolored->sharesChannelStorageWith(*input, "B"));

    expectRecolored(*recolored, pixels, deepBounds, seed, colorFrame, false);
    for (std::size_t p = 0; p < pixels.size(); ++p) {
        const std::vector<ReadSample> samples = samplesAt(*input, pixels[p].x, pixels[p].y);
        ASSERT_EQ(pixels[p].samples.size(), samples.size());
        for (std::size_t s = 0; s < samples.size(); ++s) {
            expectSampleValues(samples[s], pixels[p].samples[s], 0.f);
        }
    }
}

TEST_F(DeepNodesTest, DeepRecolorTargetInputAlphaFlattensToTheColourImagesAlpha)
{
    const RectI colorFrame(0, 0, kImageRenderTestWidth, kImageRenderTestHeight);
    const RectI deepBounds(0, 0, kImageRenderTestWidth + 2, kImageRenderTestHeight);
    const int seed = 5;
    const SynthPixels pixels = recolorPixels();

    NodePtr source = createSyntheticSource(makeDeepImage(deepBounds, rgbaChannelNames(), pixels, true));
    NodePtr color = createImageSource(seed);
    NodePtr recolor = createDeepRecolor(true);
    ASSERT_TRUE(source && color && recolor);
    connectNodes(source, recolor, 0, true);
    connectNodes(color, recolor, 1, true);

    DeepImagePtr recolored;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(recolor, 1., deepBounds, &recolored));
    ASSERT_TRUE(recolored != NULL);
    EXPECT_TRUE(deepBounds == recolored->getBounds());

    DeepImagePtr input;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(source, 1., deepBounds, &input));
    ASSERT_TRUE(input != NULL);
    EXPECT_TRUE(recolored->sharesSampleTableWith(*input));
    EXPECT_TRUE(recolored->sharesChannelStorageWith(*input, "Z"));
    EXPECT_TRUE(recolored->sharesChannelStorageWith(*input, "ZBack"));
    EXPECT_FALSE(recolored->sharesChannelStorageWith(*input, "A"));
    EXPECT_FALSE(recolored->sharesChannelStorageWith(*input, "R"));

    expectRecolored(*recolored, pixels, deepBounds, seed, colorFrame, true);

    ImagePtr flattened = makeFloatRGBAImage(deepBounds);
    {
        DeepPixelScratch scratch;
        DeepTidyWorkspace work;
        ASSERT_EQ(eStatusOK, DeepFlatten::flattenToImage(*recolored, deepBounds, rgbaChannelNames(), 3, &scratch, &work, flattened));
    }
    Image::ReadAccess access = flattened->getReadRights();
    for (std::size_t p = 0; p < pixels.size(); ++p) {
        const int x = pixels[p].x;
        const int y = pixels[p].y;
        float inputTransmittance = 1.f;
        for (std::size_t s = 0; s < pixels[p].samples.size(); ++s) {
            inputTransmittance *= 1.f - pixels[p].samples[s].channels[3];
        }
        const float* actual = imagePixel(access, x, y);
        ASSERT_TRUE(actual != NULL);
        const std::vector<ReadSample> samples = samplesAt(*recolored, x, y);
        ASSERT_EQ(pixels[p].samples.size(), samples.size());
        if ((inputTransmittance > 0.f) && (inputTransmittance < 1.f)) {
            EXPECT_NEAR(recolorAlphaAt(seed, x, y, colorFrame), actual[3], 1e-5f) << "at pixel (" << x << ", " << y << ")";
            // The colour flattens to the image's too: it was scaled to the very alphas that do.
            if (recolorAlphaAt(seed, x, y, colorFrame) > 0.f) {
                for (int c = 0; c < 3; ++c) {
                    EXPECT_NEAR(imageRenderTestValue(seed, x, y, c), actual[c], 1e-5f) << "at pixel (" << x << ", " << y << ") channel " << c;
                }
            }
        } else {
            // Nothing covering the pixel, or something covering it in full, leaves the alphas be.
            for (std::size_t s = 0; s < samples.size(); ++s) {
                EXPECT_FLOAT_EQ(pixels[p].samples[s].channels[3], samples[s].value("A")) << "at pixel (" << x << ", " << y << ") sample " << s;
            }
        }
    }
}

TEST_F(DeepNodesTest, DeepRecolorOverAWiderCachedInputCopiesAndMatchesTheAliasedRender)
{
    const RectI colorFrame(0, 0, kImageRenderTestWidth, kImageRenderTestHeight);
    const RectI deepBounds(0, 0, kImageRenderTestWidth + 2, kImageRenderTestHeight);
    const RectI window(1, 0, 8, 5);
    const int seed = 5;
    const SynthPixels pixels = recolorPixels();

    NodePtr wideSource = createSyntheticSource(makeDeepImage(deepBounds, rgbaChannelNames(), pixels, true));
    NodePtr color = createImageSource(seed);
    NodePtr copied = createDeepRecolor(false);
    ASSERT_TRUE(wideSource && color && copied);
    connectNodes(wideSource, copied, 0, true);
    connectNodes(color, copied, 1, true);

    // Rendered over everything first, the source serves the narrower pull below from that wider
    // entry, so the recolour cannot alias it and has to copy.
    DeepImagePtr wide;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(wideSource, 1., deepBounds, &wide));
    ASSERT_TRUE(wide != NULL);
    EXPECT_TRUE(deepBounds == wide->getBounds());

    DeepImagePtr copiedOut;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(copied, 1., window, &copiedOut));
    ASSERT_TRUE(copiedOut != NULL);
    EXPECT_TRUE(window == copiedOut->getBounds());
    EXPECT_TRUE(copiedOut->isTidy());
    EXPECT_FALSE(copiedOut->sharesSampleTableWith(*wide));
    EXPECT_FALSE(copiedOut->sharesChannelStorageWith(*wide, "Z"));
    EXPECT_FALSE(copiedOut->sharesChannelStorageWith(*wide, "A"));
    expectRecolored(*copiedOut, pixels, window, seed, colorFrame, false);

    NodePtr narrowSource = createSyntheticSource(makeDeepImage(deepBounds, rgbaChannelNames(), pixels, true));
    NodePtr aliased = createDeepRecolor(false);
    ASSERT_TRUE(narrowSource && aliased);
    connectNodes(narrowSource, aliased, 0, true);
    connectNodes(color, aliased, 1, true);

    DeepImagePtr aliasedOut;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(aliased, 1., window, &aliasedOut));
    ASSERT_TRUE(aliasedOut != NULL);
    EXPECT_TRUE(window == aliasedOut->getBounds());

    DeepImagePtr narrow;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(narrowSource, 1., window, &narrow));
    ASSERT_TRUE(narrow != NULL);
    EXPECT_TRUE(window == narrow->getBounds());
    EXPECT_TRUE(aliasedOut->sharesSampleTableWith(*narrow));

    EXPECT_EQ(aliasedOut->getSampleTable().getTotalSampleCount(), copiedOut->getSampleTable().getTotalSampleCount());
    const std::vector<std::string> channels = rgbaChannelNames();
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const std::vector<ReadSample> expected = samplesAt(*aliasedOut, x, y);
            const std::vector<ReadSample> actual = samplesAt(*copiedOut, x, y);
            ASSERT_EQ(expected.size(), actual.size()) << "at pixel (" << x << ", " << y << ")";
            for (std::size_t s = 0; s < expected.size(); ++s) {
                EXPECT_EQ(expected[s].z, actual[s].z) << "at pixel (" << x << ", " << y << ") sample " << s;
                EXPECT_EQ(expected[s].zback, actual[s].zback) << "at pixel (" << x << ", " << y << ") sample " << s;
                for (std::size_t c = 0; c < channels.size(); ++c) {
                    EXPECT_EQ(expected[s].value(channels[c]), actual[s].value(channels[c])) << "at pixel (" << x << ", " << y << ") sample " << s << " channel " << channels[c];
                }
            }
        }
    }
}

TEST_F(DeepNodesTest, DeepRecolorRendersNothingWithoutAAndFailsWithAMessageWithoutAlphaOnA)
{
    const RectI frame(0, 0, kImageRenderTestWidth, kImageRenderTestHeight);

    NodePtr color = createImageSource(2);
    NodePtr recolor = createDeepRecolor(false);
    ASSERT_TRUE(color && recolor);
    connectNodes(color, recolor, 1, true);

    // With nothing on A the node has no region of definition, and the deep pipeline answers an
    // empty RoD with an empty image before any render action runs.
    DeepImagePtr out;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(recolor, 1., frame, &out));
    ASSERT_TRUE(out != NULL);
    EXPECT_TRUE(out->getBounds().isNull());
    EXPECT_EQ((U64)0, out->getSampleTable().getTotalSampleCount());

    std::vector<std::string> rgbOnly = rgbaChannelNames();
    rgbOnly.pop_back();
    SynthPixels pixels;
    pixels.push_back(SynthPixel(1, 1));
    pixels.back().samples.push_back(pointSample(1.f, 0.5f, 0.5f, 0.5f, 0.f));
    NodePtr noAlpha = createSyntheticSource(makeDeepImage(frame, rgbOnly, pixels, true));
    ASSERT_TRUE(noAlpha != NULL);
    connectNodes(noAlpha, recolor, 0, true);

    out.reset();
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeFailed, renderDeepFrame(recolor, 1., frame, &out));
    EXPECT_TRUE(recolor->hasPersistentMessage());
}

TEST_F(DeepNodesTest, DeepCropIsRegisteredAndInstantiable)
{
    NodePtr crop = createTrackedNode(PLUGINID_NATRON_DEEPCROP);

    ASSERT_TRUE(crop != NULL);
    EXPECT_EQ(1, crop->getEffectInstance()->getNInputs());
    EXPECT_EQ(eDataKindDeep, crop->getEffectInstance()->getInputDataKind(0));
    EXPECT_EQ(eDataKindDeep, crop->getEffectInstance()->getOutputDataKind());
    EXPECT_EQ("Source", crop->getEffectInstance()->getInputLabel(0));

    std::list<std::string> grouping;
    crop->getEffectInstance()->getPluginGrouping(&grouping);
    ASSERT_EQ((std::size_t)1, grouping.size());
    EXPECT_EQ(PLUGIN_GROUP_DEEP, grouping.front());

    NodePtr imageSource = createImageSource(0);
    ASSERT_TRUE(imageSource != NULL);
    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, crop->canConnectInput(imageSource, 0));
}

TEST_F(DeepNodesTest, DeepCropBBoxCropsToTheIntersectionAndLeavesSamplesInsideUnchanged)
{
    const RectI bounds(0, 0, 8, 8);
    // Bbox (2, 2, 3, 2) -> canonical (2, 2, 5, 4), already inside bounds, so the crop's own
    // bounds are exactly that rectangle.
    const RectI window(2, 2, 5, 4);

    SynthPixels pixels;
    pixels.push_back(SynthPixel(2, 2));
    pixels.back().samples.push_back(pointSample(1.f, 0.1f, 0.2f, 0.3f, 0.4f));
    pixels.back().samples.push_back(volumeSample(2.f, 3.f, 0.5f, 0.5f, 0.5f, 0.5f));
    pixels.push_back(SynthPixel(4, 3));
    pixels.back().samples.push_back(pointSample(5.f, 0.9f, 0.8f, 0.7f, 0.6f));
    // Outside the crop window on every side: dropped by the narrower bounds alone.
    pixels.push_back(SynthPixel(1, 2));
    pixels.back().samples.push_back(pointSample(2.f, 0.2f, 0.2f, 0.2f, 0.2f));
    pixels.push_back(SynthPixel(4, 4));
    pixels.back().samples.push_back(pointSample(1.f, 0.3f, 0.3f, 0.3f, 0.3f));
    pixels.push_back(SynthPixel(5, 3));
    pixels.back().samples.push_back(pointSample(1.f, 0.4f, 0.4f, 0.4f, 0.4f));
    pixels.push_back(SynthPixel(0, 0));
    pixels.back().samples.push_back(pointSample(9.f, 0.5f, 0.5f, 0.5f, 0.5f));

    NodePtr source = createSyntheticSource(makeDeepImage(bounds, rgbaChannelNames(), pixels, true));
    NodePtr crop = createDeepCrop(2., 2., 3., 2., true, 0., 0., false, false);
    ASSERT_TRUE(source && crop);
    connectNodes(source, crop, 0, true);

    DeepImagePtr cropped;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(crop, 1., bounds, &cropped));
    ASSERT_TRUE(cropped != NULL);
    EXPECT_TRUE(window == cropped->getBounds());
    EXPECT_TRUE(cropped->isTidy());
    EXPECT_EQ((U64)3, cropped->getSampleTable().getTotalSampleCount());

    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const std::vector<ReadSample> expected = synthSamplesAt(pixels, x, y);
            const std::vector<ReadSample> actual = samplesAt(*cropped, x, y);
            ASSERT_EQ(expected.size(), actual.size()) << "at pixel (" << x << ", " << y << ")";
            for (std::size_t s = 0; s < expected.size(); ++s) {
                EXPECT_FLOAT_EQ(expected[s].z, actual[s].z) << "at pixel (" << x << ", " << y << ") sample " << s;
                EXPECT_FLOAT_EQ(expected[s].zback, actual[s].zback) << "at pixel (" << x << ", " << y << ") sample " << s;
                const std::vector<std::string> channels = rgbaChannelNames();
                for (std::size_t c = 0; c < channels.size(); ++c) {
                    EXPECT_FLOAT_EQ(expected[s].value(channels[c]), actual[s].value(channels[c])) << "at pixel (" << x << ", " << y << ") sample " << s << " channel " << channels[c];
                }
            }
        }
    }
} // TEST_F(DeepNodesTest, DeepCropBBoxCropsToTheIntersectionAndLeavesSamplesInsideUnchanged)

TEST_F(DeepNodesTest, DeepCropZRangeDropsExactlyTheOutOfRangeSamplesAndKeepsOrder)
{
    const RectI bounds(0, 0, 5, 5);

    // Front to back, straddling both ends of [2, 6] so the boundary is tested both ways: kept at
    // exactly Near or Far, dropped when only partly inside (no splitting).
    const DeepSample below = pointSample(1.f, 0.1f, 0.1f, 0.1f, 0.1f);
    const DeepSample atNear = pointSample(2.f, 0.2f, 0.2f, 0.2f, 0.2f);
    const DeepSample straddlesNear = volumeSample(1.f, 3.f, 0.3f, 0.3f, 0.3f, 0.3f);
    const DeepSample inside = volumeSample(3.f, 6.f, 0.4f, 0.4f, 0.4f, 0.4f);
    const DeepSample straddlesFar = volumeSample(5.f, 7.f, 0.5f, 0.5f, 0.5f, 0.5f);
    const DeepSample atFar = pointSample(6.f, 0.6f, 0.6f, 0.6f, 0.6f);
    const DeepSample above = pointSample(7.f, 0.7f, 0.7f, 0.7f, 0.7f);

    SynthPixels pixels;
    pixels.push_back(SynthPixel(3, 3));
    pixels.back().samples.push_back(below);
    pixels.back().samples.push_back(atNear);
    pixels.back().samples.push_back(straddlesNear);
    pixels.back().samples.push_back(inside);
    pixels.back().samples.push_back(straddlesFar);
    pixels.back().samples.push_back(atFar);
    pixels.back().samples.push_back(above);

    NodePtr source = createSyntheticSource(makeDeepImage(bounds, rgbaChannelNames(), pixels, true));
    NodePtr crop = createDeepCrop(0., 0., 0., 0., false, 2., 6., true, false);
    ASSERT_TRUE(source && crop);
    connectNodes(source, crop, 0, true);

    DeepImagePtr cropped;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(crop, 1., bounds, &cropped));
    ASSERT_TRUE(cropped != NULL);
    EXPECT_TRUE(bounds == cropped->getBounds());
    EXPECT_EQ((U64)3, cropped->getSampleTable().getTotalSampleCount());

    const std::vector<ReadSample> samples = samplesAt(*cropped, 3, 3);
    ASSERT_EQ((std::size_t)3, samples.size());
    expectSampleValues(samples[0], atNear, 0.f);
    expectSampleValues(samples[1], inside, 0.f);
    expectSampleValues(samples[2], atFar, 0.f);
} // TEST_F(DeepNodesTest, DeepCropZRangeDropsExactlyTheOutOfRangeSamplesAndKeepsOrder)

TEST_F(DeepNodesTest, DeepCropWithNothingToDropIsAnIdentityAndSharesTheInputsCacheEntry)
{
    const RectI bounds(0, 0, kDeepFixtureWidth, kDeepFixtureHeight);

    SynthPixels pixels;
    pixels.push_back(SynthPixel(1, 1));
    pixels.back().samples.push_back(pointSample(1.f, 0.1f, 0.2f, 0.3f, 0.4f));
    pixels.push_back(SynthPixel(2, 0));
    pixels.back().samples.push_back(volumeSample(1.f, 3.f, 0.5f, 0.5f, 0.5f, 0.5f));

    NodePtr source = createSyntheticSource(makeDeepImage(bounds, rgbaChannelNames(), pixels, true));
    // Bbox comfortably contains the source's region of definition, and Use Z Range is off, so
    // nothing the crop would do actually changes anything.
    NodePtr crop = createDeepCrop(-100., -100., 300., 300., true, 0., 0., false, false);
    ASSERT_TRUE(source && crop);
    connectNodes(source, crop, 0, true);

    DeepImagePtr fromSource;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(source, 1., bounds, &fromSource));
    ASSERT_TRUE(fromSource != NULL);

    DeepImagePtr fromCrop;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(crop, 1., bounds, &fromCrop));
    ASSERT_TRUE(fromCrop != NULL);

    EXPECT_EQ(fromSource.get(), fromCrop.get());
    // The identity bypasses the crop's own cache lookup entirely, so it never mints an entry
    // under the crop's own key.
    EXPECT_TRUE(cachedDeepEntryBounds(crop, 1.).empty());
}

TEST_F(DeepNodesTest, DeepCropReformatSetsTheOutputFormatToBboxAndOffLeavesTheInputs)
{
    const RectI bounds(0, 0, 6, 6);

    SynthPixels pixels;
    pixels.push_back(SynthPixel(1, 1));
    pixels.back().samples.push_back(pointSample(1.f, 0.1f, 0.2f, 0.3f, 0.4f));

    NodePtr source = createSyntheticSource(makeDeepImage(bounds, rgbaChannelNames(), pixels, true));
    ASSERT_TRUE(source != NULL);
    source->getEffectInstance()->refreshMetadata_public(false);

    NodePtr cropOff = createDeepCrop(1., 1., 4., 3., true, 0., 0., false, false);
    NodePtr cropOn = createDeepCrop(1., 1., 4., 3., true, 0., 0., false, true);
    ASSERT_TRUE(cropOff && cropOn);
    connectNodes(source, cropOff, 0, true);
    connectNodes(source, cropOn, 0, true);

    cropOff->getEffectInstance()->refreshMetadata_public(false);
    cropOn->getEffectInstance()->refreshMetadata_public(false);

    EXPECT_TRUE(source->getEffectInstance()->getOutputFormat() == cropOff->getEffectInstance()->getOutputFormat());
    EXPECT_TRUE(RectI(1, 1, 5, 4) == cropOn->getEffectInstance()->getOutputFormat());
}

TEST_F(DeepNodesTest, DeepCropOutputFormatRefreshesWhenReformatOrBboxChangeWithNoExplicitRefresh)
{
    const RectI bounds(0, 0, 6, 6);

    SynthPixels pixels;
    pixels.push_back(SynthPixel(1, 1));
    pixels.back().samples.push_back(pointSample(1.f, 0.1f, 0.2f, 0.3f, 0.4f));

    NodePtr source = createSyntheticSource(makeDeepImage(bounds, rgbaChannelNames(), pixels, true));
    ASSERT_TRUE(source != NULL);
    source->getEffectInstance()->refreshMetadata_public(false);

    NodePtr crop = createDeepCrop(1., 1., 4., 3., true, 0., 0., false, false);
    ASSERT_TRUE(crop != NULL);
    connectNodes(source, crop, 0, true);
    crop->getEffectInstance()->refreshMetadata_public(false);

    EXPECT_TRUE(source->getEffectInstance()->getOutputFormat() == crop->getEffectInstance()->getOutputFormat());

    DeepImagePtr image;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(crop, 1., bounds, &image));
    ASSERT_TRUE(image != NULL);

    KnobBool* reformatKnob = dynamic_cast<KnobBool*>(crop->getKnobByName("reformat").get());
    ASSERT_TRUE(reformatKnob != NULL);
    reformatKnob->setValue(true);

    EXPECT_TRUE(RectI(1, 1, 5, 4) == crop->getEffectInstance()->getOutputFormat());

    KnobDouble* bboxKnob = dynamic_cast<KnobDouble*>(crop->getKnobByName("bbox").get());
    ASSERT_TRUE(bboxKnob != NULL);
    bboxKnob->setValue(6., ViewSpec::all(), 2);

    EXPECT_TRUE(RectI(1, 1, 7, 4) == crop->getEffectInstance()->getOutputFormat());
}

namespace {

// An 8x8 source's samples: point and volume samples both, at all four extremes and in the middle,
// so that a shift has something to land wherever it puts it.
SynthPixels
reformatPixels()
{
    SynthPixels pixels;

    pixels.push_back(SynthPixel(0, 0));
    pixels.back().samples.push_back(pointSample(1.f, 0.1f, 0.2f, 0.3f, 0.4f));
    pixels.push_back(SynthPixel(2, 2));
    pixels.back().samples.push_back(pointSample(2.f, 0.5f, 0.4f, 0.3f, 0.2f));
    pixels.back().samples.push_back(volumeSample(3.f, 5.f, 0.6f, 0.6f, 0.6f, 0.6f));
    pixels.push_back(SynthPixel(3, 6));
    pixels.back().samples.push_back(pointSample(6.f, 0.25f, 0.5f, 0.75f, 0.125f));
    pixels.push_back(SynthPixel(5, 5));
    pixels.back().samples.push_back(volumeSample(1.5f, 2.5f, 0.7f, 0.8f, 0.9f, 0.5f));
    pixels.push_back(SynthPixel(7, 7));
    pixels.back().samples.push_back(pointSample(4.f, 0.9f, 0.9f, 0.9f, 1.f));

    return pixels;
}

U64
totalSampleCount(const SynthPixels& pixels)
{
    U64 count = 0;

    for (std::size_t p = 0; p < pixels.size(); ++p) {
        count += (U64)pixels[p].samples.size();
    }

    return count;
}

// DeepReformat filters nothing, so its samples are held to exact equality rather than to a
// tolerance: a value that merely came close would mean arithmetic happened somewhere.
void
expectSamplesIdentical(const std::vector<ReadSample>& expected,
                       const std::vector<ReadSample>& actual,
                       int x,
                       int y)
{
    const std::vector<std::string> channels = rgbaChannelNames();

    ASSERT_EQ(expected.size(), actual.size()) << "at pixel (" << x << ", " << y << ")";
    for (std::size_t s = 0; s < expected.size(); ++s) {
        EXPECT_FLOAT_EQ(expected[s].z, actual[s].z) << "at pixel (" << x << ", " << y << ") sample " << s;
        EXPECT_FLOAT_EQ(expected[s].zback, actual[s].zback) << "at pixel (" << x << ", " << y << ") sample " << s;
        for (std::size_t c = 0; c < channels.size(); ++c) {
            EXPECT_FLOAT_EQ(expected[s].value(channels[c]), actual[s].value(channels[c])) << "at pixel (" << x << ", " << y << ") sample " << s << " channel " << channels[c];
        }
    }
}

} // namespace

TEST_F(DeepNodesTest, DeepReformatIsRegisteredAndInstantiable)
{
    NodePtr reformat = createTrackedNode(PLUGINID_NATRON_DEEPREFORMAT);

    ASSERT_TRUE(reformat != NULL);
    EXPECT_EQ(1, reformat->getEffectInstance()->getNInputs());
    EXPECT_EQ(eDataKindDeep, reformat->getEffectInstance()->getInputDataKind(0));
    EXPECT_EQ(eDataKindDeep, reformat->getEffectInstance()->getOutputDataKind());
    EXPECT_EQ("Source", reformat->getEffectInstance()->getInputLabel(0));

    std::list<std::string> grouping;
    reformat->getEffectInstance()->getPluginGrouping(&grouping);
    ASSERT_EQ((std::size_t)1, grouping.size());
    EXPECT_EQ(PLUGIN_GROUP_DEEP, grouping.front());

    NodePtr imageSource = createImageSource(0);
    ASSERT_TRUE(imageSource != NULL);
    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, reformat->canConnectInput(imageSource, 0));
}

TEST_F(DeepNodesTest, DeepReformatCentresByAWholePixelOffsetAndLeavesEverySampleUnchanged)
{
    const RectI bounds(0, 0, 8, 8);
    const SynthPixels pixels = reformatPixels();

    NodePtr source;
    NodePtr formatted = createDeepSourceWithFormat(makeDeepImage(bounds, rgbaChannelNames(), pixels, true), 8, 8, &source);
    ASSERT_TRUE(source && formatted);

    NodePtr reformat = createDeepReformat(12, 12, true, true);
    ASSERT_TRUE(reformat != NULL);
    connectNodes(formatted, reformat, 0, true);
    reformat->getEffectInstance()->refreshMetadata_public(false);

    EXPECT_TRUE(RectI(0, 0, 12, 12) == reformat->getEffectInstance()->getOutputFormat());

    DeepImagePtr out;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(reformat, 1., RectI(0, 0, 12, 12), &out));
    ASSERT_TRUE(out != NULL);
    EXPECT_TRUE(RectI(2, 2, 10, 10) == out->getBounds());
    EXPECT_TRUE(out->isTidy());
    EXPECT_EQ(totalSampleCount(pixels), out->getSampleTable().getTotalSampleCount());

    for (int y = bounds.y1; y < bounds.y2; ++y) {
        for (int x = bounds.x1; x < bounds.x2; ++x) {
            expectSamplesIdentical(synthSamplesAt(pixels, x, y), samplesAt(*out, x + 2, y + 2), x + 2, y + 2);
        }
    }
}

TEST_F(DeepNodesTest, DeepReformatOddSizeDifferenceTruncatesTheOffset)
{
    const RectI bounds(0, 0, 8, 8);
    const SynthPixels pixels = reformatPixels();

    NodePtr source;
    NodePtr formatted = createDeepSourceWithFormat(makeDeepImage(bounds, rgbaChannelNames(), pixels, true), 8, 8, &source);
    ASSERT_TRUE(source && formatted);

    NodePtr reformat = createDeepReformat(11, 11, true, true);
    ASSERT_TRUE(reformat != NULL);
    connectNodes(formatted, reformat, 0, true);
    reformat->getEffectInstance()->refreshMetadata_public(false);

    DeepImagePtr out;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(reformat, 1., RectI(0, 0, 11, 11), &out));
    ASSERT_TRUE(out != NULL);
    // (11 - 8) / 2 truncates to one whole pixel, not to a pixel and a half.
    EXPECT_TRUE(RectI(1, 1, 9, 9) == out->getBounds());
    EXPECT_EQ(totalSampleCount(pixels), out->getSampleTable().getTotalSampleCount());

    for (int y = bounds.y1; y < bounds.y2; ++y) {
        for (int x = bounds.x1; x < bounds.x2; ++x) {
            expectSamplesIdentical(synthSamplesAt(pixels, x, y), samplesAt(*out, x + 1, y + 1), x + 1, y + 1);
        }
    }
}

TEST_F(DeepNodesTest, DeepReformatWithCentreOffOnlyChangesTheFormatAndSharesTheInputsCacheEntry)
{
    const RectI bounds(0, 0, kDeepFixtureWidth, kDeepFixtureHeight);

    SynthPixels pixels;
    pixels.push_back(SynthPixel(1, 1));
    pixels.back().samples.push_back(pointSample(1.f, 0.1f, 0.2f, 0.3f, 0.4f));
    pixels.push_back(SynthPixel(2, 0));
    pixels.back().samples.push_back(volumeSample(1.f, 3.f, 0.5f, 0.5f, 0.5f, 0.5f));

    NodePtr source = createSyntheticSource(makeDeepImage(bounds, rgbaChannelNames(), pixels, true));
    ASSERT_TRUE(source != NULL);
    source->getEffectInstance()->refreshMetadata_public(false);

    NodePtr reformat = createDeepReformat(40, 30, true, false);
    ASSERT_TRUE(reformat != NULL);
    connectNodes(source, reformat, 0, true);
    reformat->getEffectInstance()->refreshMetadata_public(false);

    DeepImagePtr fromSource;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(source, 1., bounds, &fromSource));
    ASSERT_TRUE(fromSource != NULL);

    DeepImagePtr fromReformat;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(reformat, 1., bounds, &fromReformat));
    ASSERT_TRUE(fromReformat != NULL);

    EXPECT_EQ(fromSource.get(), fromReformat.get());
    EXPECT_TRUE(cachedDeepEntryBounds(reformat, 1.).empty());
    EXPECT_TRUE(RectI(0, 0, 40, 30) == reformat->getEffectInstance()->getOutputFormat());
}

TEST_F(DeepNodesTest, DeepReformatToTheInputsOwnFormatIsAnIdentity)
{
    const RectI bounds(0, 0, 8, 8);

    NodePtr source;
    NodePtr formatted = createDeepSourceWithFormat(makeDeepImage(bounds, rgbaChannelNames(), reformatPixels(), true), 8, 8, &source);
    ASSERT_TRUE(source && formatted);

    NodePtr reformat = createDeepReformat(8, 8, true, true);
    ASSERT_TRUE(reformat != NULL);
    connectNodes(formatted, reformat, 0, true);
    reformat->getEffectInstance()->refreshMetadata_public(false);

    DeepImagePtr fromInput;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(formatted, 1., bounds, &fromInput));
    ASSERT_TRUE(fromInput != NULL);

    DeepImagePtr fromReformat;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(reformat, 1., bounds, &fromReformat));
    ASSERT_TRUE(fromReformat != NULL);

    EXPECT_EQ(fromInput.get(), fromReformat.get());
    EXPECT_TRUE(cachedDeepEntryBounds(reformat, 1.).empty());
}

TEST_F(DeepNodesTest, DeepReformatKeepsSamplesPushedOutsideTheNewFormat)
{
    const RectI bounds(0, 0, 8, 8);
    const SynthPixels pixels = reformatPixels();

    NodePtr source;
    NodePtr formatted = createDeepSourceWithFormat(makeDeepImage(bounds, rgbaChannelNames(), pixels, true), 8, 8, &source);
    ASSERT_TRUE(source && formatted);

    NodePtr reformat = createDeepReformat(4, 4, true, true);
    ASSERT_TRUE(reformat != NULL);
    connectNodes(formatted, reformat, 0, true);
    reformat->getEffectInstance()->refreshMetadata_public(false);

    EXPECT_TRUE(RectI(0, 0, 4, 4) == reformat->getEffectInstance()->getOutputFormat());

    DeepImagePtr out;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(reformat, 1., RectI(-2, -2, 6, 6), &out));
    ASSERT_TRUE(out != NULL);
    // Shrinking the format pushes the input out past it on every side, and the region of
    // definition follows the samples rather than the format: nothing is dropped.
    EXPECT_TRUE(RectI(-2, -2, 6, 6) == out->getBounds());
    EXPECT_EQ(totalSampleCount(pixels), out->getSampleTable().getTotalSampleCount());

    expectSamplesIdentical(synthSamplesAt(pixels, 0, 0), samplesAt(*out, -2, -2), -2, -2);
    expectSamplesIdentical(synthSamplesAt(pixels, 7, 7), samplesAt(*out, 5, 5), 5, 5);
}

TEST_F(DeepNodesTest, DeepReformatAsksItsInputForATranslatedRegion)
{
    const RectI bounds(0, 0, 8, 8);
    const SynthPixels pixels = reformatPixels();

    NodePtr source;
    NodePtr formatted = createDeepSourceWithFormat(makeDeepImage(bounds, rgbaChannelNames(), pixels, true), 8, 8, &source);
    ASSERT_TRUE(source && formatted);

    NodePtr reformat = createDeepReformat(12, 12, true, true);
    ASSERT_TRUE(reformat != NULL);
    connectNodes(formatted, reformat, 0, true);
    reformat->getEffectInstance()->refreshMetadata_public(false);

    // Narrower than the region of definition (2, 2, 10, 10), so the render window is what
    // propagates upstream rather than the whole frame.
    DeepImagePtr out;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(reformat, 1., RectI(4, 4, 8, 8), &out));
    ASSERT_TRUE(out != NULL);
    EXPECT_TRUE(RectI(4, 4, 8, 8) == out->getBounds());

    const std::vector<RectI> upstream = cachedDeepEntryBounds(source, 1.);
    ASSERT_EQ((std::size_t)1, upstream.size());
    EXPECT_TRUE(RectI(2, 2, 6, 6) == upstream[0]);

    EXPECT_EQ((U64)3, out->getSampleTable().getTotalSampleCount());
    expectSamplesIdentical(synthSamplesAt(pixels, 2, 2), samplesAt(*out, 4, 4), 4, 4);
    expectSamplesIdentical(synthSamplesAt(pixels, 5, 5), samplesAt(*out, 7, 7), 7, 7);
}

TEST_F(DeepNodesTest, DeepReformatOutputFormatRefreshesWhenTheFormatKnobChangesWithNoExplicitRefresh)
{
    const RectI bounds(0, 0, 6, 6);

    SynthPixels pixels;
    pixels.push_back(SynthPixel(1, 1));
    pixels.back().samples.push_back(pointSample(1.f, 0.1f, 0.2f, 0.3f, 0.4f));

    NodePtr source = createSyntheticSource(makeDeepImage(bounds, rgbaChannelNames(), pixels, true));
    ASSERT_TRUE(source != NULL);
    source->getEffectInstance()->refreshMetadata_public(false);

    NodePtr reformat = createDeepReformat(6, 6, false, false);
    ASSERT_TRUE(reformat != NULL);
    connectNodes(source, reformat, 0, true);
    reformat->getEffectInstance()->refreshMetadata_public(false);

    Format projectFormat;
    getApp()->getProject()->getProjectDefaultFormat(&projectFormat);
    EXPECT_TRUE(RectI(0, 0, projectFormat.width(), projectFormat.height()) == reformat->getEffectInstance()->getOutputFormat());

    KnobBool* useCustomSize = dynamic_cast<KnobBool*>(reformat->getKnobByName("useCustomSize").get());
    KnobInt* customSize = dynamic_cast<KnobInt*>(reformat->getKnobByName("customSize").get());
    ASSERT_TRUE(useCustomSize && customSize);

    useCustomSize->setValue(true);
    EXPECT_TRUE(RectI(0, 0, 6, 6) == reformat->getEffectInstance()->getOutputFormat());

    customSize->setValue(10, ViewSpec::all(), 1);
    EXPECT_TRUE(RectI(0, 0, 6, 10) == reformat->getEffectInstance()->getOutputFormat());

    useCustomSize->setValue(false);
    EXPECT_TRUE(RectI(0, 0, projectFormat.width(), projectFormat.height()) == reformat->getEffectInstance()->getOutputFormat());

    // The Format choice is the host's to fill and to translate into the hidden size knob, so the
    // node only ever sees the size; picking another entry has to reach the metadata all the same.
    KnobChoice* formatChoice = dynamic_cast<KnobChoice*>(reformat->getKnobByName(kNatronParamFormatChoice).get());
    KnobInt* formatSize = dynamic_cast<KnobInt*>(reformat->getKnobByName(kNatronParamFormatSize).get());
    ASSERT_TRUE(formatChoice && formatSize);

    int otherIndex = -1;
    for (int i = 0; i < formatChoice->getNumEntries(); ++i) {
        Format f;
        if (getApp()->getProject()->getProjectFormatAtIndex(i, &f) && ((f.width() != projectFormat.width()) || (f.height() != projectFormat.height()))) {
            otherIndex = i;
            break;
        }
    }
    ASSERT_NE(-1, otherIndex);

    formatChoice->setValue(otherIndex);
    EXPECT_FALSE(RectI(0, 0, projectFormat.width(), projectFormat.height()) == reformat->getEffectInstance()->getOutputFormat());
    EXPECT_TRUE(RectI(0, 0, formatSize->getValue(0), formatSize->getValue(1)) == reformat->getEffectInstance()->getOutputFormat());
}

namespace {

std::vector<std::string>
expressionsRGBAZZBack(const char* r,
                      const char* g = "",
                      const char* b = "",
                      const char* a = "",
                      const char* z = "",
                      const char* zback = "")
{
    std::vector<std::string> expressions;

    expressions.push_back(r);
    expressions.push_back(g);
    expressions.push_back(b);
    expressions.push_back(a);
    expressions.push_back(z);
    expressions.push_back(zback);

    return expressions;
}

// Two pixels of two and three samples, volumetric and point ones both, tidy.
SynthPixels
expressionPixels()
{
    SynthPixels pixels;

    pixels.push_back(SynthPixel(1, 1));
    pixels.back().samples.push_back(pointSample(1.f, 0.1f, 0.2f, 0.3f, 0.25f));
    pixels.back().samples.push_back(volumeSample(2.f, 3.f, 0.4f, 0.5f, 0.6f, 0.5f));
    pixels.push_back(SynthPixel(3, 2));
    pixels.back().samples.push_back(pointSample(4.f, 0.5f, 0.5f, 0.5f, 0.6f));
    pixels.back().samples.push_back(pointSample(5.f, 0.f, 0.f, 0.f, 0.f));
    pixels.back().samples.push_back(pointSample(6.f, 0.7f, 0.7f, 0.7f, 0.75f));

    return pixels;
}

} // namespace

TEST_F(DeepNodesTest, DeepExpressionIsRegisteredAndInstantiable)
{
    NodePtr expression = createTrackedNode(PLUGINID_NATRON_DEEPEXPRESSION);

    ASSERT_TRUE(expression != NULL);
    EXPECT_EQ(1, expression->getEffectInstance()->getNInputs());
    EXPECT_EQ(eDataKindDeep, expression->getEffectInstance()->getInputDataKind(0));
    EXPECT_EQ(eDataKindDeep, expression->getEffectInstance()->getOutputDataKind());
    EXPECT_EQ("Source", expression->getEffectInstance()->getInputLabel(0));

    std::list<std::string> grouping;
    expression->getEffectInstance()->getPluginGrouping(&grouping);
    ASSERT_EQ((std::size_t)1, grouping.size());
    EXPECT_EQ(PLUGIN_GROUP_DEEP, grouping.front());

    static const char* const knobNames[] = { "expressionR", "expressionG", "expressionB", "expressionA", "expressionZ", "expressionZBack" };
    for (std::size_t c = 0; c < 6; ++c) {
        KnobString* knob = dynamic_cast<KnobString*>(expression->getKnobByName(knobNames[c]).get());
        ASSERT_TRUE(knob != NULL) << knobNames[c];
        EXPECT_TRUE(knob->getValue().empty()) << knobNames[c];
    }

    NodePtr imageSource = createImageSource(0);
    ASSERT_TRUE(imageSource != NULL);
    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, expression->canConnectInput(imageSource, 0));
}

TEST_F(DeepNodesTest, DeepExpressionOnAlphaHalvesItAndSharesEveryOtherChannel)
{
    const RectI bounds(0, 0, kDeepFixtureWidth, kDeepFixtureHeight);
    const SynthPixels pixels = expressionPixels();

    NodePtr source = createSyntheticSource(makeDeepImage(bounds, rgbaChannelNames(), pixels, true));
    NodePtr expression = createDeepExpression(expressionsRGBAZZBack("", "", "", "A*0.5"));
    ASSERT_TRUE(source && expression);
    connectNodes(source, expression, 0, true);

    DeepImagePtr out;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(expression, 1., bounds, &out));
    ASSERT_TRUE(out != NULL);
    EXPECT_TRUE(bounds == out->getBounds());
    EXPECT_TRUE(out->isTidy());
    EXPECT_FALSE(expression->hasPersistentMessage());

    DeepImagePtr input;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(source, 1., bounds, &input));
    ASSERT_TRUE(input != NULL);
    EXPECT_NE(input.get(), out.get());

    EXPECT_TRUE(out->sharesSampleTableWith(*input));
    EXPECT_TRUE(out->sharesChannelStorageWith(*input, "R"));
    EXPECT_TRUE(out->sharesChannelStorageWith(*input, "G"));
    EXPECT_TRUE(out->sharesChannelStorageWith(*input, "B"));
    EXPECT_TRUE(out->sharesChannelStorageWith(*input, "Z"));
    EXPECT_TRUE(out->sharesChannelStorageWith(*input, "ZBack"));
    EXPECT_FALSE(out->sharesChannelStorageWith(*input, "A"));

    for (std::size_t p = 0; p < pixels.size(); ++p) {
        const std::vector<ReadSample> samples = samplesAt(*out, pixels[p].x, pixels[p].y);
        ASSERT_EQ(pixels[p].samples.size(), samples.size());
        for (std::size_t s = 0; s < samples.size(); ++s) {
            DeepSample expected = pixels[p].samples[s];
            expected.channels[3] *= 0.5f;
            expectSampleValues(samples[s], expected, 0.f);
        }
        const std::vector<ReadSample> untouched = samplesAt(*input, pixels[p].x, pixels[p].y);
        ASSERT_EQ(pixels[p].samples.size(), untouched.size());
        for (std::size_t s = 0; s < untouched.size(); ++s) {
            expectSampleValues(untouched[s], pixels[p].samples[s], 0.f);
        }
    }
}

TEST_F(DeepNodesTest, DeepExpressionOnDepthMovesSamplesAndClearsTidiness)
{
    const RectI bounds(0, 0, kDeepFixtureWidth, kDeepFixtureHeight);
    const SynthPixels pixels = expressionPixels();

    NodePtr source = createSyntheticSource(makeDeepImage(bounds, rgbaChannelNames(), pixels, true));
    // Reading ZBack while writing Z checks that expressions see the input's depths, not the
    // ones being written.
    NodePtr expression = createDeepExpression(expressionsRGBAZZBack("", "", "", "", "ZBack + 10", "Z"));
    ASSERT_TRUE(source && expression);
    connectNodes(source, expression, 0, true);

    DeepImagePtr out;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(expression, 1., bounds, &out));
    ASSERT_TRUE(out != NULL);
    EXPECT_FALSE(out->isTidy());
    EXPECT_FALSE(expression->hasPersistentMessage());

    DeepImagePtr input;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(source, 1., bounds, &input));
    ASSERT_TRUE(input != NULL);
    EXPECT_TRUE(input->isTidy());
    EXPECT_TRUE(out->sharesSampleTableWith(*input));
    EXPECT_TRUE(out->sharesChannelStorageWith(*input, "R"));
    EXPECT_TRUE(out->sharesChannelStorageWith(*input, "A"));
    EXPECT_FALSE(out->sharesChannelStorageWith(*input, "Z"));
    EXPECT_FALSE(out->sharesChannelStorageWith(*input, "ZBack"));

    for (std::size_t p = 0; p < pixels.size(); ++p) {
        const std::vector<ReadSample> samples = samplesAt(*out, pixels[p].x, pixels[p].y);
        ASSERT_EQ(pixels[p].samples.size(), samples.size());
        for (std::size_t s = 0; s < samples.size(); ++s) {
            DeepSample expected = pixels[p].samples[s];
            expected.z = pixels[p].samples[s].zback + 10.f;
            expected.zback = pixels[p].samples[s].z;
            expectSampleValues(samples[s], expected, 0.f);
        }
    }
}

TEST_F(DeepNodesTest, DeepExpressionCreatesAChannelTheInputLacks)
{
    const RectI bounds(0, 0, kDeepFixtureWidth, kDeepFixtureHeight);
    const SynthPixels pixels = expressionPixels();

    std::vector<std::string> rgbOnly = rgbaChannelNames();
    rgbOnly.pop_back();
    NodePtr source = createSyntheticSource(makeDeepImage(bounds, rgbOnly, pixels, true));
    NodePtr expression = createDeepExpression(expressionsRGBAZZBack("", "", "", "R + G", "", "Z + 0.5"));
    ASSERT_TRUE(source && expression);
    connectNodes(source, expression, 0, true);

    DeepImagePtr out;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(expression, 1., bounds, &out));
    ASSERT_TRUE(out != NULL);
    EXPECT_FALSE(out->isTidy());
    EXPECT_FALSE(expression->hasPersistentMessage());

    DeepImagePtr input;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(source, 1., bounds, &input));
    ASSERT_TRUE(input != NULL);
    EXPECT_FALSE(input->hasChannel("A"));
    ASSERT_TRUE(out->hasChannel("A"));
    EXPECT_TRUE(out->sharesSampleTableWith(*input));
    EXPECT_TRUE(out->sharesChannelStorageWith(*input, "R"));
    EXPECT_TRUE(out->sharesChannelStorageWith(*input, "G"));
    EXPECT_TRUE(out->sharesChannelStorageWith(*input, "B"));
    EXPECT_TRUE(out->sharesChannelStorageWith(*input, "Z"));
    EXPECT_FALSE(out->sharesChannelStorageWith(*input, "ZBack"));

    for (std::size_t p = 0; p < pixels.size(); ++p) {
        const std::vector<ReadSample> samples = samplesAt(*out, pixels[p].x, pixels[p].y);
        ASSERT_EQ(pixels[p].samples.size(), samples.size());
        for (std::size_t s = 0; s < samples.size(); ++s) {
            const DeepSample& in = pixels[p].samples[s];
            EXPECT_FLOAT_EQ(in.z, samples[s].z);
            EXPECT_FLOAT_EQ(in.z + 0.5f, samples[s].zback);
            EXPECT_FLOAT_EQ(in.channels[0], samples[s].value("R"));
            EXPECT_FLOAT_EQ(in.channels[0] + in.channels[1], samples[s].value("A"));
        }
    }
}

TEST_F(DeepNodesTest, DeepExpressionBuiltinsGiveEverySampleItsOwnValues)
{
    const RectI bounds(0, 0, kDeepFixtureWidth, kDeepFixtureHeight);
    const SynthPixels pixels = expressionPixels();

    NodePtr source = createSyntheticSource(makeDeepImage(bounds, rgbaChannelNames(), pixels, true));
    NodePtr expression = createDeepExpression(expressionsRGBAZZBack("sampleIndex", "sampleCount", "x + 100 * y", "sampleIndex == sampleCount - 1 ? frame : 0"));
    ASSERT_TRUE(source && expression);
    connectNodes(source, expression, 0, true);

    DeepImagePtr out;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(expression, 3., bounds, &out));
    ASSERT_TRUE(out != NULL);
    EXPECT_TRUE(out->isTidy());

    for (std::size_t p = 0; p < pixels.size(); ++p) {
        const std::vector<ReadSample> samples = samplesAt(*out, pixels[p].x, pixels[p].y);
        ASSERT_EQ(pixels[p].samples.size(), samples.size());
        for (std::size_t s = 0; s < samples.size(); ++s) {
            EXPECT_FLOAT_EQ((float)s, samples[s].value("R"));
            EXPECT_FLOAT_EQ((float)pixels[p].samples.size(), samples[s].value("G"));
            EXPECT_FLOAT_EQ((float)(pixels[p].x + 100 * pixels[p].y), samples[s].value("B"));
            EXPECT_FLOAT_EQ((s + 1 == pixels[p].samples.size()) ? 3.f : 0.f, samples[s].value("A"));
            EXPECT_FLOAT_EQ(pixels[p].samples[s].z, samples[s].z);
            EXPECT_FLOAT_EQ(pixels[p].samples[s].zback, samples[s].zback);
        }
    }
}

TEST_F(DeepNodesTest, DeepExpressionWithNoExpressionIsAnIdentity)
{
    const RectI bounds(0, 0, kDeepFixtureWidth, kDeepFixtureHeight);

    NodePtr source = createSyntheticSource(makeDeepImage(bounds, rgbaChannelNames(), expressionPixels(), true));
    NodePtr expression = createDeepExpression(expressionsRGBAZZBack("", "  ", "\t"));
    ASSERT_TRUE(source && expression);
    connectNodes(source, expression, 0, true);

    DeepImagePtr fromSource;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(source, 1., bounds, &fromSource));
    DeepImagePtr fromExpression;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(expression, 1., bounds, &fromExpression));
    ASSERT_TRUE(fromSource && fromExpression);
    EXPECT_EQ(fromSource.get(), fromExpression.get());
}

TEST_F(DeepNodesTest, DeepExpressionThatDoesNotCompileFailsWithAMessageNamingTheChannelAndColumn)
{
    const RectI bounds(0, 0, kDeepFixtureWidth, kDeepFixtureHeight);

    NodePtr source = createSyntheticSource(makeDeepImage(bounds, rgbaChannelNames(), expressionPixels(), true));
    NodePtr expression = createDeepExpression(expressionsRGBAZZBack("R", "G * (1 + ", "", "nope"));
    ASSERT_TRUE(source && expression);
    connectNodes(source, expression, 0, true);

    DeepImagePtr out;
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeFailed, renderDeepFrame(expression, 1., bounds, &out));
    ASSERT_TRUE(expression->hasPersistentMessage());
    QString message;
    int type = 0;
    expression->getPersistentMessage(&message, &type, false);
    EXPECT_TRUE(message.startsWith(QString::fromUtf8("G: "))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8("(column 10)"))) << message.toStdString();

    // Fixing the expression clears the message on the next render.
    KnobString* g = dynamic_cast<KnobString*>(expression->getKnobByName("expressionG").get());
    KnobString* a = dynamic_cast<KnobString*>(expression->getKnobByName("expressionA").get());
    ASSERT_TRUE(g && a);
    g->setValue("G * 2");
    a->setValue("A");
    out.reset();
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderDeepFrame(expression, 1., bounds, &out));
    ASSERT_TRUE(out != NULL);
    EXPECT_FALSE(expression->hasPersistentMessage());
}
