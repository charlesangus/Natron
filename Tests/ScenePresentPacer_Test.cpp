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
#include <optional>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include "Gui/ScenePresentPacer.h"

NATRON_NAMESPACE_USING

namespace {
// Fences are the addresses of ints; a fence has signalled once its int is in signalled.
class ScenePresentPacerTest : public ::testing::Test {
protected:
    using Pacer = ScenePresentPacer<int>;

    Pacer::Fence fence(int i)
    {
        return &_fences[i];
    }

    void signal(int i)
    {
        _signalled.insert(fence(i));
    }

    std::unique_ptr<Pacer> makePacer()
    {
        return std::unique_ptr<Pacer>(new Pacer(
            [this](Pacer::Fence f) { return _signalled.count(f) != 0; },
            [this](Pacer::Fence f) { _released.push_back(f); }));
    }

    int _fences[8] = {};
    std::set<Pacer::Fence> _signalled;
    std::vector<Pacer::Fence> _released;
};
} // namespace

TEST_F(ScenePresentPacerTest, AFrameIsShownOnlyOnceItsFenceSignalsAndNeverWaits)
{
    std::unique_ptr<Pacer> pacer = makePacer();
    EXPECT_EQ(-1, pacer->frontTarget());
    const int back = pacer->backTarget();

    pacer->submitted(fence(0), 7);
    EXPECT_TRUE(pacer->inFlight());
    EXPECT_FALSE(pacer->poll()) << "the GPU has not finished the frame";
    EXPECT_FALSE(pacer->takeReady().has_value());
    EXPECT_TRUE(_released.empty());

    signal(0);
    EXPECT_TRUE(pacer->poll());
    EXPECT_FALSE(pacer->inFlight());
    EXPECT_EQ(back, pacer->frontTarget()) << "the finished target is the one shown";
    EXPECT_NE(back, pacer->backTarget()) << "the next frame draws into the other target";
    ASSERT_EQ(std::vector<Pacer::Fence> { fence(0) }, _released);
    std::optional<int> ready = pacer->takeReady();
    ASSERT_TRUE(ready.has_value());
    EXPECT_EQ(7, *ready);
    EXPECT_FALSE(pacer->takeReady().has_value()) << "a frame is handed out once";
    EXPECT_FALSE(pacer->pending());
}

TEST_F(ScenePresentPacerTest, TheTargetsAlternateSoTheShownOneIsNeverDrawnInto)
{
    std::unique_ptr<Pacer> pacer = makePacer();

    for (int i = 0; i < 4; ++i) {
        const int back = pacer->backTarget();
        pacer->submitted(fence(i), i);
        signal(i);
        ASSERT_TRUE(pacer->poll());
        ASSERT_TRUE(pacer->takeReady().has_value());
        EXPECT_EQ(back, pacer->frontTarget());
        EXPECT_NE(pacer->frontTarget(), pacer->backTarget());
    }
    EXPECT_EQ(4u, _released.size());
}

TEST_F(ScenePresentPacerTest, TheNewestFrameWinsAndEveryFenceIsReleasedOnce)
{
    std::unique_ptr<Pacer> pacer = makePacer();

    pacer->submitted(fence(0), 0);
    pacer->submitted(fence(1), 1);
    ASSERT_EQ(std::vector<Pacer::Fence> { fence(0) }, _released) << "the replaced frame's fence goes at once";
    signal(0);
    EXPECT_FALSE(pacer->poll()) << "only the newest frame's fence counts";
    signal(1);
    ASSERT_TRUE(pacer->poll());
    std::optional<int> ready = pacer->takeReady();
    ASSERT_TRUE(ready.has_value());
    EXPECT_EQ(1, *ready);
    EXPECT_EQ((std::vector<Pacer::Fence> { fence(0), fence(1) }), _released);
}

TEST_F(ScenePresentPacerTest, AFinishedFrameNotYetShownGivesWayToANewerOne)
{
    std::unique_ptr<Pacer> pacer = makePacer();

    pacer->submitted(fence(0), 0);
    signal(0);
    ASSERT_TRUE(pacer->poll());
    const int first = pacer->frontTarget();
    pacer->submitted(fence(1), 1);
    EXPECT_NE(first, pacer->backTarget()) << "the finished frame's target is not drawn into";
    signal(1);
    ASSERT_TRUE(pacer->poll());
    std::optional<int> ready = pacer->takeReady();
    ASSERT_TRUE(ready.has_value());
    EXPECT_EQ(1, *ready);
}

TEST_F(ScenePresentPacerTest, ClearReleasesTheFrameOnTheGpu)
{
    std::unique_ptr<Pacer> pacer = makePacer();

    pacer->submitted(fence(0), 0);
    pacer->markOwed();
    pacer->clear();
    EXPECT_EQ(std::vector<Pacer::Fence> { fence(0) }, _released);
    EXPECT_FALSE(pacer->pending());
    EXPECT_EQ(-1, pacer->frontTarget());
    EXPECT_FALSE(pacer->takeOwed());
    pacer.reset();
    EXPECT_EQ(1u, _released.size()) << "the destructor releases nothing";
}

TEST_F(ScenePresentPacerTest, OwedIsTakenOnceAndASubmitPaysIt)
{
    std::unique_ptr<Pacer> pacer = makePacer();

    pacer->markOwed();
    EXPECT_TRUE(pacer->takeOwed());
    EXPECT_FALSE(pacer->takeOwed());
    pacer->markOwed();
    pacer->submitted(fence(0), 0);
    EXPECT_FALSE(pacer->takeOwed()) << "the frame submitted draws what was asked for";
}

TEST_F(ScenePresentPacerTest, AMissingFenceCountsAsFinished)
{
    std::unique_ptr<Pacer> pacer = makePacer();

    pacer->submitted(nullptr, 3);
    EXPECT_TRUE(pacer->poll());
    EXPECT_TRUE(_released.empty());
}
