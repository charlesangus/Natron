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

#include "TLSHolder.h"
#include "TLSHolderImpl.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>

#include "Engine/OfxClipInstance.h"
#include "Engine/OfxHost.h"
#include "Engine/OfxParamInstance.h"
#include "Engine/Project.h"
#include "Engine/ThreadPool.h"

#include <QWaitCondition>
#include <QThread>
#include <QDebug>

NATRON_NAMESPACE_ENTER

NATRON_NAMESPACE_ANONYMOUS_ENTER

// Bounds the walk up the spawn map; render threads never nest anywhere near this deep.
const int kMaxSpawnerChainDepth = 16;

// Holders with data for the current thread, so that cleaning up a thread visits only those
// instead of every holder of the application.
thread_local std::vector<TLSHolderBaseConstWPtr> tHoldersWithData;
thread_local std::size_t tHoldersPruneThreshold = 64;
thread_local std::size_t tNumInheritedCopies = 0;
thread_local const FrameRenderContext* tCurrentFrameContext = 0;
thread_local int tThreadBudget = 0;
thread_local int tRunnablePriority = 0;

NATRON_NAMESPACE_ANONYMOUS_EXIT

static void
copyAbortInfo(QThread* fromThread,
              QThread* toThread)
{
    AbortableThread* fromAbortable = dynamic_cast<AbortableThread*>(fromThread);
    AbortableThread* toAbortable = dynamic_cast<AbortableThread*>(toThread);

    if (fromAbortable && toAbortable) {
        bool isRenderResponseToUserInteraction;
        AbortableRenderInfoPtr abortInfo;
        EffectInstancePtr treeRoot;
        fromAbortable->getAbortInfo(&isRenderResponseToUserInteraction, &abortInfo, &treeRoot);
        toAbortable->setAbortInfo(isRenderResponseToUserInteraction, abortInfo, treeRoot);
    }
}

void
AppTLS::recordHolderForCurrentThread(const TLSHolderBaseConstWPtr& holder)
{
    // Threads that never clean up, such as the main thread, would otherwise keep a dead entry per
    // holder ever destroyed.
    if (tHoldersWithData.size() >= tHoldersPruneThreshold) {
        tHoldersWithData.erase(std::remove_if(tHoldersWithData.begin(), tHoldersWithData.end(),
                                              [](const TLSHolderBaseConstWPtr& w) { return w.expired(); }),
                               tHoldersWithData.end());
        tHoldersPruneThreshold = std::max<std::size_t>(64, tHoldersWithData.size() * 2);
    }
    tHoldersWithData.push_back(holder);
}

std::size_t
AppTLS::getNumInheritedCopies()
{
    return tNumInheritedCopies;
}

void
AppTLS::notifyInheritedCopy()
{
    ++tNumInheritedCopies;
}

std::size_t
AppTLS::getNumHoldersWithDataForCurrentThread()
{
    return (std::size_t)std::count_if(tHoldersWithData.begin(), tHoldersWithData.end(),
                                      [](const TLSHolderBaseConstWPtr& w) { return !w.expired(); });
}

const FrameRenderContext*
AppTLS::currentFrameContext()
{
    return tCurrentFrameContext;
}

int
AppTLS::currentThreadBudget()
{
    return tThreadBudget;
}

int
AppTLS::currentRunnablePriority()
{
    return tRunnablePriority;
}

void
AppTLS::cleanupHoldersOfCurrentThread(const QThread* curThread)
{
    std::vector<TLSHolderBaseConstWPtr> holders;

    holders.swap(tHoldersWithData);
    tHoldersPruneThreshold = 64;
    for (std::vector<TLSHolderBaseConstWPtr>::const_iterator it = holders.begin(); it != holders.end(); ++it) {
        TLSHolderBaseConstPtr p = it->lock();
        if (p) {
            p->cleanupPerThreadData(curThread);
        }
    }
}

void
AppTLS::softCopy(QThread* fromThread,
                 QThread* toThread,
                 SpawnKindEnum kind)
{
    if ( (fromThread == toThread) || !fromThread || !toThread ) {
        return;
    }
    assert(toThread == QThread::currentThread());

    copyAbortInfo(fromThread, toThread);

    // A pool thread whose previous task skipped its cleanup would otherwise find that task's data
    // instead of inheriting the spawner's.
    cleanupHoldersOfCurrentThread(toThread);

    SpawnEntry entry;
    entry.spawner = fromThread;
    entry.kind = kind;
    QWriteLocker k(&_spawnsMutex);
    _spawns[uintptr_t(toThread)] = entry;
}

void
AppTLS::getSpawnerChain(const QThread* curThread,
                        SpawnerChain* chain) const
{
    chain->clear();

    QReadLocker k(&_spawnsMutex);
    const ThreadSpawnMap& spawnsCRef = _spawns; // take a const ref, since it's a read lock
    const QThread* thread = curThread;
    for (int depth = 0; depth < kMaxSpawnerChainDepth; ++depth) {
        ThreadSpawnMap::const_iterator found = spawnsCRef.find(uintptr_t(thread));
        if (found == spawnsCRef.end()) {
            break;
        }
        const QThread* spawner = found->second.spawner;
        if (spawner == curThread) {
            break;
        }
        bool visited = false;
        for (SpawnerChain::const_iterator it = chain->begin(); it != chain->end(); ++it) {
            if (it->thread == spawner) {
                visited = true;
                break;
            }
        }
        if (visited) {
            break;
        }
        SpawnerLink link;
        link.thread = spawner;
        link.kind = found->second.kind;
        chain->push_back(link);
        thread = spawner;
    }
}

void
AppTLS::cleanupTLSForThread()
{
    QThread* curThread = QThread::currentThread();
    AbortableThread* isAbortableThread = dynamic_cast<AbortableThread*>(curThread);

    if (isAbortableThread) {
        isAbortableThread->clearAbortInfo();
    }

    {
        QWriteLocker l(&_spawnsMutex);
        _spawns.erase(uintptr_t(curThread));
    }

    cleanupHoldersOfCurrentThread(curThread);
}

AppTLS::SpawnedThreadScope::SpawnedThreadScope(QThread* fromThread,
                                               const FrameRenderContext* frameContext,
                                               SpawnKindEnum kind)
    : _spawned(fromThread && fromThread != QThread::currentThread())
    , _previousFrameContext(tCurrentFrameContext)
    , _previousBudget(tThreadBudget)
{
    if (_spawned) {
        appPTR->getAppTLS()->softCopy(fromThread, QThread::currentThread(), kind);
        tCurrentFrameContext = frameContext;
        // The spawner already counted this thread as one of its own; nesting more parallelism here oversubscribes.
        tThreadBudget = 1;
    }
}

AppTLS::SpawnedThreadScope::~SpawnedThreadScope()
{
    if (_spawned) {
        appPTR->getAppTLS()->cleanupTLSForThread();
        tCurrentFrameContext = _previousFrameContext;
        tThreadBudget = _previousBudget;
    }
}

AppTLS::FrameContextScope::FrameContextScope(const FrameRenderContext* context,
                                             int budget,
                                             int runnablePriority)
    : _previous(tCurrentFrameContext)
    , _previousBudget(tThreadBudget)
    , _previousPriority(tRunnablePriority)
{
    tCurrentFrameContext = context;
    tThreadBudget = budget;
    tRunnablePriority = runnablePriority;
}

AppTLS::FrameContextScope::~FrameContextScope()
{
    tCurrentFrameContext = _previous;
    tThreadBudget = _previousBudget;
    tRunnablePriority = _previousPriority;
    // A task run inline from within another task's scope must not drop the data the outer task is rendering with.
    if (!_previous) {
        appPTR->getAppTLS()->cleanupTLSForThread();
    }
}

AppTLS::ThreadBudgetScope::ThreadBudgetScope(int budget)
    : _previous(tThreadBudget)
{
    tThreadBudget = budget;
}

AppTLS::ThreadBudgetScope::~ThreadBudgetScope()
{
    tThreadBudget = _previous;
}

template class TLSHolder<EffectInstance::EffectTLSData>;
template class TLSHolder<NATRON_NAMESPACE::OfxHost::OfxHostTLSData>;
template class TLSHolder<KnobHelper::KnobTLSData>;
template class TLSHolder<Project::ProjectTLSData>;
template class TLSHolder<OfxClipInstance::ClipTLSData>;
template class TLSHolder<OfxParamToKnob::OfxParamTLSData>;

NATRON_NAMESPACE_EXIT

