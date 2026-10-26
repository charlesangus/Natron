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

#include "IsolatedJob.h"

#include <list>
#include <mutex>

#include <QtCore/QtGlobal>

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"
#include "Engine/TrackMarker.h"
#include "Engine/TrackerContext.h"

NATRON_NAMESPACE_ENTER

namespace {

// The jobs running in this process.
std::mutex&
runningJobsMutex()
{
    static std::mutex mutex;

    return mutex;
}

std::list<const IsolatedJob*>&
runningJobs()
{
    static std::list<const IsolatedJob*> jobs;

    return jobs;
}

// The job whose context the calling thread is in, if any. Only ever compared, never dereferenced.
thread_local const IsolatedJob* tCurrentJob = NULL;

std::string
holderName(const KnobHolder& holder)
{
    const EffectInstance* effect = dynamic_cast<const EffectInstance*>(&holder);
    if (effect) {
        NodePtr node = effect->getNode();
        if (node) {
            return node->getFullyQualifiedName();
        }
    }
    const NamedKnobHolder* named = dynamic_cast<const NamedKnobHolder*>(&holder);
    if (named) {
        return named->getFullyQualifiedName();
    }
    if (dynamic_cast<const Project*>(&holder)) {
        return "Project";
    }

    return std::string();
}

} // anon namespace

IsolatedJob::IsolatedJob(const NodePtr& root,
                         const std::string& kind)
    : _kind(kind)
    , _holders()
    , _counted()
{
    // The format, frame rate, views and colour settings every render reads.
    if (root && root->getApp()) {
        addHolder(root->getApp()->getProject());
    }

    std::set<Node*> walked;
    std::list<NodePtr> toWalk;
    toWalk.push_back(root);

    // Nodes reached only through expressions and links feed knob values, not images: they are
    // read, but their inputs are not. A node whose metadata a parameter reads is walked like an
    // input, since its metadata follows from everything upstream of it.
    std::set<NodePtr> readByKnobs;
    std::set<NodePtr> metadataSources;

    while (!toWalk.empty()) {
        NodePtr node = toWalk.front();
        toWalk.pop_front();
        if (!node || !walked.insert(node.get()).second) {
            continue;
        }
        addNode(node);
        EffectInstancePtr effect = node->getEffectInstance();
        if (effect) {
            metadataSources.clear();
            effect->getAllExpressionDependenciesRecursive(readByKnobs, &metadataSources);
            toWalk.insert(toWalk.end(), metadataSources.begin(), metadataSources.end());
        }
        NodeGroupPtr group = std::dynamic_pointer_cast<NodeGroup>(node->getGroup());
        if (group) {
            toWalk.push_back(group->getNode());
        }
        const int nInputs = node->getNInputs();
        for (int i = 0; i < nInputs; ++i) {
            // The real input keeps a group in the walk, the redirected one steps into it or out of
            // a GroupInput.
            toWalk.push_back(node->getRealInput(i));
            toWalk.push_back(node->getInput(i));
        }
    }

    for (std::set<NodePtr>::const_iterator it = readByKnobs.begin(); it != readByKnobs.end(); ++it) {
        if (*it && (walked.find(it->get()) == walked.end())) {
            addNode(*it);
        }
    }

    // Registered before the holders count it, so a holder that reports a reader always has a
    // running job to name.
    {
        std::lock_guard<std::mutex> k(runningJobsMutex());
        runningJobs().push_back(this);
    }
    for (std::size_t i = 0; i < _counted.size(); ++i) {
        KnobHolderPtr holder = _counted[i].lock();
        if (holder) {
            holder->_isolatedJobReaders.fetch_add(1, std::memory_order_acq_rel);
        }
    }
}

IsolatedJob::~IsolatedJob()
{
    for (std::size_t i = 0; i < _counted.size(); ++i) {
        KnobHolderPtr holder = _counted[i].lock();
        if (holder) {
            holder->_isolatedJobReaders.fetch_sub(1, std::memory_order_acq_rel);
        }
    }
    std::lock_guard<std::mutex> k(runningJobsMutex());
    runningJobs().remove(this);
}

void
IsolatedJob::addHolder(const KnobHolderPtr& holder)
{
    if (holder && _holders.insert(holder.get()).second) {
        _counted.push_back(holder);
    }
}

void
IsolatedJob::addNode(const NodePtr& node)
{
    addHolder(node->getEffectInstance());
    TrackerContextPtr tracker = node->getTrackerContext();
    if (tracker) {
        std::vector<TrackMarkerPtr> markers;
        tracker->getAllMarkers(&markers);
        for (std::size_t i = 0; i < markers.size(); ++i) {
            addHolder(markers[i]);
        }
    }
}

bool
IsolatedJob::reads(const KnobHolder* holder) const
{
    return _holders.find(holder) != _holders.end();
}

IsolatedJob::ThreadScope::ThreadScope(const IsolatedJob* job)
    : _previous(tCurrentJob)
    , _entered(job != NULL)
{
    if (_entered) {
        tCurrentJob = job;
    }
}

IsolatedJob::ThreadScope::~ThreadScope()
{
    if (_entered) {
        tCurrentJob = _previous;
    }
}

const IsolatedJob*
IsolatedJob::current()
{
    return tCurrentJob;
}

const IsolatedJob*
IsolatedJob::exchangeCurrent(const IsolatedJob* job)
{
    const IsolatedJob* previous = tCurrentJob;

    tCurrentJob = job;

    return previous;
}

std::string
IsolatedJob::refusal(const KnobHolder& holder,
                     const KnobI* knob,
                     const char* what)
{
    // Only reached once a job reads the holder, so the knob check stays off the common path.
    if (knob && !knob->isReadByRenders()) {
        return std::string();
    }
    const IsolatedJob* own = tCurrentJob;
    std::string kind;
    {
        std::lock_guard<std::mutex> k(runningJobsMutex());
        const std::list<const IsolatedJob*>& jobs = runningJobs();
        for (std::list<const IsolatedJob*>::const_iterator it = jobs.begin(); it != jobs.end(); ++it) {
            if ((*it != own) && (*it)->reads(&holder)) {
                kind = (*it)->_kind;
                break;
            }
        }
    }
    if (kind.empty()) {
        return std::string();
    }
    std::string name = holderName(holder);
    if (knob) {
        if (!name.empty()) {
            name += '.';
        }
        name += knob->getName();
    } else {
        name = std::string(what ? what : "") + " of " + name;
    }

    return "Cannot change " + name + " while a " + kind + " reads it";
}

bool
IsolatedJob::logRefusal(const KnobHolder& holder,
                        const KnobI* knob,
                        const char* what)
{
    const std::string message = refusal(holder, knob, what);
    if (message.empty()) {
        return false;
    }
    qWarning("%s", message.c_str());

    return true;
}

bool
IsolatedJob::raiseRefusalInPython(const KnobHolder& holder,
                                  const KnobI* knob,
                                  const char* what)
{
    const std::string message = refusal(holder, knob, what);
    if (message.empty()) {
        return false;
    }
    PyErr_SetString(PyExc_RuntimeError, message.c_str());

    return true;
}

bool
isWriteRefusedByIsolatedJob(const KnobI* knob)
{
    return IsolatedJob::refusesWrite(knob);
}

NATRON_NAMESPACE_EXIT
