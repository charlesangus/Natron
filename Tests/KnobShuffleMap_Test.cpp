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
#include <vector>

#include <gtest/gtest.h>

#include <QLatin1Char>
#include <QObject>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/KnobShuffleMap.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Channel/Shuffle.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

std::shared_ptr<KnobShuffleMap>
makeKnob()
{
    std::shared_ptr<KnobShuffleMap> knob = std::make_shared<KnobShuffleMap>(static_cast<KnobHolder*>(NULL), std::string("mapping"), 1, false);

    knob->populate();

    return knob;
}

} // namespace

TEST(KnobShuffleMap, TypeAndColumns)
{
    std::shared_ptr<KnobShuffleMap> knob = makeKnob();
    KnobIPtr asKnobI = knob;

    EXPECT_EQ(std::string("ShuffleMap"), asKnobI->typeName());
    EXPECT_EQ(2, knob->getColumnsCount());
    EXPECT_EQ(std::string("Out"), knob->getColumnTag(0));
    EXPECT_EQ(std::string("Src"), knob->getColumnTag(1));
    EXPECT_FALSE(knob->isColumnEditable(0));
    EXPECT_FALSE(knob->useEditButton());
    EXPECT_FALSE(asKnobI->canAnimate());
    EXPECT_FALSE(asKnobI->supportsExpressions());
}

TEST(KnobShuffleMap, DefaultIsEmptyMeaningKeepEverywhere)
{
    std::shared_ptr<KnobShuffleMap> knob = makeKnob();

    EXPECT_TRUE(knob->getRows().empty());
    EXPECT_EQ(ShuffleSource::eKeep, knob->getSource(1, 0).kind);
    EXPECT_EQ(ShuffleSource::eKeep, knob->getSource(2, 3).kind);
}

TEST(KnobShuffleMap, EncodeDecodeRoundTrip)
{
    std::shared_ptr<KnobShuffleMap> knob = makeKnob();
    std::vector<ShuffleMapRow> rows(3);

    rows[0].outSlot = 1;
    rows[0].outIndex = 3;
    rows[0].src = ShuffleSource::makeInput(2, 0);
    rows[1].outSlot = 1;
    rows[1].outIndex = 0;
    rows[1].src = ShuffleSource::makeZero();
    rows[2].outSlot = 2;
    rows[2].outIndex = 1;
    rows[2].src = ShuffleSource::makeOne();

    const std::string raw = knob->encodeRows(rows);
    EXPECT_NE(std::string::npos, raw.find("<Out>out1.3</Out><Src>in2.0</Src>"));
    EXPECT_NE(std::string::npos, raw.find("<Out>out1.0</Out><Src>0</Src>"));
    EXPECT_NE(std::string::npos, raw.find("<Out>out2.1</Out><Src>1</Src>"));

    knob->setValue(raw);

    std::vector<ShuffleMapRow> decoded = knob->getRows();
    ASSERT_EQ(rows.size(), decoded.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        EXPECT_EQ(rows[i], decoded[i]);
    }

    EXPECT_EQ(ShuffleSource::eInput, knob->getSource(1, 3).kind);
    EXPECT_EQ(2, knob->getSource(1, 3).slot);
    EXPECT_EQ(0, knob->getSource(1, 3).index);
    EXPECT_EQ(ShuffleSource::eZero, knob->getSource(1, 0).kind);
    EXPECT_EQ(ShuffleSource::eOne, knob->getSource(2, 1).kind);
}

TEST(KnobShuffleMap, KeepMeansNoRow)
{
    std::shared_ptr<KnobShuffleMap> knob = makeKnob();

    knob->setSource(1, 3, ShuffleSource::makeInput(2, 0));
    ASSERT_EQ(1u, knob->getRows().size());

    knob->setSource(1, 3, ShuffleSource());
    EXPECT_TRUE(knob->getRows().empty());
    EXPECT_EQ(ShuffleSource::eKeep, knob->getSource(1, 3).kind);

    // Setting keep on a channel with no row is a no-op, not a stray row.
    knob->setSource(2, 2, ShuffleSource());
    EXPECT_TRUE(knob->getRows().empty());
}

TEST(KnobShuffleMap, ClearRemovesTheRow)
{
    std::shared_ptr<KnobShuffleMap> knob = makeKnob();

    knob->setSource(1, 0, ShuffleSource::makeZero());
    knob->setSource(1, 1, ShuffleSource::makeOne());
    ASSERT_EQ(2u, knob->getRows().size());

    knob->clear(1, 0);
    ASSERT_EQ(1u, knob->getRows().size());
    EXPECT_EQ(1, knob->getRows()[0].outIndex);
    EXPECT_EQ(ShuffleSource::eKeep, knob->getSource(1, 0).kind);
}

TEST(KnobShuffleMap, ResetEmptiesTheTable)
{
    std::shared_ptr<KnobShuffleMap> knob = makeKnob();

    knob->setSource(1, 0, ShuffleSource::makeInput(1, 0));
    knob->setSource(1, 1, ShuffleSource::makeInput(1, 1));
    knob->setSource(2, 3, ShuffleSource::makeZero());
    ASSERT_EQ(3u, knob->getRows().size());

    knob->reset();
    EXPECT_TRUE(knob->getRows().empty());
    EXPECT_EQ(ShuffleSource::eKeep, knob->getSource(1, 0).kind);
}

TEST(KnobShuffleMap, MalformedCellsDecodeAsKeepAndAreDropped)
{
    std::shared_ptr<KnobShuffleMap> knob = makeKnob();
    std::string raw;

    // Malformed Out cell (no slot number).
    raw += "<Out>outX.3</Out><Src>in1.0</Src>";
    // Malformed Out cell (slot out of {1,2}).
    raw += "<Out>out3.0</Out><Src>in1.0</Src>";
    // Malformed Src cell (unknown source form).
    raw += "<Out>out1.2</Out><Src>bogus</Src>";
    // Malformed Src cell (slot out of {1,2}).
    raw += "<Out>out1.1</Out><Src>in3.0</Src>";
    // Well-formed row, to prove the others were dropped rather than the whole value rejected.
    raw += "<Out>out2.0</Out><Src>in2.2</Src>";

    knob->setValue(raw);

    std::vector<ShuffleMapRow> rows = knob->getRows();
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(2, rows[0].outSlot);
    EXPECT_EQ(0, rows[0].outIndex);
    EXPECT_EQ(ShuffleSource::eInput, rows[0].src.kind);
    EXPECT_EQ(2, rows[0].src.slot);
    EXPECT_EQ(2, rows[0].src.index);

    EXPECT_EQ(ShuffleSource::eKeep, knob->getSource(1, 2).kind);
    EXPECT_EQ(ShuffleSource::eKeep, knob->getSource(1, 1).kind);
}

TEST(KnobShuffleMap, SettingTheSameOutputTwiceLeavesOneRow)
{
    std::shared_ptr<KnobShuffleMap> knob = makeKnob();

    knob->setSource(1, 3, ShuffleSource::makeInput(1, 3));
    ASSERT_EQ(1u, knob->getRows().size());

    knob->setSource(1, 3, ShuffleSource::makeInput(2, 0));
    ASSERT_EQ(1u, knob->getRows().size());
    EXPECT_EQ(ShuffleSource::eInput, knob->getSource(1, 3).kind);
    EXPECT_EQ(2, knob->getSource(1, 3).slot);
    EXPECT_EQ(0, knob->getSource(1, 3).index);
}

// The Shuffle node's Mapping knob is a plain user KnobShuffleMap (typeName "ShuffleMap"),
// registered through KnobFactory/KnobSerialization like any other user knob table: a project
// save/reset/load round trip must hand back the exact same rows.
TEST_F(BaseTest, ProjectSaveLoadPreservesKnobShuffleMapRows)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    NodePtr shuffle = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));
    ASSERT_TRUE(bool(shuffle)) << "node creation failed for " << PLUGINID_NATRON_SHUFFLE;

    KnobShuffleMapPtr shuffleMapKnob = std::dynamic_pointer_cast<KnobShuffleMap>(shuffle->getKnobByName(kShuffleParamMapping));
    ASSERT_TRUE(bool(shuffleMapKnob));
    KnobIPtr shuffleMapKnobAsKnobI = shuffleMapKnob;
    EXPECT_EQ(std::string("ShuffleMap"), shuffleMapKnobAsKnobI->typeName());

    const std::string shuffleName = shuffle->getScriptName();

    std::vector<ShuffleMapRow> originalRows(3);
    originalRows[0].outSlot = 1;
    originalRows[0].outIndex = 0;
    originalRows[0].src = ShuffleSource::makeInput(1, 0);
    originalRows[1].outSlot = 1;
    originalRows[1].outIndex = 1;
    originalRows[1].src = ShuffleSource::makeZero();
    originalRows[2].outSlot = 2;
    originalRows[2].outIndex = 0;
    originalRows[2].src = ShuffleSource::makeOne();

    const std::string raw = shuffleMapKnob->encodeRows(originalRows);
    shuffleMapKnob->setValue(raw);

    ASSERT_EQ(originalRows.size(), shuffleMapKnob->getRows().size());

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("shuffle-map-test.ntp");

    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));

    project->reset(false, true);

    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr shuffle2 = project->getNodeByName(shuffleName);
    ASSERT_TRUE(bool(shuffle2));

    KnobShuffleMapPtr loadedShuffleMapKnob = std::dynamic_pointer_cast<KnobShuffleMap>(shuffle2->getKnobByName(kShuffleParamMapping));
    ASSERT_TRUE(bool(loadedShuffleMapKnob));

    std::vector<ShuffleMapRow> loadedRows = loadedShuffleMapKnob->getRows();
    ASSERT_EQ(originalRows.size(), loadedRows.size());

    for (std::size_t i = 0; i < originalRows.size(); ++i) {
        EXPECT_EQ(originalRows[i].outSlot, loadedRows[i].outSlot);
        EXPECT_EQ(originalRows[i].outIndex, loadedRows[i].outIndex);
        EXPECT_EQ(originalRows[i].src.kind, loadedRows[i].src.kind);
        if (originalRows[i].src.kind == ShuffleSource::eInput) {
            EXPECT_EQ(originalRows[i].src.slot, loadedRows[i].src.slot);
            EXPECT_EQ(originalRows[i].src.index, loadedRows[i].src.index);
        }
    }

    project->reset(false, true);
}

// A user-added KnobShuffleMap (e.g. dropped onto a NoOp/Dot node via the same machinery
// as user KnobChannelSet/KnobLayerSelect/KnobChannelSelect knobs) goes through
// Node::Implementation::restoreUserKnobsRecursive on load, not Node::loadKnob by name:
// that path must also carry a KnobShuffleMap branch, or the knob is silently dropped.
TEST_F(BaseTest, ProjectSaveLoadPreservesUserKnobShuffleMapRows)
{
    ProjectPtr project = getApp()->getProject();

    project->reset(false, true);

    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    ASSERT_TRUE(bool(dot)) << "node creation failed for " << PLUGINID_NATRON_DOT;

    EffectInstancePtr effect = dot->getEffectInstance();
    ASSERT_TRUE(bool(effect));

    KnobPagePtr userPage = effect->getOrCreateUserPageKnob();
    ASSERT_TRUE(bool(userPage));

    KnobShuffleMapPtr userKnob = AppManager::createKnob<KnobShuffleMap>(effect.get(), std::string("myMap"), 1, false);
    ASSERT_TRUE(bool(userKnob));
    userKnob->setName("myMap");
    userKnob->setAsUserKnob(true);
    userPage->addKnob(userKnob);

    const std::string dotName = dot->getScriptName();

    std::vector<ShuffleMapRow> originalRows(3);
    originalRows[0].outSlot = 1;
    originalRows[0].outIndex = 2;
    originalRows[0].src = ShuffleSource::makeInput(2, 1);
    originalRows[1].outSlot = 2;
    originalRows[1].outIndex = 0;
    originalRows[1].src = ShuffleSource::makeZero();
    originalRows[2].outSlot = 1;
    originalRows[2].outIndex = 0;
    originalRows[2].src = ShuffleSource::makeOne();

    const std::string raw = userKnob->encodeRows(originalRows);
    userKnob->setValue(raw);

    ASSERT_EQ(originalRows.size(), userKnob->getRows().size());

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("user-shuffle-map-test.ntp");

    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));

    project->reset(false, true);

    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr dot2 = project->getNodeByName(dotName);
    ASSERT_TRUE(bool(dot2));

    KnobShuffleMapPtr loadedUserKnob = std::dynamic_pointer_cast<KnobShuffleMap>(dot2->getKnobByName("myMap"));
    ASSERT_TRUE(bool(loadedUserKnob)) << "user KnobShuffleMap was dropped on project load";

    std::vector<ShuffleMapRow> loadedRows = loadedUserKnob->getRows();
    ASSERT_EQ(originalRows.size(), loadedRows.size());

    for (std::size_t i = 0; i < originalRows.size(); ++i) {
        EXPECT_EQ(originalRows[i], loadedRows[i]);
    }

    project->reset(false, true);
}
