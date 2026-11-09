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

#include "SceneLoadSet.h"

#include <algorithm>
#include <atomic>
#include <map>

#include "Engine/ContentHasher.h"

NATRON_NAMESPACE_ENTER

namespace {
U64
hashEntries(const std::vector<SceneLoadSet::Entry>& entries)
{
    ContentHasher hasher;

    hasher.appendU64(entries.size());
    for (const SceneLoadSet::Entry& entry : entries) {
        hasher.appendString(entry.path);
        hasher.appendU64(static_cast<U64>(entry.rule));
    }

    return hasher.finalize();
}

thread_local const SceneEvaluationContext* currentContext = nullptr;
} // anonymous namespace

SceneLoadSet::SceneLoadSet()
    : _entries()
    , _hash(hashEntries(_entries))
{
}

SceneLoadSet::SceneLoadSet(const std::vector<Entry>& entries)
    : _entries()
    , _hash(0)
{
    std::map<std::string, RuleEnum> byPath;

    for (const Entry& entry : entries) {
        byPath[entry.path] = entry.rule;
    }
    _entries.reserve(byPath.size());
    for (const std::pair<const std::string, RuleEnum>& entry : byPath) {
        _entries.push_back(Entry { entry.first, entry.second });
    }
    _hash = hashEntries(_entries);
}

SceneLoadSet
SceneLoadSet::none()
{
    return SceneLoadSet(std::vector<Entry>(1, Entry { std::string("/"), eRuleNone }));
}

const std::vector<SceneLoadSet::Entry>&
SceneLoadSet::getEntries() const
{
    return _entries;
}

U64
SceneLoadSet::getHash() const
{
    return _hash;
}

bool
SceneLoadSet::operator==(const SceneLoadSet& other) const
{
    return (_hash == other._hash) && (_entries == other._entries);
}

std::vector<std::string>
SceneLoadSet::getChangedPaths(const SceneLoadSet* a,
                              const SceneLoadSet* b)
{
    static const std::vector<Entry> noEntries;
    const std::vector<Entry>& left = a ? a->_entries : noEntries;
    const std::vector<Entry>& right = b ? b->_entries : noEntries;
    std::vector<std::string> changed;
    std::size_t i = 0, j = 0;

    while ((i < left.size()) || (j < right.size())) {
        if ((j == right.size()) || ((i < left.size()) && (left[i].path < right[j].path))) {
            changed.push_back(left[i++].path);
        } else if ((i == left.size()) || (right[j].path < left[i].path)) {
            changed.push_back(right[j++].path);
        } else {
            if (left[i].rule != right[j].rule) {
                changed.push_back(left[i].path);
            }
            ++i;
            ++j;
        }
    }

    return changed;
}

U64
SceneEvaluationContext::makeId()
{
    static std::atomic<U64> next(1);

    return next++;
}

const SceneEvaluationContext&
SceneEvaluationContext::current()
{
    static const SceneEvaluationContext defaultContext;

    return currentContext ? *currentContext : defaultContext;
}

SceneEvaluationContext::Scope::Scope(const SceneEvaluationContext& context)
    : _previous(currentContext)
    , _context(context)
{
    currentContext = &_context;
}

SceneEvaluationContext::Scope::~Scope()
{
    currentContext = _previous;
}

NATRON_NAMESPACE_EXIT
