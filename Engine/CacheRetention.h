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

#ifndef NATRON_ENGINE_CACHERETENTION_H
#define NATRON_ENGINE_CACHERETENTION_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

struct CacheEntryHeader;
class CacheRetentionSignals;

namespace CacheRetention {

// Starting values, not measurements. docs/cache/tiered-cache.md says what would move each.
const double kEditWeight = 8.;
const double kPanelWeight = 4.;
const double kBranchWeight = 2.;
const double kExpensiveWeight = 1.;
const double kForcedWeight = 16.;
const double kStructuralWeight = 4.;

const double kEditHalfLifeNs = 120e9;
const double kPanelHalfLifeNs = 60e9;
const double kBranchHalfLifeNs = 600e9;

const float kExpensiveCostNs = 16e6f;
const float kAdmissionCostNs = 16e6f;

// A slider drag and a viewer's every frame both land here. Far below any half-life, so skipping writes within it loses
// nothing, and it keeps a signal's cache line from bouncing between the threads that read it.
const std::uint64_t kStampGranularityNs = 1000000000ULL;

// Past this a signal reads as exactly absent, so a long-idle node is back at the neutral priority rather than a hair
// above it, and the score scan stops paying for the power.
const double kDecayCutoffHalfLives = 8.;

enum ReasonEnum : unsigned {
    eReasonMultipleOutputs = 1u << 0,
    eReasonViewerInput = 1u << 1,
    eReasonRotoInput = 1u << 2,
    eReasonStaticOutput = 1u << 3,
    eReasonOutputPanel = 1u << 4,
    eReasonTemporalAccess = 1u << 5,
    eReasonNoTiles = 1u << 6,
    eReasonNestedRender = 1u << 7,
    eReasonForced = 1u << 8,
    eReasonPreview = 1u << 9,
    eReasonRotoEditing = 1u << 10
};

const unsigned kStructuralReasons = eReasonMultipleOutputs | eReasonRotoInput | eReasonStaticOutput | eReasonTemporalAccess | eReasonNoTiles | eReasonNestedRender | eReasonPreview | eReasonRotoEditing;

// Monotonic: unlike an entry's last use these stamps do not outlive the session, and a wall-clock step must not revive a
// stale signal.
std::uint64_t nowNs();

// Values are the setting's choice indices.
enum AdmissionPolicyEnum {
    eAdmissionPolicyAll = 0,
    eAdmissionPolicyPriorityFloor
};

const AdmissionPolicyEnum kDefaultAdmissionPolicy = eAdmissionPolicyAll;

// Empty for a null or unknown name.
std::optional<AdmissionPolicyEnum> parseAdmissionPolicy(const char* name);

void setAdmissionPolicySetting(AdmissionPolicyEnum policy);
AdmissionPolicyEnum admissionPolicySetting();

// Held apart from the setting so that either can be written first, and so that restoring preferences cannot undo it.
void setAdmissionPolicyOverride(std::optional<AdmissionPolicyEnum> policy);
std::optional<AdmissionPolicyEnum> admissionPolicyOverride();

// NATRON_CACHE_ADMISSION. Read once at startup, since admission runs on every render call.
void readAdmissionPolicyOverrideFromEnvironment();

AdmissionPolicyEnum admissionPolicy();

// Whether an output goes in the cache, rather than into a private image, under the current policy. Asked on every render
// call, so the policy comes from an atomic rather than from the preferences.
bool admit(unsigned reasons, const CacheRetentionSignals& retention);

bool admitUnder(AdmissionPolicyEnum policy,
                unsigned reasons,
                const CacheRetentionSignals& retention,
                std::uint64_t nowNs);

} // namespace CacheRetention

/**
 * @brief What the user's activity says about a node's outputs, read by the eviction score without a lock. Owned jointly by
 * the node and the entries bound to it, because an entry can outlive its node. Every time is passed in, so a caller
 * chooses the clock. Stamps are relaxed: an order between them means nothing, and a late one is off by a scan at most.
 **/
class CacheRetentionSignals {
public:
    CacheRetentionSignals()
        : _inputOfEditedNs(0)
        , _panelNs(0)
        , _branchNs(0)
        , _editNoticeNs(0)
        , _panelScanNs(0)
        , _rememberedCostNs(0.f)
        , _reasons(0)
        , _staticOutput(false)
    {
    }

    CacheRetentionSignals(const CacheRetentionSignals&) = delete;
    CacheRetentionSignals& operator=(const CacheRetentionSignals&) = delete;

    double priority(std::uint64_t nowNs) const;

    // Whether a user's activity still vouches for this node. Leaves out what renderRoI alone notes, the static output,
    // so that the scheduler asking first gets the answer renderRoI gets.
    bool hasLiveStamp(std::uint64_t nowNs) const;

    // True when this edit should reach the node's inputs, which a burst of edits does only once per granularity.
    bool noteEdited(std::uint64_t nowNs);
    void noteInputOfEdited(std::uint64_t nowNs);

    // An edited node's input can be an identity, which holds no entry. The node it passes through inherits the edit's
    // time rather than the current one, so that re-rendering does not renew an old edit.
    void inheritInputOfEdited(const CacheRetentionSignals& consumer);

    void notePanelOfOutput(std::uint64_t nowNs);

    // Whether a node with several outputs should test their panels now. Its admission does not depend on the answer, so
    // it need not pay for one on every render.
    bool panelScanDue(std::uint64_t nowNs);

    void noteViewerBranch(std::uint64_t nowNs);

    void noteReasons(unsigned reasons);

    // Apart from the other reasons because it weighs for every node, while the reason exists only on a single output.
    void noteStaticOutput(bool isStatic);

    void rememberCost(float costNs);
    float rememberedCostNs() const;

private:
    std::atomic<std::uint64_t> _inputOfEditedNs;
    std::atomic<std::uint64_t> _panelNs;
    std::atomic<std::uint64_t> _branchNs;
    std::atomic<std::uint64_t> _editNoticeNs;
    std::atomic<std::uint64_t> _panelScanNs;
    std::atomic<float> _rememberedCostNs;
    std::atomic<unsigned> _reasons;
    std::atomic<bool> _staticOutput;
};

namespace CacheRetention {

// Called only where an entry's pixels serve a render, never from a probe. A render reading the entry several times counts
// once, because the multiplier is about being wanted again, and several consumers in one frame are a structural reason.
// Also moves the entry to the hitting node: entries are shared by content.
void noteHit(CacheEntryHeader& header,
             const CacheRetentionSignalsPtr& holder,
             std::uint64_t renderSequence);

// The render that fills an entry does not then count as its reuse.
void noteStored(CacheEntryHeader& header,
                const CacheRetentionSignalsPtr& holder,
                std::uint64_t renderSequence);

} // namespace CacheRetention

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_CACHERETENTION_H
