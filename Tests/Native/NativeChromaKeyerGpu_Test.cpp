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
#include <cmath>
#include <cstdio>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include <ofxNatron.h>

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
#include "Engine/Nodes/Keyer/ChromaKeyer.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

struct KeySetting {
    const char* name;
    double key[3];
    int colorspace;
    bool linear;
    double acceptance;
    double suppression;
    double lift;
    double gain;
};

// The defaults on a green key; a linear Rec. 2020 blue key with every ramp knob moved; a CCIR 601
// key whose angles take the "everything" branches with the lift at its step; and a zero gain
// with a full suppression angle.
const KeySetting kSettings[] = {
    { "green", { 0.1, 0.8, 0.2 }, 1, false, 120., 40., 0., 1. },
    { "blue linear rec2020", { 0.15, 0.25, 0.9 }, 2, true, 90., 20., 0.2, 1.4 },
    { "ccir601 open angles, full lift", { 0.2, 0.7, 0.3 }, 0, false, 180., 0., 1., 1. },
    { "zero gain, full suppression", { 0.3, 0.9, 0.1 }, 1, false, 150., 180., 0.3, 0. },
};

void
applySetting(const NodePtr& keyer,
             const KeySetting& s)
{
    ASSERT_TRUE(setKnobValues(keyer, kChromaKeyerParamKeyColor, std::vector<double>(s.key, s.key + 3)));
    ASSERT_TRUE(setKnobValues(keyer, kChromaKeyerParamColorspace, std::vector<double>(1, (double)s.colorspace)));
    ASSERT_TRUE(setKnobValues(keyer, kChromaKeyerParamLinear, std::vector<double>(1, s.linear ? 1. : 0.)));
    ASSERT_TRUE(setKnobValues(keyer, kChromaKeyerParamAcceptanceAngle, std::vector<double>(1, s.acceptance)));
    ASSERT_TRUE(setKnobValues(keyer, kChromaKeyerParamSuppressionAngle, std::vector<double>(1, s.suppression)));
    ASSERT_TRUE(setKnobValues(keyer, kChromaKeyerParamKeyLift, std::vector<double>(1, s.lift)));
    ASSERT_TRUE(setKnobValues(keyer, kChromaKeyerParamKeyGain, std::vector<double>(1, s.gain)));
}

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

void
selectMaskAlpha(const NodePtr& keyer)
{
    const char* const names[2] = { "maskChannel_InM", "maskChannel_OutM" };

    for (const char* name : names) {
        KnobChannelSelect* select = dynamic_cast<KnobChannelSelect*>(keyer->getKnobByName(name).get());
        ASSERT_TRUE(select != NULL) << name;
        select->set("rgba.A");
    }
}

#ifdef NATRON_HAVE_GPU
const char* const kShowNames[4] = { "intermediate", "premultiplied", "unpremultiplied", "composite" };
const char* const kSourceAlphaNames[3] = { "ignore", "insidemask", "normal" };

// The unpremultiplied output divides the suppressed colour by the key alpha, so a one-ulp
// difference in the float key, which the CPU rounds from double, grows by the inverse of an alpha
// that can be near zero.
const ParityTolerance kUnpremultipliedTolerance = ParityTolerance::make(0., 1e-3);

const ParityTolerance&
toleranceForShow(int show)
{
    static const ParityTolerance transcendental = ParityTolerance::transcendental();

    return (show == 2) ? kUnpremultipliedTolerance : transcendental;
}

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

void
enableKeyMasks(const NodePtr& keyer,
               bool enabled)
{
    ASSERT_TRUE(setKnobValues(keyer, "enableMask_InM", std::vector<double>(1, enabled ? 1. : 0.)));
    ASSERT_TRUE(setKnobValues(keyer, "enableMask_OutM", std::vector<double>(1, enabled ? 1. : 0.)));
}

GpuParityImage
imageOf(const RenderedPlane& plane)
{
    GpuParityImage image;

    image.bounds = plane.window;
    image.nComps = (int)plane.channels.size();
    image.pixels = plane.pixels;

    return image;
}

// The query the placement builds for a task rendering the keyer's RGBA plane.
GpuQuery
queryFor(const NodePtr& keyer)
{
    StagedImageEffect* effect = effectOf(keyer);
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
#endif // NATRON_HAVE_GPU

} // namespace

class NativeChromaKeyerGpuTest
    : public BaseTest {
};

#ifdef NATRON_HAVE_GPU

// The kernel against the node's own render() over the source's region of definition, for every
// output mode and source alpha option, with and without Bg and the two masks, on an RGBA and an
// RGB source, and with alpha left unprocessed. Bg and the masks sit at their own origins, so each
// covers only part of the window, and every other render lays the views out with padding.
TEST_F(NativeChromaKeyerGpuTest, MatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr bg = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr inMask = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr outMask = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr withBg = createNode(QString::fromUtf8(PLUGINID_NATRON_CHROMAKEYER));
    NodePtr withoutBg = createNode(QString::fromUtf8(PLUGINID_NATRON_CHROMAKEYER));
    ASSERT_TRUE(bool(source) && bool(bg) && bool(inMask) && bool(outMask) && bool(withBg) && bool(withoutBg));
    setParitySourceOrigin(bg, 8, 4);
    setParitySourceOrigin(inMask, -6, 10);
    setParitySourceOrigin(outMask, 12, 6);
    const NodePtr keyers[2] = { withoutBg, withBg };
    for (const NodePtr& keyer : keyers) {
        connectNodes(source, keyer, kChromaKeyerInputSource, true);
        connectNodes(inMask, keyer, kChromaKeyerInputInsideMask, true);
        connectNodes(outMask, keyer, kChromaKeyerInputOutsideMask, true);
        selectMaskAlpha(keyer);
        if (HasFatalFailure()) {
            return;
        }
    }
    connectNodes(bg, withBg, kChromaKeyerInputBg, true);

    const RectI window = paritySourceWindow(source, kTime, 0);
    RenderedPlane bgPlane;
    RenderedPlane inMaskPlane;
    RenderedPlane outMaskPlane;
    ASSERT_TRUE(renderPlane(bg, paritySourceWindow(bg, kTime, 0), 4, &bgPlane));
    ASSERT_TRUE(renderPlane(inMask, paritySourceWindow(inMask, kTime, 0), 4, &inMaskPlane));
    ASSERT_TRUE(renderPlane(outMask, paritySourceWindow(outMask, kTime, 0), 4, &outMaskPlane));

    const char* const components[2] = { "rgba", "rgb" };
    int nCase = 0;
    double worst = 0.;
    double worstUnpremultiplied = 0.;

    for (const char* componentsID : components) {
        setParitySourceComponents(source, componentsID);
        // The source in the layer each keyer's render() fetches it in: without a Bg an RGB source
        // is fetched in the RGBA output's layout, its alpha zero.
        RenderedPlane sourcePlanes[2];
        for (int k = 0; k < 2; ++k) {
            ImageLayerDesc layer;
            ASSERT_TRUE(effectOf(keyers[k])->resolveInputPlaneForRender(kChromaKeyerInputSource, kTime, ViewIdx(0), &layer, NULL));
            ASSERT_TRUE(renderPlane(source, window, layer.getNumComponents(), &sourcePlanes[k]));
        }

        for (const KeySetting& setting : kSettings) {
            for (int config = 0; config < 4; ++config) {
                const bool hasBg = (config & 1) != 0;
                const bool hasMasks = (config & 2) != 0;
                const NodePtr& keyer = keyers[hasBg ? 1 : 0];
                const RenderedPlane& sourcePlane = sourcePlanes[hasBg ? 1 : 0];
                StagedImageEffect* effect = effectOf(keyer);
                ASSERT_TRUE(effect != NULL);
                applySetting(keyer, setting);
                enableKeyMasks(keyer, hasMasks);
                if (HasFatalFailure()) {
                    return;
                }
                for (int show = 0; show < 4; ++show) {
                    for (int sourceAlpha = 0; sourceAlpha < 3; ++sourceAlpha) {
                        // Alpha is left unprocessed once per setting and input set.
                        const bool alphaUnprocessed = (show == 3) && (sourceAlpha == 1);
                        ASSERT_TRUE(setKnobValues(keyer, kChromaKeyerParamShow, std::vector<double>(1, (double)show)));
                        ASSERT_TRUE(setKnobValues(keyer, kChromaKeyerParamSourceAlpha, std::vector<double>(1, (double)sourceAlpha)));
                        setProcessedChannels(keyer, alphaUnprocessed ? 0x7u : 0xFu);
                        if (HasFatalFailure()) {
                            return;
                        }

                        const std::string what = std::string(componentsID) + " source, " + setting.name + ", " + (hasBg ? "Bg" : "no Bg") + ", " + (hasMasks ? "masks" : "no masks") + ", " + kShowNames[show] + ", source alpha " + kSourceAlphaNames[sourceAlpha] + (alphaUnprocessed ? ", alpha unprocessed" : "");
                        const GpuQuery query = queryFor(keyer);
                        ASSERT_TRUE(effect->canRenderOnGpu(query)) << what;

                        RenderedPlane cpu;
                        ASSERT_TRUE(renderPlane(keyer, window, 4, &cpu)) << what;

                        std::vector<GpuParityImage> inputs(4);
                        inputs[kChromaKeyerInputSource] = imageOf(sourcePlane);
                        if (hasMasks) {
                            inputs[kChromaKeyerInputInsideMask] = imageOf(inMaskPlane);
                            inputs[kChromaKeyerInputOutsideMask] = imageOf(outMaskPlane);
                        }
                        if (hasBg) {
                            inputs[kChromaKeyerInputBg] = imageOf(bgPlane);
                        }
                        GpuViewLayout layout;
                        if (nCase++ % 2) {
                            for (int i = 0; i < 4; ++i) {
                                inputs[i].layout.padFloats = 3 + 4 * i;
                                inputs[i].layout.rowPadFloats = 1 + 2 * i;
                            }
                            layout.padFloats = 8;
                            layout.rowPadFloats = 5;
                            layout.marginLeft = 3;
                            layout.marginBottom = 2;
                            layout.marginRight = 4;
                            layout.marginTop = 1;
                        }
                        const GpuRunResult run = runRenderGpu(*runtime, *effect, query, inputs, nullptr, window, layout);
                        ASSERT_TRUE(run.ok) << what;
                        EXPECT_EQ(0u, run.writtenOutside) << what;
                        ASSERT_EQ(cpu.pixels.size(), run.pixels.size()) << what;

                        const ImageDiff diff = compareImages(cpu.pixels, run.pixels, toleranceForShow(show));
                        worst = std::max(worst, diff.worstAbsolute);
                        if (show == 2) {
                            for (std::size_t i = 0; i < cpu.pixels.size(); ++i) {
                                const double scale = std::max(1., std::fabs((double)cpu.pixels[i]));
                                worstUnpremultiplied = std::max(worstUnpremultiplied, std::fabs((double)cpu.pixels[i] - (double)run.pixels[i]) / scale);
                            }
                        }
                        EXPECT_EQ(0u, diff.mismatches) << what << ": " << diff.mismatches << " mismatches, first at float " << diff.firstMismatchIndex
                                                       << " gpu " << run.pixels[diff.firstMismatchIndex] << " cpu " << cpu.pixels[diff.firstMismatchIndex]
                                                       << ", worst " << diff.worstAbsolute << " at float " << diff.worstIndex;
                    }
                }
            }
        }
    }
    std::printf("chromakeyer gpu vs cpu: %d renders, worst absolute difference %g, worst unpremultiplied relative difference %g\n", nCase, worst,
                worstUnpremultiplied);
}

#endif // NATRON_HAVE_GPU

// Grade, ChromaKeyer and Grade form one region, the keyer reading its Source from the first Grade
// and its Bg and inside mask from nodes outside the region, on an RGBA and an RGB source.
TEST_F(NativeChromaKeyerGpuTest, GradeChromaKeyerGradeChainRunsAsOneRegion)
{
    const char* const components[2] = { "rgba", "rgb" };

    for (const char* componentsID : components) {
        NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        NodePtr bg = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        NodePtr inMask = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        NodePtr first = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
        NodePtr keyer = createNode(QString::fromUtf8(PLUGINID_NATRON_CHROMAKEYER));
        NodePtr second = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
        ASSERT_TRUE(bool(source) && bool(bg) && bool(inMask) && bool(first) && bool(keyer) && bool(second));
        setParitySourceComponents(source, componentsID);
        setParitySourceOrigin(bg, 8, 4);
        setParitySourceOrigin(inMask, -6, 10);
        connectNodes(source, first, 0, true);
        connectNodes(first, keyer, kChromaKeyerInputSource, true);
        connectNodes(bg, keyer, kChromaKeyerInputBg, true);
        connectNodes(inMask, keyer, kChromaKeyerInputInsideMask, true);
        connectNodes(keyer, second, 0, true);
        selectMaskAlpha(keyer);
        if (HasFatalFailure()) {
            return;
        }
        ASSERT_TRUE(setKnobValues(keyer, "enableMask_InM", std::vector<double>(1, 1.)));
        applySetting(keyer, kSettings[0]);
        if (HasFatalFailure()) {
            return;
        }
        ASSERT_TRUE(setKnobValues(first, "multiply", std::vector<double>(4, 1.5)));
        ASSERT_TRUE(setKnobValues(second, "offset", std::vector<double>(4, 0.25)));

        const RectI window = paritySourceWindow(source, kTime, 0);
        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ first, keyer, second }), second, window, kTime);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << componentsID << ": " << result.error;

        int holding = 0;
        for (const std::vector<NodePtr>& nodes : result.regionNodes) {
            const bool hasFirst = std::find(nodes.begin(), nodes.end(), first) != nodes.end();
            const bool hasKeyer = std::find(nodes.begin(), nodes.end(), keyer) != nodes.end();
            const bool hasSecond = std::find(nodes.begin(), nodes.end(), second) != nodes.end();
            if (hasFirst || hasKeyer || hasSecond) {
                ++holding;
                EXPECT_TRUE(hasFirst && hasKeyer && hasSecond) << componentsID;
            }
        }
        EXPECT_EQ(1, holding) << componentsID;
        EXPECT_TRUE(result.ranInRegion(keyer)) << componentsID;

        ASSERT_EQ(result.cpu.size(), result.gpu.size());
        const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::transcendental());
        EXPECT_EQ(0u, diff.mismatches) << componentsID << ": first at float " << diff.firstMismatchIndex << " gpu " << result.gpu[diff.firstMismatchIndex]
                                       << " cpu " << result.cpu[diff.firstMismatchIndex];
    }
}
