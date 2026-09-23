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
#include <vector>

#include <gtest/gtest.h>

#include "Engine/KnobShuffleMap.h"

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
