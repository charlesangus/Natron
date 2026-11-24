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

typedef std::map<std::string, SceneLoadSet::RuleEnum> RulesByPath;

std::string
parentPath(const std::string& path)
{
    const std::string::size_type slash = path.rfind('/');

    return ((slash == 0) || (slash == std::string::npos)) ? std::string("/") : path.substr(0, slash);
}

// The rule of the nearest path at or above path, or null if none is.
const SceneLoadSet::RuleEnum*
nearestRule(const RulesByPath& rules,
            const std::string& path)
{
    for (std::string at = path;;) {
        const RulesByPath::const_iterator it = rules.find(at);
        if (it != rules.end()) {
            return &it->second;
        }
        if (at == "/") {
            return nullptr;
        }
        at = parentPath(at);
    }
}

// Whether a prim strictly below the paths of rules nearest above it has its payload included.
bool
includesBelow(const SceneLoadSet::RuleEnum* rule)
{
    return !rule || (*rule == SceneLoadSet::eRuleAll);
}

// Whether the prim at path, and a prim below it with no rule of its own, have their payloads included.
void
getInclusion(const RulesByPath& rules,
             const std::string& path,
             bool* self,
             bool* below)
{
    const SceneLoadSet::RuleEnum* rule = nearestRule(rules, path);
    const RulesByPath::const_iterator own = rules.find(path);

    *below = includesBelow(rule);
    if (own != rules.end()) {
        *self = (own->second != SceneLoadSet::eRuleNone);
    } else {
        *self = *below;
    }
}

SceneLoadSet::RuleEnum
ruleFor(bool self,
        bool below)
{
    if (below) {
        return SceneLoadSet::eRuleAll;
    }

    return self ? SceneLoadSet::eRuleOnly : SceneLoadSet::eRuleNone;
}

// rules as a load set without the rules that change nothing, null when none is left.
SceneLoadSetPtr
withoutRedundantRules(const RulesByPath& rules)
{
    std::vector<SceneLoadSet::Entry> entries;
    entries.reserve(rules.size());
    for (const std::pair<const std::string, SceneLoadSet::RuleEnum>& entry : rules) {
        const SceneLoadSet::RuleEnum* inherited = (entry.first == "/") ? nullptr : nearestRule(rules, parentPath(entry.first));
        const bool inheritedAll = includesBelow(inherited);
        const bool redundant = (inheritedAll && (entry.second == SceneLoadSet::eRuleAll)) || (!inheritedAll && (entry.second == SceneLoadSet::eRuleNone));
        if (!redundant) {
            entries.push_back(SceneLoadSet::Entry { entry.first, entry.second });
        }
    }
    if (entries.empty()) {
        return SceneLoadSetPtr();
    }

    return std::make_shared<const SceneLoadSet>(entries);
}
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

SceneLoadSetPtr
SceneLoadSet::everything()
{
    return std::make_shared<const SceneLoadSet>();
}

SceneLoadSetPtr
SceneLoadSet::withPaths(const SceneLoadSetPtr& base,
                        const std::vector<std::string>& paths)
{
    if (!base || base->getEntries().empty()) {
        return base;
    }

    std::map<std::string, RuleEnum> byPath;

    for (const Entry& entry : base->getEntries()) {
        byPath[entry.path] = entry.rule;
    }

    for (const std::string& path : paths) {
        std::string::size_type end = 0;

        while (end != std::string::npos) {
            end = path.find('/', end + 1);
            const std::string prefix = path.substr(0, end);
            if (prefix.empty()) {
                continue;
            }

            // The nearest rule at or above prefix decides, none at all meaning everything; a
            // subtree already loaded in full stays so.
            const bool includedInFull = includesBelow(nearestRule(byPath, prefix));
            if (!includedInFull) {
                byPath[prefix] = eRuleOnly;
            }
        }
    }

    std::vector<Entry> entries;
    entries.reserve(byPath.size());
    for (const std::pair<const std::string, RuleEnum>& entry : byPath) {
        entries.push_back(Entry { entry.first, entry.second });
    }

    return std::make_shared<const SceneLoadSet>(entries);
}

SceneLoadSetPtr
SceneLoadSet::unite(const SceneLoadSetPtr& a,
                    const SceneLoadSetPtr& b)
{
    if (!a || a->getEntries().empty() || !b || b->getEntries().empty()) {
        return SceneLoadSetPtr();
    }
    if ((a == b) || (*a == *b)) {
        return a;
    }

    RulesByPath left, right;
    for (const Entry& entry : a->getEntries()) {
        left[entry.path] = entry.rule;
    }
    for (const Entry& entry : b->getEntries()) {
        right[entry.path] = entry.rule;
    }

    // A prim's inclusion is decided by the nearest rule of either set at or above it, so the
    // union only needs a rule on the paths either set has one on.
    RulesByPath united;
    for (const RulesByPath* rules : { &left, &right }) {
        for (const std::pair<const std::string, RuleEnum>& entry : *rules) {
            if (united.count(entry.first)) {
                continue;
            }
            bool selfA, belowA, selfB, belowB;
            getInclusion(left, entry.first, &selfA, &belowA);
            getInclusion(right, entry.first, &selfB, &belowB);
            united[entry.first] = ruleFor(selfA || selfB, belowA || belowB);
        }
    }

    return withoutRedundantRules(united);
}

SceneLoadSetPtr
SceneLoadSet::minimal(const std::vector<Entry>& entries)
{
    RulesByPath rules;
    for (const Entry& entry : entries) {
        rules[entry.path] = entry.rule;
    }

    return withoutRedundantRules(rules);
}

SceneLoadSet::RuleEnum
SceneLoadSet::getRuleAt(const std::string& path) const
{
    RulesByPath rules;
    for (const Entry& entry : _entries) {
        rules[entry.path] = entry.rule;
    }
    bool self, below;
    getInclusion(rules, path, &self, &below);

    return ruleFor(self, below);
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

void
SceneLoadReach::add(const std::vector<std::string>& prefixes)
{
    std::lock_guard<std::mutex> lock(_mutex);

    for (const std::string& prefix : prefixes) {
        if (std::find(_prefixes.begin(), _prefixes.end(), prefix) == _prefixes.end()) {
            _prefixes.push_back(prefix);
        }
    }
}

std::vector<std::string>
SceneLoadReach::getPrefixes() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _prefixes;
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
