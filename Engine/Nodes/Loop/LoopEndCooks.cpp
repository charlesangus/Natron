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

#include "LoopEndCooks.h"

#include <algorithm>
#include <utility>

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Loop/LoopBegin.h"
#include "Engine/Nodes/Loop/LoopEnd.h"

NATRON_NAMESPACE_ENTER

namespace {

typedef std::pair<Node*, int> WalkStep;

} // anonymous namespace

void
collectLoopPairedBegins(const LoopEndKind& kind,
                        const NodePtr& from,
                        std::set<Node*>* found)
{
    std::set<WalkStep> visited;
    std::vector<std::pair<NodePtr, int>> pending;

    pending.push_back(std::make_pair(from, 0));
    while (!pending.empty()) {
        const NodePtr node = pending.back().first;
        int depth = pending.back().second;
        pending.pop_back();
        if (!node || !visited.insert(WalkStep(node.get(), depth)).second) {
            continue;
        }
        if (kind.isBegin(node)) {
            if (depth == 0) {
                found->insert(node.get());
                continue;
            }
            --depth;
        } else if (kind.isEnd(node)) {
            for (int i = 1, n = node->getNInputs(); i < n; ++i) {
                NodePtr outside = node->getInput(i);
                if (outside) {
                    pending.push_back(std::make_pair(outside, depth));
                }
            }
            NodePtr body = node->getInput(0);
            if (body) {
                pending.push_back(std::make_pair(body, depth + 1));
            }
            continue;
        }
        for (int i = 0, n = node->getNInputs(); i < n; ++i) {
            NodePtr input = node->getInput(i);
            if (input) {
                pending.push_back(std::make_pair(input, depth));
            }
        }
    }
}

void
collectLoopPairedEnds(const LoopEndKind& kind,
                      const NodePtr& begin,
                      std::set<Node*>* found)
{
    std::set<WalkStep> visited;
    std::vector<std::pair<NodePtr, int>> pending;
    std::vector<NodePtr> outputs;

    begin->appendOutputsWithGroupRedirection(&outputs);
    for (NodePtr& output : outputs) {
        pending.push_back(std::make_pair(std::move(output), 0));
    }
    while (!pending.empty()) {
        const NodePtr node = pending.back().first;
        int depth = pending.back().second;
        pending.pop_back();
        if (!node || !visited.insert(WalkStep(node.get(), depth)).second) {
            continue;
        }
        if (kind.isEnd(node)) {
            if (depth == 0) {
                found->insert(node.get());
                continue;
            }
            --depth;
        } else if (kind.isBegin(node)) {
            ++depth;
        }
        outputs.clear();
        node->appendOutputsWithGroupRedirection(&outputs);
        for (NodePtr& output : outputs) {
            pending.push_back(std::make_pair(std::move(output), depth));
        }
    }
}

NodePtr
loopPairedEnd(const LoopEndKind& kind,
              const NodePtr& begin)
{
    if (!begin) {
        return NodePtr();
    }
    std::set<Node*> found;
    collectLoopPairedEnds(kind, begin, &found);
    if (found.size() != 1) {
        return NodePtr();
    }

    return (*found.begin())->shared_from_this();
}

LoopEndPairing::LoopEndPairing(const LoopEndKind& kind)
    : _kind(kind)
{
}

std::vector<NodePtr>
LoopEndPairing::getBegins(const NodePtr& end) const
{
    const U64 generation = LoopEnd::getGraphGeneration();
    {
        QMutexLocker k(&_mutex);
        if (_valid && (_generation == generation)) {
            std::vector<NodePtr> begins;
            bool expired = false;
            for (const NodeWPtr& weak : _begins) {
                NodePtr begin = weak.lock();
                if (!begin) {
                    expired = true;
                    break;
                }
                begins.push_back(std::move(begin));
            }
            if (!expired) {
                return begins;
            }
        }
    }

    std::vector<NodePtr> begins;
    const NodePtr source = end ? end->getInput(0) : NodePtr();
    if (source) {
        std::set<Node*> found;
        collectLoopPairedBegins(_kind, source, &found);
        for (Node* node : found) {
            // Only those that pair with this end alone open its loop.
            NodePtr begin = node->shared_from_this();
            std::set<Node*> ends;
            collectLoopPairedEnds(_kind, begin, &ends);
            if ((ends.size() == 1) && (*ends.begin() == end.get())) {
                begins.push_back(std::move(begin));
            }
        }
    }
    std::sort(begins.begin(), begins.end(), [](const NodePtr& a, const NodePtr& b) { return a->getScriptName_mt_safe() < b->getScriptName_mt_safe(); });

    QMutexLocker k(&_mutex);
    _begins.assign(begins.begin(), begins.end());
    _generation = generation;
    _valid = true;

    return begins;
}

LoopEndCooks::LoopEndCooks(EffectInstance* end)
    : _end(end)
{
}

LoopEndCooks::Scope::Scope(LoopEndCooks* cooks)
    : _cooks(cooks)
{
    bool userInteraction = false;
    AbortableRenderInfoPtr abortInfo;
    EffectInstancePtr treeRoot;
    if (!cooks->_end->getAbortState(&userInteraction, &abortInfo, &treeRoot) || !userInteraction || !abortInfo || !abortInfo->canAbort()) {
        return;
    }
    _cook.startHash = cooks->_end->getContextFreeRenderHash();
    _cook.abortInfo = abortInfo;
    QMutexLocker k(&cooks->_mutex);
    _entry = cooks->_running.insert(cooks->_running.end(), _cook);
    _registered = true;
}

LoopEndCooks::Scope::~Scope()
{
    if (_registered) {
        QMutexLocker k(&_cooks->_mutex);
        _cooks->_running.erase(_entry);
    }
}

bool
LoopEndCooks::Scope::editedSinceStart() const
{
    return _registered && _cooks->abortIfEditedSince(_cook);
}

bool
LoopEndCooks::abortIfEditedSince(const RunningCook& cook) const
{
    const AppInstancePtr app = _end->getApp();
    if (app && app->isDraftRenderEnabled()) {
        return false;
    }
    const NodePtr node = _end->getNode();
    U64 current = 0;
    const bool madeBeforeEdit = cook.abortInfo->getRenderSequence() <= _editedAtRenderSequence.load();
    if (!node || (!madeBeforeEdit && node->getHashValueIfUpToDate(&current) && (current == cook.startHash))) {
        return false;
    }
    cook.abortInfo->setAborted();

    return true;
}

void
LoopEndCooks::onHashChanged(U64 hash)
{
    abortRunning(&hash);
}

void
LoopEndCooks::abortStaleAfterEdit()
{
    noteEditAtLatestRenderSequence(&_editedAtRenderSequence);
    abortRunning(nullptr);
}

void
LoopEndCooks::abortRunning(const U64* keepHash)
{
    const AppInstancePtr app = _end->getApp();
    if (app && app->isDraftRenderEnabled()) {
        return;
    }
    std::vector<AbortableRenderInfoPtr> stale;
    {
        QMutexLocker k(&_mutex);
        for (const RunningCook& cook : _running) {
            if (!keepHash || (cook.startHash != *keepHash)) {
                stale.push_back(cook.abortInfo);
            }
        }
    }
    for (const AbortableRenderInfoPtr& abortInfo : stale) {
        abortInfo->setAborted();
    }
}

NATRON_NAMESPACE_EXIT
