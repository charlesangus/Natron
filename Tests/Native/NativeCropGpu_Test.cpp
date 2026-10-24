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
#include <cstring>
#include <limits>
#include <list>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "Native/NativeGpuParity.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Transform/Crop.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#include <ofxNatron.h>

NATRON_NAMESPACE_USING

static_assert(GpuOffsetResampleOp<CropOp>);
static_assert(!GpuMatrixResampleOp<CropOp>);

namespace {

const double kTime = 1.;

GpuQuery
eligibleQuery()
{
    GpuQuery q;
    q.time = kTime;
    q.nComps = 4;
    q.bitDepth = eImageBitDepthFloat;

    return q;
}

struct CropCase {
    const char* name;
    const char* extent;
    double x;
    double y;
    double w;
    double h;
    int formatW;
    int formatH;
    double formatPar;
    bool reformat;
    bool intersect;
    bool blackOutside;
    double softness;
};

// The parity source covers (3,5)-(67,53). Rectangles run inside it, across its edge, around it,
// and sit on half pixels, where a pixel centre lies exactly on the rectangle's edge.
const CropCase kCases[] = {
    { "size", kNativeGeneratorExtentSize, 10., 8., 40., 30., 0, 0, 1., false, false, false, 0. },
    { "size reformat", kNativeGeneratorExtentSize, 10., 8., 40., 30., 0, 0, 1., true, false, false, 0. },
    { "size black outside", kNativeGeneratorExtentSize, 10., 8., 40., 30., 0, 0, 1., false, false, true, 0. },
    { "size black outside reformat", kNativeGeneratorExtentSize, 11., 9., 37., 25., 0, 0, 1., true, false, true, 0. },
    { "size soft", kNativeGeneratorExtentSize, 10., 8., 40., 30., 0, 0, 1., false, false, false, 6. },
    { "size soft reformat", kNativeGeneratorExtentSize, 10.3, 8.6, 40.4, 30.2, 0, 0, 1., true, false, false, 7.5 },
    { "size soft black outside reformat", kNativeGeneratorExtentSize, 12., 7., 35., 28., 0, 0, 1., true, false, true, 5. },
    { "size softer than half", kNativeGeneratorExtentSize, 14., 10., 30., 20., 0, 0, 1., false, false, false, 40. },
    { "size half pixel edges", kNativeGeneratorExtentSize, 10.5, 8.5, 40., 30., 0, 0, 1., false, false, false, 6. },
    { "size larger than source", kNativeGeneratorExtentSize, -10., -8., 90., 70., 0, 0, 1., false, false, true, 0. },
    { "size larger than source soft", kNativeGeneratorExtentSize, -10., -8., 90., 70., 0, 0, 1., true, false, false, 9. },
    { "size larger than source intersect", kNativeGeneratorExtentSize, -10., -8., 90., 70., 0, 0, 1., false, true, true, 0. },
    { "size across the source edge intersect soft", kNativeGeneratorExtentSize, 40., 30., 50., 40., 0, 0, 1., true, true, false, 8. },
    { "format", kNativeGeneratorExtentFormat, 0., 0., 0., 0., 50, 40, 1., false, false, false, 0. },
    { "format reformat soft anamorphic", kNativeGeneratorExtentFormat, 0., 0., 0., 0., 50, 40, 2., true, false, false, 6. },
    { "project reformat soft", kNativeGeneratorExtentProject, 0., 0., 0., 0., 0, 0, 1., true, false, false, 5. },
    { "project black outside", kNativeGeneratorExtentProject, 0., 0., 0., 0., 0, 0, 1., false, false, true, 0. },
    { "default reformat", kNativeGeneratorExtentDefault, 0., 0., 0., 0., 0, 0, 1., true, true, false, 0. },
    { "default soft black outside", kNativeGeneratorExtentDefault, 0., 0., 0., 0., 0, 0, 1., false, false, true, 10. },
};

} // namespace

class NativeCropGpuTest
    : public BaseTest {
protected:
    // A fresh project whose default format is 64x48 at the given pixel aspect ratio.
    void resetProject(double par = 1.)
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeCropGpuPar" + std::to_string(par), par));
    }

    static StagedImageEffect* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get()) : NULL;
    }

    bool applyCase(const NodePtr& node,
                   const CropCase& c)
    {
        if (!setKnobValue(node, kNativeGeneratorParamExtent, c.extent)) {
            return false;
        }
        const std::string extent = c.extent;
        if (extent == kNativeGeneratorExtentSize) {
            if (!setKnobValues(node, kNativeGeneratorParamBottomLeft, { c.x, c.y }) || !setKnobValues(node, kNativeGeneratorParamSize, { c.w, c.h })) {
                return false;
            }
        } else if (extent == kNativeGeneratorExtentFormat) {
            if (!setKnobValues(node, kNatronParamFormatSize, { (double)c.formatW, (double)c.formatH }) || !setKnobValues(node, kNatronParamFormatPar, { c.formatPar })) {
                return false;
            }
        }

        return setKnobValues(node, kCropParamReformat, { c.reformat ? 1. : 0. }) && setKnobValues(node, kCropParamIntersect, { c.intersect ? 1. : 0. }) && setKnobValues(node, kCropParamBlackOutside, { c.blackOutside ? 1. : 0. }) && setKnobValues(node, kCropParamSoftness, { c.softness });
    }

    // A source moved off the origin, or a constant of NaN, connected to a new Crop set up as the case says.
    bool createGraph(const CropCase& c,
                     bool nanSource,
                     NodePtr* source,
                     NodePtr* node)
    {
        if (nanSource) {
            *source = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
        } else {
            *source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        }
        *node = createNode(QString::fromUtf8(PLUGINID_NATRON_CROP), PLUGIN_MAJOR_NATRON_CROP);
        EXPECT_TRUE(bool(*source));
        EXPECT_TRUE(bool(*node));
        if (!*source || !*node) {
            return false;
        }
        if (nanSource) {
            const double nan = std::numeric_limits<double>::quiet_NaN();
            if (!setKnobValues(*source, kConstantParamColor, { nan, nan, nan, nan })) {
                return false;
            }
        } else {
            setParitySourceComponents(*source, "rgba");
            setParitySourceOrigin(*source, 3, 5);
        }
        connectNodes(*source, *node, 0, true);

        return applyCase(*node, c);
    }
};

TEST_F(NativeCropGpuTest, EligibilityFollowsTheRequest)
{
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    resetProject();
    NodePtr source;
    NodePtr node;
    ASSERT_TRUE(createGraph(kCases[6], false, &source, &node));
    StagedImageEffect* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    for (int nComps : { 1, 2, 3, 4 }) {
        GpuQuery q = eligibleQuery();
        q.nComps = nComps;
        EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << nComps << " components";
    }
    for (ImageBitDepthEnum depth : { eImageBitDepthByte, eImageBitDepthShort, eImageBitDepthHalf }) {
        GpuQuery q = eligibleQuery();
        q.bitDepth = depth;
        EXPECT_FALSE(effect->canRenderOnGpu(q)) << "bit depth " << (int)depth;
    }
}

// The source the GPU is given is the region Crop asks the host for on the CPU: the crop rectangle
// clipped to the source, moved back by the reformat offset.
TEST_F(NativeCropGpuTest, InputRegionIsTheNodesRegionOfInterest)
{
    for (double par : { 1., 2. }) {
        resetProject(par);
        for (const CropCase& c : kCases) {
            NodePtr source;
            NodePtr node;
            ASSERT_TRUE(createGraph(c, false, &source, &node));
            StagedImageEffect* effect = effectOf(node);
            ASSERT_TRUE(effect != NULL);
            const double sourcePar = source->getEffectInstance()->getAspectRatio(-1);

            const RectI rect(10, 20, 31, 47);
            for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
                const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
                const RectD window = rect.toCanonical_noClipping(mipmapLevel, effect->getAspectRatio(-1));
                RoIMap rois;
                effect->getRegionsOfInterest_public(kTime, scale, window, window, ViewIdx(0), &rois);
                RoIMap::const_iterator found = rois.find(source->getEffectInstance());
                ASSERT_TRUE(found != rois.end()) << c.name << " mipmap " << mipmapLevel;
                EXPECT_EQ(found->second.toPixelEnclosing(scale, sourcePar), effect->getGpuInputRoI(0, rect, scale)) << c.name << " par " << par << " mipmap " << mipmapLevel;
            }
        }
    }
}

#ifdef NATRON_HAVE_GPU

namespace {

ImageLayerDesc
colorLayer(const NodePtr& node)
{
    std::list<ImageLayerDesc> layers;

    node->getEffectInstance()->getPresentLayers(kTime, ViewIdx(0), -1, &layers);
    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        if (it->isColorLayer()) {
            return *it;
        }
    }

    return ImageLayerDesc();
}

// The render bypasses the caches, so the source it fetches covers exactly its region of interest,
// the region the GPU is handed, and no larger image an earlier render left behind.
bool
renderColor(const NodePtr& node,
            unsigned mipmapLevel,
            const RectI& window,
            RenderedPlane* plane)
{
    std::list<ImageLayerDesc> layers;
    layers.push_back(colorLayer(node));
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), mipmapLevel, window, layers, &planes, &error);
    EXPECT_TRUE(ok) << error;
    if (!ok || (planes.size() != 1)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

RectI
pixelRegionOfDefinition(const NodePtr& node,
                        unsigned mipmapLevel)
{
    EffectInstancePtr effect = node->getEffectInstance();
    RectD rod;
    bool isProjectFormat = false;
    const StatusEnum stat = effect->getRegionOfDefinition_public(effect->getRenderHash(), kTime, RenderScale::fromMipmapLevel(mipmapLevel), ViewIdx(0), &rod, &isProjectFormat);
    EXPECT_NE(eStatusFailed, stat);

    return rod.toPixelEnclosing(mipmapLevel, effect->getAspectRatio(-1));
}

GpuQuery
queryFor(const NodePtr& node,
         unsigned mipmapLevel)
{
    StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get());
    const ImageLayerDesc plane = colorLayer(node);
    GpuQuery q = eligibleQuery();

    q.mappedScale = RenderScale::fromMipmapLevel(mipmapLevel);
    q.nComps = plane.getNumComponents();
    const std::bitset<4> channels = effect->getProcessChannelsForPlane(effect->getRenderHash(), kTime, ViewIdx(0), plane);
    for (int i = 0; i < 4; ++i) {
        q.processChannels[i] = channels[i];
    }

    return q;
}

GpuViewLayout
outputLayout()
{
    GpuViewLayout layout;

    layout.padFloats = 4;
    layout.rowPadFloats = 4;
    layout.marginLeft = 2;
    layout.marginBottom = 1;
    layout.marginRight = 3;
    layout.marginTop = 2;

    return layout;
}

// The source's pixels over bounds, inside a larger buffer at an offset and with padded rows.
bool
sourceImage(const NodePtr& source,
            unsigned mipmapLevel,
            const RectI& bounds,
            GpuParityImage* image)
{
    RenderedPlane in;
    if (!renderColor(source, mipmapLevel, bounds, &in)) {
        return false;
    }
    EXPECT_EQ(bounds, in.window);
    image->bounds = in.window;
    image->nComps = (int)in.channels.size();
    image->pixels = in.pixels;
    image->layout.padFloats = 6;
    image->layout.rowPadFloats = 8;

    return true;
}

std::string
describeRoI(const RectI& roi)
{
    std::ostringstream s;
    s << "roi (" << roi.x1 << "," << roi.y1 << ")-(" << roi.x2 << "," << roi.y2 << ")";

    return s.str();
}

// The whole region of definition and an odd-sized window inside it, and at the full resolution
// four tiles that meet off-centre.
std::vector<RectI>
testWindows(const RectI& rod,
            bool withTiles)
{
    std::vector<RectI> rois;

    rois.push_back(rod);
    const int x2 = std::min(rod.x2 - 2, rod.x1 + 5 + 23);
    const int y2 = std::min(rod.y2 - 1, rod.y1 + 3 + 17);
    if ((x2 > rod.x1 + 5) && (y2 > rod.y1 + 3)) {
        rois.push_back(RectI(rod.x1 + 5, rod.y1 + 3, x2, y2));
    }
    if (withTiles) {
        const int mx = rod.x1 + rod.width() / 2 + 3;
        const int my = rod.y1 + rod.height() / 2 - 2;
        const int xs[3] = { rod.x1, mx, rod.x2 };
        const int ys[3] = { rod.y1, my, rod.y2 };
        for (int j = 0; j < 2; ++j) {
            for (int i = 0; i < 2; ++i) {
                const RectI tile(xs[i], ys[j], xs[i + 1], ys[j + 1]);
                if ((tile.width() > 0) && (tile.height() > 0)) {
                    rois.push_back(tile);
                }
            }
        }
    }

    return rois;
}

bool
sameFloat(float a,
          float b)
{
    return (std::isnan(a) && std::isnan(b)) || (std::memcmp(&a, &b, sizeof(float)) == 0);
}

// The translation the reformat applies at mipmapLevel, as the node's offset computes it.
void
reformatShift(const NodePtr& node,
              unsigned mipmapLevel,
              int* tx,
              int* ty)
{
    Crop* crop = dynamic_cast<Crop*>(node->getEffectInstance().get());
    const double scale = 1. / (double)(1u << mipmapLevel);
    RectD rect;
    double par = 1.;

    crop->getCropRectangle(kTime, ViewIdx(0), RenderScale::fromMipmapLevel(mipmapLevel), false, false, false, false, &rect, &par);
    *tx = -(int)std::floor(rect.x1 * scale / par + 0.5);
    *ty = -(int)std::floor(rect.y1 * scale + 0.5);
}

// Compares the render with the CPU's. Where the CPU wrote zero, or kept the source pixel, the
// fade weight was 0 or at least 1 and the device must agree to the bit; elsewhere it multiplied
// by a partial weight and the tolerance is resampling()'s. With softness 0 every pixel is exact.
void
expectCropEqual(const std::vector<float>& cpu,
                const GpuRunResult& run,
                const RectI& roi,
                const GpuParityImage& input,
                int tx,
                int ty,
                bool soft,
                const std::string& what)
{
    ASSERT_TRUE(run.ok) << what;
    EXPECT_EQ(0u, run.writtenOutside) << what << ": floats outside the window were written";
    ASSERT_EQ(cpu.size(), run.pixels.size()) << what;

    std::vector<float> exactWant;
    std::vector<float> exactGot;
    std::vector<float> softWant;
    std::vector<float> softGot;
    std::vector<int> exactAt;
    std::vector<int> softAt;
    const int width = roi.width();
    for (int y = roi.y1; y < roi.y2; ++y) {
        for (int x = roi.x1; x < roi.x2; ++x) {
            const std::size_t i = ((std::size_t)(y - roi.y1) * width + (x - roi.x1)) * 4;
            bool exact = !soft;
            if (soft) {
                const int sx = x - tx;
                const int sy = y - ty;
                bool zero = true;
                bool kept = true;
                for (int c = 0; c < 4; ++c) {
                    float src = 0.f;
                    if (input.bounds.contains(sx, sy) && !input.pixels.empty()) {
                        src = input.pixels[((std::size_t)(sy - input.bounds.y1) * input.bounds.width() + (sx - input.bounds.x1)) * input.nComps + c];
                    }
                    zero = zero && sameFloat(cpu[i + c], 0.f);
                    kept = kept && sameFloat(cpu[i + c], src);
                }
                exact = zero || kept;
            }
            std::vector<float>& want = exact ? exactWant : softWant;
            std::vector<float>& got = exact ? exactGot : softGot;
            std::vector<int>& at = exact ? exactAt : softAt;
            for (int c = 0; c < 4; ++c) {
                want.push_back(cpu[i + c]);
                got.push_back(run.pixels[i + c]);
            }
            at.push_back(x);
            at.push_back(y);
        }
    }
    const ImageDiff exactDiff = compareImages(exactWant, exactGot, ParityTolerance::exact());
    const std::size_t e = exactDiff.firstMismatchIndex / 4;
    EXPECT_EQ(0u, exactDiff.mismatches) << what << ": " << exactDiff.mismatches << " exact mismatches, first at (" << (exactAt.empty() ? 0 : exactAt[e * 2]) << ","
                                        << (exactAt.empty() ? 0 : exactAt[e * 2 + 1]) << ") channel " << (exactDiff.firstMismatchIndex % 4) << " gpu "
                                        << (exactGot.empty() ? 0.f : exactGot[exactDiff.firstMismatchIndex]) << " cpu "
                                        << (exactWant.empty() ? 0.f : exactWant[exactDiff.firstMismatchIndex]);
    const ImageDiff softDiff = compareImages(softWant, softGot, ParityTolerance::resampling());
    const std::size_t s = softDiff.firstMismatchIndex / 4;
    EXPECT_EQ(0u, softDiff.mismatches) << what << ": " << softDiff.mismatches << " partial-weight mismatches, first at (" << (softAt.empty() ? 0 : softAt[s * 2]) << ","
                                       << (softAt.empty() ? 0 : softAt[s * 2 + 1]) << ") channel " << (softDiff.firstMismatchIndex % 4);
    EXPECT_GT(exactDiff.compared + softDiff.compared, 0u) << what;
}

// Renders roi on the CPU and on the device from the source over the node's region of interest of
// roi, clipped to the source's own region.
void
expectCropMatches(GpuParityRuntime& runtime,
                  const NodePtr& source,
                  const NodePtr& node,
                  const CropCase& c,
                  unsigned mipmapLevel,
                  const RectI& roi,
                  const std::string& what,
                  std::vector<float>* cpuOut = NULL)
{
    StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL) << what;
    const GpuQuery query = queryFor(node, mipmapLevel);
    ASSERT_EQ(4, query.nComps) << what;
    ASSERT_TRUE(effect->canRenderOnGpu(query)) << what;

    RenderedPlane cpu;
    ASSERT_TRUE(renderColor(node, mipmapLevel, roi, &cpu)) << what;
    ASSERT_EQ(4u, cpu.channels.size()) << what;
    ASSERT_EQ(roi, cpu.window) << what;

    const RectI bounds = effect->getGpuInputRoI(0, roi, RenderScale::fromMipmapLevel(mipmapLevel)).intersect(pixelRegionOfDefinition(source, mipmapLevel));
    std::vector<GpuParityImage> inputs(1);
    if (!bounds.isNull()) {
        ASSERT_TRUE(sourceImage(source, mipmapLevel, bounds, &inputs[0])) << what;
    }
    int tx = 0;
    int ty = 0;
    if (c.reformat) {
        reformatShift(node, mipmapLevel, &tx, &ty);
    }
    const GpuRunResult run = runRenderGpu(runtime, *effect, query, inputs, nullptr, roi, outputLayout());
    expectCropEqual(cpu.pixels, run, roi, inputs[0], tx, ty, c.softness > 0., what);
    if (cpuOut) {
        *cpuOut = cpu.pixels;
    }
}

} // namespace

// Every extent type, with and without reformat, intersect and the black border, soft and hard,
// over the whole region of definition, an odd window, and tiles, at both mipmap levels and at an
// anamorphic project, into padded output views.
TEST_F(NativeCropGpuTest, CropMatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    for (double par : { 1., 2. }) {
        resetProject(par);
        for (const CropCase& c : kCases) {
            NodePtr source;
            NodePtr node;
            ASSERT_TRUE(createGraph(c, false, &source, &node));
            for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
                const std::string level = std::string(c.name) + " par " + std::to_string(par) + " mipmap " + std::to_string(mipmapLevel);
                const RectI rod = pixelRegionOfDefinition(node, mipmapLevel);
                ASSERT_FALSE(rod.isNull()) << level;
                for (const RectI& roi : testWindows(rod, mipmapLevel == 0)) {
                    expectCropMatches(*runtime, source, node, c, mipmapLevel, roi, level + " " + describeRoI(roi));
                    if (HasFatalFailure()) {
                        return;
                    }
                }
            }
        }
    }
}

// A NaN source pixel on the border or on a zero-weight edge comes out 0 on both sides, and one
// the crop keeps stays NaN.
TEST_F(NativeCropGpuTest, NaNSourceOnTheBorderIsWrittenZero)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    resetProject();
    const CropCase borderCase = { "nan border", kNativeGeneratorExtentSize, 10., 8., 40., 30., 0, 0, 1., false, false, true, 0. };
    const CropCase edgeCase = { "nan exact edge", kNativeGeneratorExtentSize, 10.5, 8.5, 40., 30., 0, 0, 1., false, false, false, 6. };
    for (const CropCase& c : { borderCase, edgeCase }) {
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            NodePtr source;
            NodePtr node;
            ASSERT_TRUE(createGraph(c, true, &source, &node));
            const RectI rod = pixelRegionOfDefinition(node, mipmapLevel);
            ASSERT_FALSE(rod.isNull()) << c.name;
            std::vector<float> cpu;
            expectCropMatches(*runtime, source, node, c, mipmapLevel, rod, std::string(c.name) + " mipmap " + std::to_string(mipmapLevel), &cpu);
            if (HasFatalFailure()) {
                return;
            }
            if (mipmapLevel != 0) {
                continue;
            }
            const auto at = [&](int x, int y, int channel) { return cpu[((std::size_t)(y - rod.y1) * rod.width() + (x - rod.x1)) * 4 + channel]; };
            int nNaN = 0;
            for (std::size_t i = 0; i < cpu.size(); ++i) {
                nNaN += std::isnan(cpu[i]) ? 1 : 0;
            }
            EXPECT_GT(nNaN, 0) << c.name << ": no source pixel was kept, so the NaN source is not exercised";
            if (c.blackOutside) {
                for (int y = 8; y < 38; ++y) {
                    EXPECT_EQ(0.f, at(10, y, 0)) << c.name << " left border row " << y;
                    EXPECT_EQ(0.f, at(49, y, 3)) << c.name << " right border row " << y;
                }
            } else {
                for (int y = 9; y < 38; ++y) {
                    EXPECT_EQ(0.f, at(10, y, 0)) << c.name << " zero-weight column, row " << y;
                }
                EXPECT_TRUE(std::isnan(at(25, 20, 0))) << c.name;
            }
        }
    }
}

// The node placed in a GPU region of the scheduler renders the frame the CPU does.
TEST_F(NativeCropGpuTest, EndToEndRunsOnTheGpu)
{
    const CropCase* cases[] = { &kCases[3], &kCases[6] };
    for (const CropCase* c : cases) {
        resetProject();
        NodePtr source;
        NodePtr node;
        ASSERT_TRUE(createGraph(*c, false, &source, &node));
        const RectI window = pixelRegionOfDefinition(node, 0);
        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>(1, node), node, window, kTime);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << c->name << ": " << result.error;
        EXPECT_TRUE(result.ranInRegion(node)) << c->name;
        ASSERT_EQ(result.cpu.size(), result.gpu.size()) << c->name;
        const ImageDiff diff = compareImages(result.cpu, result.gpu, (c->softness > 0.) ? ParityTolerance::resampling() : ParityTolerance::exact());
        EXPECT_EQ(0u, diff.mismatches) << c->name << ": first at float " << diff.firstMismatchIndex;
    }
}

#endif // NATRON_HAVE_GPU
