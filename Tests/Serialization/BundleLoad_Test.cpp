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

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "ProjectEquivalence.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoItem.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoPoint.h"
#include "Engine/RotoStrokeItem.h"

#include "Serialization/Bundle/ProjectBundle.h"
#include "Serialization/Bundle/ZipStore.h"
#include "Serialization/Yaml/ManifestYaml.h"
#include "Serialization/Yaml/PresetYaml.h"
#include "Serialization/Yaml/ProjectGuiYaml.h"
#include "Serialization/Yaml/YamlScalars.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kOldPresetXml = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\" ?>\n"
                                  "<!DOCTYPE boost_serialization>\n"
                                  "<boost_serialization signature=\"serialization::archive\" version=\"20\">\n"
                                  "<Nodes class_id=\"0\" tracking_level=\"0\" version=\"0\"><count>1</count></Nodes>\n"
                                  "</boost_serialization>\n";

const char* const kOldLayoutXml = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\" ?>\n"
                                  "<!DOCTYPE boost_serialization>\n"
                                  "<boost_serialization signature=\"serialization::archive\" version=\"20\">\n"
                                  "<Layout class_id=\"0\" tracking_level=\"0\" version=\"2\"><_windows><count>0</count></_windows></Layout>\n"
                                  "</boost_serialization>\n";

std::span<const std::byte>
textBytes(std::string_view text)
{
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}

std::string
readFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        ADD_FAILURE() << "Cannot read " << path;

        return std::string();
    }

    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

bool
writeFile(const QString& path,
          const std::string& bytes)
{
    std::ofstream file(path.toStdString(), std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), (std::streamsize)bytes.size());
    if (!file) {
        ADD_FAILURE() << "Cannot write " << path.toStdString();

        return false;
    }

    return true;
}

std::string
readBundleFixture(const char* name)
{
    return readFile(std::string(NATRON_TESTS_FIXTURES_DIR "/bundle/") + name);
}

// The text with its first occurrence of from replaced; a missing from fails the test, so that a
// fixture edit cannot silently turn a case into a copy of another.
std::string
replacedOnce(const std::string& text,
             const std::string& from,
             const std::string& to)
{
    const std::size_t at = text.find(from);
    if (at == std::string::npos) {
        ADD_FAILURE() << "\"" << from << "\" is not in the fixture";

        return text;
    }
    std::string result = text;
    result.replace(at, from.size(), to);

    return result;
}

// graph.yaml with its top-level nodes written in the given order. nodes is the last section of
// the fixture and each of its nodes starts on a line indented by exactly two spaces.
std::string
withTopLevelNodesInOrder(const std::string& text,
                         const std::vector<std::string>& order)
{
    const std::string header = "\nnodes:\n";
    const std::size_t headerAt = text.find(header);
    if (headerAt == std::string::npos) {
        ADD_FAILURE() << "the fixture has no nodes section";

        return std::string();
    }
    const std::size_t bodyAt = headerAt + header.size();

    std::map<std::string, std::string> blocks;
    std::string current;
    std::istringstream lines(text.substr(bodyAt));
    std::string line;
    while (std::getline(lines, line)) {
        if ((line.size() > 2) && (line.compare(0, 2, "  ") == 0) && (line[2] != ' ')) {
            current = line.substr(2, line.find(':') - 2);
        } else if (current.empty() || line.empty() || (line[0] != ' ')) {
            ADD_FAILURE() << "unexpected line in the nodes section: \"" << line << "\"";

            return std::string();
        }
        blocks[current] += line + '\n';
    }

    std::string result = text.substr(0, bodyAt);
    for (const std::string& name : order) {
        std::map<std::string, std::string>::iterator found = blocks.find(name);
        if (found == blocks.end()) {
            ADD_FAILURE() << "the fixture has no top-level node " << name;

            return std::string();
        }
        result += found->second;
        blocks.erase(found);
    }
    if (!blocks.empty()) {
        ADD_FAILURE() << "the order leaves out " << blocks.begin()->first;

        return std::string();
    }

    return result;
}

std::size_t
findEndOfCentralDirectory(const std::string& bytes)
{
    const std::string signature("PK\x05\x06", 4);

    return bytes.rfind(signature);
}

std::uint32_t
readU32(const std::string& bytes,
        std::size_t offset)
{
    std::uint32_t value = 0;
    for (int i = 3; i >= 0; --i) {
        value = (value << 8) | (std::uint8_t)bytes[offset + i];
    }

    return value;
}

std::string
joined(const std::vector<std::string>& lines)
{
    std::string text;
    for (const std::string& line : lines) {
        text += "\n  " + line;
    }

    return text;
}

bool
containsAll(const std::string& text,
            std::initializer_list<const char*> parts)
{
    for (const char* part : parts) {
        if (text.find(part) == std::string::npos) {
            return false;
        }
    }

    return true;
}

// Dialogs::errorDialog writes to std::cerr in background mode and an AppInstance without a GUI
// writes to std::cout, so both are taken.
class ScopedOutputCapture {
public:
    ScopedOutputCapture()
        : _text()
        , _oldOut(std::cout.rdbuf(_text.rdbuf()))
        , _oldErr(std::cerr.rdbuf(_text.rdbuf()))
    {
    }

    ~ScopedOutputCapture()
    {
        std::cout.rdbuf(_oldOut);
        std::cerr.rdbuf(_oldErr);
    }

    std::string text() const
    {
        return _text.str();
    }

private:
    std::ostringstream _text;
    std::streambuf* _oldOut;
    std::streambuf* _oldErr;
};

} // anonymous namespace

class BundleLoadTest
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

    // Packs projectYaml into a bundle the way a save lays one out, with a default manifest.
    QString writeBundle(const QTemporaryDir& tmp,
                        const QString& fileName,
                        const std::string& projectYaml)
    {
        const QString path = tmp.filePath(fileName);
        try {
            Serialization::ZipWriter writer(path.toStdString());
            const std::string manifest = Serialization::encodeManifestYaml(Serialization::BundleManifest());
            writer.addMember(Serialization::kBundleManifestMember, textBytes(manifest));
            writer.addMember(Serialization::kBundleProjectMember, textBytes(projectYaml));
            writer.finish();
        } catch (const std::exception& e) {
            ADD_FAILURE() << "Cannot write " << path.toStdString() << ": " << e.what();
        }

        return path;
    }

    // Loads the file at path into the project; output receives what the load printed, where a
    // refused file's message goes.
    bool loadFile(const QString& path,
                  std::string* output)
    {
        const QFileInfo info(path);
        ScopedOutputCapture capture;
        const bool loaded = getApp()->getProject()->loadProject(info.absolutePath() + QLatin1Char('/'), info.fileName());
        *output = capture.text();

        return loaded;
    }

    std::vector<std::string> loadWarnings() const
    {
        std::vector<std::string> warnings;
        const QStringList list = getApp()->getProject()->getLoadWarnings();
        for (const QString& warning : list) {
            warnings.push_back(warning.toStdString());
        }

        return warnings;
    }

    NodePtr node(const std::string& fullyQualifiedName) const
    {
        return getApp()->getNodeByFullySpecifiedName(fullyQualifiedName);
    }

    // Loads text as a bundle, expecting the load to be refused with a message holding expected.
    void expectRefused(const QTemporaryDir& tmp,
                       const QString& fileName,
                       const std::string& bytes,
                       const char* expected)
    {
        const QString path = tmp.filePath(fileName);
        ASSERT_TRUE(writeFile(path, bytes));
        std::string output;
        EXPECT_FALSE(loadFile(path, &output)) << fileName.toStdString();
        EXPECT_NE(std::string::npos, output.find(expected)) << fileName.toStdString() << " printed:\n"
                                                            << output;
    }
};

TEST_F(BundleLoadTest, NodeOrderDoesNotChangeTheLoadedProject)
{
    const std::string graph = readBundleFixture("graph.yaml");
    ASSERT_FALSE(graph.empty());
    const std::vector<std::vector<std::string>> orders = {
        { "Blur1", "Constant1", "Grade1", "Group1" },
        { "Group1", "Grade1", "Constant1", "Blur1" },
        { "Grade1", "Blur1", "Group1", "Constant1" },
    };

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ProjectPtr project = getApp()->getProject();
    std::optional<ProjectSnapshot> first;
    for (std::size_t i = 0; i < orders.size(); ++i) {
        SCOPED_TRACE("order " + std::to_string(i));
        const std::string text = withTopLevelNodesInOrder(graph, orders[i]);
        ASSERT_FALSE(text.empty());
        const QString path = writeBundle(tmp, QString::fromUtf8("order-%1.ntpz").arg((int)i), text);

        std::string output;
        ASSERT_TRUE(loadFile(path, &output)) << output;
        EXPECT_TRUE(loadWarnings().empty()) << joined(loadWarnings());

        const ProjectSnapshot snapshot = captureProjectSnapshot(project);
        if (first) {
            EXPECT_TRUE(projectsAreEquivalent(*first, snapshot));
            continue;
        }
        first = snapshot;

        // What the fixture says arrived, so that equal snapshots mean something.
        const std::map<std::string, std::string>& entries = snapshot.entries;
        auto entry = [&entries](const std::string& key) {
            std::map<std::string, std::string>::const_iterator found = entries.find(key);

            return (found == entries.end()) ? std::string("<absent>") : found->second;
        };
        EXPECT_EQ("Grade1", entry("node Blur1 input 0 Source"));
        EXPECT_EQ("Constant1", entry("node Grade1 input 0 Source"));
        EXPECT_EQ("Blur1", entry("node Group1 input 0 1"));
        EXPECT_EQ("Group1.Blur2", entry("node Group1.Output1 input 0 Source"));
        EXPECT_EQ("Blur1.size.1", entry("node Group1.Blur2 knob size[0] link group"));
        EXPECT_EQ("Group1.Blur2.size.0", entry("node Blur1 knob size[1] link group"));
        EXPECT_EQ("Grade1.multiply.getValue(1) * 2", entry("node Blur1 knob size[0] expression"));
        EXPECT_EQ(0u, entry("node Grade1 knob multiply[0] curve").rfind("3 keys", 0)) << entry("node Grade1 knob multiply[0] curve");
        EXPECT_EQ(0u, entry("format 0").rfind("BundleSquare [0 0 64 64]", 0)) << entry("format 0");
        EXPECT_NE(std::string("<absent>"), entry("layer zdepth"));
        EXPECT_EQ("7", entry("timeline current"));
    }
}

TEST_F(BundleLoadTest, AMissingInputTargetIsDisconnectedWithOneWarning)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = writeBundle(tmp, QString::fromUtf8("missing-input.ntpz"), readBundleFixture("missing-input.yaml"));

    std::string output;
    ASSERT_TRUE(loadFile(path, &output)) << output;

    const std::vector<std::string> warnings = loadWarnings();
    ASSERT_EQ(1u, warnings.size()) << joined(warnings);
    EXPECT_TRUE(containsAll(warnings.front(), { "Blur1", "Source", "Missing1", "node not found" })) << warnings.front();

    NodePtr blur = node("Blur1");
    NodePtr grade = node("Grade1");
    ASSERT_TRUE(blur && grade);
    EXPECT_FALSE(bool(blur->getInput(0)));
    EXPECT_EQ(node("Constant1"), grade->getInput(0));
}

TEST_F(BundleLoadTest, AnUnknownInputLabelWarns)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = writeBundle(tmp, QString::fromUtf8("unknown-input.ntpz"), readBundleFixture("unknown-input.yaml"));

    std::string output;
    ASSERT_TRUE(loadFile(path, &output)) << output;

    const std::vector<std::string> warnings = loadWarnings();
    ASSERT_EQ(1u, warnings.size()) << joined(warnings);
    EXPECT_TRUE(containsAll(warnings.front(), { "Blur1", "Bogus", "Constant1", "no such input" })) << warnings.front();

    NodePtr blur = node("Blur1");
    ASSERT_TRUE(bool(blur));
    EXPECT_EQ(node("Grade1"), blur->getInput(0));
}

TEST_F(BundleLoadTest, MissingExpressionAndLinkTargetsWarnAndTheProjectStillLoads)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = writeBundle(tmp, QString::fromUtf8("missing-references.ntpz"), readBundleFixture("missing-references.yaml"));

    std::string output;
    ASSERT_TRUE(loadFile(path, &output)) << output;

    const std::vector<std::string> warnings = loadWarnings();
    ASSERT_EQ(2u, warnings.size()) << joined(warnings);
    int expressionWarnings = 0;
    int linkWarnings = 0;
    for (const std::string& warning : warnings) {
        if (containsAll(warning, { "Blur1.size", "expression could not be evaluated" })) {
            ++expressionWarnings;
        } else if (containsAll(warning, { "Blur2.size", "Missing3", "link target not found" })) {
            ++linkWarnings;
        }
    }
    EXPECT_EQ(1, expressionWarnings) << joined(warnings);
    EXPECT_EQ(1, linkWarnings) << joined(warnings);

    NodePtr blur1 = node("Blur1");
    NodePtr blur2 = node("Blur2");
    NodePtr constant = node("Constant1");
    ASSERT_TRUE(blur1 && blur2 && constant);
    EXPECT_EQ(constant, blur1->getInput(0));
    EXPECT_EQ(blur1, blur2->getInput(0));

    KnobIPtr expressionSize = blur1->getKnobByName("size");
    ASSERT_TRUE(bool(expressionSize));
    EXPECT_EQ("Missing2.size.getValue(0) * 2", expressionSize->getExpression(0));
    KnobIPtr linkedSize = blur2->getKnobByName("size");
    ASSERT_TRUE(bool(linkedSize));
    EXPECT_FALSE(bool(linkedSize->getMaster(0).second));

    Knob<double>* color = dynamic_cast<Knob<double>*>(constant->getKnobByName("color").get());
    ASSERT_TRUE(color != nullptr);
    EXPECT_EQ(0.75, color->getValue(2));
}

TEST_F(BundleLoadTest, UnknownKeysWarnAndAreIgnored)
{
    const std::string graph = readBundleFixture("graph.yaml");
    ASSERT_FALSE(graph.empty());
    std::string withUnknownKeys = replacedOnce(graph, "formatVersion: 1\n", "formatVersion: 1\nfutureTopLevelKey: {a: 1}\n");
    withUnknownKeys = replacedOnce(withUnknownKeys, "timeline: {current: 7}", "timeline: {current: 7, futureTimelineKey: true}");
    withUnknownKeys = replacedOnce(withUnknownKeys, "    plugin: \"Constant\"\n", "    plugin: \"Constant\"\n    futureNodeKey: 3\n");
    withUnknownKeys = replacedOnce(withUnknownKeys, "{value: 4, linkGroup: \"Blur1.size.1\"}", "{value: 4, linkGroup: \"Blur1.size.1\", futureLinkKey: 0}");

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ProjectPtr project = getApp()->getProject();
    std::string output;

    ASSERT_TRUE(loadFile(writeBundle(tmp, QString::fromUtf8("graph.ntpz"), graph), &output)) << output;
    const ProjectSnapshot expected = captureProjectSnapshot(project);

    ASSERT_TRUE(loadFile(writeBundle(tmp, QString::fromUtf8("unknown-keys.ntpz"), withUnknownKeys), &output)) << output;
    const std::vector<std::string> warnings = loadWarnings();
    ASSERT_EQ(4u, warnings.size()) << joined(warnings);
    for (const char* key : { "futureTopLevelKey", "futureTimelineKey", "futureNodeKey", "futureLinkKey" }) {
        int found = 0;
        for (const std::string& warning : warnings) {
            if (containsAll(warning, { "unknown key", key })) {
                ++found;
            }
        }
        EXPECT_EQ(1, found) << key << joined(warnings);
    }
    EXPECT_TRUE(projectsAreEquivalent(expected, captureProjectSnapshot(project)));
}

TEST_F(BundleLoadTest, ANewerFormatVersionIsRefused)
{
    const std::string graph = readBundleFixture("graph.yaml");
    ASSERT_FALSE(graph.empty());
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = writeBundle(tmp, QString::fromUtf8("newer.ntpz"), replacedOnce(graph, "formatVersion: 1\n", "formatVersion: 99\n"));

    std::string output;
    EXPECT_FALSE(loadFile(path, &output));
    EXPECT_NE(std::string::npos, output.find("newer Natron")) << output;
    EXPECT_FALSE(bool(node("Blur1")));
}

TEST_F(BundleLoadTest, ATruncatedOrCorruptCentralDirectoryIsADamagedFile)
{
    const std::string graph = readBundleFixture("graph.yaml");
    ASSERT_FALSE(graph.empty());
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string bundle = readFile(writeBundle(tmp, QString::fromUtf8("intact.ntpz"), graph).toStdString());
    const std::size_t end = findEndOfCentralDirectory(bundle);
    ASSERT_NE(std::string::npos, end);
    ASSERT_GE(bundle.size(), end + 22);

    expectRefused(tmp, QString::fromUtf8("truncated.ntpz"), bundle.substr(0, bundle.size() - 10), "damaged project file");

    std::string corrupt = bundle;
    const std::uint32_t directoryOffset = readU32(bundle, end + 16);
    ASSERT_LT((std::size_t)directoryOffset + 4, end);
    corrupt.replace(directoryOffset, 4, "XXXX");
    expectRefused(tmp, QString::fromUtf8("corrupt-directory.ntpz"), corrupt, "damaged project file");
}

TEST_F(BundleLoadTest, AMissingBlobWarnsAndLeavesItsStrokeEmpty)
{
    NodePtr reader = createWorkingSpaceRead(std::string(NATRON_TESTS_FIXTURES_DIR "/flat-rgba-only.exr"));
    NodePtr rotoPaint = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTOPAINT));
    ASSERT_TRUE(reader && rotoPaint);
    connectNodes(reader, rotoPaint, 0, true);
    RotoContextPtr context = rotoPaint->getRotoContext();
    ASSERT_TRUE(bool(context));

    // RotoContext::makeStroke() inserts the item before creating its nodes and null-derefs the
    // merge node it lacks; this is how RotoPaintInteract::makeStroke() builds one instead.
    RotoStrokeItemPtr stroke(new RotoStrokeItem(eRotoStrokeTypeSolid, context, "Stroke1", RotoLayerPtr()));
    stroke->createNodes();
    context->addItem(context->getOrCreateBaseLayer(), 0, stroke, RotoItem::eSelectionReasonOther);
    // Enough samples for the stroke table to be a blob member rather than inline.
    for (int i = 0; i < 80; ++i) {
        stroke->appendPoint(i == 0, RotoPoint(1. + i * 0.075, 2. + (i % 7) * 0.5, 0.25 + (i % 4) * 0.25, 1. + i));
    }
    stroke->setStrokeFinished();
    context->clearSelection(RotoItem::eSelectionReasonOther);
    const std::string strokeName = stroke->getScriptName();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QString savedPath;
    ASSERT_TRUE(getApp()->getProject()->saveProject(tmp.path() + QLatin1Char('/'), QString::fromUtf8("complete.ntpz"), &savedPath));

    const QString damagedPath = tmp.filePath(QString::fromUtf8("missing-blob.ntpz"));
    std::string strokeBlob;
    try {
        Serialization::ZipReader source(savedPath.toStdString());
        for (const Serialization::ZipMember& member : source.members()) {
            if (member.name.ends_with(".stroke.bin")) {
                EXPECT_TRUE(strokeBlob.empty()) << "a second stroke blob " << member.name;
                strokeBlob = member.name.substr(member.name.find('/') + 1);
            }
        }
        ASSERT_FALSE(strokeBlob.empty()) << "the saved bundle has no stroke blob member";
        Serialization::ZipWriter copy(damagedPath.toStdString(), Serialization::kNatronProjectMimeType, "missing-blob");
        for (const Serialization::ZipMember& member : source.members()) {
            const std::string relative = member.name.substr(member.name.find('/') + 1);
            if ((relative != "mimetype") && (relative != strokeBlob)) {
                copy.addMember(relative, source.data(member));
            }
        }
        copy.finish();
    } catch (const std::exception& e) {
        FAIL() << "Cannot rewrite " << savedPath.toStdString() << ": " << e.what();
    }

    std::string output;
    ASSERT_TRUE(loadFile(damagedPath, &output)) << output;

    const std::vector<std::string> warnings = loadWarnings();
    ASSERT_EQ(1u, warnings.size()) << joined(warnings);
    EXPECT_TRUE(containsAll(warnings.front(), { strokeBlob.c_str(), "is missing" })) << warnings.front();

    NodePtr loadedRotoPaint = node(rotoPaint->getFullyQualifiedName());
    ASSERT_TRUE(bool(loadedRotoPaint));
    RotoContextPtr loadedContext = loadedRotoPaint->getRotoContext();
    ASSERT_TRUE(bool(loadedContext));
    RotoStrokeItemPtr loadedStroke = std::dynamic_pointer_cast<RotoStrokeItem>(loadedContext->getItemByName(strokeName));
    ASSERT_TRUE(bool(loadedStroke));
    EXPECT_TRUE(loadedStroke->getXControlPoints().empty());
}

TEST_F(BundleLoadTest, AMultiInstanceTrackerReopensWithItsInstancesAttached)
{
    // TrackerPM is a deprecated, non-user-creatable plug-in, so creation must opt in to it.
    CreateNodeArgs mainArgs(PLUGINID_OFX_TRACKERPM, getApp()->getProject());
    mainArgs.setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);
    NodePtr main = getApp()->createNode(mainArgs);
    ASSERT_TRUE(bool(main));
    ASSERT_TRUE(main->isMultiInstance()) << "TrackerPM did not open in the tracker context";
    std::vector<std::string> instanceNames;
    for (int i = 0; i < 2; ++i) {
        CreateNodeArgs args(PLUGINID_OFX_TRACKERPM, getApp()->getProject());
        args.setProperty<std::string>(kCreateNodeArgsPropMultiInstanceParentName, main->getScriptName());
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
        args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
        args.setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);
        NodePtr instance = getApp()->createNode(args);
        ASSERT_TRUE(bool(instance));
        ASSERT_EQ(main, instance->getParentMultiInstance());
        instanceNames.push_back(instance->getScriptName());
    }
    const std::string mainName = main->getScriptName();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QString savedPath;
    ASSERT_TRUE(getApp()->getProject()->saveProject(tmp.path() + QLatin1Char('/'), QString::fromUtf8("tracker.ntpz"), &savedPath));
    getApp()->getProject()->reset(false, true);

    std::string output;
    ASSERT_TRUE(loadFile(savedPath, &output)) << output;
    EXPECT_TRUE(loadWarnings().empty()) << joined(loadWarnings());

    const auto byScriptName = [this](const std::string& name) {
        const NodesList nodes = getApp()->getProject()->getNodes();
        for (const NodePtr& candidate : nodes) {
            if (candidate->getScriptName() == name) {
                return candidate;
            }
        }

        return NodePtr();
    };
    NodePtr loadedMain = byScriptName(mainName);
    ASSERT_TRUE(bool(loadedMain));
    EXPECT_TRUE(loadedMain->isMultiInstance());
    EXPECT_FALSE(bool(loadedMain->getParentMultiInstance()));
    NodesList loadedInstances;
    loadedMain->getChildrenMultiInstance(&loadedInstances);
    EXPECT_EQ(instanceNames.size(), loadedInstances.size());
    for (const std::string& name : instanceNames) {
        SCOPED_TRACE(name);
        NodePtr instance = byScriptName(name);
        ASSERT_TRUE(bool(instance));
        EXPECT_EQ(mainName, instance->getParentMultiInstanceName());
        EXPECT_EQ(loadedMain, instance->getParentMultiInstance());
    }
}

TEST_F(BundleLoadTest, TheOldXmlFormatsAreRefusedWithTheOldFormatMessage)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    std::string output;
    EXPECT_FALSE(loadFile(QString::fromUtf8(NATRON_TESTS_FIXTURES_DIR "/old-format.ntp"), &output));
    EXPECT_NE(std::string::npos, output.find("Legacy .ntp projects are not supported")) << output;

    // Presets and layouts are read only by the GUI (NodeSettingsPanel and Gui::importLayout),
    // which this binary does not link: what is checked here is the shared test those readers
    // run before showing their old-format message, and that the YAML decoders behind them
    // refuse the XML cleanly should it get past that test.
    const struct {
        const char* fileName;
        const char* text;
    } oldFiles[] = {
        { "old-preset.nps", kOldPresetXml },
        { "old-layout.nl", kOldLayoutXml },
    };
    for (const auto& oldFile : oldFiles) {
        SCOPED_TRACE(oldFile.fileName);
        const QString path = tmp.filePath(QString::fromUtf8(oldFile.fileName));
        ASSERT_TRUE(writeFile(path, oldFile.text));
        EXPECT_TRUE(Serialization::isXmlDocument(oldFile.text));
        EXPECT_EQ(Serialization::ProjectFileKind::eLegacyXml, Serialization::detectProjectFileKind(path.toStdString()));
        EXPECT_THROW(Serialization::decodePresetYaml(oldFile.text), Serialization::YamlError);
        EXPECT_THROW(Serialization::decodeGuiLayoutYaml(oldFile.text), Serialization::YamlError);

        EXPECT_FALSE(loadFile(path, &output));
        EXPECT_NE(std::string::npos, output.find("Legacy .ntp projects are not supported")) << output;
    }
}

TEST_F(BundleLoadTest, AGarbageFileIsUnrecognized)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    std::string garbage = "this is not a Natron project\n";
    for (int i = 0; i < 256; ++i) {
        garbage.push_back((char)((i * 37 + 11) & 0xff));
    }
    expectRefused(tmp, QString::fromUtf8("garbage.ntpz"), garbage, "garbage.ntpz is not a " NATRON_APPLICATION_NAME " project");
}
