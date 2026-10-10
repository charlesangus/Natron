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
#include "Engine/LayerRegistry.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Channel/AddLayers.h"
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

// The colour storage entry is named by its layout, e.g. "Color(3)", so a widened plane shows.
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

class AddLayersTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();
        project->reset(false, true);

        std::vector<std::string> a(1, "A");
        std::string error;
        ASSERT_EQ(LayerRegistry::eAddResultAdded, project->addLayer(ImageLayerDesc("mask", "mask", "", a), LayerRegistryEntry::eOriginUser, &error)) << error;
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

    NodePtr createAddLayers()
    {
        return createNode(QString::fromUtf8(PLUGINID_NATRON_ADDLAYERS));
    }

    NodePtr createAddOnReader(const std::string& fixture = "flat-three-layers.exr")
    {
        NodePtr reader = createReader(fixture);
        if (!reader) {
            return NodePtr();
        }
        NodePtr add = createAddLayers();
        if (!add) {
            return NodePtr();
        }
        connectNodes(reader, add, 0, true);

        return add;
    }

    static KnobChannelSetPtr layersKnob(const NodePtr& node)
    {
        return std::dynamic_pointer_cast<KnobChannelSet>(node->getKnobByName(kAddLayersParamLayers));
    }

    static std::vector<std::string> present(const NodePtr& node)
    {
        std::list<ImageLayerDesc> layers;

        node->getEffectInstance()->getPresentLayers(kTime, ViewIdx(0), -1, &layers);

        return describe(layers);
    }

    static std::vector<std::string> produced(const NodePtr& node)
    {
        EffectInstancePtr effect = node->getEffectInstance();
        EffectInstance::ComponentsNeededMap comps;
        std::list<ImageLayerDesc> passThroughLayers;
        double passThroughTime = 0.;
        int passThroughView = 0;
        std::bitset<4> processChannels;
        EffectInstance::ProcessChannelsPerPlaneMap processChannelsPerPlane;
        int passThroughInputNb = -1;

        effect->getComponentsNeededAndProduced_public(effect->getRenderHash(), kTime, ViewIdx(0), &comps, &passThroughLayers, &passThroughTime, &passThroughView, &processChannels, &processChannelsPerPlane, &passThroughInputNb);

        return describe(comps[-1]);
    }

    static std::string usedBy(const KnobTablePtr& table,
                              const std::string& label)
    {
        std::list<std::vector<std::string>> rows;

        table->getTable(&rows);
        for (std::list<std::vector<std::string>>::const_iterator it = rows.begin(); it != rows.end(); ++it) {
            if (((*it).size() == 3) && ((*it)[0] == label)) {
                return (*it)[2];
            }
        }

        return std::string();
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
};

TEST_F(AddLayersTest, IsRegisteredInChannel)
{
    const PluginsMap& plugins = appPTR->getPluginsList();
    PluginsMap::const_iterator found = plugins.find(PLUGINID_NATRON_ADDLAYERS);

    ASSERT_TRUE(found != plugins.end());
    ASSERT_FALSE(found->second.empty());
    for (PluginVersionsOrdered::const_iterator it = found->second.begin(); it != found->second.end(); ++it) {
        EXPECT_TRUE((*it)->getGrouping().contains(QString::fromUtf8(PLUGIN_GROUP_CHANNEL)));
        EXPECT_EQ(QString::fromUtf8("AddLayers"), (*it)->getPluginLabel());
    }
}

TEST_F(AddLayersTest, NewNodeSelectsNothingAndIsAnIdentity)
{
    NodePtr add = createAddOnReader();

    ASSERT_TRUE(bool(add));
    EffectInstancePtr effect = add->getEffectInstance();
    EXPECT_EQ(1, add->getNInputs());
    EXPECT_TRUE(effect->isInputOptional(0));
    EXPECT_EQ(std::string("Source"), effect->getInputLabel(0));
    EXPECT_TRUE(effect->isMultiPlanar());
    EXPECT_FALSE(effect->producesMetadataLayerImplicitly());
    EXPECT_FALSE(bool(add->getLayerKnob()));

    KnobChannelSetPtr layers = layersKnob(add);
    ASSERT_TRUE(bool(layers));
    EXPECT_FALSE(layers->getWithChannelButtons());
    ASSERT_EQ(1u, layers->getRows().size());
    EXPECT_EQ(ChannelSetRow::eModeNone, layers->getRows()[0].mode);
    EXPECT_TRUE(add->isTargetLayerKnob(layers));

    EXPECT_TRUE(isIdentityOfSource(add));
    EXPECT_TRUE(produced(add).empty());
    EXPECT_EQ(ids({ "Color(4)", "diffuse", "specular" }), present(add));
}

TEST_F(AddLayersTest, ListsTheRegistryWithColorAsViews)
{
    NodePtr add = createAddOnReader("flat-rgb-only.exr");

    ASSERT_TRUE(bool(add));
    KnobChannelSetPtr layers = layersKnob(add);
    ASSERT_TRUE(bool(layers));

    std::list<ImageLayerDesc> storage;
    add->listLayersForKnob(layers, &storage);
    int nColor = 0;
    bool hasMask = false;
    for (std::list<ImageLayerDesc>::const_iterator it = storage.begin(); it != storage.end(); ++it) {
        if (it->isColorLayer()) {
            ++nColor;
            EXPECT_EQ(4, it->getNumComponents());
        } else if (it->getLayerID() == "mask") {
            hasMask = true;
        }
    }
    EXPECT_EQ(1, nColor);
    EXPECT_TRUE(hasMask);

    std::list<ImageLayerDesc> views;
    add->listLayerViewsForKnob(layers, &views);
    std::vector<std::string> viewIDs;
    for (std::list<ImageLayerDesc>::const_iterator it = views.begin(); it != views.end(); ++it) {
        viewIDs.push_back(it->getLayerID());
    }
    EXPECT_NE(viewIDs.end(), std::find(viewIDs.begin(), viewIDs.end(), std::string(kNatronColorViewRGBA)));
    EXPECT_NE(viewIDs.end(), std::find(viewIDs.begin(), viewIDs.end(), std::string(kNatronColorViewAlpha)));
    EXPECT_NE(viewIDs.end(), std::find(viewIDs.begin(), viewIDs.end(), std::string("mask")));
}

TEST_F(AddLayersTest, ProducesOnlyTheLayersTheInputLacks)
{
    NodePtr add = createAddOnReader();

    ASSERT_TRUE(bool(add));
    KnobChannelSetPtr layers = layersKnob(add);
    ASSERT_TRUE(bool(layers));

    layers->setLayer(0, "mask", NULL);
    layers->addLayer("diffuse", NULL);
    EXPECT_EQ(ids({ "mask" }), produced(add));
    EXPECT_EQ(ids({ "Color(4)", "diffuse", "specular", "mask" }), present(add));
    EXPECT_FALSE(isIdentityOfSource(add));
}

TEST_F(AddLayersTest, AddingRgbaToAnRGBInputWidensColorToRGBA)
{
    NodePtr add = createAddOnReader("flat-rgb-only.exr");

    ASSERT_TRUE(bool(add));
    KnobChannelSetPtr layers = layersKnob(add);
    ASSERT_TRUE(bool(layers));
    EXPECT_EQ(ids({ "Color(3)" }), present(add));

    layers->setLayer(0, kNatronColorViewRGBA, NULL);
    EXPECT_EQ(ids({ "Color(4)" }), produced(add));
    EXPECT_EQ(ids({ "Color(4)" }), present(add));
    EXPECT_EQ(4, add->getEffectInstance()->getMetadataNComps(-1));
    EXPECT_FALSE(isIdentityOfSource(add));
}

TEST_F(AddLayersTest, AddingRgbToAnRGBInputIsAnIdentity)
{
    NodePtr add = createAddOnReader("flat-rgb-only.exr");

    ASSERT_TRUE(bool(add));
    KnobChannelSetPtr layers = layersKnob(add);
    ASSERT_TRUE(bool(layers));

    layers->setLayer(0, kNatronColorViewRGB, NULL);
    EXPECT_TRUE(isIdentityOfSource(add));
    EXPECT_TRUE(produced(add).empty());
    EXPECT_EQ(ids({ "Color(3)" }), present(add));
    EXPECT_EQ(3, add->getEffectInstance()->getMetadataNComps(-1));
}

TEST_F(AddLayersTest, AddingRgbToAnAlphaInputWidensColorToRGBA)
{
    NodePtr add = createAddOnReader("flat-alpha-only.exr");

    ASSERT_TRUE(bool(add));
    KnobChannelSetPtr layers = layersKnob(add);
    ASSERT_TRUE(bool(layers));

    layers->setLayer(0, kNatronColorViewRGB, NULL);
    EXPECT_EQ(ids({ "Color(4)" }), produced(add));
    EXPECT_EQ(ids({ "Color(4)" }), present(add));
    EXPECT_EQ(4, add->getEffectInstance()->getMetadataNComps(-1));
}

TEST_F(AddLayersTest, UnconnectedPresentsOnlyWhatItAdds)
{
    NodePtr add = createAddLayers();

    ASSERT_TRUE(bool(add));
    KnobChannelSetPtr layers = layersKnob(add);
    ASSERT_TRUE(bool(layers));

    layers->setLayer(0, "mask", NULL);
    EXPECT_EQ(ids({ "mask" }), present(add));

    layers->setLayer(0, kNatronColorViewAlpha, NULL);
    EXPECT_EQ(ids({ "Color(1)" }), present(add));
    EXPECT_EQ(1, add->getEffectInstance()->getMetadataNComps(-1));
}

TEST_F(AddLayersTest, RemovingALayerItNamesIsRefused)
{
    NodePtr add = createAddOnReader();

    ASSERT_TRUE(bool(add));
    KnobChannelSetPtr layers = layersKnob(add);
    ASSERT_TRUE(bool(layers));

    layers->setLayer(0, "mask", NULL);

    std::string error;
    EXPECT_FALSE(getApp()->getProject()->removeLayer("mask", &error));
    EXPECT_NE(std::string::npos, error.find(add->getScriptName_mt_safe())) << error;
}

TEST_F(AddLayersTest, NamingALayerCountsAsAUserAsSoonAsTheRowIsSet)
{
    NodePtr add = createAddLayers();

    ASSERT_TRUE(bool(add));
    KnobChannelSetPtr layers = layersKnob(add);
    ASSERT_TRUE(bool(layers));
    KnobTablePtr table = std::dynamic_pointer_cast<KnobTable>(getApp()->getProject()->getKnobByName("defaultLayers"));
    ASSERT_TRUE(bool(table));
    EXPECT_EQ(std::string("0"), usedBy(table, "mask"));

    layers->setLayer(0, "mask", NULL);
    EXPECT_EQ(std::string("1"), usedBy(table, "mask"));

    std::vector<ChannelSetRow> none(1);
    none[0].mode = ChannelSetRow::eModeNone;
    layers->setRows(none);
    EXPECT_EQ(std::string("0"), usedBy(table, "mask"));
}

TEST_F(AddLayersTest, SubLabelNamesTheAddedLayers)
{
    NodePtr add = createAddOnReader();

    ASSERT_TRUE(bool(add));
    KnobChannelSetPtr layers = layersKnob(add);
    KnobStringPtr sublabel = std::dynamic_pointer_cast<KnobString>(add->getKnobByName(kNatronOfxParamStringSublabelName));
    ASSERT_TRUE(bool(layers));
    ASSERT_TRUE(bool(sublabel));
    EXPECT_TRUE(sublabel->getIsSecret());
    EXPECT_EQ(std::string(), sublabel->getValue());

    layers->setLayer(0, "diffuse", NULL);
    layers->addLayer("mask", NULL);
    EXPECT_EQ(std::string("diffuse +1"), sublabel->getValue());

    std::vector<ChannelSetRow> rows(1);
    rows[0].mode = ChannelSetRow::eModeLayer;
    rows[0].layerOrPattern = kNatronColorViewAlpha;
    layers->setRows(rows);
    EXPECT_EQ(std::string("alpha"), sublabel->getValue());
}

TEST_F(AddLayersTest, SelectionSurvivesSaveResetLoad)
{
    NodePtr add = createAddOnReader("flat-rgb-only.exr");

    ASSERT_TRUE(bool(add));
    KnobChannelSetPtr layers = layersKnob(add);
    ASSERT_TRUE(bool(layers));
    layers->setLayer(0, "mask", NULL);
    layers->addLayer(kNatronColorViewRGBA, NULL);

    const std::string addName = add->getScriptName();
    const std::vector<ChannelSetRow> savedRows = layers->getRows();
    ASSERT_EQ(2u, savedRows.size());
    ASSERT_EQ(ids({ "Color(4)", "mask" }), present(add));

    ProjectPtr project = getApp()->getProject();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("add-layers.ntp");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));

    project->reset(false, true);
    ASSERT_TRUE(project->getNodeByName(addName).get() == NULL);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    ImageLayerDesc mask;
    EXPECT_TRUE(project->findLayer("mask", &mask));

    NodePtr add2 = project->getNodeByName(addName);
    ASSERT_TRUE(bool(add2));
    KnobChannelSetPtr layers2 = layersKnob(add2);
    ASSERT_TRUE(bool(layers2));

    EXPECT_EQ(savedRows, layers2->getRows());
    EXPECT_EQ(ids({ "Color(4)", "mask" }), present(add2));
    EXPECT_EQ(4, add2->getEffectInstance()->getMetadataNComps(-1));
}

TEST_F(AddLayersTest, SetChannelsRaisesValueErrorFromPython)
{
    NodePtr add = createAddOnReader();

    ASSERT_TRUE(bool(add));
    KnobChannelSetPtr layers = layersKnob(add);
    ASSERT_TRUE(bool(layers));
    layers->setLayer(0, "mask", NULL);
    const std::vector<ChannelSetRow> before = layers->getRows();

    const std::string appVar = getApp()->getAppIDString();
    const std::string script = "layersParam = " + appVar + ".getNode(\"" + add->getScriptName() + "\").getParam(\"" + kAddLayersParamLayers + "\")\n"
        + "layersParam.setChannels([\"A\"], 0)\n";

    std::string error, output;
    EXPECT_FALSE(interpretPythonScript(script, &error, &output));
    EXPECT_NE(std::string::npos, error.find("ValueError")) << error;
    EXPECT_EQ(before, layers->getRows());
}
