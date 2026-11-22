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

#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/USDCamera.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDReference.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kTestPluginIDPrefix = "test.natron.";

bool
isReloadButton(const KnobIPtr& knob)
{
    if (!dynamic_cast<KnobButton*>(knob.get())) {
        return false;
    }

    return QString::fromStdString(knob->getName()).contains(QString::fromUtf8("reload"), Qt::CaseInsensitive)
        || QString::fromStdString(knob->getLabel()).contains(QString::fromUtf8("reload"), Qt::CaseInsensitive);
}

// The file knobs of a node that show their reload icon when the node has a reload button as well.
std::vector<std::string>
fileKnobsDoublingAReloadButton(const NodePtr& node)
{
    std::vector<std::string> icons;
    bool hasButton = false;
    const KnobsVec& knobs = node->getEffectInstance()->getKnobs();

    for (const KnobIPtr& knob : knobs) {
        hasButton = hasButton || isReloadButton(knob);
        KnobFile* file = dynamic_cast<KnobFile*>(knob.get());
        if (file && file->showsReloadButton()) {
            icons.push_back(knob->getName());
        }
    }

    return hasButton ? icons : std::vector<std::string>();
}
} // anonymous namespace

typedef BaseTest FileKnobReloadSpecTest;

TEST_F(FileKnobReloadSpecTest, NoRegisteredNodeShowsAReloadIconBesideAReloadButton)
{
    std::set<std::string> walked;
    const PluginsMap& plugins = appPTR->getPluginsList();

    for (PluginsMap::const_iterator it = plugins.begin(); it != plugins.end(); ++it) {
        if (it->first.compare(0, std::string(kTestPluginIDPrefix).size(), kTestPluginIDPrefix) == 0) {
            continue;
        }
        for (Plugin* plugin : it->second) {
            // Internal-only plugins only exist inside the node that owns them.
            if (!plugin || plugin->getIsForInternalUseOnly()) {
                continue;
            }
            CreateNodeArgs args(plugin->getPluginID().toStdString(), getApp()->getProject());
            args.setProperty<int>(kCreateNodeArgsPropPluginVersion, plugin->getMajorVersion(), 0);
            args.setProperty<int>(kCreateNodeArgsPropPluginVersion, plugin->getMinorVersion(), 1);
            NodePtr node = getApp()->createNode(args);
            if (!node) {
                continue;
            }
            walked.insert(it->first);
            for (const std::string& name : fileKnobsDoublingAReloadButton(node)) {
                ADD_FAILURE() << it->first << ": " << name << " shows its reload icon beside a reload button";
            }
        }
    }

    for (const char* id : { PLUGINID_NATRON_READ, PLUGINID_NATRON_USDREAD, PLUGINID_NATRON_USDREFERENCE, PLUGINID_NATRON_USDCAMERA }) {
        EXPECT_EQ(1u, walked.count(id)) << id << " was not created, so it was not checked";
    }
}

TEST_F(FileKnobReloadSpecTest, AnOfxFileParamGivesItsIconToTheReloadButtonOnItsRow)
{
    NodePtr node = createNode(QString::fromUtf8("net.sf.openfx.Shadertoy"));
    ASSERT_TRUE(bool(node));
    KnobFile* file = dynamic_cast<KnobFile*>(node->getKnobByName("imageShaderFileName").get());
    ASSERT_TRUE(file);
    KnobIPtr button = node->getKnobByName("imageShaderReload");
    ASSERT_TRUE(bool(button)) << "the plugin keeps the button it declares";
    EXPECT_FALSE(file->showsReloadButton());
    EXPECT_EQ(button, file->getReloadButton());
}

TEST_F(FileKnobReloadSpecTest, AnOfxFileParamWithoutAReloadButtonKeepsItsIcon)
{
    NodePtr node = createNode(QString::fromUtf8("net.fxarena.openfx.Text"));
    ASSERT_TRUE(bool(node));
    KnobFile* file = dynamic_cast<KnobFile*>(node->getKnobByName("file").get());
    ASSERT_TRUE(file);
    EXPECT_TRUE(file->showsReloadButton());
    EXPECT_FALSE(bool(file->getReloadButton()));
}

TEST_F(FileKnobReloadSpecTest, TheCheckSeesAnIconBesideAButton)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    ASSERT_TRUE(bool(node));
    EffectInstance* effect = node->getEffectInstance().get();
    KnobFilePtr file = effect->createFileKnob("file", "File");
    ASSERT_TRUE(bool(file));
    EXPECT_TRUE(fileKnobsDoublingAReloadButton(node).empty()) << "an icon alone is the one reload";

    KnobButtonPtr button = effect->createButtonKnob("reload", "Reload");
    ASSERT_TRUE(bool(button));
    EXPECT_EQ(std::vector<std::string>(1, "file"), fileKnobsDoublingAReloadButton(node));

    file->setReloadButton(button);
    EXPECT_TRUE(fileKnobsDoublingAReloadButton(node).empty()) << "the button is the one reload";
}

TEST_F(FileKnobReloadSpecTest, ABuiltReloadButtonReplacesTheIcon)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    ASSERT_TRUE(bool(node));
    KnobFilePtr file = node->getEffectInstance()->createFileKnob("file", "File");
    ASSERT_TRUE(bool(file));
    EXPECT_TRUE(file->showsReloadButton());
    EXPECT_FALSE(bool(file->getReloadButton()));

    KnobButtonPtr button = file->createReloadButton("reload", QString::fromUtf8("Reads the file again."));
    ASSERT_TRUE(bool(button));
    EXPECT_FALSE(file->showsReloadButton());
    EXPECT_EQ(button, file->getReloadButton());
    EXPECT_EQ(std::string("reload"), button->getName());
    EXPECT_EQ(std::string("Reload"), button->getLabel());
    EXPECT_FALSE(file->isNewLineActivated());
}
