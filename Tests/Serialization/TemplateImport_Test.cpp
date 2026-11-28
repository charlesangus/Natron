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
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Project.h"
#include "Engine/ProjectTemplate.h"

#include "Serialization/Bundle/ProjectBundle.h"
#include "Serialization/NodeSerialization.h"
#include "Serialization/Ops/SnapshotDiff.h"
#include "Serialization/ProjectSerialization.h"
#include "Serialization/Sha256.h"
#include "Serialization/Yaml/BlobSink.h"
#include "Serialization/Yaml/ClipboardYaml.h"
#include "Serialization/Yaml/PresetYaml.h"
#include "Serialization/Yaml/ProjectYaml.h"

NATRON_NAMESPACE_USING

namespace {

std::string
memberText(const Serialization::ProjectBundleReader& bundle,
           const std::string& path)
{
    const std::optional<std::span<const std::byte>> bytes = bundle.findMember(path);
    if (!bytes) {
        return std::string();
    }

    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

} // namespace

TEST(TemplateProvenanceYaml, OriginsAndTemplateEntriesRoundTripWithTheBaseAsAMember)
{
    Serialization::ProjectSerialization base;
    Serialization::NodeSerializationPtr baseNode = std::make_shared<Serialization::NodeSerialization>();
    baseNode->scriptName = "Grade1";
    baseNode->label = "Grade1";
    baseNode->pluginID = "Grade";
    baseNode->id = "0123456789abcdef0123456789abcdef";
    base.nodes.nodes.push_back(baseNode);

    Serialization::ProjectSerialization shot;
    Serialization::NodeSerializationPtr node = std::make_shared<Serialization::NodeSerialization>();
    node->scriptName = "Grade2";
    node->label = "Grade2";
    node->pluginID = "Grade";
    node->id = "fedcba9876543210fedcba9876543210";
    node->origin.templateKey = "grade_tpl";
    node->origin.nodeId = baseNode->id;
    shot.nodes.nodes.push_back(node);
    Serialization::TemplateImportSerialization entry;
    entry.key = "grade_tpl";
    entry.templateId = "00112233445566778899aabbccddeeff";
    entry.path = "/shows/templates/grade_tpl.ntpz";
    entry.contentHash = Serialization::sha256Hex(std::string_view("abc"));
    entry.importedAt = "2026-11-20T10:00:00Z";
    entry.base = std::make_shared<Serialization::ProjectSerialization>(base);
    shot.templates.push_back(entry);

    Serialization::MemoryBlobStore store;
    const std::string text = Serialization::encodeProjectYaml(shot, store);
    EXPECT_NE(text.find("origin: {template: \"grade_tpl\", node: \"0123456789abcdef0123456789abcdef\"}"), std::string::npos) << text;
    ASSERT_TRUE(store.findMember(Serialization::templateBaseMember("grade_tpl")).has_value());

    Serialization::YamlWarnings warnings;
    const Serialization::ProjectSerialization decoded = Serialization::decodeProjectYaml(text, store, &warnings);
    EXPECT_TRUE(warnings.empty());
    ASSERT_EQ(decoded.nodes.nodes.size(), 1u);
    EXPECT_EQ(decoded.nodes.nodes.front()->origin, node->origin);
    ASSERT_EQ(decoded.templates.size(), 1u);
    const Serialization::TemplateImportSerialization& read = decoded.templates.front();
    EXPECT_EQ(read.key, entry.key);
    EXPECT_EQ(read.path, entry.path);
    EXPECT_EQ(read.contentHash, entry.contentHash);
    EXPECT_EQ(read.importedAt, entry.importedAt);
    ASSERT_TRUE(read.base != nullptr);
    ASSERT_EQ(read.base->nodes.nodes.size(), 1u);
    EXPECT_EQ(read.base->nodes.nodes.front()->id, baseNode->id);

    Serialization::MemoryBlobStore again;
    EXPECT_EQ(Serialization::encodeProjectYaml(decoded, again), text);
}

TEST(TemplateProvenanceYaml, ContentHashAndTemplateKeys)
{
    EXPECT_EQ(Serialization::sha256Hex(std::string_view("abc")), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_FALSE(Serialization::isValidTemplateKey("grade tpl"));
    EXPECT_FALSE(Serialization::isValidTemplateKey(""));
    EXPECT_TRUE(Serialization::isValidTemplateKey("grade_tpl_2"));
}

TEST(TemplateProvenanceYaml, TheClipboardCarriesAnOriginAndAPresetNever)
{
    Serialization::NodeSerializationPtr node = std::make_shared<Serialization::NodeSerialization>();
    node->scriptName = "Grade2";
    node->label = "Grade2";
    node->pluginID = "Grade";
    node->origin.templateKey = "grade_tpl";
    node->origin.nodeId = "0123456789abcdef0123456789abcdef";
    Serialization::ClipboardSerialization clipboard;
    clipboard.nodes.push_back(node);
    EXPECT_NE(Serialization::encodeClipboardYaml(clipboard).find("origin:"), std::string::npos);
    EXPECT_EQ(Serialization::encodePresetYaml(clipboard.nodes).find("origin:"), std::string::npos);
}

class TemplateImportTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        project()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        project()->reset(false, true);
        BaseTest::TearDown();
    }

    ProjectPtr project() const
    {
        return getApp()->getProject();
    }

    NodePtr create(const char* pluginID,
                   const std::string& name)
    {
        CreateNodeArgs args(pluginID, project());
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
        args.setProperty<std::string>(kCreateNodeArgsPropNodeInitialName, name);
        NodePtr node = getApp()->createNode(args);
        if (node && (node->getScriptName() != name)) {
            node->setScriptName(name);
        }

        return node;
    }

    NodePtr createFrom(const Serialization::NodeSerialization& serialization)
    {
        Serialization::NodeSerializationPtr copy = std::make_shared<Serialization::NodeSerialization>(serialization);
        copy->inputs.clear();
        CreateNodeArgs args(copy->pluginID, project());
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
        args.setProperty<bool>(kCreateNodeArgsPropSilent, true);
        args.setProperty<Serialization::NodeSerializationPtr>(kCreateNodeArgsPropNodeSerialization, copy);

        return getApp()->createNode(args);
    }

    // Grade1 -> Blur1, and Grade5 whose multiply follows Grade1's.
    QString saveTemplate(const QTemporaryDir& dir,
                         std::string* projectYaml,
                         std::string* gradeId)
    {
        NodePtr grade = create(PLUGINID_NATRON_GRADE, "Grade1");
        NodePtr blur = create(PLUGINID_NATRON_BLUR, "Blur1");
        NodePtr driven = create(PLUGINID_NATRON_GRADE, "Grade5");
        EXPECT_TRUE(grade != nullptr);
        EXPECT_TRUE(blur != nullptr);
        EXPECT_TRUE(driven != nullptr);
        if (!grade || !blur || !driven) {
            return QString();
        }
        EXPECT_TRUE(NodeCollection::connectNodes(0, grade, blur, false));
        KnobIPtr multiply = driven->getKnobByName("multiply");
        EXPECT_TRUE(multiply != nullptr);
        if (multiply) {
            multiply->setExpression(0, "Grade1.multiply.getValue(0)", false, false);
        }
        *gradeId = grade->getNodeId();
        QString saved;
        EXPECT_TRUE(project()->saveProject(dir.path() + QLatin1Char('/'), QString::fromUtf8("grade_tpl.ntpz"), &saved));
        Serialization::ProjectBundleReader bundle(saved.toStdString());
        *projectYaml = std::string(bundle.projectYaml());
        project()->reset(false, true);

        return saved;
    }

    static NodePtr named(const NodesList& nodes,
                         const std::string& name)
    {
        for (const NodePtr& node : nodes) {
            if (node->getScriptName() == name) {
                return node;
            }
        }

        return NodePtr();
    }
};

TEST_F(TemplateImportTest, ImportingTwiceGivesNewIdsThatShareTheTemplateNodeOrigin)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    std::string templateYaml;
    std::string templateGradeId;
    const QString templatePath = saveTemplate(dir, &templateYaml, &templateGradeId);
    ASSERT_FALSE(templatePath.isEmpty());

    NodePtr shotGrade = create(PLUGINID_NATRON_GRADE, "Grade1");
    ASSERT_TRUE(shotGrade != nullptr);

    QString error;
    const NodesList first = project()->importTemplate(templatePath, NodeCollectionPtr(), &error);
    EXPECT_TRUE(error.isEmpty()) << error.toStdString();
    ASSERT_EQ(first.size(), 3u);
    const NodesList second = project()->importTemplate(templatePath, NodeCollectionPtr(), &error);
    EXPECT_TRUE(error.isEmpty()) << error.toStdString();
    ASSERT_EQ(second.size(), 3u);

    const std::list<Serialization::TemplateImportSerialization>& imports = project()->getTemplateImports();
    ASSERT_EQ(imports.size(), 2u);
    EXPECT_EQ(imports.front().key, "grade_tpl");
    EXPECT_EQ(imports.back().key, "grade_tpl_2");
    EXPECT_EQ(imports.front().contentHash, Serialization::sha256Hex(templateYaml));

    NodePtr firstGrade = named(first, "Grade2");
    NodePtr secondGrade = named(second, "Grade3");
    ASSERT_TRUE(firstGrade != nullptr);
    ASSERT_TRUE(secondGrade != nullptr);
    EXPECT_NE(firstGrade->getNodeId(), templateGradeId);
    EXPECT_NE(firstGrade->getNodeId(), secondGrade->getNodeId());
    EXPECT_EQ(firstGrade->getOriginNode(), templateGradeId);
    EXPECT_EQ(secondGrade->getOriginNode(), templateGradeId);
    EXPECT_EQ(firstGrade->getOriginTemplate(), "grade_tpl");
    EXPECT_EQ(secondGrade->getOriginTemplate(), "grade_tpl_2");
    EXPECT_TRUE(shotGrade->getOriginTemplate().empty());

    NodePtr firstBlur = named(first, "Blur1");
    ASSERT_TRUE(firstBlur != nullptr);
    EXPECT_EQ(firstBlur->getInput(0), firstGrade);
    NodePtr firstDriven = named(first, "Grade5");
    NodePtr secondDriven = named(second, "Grade6");
    ASSERT_TRUE(firstDriven != nullptr);
    ASSERT_TRUE(secondDriven != nullptr);
    EXPECT_EQ(firstDriven->getKnobByName("multiply")->getExpression(0), "Grade2.multiply.getValue(0)");
    EXPECT_EQ(secondDriven->getKnobByName("multiply")->getExpression(0), "Grade3.multiply.getValue(0)");
}

TEST_F(TemplateImportTest, SaveAndLoadKeepOriginsAndTheByteIdenticalBase)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    std::string templateYaml;
    std::string templateGradeId;
    const QString templatePath = saveTemplate(dir, &templateYaml, &templateGradeId);
    ASSERT_FALSE(templatePath.isEmpty());

    QString error;
    const NodesList imported = project()->importTemplate(templatePath, NodeCollectionPtr(), &error);
    ASSERT_EQ(imported.size(), 3u);
    NodePtr grade = named(imported, "Grade1");
    ASSERT_TRUE(grade != nullptr);
    const std::string gradeId = grade->getNodeId();

    QString shotPath;
    ASSERT_TRUE(project()->saveProject(dir.path() + QLatin1Char('/'), QString::fromUtf8("shot.ntpz"), &shotPath));
    {
        Serialization::ProjectBundleReader bundle(shotPath.toStdString());
        EXPECT_EQ(memberText(bundle, Serialization::templateBaseMember("grade_tpl")), templateYaml);
    }

    ASSERT_TRUE(project()->loadProject(dir.path() + QLatin1Char('/'), QString::fromUtf8("shot.ntpz")));
    NodePtr loaded = project()->findNodeById(gradeId);
    ASSERT_TRUE(loaded != nullptr);
    EXPECT_EQ(loaded->getOriginTemplate(), "grade_tpl");
    EXPECT_EQ(loaded->getOriginNode(), templateGradeId);
    const std::list<Serialization::TemplateImportSerialization>& imports = project()->getTemplateImports();
    ASSERT_EQ(imports.size(), 1u);
    ASSERT_TRUE(imports.front().base != nullptr);
    EXPECT_EQ(imports.front().base->nodes.nodes.size(), 3u);

    QString resaved;
    ASSERT_TRUE(project()->saveProject(dir.path() + QLatin1Char('/'), QString::fromUtf8("shot2.ntpz"), &resaved));
    Serialization::ProjectBundleReader first(shotPath.toStdString());
    Serialization::ProjectBundleReader again(resaved.toStdString());
    const Serialization::ProjectSerialization before = Serialization::editableState(first.readProject());
    const Serialization::ProjectSerialization after = Serialization::editableState(again.readProject());
    EXPECT_TRUE(Serialization::diffSnapshots(before, after).empty());
    ASSERT_EQ(after.templates.size(), 1u);
    EXPECT_EQ(after.templates.front().contentHash, before.templates.front().contentHash);
    EXPECT_EQ(memberText(again, Serialization::templateBaseMember("grade_tpl")), templateYaml);
}

TEST_F(TemplateImportTest, ACopyDropsItsOriginWhileTheOriginalIsActive)
{
    NodePtr original = create(PLUGINID_NATRON_GRADE, "Grade1");
    ASSERT_TRUE(original != nullptr);
    original->setOrigin("grade_tpl", "0123456789abcdef0123456789abcdef");
    EXPECT_EQ(project()->findActiveNodeWithOrigin("grade_tpl", "0123456789abcdef0123456789abcdef"), original);

    Serialization::NodeSerialization serialization;
    original->toSerialization(&serialization);
    serialization.scriptName = "Grade1_copy";
    serialization.label = "Grade1_copy";
    NodePtr duplicate = createFrom(serialization);
    ASSERT_TRUE(duplicate != nullptr);
    EXPECT_TRUE(duplicate->getOriginTemplate().empty());

    original->deactivate(NodesList(), true, false, true, false, false);
    EXPECT_EQ(project()->findActiveNodeWithOrigin("grade_tpl", "0123456789abcdef0123456789abcdef"), NodePtr());
    serialization.scriptName = "Grade1_pasted";
    serialization.label = "Grade1_pasted";
    NodePtr pasted = createFrom(serialization);
    ASSERT_TRUE(pasted != nullptr);
    EXPECT_EQ(pasted->getOriginTemplate(), "grade_tpl");
    EXPECT_EQ(pasted->getOriginNode(), "0123456789abcdef0123456789abcdef");
}

TEST_F(TemplateImportTest, SwappingIdentitiesMovesTheIdAndTheOrigin)
{
    NodePtr a = create(PLUGINID_NATRON_GRADE, "Grade1");
    NodePtr b = create(PLUGINID_NATRON_GRADE, "Grade2");
    ASSERT_TRUE(a != nullptr);
    ASSERT_TRUE(b != nullptr);
    a->setOrigin("grade_tpl", "0123456789abcdef0123456789abcdef");
    const std::string aId = a->getNodeId();
    const std::string bId = b->getNodeId();

    Node::swapIdentity(a, b);
    EXPECT_EQ(a->getNodeId(), bId);
    EXPECT_EQ(b->getNodeId(), aId);
    EXPECT_EQ(project()->findNodeById(aId), b);
    EXPECT_EQ(project()->findNodeById(bId), a);
    EXPECT_TRUE(a->getOriginTemplate().empty());
    EXPECT_EQ(b->getOriginTemplate(), "grade_tpl");
    EXPECT_EQ(project()->findActiveNodeWithOrigin("grade_tpl", "0123456789abcdef0123456789abcdef"), b);

    Node::swapIdentity(a, b);
    EXPECT_EQ(project()->findNodeById(aId), a);
    EXPECT_EQ(project()->findActiveNodeWithOrigin("grade_tpl", "0123456789abcdef0123456789abcdef"), a);
}

TEST_F(TemplateImportTest, EachImportGetsATemplateNodeAndAPlainScriptIsVersionOne)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    std::string templateYaml;
    std::string templateGradeId;
    const QString templatePath = saveTemplate(dir, &templateYaml, &templateGradeId);
    ASSERT_FALSE(templatePath.isEmpty());

    QString error;
    ASSERT_EQ(project()->importTemplate(templatePath, NodeCollectionPtr(), &error).size(), 3u) << error.toStdString();
    ASSERT_EQ(project()->importTemplate(templatePath, NodeCollectionPtr(), &error).size(), 3u) << error.toStdString();
    const std::list<Serialization::TemplateImportSerialization> imports = project()->getTemplateImports();
    ASSERT_EQ(imports.size(), 2u);
    EXPECT_NE(imports.front().key, imports.back().key);
    EXPECT_FALSE(imports.front().templateId.empty());
    EXPECT_EQ(imports.front().version, 1);
    EXPECT_EQ(imports.front().name, "grade_tpl");
    EXPECT_EQ(imports.front().publishDir, QFileInfo(templatePath).absolutePath().toStdString());

    int templateNodes = 0;
    for (const NodePtr& node : project()->getNodes()) {
        if (node->getPluginID() == PLUGINID_NATRON_TEMPLATE) {
            ++templateNodes;
            EXPECT_TRUE(node->getOriginTemplate().empty());
        }
    }
    EXPECT_EQ(templateNodes, 2);
    EXPECT_NE(project()->findTemplateNode(imports.front().key), nullptr);
    EXPECT_NE(project()->findTemplateNode(imports.back().key), nullptr);
    EXPECT_NE(project()->findTemplateNode(imports.front().key), project()->findTemplateNode(imports.back().key));
}

TEST_F(TemplateImportTest, ImportingAPublishedVersionTwiceSharesItsTemplateId)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    NodePtr grade = create(PLUGINID_NATRON_GRADE, "Grade1");
    ASSERT_TRUE(grade != nullptr);
    TemplateEdit edit;
    ASSERT_TRUE(project()->defineTemplate({ grade }, "grade", NodeCollectionPtr(), &edit)) << edit.error.toStdString();
    Serialization::TemplateImportSerialization defined = project()->getTemplateImports().front();
    defined.publishDir = dir.path().toStdString();
    project()->setTemplateImport(defined);
    QString error;
    ASSERT_TRUE(project()->publishTemplate(edit.key, &error, &edit)) << error.toStdString();
    project()->reset(false, true);

    ASSERT_EQ(project()->importTemplate(edit.path, NodeCollectionPtr(), &error).size(), 1u) << error.toStdString();
    ASSERT_EQ(project()->importTemplate(edit.path, NodeCollectionPtr(), &error).size(), 1u) << error.toStdString();
    const std::list<Serialization::TemplateImportSerialization> imports = project()->getTemplateImports();
    ASSERT_EQ(imports.size(), 2u);
    EXPECT_NE(imports.front().key, imports.back().key);
    EXPECT_EQ(imports.front().templateId, defined.templateId);
    EXPECT_EQ(imports.back().templateId, defined.templateId);
    EXPECT_EQ(imports.front().version, 1);
    EXPECT_EQ(imports.front().name, "grade");
    for (const Serialization::TemplateImportSerialization& entry : imports) {
        ASSERT_TRUE(entry.base != nullptr);
        EXPECT_EQ(entry.base->nodes.nodes.size(), 1u);
    }
}
