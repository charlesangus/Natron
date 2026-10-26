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

#ifndef NATRON_ENGINE_ISOLATEDJOB_H
#define NATRON_ENGINE_ISOLATEDJOB_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <set>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Knob.h"
#include "Global/Enums.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A render or a track that must read the graph as it was when it started, from start to
 * end. While one lives, the knob holders it reads refuse content writes (values, keyframes,
 * expressions, links) made outside the job's own contexts. A refused write changes nothing: the
 * knob's entry points return as if nothing changed and log a warning naming the knob, and the
 * Python parameter bindings raise a RuntimeError before they record or change anything.
 *
 * The holders it reads are the project, its root node, every node upstream of it through inputs
 * (into and out of groups), the groups holding those nodes, the nodes their expressions and links
 * read, the nodes whose metadata their parameters read together with everything upstream of
 * those, and the tracker nodes and tracks among them or read by their expressions and links.
 *
 * The nodes it reads also refuse graph edits from outside the job: connecting, replacing,
 * swapping or disconnecting their inputs, and editing their Roto shapes from Python.
 *
 * A thread is in a job's context while a ThreadScope for that job lives on it, while it runs a
 * task of a frame created in the job's context (AppTLS::FrameContextScope), and while it runs work
 * a thread in the job's context handed out (AppTLS::SpawnedThreadScope). A write from a job's
 * context to a holder no other job reads goes through.
 *
 * Checking a write to a holder no job reads costs one atomic load (KnobHolder::isReadByIsolatedJob()).
 **/
class IsolatedJob {
public:
    /**
     * @brief Starts refusing writes to what root reads. kind names the job in the refusal
     * message ("render", "track").
     **/
    IsolatedJob(const NodePtr& root,
                const std::string& kind);

    ~IsolatedJob();

    IsolatedJob(const IsolatedJob&) = delete;
    IsolatedJob& operator=(const IsolatedJob&) = delete;

    bool reads(const KnobHolder* holder) const;

    /**
     * @brief Puts the calling thread in job's context while it lives, restoring the context it
     * had before. A null job leaves the thread's context as it is.
     **/
    class ThreadScope {
    public:
        explicit ThreadScope(const IsolatedJob* job);
        ~ThreadScope();

        ThreadScope(const ThreadScope&) = delete;
        ThreadScope& operator=(const ThreadScope&) = delete;

        /**
         * @brief Makes the destructor leave the context of the thread that made this scope as it
         * is, for a scope destroyed on another thread: restoring would change the destroying
         * thread's context instead.
         **/
        void detach()
        {
            _entered = false;
        }

    private:
        const IsolatedJob* _previous;
        bool _entered;
    };

    /**
     * @brief The job whose context the calling thread is in, or null. A thread handing work to
     * other threads reads it to put them in the same context.
     **/
    static const IsolatedJob* current();

    /**
     * @brief Puts the calling thread in job's context, or in none for a null job, and returns the
     * context it was in, for scopes that cannot hold a ThreadScope to restore it with.
     **/
    static const IsolatedJob* exchangeCurrent(const IsolatedJob* job);

    /**
     * @brief True, after logging a warning naming knob, if a running job other than the calling
     * thread's reads knob's holder: the caller then leaves knob as it is.
     **/
    static bool refusesWrite(const KnobI* knob)
    {
        KnobHolder* holder = knob ? knob->getHolder() : NULL;

        return holder && holder->isReadByIsolatedJob() && logRefusal(*holder, knob, NULL);
    }

    /**
     * @brief refusesWrite() for a value or keyframe set for reason. Values a plug-in sets from its
     * own actions, a time change refreshes and a master pushes to its slaves follow from a write
     * or a render already under way, so they are never refused.
     **/
    static bool refusesValueWrite(const KnobI* knob,
                                  ValueChangedReasonEnum reason)
    {
        return (reason != eValueChangedReasonPluginEdited) && (reason != eValueChangedReasonTimeChanged) && (reason != eValueChangedReasonSlaveRefresh) && refusesWrite(knob);
    }

    /**
     * @brief refusesWrite() for a write a Python script asks for: when refused, sets a Python
     * RuntimeError naming knob instead of logging. Call with the GIL held.
     **/
    static bool refusesPythonWrite(const KnobI* knob)
    {
        KnobHolder* holder = knob ? knob->getHolder() : NULL;

        return holder && holder->isReadByIsolatedJob() && raiseRefusalInPython(*holder, knob, NULL);
    }

    /**
     * @brief True, after logging a warning, if a running job other than the calling thread's
     * reads holder: the caller then leaves holder's inputs or shapes as they are. what names the
     * change, as in "Cannot change <what> of <holder> while a render reads it".
     **/
    static bool refusesEdit(const KnobHolder* holder,
                            const char* what)
    {
        return holder && holder->isReadByIsolatedJob() && logRefusal(*holder, NULL, what);
    }

    /**
     * @brief refusesEdit() for an edit a Python script asks for: when refused, sets a Python
     * RuntimeError instead of logging. Call with the GIL held.
     **/
    static bool refusesPythonEdit(const KnobHolder* holder,
                                  const char* what)
    {
        return holder && holder->isReadByIsolatedJob() && raiseRefusalInPython(*holder, NULL, what);
    }

private:
    // Why a change from the calling thread is refused, or empty if it is not: a write to knob
    // when knob is set, otherwise the edit what of holder.
    static std::string refusal(const KnobHolder& holder, const KnobI* knob, const char* what);
    static bool logRefusal(const KnobHolder& holder, const KnobI* knob, const char* what);
    static bool raiseRefusalInPython(const KnobHolder& holder, const KnobI* knob, const char* what);

    void addHolder(const KnobHolderPtr& holder);
    // The node's effect and, for a tracker, its tracks.
    void addNode(const NodePtr& node);

    std::string _kind;
    std::set<const KnobHolder*> _holders;
    std::vector<KnobHolderWPtr> _counted;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_ISOLATEDJOB_H
