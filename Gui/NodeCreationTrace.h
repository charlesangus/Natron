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

#ifndef Gui_NodeCreationTrace_h
#define Gui_NodeCreationTrace_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>

class QEvent;
class QObject;
class QString;

NATRON_NAMESPACE_ENTER

/**
 * @brief Opt-in end-to-end trace of an interactive node creation, from the input event that chose the
 * node to the first frame of its properties panel and on until the main thread goes quiet. Enabled by
 * NATRON_TRACE_NODE_CREATION=1; each creation's report goes to stderr and the Script Editor.
 *
 * Every NodeCreationStageLog stage the main thread runs during a trace is listed with its start since
 * the input event and the main thread's busy and idle time up to that start. Every event the
 * application dispatches is timed as well, so each stretch of more than a few milliseconds between
 * stages is attributed to the events (type and receiver) that ran in it.
 *
 * Without the variable, each dispatched event costs one relaxed load.
 **/
class NodeCreationTrace {
public:
    static bool enabled()
    {
        return s_enabled.load(std::memory_order_relaxed);
    }

    /**
     * @brief Starts tracing when NATRON_TRACE_NODE_CREATION is 1. Call on the main thread once the
     * event dispatcher exists.
     **/
    static bool startFromEnvironment();

    /**
     * @brief Starts a trace of the creation the event being dispatched leads to, timed from the input
     * event (key or mouse) that it came from. A trace still running is reported first.
     **/
    static void begin(const char* origin,
                      const QString& pluginID);

    /**
     * @brief A properties panel painted for the first time: its frame is on screen once the event that
     * painted it has returned.
     **/
    static void notePanelPainted();

    /**
     * @brief Times one event dispatch; QApplication::notify holds one per event.
     **/
    class EventScope {
    public:
        EventScope(QObject* receiver,
                   QEvent* e)
            : _tracked(enabled() && push(receiver, e))
        {
        }

        ~EventScope()
        {
            if (_tracked) {
                pop();
            }
        }

        EventScope(const EventScope&) = delete;
        EventScope& operator=(const EventScope&) = delete;

    private:
        bool _tracked;
    };

private:
    static bool push(QObject* receiver, QEvent* e);
    static void pop();

    static std::atomic<bool> s_enabled;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_NodeCreationTrace_h
