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

#include "NodeGraphKiss.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

NATRON_NAMESPACE_ENTER

namespace {

struct KissCandidate {
    KissPlan plan;
    std::size_t sourceIndex;
    std::size_t targetIndex;
};

bool
kissCandidateLess(const KissCandidate& a,
                  const KissCandidate& b)
{
    if (a.plan.distance != b.plan.distance) {
        return a.plan.distance < b.plan.distance;
    }
    if (a.plan.inputNb != b.plan.inputNb) {
        return a.plan.inputNb < b.plan.inputNb;
    }
    if (a.plan.end != b.plan.end) {
        return a.plan.end == eKissEndInput;
    }
    if (a.sourceIndex != b.sourceIndex) {
        return a.sourceIndex < b.sourceIndex;
    }

    return a.targetIndex < b.targetIndex;
}

} // anonymous namespace

double
kissDistanceToTarget(double x,
                     double y,
                     const KissTarget& target)
{
    const double dx = std::max(0., std::max(target.left - x, x - target.right));
    const double dy = std::max(0., std::max(target.top - y, y - target.bottom));

    return std::sqrt(dx * dx + dy * dy);
}

namespace {

void
addInputCandidate(const std::vector<KissInputTip>& inputs,
                  std::size_t i,
                  const std::vector<KissTarget>& targets,
                  std::size_t t,
                  double reach,
                  std::vector<KissCandidate>* candidates)
{
    const KissInputTip& tip = inputs[i];
    const KissTarget& target = targets[t];
    if (tip.connected || (tip.nodeId == target.id)) {
        return;
    }
    const double distance = kissDistanceToTarget(tip.x, tip.y, target);
    if (distance > reach) {
        return;
    }
    KissCandidate c;
    c.plan.end = eKissEndInput;
    c.plan.draggedId = tip.nodeId;
    c.plan.targetId = target.id;
    c.plan.inputNb = tip.inputNb;
    c.plan.distance = distance;
    c.sourceIndex = i;
    c.targetIndex = t;
    candidates->push_back(c);
}

void
addOutputCandidate(const std::vector<KissOutput>& outputs,
                   std::size_t o,
                   const std::vector<KissTarget>& targets,
                   std::size_t t,
                   double reach,
                   std::vector<KissCandidate>* candidates)
{
    const KissOutput& output = outputs[o];
    const KissTarget& target = targets[t];
    if ((target.freeInput < 0) || (output.nodeId == target.id)) {
        return;
    }
    const double distance = kissDistanceToTarget(output.x, output.y, target);
    if (distance > reach) {
        return;
    }
    KissCandidate c;
    c.plan.end = eKissEndOutput;
    c.plan.draggedId = output.nodeId;
    c.plan.targetId = target.id;
    c.plan.inputNb = target.freeInput;
    c.plan.distance = distance;
    c.sourceIndex = o;
    c.targetIndex = t;
    candidates->push_back(c);
}

KissPlan
bestValidKiss(std::vector<KissCandidate>* candidates,
              const KissValidator& isValid)
{
    std::sort(candidates->begin(), candidates->end(), kissCandidateLess);
    for (std::size_t i = 0; i < candidates->size(); ++i) {
        if (!isValid || isValid((*candidates)[i].plan)) {
            return (*candidates)[i].plan;
        }
    }

    return KissPlan();
}

} // anonymous namespace

KissPlan
planKiss(const std::vector<KissInputTip>& inputs,
         const std::vector<KissOutput>& outputs,
         const std::vector<KissTarget>& targets,
         double reach,
         const KissValidator& isValid)
{
    std::vector<KissCandidate> candidates;

    for (std::size_t i = 0; i < inputs.size(); ++i) {
        for (std::size_t t = 0; t < targets.size(); ++t) {
            addInputCandidate(inputs, i, targets, t, reach, &candidates);
        }
    }
    for (std::size_t o = 0; o < outputs.size(); ++o) {
        for (std::size_t t = 0; t < targets.size(); ++t) {
            addOutputCandidate(outputs, o, targets, t, reach, &candidates);
        }
    }

    return bestValidKiss(&candidates, isValid);
}

KissPlan
planKiss(const std::vector<KissInputTip>& inputs,
         const std::vector<std::vector<std::size_t>>& inputNear,
         const std::vector<KissOutput>& outputs,
         const std::vector<std::vector<std::size_t>>& outputNear,
         const std::vector<KissTarget>& targets,
         double reach,
         const KissValidator& isValid)
{
    std::vector<KissCandidate> candidates;

    for (std::size_t i = 0; (i < inputs.size()) && (i < inputNear.size()); ++i) {
        for (std::size_t n = 0; n < inputNear[i].size(); ++n) {
            if (inputNear[i][n] < targets.size()) {
                addInputCandidate(inputs, i, targets, inputNear[i][n], reach, &candidates);
            }
        }
    }
    for (std::size_t o = 0; (o < outputs.size()) && (o < outputNear.size()); ++o) {
        for (std::size_t n = 0; n < outputNear[o].size(); ++n) {
            if (outputNear[o][n] < targets.size()) {
                addOutputCandidate(outputs, o, targets, outputNear[o][n], reach, &candidates);
            }
        }
    }

    return bestValidKiss(&candidates, isValid);
}

NATRON_NAMESPACE_EXIT
