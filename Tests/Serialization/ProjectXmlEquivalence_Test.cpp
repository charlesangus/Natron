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
#include <exception>
#include <list>
#include <memory>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include <QFile>
#include <QRegularExpression>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "SyntheticProjectGraphs.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Project.h"

#include "Serialization/BoostXml/NodeXml.h"
#include "Serialization/BoostXml/ProjectXml.h"
#include "Serialization/NodeSerialization.h"
#include "Serialization/ProjectSerialization.h"

NATRON_NAMESPACE_USING

namespace {

// The project files written here carry no GUI section, so they are always marked as saved by a
// background app: loading one into a GUI app would otherwise read a GUI section next.
std::string
saveProjectXml(const Serialization::ProjectSerialization& serialization)
{
    std::stringstream buffer;
    {
        boost::archive::xml_oarchive archive(buffer);
        bool backgroundProject = true;
        archive << boost::serialization::make_nvp("Background_project", backgroundProject);
        archive << boost::serialization::make_nvp("Project", serialization);
    }

    return buffer.str();
}

bool
loadProjectXml(const std::string& xml,
               Serialization::ProjectSerialization* serialization)
{
    try {
        std::stringstream buffer(xml);
        boost::archive::xml_iarchive archive(buffer);
        bool backgroundProject = false;
        archive >> boost::serialization::make_nvp("Background_project", backgroundProject);
        archive >> boost::serialization::make_nvp("Project", *serialization);
    } catch (const std::exception& e) {
        ADD_FAILURE() << "Loading through Serialization/BoostXml failed: " << e.what();

        return false;
    }

    return true;
}

// KnobsAge and CacheID are session counters, not saved state: a node's age moves on as soon as
// the loaded graph is connected and its expressions are restored.
std::string
withoutSessionCounters(const std::string& xml)
{
    QString normalized = QString::fromUtf8(xml.c_str());
    const char* const sessionCounterTags[] = { "KnobsAge", "CacheID" };

    for (std::size_t i = 0; i < sizeof(sessionCounterTags) / sizeof(sessionCounterTags[0]); ++i) {
        const QString tag = QString::fromUtf8(sessionCounterTags[i]);
        QRegularExpression pattern(QString::fromUtf8("<%1>[^<]*</%1>").arg(tag));
        normalized.replace(pattern, QString::fromUtf8("<%1>SESSION_COUNTER</%1>").arg(tag));
    }

    return normalized.toStdString();
}

Serialization::NodeSerializationPtr
findSerializedNode(const std::list<Serialization::NodeSerializationPtr>& nodes,
                   const std::string& pluginID)
{
    for (std::list<Serialization::NodeSerializationPtr>::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if ((*it)->pluginID == pluginID) {
            return *it;
        }
    }

    return Serialization::NodeSerializationPtr();
}

} // anonymous namespace

class ProjectXmlEquivalenceTest
    : public SyntheticProjectGraphTest {
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

    enum ReloadExpectationEnum {
        // Saving the loaded project writes the XML saved from the graph as it was built.
        eReloadExpectationSameXml,
        // Loading settles some session state into saved state, so only a second load of the
        // first reload's save is expected to write the same XML again.
        eReloadExpectationFixedPoint
    };

    // Writes xml to fileName in tmp, loads it as a project file and returns the XML saving that
    // project writes.
    std::string reloadProjectXml(const QTemporaryDir& tmp,
                                 const QString& fileName,
                                 const std::string& xml)
    {
        const QString directoryPath = tmp.path() + QLatin1Char('/');
        {
            QFile projectFile(directoryPath + fileName);
            if (!projectFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                ADD_FAILURE() << "Cannot write " << fileName.toStdString();

                return std::string();
            }
            projectFile.write(xml.c_str(), (qint64)xml.size());
        }
        ProjectPtr project = getApp()->getProject();
        if (!project->loadProject(directoryPath, fileName)) {
            ADD_FAILURE() << "Cannot load " << fileName.toStdString();

            return std::string();
        }

        Serialization::ProjectSerialization reloaded;
        project->toSerialization(&reloaded);
        project->toSerialization(&reloaded);

        return saveProjectXml(reloaded);
    }

    // Saves the graph built in the current project to XML, checks that reading that XML and
    // writing it again changes nothing, then loads it as a project file from tmp into a fresh
    // project and checks what saving that project writes, as reloadExpectation says.
    void expectRoundTrip(const QTemporaryDir& tmp,
                         Serialization::ProjectSerialization* loaded,
                         ReloadExpectationEnum reloadExpectation = eReloadExpectationSameXml)
    {
        ProjectPtr project = getApp()->getProject();

        // Loading the file from tmp points the [Project] variable at tmp: point it there first
        // so the project knobs saved before and after loading agree.
        ASSERT_TRUE(setProjectPathVariable(tmp.path()));

        // Saving syncs the private data of OpenFX effects into their knobs, which may change
        // knob ages on the first save only.
        Serialization::ProjectSerialization saved;
        project->toSerialization(&saved);
        project->toSerialization(&saved);
        const std::string savedXml = saveProjectXml(saved);

        ASSERT_TRUE(loadProjectXml(savedXml, loaded));
        EXPECT_EQ(savedXml, saveProjectXml(*loaded));
        EXPECT_EQ((unsigned int)NATRON_SERIALIZATION_PROJECT_VERSION, loaded->version);
        EXPECT_EQ(NATRON_VERSION_MAJOR, loaded->savedByVersion.versionMajor);
        ASSERT_EQ(saved.nodes.nodes.size(), loaded->nodes.nodes.size());
        for (std::list<Serialization::NodeSerializationPtr>::const_iterator it = loaded->nodes.nodes.begin(); it != loaded->nodes.nodes.end(); ++it) {
            EXPECT_EQ((unsigned int)NATRON_SERIALIZATION_NODE_VERSION, (*it)->version);
        }

        const std::string reloadedXml = reloadProjectXml(tmp, QString::fromUtf8("round-trip.ntp"), savedXml);
        ASSERT_FALSE(reloadedXml.empty());
        if (reloadExpectation == eReloadExpectationSameXml) {
            EXPECT_EQ(withoutSessionCounters(savedXml), withoutSessionCounters(reloadedXml));
        } else {
            const std::string reloadedAgainXml = reloadProjectXml(tmp, QString::fromUtf8("round-trip-again.ntp"), reloadedXml);
            EXPECT_EQ(withoutSessionCounters(reloadedXml), withoutSessionCounters(reloadedAgainXml));
        }
    }
};

TEST_F(ProjectXmlEquivalenceTest, GeneratorGradeWrite)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(buildGeneratorGradeWriteGraph(tmp));

    Serialization::ProjectSerialization loaded;
    expectRoundTrip(tmp, &loaded);

    ASSERT_EQ(3u, loaded.nodes.nodes.size());
    Serialization::NodeSerializationPtr grade = findSerializedNode(loaded.nodes.nodes, PLUGINID_NATRON_GRADE);
    ASSERT_TRUE(bool(grade));
    EXPECT_FALSE(grade->inputs.empty());
    EXPECT_FALSE(grade->knobs.empty());
    EXPECT_TRUE(grade->oldInputs.empty());
    EXPECT_FALSE(grade->hasRotoContext);
    EXPECT_FALSE(grade->hasTrackerContext);
}

TEST_F(ProjectXmlEquivalenceTest, GroupWithUserKnobAndNestedGroup)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(buildGroupWithUserKnobAndNestedGroupGraph());

    Serialization::ProjectSerialization loaded;
    expectRoundTrip(tmp, &loaded);

    ASSERT_EQ(1u, loaded.nodes.nodes.size());
    const Serialization::NodeSerialization& outerGroup = *loaded.nodes.nodes.front();
    EXPECT_EQ(std::string(PLUGINID_NATRON_GROUP), outerGroup.pluginID);
    ASSERT_EQ(1u, outerGroup.userPages.size());
    ASSERT_EQ(1u, outerGroup.userPages.front().children.size());
    EXPECT_EQ(std::string("myUserValue"), outerGroup.userPages.front().children.front().knob.name);
    EXPECT_FALSE(outerGroup.pagesOrder.empty());

    Serialization::NodeSerializationPtr nestedGroup = findSerializedNode(outerGroup.children, PLUGINID_NATRON_GROUP);
    ASSERT_TRUE(bool(nestedGroup));
    EXPECT_FALSE(nestedGroup->children.empty());
    EXPECT_TRUE(bool(findSerializedNode(outerGroup.children, PLUGINID_NATRON_DOT)));
}

TEST_F(ProjectXmlEquivalenceTest, RotoPaintAndTracker)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(buildRotoPaintAndTrackerGraph());

    // Loading a RotoPaint leaves it in a state the built graph is not in: the context makes its
    // knobs' default state disabled and then enables the transform knobs again by restoring the
    // selection, RotoPaint::onKnobsLoaded switches to the select tool while the paint role is
    // still current (setting the blending mode to copy), and a loaded stroke is finished, which
    // resets its transform center to the middle of its points.
    Serialization::ProjectSerialization loaded;
    expectRoundTrip(tmp, &loaded, eReloadExpectationFixedPoint);

    Serialization::NodeSerializationPtr rotoPaint = findSerializedNode(loaded.nodes.nodes, PLUGINID_NATRON_ROTOPAINT);
    ASSERT_TRUE(bool(rotoPaint));
    EXPECT_TRUE(rotoPaint->hasRotoContext);
    EXPECT_EQ(2u, rotoPaint->rotoContext.baseLayer.children.size());

    Serialization::NodeSerializationPtr tracker = findSerializedNode(loaded.nodes.nodes, PLUGINID_NATRON_TRACKER);
    ASSERT_TRUE(bool(tracker));
    EXPECT_TRUE(tracker->hasTrackerContext);
    EXPECT_EQ(2u, tracker->trackerContext.tracks.size());
}

TEST(ProjectXmlLegacyVersionString, Parse)
{
    int versionMajor = -1;
    int versionMinor = -1;
    int versionRevision = -1;

    EXPECT_TRUE(Serialization::parseLegacyNatronVersionString("Natron v2.0.3 from git branch master commit 1234abc for  Linux 64bit",
                                                              &versionMajor, &versionMinor, &versionRevision));
    EXPECT_EQ(2, versionMajor);
    EXPECT_EQ(0, versionMinor);
    EXPECT_EQ(3, versionRevision);

    // The revision part keeps the release-candidate suffix, which is not an integer.
    EXPECT_TRUE(Serialization::parseLegacyNatronVersionString("Natron v1.0.0 RC2 from git branch master commit 1234abc for  Linux 64bit",
                                                              &versionMajor, &versionMinor, &versionRevision));
    EXPECT_EQ(1, versionMajor);
    EXPECT_EQ(0, versionMinor);
    EXPECT_EQ(0, versionRevision);

    versionMajor = -1;
    EXPECT_FALSE(Serialization::parseLegacyNatronVersionString("Natron v2.1 from git branch master", &versionMajor, &versionMinor, &versionRevision));
    EXPECT_FALSE(Serialization::parseLegacyNatronVersionString("not a version", &versionMajor, &versionMinor, &versionRevision));
    EXPECT_EQ(-1, versionMajor);
}
