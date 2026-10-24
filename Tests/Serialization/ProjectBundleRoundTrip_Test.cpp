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
#include <string>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "ProjectBundleTestHelpers.h"
#include "SyntheticProjectGraphs.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Project.h"

#include "Serialization/NodeSerialization.h"
#include "Serialization/ProjectSerialization.h"
#include "Serialization/Yaml/BlobSink.h"
#include "Serialization/Yaml/ProjectYaml.h"

NATRON_NAMESPACE_USING

class ProjectBundleRoundTripTest
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
        // Saving the loaded project writes the project.yaml written from the graph as it was built.
        eReloadExpectationSameYaml,
        // Loading settles some session state into saved state, so only a second load of the
        // first reload's save is expected to write the same project.yaml again.
        eReloadExpectationFixedPoint
    };

    // Writes the graph built in the current project as a bundle in tmp, checks that decoding its
    // project.yaml and encoding it again changes nothing, then loads the bundle into a fresh
    // project and checks what saving that project writes, as reloadExpectation says.
    void expectRoundTrip(const QTemporaryDir& tmp,
                         Serialization::ProjectSerialization* loaded,
                         ReloadExpectationEnum reloadExpectation = eReloadExpectationSameYaml)
    {
        ProjectPtr project = getApp()->getProject();

        // Loading the file from tmp points the [Project] variable at tmp: point it there first
        // so the project knobs saved before and after loading agree.
        ASSERT_TRUE(setProjectPathVariable(tmp.path()));

        // Saving syncs the private data of OpenFX effects into their knobs, which may change
        // them on the first save only.
        Serialization::ProjectSerialization saved;
        project->toSerialization(&saved);
        project->toSerialization(&saved);

        Serialization::MemoryBlobStore blobs;
        const std::string savedYaml = Serialization::encodeProjectYaml(saved, blobs);
        *loaded = Serialization::decodeProjectYaml(savedYaml, blobs);
        Serialization::MemoryBlobStore reencodedBlobs;
        EXPECT_EQ(savedYaml, Serialization::encodeProjectYaml(*loaded, reencodedBlobs));
        EXPECT_EQ(NATRON_VERSION_MAJOR, loaded->savedByVersion.versionMajor);
        ASSERT_EQ(saved.nodes.nodes.size(), loaded->nodes.nodes.size());

        project->reset(false, true);
        std::string writtenYaml;
        ASSERT_TRUE(loadProjectBundle(project, tmp, QString::fromUtf8("round-trip.ntpz"), saved, &writtenYaml));
        EXPECT_EQ(savedYaml, writtenYaml);

        std::string resavedYaml;
        ASSERT_TRUE(saveProjectBundle(project, tmp, QString::fromUtf8("round-trip-resaved.ntpz"), &resavedYaml));
        if (reloadExpectation == eReloadExpectationSameYaml) {
            EXPECT_EQ(savedYaml, resavedYaml);
        } else {
            project->reset(false, true);
            ASSERT_TRUE(project->loadProject(tmp.path() + QLatin1Char('/'), QString::fromUtf8("round-trip-resaved.ntpz")));
            std::string resavedAgainYaml;
            ASSERT_TRUE(saveProjectBundle(project, tmp, QString::fromUtf8("round-trip-resaved-again.ntpz"), &resavedAgainYaml));
            EXPECT_EQ(resavedYaml, resavedAgainYaml);
        }
    }
};

TEST_F(ProjectBundleRoundTripTest, GeneratorGradeWrite)
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
    EXPECT_FALSE(grade->hasRotoContext);
    EXPECT_FALSE(grade->hasTrackerContext);
}

TEST_F(ProjectBundleRoundTripTest, GroupWithUserKnobAndNestedGroup)
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

TEST_F(ProjectBundleRoundTripTest, RotoPaintAndTracker)
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
