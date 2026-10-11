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

#include <exception>
#include <list>
#include <memory>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "SyntheticProjectGraphs.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/NodeSerialization.h"
#include "Engine/Project.h"
#include "Engine/ProjectSerialization.h"

#include "Serialization/NodeSerialization.h"
#include "Serialization/ProjectSerialization.h"
#include "Serialization/BoostXml/NodeXml.h"
#include "Serialization/BoostXml/ProjectXml.h"

NATRON_NAMESPACE_USING

namespace {

std::string
saveProjectWithOldPath(const AppInstancePtr& app)
{
    std::stringstream buffer;
    {
        ProjectSerialization serialization(app);
        serialization.initialize( app->getProject().get() );
        boost::archive::xml_oarchive archive(buffer);
        bool backgroundProject = app->isBackground();
        archive << boost::serialization::make_nvp("Background_project", backgroundProject);
        archive << boost::serialization::make_nvp("Project", serialization);
    }

    return buffer.str();
}

std::string
saveProjectWithNewPath(bool backgroundProject,
                       const Serialization::ProjectSerialization& serialization)
{
    std::stringstream buffer;
    {
        boost::archive::xml_oarchive archive(buffer);
        archive << boost::serialization::make_nvp("Background_project", backgroundProject);
        archive << boost::serialization::make_nvp("Project", serialization);
    }

    return buffer.str();
}

bool
loadProjectWithNewPath(const std::string& xml,
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

std::string
saveNodeWithOldPath(const NodeSerialization& serialization)
{
    std::stringstream buffer;
    {
        boost::archive::xml_oarchive archive(buffer);
        archive << boost::serialization::make_nvp("Node", serialization);
    }

    return buffer.str();
}

Serialization::NodeSerializationPtr
findSerializedNode(const std::list<Serialization::NodeSerializationPtr>& nodes,
                   const std::string& pluginID)
{
    for (std::list<Serialization::NodeSerializationPtr>::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if ( (*it)->pluginID == pluginID ) {
            return *it;
        }
    }

    return Serialization::NodeSerializationPtr();
}

} // anonymous namespace

class ProjectXmlEquivalenceTest
    : public SyntheticProjectGraphTest
{
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

    // Checks that both paths write the same project XML, that loading it through the new path
    // writes it again unchanged, and that every loaded top-level node, handed to the old node
    // class, archives as that node saved by the old path does.
    void expectOldAndNewPathsMatch(Serialization::ProjectSerialization* loaded)
    {
        AppInstancePtr app = getApp();
        ProjectPtr project = app->getProject();

        // Saving syncs the private data of OpenFX effects into their knobs, which may change
        // knob ages on the first save only.
        saveProjectWithOldPath(app);
        const std::string oldXml = saveProjectWithOldPath(app);

        Serialization::ProjectSerialization saved;
        project->toSerialization(&saved);
        const std::string newXml = saveProjectWithNewPath(app->isBackground(), saved);
        EXPECT_EQ(oldXml, newXml);

        ASSERT_TRUE( loadProjectWithNewPath(newXml, loaded) );
        EXPECT_EQ( newXml, saveProjectWithNewPath(app->isBackground(), *loaded) );
        EXPECT_EQ( (unsigned int)NATRON_SERIALIZATION_PROJECT_VERSION, loaded->version );
        EXPECT_EQ( NATRON_VERSION_MAJOR, loaded->savedByVersion.versionMajor );

        ASSERT_EQ( saved.nodes.nodes.size(), loaded->nodes.nodes.size() );
        for (std::list<Serialization::NodeSerializationPtr>::const_iterator it = loaded->nodes.nodes.begin(); it != loaded->nodes.nodes.end(); ++it) {
            EXPECT_EQ( (unsigned int)NATRON_SERIALIZATION_NODE_VERSION, (*it)->version );
            NodePtr liveNode = project->getNodeByName( (*it)->scriptName );
            ASSERT_TRUE( bool(liveNode) ) << (*it)->scriptName;
            EXPECT_EQ( saveNodeWithOldPath( NodeSerialization(liveNode) ), saveNodeWithOldPath( NodeSerialization(**it) ) ) << (*it)->scriptName;
        }
    }
};

TEST_F(ProjectXmlEquivalenceTest, GeneratorGradeWrite)
{
    QTemporaryDir tmp;
    ASSERT_TRUE( tmp.isValid() );
    ASSERT_TRUE( buildGeneratorGradeWriteGraph(tmp) );

    Serialization::ProjectSerialization loaded;
    expectOldAndNewPathsMatch(&loaded);

    ASSERT_EQ( 3u, loaded.nodes.nodes.size() );
    Serialization::NodeSerializationPtr grade = findSerializedNode(loaded.nodes.nodes, PLUGINID_NATRON_GRADE);
    ASSERT_TRUE( bool(grade) );
    EXPECT_FALSE( grade->inputs.empty() );
    EXPECT_FALSE( grade->knobs.empty() );
    EXPECT_TRUE( grade->oldInputs.empty() );
    EXPECT_FALSE( grade->hasRotoContext );
    EXPECT_FALSE( grade->hasTrackerContext );
}

TEST_F(ProjectXmlEquivalenceTest, GroupWithUserKnobAndNestedGroup)
{
    ASSERT_TRUE( buildGroupWithUserKnobAndNestedGroupGraph() );

    Serialization::ProjectSerialization loaded;
    expectOldAndNewPathsMatch(&loaded);

    ASSERT_EQ( 1u, loaded.nodes.nodes.size() );
    const Serialization::NodeSerialization& outerGroup = *loaded.nodes.nodes.front();
    EXPECT_EQ( std::string(PLUGINID_NATRON_GROUP), outerGroup.pluginID );
    ASSERT_EQ( 1u, outerGroup.userPages.size() );
    ASSERT_EQ( 1u, outerGroup.userPages.front().children.size() );
    EXPECT_EQ( std::string("myUserValue"), outerGroup.userPages.front().children.front().knob.name );
    EXPECT_FALSE( outerGroup.pagesOrder.empty() );

    Serialization::NodeSerializationPtr nestedGroup = findSerializedNode(outerGroup.children, PLUGINID_NATRON_GROUP);
    ASSERT_TRUE( bool(nestedGroup) );
    EXPECT_FALSE( nestedGroup->children.empty() );
    EXPECT_TRUE( bool( findSerializedNode(outerGroup.children, PLUGINID_NATRON_DOT) ) );
}

TEST_F(ProjectXmlEquivalenceTest, RotoPaintAndTracker)
{
    ASSERT_TRUE( buildRotoPaintAndTrackerGraph() );

    Serialization::ProjectSerialization loaded;
    expectOldAndNewPathsMatch(&loaded);

    Serialization::NodeSerializationPtr rotoPaint = findSerializedNode(loaded.nodes.nodes, PLUGINID_NATRON_ROTOPAINT);
    ASSERT_TRUE( bool(rotoPaint) );
    EXPECT_TRUE( rotoPaint->hasRotoContext );
    EXPECT_EQ( 2u, rotoPaint->rotoContext.baseLayer.children.size() );

    Serialization::NodeSerializationPtr tracker = findSerializedNode(loaded.nodes.nodes, PLUGINID_NATRON_TRACKER);
    ASSERT_TRUE( bool(tracker) );
    EXPECT_TRUE( tracker->hasTrackerContext );
    EXPECT_EQ( 2u, tracker->trackerContext.tracks.size() );
}

TEST(ProjectXmlLegacyVersionString, Parse)
{
    int versionMajor = -1;
    int versionMinor = -1;
    int versionRevision = -1;

    EXPECT_TRUE( Serialization::parseLegacyNatronVersionString("Natron v2.0.3 from git branch master commit 1234abc for  Linux 64bit",
                                                               &versionMajor, &versionMinor, &versionRevision) );
    EXPECT_EQ(2, versionMajor);
    EXPECT_EQ(0, versionMinor);
    EXPECT_EQ(3, versionRevision);

    // The revision part keeps the release-candidate suffix, which is not an integer.
    EXPECT_TRUE( Serialization::parseLegacyNatronVersionString("Natron v1.0.0 RC2 from git branch master commit 1234abc for  Linux 64bit",
                                                               &versionMajor, &versionMinor, &versionRevision) );
    EXPECT_EQ(1, versionMajor);
    EXPECT_EQ(0, versionMinor);
    EXPECT_EQ(0, versionRevision);

    versionMajor = -1;
    EXPECT_FALSE( Serialization::parseLegacyNatronVersionString("Natron v2.1 from git branch master", &versionMajor, &versionMinor, &versionRevision) );
    EXPECT_FALSE( Serialization::parseLegacyNatronVersionString("not a version", &versionMajor, &versionMinor, &versionRevision) );
    EXPECT_EQ(-1, versionMajor);
}
