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
#include <fstream>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "RawReferenceSupport.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/ContrastTexture.h"
#include "Engine/Nodes/Filter/ContrastTextureKernels.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace CT = ContrastTextureKernels;

namespace {

const double kTime = 1.;

// Against darktable's export, relative to the reference's mean level, as RawReference compares:
// the mean error and the largest one. Engine/Nodes/Filter/docs/ContrastTexture.md quotes them.
const RawReference::Tolerance kDarktableClass = { 1e-3, 1e-2, 0 };

// The node against ContrastTextureKernels run directly on its source: the same float code, on
// threads that only split rows.
const double kKernelTolerance = 1e-5;

const char* const kReferenceDir = NATRON_TESTS_FIXTURES_DIR "/raw/reference/";

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

RectI
pixelRoD(const NodePtr& node)
{
    EffectInstancePtr effect = node->getEffectInstance();
    RectD rod;
    const StatusEnum stat = effect->getRegionOfDefinition(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod);
    EXPECT_NE(eStatusFailed, stat);

    return rod.toPixelEnclosing(0, 1.);
}

bool
renderRGBA(const NodePtr& node,
           const RectI& window,
           RenderedPlane* plane)
{
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error);
    EXPECT_TRUE(ok) << error;
    if (!ok || (planes.size() != 1) || (planes[0].channels.size() != 4)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

// The RGBA plane's pixels top-down, as the kernels take them.
std::vector<float>
topDown(const RenderedPlane& plane)
{
    const int width = plane.window.width();
    const int height = plane.window.height();
    std::vector<float> out(plane.pixels.size());

    for (int row = 0; row < height; ++row) {
        std::copy(plane.pixels.begin() + (std::size_t)(height - 1 - row) * width * 4,
                  plane.pixels.begin() + (std::size_t)(height - row) * width * 4,
                  out.begin() + (std::size_t)row * width * 4);
    }

    return out;
}

// image filtered by the kernels with params, image being width x height pixels of nComps floats,
// top-down, and the whole image.
std::vector<float>
applyKernels(const std::vector<float>& image,
             int width,
             int height,
             int nComps,
             const CT::Params& params)
{
    const CT::Plan plan = CT::makePlan(params, (float)std::max(width, height), 1.f);
    std::vector<float> map((std::size_t)width * height);
    CT::computeMap(image.data(), width, height, nComps, plan, map.data(), EdgeAware::serialFor);
    std::vector<float> out(image.size());
    for (std::size_t k = 0; k < map.size(); ++k) {
        CT::writePixel(&image[k * nComps], map[k], plan, nComps, &out[k * nComps]);
    }

    return out;
}

// Whether path holds a reference rather than nothing or a Git LFS pointer.
bool
referenceAvailable(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    char head[24] = { 0 };
    in.read(head, sizeof(head) - 1);

    return std::string(head).compare(0, 23, "version https://git-lfs") != 0;
}

struct ReferenceJob {
    const char* suffix;
    CT::Params params;
};

CT::Params
params(float coarse,
       float medium,
       float fine,
       float detailLevel,
       float halo,
       float noiseBias,
       float shadows,
       float highlights)
{
    CT::Params p;

    p.gainCoarse = coarse;
    p.gainMedium = medium;
    p.gainFine = fine;
    p.detailLevel = detailLevel;
    p.haloControl = halo;
    p.noiseBias = noiseBias;
    p.gainShadows = shadows;
    p.gainHighlights = highlights;

    return p;
}

// make-reference.py's CONTRASTNTEXTURE_VARIANTS.
std::vector<ReferenceJob>
variants()
{
    return {
        { "default", params(0.f, 0.f, 0.f, 5.f, 0.f, 0.001f, 0.f, 0.f) },
        { "coarse-1", params(1.f, 0.f, 0.f, 5.f, 0.f, 0.001f, 0.f, 0.f) },
        { "coarse-minus-0.5", params(-0.5f, 0.f, 0.f, 5.f, 0.f, 0.001f, 0.f, 0.f) },
        { "medium-1", params(0.f, 1.f, 0.f, 5.f, 0.f, 0.001f, 0.f, 0.f) },
        { "medium-minus-0.5", params(0.f, -0.5f, 0.f, 5.f, 0.f, 0.001f, 0.f, 0.f) },
        { "fine-1", params(0.f, 0.f, 1.f, 5.f, 0.f, 0.001f, 0.f, 0.f) },
        { "fine-3", params(0.f, 0.f, 3.f, 5.f, 0.f, 0.001f, 0.f, 0.f) },
        { "detail-level-2", params(0.f, 1.f, 0.f, 2.f, 0.f, 0.001f, 0.f, 0.f) },
        { "detail-level-9", params(0.f, 1.f, 0.f, 9.f, 0.f, 0.001f, 0.f, 0.f) },
        { "halo-minus-3", params(0.f, 0.f, 1.f, 5.f, -3.f, 0.001f, 0.f, 0.f) },
        { "halo-3", params(0.f, 0.f, 1.f, 5.f, 3.f, 0.001f, 0.f, 0.f) },
        { "noise-bias-0.05", params(0.f, 0.f, 1.f, 5.f, 0.f, 0.05f, 0.f, 0.f) },
        { "noise-bias-0", params(0.f, 0.f, 1.f, 5.f, 0.f, 0.f, 0.f, 0.f) },
        { "shadows-1", params(0.f, 0.f, 0.f, 5.f, 0.f, 0.001f, 1.f, 0.f) },
        { "shadows-minus-1", params(0.f, 0.f, 0.f, 5.f, 0.f, 0.001f, -1.f, 0.f) },
        { "highlights-1", params(0.f, 0.f, 0.f, 5.f, 0.f, 0.001f, 0.f, 1.f) },
        { "highlights-minus-1", params(0.f, 0.f, 0.f, 5.f, 0.f, 0.001f, 0.f, -1.f) },
        { "combined", params(0.4f, 0.6f, 0.8f, 4.f, 1.f, 0.01f, 0.5f, -0.5f) },
    };
}

} // namespace

class NativeContrastTextureTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeContrastTexture64x48", 1.));
    }

    NodePtr makeParitySource()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);

        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceOrigin(source, 0, 0);
        }

        return source;
    }

    NodePtr makeContrastTexture(const NodePtr& source)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_CONTRASTTEXTURE), PLUGIN_MAJOR_NATRON_CONTRASTTEXTURE);

        EXPECT_TRUE(bool(node));
        if (node && source) {
            connectNodes(source, node, 0, true);
        }

        return node;
    }

    // Expects the double knob name with darktable's default, range and soft range.
    void expectDouble(const NodePtr& node,
                      const char* name,
                      const char* label,
                      double def,
                      double min,
                      double max,
                      double displayMin,
                      double displayMax)
    {
        KnobDoublePtr knob = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(name));
        ASSERT_TRUE(bool(knob)) << name;
        EXPECT_EQ(std::string(label), knob->getLabel()) << name;
        EXPECT_EQ(1, knob->getDimension()) << name;
        EXPECT_DOUBLE_EQ(def, knob->getDefaultValue(0)) << name;
        EXPECT_DOUBLE_EQ(def, knob->getValue()) << name;
        EXPECT_DOUBLE_EQ(min, knob->getMinimum(0)) << name;
        EXPECT_DOUBLE_EQ(max, knob->getMaximum(0)) << name;
        EXPECT_DOUBLE_EQ(displayMin, knob->getDisplayMinimum(0)) << name;
        EXPECT_DOUBLE_EQ(displayMax, knob->getDisplayMaximum(0)) << name;
    }
};

TEST_F(NativeContrastTextureTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_CONTRASTTEXTURE, PLUGIN_MAJOR_NATRON_CONTRASTTEXTURE, "ContrastTexture");
}

// Every field of dt_iop_contrastntexture_params_t (version 2) is a knob with darktable's
// default, range and soft range, and every mask toggle of its GUI an option of Mask Display.
TEST_F(NativeContrastTextureTest, KnobsMatchDarktableParams)
{
    resetProject();
    NodePtr node = makeContrastTexture(NodePtr());
    ASSERT_TRUE(isNative(node));
    EXPECT_EQ(std::string("Source"), node->getInputLabel(0));
    EXPECT_EQ(std::string("Mask"), node->getInputLabel(1));
    EXPECT_TRUE(node->getEffectInstance()->isInputMask(1));
    EXPECT_TRUE(bool(node->getKnobByName("enableMask_Mask")));
    EXPECT_TRUE(bool(node->getKnobByName(kOfxMixParamName)));

    // name, label, default, min, max, soft min, soft max
    expectDouble(node, kContrastTextureParamCoarse, "Coarse Details", 0., -1., 5., -1., 1.5);
    expectDouble(node, kContrastTextureParamMedium, "Medium Details", 0., -1., 5., -1., 1.5);
    expectDouble(node, kContrastTextureParamFine, "Fine Details", 0., -1., 5., -1., 1.5);
    expectDouble(node, kContrastTextureParamDetailLevel, "Base Detail Level", 5., 1., 15., 1., 10.);
    expectDouble(node, kContrastTextureParamHaloControl, "Halo Control", 0., -10., 10., -5., 5.);
    expectDouble(node, kContrastTextureParamNoiseBias, "Noise Bias", 0.001, 0., 1., 0., 0.2);
    expectDouble(node, kContrastTextureParamShadows, "Shadows", 0., -5., 5., -2., 2.);
    expectDouble(node, kContrastTextureParamHighlights, "Highlights", 0., -5., 5., -2., 2.);

    for (const char* group : { kContrastTextureParamLocalContrast, kContrastTextureParamFilterSettings }) {
        EXPECT_TRUE(bool(std::dynamic_pointer_cast<KnobGroup>(node->getKnobByName(group)))) << group;
    }

    KnobChoicePtr display = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kContrastTextureParamMaskDisplay));
    ASSERT_TRUE(bool(display));
    EXPECT_EQ(std::string("Mask Display"), display->getLabel());
    EXPECT_EQ((int)CT::eDisplayOff, display->getValue());
    const std::vector<ChoiceOption> options = display->getEntries_mt_safe();
    const char* const ids[] = { kContrastTextureMaskDisplayOff, kContrastTextureMaskDisplayCoarse, kContrastTextureMaskDisplayMedium,
                                kContrastTextureMaskDisplayFine, kContrastTextureMaskDisplayLowpass };
    const int displays[] = { CT::eDisplayOff, CT::eDisplayCoarse, CT::eDisplayMedium, CT::eDisplayFine, CT::eDisplayLowpass };
    ASSERT_EQ(5u, options.size());
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(std::string(ids[i]), options[displays[i]].id);
    }

    // The defaults of the kernels' params are darktable's too.
    const CT::Params p;
    EXPECT_EQ(0.f, p.gainCoarse);
    EXPECT_EQ(0.f, p.gainMedium);
    EXPECT_EQ(0.f, p.gainFine);
    EXPECT_EQ(5.f, p.detailLevel);
    EXPECT_EQ(0.f, p.haloControl);
    EXPECT_EQ(0.001f, p.noiseBias);
    EXPECT_EQ(0.f, p.gainShadows);
    EXPECT_EQ(0.f, p.gainHighlights);
    EXPECT_EQ(CT::eDisplayOff, p.display);
}

// darktable's band crossfade: the three weights sum to 1 at every level.
TEST_F(NativeContrastTextureTest, BandWeightsArePartitionOfUnity)
{
    for (float maxLevel : { 1.f, 3.f, 7.5f }) {
        for (int level = 0; level < CT::kMaxLevels; ++level) {
            const float sum = CT::bandWeight(CT::eBandCoarse, level, maxLevel) + CT::bandWeight(CT::eBandMedium, level, maxLevel) + CT::bandWeight(CT::eBandFine, level, maxLevel);
            EXPECT_NEAR(1.f, sum, 1e-6f) << "level " << level << " of " << maxLevel;
        }
    }
    // The coarsest level is all coarse, the middle all medium, the finest all fine.
    EXPECT_NEAR(1.f, CT::bandWeight(CT::eBandCoarse, 0, 4.f), 1e-6f);
    EXPECT_NEAR(1.f, CT::bandWeight(CT::eBandMedium, 2, 4.f), 1e-6f);
    EXPECT_NEAR(1.f, CT::bandWeight(CT::eBandFine, 4, 4.f), 1e-6f);
}

// At its defaults, and with only the filter settings changed, the node is an identity and passes
// the source through exactly.
TEST_F(NativeContrastTextureTest, DefaultsAreIdentity)
{
    resetProject();
    NodePtr source = makeParitySource();
    NodePtr node = makeContrastTexture(source);
    ASSERT_TRUE(bool(source) && bool(node));
    const RectI rod = pixelRoD(node);
    ASSERT_EQ(pixelRoD(source), rod);
    RenderedPlane in;
    ASSERT_TRUE(renderRGBA(source, rod, &in));

    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            ASSERT_TRUE(setKnobValues(node, kContrastTextureParamDetailLevel, { 3. }));
            ASSERT_TRUE(setKnobValues(node, kContrastTextureParamHaloControl, { 2. }));
            ASSERT_TRUE(setKnobValues(node, kContrastTextureParamNoiseBias, { 0.1 }));
        }
        RenderedPlane out;
        ASSERT_TRUE(renderRGBA(node, rod, &out));
        ASSERT_EQ(in.pixels.size(), out.pixels.size());
        for (std::size_t i = 0; i < in.pixels.size(); ++i) {
            ASSERT_EQ(in.pixels[i], out.pixels[i]) << "pass " << pass << ", float " << i;
        }
    }
    EXPECT_TRUE(CT::isIdentity(CT::Params()));
}

// The node renders what the kernels compute on its source, with the source's alpha kept, for the
// image and the previews.
TEST_F(NativeContrastTextureTest, NodeMatchesKernels)
{
    resetProject();
    NodePtr source = makeParitySource();
    NodePtr node = makeContrastTexture(source);
    ASSERT_TRUE(bool(source) && bool(node));
    const RectI rod = pixelRoD(node);
    RenderedPlane in;
    ASSERT_TRUE(renderRGBA(source, rod, &in));
    const std::vector<float> image = topDown(in);

    CT::Params p = params(0.4f, 0.6f, 0.8f, 2.f, 1.f, 0.01f, 0.5f, -0.5f);
    ASSERT_TRUE(setKnobValues(node, kContrastTextureParamCoarse, { 0.4 }));
    ASSERT_TRUE(setKnobValues(node, kContrastTextureParamMedium, { 0.6 }));
    ASSERT_TRUE(setKnobValues(node, kContrastTextureParamFine, { 0.8 }));
    ASSERT_TRUE(setKnobValues(node, kContrastTextureParamDetailLevel, { 2. }));
    ASSERT_TRUE(setKnobValues(node, kContrastTextureParamHaloControl, { 1. }));
    ASSERT_TRUE(setKnobValues(node, kContrastTextureParamNoiseBias, { 0.01 }));
    ASSERT_TRUE(setKnobValues(node, kContrastTextureParamShadows, { 0.5 }));
    ASSERT_TRUE(setKnobValues(node, kContrastTextureParamHighlights, { -0.5 }));

    const struct {
        const char* id;
        CT::DisplayEnum display;
    } displays[] = {
        { kContrastTextureMaskDisplayOff, CT::eDisplayOff },
        { kContrastTextureMaskDisplayCoarse, CT::eDisplayCoarse },
        { kContrastTextureMaskDisplayFine, CT::eDisplayFine },
        { kContrastTextureMaskDisplayLowpass, CT::eDisplayLowpass },
    };
    for (const auto& d : displays) {
        ASSERT_TRUE(setKnobValue(node, kContrastTextureParamMaskDisplay, std::string(d.id)));
        p.display = d.display;
        const std::vector<float> want = applyKernels(image, rod.width(), rod.height(), 4, p);
        RenderedPlane out;
        ASSERT_TRUE(renderRGBA(node, rod, &out));
        const std::vector<float> got = topDown(out);
        ASSERT_EQ(want.size(), got.size());
        std::size_t mismatches = 0;
        for (std::size_t i = 0; i < want.size(); ++i) {
            if (std::fabs(want[i] - got[i]) > kKernelTolerance * std::max(1.f, std::fabs(want[i]))) {
                if (mismatches++ == 0) {
                    ADD_FAILURE() << d.id << ": first mismatch at float " << i << ", " << got[i] << " for " << want[i];
                }
            }
        }
        EXPECT_EQ(0u, mismatches) << d.id;
        if (d.display == CT::eDisplayOff) {
            for (std::size_t k = 0; k < want.size() / 4; ++k) {
                ASSERT_EQ(image[k * 4 + 3], got[k * 4 + 3]) << "alpha of pixel " << k;
            }
        }
    }
}

// The kernels against darktable's contrast & texture on make-reference.py's plate, for every job
// of contrastntexture_jobs(). Skips when the references are not checked out.
TEST_F(NativeContrastTextureTest, MatchesDarktableReferences)
{
    const std::string platePath = std::string(kReferenceDir) + "contrastntexture-plate.tif";
    if (!referenceAvailable(platePath)) {
        std::printf("NativeContrastTexture: %s is missing or a Git LFS pointer; run tools/raw/make-reference.sh --only contrastntexture "
                    "and git lfs pull. Skipped.\n",
                    platePath.c_str());

        return;
    }
    std::vector<float> plate;
    int width = 0;
    int height = 0;
    std::string error;
    ASSERT_TRUE(RawReference::readReference(platePath, &plate, &width, &height, &error)) << error;
    ASSERT_GT(width, 0);
    ASSERT_GT(height, 0);

    for (const ReferenceJob& v : variants()) {
        const std::string name = std::string("contrastntexture-") + v.suffix;
        const std::string path = std::string(kReferenceDir) + name + ".tif";
        ASSERT_TRUE(referenceAvailable(path)) << path << " is missing while the plate is not";
        std::vector<float> expected;
        int refWidth = 0;
        int refHeight = 0;
        ASSERT_TRUE(RawReference::readReference(path, &expected, &refWidth, &refHeight, &error)) << error;
        ASSERT_EQ(width, refWidth) << name;
        ASSERT_EQ(height, refHeight) << name;

        const std::vector<float> got = applyKernels(plate, width, height, 3, v.params);
        double sum = 0.;
        double level = 0.;
        double worst = 0.;
        int worstX = 0;
        int worstY = 0;
        for (std::size_t i = 0; i < got.size(); ++i) {
            const double diff = std::fabs((double)got[i] - expected[i]);
            sum += diff;
            level += std::fabs(expected[i]);
            if (diff > worst) {
                worst = diff;
                worstX = (int)((i / 3) % width);
                worstY = (int)((i / 3) / width);
            }
        }
        ASSERT_GT(level, 0.) << name;
        const double relMean = sum / level;
        const double relMax = worst * got.size() / level;
        std::printf("%s against darktable: mean error %.4f%% of the mean level, max %.3f%% at %d,%d from the top-left\n",
                    name.c_str(), 100. * relMean, 100. * relMax, worstX, worstY);
        EXPECT_LE(relMean, kDarktableClass.mean) << name;
        EXPECT_LE(relMax, kDarktableClass.max) << name;
    }
}
