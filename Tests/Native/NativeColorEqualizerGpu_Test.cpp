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

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "Tests/BaseTest.h"
#include "Tests/Native/NativeGpuParity.h"
#include "Tests/Native/RegionIndependenceSupport.h"
#include "Tests/NativeParity.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/ColorEqualizer.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const double kTime = 1.;

// The GPU runs the same guided filters through EdgeAwareGpu's recursive blurs and float32
// transcendentals, so it matches the CPU to within the blurs' rounding, not bit for bit.
ParityTolerance
gpuTolerance()
{
    return ParityTolerance::make(2e-3, 1e-3);
}

ColorEqualizer*
effectOf(const NodePtr& node)
{
    return node ? dynamic_cast<ColorEqualizer*>(node->getEffectInstance().get()) : NULL;
}

void
setWorkingSpace(const NodePtr& node,
                const std::string& name)
{
    KnobChoicePtr workingSpace = node->getApp()->getProject()->getKnobByNameAndType<KnobChoice>("workingSpace");
    ASSERT_TRUE(bool(workingSpace));
    workingSpace->setValueFromID(name, 0);
    ASSERT_EQ(name, node->getApp()->getProject()->getWorkingColorSpace());
}

void
setNode(const NodePtr& node,
        const char* prefix,
        const double values[ColorEqualizerParams::kNodes])
{
    for (int i = 0; i < ColorEqualizerParams::kNodes; ++i) {
        const std::string name = std::string(prefix) + ColorEqualizer::nodeName(i);
        ASSERT_TRUE(setKnobValues(node, name, std::vector<double>(1, values[i]))) << name;
    }
}

void
setBusy(const NodePtr& node)
{
    const double hue[] = { 20., -15., 10., 35., -25., 12., -8., 30. };
    const double saturation[] = { 1.3, 0.7, 1.1, 0.6, 1.4, 0.9, 1.2, 0.8 };
    const double brightness[] = { 1.2, 0.85, 1.1, 0.75, 1.15, 0.9, 1.05, 0.8 };

    setNode(node, kColorEqualizerParamHuePrefix, hue);
    setNode(node, kColorEqualizerParamSaturationPrefix, saturation);
    setNode(node, kColorEqualizerParamBrightnessPrefix, brightness);
    ASSERT_TRUE(setKnobValues(node, kColorEqualizerParamNodePlacement, std::vector<double>(1, 10.)));
    ASSERT_TRUE(setKnobValues(node, kColorEqualizerParamHueCurveSmoothing, std::vector<double>(1, 0.6)));
    ASSERT_TRUE(setKnobValues(node, kColorEqualizerParamSaturationThreshold, std::vector<double>(1, 0.05)));
    ASSERT_TRUE(setKnobValues(node, kColorEqualizerParamContrast, std::vector<double>(1, -0.3)));
    ASSERT_TRUE(setKnobValues(node, kColorEqualizerParamWhiteLevel, std::vector<double>(1, 2.)));
}

void
setUseFilter(ColorEqualizer* effect,
             bool on)
{
    KnobBoolPtr knob = effect->getKnobByNameAndType<KnobBool>(kColorEqualizerParamUseFilter);
    ASSERT_TRUE(bool(knob));
    knob->setValue(on);
}

class NativeColorEqualizerGpuTest
    : public BaseTest {
protected:
    // The equalizer on a parity source; false when no GPU is available.
    void build()
    {
        _source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        _node = createNode(QString::fromUtf8(PLUGINID_NATRON_COLOREQUALIZER), PLUGIN_MAJOR_NATRON_COLOREQUALIZER);
        ASSERT_TRUE(bool(_source));
        ASSERT_TRUE(bool(_node));
        connectNodes(_source, _node, kColorEqualizerInputSource, true);
    }

    void expectGpuMatchesCpu(const std::string& what)
    {
        const RectI window = paritySourceWindow(_source, kTime, 0);
        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>(1, _node), _node, window, kTime);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << what << ": " << result.error;
        EXPECT_TRUE(result.ranInRegion(_node)) << what;
        ASSERT_EQ(result.cpu.size(), result.gpu.size()) << what;
        const ImageDiff diff = compareImages(result.cpu, result.gpu, gpuTolerance());
        EXPECT_EQ(0u, diff.mismatches) << what << ": first at float " << diff.firstMismatchIndex << " gpu "
                                       << result.gpu[diff.firstMismatchIndex] << " cpu " << result.cpu[diff.firstMismatchIndex]
                                       << ", worst " << diff.worstAbsolute;
    }

    NodePtr _source;
    NodePtr _node;
};
} // namespace

// Defaults and every control moved, with and without the filter, in the default working space
// and in Linear Rec.2020.
TEST_F(NativeColorEqualizerGpuTest, MatchesTheCpuRender)
{
    build();
    if (HasFatalFailure()) {
        return;
    }
    ColorEqualizer* effect = effectOf(_node);
    ASSERT_TRUE(effect != NULL);

    const std::string defaultSpace = _node->getApp()->getProject()->getWorkingColorSpace();
    const std::string spaces[] = { defaultSpace, "Linear Rec.2020" };
    for (const std::string& space : spaces) {
        setWorkingSpace(_node, space);
        if (HasFatalFailure()) {
            return;
        }
        expectGpuMatchesCpu(space + " defaults");
        setBusy(_node);
        if (HasFatalFailure()) {
            return;
        }
        expectGpuMatchesCpu(space + " every control");
        setUseFilter(effect, false);
        expectGpuMatchesCpu(space + " no filter");
        setUseFilter(effect, true);
        if (HasFatalFailure()) {
            return;
        }
    }
}

// Chroma and param sizes large enough that both guided filters run on a downsampled grid.
TEST_F(NativeColorEqualizerGpuTest, MatchesTheCpuRenderDownsampled)
{
    build();
    if (HasFatalFailure()) {
        return;
    }
    setBusy(_node);
    ASSERT_TRUE(setKnobValues(_node, kColorEqualizerParamChromaSize, std::vector<double>(1, 10.)));
    ASSERT_TRUE(setKnobValues(_node, kColorEqualizerParamParamSize, std::vector<double>(1, 19.)));
    expectGpuMatchesCpu("downsampled");
}

// Every output view.
TEST_F(NativeColorEqualizerGpuTest, MatchesTheCpuRenderForEveryOutput)
{
    build();
    if (HasFatalFailure()) {
        return;
    }
    setBusy(_node);
    if (HasFatalFailure()) {
        return;
    }
    for (int output = ColorEqualizerParams::eOutputHue; output <= ColorEqualizerParams::eOutputBrightnessThreshold; ++output) {
        ASSERT_TRUE(setKnobValues(_node, kColorEqualizerParamOutput, std::vector<double>(1, (double)output)));
        expectGpuMatchesCpu("output " + std::to_string(output));
        if (HasFatalFailure()) {
            return;
        }
    }
}

// Every darktable preset.
TEST_F(NativeColorEqualizerGpuTest, MatchesTheCpuRenderForEveryPreset)
{
    build();
    if (HasFatalFailure()) {
        return;
    }
    ColorEqualizer* effect = effectOf(_node);
    ASSERT_TRUE(effect != NULL);
    for (int i = 0; i < ColorEqualizer::presetCount(); ++i) {
        effect->applyPresetAt(i);
        expectGpuMatchesCpu(std::string("preset ") + ColorEqualizer::presetLabel(i));
        if (HasFatalFailure()) {
            return;
        }
    }
}

#ifdef NATRON_HAVE_GPU
// An OCIO working space with no matrix to XYZ converts on the CPU only.
TEST_F(NativeColorEqualizerGpuTest, RefusesANonMatrixWorkingSpace)
{
    build();
    if (HasFatalFailure()) {
        return;
    }
    ColorEqualizer* effect = effectOf(_node);
    ASSERT_TRUE(effect != NULL);

    setWorkingSpace(_node, "ACEScct");
    if (HasFatalFailure()) {
        return;
    }
    GpuQuery query;
    query.time = kTime;
    query.nComps = 4;
    EXPECT_FALSE(effect->canRenderOnGpu(query));
}
#endif // NATRON_HAVE_GPU

namespace {

class ColorEqualizerGpuRegionTest
    : public RegionIndependenceTest {
protected:
    // The equalizer of source with the Kodachrome 64-like preset, which subsamples its parameter
    // filter by 4, showing output.
    NodePtr makeEqualizer(const NodePtr& source,
                          int output)
    {
        NodePtr node = createAfter(PLUGINID_NATRON_COLOREQUALIZER, source, kColorEqualizerInputSource);
        ColorEqualizer* effect = effectOf(node);

        EXPECT_TRUE(effect != NULL);
        if (!effect) {
            return node;
        }
        effect->applyPresetAt(1);
        EXPECT_TRUE(setKnobValues(node, kColorEqualizerParamOutput, { (double)output }));

        return node;
    }

    // The query a full-scale render of node's RGBA plane makes.
    static GpuQuery rgbaQuery()
    {
        GpuQuery query;

        query.time = kTime;
        query.view = ViewIdx(0);
        query.mappedScale = RenderScale::identity;
        query.plane = ImageLayerDesc::getRGBAComponents();
        query.nComps = 4;
        query.bitDepth = eImageBitDepthFloat;
        for (int i = 0; i < 4; ++i) {
            query.processChannels[i] = true;
        }

        return query;
    }
};

#ifdef NATRON_HAVE_GPU
// A frame too large for one device buffer is refused before anything is recorded; the same node
// over a frame that fits is accepted.
TEST_F(ColorEqualizerGpuRegionTest, OversizedFrameStaysOnTheCpu)
{
    NodePtr small = createRegionSource(RectI(0, 0, 203, 157), false);
    NodePtr fits = makeEqualizer(small, ColorEqualizerParams::eOutputResult);
    ASSERT_TRUE(effectOf(fits) != NULL);
    EXPECT_TRUE(effectOf(fits)->canRenderOnGpu(rgbaQuery()));

    NodePtr huge = createRegionSource(RectI(0, 0, 70000, 70000), false);
    NodePtr oversized = makeEqualizer(huge, ColorEqualizerParams::eOutputResult);
    ASSERT_TRUE(effectOf(oversized) != NULL);
    EXPECT_FALSE(effectOf(oversized)->canRenderOnGpu(rgbaQuery()));
}

// GPU windows of a 641x479 frame equal the GPU frame bit for bit, and the GPU frame matches the
// CPU frame within the node's parity tolerance.
TEST_F(ColorEqualizerGpuRegionTest, RegionIndependentGpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    RegionIndependenceOptions options;
    options.width = 641;
    options.height = 479;
    options.gpu = runtime.get();
    options.gpuTolerance = gpuTolerance();
    for (int output : { (int)ColorEqualizerParams::eOutputResult, (int)ColorEqualizerParams::eOutputSaturationThreshold }) {
        SCOPED_TRACE("output " + std::to_string(output));
        expectRegionIndependent([this, output](const NodePtr& source) {
            return makeEqualizer(source, output);
        },
                                options);
    }
}
#endif // NATRON_HAVE_GPU

} // namespace
