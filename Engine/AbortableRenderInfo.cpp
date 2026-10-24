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

#include "AbortableRenderInfo.h"

#include <atomic>
#include <set>
#include <sstream>
#include <string>

#include <QAtomicInt>
#include <QMutex>

#include "Engine/AbortWatchdog.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/ThreadPool.h"

NATRON_NAMESPACE_ENTER

typedef std::set<AbortableThread*> ThreadSet;

static std::atomic<U64> s_latestRenderSequence(0);

struct AbortableRenderInfoPrivate {
    bool canAbort;
    QAtomicInt aborted;
    U64 age;
    U64 sequence;
    std::atomic<bool> superseded;
    mutable QMutex threadsMutex;
    ThreadSet threadsForThisRender;

    AbortableRenderInfoPrivate(bool canAbort,
                               U64 age)
        : canAbort(canAbort)
        , aborted(0)
        , age(age)
        , sequence(++s_latestRenderSequence)
        , superseded(false)
        , threadsMutex()
        , threadsForThisRender()
    {
    }
};

AbortableRenderInfo::AbortableRenderInfo()
    : _imp(new AbortableRenderInfoPrivate(true, 0))
{
}

AbortableRenderInfo::AbortableRenderInfo(bool canAbort,
                                         U64 age)
    : _imp(new AbortableRenderInfoPrivate(canAbort, age))
{
}

AbortableRenderInfo::~AbortableRenderInfo()
{
}

U64
AbortableRenderInfo::getRenderAge() const
{
    return _imp->age;
}

U64
AbortableRenderInfo::getRenderSequence() const
{
    return _imp->sequence;
}

U64
AbortableRenderInfo::getLatestRenderSequence()
{
    return s_latestRenderSequence.load();
}

void
AbortableRenderInfo::setSuperseded()
{
    _imp->superseded.store(true);
}

bool
AbortableRenderInfo::isSuperseded() const
{
    return _imp->superseded.load();
}

bool
AbortableRenderInfo::canAbort() const
{
    return _imp->canAbort;
}

bool
AbortableRenderInfo::isAborted() const
{
    return (int)_imp->aborted > 0;
}

void
AbortableRenderInfo::setAborted()
{
    int abortedValue = _imp->aborted.fetchAndAddAcquire(1);

    if (abortedValue > 0) {
        return;
    }
    AbortWatchdog::watch(weak_from_this());
}

void
AbortableRenderInfo::registerThreadForRender(AbortableThread* thread)
{
    QMutexLocker k(&_imp->threadsMutex);

    _imp->threadsForThisRender.insert(thread);
}

bool
AbortableRenderInfo::unregisterThreadForRender(AbortableThread* thread)
{
    QMutexLocker k(&_imp->threadsMutex);
    ThreadSet::iterator found = _imp->threadsForThisRender.find(thread);

    if (found == _imp->threadsForThisRender.end()) {
        return false;
    }
    _imp->threadsForThisRender.erase(found);

    return true;
}

bool
AbortableRenderInfo::getStallReport(std::string* report) const
{
    ThreadSet threads;
    {
        QMutexLocker k(&_imp->threadsMutex);
        threads = _imp->threadsForThisRender;
    }

    std::stringstream ss;
    bool hasAtLeastOneThreadInNodeAction = false;
    for (ThreadSet::const_iterator it = threads.begin(); it != threads.end(); ++it) {
        std::string actionName;
        NodePtr node;
        (*it)->getCurrentActionInfos(&actionName, &node);
        if (!node) {
            continue;
        }
        hasAtLeastOneThreadInNodeAction = true;
        if (node->getEffectInstance()->isReader() || node->getEffectInstance()->isWriter()) {
            return false;
        }
        const std::string nodeName = node->getFullyQualifiedName();
        const std::string pluginId = node->getPluginID();

        ss << " - " << (*it)->getThreadName() << tr(" stalled in:").toStdString() << std::endl
           << std::endl;
        if (!nodeName.empty()) {
            ss << "    Node: " << nodeName << std::endl;
        }
        if (!pluginId.empty()) {
            ss << "    Plugin: " << pluginId << std::endl;
        }
        if (!actionName.empty()) {
            ss << "    Action: " << actionName << std::endl;
        }
        ss << std::endl;
    }
    if (!hasAtLeastOneThreadInNodeAction) {
        return false;
    }
    *report = ss.str();

    return true;
} // AbortableRenderInfo::getStallReport

void
AbortableRenderInfo::killRegisteredThreads()
{
    QMutexLocker k(&_imp->threadsMutex);

    for (ThreadSet::const_iterator it = _imp->threadsForThisRender.begin(); it != _imp->threadsForThisRender.end(); ++it) {
        (*it)->killThread();
    }
}

NATRON_NAMESPACE_EXIT
