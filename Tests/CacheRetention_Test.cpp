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

#include <cstdint>
#include <list>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "CacheStress.h"
#include "NativeParity.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Cache.h"
#include "Engine/CacheRetention.h"
#include "Engine/CacheTier.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/ImageKey.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/ImageParams.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

using namespace CacheStress;

const RectI kWindow(0, 0, 32, 32);

// Any nonzero start: a stamp of zero means the signal never fired.
const std::uint64_t kStartNs = 1000000000000ULL;

const float kCostNs = 1e6f;
const std::size_t kBytes = 1024;

double
scoreOf(double priority,
        std::uint32_t reuseCount)
{
    return CacheEviction::score(priority, kCostNs, reuseCount, 0, kBytes);
}

TEST(CacheRetentionTest, FreshSignalsOrderEditedThenPanelThenReusedThenOffBranch)
{
    CacheRetentionSignals inputOfEdited;
    inputOfEdited.noteInputOfEdited(kStartNs);

    CacheRetentionSignals inputOfPanel;
    inputOfPanel.notePanelOfOutput(kStartNs);

    const CacheRetentionSignals reused;
    const CacheRetentionSignals offBranch;

    EXPECT_DOUBLE_EQ(9., inputOfEdited.priority(kStartNs));
    EXPECT_DOUBLE_EQ(5., inputOfPanel.priority(kStartNs));
    EXPECT_DOUBLE_EQ(CacheEviction::kNeutralPriority, reused.priority(kStartNs));
    EXPECT_DOUBLE_EQ(CacheEviction::kNeutralPriority, offBranch.priority(kStartNs));

    const double editedScore = scoreOf(inputOfEdited.priority(kStartNs), 0);
    const double panelScore = scoreOf(inputOfPanel.priority(kStartNs), 0);
    const double reusedScore = scoreOf(reused.priority(kStartNs), 3);
    const double offBranchScore = scoreOf(offBranch.priority(kStartNs), 0);

    EXPECT_GT(editedScore, panelScore);
    EXPECT_GT(panelScore, reusedScore);
    EXPECT_GT(reusedScore, offBranchScore);
    EXPECT_DOUBLE_EQ(3., reusedScore / offBranchScore);
}

TEST(CacheRetentionTest, ABoostHalvesEveryHalfLifeAndEndsAtExactlyNeutral)
{
    CacheRetentionSignals edited;
    edited.noteInputOfEdited(kStartNs);

    const std::uint64_t halfLife = (std::uint64_t)CacheRetention::kEditHalfLifeNs;
    EXPECT_DOUBLE_EQ(1. + CacheRetention::kEditWeight / 2., edited.priority(kStartNs + halfLife));
    EXPECT_DOUBLE_EQ(1. + CacheRetention::kEditWeight / 4., edited.priority(kStartNs + 2 * halfLife));

    const std::uint64_t cutoff = (std::uint64_t)(CacheRetention::kDecayCutoffHalfLives * CacheRetention::kEditHalfLifeNs);
    EXPECT_GT(edited.priority(kStartNs + cutoff - 1000000000ULL), CacheEviction::kNeutralPriority);
    EXPECT_EQ(CacheEviction::kNeutralPriority, edited.priority(kStartNs + cutoff));
}

TEST(CacheRetentionTest, AnOldEditFallsBelowARecentPanel)
{
    CacheRetentionSignals edited;
    edited.noteInputOfEdited(kStartNs);

    const std::uint64_t later = kStartNs + (std::uint64_t)(2. * CacheRetention::kEditHalfLifeNs);
    CacheRetentionSignals panel;
    panel.notePanelOfOutput(later);

    EXPECT_LT(edited.priority(later), panel.priority(later));
}

TEST(CacheRetentionTest, ReasonsMapToTheirWeights)
{
    CacheRetentionSignals retention;

    retention.noteReasons(CacheRetention::eReasonForced);
    EXPECT_DOUBLE_EQ(1. + CacheRetention::kForcedWeight, retention.priority(kStartNs));

    for (const unsigned structural : { CacheRetention::eReasonMultipleOutputs, CacheRetention::eReasonTemporalAccess, CacheRetention::eReasonNoTiles }) {
        retention.noteReasons(structural);
        EXPECT_DOUBLE_EQ(1. + CacheRetention::kStructuralWeight, retention.priority(kStartNs));
    }

    // These hold a place in admission only: their meaning reaches the priority through the panel and branch stamps.
    retention.noteReasons(CacheRetention::eReasonViewerInput | CacheRetention::eReasonOutputPanel | CacheRetention::eReasonAggressive);
    EXPECT_DOUBLE_EQ(CacheEviction::kNeutralPriority, retention.priority(kStartNs));

    retention.noteReasons(0);
    retention.noteStaticOutput(true);
    EXPECT_DOUBLE_EQ(1. + CacheRetention::kStructuralWeight, retention.priority(kStartNs));
    retention.noteStaticOutput(false);

    retention.noteViewerBranch(kStartNs);
    EXPECT_DOUBLE_EQ(1. + CacheRetention::kBranchWeight, retention.priority(kStartNs));
}

TEST(CacheRetentionTest, ACostAtTheThresholdCountsAsExpensive)
{
    CacheRetentionSignals retention;

    retention.rememberCost(CacheRetention::kExpensiveCostNs / 2.f);
    EXPECT_DOUBLE_EQ(CacheEviction::kNeutralPriority, retention.priority(kStartNs));

    retention.rememberCost(CacheRetention::kExpensiveCostNs);
    EXPECT_DOUBLE_EQ(1. + CacheRetention::kExpensiveWeight, retention.priority(kStartNs));
}

TEST(CacheRetentionTest, AdmissionIsAnyReason)
{
    EXPECT_FALSE(CacheRetention::admit(0));
    for (unsigned bit = CacheRetention::eReasonMultipleOutputs; bit <= CacheRetention::eReasonRotoEditing; bit <<= 1) {
        EXPECT_TRUE(CacheRetention::admit(bit));
    }
}

TEST(CacheRetentionTest, AnEditBurstReachesTheInputsOncePerGranularity)
{
    CacheRetentionSignals retention;

    EXPECT_TRUE(retention.noteEdited(kStartNs));
    EXPECT_FALSE(retention.noteEdited(kStartNs + CacheRetention::kStampGranularityNs - 1));
    EXPECT_TRUE(retention.noteEdited(kStartNs + CacheRetention::kStampGranularityNs));
}

TEST(CacheRetentionTest, AStampInsideTheGranularityDoesNotMove)
{
    CacheRetentionSignals retention;

    retention.noteInputOfEdited(kStartNs);
    retention.noteInputOfEdited(kStartNs + CacheRetention::kStampGranularityNs - 1);

    CacheRetentionSignals reference;
    reference.noteInputOfEdited(kStartNs);

    const std::uint64_t later = kStartNs + (std::uint64_t)CacheRetention::kEditHalfLifeNs;
    EXPECT_DOUBLE_EQ(reference.priority(later), retention.priority(later));
}

TEST(CacheRetentionTest, AnIdentityPassesAnEditOnWithoutRenewingIt)
{
    CacheRetentionSignals identity;
    identity.noteInputOfEdited(kStartNs);

    CacheRetentionSignals input;
    input.inheritInputOfEdited(identity);

    const std::uint64_t later = kStartNs + (std::uint64_t)CacheRetention::kEditHalfLifeNs;
    EXPECT_DOUBLE_EQ(identity.priority(later), input.priority(later));

    // A newer edit of the input's own consumer is not overwritten by the older one.
    CacheRetentionSignals recent;
    recent.noteInputOfEdited(later);
    const double recentPriority = recent.priority(later);
    recent.inheritInputOfEdited(identity);
    EXPECT_DOUBLE_EQ(recentPriority, recent.priority(later));
}

TEST(CacheRetentionTest, AnImageOutsideACacheIsNeverBoundOrCounted)
{
    CacheEntryHeader header;
    const CacheRetentionSignalsPtr retention = std::make_shared<CacheRetentionSignals>();

    CacheRetention::noteHit(header, retention, 1);
    CacheRetention::noteStored(header, retention, 2);

    EXPECT_EQ(0u, header.reuseCount.load());
    EXPECT_FALSE(bool(header.retention.load()));
}

ImagePtr
createEntry(Cache<Image>& cache,
            U64 hash,
            std::uint64_t lastUse)
{
    const ImageParamsPtr params = Image::makeParams(RectD(0, 0, 64, 64), 1., 0, false,
                                                    ImageLayerDesc::getRGBAComponents(),
                                                    eImageBitDepthFloat,
                                                    eImageFieldingOrderNone);
    const ImageKey key = Image::makeKey(NULL, hash, false, 1., ViewIdx(0), false, false).forEntry(0, ImageLayerDesc::getRGBAComponents(), eImageBitDepthFloat);
    ImagePtr image;

    if (cache.getOrCreate(key, params, 0, &image) || !image) {
        return ImagePtr();
    }
    image->allocateMemory();
    image->getEntryHeader()->recomputeCostNs.store(kCostNs);
    image->getEntryHeader()->lastUse.store(lastUse);

    return image;
}

class CacheRetentionCacheTest
    : public BaseTest {
protected:
    virtual void TearDown() override
    {
        getApp()->getProject()->reset(false, true);
        appPTR->clearAllCaches();
        BaseTest::TearDown();
    }
};

TEST_F(CacheRetentionCacheTest, OneRenderCountsOnceAndStoringIsNotAReuse)
{
    Cache<Image> cache("CacheRetentionTest_Count", 1, 64ULL * 1024 * 1024, 1.);
    const ImagePtr image = createEntry(cache, 1, CacheEviction::nowNs());
    ASSERT_TRUE(bool(image));

    CacheEntryHeader& header = *image->getEntryHeader();
    const CacheRetentionSignalsPtr first = std::make_shared<CacheRetentionSignals>();
    const CacheRetentionSignalsPtr second = std::make_shared<CacheRetentionSignals>();

    CacheRetention::noteStored(header, first, 10);
    EXPECT_EQ(first, header.retention.load());
    EXPECT_EQ(0u, header.reuseCount.load());

    CacheRetention::noteHit(header, first, 10);
    EXPECT_EQ(0u, header.reuseCount.load());

    CacheRetention::noteHit(header, first, 11);
    CacheRetention::noteHit(header, first, 11);
    EXPECT_EQ(1u, header.reuseCount.load());

    CacheRetention::noteHit(header, second, 12);
    EXPECT_EQ(2u, header.reuseCount.load());
    EXPECT_EQ(second, header.retention.load());

    CacheRetention::noteHit(header, second, 0);
    CacheRetention::noteHit(header, second, 0);
    EXPECT_EQ(4u, header.reuseCount.load());
}

TEST_F(CacheRetentionCacheTest, EvictionKeepsTheEntryOfAnEditedNodesInput)
{
    Cache<Image> cache("CacheRetentionTest_Evict", 1, 64ULL * 1024 * 1024, 1.);
    cache.useEvictionScore();

    const std::uint64_t lastUse = CacheEviction::nowNs();
    ImagePtr boosted = createEntry(cache, 1, lastUse);
    ImagePtr neutral = createEntry(cache, 2, lastUse);
    ASSERT_TRUE(boosted && neutral);

    const CacheRetentionSignalsPtr retention = std::make_shared<CacheRetentionSignals>();
    retention->noteInputOfEdited(CacheRetention::nowNs());
    CacheRetention::noteStored(*boosted->getEntryHeader(), retention, 1);

    const std::weak_ptr<Image> boostedWeak = boosted;
    const std::weak_ptr<Image> neutralWeak = neutral;
    boosted.reset();
    neutral.reset();

    cache.setMaximumCacheSize((U64)((double)cache.getMemoryCacheSize() / 0.95));
    cache.setMaximumInMemorySize(1.);
    cache.clearExceedingEntries();

    EXPECT_TRUE(neutralWeak.expired());
    EXPECT_FALSE(boostedWeak.expired());
    cache.waitForDeleterThread();
}

TEST_F(CacheRetentionCacheTest, OnlyARenderServedByTheEntryCountsAsReuse)
{
    const AppInstancePtr app = getApp();
    clearEverything(app);

    app->getProject()->setOrAddProjectFormat(Format(0, 0, kWindow.x2, kWindow.y2, "cacheRetention", 1.));
    const NodePtr constant = createConstantIn(app, app->getProject(), kWindow, { 0.25, 0.5, 0.75, 1. });
    ASSERT_TRUE(bool(constant));
    KnobBoolPtr forceCaching = std::dynamic_pointer_cast<KnobBool>(constant->getKnobByName("forceCaching"));
    ASSERT_TRUE(bool(forceCaching));
    forceCaching->setValue(true);

    RenderedPlane plane;
    std::string error;
    ASSERT_TRUE(renderProbe(constant, 1., 0, kWindow, eRenderSchedulerModeLegacy, false, &plane, &error)) << error;

    const ImagePtr entry = std::dynamic_pointer_cast<Image>(appPTR->getNodeCacheHostTier().demotionCandidate());
    ASSERT_TRUE(bool(entry));
    const CacheEntryHeader& header = *entry->getEntryHeader();
    EXPECT_EQ(constant->getRetentionSignals(), header.retention.load());
    EXPECT_EQ(0u, header.reuseCount.load());

    std::list<ImagePtr> found;
    ASSERT_TRUE(appPTR->getImage(entry->getKey(), &found));
    EXPECT_EQ(0u, header.reuseCount.load());

    ASSERT_TRUE(renderProbe(constant, 1., 0, kWindow, eRenderSchedulerModeLegacy, false, &plane, &error)) << error;
    EXPECT_EQ(1u, header.reuseCount.load());

    EXPECT_GT(constant->getRetentionSignals()->priority(CacheRetention::nowNs()), CacheRetention::kForcedWeight);
}

TEST_F(CacheRetentionCacheTest, AKnobEditBoostsADirectInputAndNothingFurtherUp)
{
    const AppInstancePtr app = getApp();
    const NodeCollectionPtr root = app->getProject();
    const NodePtr grandparent = createConstantIn(app, root, kWindow, { 0.25, 0.5, 0.75, 1. });
    const NodePtr parent = createIn(app, root, PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE);
    const NodePtr edited = createIn(app, root, PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE);
    ASSERT_TRUE(grandparent && parent && edited);
    ASSERT_TRUE(parent->connectInput(grandparent, 0));
    ASSERT_TRUE(edited->connectInput(parent, 0));

    // One instant for every reading, so that only a new stamp can tell them apart.
    const std::uint64_t now = CacheRetention::nowNs();
    const double grandparentBefore = grandparent->getRetentionSignals()->priority(now + CacheRetention::kStampGranularityNs);

    ASSERT_TRUE(setKnobValues(edited, kGradeParamWhite, { 2. }));

    EXPECT_GT(parent->getRetentionSignals()->priority(now + CacheRetention::kStampGranularityNs), CacheEviction::kNeutralPriority);
    EXPECT_DOUBLE_EQ(grandparentBefore, grandparent->getRetentionSignals()->priority(now + CacheRetention::kStampGranularityNs));
    EXPECT_DOUBLE_EQ(CacheEviction::kNeutralPriority, edited->getRetentionSignals()->priority(now + CacheRetention::kStampGranularityNs));
}

} // anon namespace
