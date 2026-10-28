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
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <OpenColorIO/OpenColorIO.h>

#include "BaseTest.h"
#include "NativeParity.h"
#include "OcioNodeTestSupport.h"

#include "Engine/AppInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/OCIO/OCIODisplay.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING
using namespace OcioNodeTest;

namespace {

// The display processors differ from the node's only in how OpenColorIO is asked for them.
const double kDisplayTolerance = 1e-4;

// The viewer's gamma, written out here so the expected values do not come from the node.
void
applyViewerGamma(float rgb[3],
                 double gamma)
{
    for (int c = 0; c < 3; ++c) {
        if (gamma <= 0.) {
            rgb[c] = (rgb[c] >= 1.f) ? 1.f : 0.f;
        } else if (gamma != 1.) {
            rgb[c] = std::pow(std::max(rgb[c], 0.f), (float)(1. / gamma));
        }
    }
}

} // namespace

class NativeOCIODisplayTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        setConfig(getApp(), kStudioURI);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    ParityPair makePair()
    {
        ParityPair pair = makeParityPair(getApp(), PLUGINID_NATRON_OCIODISPLAY, PLUGIN_MAJOR_NATRON_OCIODISPLAY);

        EXPECT_TRUE(bool(pair.native));
        EXPECT_TRUE(pair.native && dynamic_cast<OCIODisplay*>(pair.native->getEffectInstance().get()));

        return pair;
    }

    ProjectColorManagementPtr colorManagement() const
    {
        return getApp()->getProject()->getColorManagement();
    }

    // The viewer's processor for the working space on the config's default display and view.
    OCIO_NAMESPACE::ConstCPUProcessorRcPtr defaultDisplayProcessor() const
    {
        ProjectColorManagementPtr cm = colorManagement();
        const std::string display = cm->getDefaultDisplay();
        std::string error;
        ProjectColorManagement::DisplayProcessorPtr processor = cm->getDisplayProcessor(getApp()->getProject()->getWorkingColorSpace(), display, cm->getDefaultView(display), std::string(), &error);

        EXPECT_TRUE(processor != nullptr) << error;

        return processor ? processor->cpu : OCIO_NAMESPACE::ConstCPUProcessorRcPtr();
    }
};

TEST_F(NativeOCIODisplayTest, DefaultsToTheConfigsDefaultDisplayAndView)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));

    ProjectColorManagementPtr cm = colorManagement();
    const std::string display = cm->getDefaultDisplay();
    ASSERT_FALSE(display.empty());
    KnobChoicePtr displayKnob = std::dynamic_pointer_cast<KnobChoice>(pair.native->getKnobByName(kOCIODisplayParamDisplay));
    KnobChoicePtr viewKnob = std::dynamic_pointer_cast<KnobChoice>(pair.native->getKnobByName(kOCIODisplayParamView));
    ASSERT_TRUE(bool(displayKnob));
    ASSERT_TRUE(bool(viewKnob));
    EXPECT_EQ(display, displayKnob->getActiveEntry().id);
    EXPECT_EQ(cm->getDefaultView(display), viewKnob->getActiveEntry().id);
    EXPECT_EQ(cm->getDisplays().size(), (std::size_t)displayKnob->getNumEntries());
    EXPECT_EQ(cm->getViews(display).size(), (std::size_t)viewKnob->getNumEntries());

    OCIO_NAMESPACE::ConstCPUProcessorRcPtr cpu = defaultDisplayProcessor();
    ASSERT_TRUE(bool(cpu));
    expectRender(pair.native, pair.source, [&cpu](float rgb[3], float /*alpha*/) { cpu->applyRGB(rgb); }, "default display", kDisplayTolerance);
}

TEST_F(NativeOCIODisplayTest, GainBeforeAndGammaAfterAsTheViewer)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    OCIO_NAMESPACE::ConstCPUProcessorRcPtr cpu = defaultDisplayProcessor();
    ASSERT_TRUE(bool(cpu));

    const double gains[3] = { 2.5, 0.25, 1. };
    const double gammas[3] = { 2.2, 0.5, 0. };
    for (int i = 0; i < 3; ++i) {
        const float gain = (float)gains[i];
        const double gamma = gammas[i];
        ASSERT_TRUE(setKnobValues(pair.native, kOCIODisplayParamGain, std::vector<double>(1, gains[i])));
        ASSERT_TRUE(setKnobValues(pair.native, kOCIODisplayParamGamma, std::vector<double>(1, gamma)));
        expectRender(pair.native, pair.source, [&cpu, gain, gamma](float rgb[3], float /*alpha*/) {
            for (int c = 0; c < 3; ++c) {
                rgb[c] *= gain;
            }
            cpu->applyRGB(rgb);
            applyViewerGamma(rgb, gamma); }, "gain " + std::to_string(gains[i]) + ", gamma " + std::to_string(gamma), kDisplayTolerance);
    }
}

TEST_F(NativeOCIODisplayTest, ChannelViewComesBeforeTheDisplayTransform)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    OCIO_NAMESPACE::ConstCPUProcessorRcPtr cpu = defaultDisplayProcessor();
    ASSERT_TRUE(bool(cpu));
    double weights[3];
    colorManagement()->getWorkingLuma(weights);

    ASSERT_TRUE(setKnobValue(pair.native, kOCIODisplayParamChannel, "g"));
    expectRender(pair.native, pair.source, [&cpu](float rgb[3], float /*alpha*/) {
        rgb[0] = rgb[2] = rgb[1];
        cpu->applyRGB(rgb); }, "green", kDisplayTolerance);

    ASSERT_TRUE(setKnobValue(pair.native, kOCIODisplayParamChannel, "a"));
    expectRender(pair.native, pair.source, [&cpu](float rgb[3], float alpha) {
        rgb[0] = rgb[1] = rgb[2] = alpha;
        cpu->applyRGB(rgb); }, "alpha", kDisplayTolerance);

    ASSERT_TRUE(setKnobValue(pair.native, kOCIODisplayParamChannel, "luminance"));
    expectRender(pair.native, pair.source, [&cpu, &weights](float rgb[3], float /*alpha*/) {
        const float l = (float)weights[0] * rgb[0] + (float)weights[1] * rgb[1] + (float)weights[2] * rgb[2];
        rgb[0] = rgb[1] = rgb[2] = l;
        cpu->applyRGB(rgb); }, "luminance", kDisplayTolerance);
}

TEST_F(NativeOCIODisplayTest, ChangingTheDisplayTakesItsDefaultView)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));

    ProjectColorManagementPtr cm = colorManagement();
    const std::vector<std::string> displays = cm->getDisplays();
    ASSERT_GE(displays.size(), 2u);
    const std::string other = (displays[0] == cm->getDefaultDisplay()) ? displays[1] : displays[0];

    ASSERT_TRUE(setKnobValue(pair.native, kOCIODisplayParamDisplay, other));
    KnobChoicePtr viewKnob = std::dynamic_pointer_cast<KnobChoice>(pair.native->getKnobByName(kOCIODisplayParamView));
    ASSERT_TRUE(bool(viewKnob));
    const std::vector<std::string> views = cm->getViews(other);
    EXPECT_EQ(views.size(), (std::size_t)viewKnob->getNumEntries());
    EXPECT_TRUE(std::find(views.begin(), views.end(), viewKnob->getActiveEntry().id) != views.end()) << viewKnob->getActiveEntry().id;
}

namespace {
// Both configs share their default display; the ACES 1.3 one has none of the ACES 2.0 views and fewer displays.
const char* const kCG13URI = "ocio://cg-config-v1.0.0_aces-v1.3_ocio-v2.1";
const char* const kDefaultDisplay = "sRGB - Display";
const char* const kStudioOnlyDisplay = "Display P3 - Display";
const char* const kSharedView = "Un-tone-mapped";
const char* const kCG13DefaultView = "ACES 1.0 - SDR Video";

KnobChoicePtr
choice(const NodePtr& node,
       const char* name)
{
    return std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(name));
}

std::string
activeID(const NodePtr& node,
         const char* name)
{
    KnobChoicePtr knob = choice(node, name);

    return knob ? knob->getActiveEntry().id : std::string("<missing>");
}

std::string
defaultID(const NodePtr& node,
          const char* name)
{
    KnobChoicePtr knob = choice(node, name);
    if (!knob) {
        return std::string("<missing>");
    }
    const std::vector<ChoiceOption> entries = knob->getEntries_mt_safe();
    const int index = knob->getDefaultValue(0);

    return ((index >= 0) && (index < (int)entries.size())) ? entries[index].id : std::string("<out of range>");
}
} // namespace

// A display or view the new config lacks, or one still at the old config's default, moves to the new config's
// default without an error; a view the user picked that the new config also has stays.
TEST_F(NativeOCIODisplayTest, SwitchingConfigMovesTheDisplayAndViewToTheNewDefaults)
{
    ProjectColorManagementPtr cm = colorManagement();
    ASSERT_EQ(kDefaultDisplay, cm->getDefaultDisplay());
    const std::string studioDefaultView = cm->getDefaultView(kDefaultDisplay);

    ParityPair atDefaults = makePair();
    ParityPair studioOnly = makePair();
    ParityPair pickedView = makePair();
    ASSERT_TRUE(atDefaults.native && studioOnly.native && pickedView.native);
    ASSERT_EQ(studioDefaultView, activeID(atDefaults.native, kOCIODisplayParamView));
    ASSERT_TRUE(setKnobValue(studioOnly.native, kOCIODisplayParamDisplay, kStudioOnlyDisplay));
    ASSERT_TRUE(setKnobValue(pickedView.native, kOCIODisplayParamView, kSharedView));

    setConfig(getApp(), kCG13URI);
    ASSERT_EQ(kDefaultDisplay, cm->getDefaultDisplay());
    ASSERT_EQ(kCG13DefaultView, cm->getDefaultView(kDefaultDisplay));
    const std::vector<std::string> displays = cm->getDisplays();
    ASSERT_TRUE(std::find(displays.begin(), displays.end(), kStudioOnlyDisplay) == displays.end());

    for (const NodePtr& node : { atDefaults.native, studioOnly.native }) {
        EXPECT_EQ(kDefaultDisplay, activeID(node, kOCIODisplayParamDisplay));
        EXPECT_EQ(kCG13DefaultView, activeID(node, kOCIODisplayParamView));
    }
    EXPECT_EQ(kDefaultDisplay, activeID(pickedView.native, kOCIODisplayParamDisplay));
    EXPECT_EQ(kSharedView, activeID(pickedView.native, kOCIODisplayParamView));
    for (const NodePtr& node : { atDefaults.native, studioOnly.native, pickedView.native }) {
        EXPECT_EQ(kDefaultDisplay, defaultID(node, kOCIODisplayParamDisplay));
        EXPECT_EQ(kCG13DefaultView, defaultID(node, kOCIODisplayParamView));
        EXPECT_TRUE(persistentMessage(node).isEmpty()) << persistentMessage(node).toStdString();
    }
}
