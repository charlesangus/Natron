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

#include "CacheRetention.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "Engine/CacheEntry.h"
#include "Engine/CacheTier.h"

NATRON_NAMESPACE_ENTER

namespace {

double
decayed(std::uint64_t stampNs,
        std::uint64_t nowNs,
        double halfLifeNs)
{
    if (stampNs == 0) {
        return 0.;
    }

    const double halfLives = nowNs > stampNs ? (double)(nowNs - stampNs) / halfLifeNs : 0.;

    return halfLives < CacheRetention::kDecayCutoffHalfLives ? std::exp2(-halfLives) : 0.;
}

bool
touchStamp(std::atomic<std::uint64_t>& stamp,
           std::uint64_t nowNs)
{
    const std::uint64_t previous = stamp.load(std::memory_order_relaxed);

    if ((previous != 0) && (nowNs < previous + CacheRetention::kStampGranularityNs)) {
        return false;
    }
    stamp.store(nowNs, std::memory_order_relaxed);

    return true;
}

const int kNoAdmissionOverride = -1;

std::atomic<int> s_admissionSetting { (int)CacheRetention::kDefaultAdmissionPolicy };
std::atomic<int> s_admissionOverride { kNoAdmissionOverride };

} // namespace

namespace CacheRetention {

std::uint64_t
nowNs()
{
    return (std::uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::optional<AdmissionPolicyEnum>
parseAdmissionPolicy(const char* name)
{
    if (!name) {
        return std::nullopt;
    }
    if (std::strcmp(name, "all") == 0) {
        return eAdmissionPolicyAll;
    }
    if (std::strcmp(name, "priorityFloor") == 0) {
        return eAdmissionPolicyPriorityFloor;
    }

    return std::nullopt;
}

void
setAdmissionPolicySetting(AdmissionPolicyEnum policy)
{
    s_admissionSetting.store((int)policy, std::memory_order_relaxed);
}

AdmissionPolicyEnum
admissionPolicySetting()
{
    return (AdmissionPolicyEnum)s_admissionSetting.load(std::memory_order_relaxed);
}

void
setAdmissionPolicyOverride(std::optional<AdmissionPolicyEnum> policy)
{
    s_admissionOverride.store(policy ? (int)*policy : kNoAdmissionOverride, std::memory_order_relaxed);
}

std::optional<AdmissionPolicyEnum>
admissionPolicyOverride()
{
    const int policy = s_admissionOverride.load(std::memory_order_relaxed);

    return policy == kNoAdmissionOverride ? std::nullopt : std::optional<AdmissionPolicyEnum>((AdmissionPolicyEnum)policy);
}

void
readAdmissionPolicyOverrideFromEnvironment()
{
    const char* value = std::getenv("NATRON_CACHE_ADMISSION");
    const std::optional<AdmissionPolicyEnum> policy = parseAdmissionPolicy(value);

    // A mistyped name would otherwise pass for the setting's policy in a benchmark comparing the two.
    if (value && !policy) {
        std::cerr << "NATRON_CACHE_ADMISSION: unknown policy \"" << value << "\", expected all or priorityFloor" << std::endl;
    }
    setAdmissionPolicyOverride(policy);
}

AdmissionPolicyEnum
admissionPolicy()
{
    const std::optional<AdmissionPolicyEnum> forced = admissionPolicyOverride();

    return forced ? *forced : admissionPolicySetting();
}

bool
admit(unsigned reasons,
      const CacheRetentionSignals& retention)
{
    return admitUnder(admissionPolicy(), reasons, retention, nowNs());
}

bool
admitUnder(AdmissionPolicyEnum policy,
           unsigned reasons,
           const CacheRetentionSignals& retention,
           std::uint64_t nowNs)
{
    if (policy == eAdmissionPolicyAll) {
        return true;
    }

    return (reasons != 0) || (retention.rememberedCostNs() >= kAdmissionCostNs) || retention.hasLiveStamp(nowNs);
}

void
noteHit(CacheEntryHeader& header,
        const CacheRetentionSignalsPtr& holder,
        std::uint64_t renderSequence)
{
    // A private image is not an entry, and has no key.
    if (!header.key) {
        return;
    }
    if (header.retention.load() != holder) {
        header.retention.store(holder);
    }
    // A render with no abort info has no sequence to tell its reads apart, so each one counts.
    if ((renderSequence == 0) || (header.lastReuseRender.exchange(renderSequence, std::memory_order_relaxed) != renderSequence)) {
        header.reuseCount.fetch_add(1, std::memory_order_relaxed);
    }
}

void
noteStored(CacheEntryHeader& header,
           const CacheRetentionSignalsPtr& holder,
           std::uint64_t renderSequence)
{
    if (!header.key) {
        return;
    }
    if (header.retention.load() != holder) {
        header.retention.store(holder);
    }
    header.lastReuseRender.store(renderSequence, std::memory_order_relaxed);
}

} // namespace CacheRetention

double
CacheRetentionSignals::priority(std::uint64_t nowNs) const
{
    using namespace CacheRetention;

    double priority = CacheEviction::kNeutralPriority;

    priority += kEditWeight * decayed(_inputOfEditedNs.load(std::memory_order_relaxed), nowNs, kEditHalfLifeNs);
    priority += kPanelWeight * decayed(_panelNs.load(std::memory_order_relaxed), nowNs, kPanelHalfLifeNs);
    priority += kBranchWeight * decayed(_branchNs.load(std::memory_order_relaxed), nowNs, kBranchHalfLifeNs);

    const unsigned reasons = _reasons.load(std::memory_order_relaxed);
    if (reasons & eReasonForced) {
        priority += kForcedWeight;
    }
    if ((reasons & kStructuralReasons) || _staticOutput.load(std::memory_order_relaxed)) {
        priority += kStructuralWeight;
    }
    if (_rememberedCostNs.load(std::memory_order_relaxed) >= kExpensiveCostNs) {
        priority += kExpensiveWeight;
    }

    return priority;
}

bool
CacheRetentionSignals::hasLiveStamp(std::uint64_t nowNs) const
{
    using namespace CacheRetention;

    return (decayed(_inputOfEditedNs.load(std::memory_order_relaxed), nowNs, kEditHalfLifeNs) > 0.)
        || (decayed(_panelNs.load(std::memory_order_relaxed), nowNs, kPanelHalfLifeNs) > 0.)
        || (decayed(_branchNs.load(std::memory_order_relaxed), nowNs, kBranchHalfLifeNs) > 0.);
}

bool
CacheRetentionSignals::noteEdited(std::uint64_t nowNs)
{
    return touchStamp(_editNoticeNs, nowNs);
}

void
CacheRetentionSignals::noteInputOfEdited(std::uint64_t nowNs)
{
    touchStamp(_inputOfEditedNs, nowNs);
}

void
CacheRetentionSignals::inheritInputOfEdited(const CacheRetentionSignals& consumer)
{
    const std::uint64_t edited = consumer._inputOfEditedNs.load(std::memory_order_relaxed);

    if (edited > _inputOfEditedNs.load(std::memory_order_relaxed)) {
        _inputOfEditedNs.store(edited, std::memory_order_relaxed);
    }
}

void
CacheRetentionSignals::notePanelOfOutput(std::uint64_t nowNs)
{
    touchStamp(_panelNs, nowNs);
}

bool
CacheRetentionSignals::panelScanDue(std::uint64_t nowNs)
{
    return touchStamp(_panelScanNs, nowNs);
}

void
CacheRetentionSignals::noteViewerBranch(std::uint64_t nowNs)
{
    touchStamp(_branchNs, nowNs);
}

void
CacheRetentionSignals::noteReasons(unsigned reasons)
{
    if (_reasons.load(std::memory_order_relaxed) != reasons) {
        _reasons.store(reasons, std::memory_order_relaxed);
    }
}

void
CacheRetentionSignals::noteStaticOutput(bool isStatic)
{
    if (_staticOutput.load(std::memory_order_relaxed) != isStatic) {
        _staticOutput.store(isStatic, std::memory_order_relaxed);
    }
}

void
CacheRetentionSignals::rememberCost(float costNs)
{
    _rememberedCostNs.store(costNs, std::memory_order_relaxed);
}

float
CacheRetentionSignals::rememberedCostNs() const
{
    return _rememberedCostNs.load(std::memory_order_relaxed);
}

NATRON_NAMESPACE_EXIT
