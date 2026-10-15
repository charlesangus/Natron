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

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Nodes/MissingPluginNode.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"

#include "Serialization/NodeSerialization.h"

NATRON_NAMESPACE_USING

namespace {

NodePtr
createPlaceholder(const AppInstancePtr& app,
                  const Serialization::NodeSerializationPtr& serialization)
{
    CreateNodeArgs args(PLUGINID_NATRON_MISSING_PLUGIN, app->getProject());

    args.setProperty<Serialization::NodeSerializationPtr>(kCreateNodeArgsPropNodeSerialization, serialization);
    args.setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);

    return app->createNode(args);
}

Serialization::NodeSerializationPtr
makeSerialization()
{
    Serialization::NodeSerializationPtr serialization = std::make_shared<Serialization::NodeSerialization>();

    serialization->scriptName = "Vanished1";
    serialization->label = "Vanished1";
    serialization->pluginID = "com.example.NotInstalled";
    serialization->pluginMajorVersion = 3;
    serialization->pluginMinorVersion = 7;
    serialization->inputs["Fg"] = "Constant1";
    serialization->inputs["Bg"] = "Constant2";

    return serialization;
}

} // namespace

TEST(MissingPluginNodeRegistration, IsRegisteredInternalOnlyAndNotAReaderOrWriter)
{
    const PluginsMap& plugins = appPTR->getPluginsList();
    PluginsMap::const_iterator it = plugins.find(PLUGINID_NATRON_MISSING_PLUGIN);

    ASSERT_TRUE(it != plugins.end());
    ASSERT_EQ(1u, it->second.size());

    const Plugin* plugin = *it->second.begin();

    EXPECT_TRUE(plugin->getIsForInternalUseOnly());
    EXPECT_FALSE(plugin->isReader());
    EXPECT_FALSE(plugin->isWriter());
}

TEST_F(BaseTest, MissingPluginNodeExposesStoredInputsAndErrors)
{
    const Serialization::NodeSerializationPtr serialization = makeSerialization();
    NodePtr node = createPlaceholder(getApp(), serialization);

    ASSERT_TRUE(node.get() != nullptr);
    EXPECT_EQ(std::string(PLUGINID_NATRON_MISSING_PLUGIN), node->getPluginID());
    ASSERT_EQ(2, node->getNInputs());
    EXPECT_EQ("Bg", node->getInputLabel(0));
    EXPECT_EQ("Fg", node->getInputLabel(1));

    EXPECT_TRUE(node->hasPersistentMessage());

    QString message;
    int type = 0;

    node->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeError, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("com.example.NotInstalled")));
    EXPECT_TRUE(message.contains(QString::fromUtf8("3.7")));

    std::shared_ptr<MissingPluginNode> placeholder = std::dynamic_pointer_cast<MissingPluginNode>(node->getEffectInstance());

    ASSERT_TRUE(placeholder.get() != nullptr);
    EXPECT_EQ(serialization, placeholder->getStoredSerialization());
}
