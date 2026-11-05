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

#ifndef Engine_Nodes_Transform_SplineWarp_SplineWarpCache_h
#define Engine_Nodes_Transform_SplineWarp_SplineWarpCache_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <functional>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <utility>

NATRON_NAMESPACE_ENTER

/**
 * @brief A small MT-safe most-recently-used cache. A value is computed once: threads asking for a
 * key being computed wait for it rather than computing it again, and other keys are not held up.
 **/
template <typename Key, typename Value>
class SplineWarpLruCache {
public:
    typedef std::shared_ptr<const Value> ValuePtr;

    explicit SplineWarpLruCache(std::size_t capacity = 4)
        : _capacity(capacity)
    {
    }

    ValuePtr get(const Key& key,
                 const std::function<Value()>& compute)
    {
        std::shared_future<ValuePtr> future;
        std::shared_ptr<std::promise<ValuePtr>> promise;
        {
            std::lock_guard<std::mutex> l(_mutex);
            for (typename std::list<Entry>::iterator it = _entries.begin(); it != _entries.end(); ++it) {
                if (it->first == key) {
                    _entries.splice(_entries.begin(), _entries, it);
                    future = it->second;
                    break;
                }
            }
            if (!future.valid()) {
                promise = std::make_shared<std::promise<ValuePtr>>();
                future = promise->get_future().share();
                _entries.push_front(Entry(key, future));
                while (_entries.size() > _capacity) {
                    _entries.pop_back();
                }
            }
        }
        if (promise) {
            try {
                promise->set_value(std::make_shared<const Value>(compute()));
            } catch (...) {
                promise->set_exception(std::current_exception());
                remove(key);
            }
        }

        return future.get();
    }

    void clear()
    {
        std::lock_guard<std::mutex> l(_mutex);

        _entries.clear();
    }

    // The cached keys, most recently used first.
    std::list<Key> getKeys() const
    {
        std::lock_guard<std::mutex> l(_mutex);
        std::list<Key> keys;

        for (typename std::list<Entry>::const_iterator it = _entries.begin(); it != _entries.end(); ++it) {
            keys.push_back(it->first);
        }

        return keys;
    }

private:
    typedef std::pair<Key, std::shared_future<ValuePtr>> Entry;

    void remove(const Key& key)
    {
        std::lock_guard<std::mutex> l(_mutex);

        for (typename std::list<Entry>::iterator it = _entries.begin(); it != _entries.end(); ++it) {
            if (it->first == key) {
                _entries.erase(it);

                return;
            }
        }
    }

    std::size_t _capacity;
    mutable std::mutex _mutex;
    std::list<Entry> _entries;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_SplineWarp_SplineWarpCache_h
