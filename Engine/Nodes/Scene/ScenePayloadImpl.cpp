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

#include "ScenePayloadImpl.h"

#include <functional>
#include <unordered_set>

NATRON_NAMESPACE_ENTER

namespace {
struct DrawRuleHash {
    std::size_t operator()(const SceneDrawRule* rule) const
    {
        const std::hash<std::string> hashString;
        std::size_t h = hashString(rule->pattern);
        const auto mix = [&h](std::size_t value) { h ^= value + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
        mix((std::size_t)rule->state);
        mix((std::size_t)rule->levels);
        mix(hashString(rule->toKind));
        mix(hashString(rule->origin));
        for (const std::string& layer : rule->layers) {
            mix(hashString(layer));
        }

        return h;
    }
};

struct DrawRuleEqual {
    bool operator()(const SceneDrawRule* a,
                    const SceneDrawRule* b) const
    {
        return *a == *b;
    }
};
} // anonymous namespace

void
appendSceneDrawRules(const SceneDrawRules& rules,
                     SceneDrawRules* to)
{
    if (rules.empty()) {
        return;
    }
    if (to->empty()) {
        *to = rules;

        return;
    }
    // Reserved first, so the set's pointers into *to stay valid as it grows. Only the rules *to
    // held before are looked up: a repeat within rules stays, as of two rows the later one wins.
    to->reserve(to->size() + rules.size());
    std::unordered_set<const SceneDrawRule*, DrawRuleHash, DrawRuleEqual> held;
    held.reserve(to->size());
    for (const SceneDrawRule& rule : *to) {
        held.insert(&rule);
    }
    for (const SceneDrawRule& rule : rules) {
        if (held.find(&rule) == held.end()) {
            to->push_back(rule);
        }
    }
}

ScenePayload::ScenePayload()
    : _imp()
    , _drawRules()
{
}

ScenePayload::ScenePayload(const std::shared_ptr<const ScenePayloadImpl>& imp)
    : _imp(imp)
    , _drawRules()
{
}

ScenePayload::~ScenePayload()
{
}

bool
ScenePayload::isEmpty() const
{
    return !_imp;
}

std::size_t
ScenePayload::getNumLayers() const
{
    return _imp ? _imp->numLayers : 0;
}

U64
ScenePayload::getStackHash() const
{
    return _imp ? _imp->stackHash : 0;
}

const SceneMetadata&
ScenePayload::getMetadata() const
{
    static const SceneMetadata fallback;

    return _imp ? _imp->metadata : fallback;
}

bool
ScenePayload::isSameStack(const ScenePayload& other) const
{
    const ScenePayloadImpl* a = _imp.get();
    const ScenePayloadImpl* b = other._imp.get();

    // Stacks that share a base share everything below it, so the walk stops at the first
    // common link rather than at the bottom.
    while (a != b) {
        if (!a || !b || (a->numLayers != b->numLayers) || (a->top.layer != b->top.layer)) {
            return false;
        }
        a = a->base.get();
        b = b->base.get();
    }

    return true;
}

const SceneDrawRules&
ScenePayload::getDrawRules() const
{
    static const SceneDrawRules none;

    return _drawRules ? *_drawRules : none;
}

ScenePayload
ScenePayload::withDrawRules(const SceneDrawRules& rules) const
{
    ScenePayload payload(_imp);

    if (!rules.empty()) {
        payload._drawRules = std::make_shared<const SceneDrawRules>(rules);
    }

    return payload;
}

NATRON_NAMESPACE_EXIT
