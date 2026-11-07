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

#ifndef Engine_RenderEndWatch_h
#define Engine_RenderEndWatch_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <chrono>
#include <functional>
#include <memory>

NATRON_NAMESPACE_ENTER

/**
 * @brief Tells its owner when the viewer renders requested on a thread while it is active start and end. The viewer
 * scheduler takes a ticket for each render it starts and drops it once the render is over, finished or aborted.
 *
 * started runs synchronously on the requesting thread when the ticket is taken; ended runs on whichever thread drops the
 * last reference to the ticket, with the time the render took in milliseconds.
 **/
class RenderEndWatch {
public:
    typedef std::function<void()> StartedCallback;
    typedef std::function<void(double)> EndedCallback;

    class Ticket {
    public:
        explicit Ticket(const std::shared_ptr<RenderEndWatch>& watch)
            : _watch(watch)
            , _start(std::chrono::steady_clock::now())
        {
        }

        Ticket(const Ticket&) = delete;
        Ticket& operator=(const Ticket&) = delete;

        ~Ticket()
        {
            std::shared_ptr<RenderEndWatch> watch = _watch.lock();
            if (watch && watch->_ended) {
                watch->_ended(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _start).count());
            }
        }

    private:
        std::weak_ptr<RenderEndWatch> _watch;
        std::chrono::steady_clock::time_point _start;
    };

    typedef std::shared_ptr<Ticket> TicketPtr;

    RenderEndWatch(const StartedCallback& started,
                   const EndedCallback& ended)
        : _started(started)
        , _ended(ended)
    {
    }

    /**
     * @brief Makes watch the one whose tickets requests on the calling thread take, until another call replaces it. The
     * slot holds a weak reference, so a destroyed watch deactivates itself.
     **/
    static void setActiveOnThisThread(const std::shared_ptr<RenderEndWatch>& watch)
    {
        activeSlot() = watch;
    }

    /**
     * @brief A ticket on the calling thread's active watch, or null when none is active.
     **/
    static TicketPtr takeTicket()
    {
        std::shared_ptr<RenderEndWatch> watch = activeSlot().lock();
        if (!watch) {
            return TicketPtr();
        }
        TicketPtr ticket = std::make_shared<Ticket>(watch);
        if (watch->_started) {
            watch->_started();
        }

        return ticket;
    }

private:
    static std::weak_ptr<RenderEndWatch>& activeSlot()
    {
        thread_local std::weak_ptr<RenderEndWatch> slot;

        return slot;
    }

    const StartedCallback _started;
    const EndedCallback _ended;
};

typedef std::shared_ptr<RenderEndWatch> RenderEndWatchPtr;

NATRON_NAMESPACE_EXIT

#endif // Engine_RenderEndWatch_h
