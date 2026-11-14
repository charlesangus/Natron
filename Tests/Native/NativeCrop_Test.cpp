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

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include <ofxNatron.h>

#include "BaseTest.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/HostOverlaySupport.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Color/Invert.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/EdgeModeKnob.h"
#include "Engine/Nodes/Image/NativeGenerator.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Transform/Crop.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kCropID = PLUGINID_NATRON_CROP;
const int kNativeCropMajor = PLUGIN_MAJOR_NATRON_CROP;
const double kTime = 1.;

// Softness 0 copies source pixels, so the output is bit-identical.
const ParityTolerance kExactTolerance = ParityTolerance::exact();
// A soft edge multiplies by a smoothstep evaluated in double, then rounds to float.
const ParityTolerance kSoftTolerance = ParityTolerance::resampling();

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

// Sets a choice by option ID as a user edit, which is what makes the OpenFX plug-in refresh
// which of its knobs are shown.
bool
setChoiceAsUser(const NodePtr& node,
                const std::string& name,
                const std::string& optionID)
{
    KnobChoice* choice = node ? dynamic_cast<KnobChoice*>(node->getKnobByName(name).get()) : NULL;

    EXPECT_TRUE(choice != NULL) << name;
    if (!choice) {
        return false;
    }
    const std::vector<ChoiceOption> entries = choice->getEntries_mt_safe();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].id == optionID) {
            choice->setValue((int)i, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);

            return true;
        }
    }
    ADD_FAILURE() << name << " has no option " << optionID;

    return false;
}

bool
setBoolAsUser(const NodePtr& node,
              const std::string& name,
              bool value)
{
    KnobBool* knob = node ? dynamic_cast<KnobBool*>(node->getKnobByName(name).get()) : NULL;

    EXPECT_TRUE(knob != NULL) << name;
    if (!knob) {
        return false;
    }
    knob->setValue(value, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);

    return true;
}

RectD
regionOfDefinition(const NodePtr& node,
                   double time,
                   const RenderScale& scale = RenderScale::identity)
{
    RectD rod;
    EffectInstancePtr effect = node ? node->getEffectInstance() : EffectInstancePtr();

    EXPECT_TRUE(bool(effect));
    if (!effect) {
        return rod;
    }
    // Called directly rather than through the cached action: a knob edit does not always change
    // the render hash.
    const StatusEnum stat = effect->getRegionOfDefinition(effect->getRenderHash(), time, scale, ViewIdx(0), &rod);
    EXPECT_NE(eStatusFailed, stat) << node->getPluginID();

    return rod;
}

// A window wider than the source and the rectangles the cases use, so clipping and the black
// border are compared too.
RectI
caseWindow(unsigned mipmapLevel)
{
    return RectD(-12., -4., 76., 62.).toPixelEnclosing(mipmapLevel, 1.);
}

} // namespace

class NativeCropTest
    : public BaseTest {
protected:
    // A fresh project whose default format is 64x48 at the given pixel aspect ratio.
    void resetProject(double par = 1.)
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        const std::string name = "nativeCrop64x48par" + std::to_string(par);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, 64, 48, name, par));
    }

    ParityPair makePair()
    {
        ParityPair pair = makeParityPair(getApp(), kCropID, kNativeCropMajor);

        EXPECT_TRUE(bool(pair.native));
        EXPECT_TRUE(isNative(pair.native));
        if (pair.source) {
            setParitySourceOrigin(pair.source, 0, 0);
        }

        return pair;
    }

    bool setRectangle(const ParityPair& pair,
                      double x,
                      double y,
                      double w,
                      double h)
    {
        return setKnobOnBoth(pair, kNativeGeneratorParamBottomLeft, { x, y }) && setKnobOnBoth(pair, kNativeGeneratorParamSize, { w, h });
    }

    // The parity source at the origin, carrying mode: under Black, through a Crop to its own
    // window that forces Black, since the parity source itself carries Stretch.
    NodePtr createEdgeSource(EdgeMode mode)
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);

        EXPECT_TRUE(bool(source));
        if (!source) {
            return source;
        }
        setParitySourceComponents(source, "rgba");
        setParitySourceOrigin(source, 0, 0);
        if (mode == EdgeMode::Stretch) {
            return source;
        }
        const RectI window = paritySourceWindow(source, kTime, 0);

        return createCrop(source, RectD(window.x1, window.y1, window.x2, window.y2), "black");
    }

    // A hard Crop of input to rect at the given Edge Mode option.
    NodePtr createCrop(const NodePtr& input,
                       const RectD& rect,
                       const char* edgeModeID)
    {
        NodePtr crop = createNode(QString::fromUtf8(kCropID), kNativeCropMajor);

        EXPECT_TRUE(isNative(crop));
        if (!crop) {
            return crop;
        }
        connectNodes(input, crop, 0, true);
        const bool ok = setKnobValues(crop, kNativeGeneratorParamBottomLeft, { rect.x1, rect.y1 }) && setKnobValues(crop, kNativeGeneratorParamSize, { rect.width(), rect.height() }) && setKnobValues(crop, kCropParamSoftness, { 0. }) && setKnobValue(crop, kEdgeModeParam, std::string(edgeModeID));
        EXPECT_TRUE(ok) << edgeModeID;

        return ok ? crop : NodePtr();
    }

    void expectCropLargerThanTheSource(EdgeMode sourceMode, EdgeModeChoice choice, const char* choiceID);

    void expectParity(const ParityPair& pair,
                      const std::string& caseName,
                      const ParityTolerance& tolerance,
                      bool record)
    {
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const ParityResult r = compareParity(pair, caseName, caseWindow(mipmapLevel), mipmapLevel, tolerance, record);
            EXPECT_TRUE(r.ok) << caseName << ", mipmap " << mipmapLevel << ": " << describe(r);
            EXPECT_GE(r.planesCompared, 1) << caseName;
            std::cout << "[ parity ] Crop " << caseName << " mipmap " << mipmapLevel << ": planes " << r.planesCompared
                      << ", max abs diff " << r.maxAbsDiff << std::endl;
        }
    }
};

TEST_F(NativeCropTest, SizeExtentIsTheDefault)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(kCropID), kNativeCropMajor);
    ASSERT_TRUE(isNative(node));
    KnobChoice* extent = dynamic_cast<KnobChoice*>(node->getKnobByName(kNativeGeneratorParamExtent).get());
    ASSERT_TRUE(extent != NULL);
    EXPECT_EQ((int)Crop::eExtentSize, extent->getValue());
    EXPECT_FALSE(node->getKnobByName(kNativeGeneratorParamSize)->getIsSecret());
    EXPECT_TRUE(node->getKnobByName(kNatronParamFormatChoice)->getIsSecret());
}

TEST_F(NativeCropTest, SizeExtentWithoutSoftness)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setRectangle(pair, 10., 8., 40., 30.));
    expectParity(pair, "size", kExactTolerance, true);
}

TEST_F(NativeCropTest, SoftEdges)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setRectangle(pair, 6., 5., 50., 38.));
    ASSERT_TRUE(setKnobOnBoth(pair, kCropParamSoftness, { 20. }));
    expectParity(pair, "softness-20", kSoftTolerance, true);
}

TEST_F(NativeCropTest, ForcedBlack)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setRectangle(pair, 10., 8., 40., 30.));
    ASSERT_TRUE(setKnobOnBoth(pair, kEdgeModeParam, std::string("black")));
    expectParity(pair, "black-outside", kExactTolerance, true);
}

namespace {

bool
renderRGBA(const NodePtr& node,
           const RectI& window,
           RenderedPlane* plane)
{
    std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, &error);

    EXPECT_TRUE(ok) << error;
    if (!ok || (planes.size() != 1) || (planes[0].channels.size() != 4)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

EdgeMode
outputEdgeMode(const NodePtr& node)
{
    return node->getEffectInstance()->getTimeInvariantMetadata()->getEdgeMode();
}

const char*
edgeModeName(EdgeMode mode)
{
    return (mode == EdgeMode::Stretch) ? "Stretch" : "Black";
}

// Inside kept the plane holds the source's pixels; outside it, the nearest pixel of kept under
// Stretch and zero under Black.
void
expectEdge(const RenderedPlane& plane,
           const RenderedPlane& source,
           const RectI& kept,
           EdgeMode mode,
           const std::string& what)
{
    const RectI& w = plane.window;
    const RectI& s = source.window;
    int mismatches = 0;
    int outside = 0;
    std::string first;

    for (int y = w.y1; y < w.y2; ++y) {
        for (int x = w.x1; x < w.x2; ++x) {
            const bool inside = kept.contains(x, y);
            const int sx = std::min(std::max(x, kept.x1), kept.x2 - 1);
            const int sy = std::min(std::max(y, kept.y1), kept.y2 - 1);
            outside += inside ? 0 : 1;
            for (int c = 0; c < 4; ++c) {
                const float got = plane.pixels[((std::size_t)(y - w.y1) * w.width() + (x - w.x1)) * 4 + c];
                const float want = (inside || (mode == EdgeMode::Stretch)) ? source.pixels[((std::size_t)(sy - s.y1) * s.width() + (sx - s.x1)) * 4 + c] : 0.f;
                if (got != want) {
                    if (mismatches == 0) {
                        first = std::to_string(x) + "," + std::to_string(y) + " channel " + std::to_string(c) + ": " + std::to_string(got) + ", want " + std::to_string(want);
                    }
                    ++mismatches;
                }
            }
        }
    }
    EXPECT_EQ(0, mismatches) << what << " under " << edgeModeName(mode) << ", first at " << first;
    EXPECT_GT(outside, 0) << what;
}

} // namespace

// Over a source that carries mode, a Crop at choice larger than the source fills the band
// between them as the resolved mode says and carries that mode on.
void
NativeCropTest::expectCropLargerThanTheSource(EdgeMode sourceMode,
                                              EdgeModeChoice choice,
                                              const char* choiceID)
{
    resetProject();
    NodePtr source = createEdgeSource(sourceMode);
    ASSERT_TRUE(bool(source));
    NodePtr crop = createCrop(source, RectD(-10., -8., 80., 62.), choiceID);
    ASSERT_TRUE(bool(crop));

    const EdgeMode mode = resolveEdgeMode(choice, sourceMode);
    const std::string what = std::string(choiceID) + " over a " + edgeModeName(sourceMode) + " source";
    EXPECT_EQ(sourceMode, outputEdgeMode(source)) << what;
    EXPECT_EQ(mode, outputEdgeMode(crop)) << what;

    const RectI sourceWindow = paritySourceWindow(source, kTime, 0);
    RenderedPlane sourcePlane;
    RenderedPlane cropPlane;
    ASSERT_TRUE(renderRGBA(source, sourceWindow, &sourcePlane)) << what;
    ASSERT_TRUE(renderRGBA(crop, RectI(-10, -8, 80, 62), &cropPlane)) << what;
    expectEdge(cropPlane, sourcePlane, sourceWindow, mode, what);
}

TEST_F(NativeCropTest, LargerThanAStretchSourceStretchesUnderInput)
{
    expectCropLargerThanTheSource(EdgeMode::Stretch, EdgeModeChoice::Input, "input");
}

TEST_F(NativeCropTest, LargerThanABlackSourceIsBlackUnderInput)
{
    expectCropLargerThanTheSource(EdgeMode::Black, EdgeModeChoice::Input, "input");
}

TEST_F(NativeCropTest, ForcedStretchOverABlackSourceStretches)
{
    expectCropLargerThanTheSource(EdgeMode::Black, EdgeModeChoice::Stretch, "stretch");
}

TEST_F(NativeCropTest, ForcedBlackOverAStretchSourceIsBlack)
{
    expectCropLargerThanTheSource(EdgeMode::Stretch, EdgeModeChoice::Black, "black");
}

// A Crop smaller than its source carries the resolved mode, so what reads past the crop
// rectangle sees black under Black and the rectangle's own edge under Stretch, never the source
// pixels the crop removed.
TEST_F(NativeCropTest, SmallerThanTheSourceForEachChoice)
{
    const struct {
        EdgeModeChoice choice;
        const char* id;
    } choices[] = { { EdgeModeChoice::Input, "input" }, { EdgeModeChoice::Black, "black" }, { EdgeModeChoice::Stretch, "stretch" } };

    for (EdgeMode sourceMode : { EdgeMode::Stretch, EdgeMode::Black }) {
        for (const auto& choice : choices) {
            resetProject();
            NodePtr source = createEdgeSource(sourceMode);
            ASSERT_TRUE(bool(source));
            NodePtr crop = createCrop(source, RectD(10., 8., 50., 38.), choice.id);
            ASSERT_TRUE(bool(crop));
            NodePtr reader = createCrop(crop, RectD(-10., -8., 80., 62.), "input");
            ASSERT_TRUE(bool(reader));

            const EdgeMode mode = resolveEdgeMode(choice.choice, sourceMode);
            const std::string what = std::string(choice.id) + " over a " + edgeModeName(sourceMode) + " source";
            EXPECT_EQ(mode, outputEdgeMode(crop)) << what;
            EXPECT_EQ(mode, outputEdgeMode(reader)) << what;
            EXPECT_EQ(RectD(10., 8., 50., 38.), regionOfDefinition(crop, kTime)) << what;

            const RectI sourceWindow = paritySourceWindow(source, kTime, 0);
            RenderedPlane sourcePlane;
            RenderedPlane readerPlane;
            ASSERT_TRUE(renderRGBA(source, sourceWindow, &sourcePlane)) << what;
            ASSERT_TRUE(renderRGBA(reader, RectI(-10, -8, 80, 62), &readerPlane)) << what;
            expectEdge(readerPlane, sourcePlane, RectI(10, 8, 50, 38), mode, what);
        }
    }
}

// A pixel centre exactly on a soft edge weighs 0 and is multiplied by it rather than written 0,
// so a negative source becomes -0 and an infinite one NaN.
TEST_F(NativeCropTest, ZeroSoftEdgeWeightMultipliesTheSource)
{
    resetProject();
    const double inf = std::numeric_limits<double>::infinity();
    NodePtr source = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    NodePtr crop = createNode(QString::fromUtf8(kCropID), kNativeCropMajor);
    ASSERT_TRUE(bool(source));
    ASSERT_TRUE(isNative(crop));
    ASSERT_TRUE(setKnobValues(source, kConstantParamColor, { -1., inf, 0.5, 1. }));
    connectNodes(source, crop, 0, true);
    ASSERT_TRUE(setKnobValues(crop, kNativeGeneratorParamBottomLeft, { 10.5, 8.5 }));
    ASSERT_TRUE(setKnobValues(crop, kNativeGeneratorParamSize, { 40., 30. }));
    ASSERT_TRUE(setKnobValues(crop, kCropParamSoftness, { 6. }));

    const RectI window = regionOfDefinition(crop, kTime).toPixelEnclosing(0, 1.);
    ASSERT_TRUE(window.contains(10, 20));
    ASSERT_TRUE(window.contains(25, 20));
    std::list<ImageLayerDesc> layers;
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> planes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(crop, kTime, ViewIdx(0), 0, window, layers, &planes, &error)) << error;
    ASSERT_EQ(1u, planes.size());
    ASSERT_EQ(4u, planes[0].channels.size());
    const auto at = [&](int x, int y, int c) { return planes[0].pixels[((std::size_t)(y - window.y1) * window.width() + (x - window.x1)) * 4 + c]; };

    EXPECT_EQ(-1.f, at(25, 20, 0));
    EXPECT_TRUE(std::isinf(at(25, 20, 1)));
    for (int y = 9; y < 38; ++y) {
        EXPECT_EQ(0.f, at(10, y, 0)) << "row " << y;
        EXPECT_TRUE(std::signbit(at(10, y, 0))) << "row " << y;
        EXPECT_TRUE(std::isnan(at(10, y, 1))) << "row " << y;
        EXPECT_EQ(0.f, at(10, y, 2)) << "row " << y;
        EXPECT_FALSE(std::signbit(at(10, y, 2))) << "row " << y;
    }
}

TEST_F(NativeCropTest, ReformatOnFormatSetsTheOutputFormat)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(kCropID), kNativeCropMajor);
    ASSERT_TRUE(isNative(node));
    ASSERT_TRUE(setChoiceAsUser(node, kNativeGeneratorParamExtent, kNativeGeneratorExtentFormat));
    ASSERT_TRUE(setKnobValues(node, kNatronParamFormatSize, { 50., 40. }));
    ASSERT_TRUE(setKnobValues(node, kNatronParamFormatPar, { 2. }));
    ASSERT_TRUE(setKnobValues(node, kCropParamReformat, { 1. }));

    EffectInstancePtr effect = node->getEffectInstance();
    EXPECT_EQ(2., effect->getTimeInvariantMetadata()->getPixelAspectRatio(-1));
    const RectI format = effect->getTimeInvariantMetadata()->getOutputFormat();
    EXPECT_EQ(0, format.x1);
    EXPECT_EQ(0, format.y1);
    EXPECT_EQ(50, format.x2);
    EXPECT_EQ(40, format.y2);
}

TEST_F(NativeCropTest, UserReformatEditTogglesTheOverlay)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(kCropID), kNativeCropMajor);
    ASSERT_TRUE(isNative(node));

    std::list<HostOverlayKnobsPtr> pending = node->getPendingHostOverlays();
    ASSERT_EQ(1u, pending.size());
    HostOverlayKnobsRectanglePtr overlay = std::dynamic_pointer_cast<HostOverlayKnobsRectangle>(pending.front());
    ASSERT_TRUE(bool(overlay));
    EXPECT_TRUE(overlay->checkHostOverlayValid());
    EXPECT_EQ(node->getKnobByName(kNativeGeneratorParamBottomLeft), overlay->getKnob(HostOverlayKnobsRectangle::eKnobsEnumerationBottomLeft));
    EXPECT_EQ(node->getKnobByName(kNativeGeneratorParamSize), overlay->getKnob(HostOverlayKnobsRectangle::eKnobsEnumerationSize));

    KnobBoolPtr enable = std::dynamic_pointer_cast<KnobBool>(overlay->getKnob(HostOverlayKnobsRectangle::eKnobsEnumerationEnable));
    ASSERT_TRUE(bool(enable));
    EXPECT_EQ(node->getKnobByName(kNativeGeneratorParamRectangleEnable), KnobIPtr(enable));
    EXPECT_TRUE(enable->getIsSecret());
    EXPECT_TRUE(enable->getValue());

    KnobChoice* edgeMode = dynamic_cast<KnobChoice*>(node->getKnobByName(kEdgeModeParam).get());
    ASSERT_TRUE(edgeMode != NULL);
    EXPECT_EQ((int)EdgeModeChoice::Input, edgeMode->getValue());

    ASSERT_TRUE(setBoolAsUser(node, kCropParamReformat, true));
    EXPECT_FALSE(enable->getValue());
    EXPECT_EQ((int)EdgeModeChoice::Input, edgeMode->getValue());

    ASSERT_TRUE(setBoolAsUser(node, kCropParamReformat, false));
    EXPECT_TRUE(enable->getValue());
    EXPECT_EQ((int)EdgeModeChoice::Input, edgeMode->getValue());
}

TEST_F(NativeCropTest, RecenterCentresTheRectangleOnTheSource)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    NodePtr node = createNode(QString::fromUtf8(kCropID), kNativeCropMajor);
    ASSERT_TRUE(bool(source));
    ASSERT_TRUE(isNative(node));
    setParitySourceOrigin(source, 20, 10);
    connectNodes(source, node, 0, true);
    ASSERT_TRUE(setKnobValues(node, kNativeGeneratorParamSize, { 20., 10. }));

    KnobButton* recenter = dynamic_cast<KnobButton*>(node->getKnobByName(kNativeGeneratorParamRecenter).get());
    ASSERT_TRUE(recenter != NULL);
    recenter->trigger();

    KnobDouble* bottomLeft = dynamic_cast<KnobDouble*>(node->getKnobByName(kNativeGeneratorParamBottomLeft).get());
    ASSERT_TRUE(bottomLeft != NULL);
    EXPECT_EQ(20. + 32. - 10., bottomLeft->getValue(0));
    EXPECT_EQ(10. + 24. - 5., bottomLeft->getValue(1));
}

TEST_F(NativeCropTest, UnversionedRequestsGetTheNativeCrop)
{
    NodePtr unversioned = createNode(QString::fromUtf8(kCropID));
    ASSERT_TRUE(bool(unversioned));
    EXPECT_TRUE(isNative(unversioned));
    EXPECT_EQ(kNativeCropMajor, unversioned->getMajorVersion());
}

TEST_F(NativeCropTest, RendersTheSameInBothSchedulerModes)
{
    resetProject();
    std::vector<int> poolSizes;
    poolSizes.push_back(1);
    poolSizes.push_back(4);

    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    NodePtr crop = createNode(QString::fromUtf8(kCropID), kNativeCropMajor);
    ASSERT_TRUE(bool(source));
    ASSERT_TRUE(isNative(crop));
    connectNodes(source, crop, 0, true);
    ASSERT_TRUE(setKnobValues(crop, kNativeGeneratorParamBottomLeft, { 10., 8. }));
    ASSERT_TRUE(setKnobValues(crop, kNativeGeneratorParamSize, { 40., 30. }));
    ASSERT_TRUE(setKnobValues(crop, kCropParamSoftness, { 7. }));
    ASSERT_TRUE(setKnobValue(crop, kEdgeModeParam, std::string("black")));

    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        std::vector<int> unplannedPulls;
        const RectI window = regionOfDefinition(crop, kTime, RenderScale::fromMipmapLevel(mipmapLevel)).toPixelEnclosing(mipmapLevel, 1.);
        const RenderMismatch m = renderBothWaysDirect(crop, kTime, ViewIdx(0), mipmapLevel, window, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
        EXPECT_FALSE(m.any) << "mipmap " << mipmapLevel << ": " << describe(m);
        for (std::size_t p = 0; p < unplannedPulls.size(); ++p) {
            EXPECT_EQ(0, unplannedPulls[p]) << "mipmap " << mipmapLevel << ", pool " << poolSizes[p];
        }
    }
}

TEST_F(NativeCropTest, ReformatOnSizeMakesTheRectangleTheOutputFormat)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    NodePtr crop = createNode(QString::fromUtf8(kCropID), kNativeCropMajor);
    NodePtr downstream = createNode(QString::fromUtf8(PLUGINID_NATRON_INVERT));
    ASSERT_TRUE(bool(source));
    ASSERT_TRUE(isNative(crop));
    ASSERT_TRUE(isNative(downstream));
    setParitySourceOrigin(source, 0, 0);
    connectNodes(source, crop, 0, true);
    connectNodes(crop, downstream, 0, true);
    ASSERT_TRUE(setKnobValues(crop, kNativeGeneratorParamBottomLeft, { 10., 8. }));
    ASSERT_TRUE(setKnobValues(crop, kNativeGeneratorParamSize, { 40., 30. }));

    const std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> inPlace;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(crop, kTime, ViewIdx(0), 0, RectI(10, 8, 50, 38), layers, &inPlace, &error)) << error;

    ASSERT_TRUE(setBoolAsUser(crop, kCropParamReformat, true));
    EffectInstancePtr effect = crop->getEffectInstance();
    const RectI format = effect->getTimeInvariantMetadata()->getOutputFormat();
    EXPECT_EQ(RectI(0, 0, 40, 30), format);
    EXPECT_EQ(1., effect->getTimeInvariantMetadata()->getPixelAspectRatio(-1));
    EXPECT_EQ(RectD(0., 0., 40., 30.), regionOfDefinition(crop, kTime));
    EXPECT_EQ(RectI(0, 0, 40, 30), downstream->getEffectInstance()->getTimeInvariantMetadata()->getOutputFormat());

    std::vector<RenderedPlane> moved;
    ASSERT_TRUE(renderNodePlanesDirect(crop, kTime, ViewIdx(0), 0, RectI(0, 0, 40, 30), layers, &moved, &error)) << error;
    ASSERT_EQ(1u, inPlace.size());
    ASSERT_EQ(1u, moved.size());
    EXPECT_EQ(inPlace[0].pixels, moved[0].pixels);

    ASSERT_TRUE(setKnobValues(crop, kNativeGeneratorParamSize, { 24., 12. }));
    EXPECT_EQ(RectI(0, 0, 24, 12), effect->getTimeInvariantMetadata()->getOutputFormat());

    ASSERT_TRUE(setBoolAsUser(crop, kCropParamReformat, false));
    EXPECT_EQ(RectI(0, 0, 64, 48), effect->getTimeInvariantMetadata()->getOutputFormat());
}

TEST_F(NativeCropTest, ReformatOnDefaultMakesTheSourceExtentTheOutputFormat)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    NodePtr crop = createNode(QString::fromUtf8(kCropID), kNativeCropMajor);
    ASSERT_TRUE(bool(source));
    ASSERT_TRUE(isNative(crop));
    setParitySourceOrigin(source, 20, 10);
    connectNodes(source, crop, 0, true);
    ASSERT_TRUE(setChoiceAsUser(crop, kNativeGeneratorParamExtent, kNativeGeneratorExtentDefault));
    ASSERT_TRUE(setBoolAsUser(crop, kCropParamReformat, true));

    const RectD sourceRoD = regionOfDefinition(source, kTime);
    const RectI expected(0, 0, (int)(sourceRoD.x2 - sourceRoD.x1), (int)(sourceRoD.y2 - sourceRoD.y1));
    EXPECT_EQ(expected, crop->getEffectInstance()->getTimeInvariantMetadata()->getOutputFormat());
    EXPECT_EQ(RectD(0., 0., expected.x2, expected.y2), regionOfDefinition(crop, kTime));
}

TEST_F(NativeCropTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_CROP, PLUGIN_MAJOR_NATRON_CROP, "Crop");
}

// With reformat, a keyed rectangle gives each frame its own format, whichever frame the timeline
// is on.
TEST_F(NativeCropTest, AnAnimatedRectangleIsEachFramesFormat)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    NodePtr crop = createNode(QString::fromUtf8(kCropID), kNativeCropMajor);
    ASSERT_TRUE(bool(source));
    ASSERT_TRUE(isNative(crop));
    setParitySourceOrigin(source, 0, 0);
    connectNodes(source, crop, 0, true);
    ASSERT_TRUE(setKnobValues(crop, kCropParamReformat, { 1. }));
    ASSERT_TRUE(setKnobValues(crop, kNativeGeneratorParamBottomLeft, { 0., 0. }));
    KnobDouble* size = dynamic_cast<KnobDouble*>(crop->getKnobByName(kNativeGeneratorParamSize).get());
    ASSERT_TRUE(size != NULL);
    size->setValueAtTime(1, 40., ViewSpec::all(), 0);
    size->setValueAtTime(1, 30., ViewSpec::all(), 1);
    size->setValueAtTime(2, 20., ViewSpec::all(), 0);
    size->setValueAtTime(2, 16., ViewSpec::all(), 1);
    ASSERT_TRUE(size->isAnimated(0) && size->isAnimated(1));

    const SequenceTime timelineFrames[2] = { 2, 1 };
    for (int t = 0; t < 2; ++t) {
        getApp()->getTimeLine()->seekFrame(timelineFrames[t], false, NULL, eTimelineChangeReasonOtherSeek);
        NodeMetadataConstPtr first = crop->getEffectInstance()->getMetadata(1, ViewIdx(0));
        NodeMetadataConstPtr second = crop->getEffectInstance()->getMetadata(2, ViewIdx(0));
        ASSERT_TRUE(first && second);
        EXPECT_EQ(RectI(0, 0, 40, 30), first->getOutputFormat()) << "timeline at " << timelineFrames[t];
        EXPECT_EQ(RectI(0, 0, 20, 16), second->getOutputFormat()) << "timeline at " << timelineFrames[t];
    }
}
