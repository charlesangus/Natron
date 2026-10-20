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
#include <bitset>
#include <cstdio>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "Tests/BaseTest.h"
#include "Tests/Native/NativeGpuParity.h"
#include "Tests/NativeParity.h"
#include "Tests/RenderBothWays.h"

#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Keyer/Keyer.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

const double kGreenKey[3] = { 0.1, 0.8, 0.2 };

// Leaves the channels of `processed` (out of R, G, B, A) to the node and the rest to the source;
// all four restore the layer row's default of every channel the plane has.
void
setProcessedChannels(const NodePtr& node,
                     unsigned processed)
{
    KnobChannelSetPtr channelSet = std::dynamic_pointer_cast<KnobChannelSet>(node->getKnobByName(kNodeParamChannelSet));
    ASSERT_TRUE(bool(channelSet));
    std::vector<ChannelSetRow> rows = channelSet->getRows();
    ASSERT_FALSE(rows.empty());
    ASSERT_EQ(ChannelSetRow::eModeLayer, rows[0].mode);
    rows[0].channels.clear();
    if (processed != 0xFu) {
        const char* const names[4] = { "R", "G", "B", "A" };
        for (int c = 0; c < 4; ++c) {
            if ((processed >> c) & 1u) {
                rows[0].channels.push_back(names[c]);
            }
        }
    }
    channelSet->setRows(rows);
}

const char* const kModes[] = { "luminance", "color", "screen", "none" };
const char* const kShows[] = { "intermediate", "premultiplied", "unpremultiplied", "composite" };
const char* const kSourceAlphas[] = { "ignore", "inside", "normal" };

// Which of the optional inputs a case connects.
struct InputSet {
    const char* name;
    bool inMask;
    bool outMask;
    bool bg;
};

const InputSet kInputSets[] = {
    { "source only", false, false, false },
    { "InM", true, false, false },
    { "OutM", false, true, false },
    { "Bg", false, false, true },
    { "InM, OutM and Bg", true, true, true },
};

#ifdef NATRON_HAVE_GPU
StagedImageEffect*
effectOf(const NodePtr& node)
{
    return node ? dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get()) : NULL;
}

bool
renderPlane(const NodePtr& node,
            const RectI& window,
            int nComps,
            RenderedPlane* plane)
{
    std::list<ImageLayerDesc> layers(1, ImageLayerDesc::mapNCompsToColorLayer(nComps));
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, &error);

    EXPECT_TRUE(ok) << error;
    if (!ok || (planes.size() != 1)) {
        return false;
    }
    *plane = planes[0];

    return true;
}
#endif // NATRON_HAVE_GPU

bool
setChannelSelect(const NodePtr& node,
                 const std::string& name,
                 const std::string& value)
{
    KnobChannelSelect* select = node ? dynamic_cast<KnobChannelSelect*>(node->getKnobByName(name).get()) : NULL;

    EXPECT_TRUE(select != NULL) << name;
    if (!select) {
        return false;
    }
    select->set(value);

    return true;
}

// The mode first, then the key colour: both edits reset the thresholds to span the key.
void
setModeAndKey(const NodePtr& keyer,
              const char* mode)
{
    ASSERT_TRUE(setKnobValue(keyer, kKeyerParamMode, std::string(mode)));
    ASSERT_TRUE(setKnobValues(keyer, kKeyerParamKeyColor, std::vector<double>(kGreenKey, kGreenKey + 3)));
}

// The planes of the four parity sources that feed the Keyers, by input number.
struct KeyerSources {
    NodePtr source;
    NodePtr inMask;
    NodePtr outMask;
    NodePtr bg;
    GpuParityImage images[4];
};

#ifdef NATRON_HAVE_GPU
GpuParityImage
imageOf(const RenderedPlane& plane)
{
    GpuParityImage image;

    image.bounds = plane.window;
    image.nComps = (int)plane.channels.size();
    image.pixels = plane.pixels;

    return image;
}

// The query the placement builds for a task rendering the RGBA plane of the Keyer.
GpuQuery
queryFor(StagedImageEffect* effect)
{
    const ImageLayerDesc plane = ImageLayerDesc::mapNCompsToColorLayer(4);
    GpuQuery q;

    q.time = kTime;
    q.view = ViewIdx(0);
    q.nComps = 4;
    q.bitDepth = eImageBitDepthFloat;
    const int maskInput = effect->getMaskInput();
    q.hasMask = (maskInput >= 0) && effect->getInput(maskInput) && effect->isMaskEnabled(maskInput);
    const std::bitset<4> channels = effect->getProcessChannelsForPlane(effect->getRenderHash(), kTime, ViewIdx(0), plane);
    for (int i = 0; i < 4; ++i) {
        q.processChannels[i] = channels[i];
    }

    return q;
}

// The GPU render of keyer over window against its CPU render, with the inputs of `inputs`.
void
expectGpuMatchesCpu(GpuParityRuntime& runtime,
                    const NodePtr& keyer,
                    const KeyerSources& sources,
                    const InputSet& inputs,
                    const RectI& window,
                    bool padded,
                    const std::string& name,
                    double* worst)
{
    StagedImageEffect* effect = effectOf(keyer);
    ASSERT_TRUE(effect != NULL);
    const GpuQuery query = queryFor(effect);
    ASSERT_TRUE(effect->canRenderOnGpu(query)) << name;

    RenderedPlane cpu;
    ASSERT_TRUE(renderPlane(keyer, window, 4, &cpu)) << name;

    std::vector<GpuParityImage> images(4);
    images[kKeyerInputSource] = sources.images[kKeyerInputSource];
    if (inputs.inMask) {
        images[kKeyerInputInsideMask] = sources.images[kKeyerInputInsideMask];
    }
    if (inputs.outMask) {
        images[kKeyerInputOutsideMask] = sources.images[kKeyerInputOutsideMask];
    }
    if (inputs.bg) {
        images[kKeyerInputBg] = sources.images[kKeyerInputBg];
    }
    GpuViewLayout layout;
    if (padded) {
        for (int i = 0; i < 4; ++i) {
            images[i].layout.padFloats = 3 + 4 * i;
            images[i].layout.rowPadFloats = 1 + 2 * i;
        }
        layout.padFloats = 8;
        layout.rowPadFloats = 5;
        layout.marginLeft = 3;
        layout.marginBottom = 2;
        layout.marginRight = 4;
        layout.marginTop = 1;
    }
    const GpuRunResult run = runRenderGpu(runtime, *effect, query, images, nullptr, window, layout);
    ASSERT_TRUE(run.ok) << name;
    EXPECT_EQ(0u, run.writtenOutside) << name;
    ASSERT_EQ(cpu.pixels.size(), run.pixels.size()) << name;

    const ImageDiff diff = compareImages(cpu.pixels, run.pixels, ParityTolerance::transcendental());
    EXPECT_EQ(0u, diff.mismatches) << name << ": first at float " << diff.firstMismatchIndex << " gpu " << run.pixels[diff.firstMismatchIndex]
                                   << " cpu " << cpu.pixels[diff.firstMismatchIndex];
    *worst = std::max(*worst, diff.worstAbsolute);
}
#endif // NATRON_HAVE_GPU

} // namespace

class NativeKeyerGpuTest
    : public BaseTest {
protected:
    // A Keyer on source with the inputs of `inputs` connected, the masks reading their alpha.
    NodePtr makeKeyer(const NodePtr& source,
                      const NodePtr& inMask,
                      const NodePtr& outMask,
                      const NodePtr& bg,
                      const InputSet& inputs)
    {
        NodePtr keyer = createNode(QString::fromUtf8(PLUGINID_NATRON_KEYER));

        EXPECT_TRUE(bool(keyer));
        if (!keyer) {
            return keyer;
        }
        connectNodes(source, keyer, kKeyerInputSource, true);
        const bool used[2] = { inputs.inMask, inputs.outMask };
        const NodePtr masks[2] = { inMask, outMask };
        const int maskInputs[2] = { kKeyerInputInsideMask, kKeyerInputOutsideMask };
        const char* const labels[2] = { "InM", "OutM" };
        for (int k = 0; k < 2; ++k) {
            if (!used[k]) {
                continue;
            }
            connectNodes(masks[k], keyer, maskInputs[k], true);
            EXPECT_TRUE(setKnobValues(keyer, std::string("enableMask_") + labels[k], std::vector<double>(1, 1.)));
            EXPECT_TRUE(setChannelSelect(keyer, std::string("maskChannel_") + labels[k], "rgba.A"));
        }
        if (inputs.bg) {
            connectNodes(bg, keyer, kKeyerInputBg, true);
        }

        return keyer;
    }

#ifdef NATRON_HAVE_GPU
    // The four parity sources, an RGB or RGBA source and RGBA masks and Bg, and their planes.
    bool makeSources(int sourceNComps,
                     KeyerSources* s)
    {
        s->source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        s->inMask = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        s->outMask = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        s->bg = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        EXPECT_TRUE(bool(s->source) && bool(s->inMask) && bool(s->outMask) && bool(s->bg));
        if (!s->source || !s->inMask || !s->outMask || !s->bg) {
            return false;
        }
        if (sourceNComps == 3) {
            setParitySourceComponents(s->source, "rgb");
        }
        // Each optional input covers only part of the source's window, so that the Keyer reads past
        // its edges.
        setParitySourceOrigin(s->inMask, -6, 10);
        setParitySourceOrigin(s->outMask, 12, 6);
        setParitySourceOrigin(s->bg, 8, 4);
        const NodePtr nodes[4] = { s->source, s->inMask, s->outMask, s->bg };
        for (int i = 0; i < 4; ++i) {
            RenderedPlane plane;
            if (!renderPlane(nodes[i], paritySourceWindow(nodes[i], kTime, 0), (i == 0) ? sourceNComps : 4, &plane)) {
                return false;
            }
            s->images[i] = imageOf(plane);
        }

        return true;
    }
#endif
};

#ifdef NATRON_HAVE_GPU

// Every key mode, output mode and source alpha handling, with no optional input, each one alone,
// and all three, against the node's own render() over the source's window, which the Bg and the
// masks only partly cover. Despill strengths below and above 1 alternate.
TEST_F(NativeKeyerGpuTest, MatchesTheCpuRenderInEveryMode)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    KeyerSources sources;
    ASSERT_TRUE(makeSources(4, &sources));
    // The Keyer's region of definition: the region engine never asks renderGpu() for pixels
    // outside it.
    const RectI window = paritySourceWindow(sources.source, kTime, 0);
    double worst = 0.;
    int nCase = 0;

    for (const InputSet& inputs : kInputSets) {
        NodePtr keyer = makeKeyer(sources.source, sources.inMask, sources.outMask, sources.bg, inputs);
        ASSERT_TRUE(bool(keyer));
        for (const char* mode : kModes) {
            setModeAndKey(keyer, mode);
            if (HasFatalFailure()) {
                return;
            }
            for (const char* show : kShows) {
                for (const char* sourceAlpha : kSourceAlphas) {
                    ASSERT_TRUE(setKnobValue(keyer, kKeyerParamShow, std::string(show)));
                    ASSERT_TRUE(setKnobValue(keyer, kKeyerParamSourceAlpha, std::string(sourceAlpha)));
                    ASSERT_TRUE(setKnobValues(keyer, kKeyerParamDespill, std::vector<double>(1, (nCase % 2) ? 1.5 : 0.7)));
                    ASSERT_TRUE(setKnobValues(keyer, kKeyerParamDespillAngle, std::vector<double>(1, (nCase % 3) ? 120. : 150.)));
                    const std::string name = std::string(inputs.name) + ", " + mode + ", " + show + ", source alpha " + sourceAlpha;
                    expectGpuMatchesCpu(*runtime, keyer, sources, inputs, window, (nCase % 2) != 0, name, &worst);
                    if (HasFatalFailure()) {
                        return;
                    }
                    ++nCase;
                }
            }
        }
    }
    std::printf("keyer gpu vs cpu: %d renders, worst absolute difference %g\n", nCase, worst);
}

// The luminance options in luminance mode, and thresholds the key colour does not span: a soft
// lower edge below zero, a hard upper edge above one, and a centre whose tolerances leave no soft
// range.
TEST_F(NativeKeyerGpuTest, MatchesTheCpuRenderForEveryLuminanceAndThreshold)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    KeyerSources sources;
    ASSERT_TRUE(makeSources(4, &sources));
    const RectI window = paritySourceWindow(sources.source, kTime, 0);
    const InputSet& inputs = kInputSets[4];
    NodePtr keyer = makeKeyer(sources.source, sources.inMask, sources.outMask, sources.bg, inputs);
    ASSERT_TRUE(bool(keyer));
    double worst = 0.;

    const char* const luminances[] = { "rec709", "rec2020", "acesap0", "acesap1", "ccir601", "average", "max" };
    struct Thresholds {
        double softnessLower;
        double toleranceLower;
        double center;
        double toleranceUpper;
        double softnessUpper;
    };
    const Thresholds thresholds[] = {
        { -0.5, 0., 1., 0., 0.5 },
        { -0.8, -0.3, 0.2, 0.1, 0.3 },
        { -0.25, 0., 0.9, 0.4, 0. },
        { 0., -0.2, 0.5, 0.2, 0. },
    };
    for (const char* luminance : luminances) {
        for (const char* mode : { "luminance", "color" }) {
            setModeAndKey(keyer, mode);
            if (HasFatalFailure()) {
                return;
            }
            ASSERT_TRUE(setKnobValue(keyer, "luminanceMath", std::string(luminance)));
            for (const Thresholds& t : thresholds) {
                ASSERT_TRUE(setKnobValues(keyer, kKeyerParamSoftnessLower, std::vector<double>(1, t.softnessLower)));
                ASSERT_TRUE(setKnobValues(keyer, kKeyerParamToleranceLower, std::vector<double>(1, t.toleranceLower)));
                ASSERT_TRUE(setKnobValues(keyer, kKeyerParamCenter, std::vector<double>(1, t.center)));
                ASSERT_TRUE(setKnobValues(keyer, kKeyerParamToleranceUpper, std::vector<double>(1, t.toleranceUpper)));
                ASSERT_TRUE(setKnobValues(keyer, kKeyerParamSoftnessUpper, std::vector<double>(1, t.softnessUpper)));
                ASSERT_TRUE(setKnobValue(keyer, kKeyerParamShow, std::string("composite")));
                const std::string name = std::string(mode) + ", " + luminance + ", center " + std::to_string(t.center);
                expectGpuMatchesCpu(*runtime, keyer, sources, inputs, window, false, name, &worst);
                if (HasFatalFailure()) {
                    return;
                }
            }
        }
    }
    std::printf("keyer gpu vs cpu over luminances and thresholds: worst absolute difference %g\n", worst);
}

// An RGB source has no alpha: "Add to Inside Mask" adds nothing, "Normal" composites with an
// alpha of one, and an unprocessed alpha is written as zero.
TEST_F(NativeKeyerGpuTest, RgbSourceReadsNoAlpha)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    KeyerSources sources;
    ASSERT_TRUE(makeSources(3, &sources));
    const RectI window = paritySourceWindow(sources.source, kTime, 0);
    const InputSet& inputs = kInputSets[4];
    NodePtr keyer = makeKeyer(sources.source, sources.inMask, sources.outMask, sources.bg, inputs);
    ASSERT_TRUE(bool(keyer));
    double worst = 0.;

    for (const char* mode : kModes) {
        setModeAndKey(keyer, mode);
        if (HasFatalFailure()) {
            return;
        }
        for (const char* sourceAlpha : { "inside", "normal" }) {
            for (bool processAlpha : { true, false }) {
                ASSERT_TRUE(setKnobValue(keyer, kKeyerParamShow, std::string("composite")));
                ASSERT_TRUE(setKnobValue(keyer, kKeyerParamSourceAlpha, std::string(sourceAlpha)));
                setProcessedChannels(keyer, processAlpha ? 0xFu : 0x7u);
                if (HasFatalFailure()) {
                    return;
                }
                const std::string name = std::string(mode) + ", source alpha " + sourceAlpha + (processAlpha ? "" : ", alpha not processed");
                expectGpuMatchesCpu(*runtime, keyer, sources, inputs, window, true, name, &worst);
                if (HasFatalFailure()) {
                    return;
                }
            }
        }
    }
    std::printf("keyer gpu vs cpu on an RGB source: worst absolute difference %g\n", worst);
}

// Channels the host selector leaves out pass the source through.
TEST_F(NativeKeyerGpuTest, UnprocessedChannelsPassTheSourceThrough)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    KeyerSources sources;
    ASSERT_TRUE(makeSources(4, &sources));
    const RectI window = paritySourceWindow(sources.source, kTime, 0);
    const InputSet& inputs = kInputSets[4];
    NodePtr keyer = makeKeyer(sources.source, sources.inMask, sources.outMask, sources.bg, inputs);
    ASSERT_TRUE(bool(keyer));
    setModeAndKey(keyer, "screen");
    if (HasFatalFailure()) {
        return;
    }
    ASSERT_TRUE(setKnobValue(keyer, kKeyerParamShow, std::string("premultiplied")));
    double worst = 0.;
    for (unsigned mask : { 0x5u, 0xAu, 0x7u }) {
        setProcessedChannels(keyer, mask);
        if (HasFatalFailure()) {
            return;
        }
        expectGpuMatchesCpu(*runtime, keyer, sources, inputs, window, true, "channels " + std::to_string(mask), &worst);
        if (HasFatalFailure()) {
            return;
        }
    }
}

#endif // NATRON_HAVE_GPU

// Grade -> Keyer -> Grade forms one region, with the Bg and the inside mask uploaded from outside
// it, the mask in its own layer.
TEST_F(NativeKeyerGpuTest, GradeKeyerGradeChainRunsAsOneRegion)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr bg = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr mask = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr first = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    NodePtr second = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(bool(source) && bool(bg) && bool(mask) && bool(first) && bool(second));
    setParitySourceOrigin(bg, 8, 4);
    setParitySourceOrigin(mask, -6, 10);
    connectNodes(source, first, 0, true);
    InputSet inputs = { "InM and Bg", true, false, true };
    NodePtr keyer = makeKeyer(first, mask, NodePtr(), bg, inputs);
    ASSERT_TRUE(bool(keyer));
    connectNodes(keyer, second, 0, true);
    ASSERT_TRUE(setKnobValues(first, "multiply", std::vector<double>(4, 1.25)));
    ASSERT_TRUE(setKnobValues(second, "offset", std::vector<double>(4, 0.25)));
    setModeAndKey(keyer, "screen");
    if (HasFatalFailure()) {
        return;
    }
    ASSERT_TRUE(setKnobValue(keyer, kKeyerParamShow, std::string("composite")));
    ASSERT_TRUE(setKnobValue(keyer, kKeyerParamSourceAlpha, std::string("normal")));

    const RectI window = paritySourceWindow(source, kTime, 0);
    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ first, keyer, second }), second, window, kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.ranInRegion(keyer));

    int holding = 0;
    for (const std::vector<NodePtr>& nodes : result.regionNodes) {
        const bool hasFirst = std::find(nodes.begin(), nodes.end(), first) != nodes.end();
        const bool hasKeyer = std::find(nodes.begin(), nodes.end(), keyer) != nodes.end();
        const bool hasSecond = std::find(nodes.begin(), nodes.end(), second) != nodes.end();
        if (hasFirst || hasKeyer || hasSecond) {
            ++holding;
            EXPECT_TRUE(hasFirst && hasKeyer && hasSecond);
        }
    }
    EXPECT_EQ(1, holding);

    ASSERT_EQ(result.cpu.size(), result.gpu.size());
    const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::transcendental());
    EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex << " gpu " << result.gpu[diff.firstMismatchIndex] << " cpu "
                                   << result.cpu[diff.firstMismatchIndex];
}
