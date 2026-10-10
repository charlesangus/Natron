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

#include <list>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Channel/RemoveLayers.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

#include <ofxImageEffect.h>
#include <ofxNatron.h>

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

namespace {

const double kTime = 1.;
const char* const kBlurPluginID = "net.sf.cimg.CImgBlur";

// The colour storage entry is named by its layout, e.g. "Color(3)", so a narrowed plane shows.
std::vector<std::string>
describe(const std::list<ImageLayerDesc>& layers)
{
    std::vector<std::string> ids;

    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        if (it->isColorLayer()) {
            std::ostringstream os;
            os << "Color(" << it->getNumComponents() << ")";
            ids.push_back(os.str());
        } else {
            ids.push_back(it->getLayerID());
        }
    }

    return ids;
}

std::vector<std::string>
ids(std::initializer_list<const char*> names)
{
    std::vector<std::string> result;

    for (const char* name : names) {
        result.push_back(name);
    }

    return result;
}

} // namespace

class RemoveLayersTest
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

    NodePtr createReader(const std::string& fixture)
    {
        CreateNodeArgs readerArgs(_readPluginID.toStdString(), getApp()->getProject());

        readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/") + fixture);

        return getApp()->createNode(readerArgs);
    }

    NodePtr createRemoveOnReader(const std::string& fixture = "flat-three-layers.exr")
    {
        NodePtr reader = createReader(fixture);
        if (!reader) {
            return NodePtr();
        }
        NodePtr remove = createNode(QString::fromUtf8(PLUGINID_NATRON_REMOVELAYERS));
        if (!remove) {
            return NodePtr();
        }
        connectNodes(reader, remove, 0, true);

        return remove;
    }

    static KnobChannelSetPtr channelsKnob(const NodePtr& node)
    {
        return std::dynamic_pointer_cast<KnobChannelSet>(node->getKnobByName(kRemoveLayersParamChannels));
    }

    static KnobChoicePtr operationKnob(const NodePtr& node)
    {
        return std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kRemoveLayersParamOperation));
    }

    static void setKeep(const NodePtr& node)
    {
        KnobChoicePtr operation = operationKnob(node);

        ASSERT_TRUE(bool(operation));
        operation->setValue((int)RemoveLayers::eOperationKeep);
    }

    static std::vector<std::string> present(const NodePtr& node)
    {
        std::list<ImageLayerDesc> layers;

        node->getEffectInstance()->getPresentLayers(kTime, ViewIdx(0), -1, &layers);

        return describe(layers);
    }

    static bool isIdentityOfSource(const NodePtr& node)
    {
        EffectInstancePtr effect = node->getEffectInstance();
        const RectI window(0, 0, 8, 8);
        double inputTime = 0.;
        ViewIdx inputView(0);
        int inputNb = -1;
        const bool identity = effect->isIdentity_public(false, effect->getRenderHash(), kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb);

        return identity && (inputNb == 0);
    }

    static std::vector<std::string> blurListing(const NodePtr& blur)
    {
        std::list<ImageLayerDesc> views;

        blur->listLayerViewsForKnob(blur->getLayerKnob(), kTime, ViewIdx(0), &views);

        std::vector<std::string> result;
        for (std::list<ImageLayerDesc>::const_iterator it = views.begin(); it != views.end(); ++it) {
            result.push_back(it->getLayerID());
        }

        return result;
    }
};

TEST_F(RemoveLayersTest, IsRegisteredInChannel)
{
    const PluginsMap& plugins = appPTR->getPluginsList();
    PluginsMap::const_iterator found = plugins.find(PLUGINID_NATRON_REMOVELAYERS);

    ASSERT_TRUE(found != plugins.end());
    ASSERT_FALSE(found->second.empty());
    for (PluginVersionsOrdered::const_iterator it = found->second.begin(); it != found->second.end(); ++it) {
        EXPECT_TRUE((*it)->getGrouping().contains(QString::fromUtf8(PLUGIN_GROUP_CHANNEL)));
        EXPECT_EQ(QString::fromUtf8("RemoveLayers"), (*it)->getPluginLabel());
    }
}

TEST_F(RemoveLayersTest, NewNodeSelectsNothingAndIsAnIdentity)
{
    NodePtr remove = createRemoveOnReader();

    ASSERT_TRUE(bool(remove));
    EffectInstancePtr effect = remove->getEffectInstance();
    EXPECT_EQ(1, remove->getNInputs());
    EXPECT_TRUE(effect->isInputOptional(0));
    EXPECT_EQ(std::string("Source"), effect->getInputLabel(0));
    EXPECT_TRUE(effect->isMultiPlanar());
    EXPECT_FALSE(effect->producesMetadataLayerImplicitly());
    EXPECT_FALSE(bool(remove->getLayerKnob()));

    KnobChoicePtr operation = operationKnob(remove);
    KnobChannelSetPtr channels = channelsKnob(remove);
    ASSERT_TRUE(bool(operation));
    ASSERT_TRUE(bool(channels));
    EXPECT_EQ((int)RemoveLayers::eOperationRemove, operation->getValue());
    EXPECT_FALSE(channels->getWithChannelButtons());
    ASSERT_EQ(1u, channels->getRows().size());
    EXPECT_EQ(ChannelSetRow::eModeNone, channels->getRows()[0].mode);
    EXPECT_FALSE(remove->isTargetLayerKnob(channels));

    EXPECT_TRUE(isIdentityOfSource(remove));
    EXPECT_EQ(ids({ "Color(4)", "diffuse", "specular" }), present(remove));
}

TEST_F(RemoveLayersTest, RemovingANonColorLayerHidesItAndKeepsColor)
{
    NodePtr remove = createRemoveOnReader();

    ASSERT_TRUE(bool(remove));
    KnobChannelSetPtr channels = channelsKnob(remove);
    ASSERT_TRUE(bool(channels));

    channels->setLayer(0, "diffuse", NULL);
    EXPECT_EQ(ids({ "Color(4)", "specular" }), present(remove));
    EXPECT_FALSE(isIdentityOfSource(remove));
}

TEST_F(RemoveLayersTest, RemovingAlphaNarrowsColorToRGB)
{
    NodePtr remove = createRemoveOnReader();

    ASSERT_TRUE(bool(remove));
    KnobChannelSetPtr channels = channelsKnob(remove);
    ASSERT_TRUE(bool(channels));

    channels->setLayer(0, kNatronColorViewAlpha, NULL);
    EXPECT_EQ(ids({ "Color(3)", "diffuse", "specular" }), present(remove));
    EXPECT_FALSE(isIdentityOfSource(remove));
    EXPECT_EQ(3, remove->getEffectInstance()->getMetadataNComps(-1));
}

TEST_F(RemoveLayersTest, RemovingRgbNarrowsColorToAlpha)
{
    NodePtr remove = createRemoveOnReader();

    ASSERT_TRUE(bool(remove));
    KnobChannelSetPtr channels = channelsKnob(remove);
    ASSERT_TRUE(bool(channels));

    channels->setLayer(0, kNatronColorViewRGB, NULL);
    EXPECT_EQ(ids({ "Color(1)", "diffuse", "specular" }), present(remove));
    EXPECT_EQ(1, remove->getEffectInstance()->getMetadataNComps(-1));
}

TEST_F(RemoveLayersTest, RemovingRgbaDropsTheColorPlane)
{
    NodePtr remove = createRemoveOnReader();

    ASSERT_TRUE(bool(remove));
    KnobChannelSetPtr channels = channelsKnob(remove);
    ASSERT_TRUE(bool(channels));

    channels->setLayer(0, kNatronColorViewRGBA, NULL);
    EXPECT_EQ(ids({ "diffuse", "specular" }), present(remove));
    EXPECT_FALSE(isIdentityOfSource(remove));
}

TEST_F(RemoveLayersTest, RemovingRgbAndAlphaRowsDropsTheColorPlane)
{
    NodePtr remove = createRemoveOnReader();

    ASSERT_TRUE(bool(remove));
    KnobChannelSetPtr channels = channelsKnob(remove);
    ASSERT_TRUE(bool(channels));

    channels->setLayer(0, kNatronColorViewRGB, NULL);
    channels->addLayer(kNatronColorViewAlpha, NULL);
    EXPECT_EQ(ids({ "diffuse", "specular" }), present(remove));
}

TEST_F(RemoveLayersTest, RemovingRegexMatchingEverythingEmptiesTheStream)
{
    NodePtr remove = createRemoveOnReader();

    ASSERT_TRUE(bool(remove));
    KnobChannelSetPtr channels = channelsKnob(remove);
    ASSERT_TRUE(bool(channels));

    channels->setRegex(0, ".*");
    EXPECT_TRUE(present(remove).empty());
    EXPECT_FALSE(isIdentityOfSource(remove));
}

TEST_F(RemoveLayersTest, KeepingARegexKeepsOnlyTheLayersItMatches)
{
    NodePtr remove = createRemoveOnReader();

    ASSERT_TRUE(bool(remove));
    KnobChannelSetPtr channels = channelsKnob(remove);
    ASSERT_TRUE(bool(channels));
    setKeep(remove);

    channels->setRegex(0, "spec.*");
    EXPECT_EQ(ids({ "specular" }), present(remove));
}

TEST_F(RemoveLayersTest, KeepingAlphaAndDiffuseNarrowsColorToAlpha)
{
    NodePtr remove = createRemoveOnReader();

    ASSERT_TRUE(bool(remove));
    KnobChannelSetPtr channels = channelsKnob(remove);
    ASSERT_TRUE(bool(channels));
    setKeep(remove);

    channels->setLayer(0, kNatronColorViewAlpha, NULL);
    channels->addLayer("diffuse", NULL);
    EXPECT_EQ(ids({ "Color(1)", "diffuse" }), present(remove));
}

TEST_F(RemoveLayersTest, KeepingRegexMatchingEverythingIsAnIdentity)
{
    NodePtr remove = createRemoveOnReader();

    ASSERT_TRUE(bool(remove));
    KnobChannelSetPtr channels = channelsKnob(remove);
    ASSERT_TRUE(bool(channels));
    setKeep(remove);

    channels->setRegex(0, ".*");
    EXPECT_TRUE(isIdentityOfSource(remove));
    EXPECT_EQ(ids({ "Color(4)", "diffuse", "specular" }), present(remove));
}

TEST_F(RemoveLayersTest, KeepingNoneEmptiesTheStream)
{
    NodePtr remove = createRemoveOnReader();

    ASSERT_TRUE(bool(remove));
    setKeep(remove);

    EXPECT_TRUE(present(remove).empty());
    EXPECT_FALSE(isIdentityOfSource(remove));
}

TEST_F(RemoveLayersTest, RemovingAlphaFromAnRGBInputIsAnIdentity)
{
    NodePtr remove = createRemoveOnReader("flat-rgb-only.exr");

    ASSERT_TRUE(bool(remove));
    KnobChannelSetPtr channels = channelsKnob(remove);
    ASSERT_TRUE(bool(channels));

    channels->setLayer(0, kNatronColorViewAlpha, NULL);
    EXPECT_TRUE(isIdentityOfSource(remove));
    EXPECT_EQ(ids({ "Color(3)" }), present(remove));
    EXPECT_EQ(3, remove->getEffectInstance()->getMetadataNComps(-1));
}

// A colourless stream still lists the colour views downstream: they read zero there.
TEST_F(RemoveLayersTest, DownstreamListingFollowsTheSelection)
{
    NodePtr remove = createRemoveOnReader();

    ASSERT_TRUE(bool(remove));
    KnobChannelSetPtr channels = channelsKnob(remove);
    ASSERT_TRUE(bool(channels));
    NodePtr blur = createNode(QString::fromUtf8(kBlurPluginID));
    ASSERT_TRUE(bool(blur));
    connectNodes(remove, blur, 0, true);
    ASSERT_TRUE(bool(blur->getLayerKnob()));

    const std::string rgba = kNatronColorViewRGBA;
    const std::string rgb = kNatronColorViewRGB;
    const std::string alpha = kNatronColorViewAlpha;

    std::vector<std::string> expected = { rgba, rgb, alpha, "diffuse", "specular" };
    EXPECT_EQ(expected, blurListing(blur));

    channels->setLayer(0, "diffuse", NULL);
    expected = { rgba, rgb, alpha, "specular" };
    EXPECT_EQ(expected, blurListing(blur));

    channels->setLayer(0, kNatronColorViewRGBA, NULL);
    expected = { rgba, rgb, alpha, "diffuse", "specular" };
    EXPECT_EQ(expected, blurListing(blur));
    EXPECT_EQ(ids({ "diffuse", "specular" }), present(remove));
}

TEST_F(RemoveLayersTest, ARowNamingAnAbsentLayerPostsNoError)
{
    NodePtr remove = createRemoveOnReader();

    ASSERT_TRUE(bool(remove));
    KnobChannelSetPtr channels = channelsKnob(remove);
    ASSERT_TRUE(bool(channels));

    channels->setLayer(0, "depth", NULL);
    EXPECT_EQ(ids({ "Color(4)", "diffuse", "specular" }), present(remove));
    EXPECT_TRUE(isIdentityOfSource(remove));

    std::string message;
    EXPECT_TRUE(remove->checkSelectedChannelsPresent(kTime, ViewIdx(0), &message)) << message;

    QString persistent;
    int type = 0;
    remove->getPersistentMessage(&persistent, &type, false);
    EXPECT_TRUE(persistent.isEmpty()) << persistent.toStdString();

    setKeep(remove);
    EXPECT_TRUE(present(remove).empty());
    EXPECT_TRUE(remove->checkSelectedChannelsPresent(kTime, ViewIdx(0), &message)) << message;
    remove->getPersistentMessage(&persistent, &type, false);
    EXPECT_TRUE(persistent.isEmpty()) << persistent.toStdString();
}

TEST_F(RemoveLayersTest, RemovingALayerItNamesIsRefused)
{
    NodePtr remove = createRemoveOnReader();

    ASSERT_TRUE(bool(remove));
    KnobChannelSetPtr channels = channelsKnob(remove);
    ASSERT_TRUE(bool(channels));

    channels->setLayer(0, "diffuse", NULL);

    std::string error;
    EXPECT_FALSE(getApp()->getProject()->removeLayer("diffuse", &error));
    EXPECT_NE(std::string::npos, error.find(remove->getScriptName_mt_safe())) << error;
}

TEST_F(RemoveLayersTest, SubLabelNamesTheOperationAndSelection)
{
    NodePtr remove = createRemoveOnReader();

    ASSERT_TRUE(bool(remove));
    KnobChannelSetPtr channels = channelsKnob(remove);
    KnobStringPtr sublabel = std::dynamic_pointer_cast<KnobString>(remove->getKnobByName(kNatronOfxParamStringSublabelName));
    ASSERT_TRUE(bool(channels));
    ASSERT_TRUE(bool(sublabel));
    EXPECT_TRUE(sublabel->getIsSecret());

    EXPECT_EQ(std::string(), sublabel->getValue());

    channels->setLayer(0, "diffuse", NULL);
    channels->addLayer(kNatronColorViewAlpha, NULL);
    EXPECT_EQ(std::string("remove\ndiffuse +1"), sublabel->getValue());

    channels->addRegex("spec.*");
    EXPECT_EQ(std::string("remove\ndiffuse +2"), sublabel->getValue());

    setKeep(remove);
    std::vector<ChannelSetRow> rows(1);
    rows[0].mode = ChannelSetRow::eModeRegex;
    rows[0].layerOrPattern = "spec.*";
    channels->setRows(rows);
    EXPECT_EQ(std::string("keep\nspecular"), sublabel->getValue());
}

TEST_F(RemoveLayersTest, KeepSelectionSurvivesSaveResetLoad)
{
    NodePtr remove = createRemoveOnReader();

    ASSERT_TRUE(bool(remove));
    KnobChannelSetPtr channels = channelsKnob(remove);
    ASSERT_TRUE(bool(channels));
    setKeep(remove);
    channels->setRegex(0, "spec.*");
    channels->addLayer(kNatronColorViewAlpha, NULL);

    const std::string removeName = remove->getScriptName();
    const std::vector<ChannelSetRow> savedRows = channels->getRows();
    ASSERT_EQ(2u, savedRows.size());
    ASSERT_EQ(ids({ "Color(1)", "specular" }), present(remove));

    ProjectPtr project = getApp()->getProject();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("remove-layers.ntp");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));

    project->reset(false, true);
    ASSERT_TRUE(project->getNodeByName(removeName).get() == NULL);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr remove2 = project->getNodeByName(removeName);
    ASSERT_TRUE(bool(remove2));
    KnobChannelSetPtr channels2 = channelsKnob(remove2);
    KnobChoicePtr operation2 = operationKnob(remove2);
    ASSERT_TRUE(bool(channels2));
    ASSERT_TRUE(bool(operation2));

    EXPECT_EQ((int)RemoveLayers::eOperationKeep, operation2->getValue());
    EXPECT_EQ(savedRows, channels2->getRows());
    EXPECT_EQ(ids({ "Color(1)", "specular" }), present(remove2));
    EXPECT_EQ(1, remove2->getEffectInstance()->getMetadataNComps(-1));
}

TEST_F(RemoveLayersTest, SetChannelsRaisesValueErrorFromPython)
{
    NodePtr remove = createRemoveOnReader();

    ASSERT_TRUE(bool(remove));
    KnobChannelSetPtr channels = channelsKnob(remove);
    ASSERT_TRUE(bool(channels));
    channels->setLayer(0, "diffuse", NULL);
    const std::vector<ChannelSetRow> before = channels->getRows();

    const std::string appVar = getApp()->getAppIDString();
    const std::string script = "channelsParam = " + appVar + ".getNode(\"" + remove->getScriptName() + "\").getParam(\"" + kRemoveLayersParamChannels + "\")\n"
        + "channelsParam.setChannels([\"R\"], 0)\n";

    std::string error, output;
    EXPECT_FALSE(interpretPythonScript(script, &error, &output));
    EXPECT_NE(std::string::npos, error.find("ValueError")) << error;
    EXPECT_EQ(before, channels->getRows());
}
