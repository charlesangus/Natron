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

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "Native/NativeGpuParity.h"
#include "NativeParity.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Transform/LensDistortion.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

// LensDistortion's lensdistortion.slang against its CPU render: every model, both directions, image
// and STMap output. The kernel maps each pixel through lensmodels.slang in float pairs, which agree
// with the CPU's doubles to about 2^-40 (LensModelsGpu_Test), so the coordinates round to the same
// floats and the frames are held to the resampling tolerance.

namespace {

const double kTime = 1.;

const char* const kFilters[] = { "impulse", "box", "bilinear", "cubic", "keys", "simon", "rifman", "mitchell", "parzen", "notch" };

struct KnobSetting {
    const char* name;
    std::vector<double> values;
};

// A model and the knobs that give it on the 64x48 parity source.
struct ModelSetup {
    const char* model;
    std::vector<KnobSetting> knobs;
};

// The camera of LensModels3DE_Test.
std::vector<KnobSetting>
camera3DE(double focalLength)
{
    return {
        { kLensDistortionParam3DEFocalLength, { focalLength } },
        { kLensDistortionParam3DEFocusDistance, { 100. } },
        { kLensDistortionParam3DEFilmbackWidth, { 3.6 } },
        { kLensDistortionParam3DEFilmbackHeight, { 2.025 } },
        { kLensDistortionParam3DELensCenterOffsetX, { 0.01 } },
        { kLensDistortionParam3DELensCenterOffsetY, { -0.02 } },
    };
}

ModelSetup
with3DECamera(const char* model,
              double focalLength,
              std::vector<KnobSetting> knobs)
{
    ModelSetup setup = { model, camera3DE(focalLength) };

    setup.knobs.insert(setup.knobs.end(), knobs.begin(), knobs.end());

    return setup;
}

// The coefficients of LensModels_Test and LensModels3DE_Test, every one of each model away from
// its default.
std::vector<ModelSetup>
modelSetups()
{
    const std::vector<KnobSetting> anamorphicDegrees24 = {
        { kLensDistortionParam3DECx02, { 0.04 } },
        { kLensDistortionParam3DECy02, { 0.03 } },
        { kLensDistortionParam3DECx22, { 0.01 } },
        { kLensDistortionParam3DECy22, { -0.005 } },
        { kLensDistortionParam3DECx04, { 0.01 } },
        { kLensDistortionParam3DECy04, { 0.008 } },
        { kLensDistortionParam3DECx24, { 0.002 } },
        { kLensDistortionParam3DECy24, { -0.001 } },
        { kLensDistortionParam3DECx44, { 0.001 } },
        { kLensDistortionParam3DECy44, { 0.0005 } },
    };
    std::vector<KnobSetting> anamorphic6 = anamorphicDegrees24;
    anamorphic6.insert(anamorphic6.end(), {
                                              { kLensDistortionParam3DECx06, { 0.002 } },
                                              { kLensDistortionParam3DECy06, { 0.001 } },
                                              { kLensDistortionParam3DECx26, { 0.0005 } },
                                              { kLensDistortionParam3DECy26, { -0.0003 } },
                                              { kLensDistortionParam3DECx46, { 0.0002 } },
                                              { kLensDistortionParam3DECy46, { 0.0001 } },
                                              { kLensDistortionParam3DECx66, { 0.0001 } },
                                              { kLensDistortionParam3DECy66, { -0.0001 } },
                                          });
    std::vector<KnobSetting> anamorphic4 = anamorphicDegrees24;
    anamorphic4.insert(anamorphic4.end(), {
                                              { kLensDistortionParam3DELensRotation, { 10. } },
                                              { kLensDistortionParam3DESqueezeX, { 1.05 } },
                                              { kLensDistortionParam3DESqueezeY, { 0.98 } },
                                          });

    return {
        { kLensDistortionModelNuke, {
                                        { kLensDistortionParamK1, { 0.05 } },
                                        { kLensDistortionParamK2, { 0.01 } },
                                        { kLensDistortionParamCenter, { 0.02, -0.01 } },
                                        { kLensDistortionParamSqueeze, { 1.2 } },
                                        { kLensDistortionParamAsymmetric, { 0.01, 0.005 } },
                                    } },
        { kLensDistortionModelPFBarrel, {
                                            { kLensDistortionParamPFC3, { 0.05 } },
                                            { kLensDistortionParamPFC5, { 0.01 } },
                                            { kLensDistortionParamPFP, { 0.52, 0.47 } },
                                            { kLensDistortionParamPFSqueeze, { 1.1 } },
                                        } },
        with3DECamera(kLensDistortionModel3DEClassic, 3.5, {
                                                               { kLensDistortionParam3DEDistortion, { 0.05 } },
                                                               { kLensDistortionParam3DEAnamorphicSqueeze, { 1.1 } },
                                                               { kLensDistortionParam3DECurvatureX, { 0.01 } },
                                                               { kLensDistortionParam3DECurvatureY, { -0.02 } },
                                                               { kLensDistortionParam3DEQuarticDistortion, { 0.01 } },
                                                           }),
        with3DECamera(kLensDistortionModel3DEAnamorphic6, 3.5, anamorphic6),
        with3DECamera(kLensDistortionModel3DEFisheye8, 2.5, {
                                                                { kLensDistortionParam3DEDistortionDegree2, { -0.05 } },
                                                                { kLensDistortionParam3DEQuarticDistortionDegree4, { 0.01 } },
                                                                { kLensDistortionParam3DEDegree8, { 0.001 } },
                                                            }),
        with3DECamera(kLensDistortionModel3DEStandard, 3.5, {
                                                                { kLensDistortionParam3DEDistortionDegree2, { 0.05 } },
                                                                { kLensDistortionParam3DEUDegree2, { 0.01 } },
                                                                { kLensDistortionParam3DEVDegree2, { -0.01 } },
                                                                { kLensDistortionParam3DEQuarticDistortionDegree4, { 0.01 } },
                                                                { kLensDistortionParam3DEUDegree4, { 0.002 } },
                                                                { kLensDistortionParam3DEVDegree4, { -0.003 } },
                                                                { kLensDistortionParam3DEPhiCylindricDirection, { 30. } },
                                                                { kLensDistortionParam3DEBCylindricBending, { 0.05 } },
                                                            }),
        with3DECamera(kLensDistortionModel3DEAnamorphic4, 5., anamorphic4),
        { kLensDistortionModelPanoTools, {
                                             { kLensDistortionParamPanoToolsA, { 0.01 } },
                                             { kLensDistortionParamPanoToolsB, { -0.02 } },
                                             { kLensDistortionParamPanoToolsC, { 0.03 } },
                                             { kLensDistortionParamPanoToolsD, { 5. } },
                                             { kLensDistortionParamPanoToolsE, { -3. } },
                                             { kLensDistortionParamPanoToolsG, { 2. } },
                                             { kLensDistortionParamPanoToolsT, { 1. } },
                                         } },
    };
} // modelSetups

// A fisheye wide enough that the corners of the 64x48 frame have no distort-direction solution.
ModelSetup
wideFisheyeSetup()
{
    return with3DECamera(kLensDistortionModel3DEFisheye8, 1., {
                                                                  { kLensDistortionParam3DEDistortionDegree2, { -0.05 } },
                                                                  { kLensDistortionParam3DEQuarticDistortionDegree4, { 0.01 } },
                                                                  { kLensDistortionParam3DEDegree8, { 0.001 } },
                                                              });
}

StagedImageEffect*
effectOf(const NodePtr& node)
{
    return node ? dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get()) : NULL;
}

GpuQuery
eligibleQuery()
{
    GpuQuery q;
    q.time = kTime;
    q.nComps = 4;
    q.bitDepth = eImageBitDepthFloat;

    return q;
}

std::string
outputMode(bool stmap)
{
    return stmap ? kLensDistortionOutputSTMap : kLensDistortionOutputImage;
}

} // namespace

class NativeLensDistortionGpuTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeLensDistortionGpu64x48", 1.));
    }

    NodePtr makeSource()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        EXPECT_TRUE(bool(source));

        return source;
    }

    // A LensDistortion on source with setup's model.
    NodePtr makeLensDistortion(const NodePtr& source,
                               const ModelSetup& setup)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_LENSDISTORTION), PLUGIN_MAJOR_NATRON_LENSDISTORTION);
        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        if (source) {
            connectNodes(source, node, kLensDistortionInputSource, true);
        }
        EXPECT_TRUE(setKnobValue(node, kLensDistortionParamModel, std::string(setup.model)));
        for (const KnobSetting& knob : setup.knobs) {
            EXPECT_TRUE(setKnobValues(node, knob.name, knob.values)) << setup.model << " " << knob.name;
        }

        return node;
    }

    // The frame of node over the source's window matches on the CPU and on the GPU, when there is
    // one, and node ran in a GPU region.
    void expectParity(const NodePtr& source,
                      const NodePtr& node,
                      const std::string& what)
    {
        const RectI window = paritySourceWindow(source, kTime, 0);
        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ node }), node, window, kTime);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << what << ": " << result.error;
        EXPECT_TRUE(result.ranInRegion(node)) << what;
        ASSERT_EQ(result.cpu.size(), result.gpu.size()) << what;
        const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::resampling());
        EXPECT_EQ(0u, diff.mismatches) << what << ": worst " << diff.worstAbsolute << ", first at float " << diff.firstMismatchIndex;
    }
};

TEST_F(NativeLensDistortionGpuTest, EligibilityFollowsTheSupportedSettings)
{
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr node = makeLensDistortion(source, modelSetups().front());
    ASSERT_TRUE(bool(node));
    StagedImageEffect* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    for (bool stmap : { false, true }) {
        ASSERT_TRUE(setKnobValue(node, kLensDistortionParamOutputMode, outputMode(stmap)));
        const std::string what = outputMode(stmap);
        for (int nComps : { 1, 2, 3, 4 }) {
            GpuQuery q = eligibleQuery();
            q.nComps = nComps;
            EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << what << " " << nComps << " components";
        }
        for (ImageBitDepthEnum depth : { eImageBitDepthByte, eImageBitDepthShort, eImageBitDepthHalf }) {
            GpuQuery q = eligibleQuery();
            q.bitDepth = depth;
            EXPECT_FALSE(effect->canRenderOnGpu(q)) << what << " bit depth " << (int)depth;
        }
        GpuQuery q = eligibleQuery();
        q.draftMode = true;
        EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << what << " draft";
        q.draftMode = false;
        q.hasMask = true;
        q.mix = 0.5;
        EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << what << " mask and mix";
    }
}

TEST_F(NativeLensDistortionGpuTest, EveryModelMatchesTheCpu)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    for (const ModelSetup& setup : modelSetups()) {
        NodePtr node = makeLensDistortion(source, setup);
        ASSERT_TRUE(bool(node)) << setup.model;
        for (const char* direction : { kLensDistortionDirectionDistort, kLensDistortionDirectionUndistort }) {
            ASSERT_TRUE(setKnobValue(node, kLensDistortionParamDirection, std::string(direction)));
            for (bool stmap : { false, true }) {
                ASSERT_TRUE(setKnobValue(node, kLensDistortionParamOutputMode, outputMode(stmap)));
                expectParity(source, node, std::string(setup.model) + " " + direction + " " + outputMode(stmap));
            }
        }
    }
}

TEST_F(NativeLensDistortionGpuTest, EveryFilterMatchesTheCpu)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr node = makeLensDistortion(source, modelSetups().front());
    ASSERT_TRUE(bool(node));
    for (const char* direction : { kLensDistortionDirectionDistort, kLensDistortionDirectionUndistort }) {
        ASSERT_TRUE(setKnobValue(node, kLensDistortionParamDirection, std::string(direction)));
        for (const char* filter : kFilters) {
            ASSERT_TRUE(setKnobValue(node, kResamplerParamFilterType, std::string(filter)));
            expectParity(source, node, std::string(direction) + " " + filter);
        }
    }
}

// Clamp, Black Outside and Mix reach the kernel.
TEST_F(NativeLensDistortionGpuTest, SettingsMatchTheCpu)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr node = makeLensDistortion(source, modelSetups().front());
    ASSERT_TRUE(bool(node));
    // Undistort reads outside the Source at the corners, where Black Outside shows.
    ASSERT_TRUE(setKnobValue(node, kLensDistortionParamDirection, std::string(kLensDistortionDirectionUndistort)));
    ASSERT_TRUE(setKnobValue(node, kResamplerParamFilterType, std::string("notch")));
    ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterClamp, { 1. }));
    expectParity(source, node, "clamp");
    ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterClamp, { 0. }));
    ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterBlackOutside, { 0. }));
    expectParity(source, node, "no black outside");
    ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterBlackOutside, { 1. }));
    ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { 0.35 }));
    expectParity(source, node, "mix");
    ASSERT_TRUE(setKnobValue(node, kLensDistortionParamOutputMode, outputMode(true)));
    expectParity(source, node, "STMap mix");
}

// Where the model has no solution both sides give black, in the image and in the STMap.
TEST_F(NativeLensDistortionGpuTest, NonConvergentFisheyeEdgeMatchesTheCpu)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr node = makeLensDistortion(source, wideFisheyeSetup());
    ASSERT_TRUE(bool(node));
    for (const char* direction : { kLensDistortionDirectionDistort, kLensDistortionDirectionUndistort }) {
        ASSERT_TRUE(setKnobValue(node, kLensDistortionParamDirection, std::string(direction)));
        for (bool stmap : { false, true }) {
            ASSERT_TRUE(setKnobValue(node, kLensDistortionParamOutputMode, outputMode(stmap)));
            expectParity(source, node, std::string(direction) + " " + outputMode(stmap));
        }
    }
}
