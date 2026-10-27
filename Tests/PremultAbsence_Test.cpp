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
#include <iostream>
#include <iterator>
#include <list>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "HashSweepEdits.h"
#include "NativeParity.h"
#include "OfxTestPlugins/natronTest/NatronTestPluginIDs.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/OfxEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

// ofxsImageEffect.h's PreMultiplicationEnum values, as the probe renders them.
const float kProbeUnPreMultiplied = 2.f;

const int kWindowSize = 4;

// What a probe's render says its Source clip and its fetched Source image reported.
struct ProbeReading {
    float clip;
    float image;
};

class PremultAbsenceTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr createConstant()
    {
        NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);
        if (constant) {
            EXPECT_TRUE(setKnobValues(constant, kConstantParamColor, { 0.25, 0.5, 0.75, 0.5 }));
        }

        return constant;
    }

    NodePtr createProbe()
    {
        return createNode(QString::fromUtf8(kNatronTestPremultProbePluginID), kNatronTestPremultProbeMajor);
    }

    // Renders `probe` and reads back what its clip and image reported; false, with a test
    // failure, when it cannot.
    bool readProbe(const NodePtr& probe,
                   ProbeReading* reading)
    {
        std::vector<RenderedPlane> planes;
        std::string error;
        const RectI window(0, 0, kWindowSize, kWindowSize);
        if (!renderNodePlanesDirect(probe, 1., ViewIdx(0), 0, window, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error)) {
            ADD_FAILURE() << probe->getScriptName_mt_safe() << ": " << error;

            return false;
        }
        if ((planes.size() != 1) || planes[0].pixels.empty()) {
            ADD_FAILURE() << probe->getScriptName_mt_safe() << ": no RGBA plane rendered";

            return false;
        }
        const RenderedPlane& plane = planes[0];
        const std::vector<std::string>::const_iterator r = std::find(plane.channels.begin(), plane.channels.end(), std::string("R"));
        const std::vector<std::string>::const_iterator g = std::find(plane.channels.begin(), plane.channels.end(), std::string("G"));
        if ((r == plane.channels.end()) || (g == plane.channels.end())) {
            ADD_FAILURE() << probe->getScriptName_mt_safe() << ": the rendered plane has no R or G";

            return false;
        }
        reading->clip = plane.pixels[r - plane.channels.begin()];
        reading->image = plane.pixels[g - plane.channels.begin()];

        return true;
    }
};

const char*
reportedOption(bool opaque)
{
    return opaque ? kNatronTestPremultProbeReportedOpaqueID : kNatronTestPremultProbeReportedPreMultipliedID;
}

bool
hasAlphaDeclarationKnob(const NodePtr& node)
{
    return bool(node->getKnobByName("inputAlpha"));
}

const char* const kBannedKnobNames[] = {
    "premult",
    "premultChannel",
    "premultChanged",
    "filePremult",
    "outputPremult",
    "inputPremult",
    "inputAlpha",
};

// OFX plug-ins that still load and declare a banned knob at the pinned openfx-misc.
const char* const kExemptOfxPluginIDs[] = {
    "net.sf.openfx.GodRays",
};

} // anonymous namespace

// Whatever an upstream plug-in reports on its output, the downstream clip and fetched image
// report Unpremultiplied.
TEST_F(PremultAbsenceTest, DownstreamAlwaysSeesUnpremultiplied)
{
    NodePtr constant = createConstant();
    NodePtr upstream = createProbe();
    NodePtr downstream = createProbe();
    ASSERT_TRUE(constant && upstream && downstream);
    connectNodes(constant, upstream, 0, true);
    connectNodes(upstream, downstream, 0, true);

    for (int reportsOpaque = 0; reportsOpaque < 2; ++reportsOpaque) {
        ASSERT_TRUE(setKnobValue(upstream, kNatronTestPremultProbeParamReported, reportedOption(reportsOpaque)));
        const std::string what = std::string("upstream reports ") + reportedOption(reportsOpaque);

        ProbeReading reading;
        ASSERT_TRUE(readProbe(downstream, &reading));
        EXPECT_EQ(kProbeUnPreMultiplied, reading.clip) << what << ": downstream clip";
        EXPECT_EQ(kProbeUnPreMultiplied, reading.image) << what << ": downstream image";

        ASSERT_TRUE(readProbe(upstream, &reading));
        EXPECT_EQ(kProbeUnPreMultiplied, reading.clip) << what << ": upstream clip";
        EXPECT_EQ(kProbeUnPreMultiplied, reading.image) << what << ": upstream image";
    }
}

// The premultiplication a plug-in reports on its output is discarded: it changes neither its
// own metadata nor anything its downstream node is handed.
TEST_F(PremultAbsenceTest, ReportedOutputPremultIsDiscarded)
{
    NodePtr constant = createConstant();
    NodePtr upstream = createProbe();
    NodePtr downstream = createProbe();
    ASSERT_TRUE(constant && upstream && downstream);
    connectNodes(constant, upstream, 0, true);
    connectNodes(upstream, downstream, 0, true);

    ASSERT_TRUE(setKnobValue(upstream, kNatronTestPremultProbeParamReported, kNatronTestPremultProbeReportedPreMultipliedID));
    NodeMetadataConstPtr upstreamBefore, downstreamBefore;
    ASSERT_TRUE(upstreamBefore = upstream->getEffectInstance()->getMetadata(1., ViewIdx(0)));
    ASSERT_TRUE(downstreamBefore = downstream->getEffectInstance()->getMetadata(1., ViewIdx(0)));
    ProbeReading before;
    ASSERT_TRUE(readProbe(downstream, &before));

    ASSERT_TRUE(setKnobValue(upstream, kNatronTestPremultProbeParamReported, kNatronTestPremultProbeReportedOpaqueID));
    NodeMetadataConstPtr upstreamAfter, downstreamAfter;
    ASSERT_TRUE(upstreamAfter = upstream->getEffectInstance()->getMetadata(1., ViewIdx(0)));
    ASSERT_TRUE(downstreamAfter = downstream->getEffectInstance()->getMetadata(1., ViewIdx(0)));
    ProbeReading after;
    ASSERT_TRUE(readProbe(downstream, &after));

    EXPECT_TRUE(*upstreamBefore == *upstreamAfter) << "upstream metadata";
    EXPECT_TRUE(*downstreamBefore == *downstreamAfter) << "downstream metadata";
    EXPECT_EQ(before.clip, after.clip) << "downstream clip";
    EXPECT_EQ(before.image, after.image) << "downstream image";
    EXPECT_EQ(kProbeUnPreMultiplied, after.clip) << "downstream clip";
    EXPECT_EQ(kProbeUnPreMultiplied, after.image) << "downstream image";
}

// No node, OFX or native, carries a knob declaring its source alpha.
TEST_F(PremultAbsenceTest, NoNodeHasAnAlphaDeclarationKnob)
{
    NodePtr probe = createProbe();
    NodePtr constant = createConstant();
    NodePtr copy = createNode(QString::fromUtf8(kNatronTestTwoInputCopyPluginID), kNatronTestTwoInputCopyMajor);
    ASSERT_TRUE(probe && constant && copy);

    EXPECT_FALSE(hasAlphaDeclarationKnob(probe));
    EXPECT_FALSE(hasAlphaDeclarationKnob(constant));
    EXPECT_FALSE(hasAlphaDeclarationKnob(copy));
}

TEST_F(PremultAbsenceTest, NoRegisteredPluginHasAPremultKnob)
{
    const std::set<std::string> exempt(std::begin(kExemptOfxPluginIDs), std::end(kExemptOfxPluginIDs));
    const std::vector<HashSweep::PluginVersion> plugins = HashSweep::sweptPlugins();
    ASSERT_FALSE(plugins.empty());

    const bool bundlesLoaded = HashSweep::ofxBundleExpected();
    if (!bundlesLoaded) {
        std::cout << "[premult absence] OFX_PLUGIN_PATH unset: the shipped OpenFX bundles are not covered" << std::endl;
    }

    std::set<std::string> exemptSeen;
    int checked = 0;
    for (std::size_t i = 0; i < plugins.size(); ++i) {
        const HashSweep::PluginVersion& plugin = plugins[i];
        if (plugin.isOfx && exempt.count(plugin.id)) {
            exemptSeen.insert(plugin.id);
            continue;
        }
        CreateNodeArgs args(plugin.id, getApp()->getProject());
        args.setProperty<int>(kCreateNodeArgsPropPluginVersion, plugin.major, 0);
        args.setProperty<int>(kCreateNodeArgsPropPluginVersion, plugin.minor, 1);
        args.setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);
        args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
        args.setProperty<bool>(kCreateNodeArgsPropSilent, true);
        args.setProperty<bool>(kCreateNodeArgsPropTrustPluginID, true);
        NodePtr node = getApp()->createNode(args);
        if (!node) {
            ADD_FAILURE() << plugin.describe() << ": the node could not be created";
            continue;
        }
        for (const char* name : kBannedKnobNames) {
            EXPECT_FALSE(bool(node->getKnobByName(name))) << plugin.describe() << " has a knob named " << name;
        }
        node->destroyNode(true, false);
        ++checked;
    }
    std::cout << "[premult absence] " << checked << " plugins checked, " << exemptSeen.size() << " exempt" << std::endl;

    if (bundlesLoaded) {
        for (const std::string& id : exempt) {
            EXPECT_TRUE(exemptSeen.count(id)) << id << " is exempt but no longer loaded as an OpenFX plugin; remove the entry";
        }
    }
}
