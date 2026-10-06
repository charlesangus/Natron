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

#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/Image/NativeGenerator.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/OfxEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kCheckerBoardID = PLUGINID_NATRON_CHECKERBOARD;
const int kOfxCheckerBoardMajor = 1;
const int kNativeCheckerBoardMajor = PLUGIN_MAJOR_NATRON_CHECKERBOARD;
const double kTime = 1.;

// The output layout choices the layer select replaces on the native node: the host hides
// outputComponents and outputBitDepth is only shown in debug plug-in builds.
const std::vector<std::string> kIgnoredOfxKnobs = { "outputComponents", "outputBitDepth" };

// Every pixel is one of six knob colours chosen by comparisons, so the exact class.
const ParityTolerance kCheckerBoardTolerance = ParityTolerance::exact();

const char* const kExtents[4] = {
    kNativeGeneratorExtentFormat, kNativeGeneratorExtentSize, kNativeGeneratorExtentProject, kNativeGeneratorExtentDefault
};

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

bool
isOfx(const NodePtr& node)
{
    return node && dynamic_cast<OfxEffectInstance*>(node->getEffectInstance().get());
}

// Sets a choice by option ID as a user edit, which is what makes the OpenFX generator refresh
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
setExtentOnBoth(const ParityPair& pair,
                const std::string& extent)
{
    bool ok = setChoiceAsUser(pair.native, kNativeGeneratorParamExtent, extent);

    if (pair.ofx) {
        ok = setChoiceAsUser(pair.ofx, kNativeGeneratorParamExtent, extent) && ok;
    }

    return ok;
}

// The project window and a margin around it, so clipping to the region of definition is
// compared too.
RectI
caseWindow(unsigned mipmapLevel,
           double par)
{
    return RectD(-8. * par, -8., 72. * par, 56.).toPixelEnclosing(mipmapLevel, par);
}

} // namespace

class NativeCheckerBoardTest
    : public BaseTest {
protected:
    // A fresh project whose default format is 64x48 at the given pixel aspect ratio.
    void resetProject(double par = 1.)
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        // The format choice is keyed by name, so each pixel aspect ratio needs its own.
        project->setOrAddProjectFormat(Format(0, 0, 64, 48, "nativeCheckerBoardPar" + std::to_string(par), par));
    }

    // Both versions with nothing connected. The parity source is only there because
    // compareParity() requires one; every case passes its own window.
    ParityPair makePair()
    {
        ParityPair pair;

        pair.id = kCheckerBoardID;
        pair.ofxMajor = kOfxCheckerBoardMajor;
        pair.nativeMajor = kNativeCheckerBoardMajor;
        pair.app = getApp();
        pair.source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        if (isPluginMajorRegistered(kCheckerBoardID, kOfxCheckerBoardMajor)) {
            pair.ofx = createNodeAtMajor(getApp(), kCheckerBoardID, kOfxCheckerBoardMajor);
        }
        pair.native = createNodeAtMajor(getApp(), kCheckerBoardID, kNativeCheckerBoardMajor);
        EXPECT_TRUE(bool(pair.native));
        EXPECT_TRUE(pair.live()) << "the OFX CheckerBoard must be loadable at major " << kOfxCheckerBoardMajor;
        EXPECT_TRUE(isNative(pair.native));

        return pair;
    }

    // Compares the pair at mipmap 0, 1 and 2: the box size, the line widths and the centre all
    // scale with the render scale, and the centre lines stay at least a pixel wide.
    void expectParity(const ParityPair& pair,
                      const std::string& caseName,
                      bool record,
                      double par = 1.,
                      double time = kTime)
    {
        ParityOptions options;

        options.time = time;
        for (unsigned mipmapLevel = 0; mipmapLevel <= 2; ++mipmapLevel) {
            const ParityResult r = compareParity(pair, caseName, caseWindow(mipmapLevel, par), mipmapLevel, kCheckerBoardTolerance, record, options);
            EXPECT_TRUE(r.ok) << caseName << ", mipmap " << mipmapLevel << ": " << describe(r);
            EXPECT_GE(r.planesCompared, 1) << caseName;
            std::cout << "[ parity ] CheckerBoard " << caseName << " mipmap " << mipmapLevel << ": planes " << r.planesCompared
                      << ", max abs diff " << r.maxAbsDiff << std::endl;
        }
    }
};

TEST_F(NativeCheckerBoardTest, KnobsMatchTheOfxCheckerBoard)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(pair.live());

    expectKnobParity(pair.ofx, pair.native, kIgnoredOfxKnobs);
    for (int e = 0; e < 4; ++e) {
        ASSERT_TRUE(setExtentOnBoth(pair, kExtents[e]));
        SCOPED_TRACE(kExtents[e]);
        expectKnobParity(pair.ofx, pair.native, kIgnoredOfxKnobs);
    }
}

TEST_F(NativeCheckerBoardTest, Default)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    expectParity(pair, "default", true);
}

TEST_F(NativeCheckerBoardTest, SmallBoxesWithoutLines)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    ASSERT_TRUE(setKnobOnBoth(pair, kCheckerBoardParamBoxSize, { 10., 7. }));
    ASSERT_TRUE(setKnobOnBoth(pair, kCheckerBoardParamColor0, { 0.9, -0.2, 0.3, 1. }));
    ASSERT_TRUE(setKnobOnBoth(pair, kCheckerBoardParamColor2, { 0.2, 1.5, 0.4, 0.5 }));
    ASSERT_TRUE(setKnobOnBoth(pair, kCheckerBoardParamLineWidth, { 0. }));
    expectParity(pair, "boxes-line0", false);
}

TEST_F(NativeCheckerBoardTest, SmallBoxesWithLines)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    ASSERT_TRUE(setKnobOnBoth(pair, kCheckerBoardParamBoxSize, { 10., 7. }));
    ASSERT_TRUE(setKnobOnBoth(pair, kCheckerBoardParamLineWidth, { 2. }));
    ASSERT_TRUE(setKnobOnBoth(pair, kCheckerBoardParamCenterLineWidth, { 3. }));
    ASSERT_TRUE(setKnobOnBoth(pair, kCheckerBoardParamLineColor, { 0.25, 0.5, 0.75, 1. }));
    expectParity(pair, "boxes-line2", true);
}

TEST_F(NativeCheckerBoardTest, PixelAspectRatioTwoProject)
{
    resetProject(2.);
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    ASSERT_TRUE(setKnobOnBoth(pair, kCheckerBoardParamBoxSize, { 12., 9. }));
    ASSERT_TRUE(setKnobOnBoth(pair, kCheckerBoardParamLineWidth, { 2. }));
    expectParity(pair, "par2", true, 2.);
}

TEST_F(NativeCheckerBoardTest, SizeExtentCentresOnItsRectangle)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    ASSERT_TRUE(setExtentOnBoth(pair, kNativeGeneratorExtentSize));
    ASSERT_TRUE(setKnobOnBoth(pair, kNativeGeneratorParamBottomLeft, { 9., 5. }));
    ASSERT_TRUE(setKnobOnBoth(pair, kNativeGeneratorParamSize, { 43., 31. }));
    ASSERT_TRUE(setKnobOnBoth(pair, kCheckerBoardParamBoxSize, { 8., 8. }));
    ASSERT_TRUE(setKnobOnBoth(pair, kCheckerBoardParamLineWidth, { 1. }));
    expectParity(pair, "size-offset", false);
}

TEST_F(NativeCheckerBoardTest, AnimatedColor0)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    ASSERT_TRUE(setKnobOnBoth(pair, kNativeGeneratorParamFrameRange, { 1., 10. }));
    ASSERT_TRUE(setKnobOnBoth(pair, kCheckerBoardParamBoxSize, { 10., 7. }));

    const NodePtr nodes[2] = { pair.ofx, pair.native };
    for (int i = 0; i < 2; ++i) {
        KnobColor* color0 = dynamic_cast<KnobColor*>(nodes[i]->getKnobByName(kCheckerBoardParamColor0).get());
        ASSERT_TRUE(color0 != NULL);
        for (int d = 0; d < 4; ++d) {
            color0->setValueAtTime(1., 0.1 * (d + 1), ViewSpec::all(), d);
            color0->setValueAtTime(10., 1. - 0.1 * d, ViewSpec::all(), d);
        }
        nodes[i]->getEffectInstance()->refreshMetadata_public(false);
        EXPECT_TRUE(nodes[i]->getEffectInstance()->isFrameVarying()) << (isOfx(nodes[i]) ? "ofx" : "native");
    }
    expectParity(pair, "animated-color0", false, 1., 5.);
}

TEST_F(NativeCheckerBoardTest, UnversionedRequestsGetTheNativeCheckerBoard)
{
    NodePtr unversioned = createNode(QString::fromUtf8(kCheckerBoardID));
    ASSERT_TRUE(bool(unversioned));
    EXPECT_TRUE(isNative(unversioned));
    EXPECT_EQ(kNativeCheckerBoardMajor, unversioned->getMajorVersion());

    NodePtr ofx = createNode(QString::fromUtf8(kCheckerBoardID), kOfxCheckerBoardMajor);
    ASSERT_TRUE(bool(ofx));
    EXPECT_TRUE(isOfx(ofx));
}

TEST_F(NativeCheckerBoardTest, RendersTheSameInBothSchedulerModes)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(kCheckerBoardID), kNativeCheckerBoardMajor);
    ASSERT_TRUE(isNative(node));
    ASSERT_TRUE(setKnobValues(node, kCheckerBoardParamBoxSize, { 10., 7. }));
    ASSERT_TRUE(setKnobValues(node, kCheckerBoardParamLineWidth, { 2. }));

    std::vector<int> poolSizes;
    poolSizes.push_back(1);
    poolSizes.push_back(4);
    for (unsigned mipmapLevel = 0; mipmapLevel <= 2; ++mipmapLevel) {
        std::vector<int> unplannedPulls;
        const RectI window = RectD(0., 0., 64., 48.).toPixelEnclosing(mipmapLevel, 1.);
        const RenderMismatch m = renderBothWaysDirect(node, kTime, ViewIdx(0), mipmapLevel, window, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
        EXPECT_FALSE(m.any) << "mipmap " << mipmapLevel << ": " << describe(m);
        for (std::size_t p = 0; p < unplannedPulls.size(); ++p) {
            EXPECT_EQ(0, unplannedPulls[p]) << "mipmap " << mipmapLevel << ", pool " << poolSizes[p];
        }
    }
}
