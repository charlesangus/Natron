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

#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Color/Clamp.h"
#include "Engine/Nodes/Color/ColorCorrect.h"
#include "Engine/Nodes/Color/ColorLookup.h"
#include "Engine/Nodes/Color/ColorMathNode.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Color/Invert.h"
#include "Engine/Nodes/Color/Saturation.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Filter/EdgeDetect.h"
#include "Engine/Nodes/Filter/ErodeDilate.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Keyer/ChromaKeyer.h"
#include "Engine/Nodes/Keyer/Keyer.h"
#include "Engine/Nodes/Merge/Dissolve.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Nodes/Transform/Crop.h"
#include "Engine/Nodes/Transform/Position.h"
#include "Engine/Nodes/Transform/Reformat.h"
#include "Engine/Nodes/Transform/Transform.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

namespace {

// Every ID a native node has taken over from an OpenFX plug-in. NativePluginList_Test owns the
// authoritative list of what resolves to a native version; this one only decides which
// createNode() blocks of a PyPlug are audited.
const std::set<std::string>&
retiredIds()
{
    static const std::set<std::string> ids = {
        PLUGINID_NATRON_GRADE,
        PLUGINID_NATRON_COLORCORRECT,
        PLUGINID_NATRON_SATURATION,
        PLUGINID_NATRON_CLAMP,
        PLUGINID_NATRON_INVERT,
        PLUGINID_NATRON_ADD,
        PLUGINID_NATRON_MULTIPLY,
        PLUGINID_NATRON_GAMMA,
        PLUGINID_NATRON_NATIVE_MERGE,
        PLUGINID_NATRON_DISSOLVE,
        PLUGINID_NATRON_CONSTANT,
        PLUGINID_NATRON_SOLID,
        PLUGINID_NATRON_CHECKERBOARD,
        PLUGINID_NATRON_TRANSFORM,
        PLUGINID_NATRON_TRANSFORMMASKED,
        PLUGINID_NATRON_CROP,
        PLUGINID_NATRON_POSITION,
        PLUGINID_NATRON_REFORMAT,
        PLUGINID_NATRON_BLUR,
        PLUGINID_NATRON_KEYER,
        PLUGINID_NATRON_CHROMAKEYER,
        PLUGINID_NATRON_ERODE,
        PLUGINID_NATRON_DILATE,
        PLUGINID_NATRON_EDGEDETECT,
        PLUGINID_NATRON_COLORLOOKUP,
    };

    return ids;
}

struct AllowedName {
    const char* name;
    bool prefix;
    const char* reason;
};

// A PyPlug may name a parameter that exists only on the OpenFX plug-in. Each one is guarded
// in the exported script by "if param is not None", so its absence on the native node is
// harmless; the reason says why the native node has no counterpart.
const AllowedName kAllowedNames[] = {
    { "NatronOfxParamProcess", true,
      "Per-channel process toggles that the plug-in declared for the host's channel selector; "
      "the native node's channel set replaces them." },
};

bool
isAllowedName(const std::string& name)
{
    for (const AllowedName& allowed : kAllowedNames) {
        if (allowed.prefix ? (name.rfind(allowed.name, 0) == 0) : (name == allowed.name)) {
            return true;
        }
    }

    return false;
}

typedef std::set<std::pair<std::string, std::string>> ParamSet;

struct PyPlugAudit {
    std::map<std::string, std::string> idOfNode;
    std::map<std::string, ParamSet> paramsOfNode;
};

// Reads the three ways a bundled PyPlug names a parameter of one of its inner nodes: inside the
// node's own createNode() block, through the group<ScriptName> variable the script keeps for
// linking and expressions, and through thisGroup.<ScriptName>.<param> in an expression or a
// thisNode.getNode("<ScriptName>").getParam("<param>") in a paramChanged callback.
PyPlugAudit
auditScript(const QString& source)
{
    static const QRegularExpression createRe(QStringLiteral("createNode\\(\"([^\"]+)\""));
    static const QRegularExpression scriptNameRe(QStringLiteral("lastNode\\.setScriptName\\(\"([^\"]+)\""));
    static const QRegularExpression lastNodeParamRe(QStringLiteral("lastNode\\.getParam\\(\"([^\"]+)\""));
    static const QRegularExpression groupVarParamRe(QStringLiteral("\\bgroup(\\w+)\\.getParam\\(\"([^\"]+)\""));
    static const QRegularExpression exprRe(QStringLiteral("thisGroup\\.(\\w+)\\.(\\w+)\\.get"));
    static const QRegularExpression getNodeRe(QStringLiteral("getNode\\(\"([^\"]+)\"\\)\\.getParam\\(\"([^\"]+)\""));

    PyPlugAudit audit;
    const QStringList lines = source.split(QLatin1Char('\n'));

    std::string currentId;
    std::string currentName;
    for (const QString& line : lines) {
        QRegularExpressionMatch match = createRe.match(line);
        if (match.hasMatch()) {
            currentId = match.captured(1).toStdString();
            currentName.clear();
            continue;
        }
        if (line.contains(QStringLiteral("# End of node"))) {
            currentId.clear();
            currentName.clear();
            continue;
        }
        match = scriptNameRe.match(line);
        if (match.hasMatch()) {
            currentName = match.captured(1).toStdString();
            audit.idOfNode[currentName] = currentId;
            continue;
        }
        match = lastNodeParamRe.match(line);
        if (match.hasMatch() && !currentName.empty()) {
            audit.paramsOfNode[currentName].insert(std::make_pair(match.captured(1).toStdString(), std::string("createNode block")));
        }
    }

    for (const QString& line : lines) {
        QRegularExpressionMatchIterator it = groupVarParamRe.globalMatch(line);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            audit.paramsOfNode[match.captured(1).toStdString()].insert(std::make_pair(match.captured(2).toStdString(), std::string("link or expression")));
        }
        it = exprRe.globalMatch(line);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            audit.paramsOfNode[match.captured(1).toStdString()].insert(std::make_pair(match.captured(2).toStdString(), std::string("expression")));
        }
        it = getNodeRe.globalMatch(line);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            audit.paramsOfNode[match.captured(1).toStdString()].insert(std::make_pair(match.captured(2).toStdString(), std::string("callback")));
        }
    }

    return audit;
}

bool
readFile(const QString& path,
         QString* contents)
{
    QFile file(path);

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    *contents = QString::fromUtf8(file.readAll());

    return true;
}

bool
isPluginLoaded(const char* pluginID)
{
    try {
        return appPTR->getPluginBinary(QString::fromUtf8(pluginID), -1, -1, false) != NULL;
    } catch (const std::exception&) {
        return false;
    }
}

// fr.inria.SplitAndJoin is left out for the reason PyPlugInstantiate_Test gives; it creates no
// retired node anyway.
const char* const kAuditedPyPlugs[] = {
    "AngleBlur",
    "DropShadow",
    "LightWrap",
    "PIKColor",
    "ZMask",
    "ZRemap",
};

} // namespace

class PyPlugNativeParamsTest
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
};

TEST_F(PyPlugNativeParamsTest, EveryParamAPyPlugSetsOnANativeNodeExistsOnIt)
{
    const std::string firstId = std::string("fr.inria.") + kAuditedPyPlugs[0];
    if (!isPluginLoaded(firstId.c_str())) {
        std::cerr << "Skipping: PyPlugs under " NATRON_TESTS_PYPLUGS_DIR " were not registered as plugins." << std::endl;
        return;
    }

    std::ostringstream findings;
    int nMissing = 0;
    int nChecked = 0;

    for (const char* pyPlugName : kAuditedPyPlugs) {
        const std::string pluginID = std::string("fr.inria.") + pyPlugName;
        QString source;
        ASSERT_TRUE(readFile(QDir(QString::fromUtf8(NATRON_TESTS_PYPLUGS_DIR)).filePath(QString::fromUtf8(pyPlugName) + QStringLiteral(".py")), &source)) << pyPlugName;
        const PyPlugAudit audit = auditScript(source);

        NodePtr group = createNode(QString::fromStdString(pluginID));
        ASSERT_TRUE(bool(group)) << pluginID;
        NodeCollectionPtr collection = std::dynamic_pointer_cast<NodeCollection>(group->getEffectInstance());
        ASSERT_TRUE(bool(collection)) << pluginID;

        NodesList innerNodes;
        collection->getNodes_recursive(innerNodes, true);
        std::map<std::string, NodePtr> innerByName;
        for (const NodePtr& inner : innerNodes) {
            innerByName[inner->getScriptName()] = inner;
        }

        for (const auto& entry : audit.idOfNode) {
            const std::string& scriptName = entry.first;
            const std::string& expectedId = entry.second;
            if (retiredIds().find(expectedId) == retiredIds().end()) {
                continue;
            }
            const std::map<std::string, NodePtr>::const_iterator nodeIt = innerByName.find(scriptName);
            ASSERT_TRUE(nodeIt != innerByName.end()) << pyPlugName << ": inner node " << scriptName << " was not created";
            const NodePtr& node = nodeIt->second;
            EXPECT_EQ(expectedId, node->getPluginID()) << pyPlugName << "/" << scriptName;

            const std::map<std::string, ParamSet>::const_iterator paramsIt = audit.paramsOfNode.find(scriptName);
            if (paramsIt == audit.paramsOfNode.end()) {
                continue;
            }
            for (const auto& param : paramsIt->second) {
                if (isAllowedName(param.first)) {
                    continue;
                }
                ++nChecked;
                if (!node->getKnobByName(param.first)) {
                    ++nMissing;
                    findings << "  " << pyPlugName << ": node " << scriptName << " (" << expectedId << ") has no param \""
                             << param.first << "\" (" << param.second << ")\n";
                }
            }
        }
    }

    EXPECT_GT(nChecked, 0) << "no parameter of a retired node was audited";
    EXPECT_EQ(0, nMissing) << "Params the bundled PyPlugs set on a native node that the node lacks:\n"
                           << findings.str();
}

// The PyPlugs' inner Merges were saved writing some channels of rgba, the rest passing through from
// B: each must have that one rgba row in Layers with those channels on.
TEST_F(PyPlugNativeParamsTest, InnerMergesWriteTheChannelsTheyWereSavedWith)
{
    if (!isPluginLoaded("fr.inria.Glow")) {
        std::cerr << "Skipping: PyPlugs under " NATRON_TESTS_PYPLUGS_DIR " were not registered as plugins." << std::endl;
        return;
    }
    struct Expected {
        const char* pyPlug;
        const char* merge;
        std::vector<std::string> channels;
    };
    const std::vector<std::string> rgb = { "R", "G", "B" };
    const Expected expected[] = {
        { "Glow", "Merge1", rgb },
        { "Glow", "Merge2", rgb },
        { "Glow", "Merge3", rgb },
        { "Glow", "Merge6", rgb },
        { "LightWrap", "EdgeMerge", { "A" } },
        { "LightWrap", "MaskMerge", rgb },
    };
    std::map<std::string, NodeCollectionPtr> groups;
    for (const Expected& want : expected) {
        NodeCollectionPtr& group = groups[want.pyPlug];
        if (!group) {
            NodePtr node = createNode(QString::fromUtf8("fr.inria.") + QString::fromUtf8(want.pyPlug));
            ASSERT_TRUE(bool(node)) << want.pyPlug;
            group = std::dynamic_pointer_cast<NodeCollection>(node->getEffectInstance());
            ASSERT_TRUE(bool(group)) << want.pyPlug;
        }
        NodePtr merge = group->getNodeByName(want.merge);
        ASSERT_TRUE(bool(merge)) << want.pyPlug << "/" << want.merge;
        ASSERT_EQ(std::string(PLUGINID_NATRON_NATIVE_MERGE), merge->getPluginID()) << want.pyPlug << "/" << want.merge;
        KnobChannelSetPtr layers = merge->getEffectInstance()->getKnobByNameAndType<KnobChannelSet>(kNativeMergeParamLayers);
        ASSERT_TRUE(bool(layers)) << want.pyPlug << "/" << want.merge;
        const std::vector<ChannelSetRow> rows = layers->getRows();
        ASSERT_EQ(1u, rows.size()) << want.pyPlug << "/" << want.merge;
        EXPECT_EQ(ChannelSetRow::eModeLayer, rows[0].mode) << want.pyPlug << "/" << want.merge;
        EXPECT_EQ(std::string(kNatronColorViewRGBA), rows[0].layerOrPattern) << want.pyPlug << "/" << want.merge;
        EXPECT_EQ(want.channels, rows[0].channels) << want.pyPlug << "/" << want.merge;
    }
}

// These colour inner nodes were saved with the OpenFX plug-in's un-premultiply toggle on. The
// native node has no such toggle, so the PyPlug has to set the host selector to alpha itself, or
// the node silently stops dividing by alpha.
TEST_F(PyPlugNativeParamsTest, InnerNodesSavedUnPremultipliedStillUnPremultiplyByAlpha)
{
    struct Expected {
        const char* pyPlug;
        const char* scriptName;
        const char* pluginID;
    };
    const Expected expected[] = {
        { "DropShadow", "Multiply1", PLUGINID_NATRON_MULTIPLY },
    };

    const std::string firstId = std::string("fr.inria.") + expected[0].pyPlug;
    if (!isPluginLoaded(firstId.c_str())) {
        std::cerr << "Skipping: PyPlugs under " NATRON_TESTS_PYPLUGS_DIR " were not registered as plugins." << std::endl;
        return;
    }

    for (const Expected& e : expected) {
        NodePtr group = createNode(QString::fromStdString(std::string("fr.inria.") + e.pyPlug));
        ASSERT_TRUE(bool(group)) << e.pyPlug;
        NodeCollectionPtr collection = std::dynamic_pointer_cast<NodeCollection>(group->getEffectInstance());
        ASSERT_TRUE(bool(collection)) << e.pyPlug;

        NodesList innerNodes;
        collection->getNodes_recursive(innerNodes, true);
        NodePtr inner;
        for (const NodePtr& candidate : innerNodes) {
            if (candidate->getScriptName() == e.scriptName) {
                inner = candidate;
                break;
            }
        }
        ASSERT_TRUE(bool(inner)) << e.pyPlug << "/" << e.scriptName;
        EXPECT_EQ(std::string(e.pluginID), inner->getPluginID()) << e.pyPlug << "/" << e.scriptName;

        KnobChannelSelectPtr unPremultBy = inner->getUnPremultBySelector();
        ASSERT_TRUE(bool(unPremultBy)) << e.pyPlug << "/" << e.scriptName;
        EXPECT_FALSE(unPremultBy->isNone()) << e.pyPlug << "/" << e.scriptName;
        EXPECT_EQ(std::string("rgba.A"), unPremultBy->get()) << e.pyPlug << "/" << e.scriptName;
    }
}

// The OpenFX dilates were saved un-premultiplying by alpha too, but the native filters work on
// premultiplied data as stored and offer no selector. Their source here has R/G/B at zero, so
// dividing by alpha first never changed what they produced.
TEST_F(PyPlugNativeParamsTest, FilterInnerNodesSavedUnPremultipliedFilterAsStored)
{
    const char* const pyPlug = "PIKColor";
    const char* const scriptNames[] = { "DilateFast1", "DilateFast2" };

    const std::string pluginID = std::string("fr.inria.") + pyPlug;
    if (!isPluginLoaded(pluginID.c_str())) {
        std::cerr << "Skipping: PyPlugs under " NATRON_TESTS_PYPLUGS_DIR " were not registered as plugins." << std::endl;
        return;
    }

    NodePtr group = createNode(QString::fromStdString(pluginID));
    ASSERT_TRUE(bool(group)) << pyPlug;
    NodeCollectionPtr collection = std::dynamic_pointer_cast<NodeCollection>(group->getEffectInstance());
    ASSERT_TRUE(bool(collection)) << pyPlug;

    NodesList innerNodes;
    collection->getNodes_recursive(innerNodes, true);
    for (const char* scriptName : scriptNames) {
        NodePtr inner;
        for (const NodePtr& candidate : innerNodes) {
            if (candidate->getScriptName() == scriptName) {
                inner = candidate;
                break;
            }
        }
        ASSERT_TRUE(bool(inner)) << pyPlug << "/" << scriptName;
        EXPECT_EQ(std::string(PLUGINID_NATRON_DILATE), inner->getPluginID()) << pyPlug << "/" << scriptName;
        EXPECT_FALSE(bool(inner->getUnPremultBySelector())) << pyPlug << "/" << scriptName;
        EXPECT_FALSE(bool(inner->getKnobByName(kUnPremultByKnobName))) << pyPlug << "/" << scriptName;
    }
}
