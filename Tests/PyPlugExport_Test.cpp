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

#include <string>
#include <vector>

#include <gtest/gtest.h>

CLANG_DIAG_OFF(deprecated)
#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QTextStream>
CLANG_DIAG_ON(deprecated)

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/LayerRegistry.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

namespace {

// The export writes the layer channels using the exact case-sensitive Python list syntax
// produced by NodeCollection::exportGroupToPython(), so the test checks for this literal.
const char* const kExpectedMaskLine = "app.addProjectLayer(\"mask\", [\"A\"])";

} // namespace

class PyPlugExportTest
    : public BaseTest {
protected:
    // Builds a Group node containing a Roto node whose target layer is `layerID`, and
    // returns the group.
    NodeGroupPtr buildGroupWithRotoTargeting(const std::string& layerID)
    {
        CreateNodeArgs groupArgs(PLUGINID_NATRON_GROUP, getApp()->getProject());
        NodePtr groupNode = getApp()->createNode(groupArgs);
        if (!groupNode) {
            return NodeGroupPtr();
        }
        NodeGroupPtr group = std::dynamic_pointer_cast<NodeGroup>(groupNode->getEffectInstance());
        if (!group) {
            return NodeGroupPtr();
        }
        NodeCollectionPtr collection = std::dynamic_pointer_cast<NodeCollection>(group);

        CreateNodeArgs rotoArgs(PLUGINID_NATRON_ROTO, collection);
        NodePtr roto = getApp()->createNode(rotoArgs);
        if (!roto) {
            return NodeGroupPtr();
        }
        KnobLayerSelectPtr layer = std::dynamic_pointer_cast<KnobLayerSelect>(roto->getLayerKnob());
        if (!layer) {
            return NodeGroupPtr();
        }
        layer->setLayer(layerID);

        return group;
    }
};

// A Roto node targeting a project-registered, non-built-in layer must have that layer
// recreated by the exported script, and the recreation must happen before any node is
// created so that later addProjectLayer() lookups and createNode() calls both see it.
TEST_F(PyPlugExportTest, CustomLayerReferenceEmitsAddProjectLayerBeforeNodeCreation)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    std::vector<std::string> channels(1, "A");
    std::string error;
    ASSERT_EQ(LayerRegistry::eAddResultAdded, project->addLayer(ImageLayerDesc("mask", "mask", "", channels), LayerRegistryEntry::eOriginUser, &error)) << error;

    NodeGroupPtr group = buildGroupWithRotoTargeting("mask");
    ASSERT_TRUE(bool(group));

    QString output;
    group->exportGroupToPython(QString::fromUtf8("test.pyplug.mask"), QString::fromUtf8("MaskGroup"), QString(), QString(), QString::fromUtf8("Other"), 1, output);

    QString maskLine = QString::fromUtf8(kExpectedMaskLine);
    EXPECT_EQ(1, output.count(maskLine));

    QString rotoCreation = QString::fromUtf8("\"") + QString::fromUtf8(PLUGINID_NATRON_ROTO) + QString::fromUtf8("\"");
    int maskLineIndex = output.indexOf(maskLine);
    int rotoCreationIndex = output.indexOf(rotoCreation);
    ASSERT_NE(-1, maskLineIndex);
    ASSERT_NE(-1, rotoCreationIndex);
    EXPECT_LT(maskLineIndex, rotoCreationIndex);

    project->reset(false, true);
}

// A Roto node left on its default Color target (a built-in layer) has nothing for the
// exported script to recreate.
TEST_F(PyPlugExportTest, BuiltInLayerReferenceEmitsNoAddProjectLayer)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    NodeGroupPtr group = buildGroupWithRotoTargeting(kNatronColorLayerID);
    ASSERT_TRUE(bool(group));

    QString output;
    group->exportGroupToPython(QString::fromUtf8("test.pyplug.color"), QString::fromUtf8("ColorGroup"), QString(), QString(), QString::fromUtf8("Other"), 1, output);

    EXPECT_FALSE(output.contains(QString::fromUtf8("addProjectLayer")));

    project->reset(false, true);
}

// Round-trip: the exported script is handed to the embedded interpreter exactly like a
// loaded PyPlug is (see AppInstance::createNodeFromPythonModule(), which runs
// "<module>.createInstance(appN, appN.<container>)" against an already-created, empty
// container group). A full round-trip through AppManager's PyPlug file-discovery
// (loadPythonGroups()) is impractical here: that scan runs once at AppManager
// construction and the discovery entry point is private, so a test can't register a
// freshly-written .py file as a plugin without restarting the whole Tests binary. Running
// createInstance() against a real container node is the practical equivalent: it proves
// the addProjectLayer() call the export emitted actually re-populates the registry when
// executed by Python, ahead of the createNode() calls that depend on it.
TEST_F(PyPlugExportTest, ExportedAddProjectLayerCallRepopulatesRegistryOnLoad)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    std::vector<std::string> channels(1, "A");
    std::string error;
    ASSERT_EQ(LayerRegistry::eAddResultAdded, project->addLayer(ImageLayerDesc("mask", "mask", "", channels), LayerRegistryEntry::eOriginUser, &error)) << error;

    NodeGroupPtr group = buildGroupWithRotoTargeting("mask");
    ASSERT_TRUE(bool(group));

    QString output;
    group->exportGroupToPython(QString::fromUtf8("test.pyplug.mask"), QString::fromUtf8("MaskGroup"), QString(), QString(), QString::fromUtf8("Other"), 1, output);
    ASSERT_TRUE(output.contains(QString::fromUtf8(kExpectedMaskLine)));

    // Also exercise the "write to a temp .py file" leg of the round-trip, even though the
    // file is read back in-process rather than discovered by AppManager.
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString scriptPath = tmpDir.path() + QString::fromUtf8("/MaskGroup.py");
    {
        QFile f(scriptPath);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream ts(&f);
        ts << output;
    }

    // Start over: a fresh project has neither the "mask" layer nor any nodes.
    project->reset(false, true);
    ImageLayerDesc unused;
    ASSERT_FALSE(project->findLayer("mask", &unused));

    QFile scriptFile(scriptPath);
    ASSERT_TRUE(scriptFile.open(QIODevice::ReadOnly | QIODevice::Text));
    std::string scriptText = QTextStream(&scriptFile).readAll().toStdString();

    std::string interpError, interpOutput;
    ASSERT_TRUE(interpretPythonScript(scriptText, &interpError, &interpOutput)) << interpError;

    CreateNodeArgs containerArgs(PLUGINID_NATRON_GROUP, getApp()->getProject());
    containerArgs.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);
    NodePtr container = getApp()->createNode(containerArgs);
    ASSERT_TRUE(bool(container));

    std::string appVar = getApp()->getAppIDString();
    std::string callScript = "createInstance(" + appVar + ", " + appVar + "." + container->getFullyQualifiedName() + ")\n";
    ASSERT_TRUE(interpretPythonScript(callScript, &interpError, &interpOutput)) << interpError;

    ImageLayerDesc found;
    ASSERT_TRUE(project->findLayer("mask", &found));
    EXPECT_EQ(std::vector<std::string>(1, "A"), found.getChannels());

    project->reset(false, true);
}
