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
#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "Tests/BaseTest.h"
#include "Tests/Native/NativeGpuParity.h"
#include "Tests/NativeParity.h"
#include "Tests/RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobShuffleMap.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Channel/Shuffle.h"
#include "Engine/Nodes/Filter/Bloom.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Filter/Defocus.h"
#include "Engine/Nodes/Filter/DirBlur.h"
#include "Engine/Nodes/Filter/EdgeBlur.h"
#include "Engine/Nodes/Filter/EdgeDetect.h"
#include "Engine/Nodes/Filter/EdgeExtend.h"
#include "Engine/Nodes/Filter/ErodeDilate.h"
#include "Engine/Nodes/Filter/Glow.h"
#include "Engine/Nodes/Filter/GodRays.h"
#include "Engine/Nodes/Filter/IBlur.h"
#include "Engine/Nodes/Filter/IDefocus.h"
#include "Engine/Nodes/Filter/IErodeDilate.h"
#include "Engine/Nodes/Filter/Sharpen.h"
#include "Engine/Nodes/Filter/VectorBlur.h"
#include "Engine/Nodes/Filter/ZDefocus.h"
#include "Engine/Nodes/Image/EdgeModeKnob.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Merge/Dissolve.h"
#include "Engine/Nodes/Merge/KeyMix.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Nodes/Transform/AdjustBBox.h"
#include "Engine/Nodes/Transform/AutoStraighten.h"
#include "Engine/Nodes/Transform/CornerPin.h"
#include "Engine/Nodes/Transform/CornerPinKnobs.h"
#include "Engine/Nodes/Transform/Crop.h"
#include "Engine/Nodes/Transform/ICornerPin.h"
#include "Engine/Nodes/Transform/IDistort.h"
#include "Engine/Nodes/Transform/ITransform.h"
#include "Engine/Nodes/Transform/IWarp.h"
#include "Engine/Nodes/Transform/LensCorrect.h"
#include "Engine/Nodes/Transform/LensDistortion.h"
#include "Engine/Nodes/Transform/NeuralUpscale.h"
#include "Engine/Nodes/Transform/Reformat.h"
#include "Engine/Nodes/Transform/STMap.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarp.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpContext.h"
#include "Engine/Nodes/Transform/Transform.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

class EdgePolicyTest;

namespace {

const double kTime = 1.;

// The small image every row reads past: a box of the parity source, so its border pixels all
// differ, carrying the edge mode under test.
const RectI kBox(20, 14, 36, 26);
const RectI kWindow(0, 0, kParitySourceWidth, kParitySourceHeight);

const char*
edgeModeName(EdgeMode mode)
{
    return (mode == EdgeMode::Stretch) ? "stretch" : "black";
}

const char*
choiceID(EdgeModeChoice choice)
{
    switch (choice) {
    case EdgeModeChoice::Black:
        return "black";
    case EdgeModeChoice::Stretch:
        return "stretch";
    case EdgeModeChoice::Input:
        break;
    }

    return "input";
}

std::size_t
floatIndex(const RectI& window,
           int x,
           int y,
           int c)
{
    return ((std::size_t)(y - window.y1) * window.width() + (std::size_t)(x - window.x1)) * 4 + c;
}

// The box read by mode at canonical pixel (x, y): the source pixel inside the box, the nearest
// one under Stretch and zero under Black outside it.
float
boxValue(int c,
         int x,
         int y,
         EdgeMode mode)
{
    if (!kBox.contains(x, y) && (mode == EdgeMode::Black)) {
        return 0.f;
    }
    const int sx = std::min(std::max(x, kBox.x1), kBox.x2 - 1);
    const int sy = std::min(std::max(y, kBox.y1), kBox.y2 - 1);

    return paritySourceColorValue(c, sx, sy, kTime);
}

RectI
pixelRegionOfDefinition(const NodePtr& node)
{
    EffectInstancePtr effect = node->getEffectInstance();
    RectD rod;
    bool isProjectFormat = false;
    const StatusEnum st = effect->getRegionOfDefinition_public(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat);

    if ((st == eStatusFailed) || rod.isNull()) {
        return RectI();
    }

    return rod.toPixelEnclosing(0, 1.);
}

RectI
clipToWindow(const RectI& r)
{
    return RectI(std::max(r.x1, kWindow.x1), std::max(r.y1, kWindow.y1), std::min(r.x2, kWindow.x2), std::min(r.y2, kWindow.y2));
}

EdgeMode
outputEdgeMode(const NodePtr& node)
{
    return node->getEffectInstance()->getTimeInvariantMetadata()->getEdgeMode();
}

// The window of node rendered on the CPU, RGBA from the bottom row, zero past its region of
// definition.
bool
renderCpu(const NodePtr& node,
          std::vector<float>* pixels,
          std::string* error)
{
    std::vector<RenderedPlane> planes;
    std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBAComponents());
    if (!renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, kWindow, layers, &planes, error)) {
        return false;
    }
    if ((planes.size() != 1) || (planes[0].channels.size() != 4)) {
        *error = "no single RGBA plane";

        return false;
    }
    *pixels = planes[0].pixels;

    return true;
}

std::vector<float>
extract(const std::vector<float>& frame,
        const RectI& region)
{
    std::vector<float> out;
    for (int y = region.y1; y < region.y2; ++y) {
        for (int x = region.x1; x < region.x2; ++x) {
            for (int c = 0; c < 4; ++c) {
                out.push_back(frame[floatIndex(kWindow, x, y, c)]);
            }
        }
    }

    return out;
}

// Inside `grown` the frame holds the box read by mode, and zero past it.
void
expectRepeatedBox(const std::vector<float>& frame,
                  const RectI& grown,
                  EdgeMode mode,
                  const std::string& what)
{
    int mismatches = 0;
    int outsideTheBox = 0;
    std::string first;
    for (int y = kWindow.y1; y < kWindow.y2; ++y) {
        for (int x = kWindow.x1; x < kWindow.x2; ++x) {
            const bool inGrown = grown.contains(x, y);
            outsideTheBox += (inGrown && !kBox.contains(x, y)) ? 1 : 0;
            for (int c = 0; c < 4; ++c) {
                const float want = inGrown ? boxValue(c, x, y, mode) : 0.f;
                const float got = frame[floatIndex(kWindow, x, y, c)];
                if (got != want) {
                    if (mismatches == 0) {
                        first = std::to_string(x) + "," + std::to_string(y) + " channel " + std::to_string(c) + ": " + std::to_string(got) + ", want " + std::to_string(want);
                    }
                    ++mismatches;
                }
            }
        }
    }
    EXPECT_EQ(0, mismatches) << what << ", first at " << first;
    EXPECT_GT(outsideTheBox, 0) << what;
}

enum class Reads {
    // The box feeds the input the Edge Mode knob governs.
    Source,
    // The box feeds an input read at the output position (A, a mask, a Map or a UV), which follows
    // its own edge mode while the knob, if any, governs a full-frame Source.
    SamePosition
};

enum class Oracle {
    // The row's own hand-computed expectation.
    Hand,
    // The render equals the same node's over the box materialised by a Crop the size of the frame
    // forced to the mode the box is read under, within the node's region of definition: reading past
    // an image by its mode must be the same as reading that image extended by its mode.
    Materialised,
    // The node's result depends on its input's region of definition (pyramids, whole-image
    // filling), so only the CPU/GPU parity and the frame-per-mode checks apply.
    None
};

typedef std::function<NodePtr(EdgePolicyTest& t, const NodePtr& box)> BuildRow;
typedef std::function<void(const std::vector<float>& frame, EdgeMode boxMode, const std::string& what)> HandCheck;

struct MatrixRow {
    const char* name;
    const char* pluginID;
    int major;
    bool hasKnob;
    Reads reads;
    // False for a node that needs files the test run lacks: only its knob and output mode are checked.
    bool renders;
    bool gpu;
    ParityTolerance gpuTolerance;
    Oracle oracle;
    BuildRow build;
    HandCheck hand;
};

const std::vector<MatrixRow>& matrixRows();

} // namespace

class EdgePolicyTest
    : public BaseTest {
public:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();
        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "edgePolicy64x48", 1.));
    }

    NodePtr node(const char* pluginID,
                 int major)
    {
        NodePtr n = createNode(QString::fromUtf8(pluginID), major);
        EXPECT_TRUE(bool(n)) << pluginID;

        return n;
    }

    void connect(const NodePtr& input,
                 const NodePtr& output,
                 int inputNb)
    {
        connectNodes(input, output, inputNb, true);
    }

    NodePtr source(int x,
                   int y)
    {
        NodePtr s = node(kTestPluginIDParitySource, -1);
        if (s) {
            setParitySourceComponents(s, "rgba");
            setParitySourceOrigin(s, x, y);
        }

        return s;
    }

    // A hard Crop of input to rect under the forced mode.
    NodePtr crop(const NodePtr& input,
                 const RectI& rect,
                 EdgeMode mode)
    {
        NodePtr c = node(PLUGINID_NATRON_CROP, PLUGIN_MAJOR_NATRON_CROP);
        if (!input || !c) {
            return NodePtr();
        }
        connect(input, c, 0);
        const bool ok = setKnobValue(c, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize) && setKnobValues(c, kNativeGeneratorParamBottomLeft, { (double)rect.x1, (double)rect.y1 }) && setKnobValues(c, kNativeGeneratorParamSize, { (double)rect.width(), (double)rect.height() }) && setKnobValues(c, kCropParamReformat, { 0. }) && setKnobValues(c, kCropParamSoftness, { 0. }) && setKnobValue(c, kEdgeModeParam, std::string(edgeModeName(mode)));
        EXPECT_TRUE(ok);

        return ok ? c : NodePtr();
    }

    NodePtr box(EdgeMode mode)
    {
        return crop(source(0, 0), kBox, mode);
    }

    // Node pluginID with input at inputNb and the given inputs on the others.
    NodePtr withInputs(const char* pluginID,
                       int major,
                       const std::vector<std::pair<int, NodePtr>>& inputs)
    {
        NodePtr n = node(pluginID, major);
        if (!n) {
            return n;
        }
        for (const std::pair<int, NodePtr>& in : inputs) {
            if (!in.second) {
                ADD_FAILURE() << pluginID << " input " << in.first;
                return NodePtr();
            }
            connect(in.second, n, in.first);
        }

        return n;
    }

    // pluginID reading the box on its input 0.
    NodePtr on(const char* pluginID,
               int major,
               const NodePtr& b)
    {
        return withInputs(pluginID, major, { { 0, b } });
    }

    static bool enableMask(const NodePtr& n)
    {
        KnobChannelSelect* maskChannel = dynamic_cast<KnobChannelSelect*>(n->getKnobByName("maskChannel_Mask").get());
        EXPECT_TRUE(maskChannel != NULL);
        if (!maskChannel) {
            return false;
        }
        maskChannel->set("rgba.A");

        return setKnobValues(n, "enableMask_Mask", { 1. });
    }

    static int maskInputOf(const NodePtr& n)
    {
        StagedImageEffect* staged = n ? dynamic_cast<StagedImageEffect*>(n->getEffectInstance().get()) : NULL;

        return staged ? staged->getMaskInput() : -1;
    }

protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        resetProject();
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }
};

namespace {

BezierPtr
makeCurve(const SplineWarpContextPtr& ctx,
          const std::vector<std::pair<double, double>>& points)
{
    BezierPtr bezier = ctx->makeBezier(points[0].first, points[0].second, kRotoBezierBaseName, kTime, false);
    if (!bezier) {
        return bezier;
    }
    for (std::size_t i = 1; i < points.size(); ++i) {
        bezier->addControlPoint(points[i].first, points[i].second, kTime);
    }
    bezier->setCurveFinished(true);

    return bezier;
}

// A quadrilateral over the box, moved by (dx, dy) and scaled by s about the box centre, so a link
// between two of them bends the box and pulls taps from outside it.
BezierPtr
makeQuad(const SplineWarpContextPtr& ctx,
         double dx,
         double dy,
         double s)
{
    const double base[4][2] = { { 18., 12. }, { 38., 12. }, { 40., 28. }, { 17., 27. } };
    const double cx = 28.;
    const double cy = 20.;
    std::vector<std::pair<double, double>> points;
    for (int i = 0; i < 4; ++i) {
        points.push_back(std::make_pair(cx + s * (base[i][0] - cx) + dx, cy + s * (base[i][1] - cy) + dy));
    }

    return makeCurve(ctx, points);
}

bool
setPins(const NodePtr& n)
{
    const double from[4][2] = { { 0., 0. }, { 64., 0. }, { 64., 48. }, { 0., 48. } };
    const double to[4][2] = { { 6., 3. }, { 58., -4. }, { 70., 50. }, { -3., 44. } };
    const char* const fromNames[4] = { kCornerPinNodeParamFrom1, kCornerPinNodeParamFrom2, kCornerPinNodeParamFrom3, kCornerPinNodeParamFrom4 };
    const char* const toNames[4] = { kCornerPinNodeParamTo1, kCornerPinNodeParamTo2, kCornerPinNodeParamTo3, kCornerPinNodeParamTo4 };

    for (int i = 0; i < 4; ++i) {
        if (!setKnobValues(n, fromNames[i], { from[i][0], from[i][1] }) || !setKnobValues(n, toNames[i], { to[i][0], to[i][1] })) {
            return false;
        }
    }

    return true;
}

NodePtr
orNull(const NodePtr& n,
       bool ok)
{
    EXPECT_TRUE(ok) << (n ? n->getScriptName_mt_safe() : std::string("null node"));

    return (n && ok) ? n : NodePtr();
}

MatrixRow
sourceRow(const char* name,
          const char* pluginID,
          int major,
          const ParityTolerance& gpuTolerance,
          Oracle oracle,
          const BuildRow& build)
{
    return MatrixRow { name, pluginID, major, true, Reads::Source, true, true, gpuTolerance, oracle, build, HandCheck() };
}

MatrixRow
samePositionRow(const char* name,
                const char* pluginID,
                int major,
                bool hasKnob,
                const ParityTolerance& gpuTolerance,
                const BuildRow& build)
{
    return MatrixRow { name, pluginID, major, hasKnob, Reads::SamePosition, true, true, gpuTolerance, Oracle::Materialised, build, HandCheck() };
}

const ParityTolerance kFilterTolerance = ParityTolerance::make(1e-4, 1e-4);
// CPU against CPU over different buffer extents: only summation order and the edge band's extent
// differ, never the pixels read.
const ParityTolerance kOracleTolerance = ParityTolerance::make(1e-4, 1e-4);

std::vector<MatrixRow>
buildMatrixRows()
{
    std::vector<MatrixRow> rows;

    // Taps past the Source, which the Edge Mode knob governs.
    {
        MatrixRow crop = sourceRow("Crop larger than the source", PLUGINID_NATRON_CROP, PLUGIN_MAJOR_NATRON_CROP, ParityTolerance::exact(), Oracle::Hand, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
            NodePtr n = t.on(PLUGINID_NATRON_CROP, PLUGIN_MAJOR_NATRON_CROP, b);
            return orNull(n, n && setKnobValue(n, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize) && setKnobValues(n, kNativeGeneratorParamBottomLeft, { 8., 6. }) && setKnobValues(n, kNativeGeneratorParamSize, { 48., 36. }) && setKnobValues(n, kCropParamReformat, { 0. }) && setKnobValues(n, kCropParamSoftness, { 0. }));
        });
        crop.hand = [](const std::vector<float>& frame, EdgeMode mode, const std::string& what) {
            expectRepeatedBox(frame, RectI(8, 6, 56, 42), mode, what);
        };
        rows.push_back(crop);
    }
    {
        MatrixRow adjust = sourceRow("AdjustBBox", PLUGINID_NATRON_ADJUSTBBOX, PLUGIN_MAJOR_NATRON_ADJUSTBBOX, ParityTolerance::exact(), Oracle::Hand, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
            NodePtr n = t.on(PLUGINID_NATRON_ADJUSTBBOX, PLUGIN_MAJOR_NATRON_ADJUSTBBOX, b);
            return orNull(n, n && setKnobValues(n, kAdjustBBoxParamNumPixels, { 6., 4. }));
        });
        adjust.hand = [](const std::vector<float>& frame, EdgeMode mode, const std::string& what) {
            expectRepeatedBox(frame, RectI(kBox.x1 - 6, kBox.y1 - 4, kBox.x2 + 6, kBox.y2 + 4), mode, what);
        };
        rows.push_back(adjust);
    }
    rows.push_back(sourceRow("Transform", PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM, ParityTolerance::resampling(), Oracle::Materialised, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.on(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM, b);
        return orNull(n, n && setKnobValues(n, kTransformNodeParamCenter, { 28., 20. }) && setKnobValues(n, kTransformNodeParamRotate, { 12. }) && setKnobValues(n, kTransformNodeParamTranslate, { 2., 1. }));
    }));
    rows.push_back(sourceRow("Reformat", PLUGINID_NATRON_REFORMAT, PLUGIN_MAJOR_NATRON_REFORMAT, ParityTolerance::resampling(), Oracle::Materialised, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.on(PLUGINID_NATRON_REFORMAT, PLUGIN_MAJOR_NATRON_REFORMAT, b);
        return orNull(n, n && setKnobValue(n, kReformatParamType, kReformatParamTypeOptionToBox) && setKnobValues(n, kReformatParamBoxSize, { 100., 80. }) && setKnobValues(n, kReformatParamBoxFixed, { 1. }) && setKnobValue(n, kReformatParamResize, kReformatParamResizeOptionDistort));
    }));
    rows.push_back(sourceRow("CornerPin", PLUGINID_NATRON_CORNERPIN, PLUGIN_MAJOR_NATRON_CORNERPIN, ParityTolerance::resampling(), Oracle::Materialised, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.on(PLUGINID_NATRON_CORNERPIN, PLUGIN_MAJOR_NATRON_CORNERPIN, b);
        return orNull(n, n && setPins(n));
    }));
    rows.push_back(sourceRow("ITransform", PLUGINID_NATRON_ITRANSFORM, PLUGIN_MAJOR_NATRON_ITRANSFORM, ParityTolerance::resampling(), Oracle::Materialised, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.withInputs(PLUGINID_NATRON_ITRANSFORM, PLUGIN_MAJOR_NATRON_ITRANSFORM, { { kIWarpInputSource, b } });
        return orNull(n, n && setKnobValues(n, kTransformNodeParamCenter, { 28., 20. }) && setKnobValues(n, kTransformNodeParamRotate, { 12. }) && setKnobValues(n, kTransformNodeParamTranslate, { 2., 1. }));
    }));
    rows.push_back(sourceRow("ICornerPin", PLUGINID_NATRON_ICORNERPIN, PLUGIN_MAJOR_NATRON_ICORNERPIN, ParityTolerance::resampling(), Oracle::Materialised, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.withInputs(PLUGINID_NATRON_ICORNERPIN, PLUGIN_MAJOR_NATRON_ICORNERPIN, { { kIWarpInputSource, b } });
        return orNull(n, n && setPins(n));
    }));
    rows.push_back(sourceRow("LensDistortion", PLUGINID_NATRON_LENSDISTORTION, PLUGIN_MAJOR_NATRON_LENSDISTORTION, ParityTolerance::resampling(), Oracle::Materialised, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.withInputs(PLUGINID_NATRON_LENSDISTORTION, PLUGIN_MAJOR_NATRON_LENSDISTORTION, { { kLensDistortionInputSource, b } });
        return orNull(n, n && setKnobValue(n, kLensDistortionParamModel, std::string(kLensDistortionModelNuke)) && setKnobValues(n, kLensDistortionParamK1, { 0.3 }));
    }));
    rows.push_back(sourceRow("LensCorrect", PLUGINID_NATRON_LENSCORRECT, PLUGIN_MAJOR_NATRON_LENSCORRECT, ParityTolerance::make(2e-4, 1e-4), Oracle::Materialised, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.withInputs(PLUGINID_NATRON_LENSCORRECT, PLUGIN_MAJOR_NATRON_LENSCORRECT, { { kLensCorrectInputSource, b } });
        return orNull(n, n && setKnobValue(n, kLensCorrectParamCamera, std::string("Canon EOS 5D Mark III")) && setKnobValue(n, kLensCorrectParamLens, std::string("Canon EF 70-200mm f/2.8L IS II USM")) && setKnobValues(n, kLensCorrectParamFocalLength, { 100. }) && setKnobValues(n, kLensCorrectParamAperture, { 2.8 }) && setKnobValue(n, kLensCorrectParamMode, std::string(kLensCorrectModeDistort)) && setKnobValue(n, kLensCorrectParamTargetGeometry, std::string(kLensCorrectGeometryLens)) && setKnobValues(n, kLensCorrectParamAutoScale, { 0. }) && setKnobValues(n, kLensCorrectParamScale, { 0.7 }));
    }));
    rows.push_back(sourceRow("AutoStraighten", PLUGINID_NATRON_AUTOSTRAIGHTEN, PLUGIN_MAJOR_NATRON_AUTOSTRAIGHTEN, ParityTolerance::make(1e-5, 1e-4), Oracle::Materialised, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.on(PLUGINID_NATRON_AUTOSTRAIGHTEN, PLUGIN_MAJOR_NATRON_AUTOSTRAIGHTEN, b);
        return orNull(n, n && setKnobValues(n, kAutoStraightenParamRotation, { 7. }) && setKnobValue(n, kAutoStraightenParamAutoCrop, std::string("off")));
    }));
    rows.push_back(sourceRow("SplineWarp", PLUGINID_NATRON_SPLINEWARP, PLUGIN_MAJOR_NATRON_SPLINEWARP, ParityTolerance::resampling(), Oracle::Materialised, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.withInputs(PLUGINID_NATRON_SPLINEWARP, PLUGIN_MAJOR_NATRON_SPLINEWARP, { { kSplineWarpInputSource, b } });
        SplineWarpContextPtr ctx = n ? std::dynamic_pointer_cast<SplineWarpContext>(n->getCurveContext()) : SplineWarpContextPtr();
        if (!ctx) {
            ADD_FAILURE() << "SplineWarp context";
            return NodePtr();
        }
        BezierPtr from = makeQuad(ctx, 0., 0., 1.);
        BezierPtr to = makeQuad(ctx, 2.75, -1.5, 0.8);
        if (!from || !to || !ctx->linkWithAlignment(from, to, 0., false)) {
            ADD_FAILURE() << "SplineWarp link";
            return NodePtr();
        }
        return orNull(n, setKnobValues(n, kSplineWarpParamBoundaryBBox, { 0. }) && setKnobValue(n, kSplineWarpParamBBox, std::string(kSplineWarpBBoxUnion)));
    }));
    rows.push_back(sourceRow("IDistort", PLUGINID_NATRON_IDISTORT, PLUGIN_MAJOR_NATRON_IDISTORT, ParityTolerance::resampling(), Oracle::Materialised, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.withInputs(PLUGINID_NATRON_IDISTORT, PLUGIN_MAJOR_NATRON_IDISTORT, { { kIDistortInputSource, b }, { kIDistortInputUV, t.source(3, 5) } });
        return orNull(n, n && setKnobValues(n, kIDistortParamUVScale, { 5., 4. }));
    }));
    rows.push_back(sourceRow("STMap", PLUGINID_NATRON_STMAP, PLUGIN_MAJOR_NATRON_STMAP, ParityTolerance::resampling(), Oracle::Materialised, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        return t.withInputs(PLUGINID_NATRON_STMAP, PLUGIN_MAJOR_NATRON_STMAP, { { kSTMapInputSource, b }, { kSTMapInputUV, t.source(3, 5) } });
    }));
    rows.push_back(sourceRow("Blur", PLUGINID_NATRON_BLUR, PLUGIN_MAJOR_NATRON_BLUR, ParityTolerance::make(2e-6, 0.), Oracle::Materialised, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.on(PLUGINID_NATRON_BLUR, PLUGIN_MAJOR_NATRON_BLUR, b);
        return orNull(n, n && setKnobValues(n, kBlurParamSize, { 6., 6. }));
    }));
    rows.push_back(sourceRow("Defocus", PLUGINID_NATRON_DEFOCUS, PLUGIN_MAJOR_NATRON_DEFOCUS, ParityTolerance::make(1e-5, 1e-4), Oracle::Materialised, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.withInputs(PLUGINID_NATRON_DEFOCUS, PLUGIN_MAJOR_NATRON_DEFOCUS, { { kDefocusInputSource, b } });
        return orNull(n, n && setKnobValues(n, kDefocusParamSize, { 8. }));
    }));
    rows.push_back(sourceRow("DirBlur", PLUGINID_NATRON_DIRBLUR, PLUGIN_MAJOR_NATRON_DIRBLUR, ParityTolerance::make(1e-5, 1e-4), Oracle::Materialised, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.withInputs(PLUGINID_NATRON_DIRBLUR, PLUGIN_MAJOR_NATRON_DIRBLUR, { { kDirBlurInputSource, b } });
        return orNull(n, n && setKnobValues(n, kDirBlurParamLength, { 6. }) && setKnobValues(n, kDirBlurParamDirection, { 30. }));
    }));
    rows.push_back(sourceRow("GodRays", PLUGINID_NATRON_GODRAYS, PLUGIN_MAJOR_NATRON_GODRAYS, ParityTolerance::make(1e-5, 1e-4), Oracle::Materialised, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.withInputs(PLUGINID_NATRON_GODRAYS, PLUGIN_MAJOR_NATRON_GODRAYS, { { kGodRaysInputSource, b } });
        return orNull(n, n && setKnobValues(n, kGodRaysParamCenter, { 28., 20. }) && setKnobValues(n, kGodRaysParamScale, { 1.3, 1.3 }) && setKnobValues(n, kGodRaysParamSamples, { 9. }));
    }));
    rows.push_back(sourceRow("EdgeDetect", PLUGINID_NATRON_EDGEDETECT, PLUGIN_MAJOR_NATRON_EDGEDETECT, ParityTolerance::iir(), Oracle::Materialised, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        return t.on(PLUGINID_NATRON_EDGEDETECT, PLUGIN_MAJOR_NATRON_EDGEDETECT, b);
    }));
    rows.push_back(sourceRow("ErodeDilate", PLUGINID_NATRON_ERODEDILATE, PLUGIN_MAJOR_NATRON_ERODEDILATE, ParityTolerance::exact(), Oracle::Materialised, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.on(PLUGINID_NATRON_ERODEDILATE, PLUGIN_MAJOR_NATRON_ERODEDILATE, b);
        return orNull(n, n && setKnobValues(n, kErodeDilateParamSize, { 3., 2. }));
    }));
    rows.push_back(sourceRow("Sharpen", PLUGINID_NATRON_SHARPEN, PLUGIN_MAJOR_NATRON_SHARPEN, ParityTolerance::iir(), Oracle::Materialised, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.on(PLUGINID_NATRON_SHARPEN, PLUGIN_MAJOR_NATRON_SHARPEN, b);
        return orNull(n, n && setKnobValues(n, kSharpenParamSize, { 3., 3. }));
    }));
    rows.push_back(sourceRow("IBlur", PLUGINID_NATRON_IBLUR, PLUGIN_MAJOR_NATRON_IBLUR, ParityTolerance::make(0., 1e-4), Oracle::Materialised, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.withInputs(PLUGINID_NATRON_IBLUR, PLUGIN_MAJOR_NATRON_IBLUR, { { kIBlurInputSource, b }, { kIBlurInputMap, t.source(3, 5) } });
        return orNull(n, n && setKnobValues(n, kIBlurParamSize, { 6., 6. }));
    }));
    rows.push_back(sourceRow("IErodeDilate", PLUGINID_NATRON_IERODEDILATE, PLUGIN_MAJOR_NATRON_IERODEDILATE, ParityTolerance::exact(), Oracle::Materialised, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.withInputs(PLUGINID_NATRON_IERODEDILATE, PLUGIN_MAJOR_NATRON_IERODEDILATE, { { kIErodeDilateInputSource, b }, { kIErodeDilateInputMap, t.source(3, 5) } });
        return orNull(n, n && setKnobValues(n, kIErodeDilateParamSize, { 4., 3. }));
    }));
    rows.push_back(sourceRow("IDefocus", PLUGINID_NATRON_IDEFOCUS, PLUGIN_MAJOR_NATRON_IDEFOCUS, ParityTolerance::make(1e-5, 1e-4), Oracle::Materialised, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.withInputs(PLUGINID_NATRON_IDEFOCUS, PLUGIN_MAJOR_NATRON_IDEFOCUS, { { kIDefocusInputSource, b }, { kIDefocusInputMap, t.source(3, 5) } });
        return orNull(n, n && setKnobValues(n, kDefocusParamSize, { 8. }));
    }));
    rows.push_back(sourceRow("ZDefocus with its Depth past the source", PLUGINID_NATRON_ZDEFOCUS, PLUGIN_MAJOR_NATRON_ZDEFOCUS, ParityTolerance::make(1e-4, 1e-3), Oracle::Materialised, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.withInputs(PLUGINID_NATRON_ZDEFOCUS, PLUGIN_MAJOR_NATRON_ZDEFOCUS, { { kZDefocusInputSource, b } });
        KnobChannelSelect* depth = n ? dynamic_cast<KnobChannelSelect*>(n->getKnobByName(kZDefocusParamDepth).get()) : NULL;
        if (!depth) {
            ADD_FAILURE() << "ZDefocus depth";
            return NodePtr();
        }
        depth->set("rgba.R");
        return orNull(n, setKnobValues(n, kZDefocusParamFocalPlane, { 0.5 }) && setKnobValues(n, kZDefocusParamSize, { 9. }) && setKnobValues(n, kZDefocusParamMaxSize, { 16. }));
    }));
    rows.push_back(sourceRow("VectorBlur with its UV past the source", PLUGINID_NATRON_VECTORBLUR, PLUGIN_MAJOR_NATRON_VECTORBLUR, ParityTolerance::make(1e-5, 1e-4), Oracle::Materialised, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.withInputs(PLUGINID_NATRON_VECTORBLUR, PLUGIN_MAJOR_NATRON_VECTORBLUR, { { kVectorBlurInputSource, b } });
        KnobLayerSelect* uv = n ? dynamic_cast<KnobLayerSelect*>(n->getKnobByName(kVectorBlurParamUV).get()) : NULL;
        if (!uv) {
            ADD_FAILURE() << "VectorBlur UV";
            return NodePtr();
        }
        uv->setLayer(kNatronColorViewRGBA);
        uv->setChannels({ "R", "G" });
        return orNull(n, setKnobValues(n, kVectorBlurParamScale, { 4. }));
    }));
    rows.push_back(sourceRow("Glow", PLUGINID_NATRON_GLOW, PLUGIN_MAJOR_NATRON_GLOW, kFilterTolerance, Oracle::None, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.on(PLUGINID_NATRON_GLOW, PLUGIN_MAJOR_NATRON_GLOW, b);
        return orNull(n, n && setKnobValues(n, kGlowParamSize, { 3. }) && setKnobValues(n, kGlowParamCount, { 3. }));
    }));
    rows.push_back(sourceRow("Bloom", PLUGINID_NATRON_BLOOM, PLUGIN_MAJOR_NATRON_BLOOM, kFilterTolerance, Oracle::None, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.on(PLUGINID_NATRON_BLOOM, PLUGIN_MAJOR_NATRON_BLOOM, b);
        return orNull(n, n && setKnobValues(n, kBloomParamSize, { 3., 3. }));
    }));
    rows.push_back(sourceRow("EdgeExtend", PLUGINID_NATRON_EDGEEXTEND, PLUGIN_MAJOR_NATRON_EDGEEXTEND, ParityTolerance::resampling(), Oracle::None, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        return t.on(PLUGINID_NATRON_EDGEEXTEND, PLUGIN_MAJOR_NATRON_EDGEEXTEND, b);
    }));
    // The matte EdgeBlur derives from its source always repeats its edge, so a box materialised black
    // gains a matte edge the Black read does not have.
    rows.push_back(sourceRow("EdgeBlur", PLUGINID_NATRON_EDGEBLUR, PLUGIN_MAJOR_NATRON_EDGEBLUR, ParityTolerance::make(2e-4, 1e-4), Oracle::None, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.withInputs(PLUGINID_NATRON_EDGEBLUR, PLUGIN_MAJOR_NATRON_EDGEBLUR, { { kEdgeBlurInputSource, b } });
        return orNull(n, n && setKnobValues(n, kEdgeBlurParamSize, { 9. }));
    }));
    {
        // Inference needs a downloaded model, which the matrix does not fetch.
        MatrixRow upscale = sourceRow("NeuralUpscale", PLUGINID_NATRON_NEURALUPSCALE, PLUGIN_MAJOR_NATRON_NEURALUPSCALE, ParityTolerance::exact(), Oracle::None, [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
            return t.withInputs(PLUGINID_NATRON_NEURALUPSCALE, PLUGIN_MAJOR_NATRON_NEURALUPSCALE, { { kNeuralUpscaleInputSource, b } });
        });
        upscale.renders = false;
        upscale.gpu = false;
        rows.push_back(upscale);
    }

    // Same-position reads of the box, by its own edge mode.
    {
        MatrixRow merge = samePositionRow("Merge A smaller than B", PLUGINID_NATRON_NATIVE_MERGE, PLUGIN_MAJOR_NATRON_NATIVE_MERGE, false, ParityTolerance::arithmetic(), [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
            NodePtr n = t.withInputs(PLUGINID_NATRON_NATIVE_MERGE, PLUGIN_MAJOR_NATRON_NATIVE_MERGE, { { kNativeMergeInputB, t.source(0, 0) }, { kNativeMergeInputA, b } });
            return orNull(n, n && setKnobValue(n, kNativeMergeParamOperation, std::string("over")));
        });
        merge.oracle = Oracle::Hand;
        // Past A's box, A over B is B under Black, and the nearest A pixel over B under Stretch.
        merge.hand = [](const std::vector<float>& frame, EdgeMode mode, const std::string& what) {
            const int probes[4][2] = { { 2, 2 }, { 61, 3 }, { 4, 45 }, { 60, 44 } };
            for (const auto& p : probes) {
                const float aAlpha = boxValue(3, p[0], p[1], mode);
                for (int c = 0; c < 4; ++c) {
                    const float bValue = paritySourceColorValue(c, p[0], p[1], kTime);
                    const float got = frame[floatIndex(kWindow, p[0], p[1], c)];
                    if (mode == EdgeMode::Black) {
                        EXPECT_EQ(bValue, got) << what << " at " << p[0] << "," << p[1] << " channel " << c;
                    } else {
                        const float want = boxValue(c, p[0], p[1], mode) + bValue * (1.f - aAlpha);
                        EXPECT_NEAR(want, got, 1e-5) << what << " at " << p[0] << "," << p[1] << " channel " << c;
                    }
                }
            }
        };
        rows.push_back(merge);
    }
    rows.push_back(samePositionRow("Merge mask", PLUGINID_NATRON_NATIVE_MERGE, PLUGIN_MAJOR_NATRON_NATIVE_MERGE, false, ParityTolerance::arithmetic(), [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.withInputs(PLUGINID_NATRON_NATIVE_MERGE, PLUGIN_MAJOR_NATRON_NATIVE_MERGE, { { kNativeMergeInputB, t.source(0, 0) }, { kNativeMergeInputA, t.source(4, 2) }, { kNativeMergeInputMask, b } });
        return orNull(n, n && EdgePolicyTest::enableMask(n));
    }));
    rows.push_back(samePositionRow("KeyMix A smaller than B", PLUGINID_NATRON_KEYMIX, PLUGIN_MAJOR_NATRON_KEYMIX, false, ParityTolerance::arithmetic(), [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.withInputs(PLUGINID_NATRON_KEYMIX, PLUGIN_MAJOR_NATRON_KEYMIX, { { kKeyMixInputB, t.source(0, 0) }, { kKeyMixInputA, b }, { kKeyMixInputMask, t.source(5, 3) } });
        return orNull(n, n && EdgePolicyTest::enableMask(n));
    }));
    rows.push_back(samePositionRow("KeyMix mask", PLUGINID_NATRON_KEYMIX, PLUGIN_MAJOR_NATRON_KEYMIX, false, ParityTolerance::arithmetic(), [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.withInputs(PLUGINID_NATRON_KEYMIX, PLUGIN_MAJOR_NATRON_KEYMIX, { { kKeyMixInputB, t.source(0, 0) }, { kKeyMixInputA, t.source(4, 2) }, { kKeyMixInputMask, b } });
        return orNull(n, n && EdgePolicyTest::enableMask(n));
    }));
    rows.push_back(samePositionRow("Dissolve", PLUGINID_NATRON_DISSOLVE, PLUGIN_MAJOR_NATRON_DISSOLVE, false, ParityTolerance::arithmetic(), [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.withInputs(PLUGINID_NATRON_DISSOLVE, PLUGIN_MAJOR_NATRON_DISSOLVE, { { dissolveSourceInput(0), t.source(0, 0) }, { dissolveSourceInput(1), b } });
        return orNull(n, n && setKnobValues(n, kDissolveParamWhich, { 0.5 }));
    }));
    rows.push_back(samePositionRow("ShuffleCopy", PLUGINID_NATRON_SHUFFLECOPY, -1, false, ParityTolerance::exact(), [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.withInputs(PLUGINID_NATRON_SHUFFLECOPY, -1, { { Shuffle::eInputMain, t.source(0, 0) }, { Shuffle::eInputCopy1, b } });
        if (!n) {
            return n;
        }
        const char* const layerKnobs[3] = { kShuffleParamIn1, kShuffleParamIn2, kShuffleParamOut1 };
        for (const char* name : layerKnobs) {
            KnobLayerSelectPtr knob = std::dynamic_pointer_cast<KnobLayerSelect>(n->getKnobByName(name));
            if (!knob) {
                ADD_FAILURE() << name;
                return NodePtr();
            }
            knob->setLayer(kNatronColorViewRGBA);
        }
        KnobLayerSelectPtr out2 = std::dynamic_pointer_cast<KnobLayerSelect>(n->getKnobByName(kShuffleParamOut2));
        KnobShuffleMapPtr mapping = std::dynamic_pointer_cast<KnobShuffleMap>(n->getKnobByName(kShuffleParamMapping));
        if (!out2 || !mapping) {
            ADD_FAILURE() << "ShuffleCopy knobs";
            return NodePtr();
        }
        out2->setLayer("");
        mapping->setSource(1, 3, ShuffleSource::makeInput(2, 3));
        return orNull(n, setKnobValue(n, kShuffleCopyParamBBox, std::string("2")));
    }));
    rows.push_back(samePositionRow("Blur mask", PLUGINID_NATRON_BLUR, PLUGIN_MAJOR_NATRON_BLUR, true, ParityTolerance::make(2e-6, 0.), [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.on(PLUGINID_NATRON_BLUR, PLUGIN_MAJOR_NATRON_BLUR, t.source(3, 5));
        const int mask = EdgePolicyTest::maskInputOf(n);
        if (!n || (mask < 0)) {
            ADD_FAILURE() << "Blur mask input";
            return NodePtr();
        }
        t.connect(b, n, mask);
        return orNull(n, EdgePolicyTest::enableMask(n) && setKnobValues(n, kBlurParamSize, { 6., 6. }));
    }));
    rows.push_back(samePositionRow("IBlur Map", PLUGINID_NATRON_IBLUR, PLUGIN_MAJOR_NATRON_IBLUR, true, ParityTolerance::make(0., 1e-4), [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.withInputs(PLUGINID_NATRON_IBLUR, PLUGIN_MAJOR_NATRON_IBLUR, { { kIBlurInputSource, t.source(3, 5) }, { kIBlurInputMap, b } });
        return orNull(n, n && setKnobValues(n, kIBlurParamSize, { 6., 6. }));
    }));
    rows.push_back(samePositionRow("IErodeDilate Map", PLUGINID_NATRON_IERODEDILATE, PLUGIN_MAJOR_NATRON_IERODEDILATE, true, ParityTolerance::exact(), [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.withInputs(PLUGINID_NATRON_IERODEDILATE, PLUGIN_MAJOR_NATRON_IERODEDILATE, { { kIErodeDilateInputSource, t.source(3, 5) }, { kIErodeDilateInputMap, b } });
        return orNull(n, n && setKnobValues(n, kIErodeDilateParamSize, { 4., 3. }));
    }));
    rows.push_back(samePositionRow("IDefocus Map", PLUGINID_NATRON_IDEFOCUS, PLUGIN_MAJOR_NATRON_IDEFOCUS, true, ParityTolerance::make(1e-5, 1e-4), [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.withInputs(PLUGINID_NATRON_IDEFOCUS, PLUGIN_MAJOR_NATRON_IDEFOCUS, { { kIDefocusInputSource, t.source(3, 5) }, { kIDefocusInputMap, b } });
        return orNull(n, n && setKnobValues(n, kDefocusParamSize, { 8. }));
    }));
    rows.push_back(samePositionRow("IDistort UV", PLUGINID_NATRON_IDISTORT, PLUGIN_MAJOR_NATRON_IDISTORT, true, ParityTolerance::resampling(), [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        NodePtr n = t.withInputs(PLUGINID_NATRON_IDISTORT, PLUGIN_MAJOR_NATRON_IDISTORT, { { kIDistortInputSource, t.source(3, 5) }, { kIDistortInputUV, b } });
        return orNull(n, n && setKnobValues(n, kIDistortParamUVScale, { 5., 4. }));
    }));
    rows.push_back(samePositionRow("STMap UV", PLUGINID_NATRON_STMAP, PLUGIN_MAJOR_NATRON_STMAP, true, ParityTolerance::resampling(), [](EdgePolicyTest& t, const NodePtr& b) -> NodePtr {
        return t.withInputs(PLUGINID_NATRON_STMAP, PLUGIN_MAJOR_NATRON_STMAP, { { kSTMapInputSource, t.source(3, 5) }, { kSTMapInputUV, b } });
    }));

    return rows;
}

const std::vector<MatrixRow>&
matrixRows()
{
    static const std::vector<MatrixRow> rows = buildMatrixRows();

    return rows;
}

// The plugins whose Edge Mode knob is a copy of another node's, which that node's row covers.
const std::map<std::string, std::string>&
delegatedEdgeModeKnobs()
{
    static const std::map<std::string, std::string> delegated = {
        { PLUGINID_NATRON_TRACKER, "its Edge Mode drives its internal Transform" },
    };

    return delegated;
}

struct RenderedCase {
    std::string what;
    // The mode the knob's input is read under, and the mode the box is read under.
    EdgeMode knobMode;
    EdgeMode boxMode;
    std::vector<float> frame;
};

} // namespace

class EdgePolicyMatrixTest
    : public EdgePolicyTest,
      public ::testing::WithParamInterface<int> {
protected:
    // node at choice, or null when the knob cannot be set.
    static NodePtr applyChoice(const MatrixRow& row,
                               const NodePtr& n,
                               EdgeModeChoice choice)
    {
        if (!n) {
            return n;
        }
        if (!row.hasKnob) {
            EXPECT_FALSE(bool(n->getKnobByName(kEdgeModeParam))) << row.name << " has an Edge Mode knob its row does not declare";

            return n;
        }
        if (!n->getKnobByName(kEdgeModeParam)) {
            ADD_FAILURE() << row.name << " has no Edge Mode knob";

            return NodePtr();
        }

        return setKnobValue(n, kEdgeModeParam, std::string(choiceID(choice))) ? n : NodePtr();
    }

    void expectMatchesTheMaterialisedBox(const MatrixRow& row,
                                         EdgeModeChoice choice,
                                         EdgeMode boxMode,
                                         const RectI& region,
                                         const std::vector<float>& frame,
                                         const std::string& what)
    {
        NodePtr materialised = crop(box(boxMode), kWindow, boxMode);
        ASSERT_TRUE(bool(materialised)) << what;
        NodePtr n = applyChoice(row, row.build(*this, materialised), choice);
        ASSERT_TRUE(bool(n)) << what << ": the materialised graph";
        std::vector<float> reference;
        std::string error;
        ASSERT_TRUE(renderCpu(n, &reference, &error)) << what << ": the materialised render: " << error;
        const ImageDiff diff = compareImages(extract(reference, region), extract(frame, region), kOracleTolerance);
        EXPECT_EQ(0u, diff.mismatches) << what << ": differs from the box materialised " << edgeModeName(boxMode) << " at pixel " << diff.firstMismatchIndex / 4 << " of " << region.x1 << "," << region.y1 << "-" << region.x2 << "," << region.y2 << ", worst " << diff.worstAbsolute;
    }
};

// Each row's node reads past the box under input Stretch and input Black and, where it has the
// knob, each of its three choices: the output carries the resolved mode, the CPU frame meets the
// row's oracle, the GPU frame matches it, and the frame depends on the resolved modes alone.
TEST_P(EdgePolicyMatrixTest, ReadsPastTheBoxByItsMode)
{
    const MatrixRow& row = matrixRows()[(std::size_t)GetParam()];
    SCOPED_TRACE(row.name);

    std::vector<EdgeModeChoice> choices(1, EdgeModeChoice::Input);
    if (row.hasKnob) {
        choices = { EdgeModeChoice::Input, EdgeModeChoice::Black, EdgeModeChoice::Stretch };
    }
    std::vector<RenderedCase> cases;
    for (EdgeMode inputMode : { EdgeMode::Stretch, EdgeMode::Black }) {
        for (EdgeModeChoice choice : choices) {
            resetProject();
            const std::string what = std::string(row.name) + ", " + choiceID(choice) + " over a " + edgeModeName(inputMode) + " box";
            // A same-position row's knob governs a full-frame parity source, which is Stretch.
            const EdgeMode knobMode = resolveEdgeMode(choice, (row.reads == Reads::Source) ? inputMode : EdgeMode::Stretch);
            const EdgeMode boxMode = (row.reads == Reads::Source) ? knobMode : inputMode;

            NodePtr n = applyChoice(row, row.build(*this, box(inputMode)), choice);
            ASSERT_TRUE(bool(n)) << what;
            EXPECT_EQ(knobMode, outputEdgeMode(n)) << what << ": the output's edge mode";
            if (!row.renders) {
                continue;
            }

            RenderedCase rendered = { what, knobMode, boxMode, std::vector<float>() };
            std::string error;
            ASSERT_TRUE(renderCpu(n, &rendered.frame, &error)) << what << ": " << error;
            const RectI region = clipToWindow(pixelRegionOfDefinition(n));
            ASSERT_FALSE(region.isNull()) << what;

            switch (row.oracle) {
            case Oracle::Hand:
                row.hand(rendered.frame, boxMode, what);
                break;
            case Oracle::Materialised:
                expectMatchesTheMaterialisedBox(row, choice, boxMode, region, rendered.frame, what);
                break;
            case Oracle::None:
                break;
            }

            const EndToEndResult gpu = renderEndToEnd(std::vector<NodePtr>(1, n), n, kWindow, kTime);
            if (!gpu.skipped) {
                ASSERT_TRUE(gpu.ok) << what << ": " << gpu.error;
                if (row.gpu) {
                    EXPECT_EQ(1u, gpu.gpuTasksPerNode.count(n->getScriptName_mt_safe())) << what << ": the node fell back to the CPU";
                }
                ASSERT_EQ(gpu.cpu.size(), gpu.gpu.size()) << what;
                ASSERT_FALSE(gpu.cpu.empty()) << what;
                const ImageDiff diff = compareImages(gpu.cpu, gpu.gpu, row.gpuTolerance);
                EXPECT_EQ(0u, diff.mismatches) << what << ": GPU against CPU, first at float " << diff.firstMismatchIndex << ", worst " << diff.worstAbsolute;
            }
            cases.push_back(rendered);
        }
    }

    for (std::size_t i = 0; i < cases.size(); ++i) {
        for (std::size_t j = i + 1; j < cases.size(); ++j) {
            if (cases[i].knobMode != cases[j].knobMode) {
                continue;
            }
            if (cases[i].boxMode == cases[j].boxMode) {
                EXPECT_TRUE(cases[i].frame == cases[j].frame) << cases[i].what << " and " << cases[j].what << " resolve to the same modes but render different frames";
            } else {
                EXPECT_FALSE(cases[i].frame == cases[j].frame) << cases[i].what << " and " << cases[j].what << " render the same frame, so the box's edge mode is not exercised";
            }
        }
    }
}

INSTANTIATE_TEST_CASE_P(Rows, EdgePolicyMatrixTest, ::testing::Range(0, (int)matrixRows().size()));

// Every registered native plugin with the Edge Mode knob has a matrix row, and every row that
// declares the knob names a plugin that has it.
TEST_F(EdgePolicyTest, EveryNodeWithTheEdgeModeKnobHasAMatrixRow)
{
    std::set<std::string> covered;
    for (const MatrixRow& row : matrixRows()) {
        if (row.hasKnob) {
            covered.insert(row.pluginID);
        }
    }

    std::set<std::string> withKnob;
    for (const auto& versions : appPTR->getPluginsList()) {
        for (const Plugin* plugin : versions.second) {
            // The OFX descriptor is only loaded on demand, so the plugin handle is the reliable OFX marker.
            if (plugin->getOfxPlugin() || !plugin->getIsUserCreatable()) {
                continue;
            }
            NodePtr n = createNode(plugin->getPluginID(), plugin->getMajorVersion());
            if (!n || !n->getKnobByName(kEdgeModeParam)) {
                continue;
            }
            const std::string id = versions.first;
            withKnob.insert(id);
            EXPECT_TRUE(covered.count(id) || delegatedEdgeModeKnobs().count(id)) << id << " has the Edge Mode knob but no row in the edge policy matrix";
        }
    }
    for (const std::string& id : covered) {
        EXPECT_EQ(1u, withKnob.count(id)) << id << " has a matrix row for its Edge Mode knob but no registered plugin of that id has the knob";
    }
}
