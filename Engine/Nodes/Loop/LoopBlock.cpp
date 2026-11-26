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

#include "LoopBlock.h"

#include <algorithm>
#include <set>

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Loop/GeoForEachBegin.h"
#include "Engine/Nodes/Loop/GeoForEachEnd.h"
#include "Engine/Nodes/Loop/GeoSolverBegin.h"
#include "Engine/Nodes/Loop/GeoSolverEnd.h"
#include "Engine/Nodes/Loop/LoopBegin.h"
#include "Engine/Nodes/Loop/LoopEnd.h"
#include "Engine/Project.h"
#include "Engine/RenderContext.h"

NATRON_NAMESPACE_ENTER

LoopBlockEnd::~LoopBlockEnd()
{
}

LoopBlockEnd*
LoopBlockEnd::of(const NodePtr& node)
{
    const EffectInstancePtr effect = node ? node->getEffectInstance() : EffectInstancePtr();

    return dynamic_cast<LoopBlockEnd*>(effect.get());
}

namespace LoopBlocks {
namespace {

    void
    collectEnds(const NodePtr& from,
                std::set<Node*>* found)
    {
        for (const LoopEndKind& kind : kinds()) {
            collectLoopPairedEnds(kind, from, found);
        }
    }

    std::string
    nameOf(const NodePtr& end)
    {
        const LoopBlockEnd* block = LoopBlockEnd::of(end);
        const KnobStringPtr knob = block ? block->getBlockNameKnob() : KnobStringPtr();
        const std::string value = knob ? knob->getValue() : std::string();

        // An empty geometry block name stands for the script name.
        return value.empty() ? end->getScriptName_mt_safe() : value;
    }

} // anonymous namespace

const std::vector<LoopEndKind>&
kinds()
{
    static const std::vector<LoopEndKind> all = {
        { &LoopBegin::isLoopBegin, &LoopEnd::isLoopEnd },
        { &GeoForEachBegin::isGeoForEachBegin, &GeoForEachEnd::isGeoForEachEnd },
        { &GeoSolverBegin::isGeoSolverBegin, &GeoSolverEnd::isGeoSolverEnd },
    };

    return all;
}

bool
isBegin(const NodePtr& node)
{
    for (const LoopEndKind& kind : kinds()) {
        if (kind.isBegin(node)) {
            return true;
        }
    }

    return false;
}

bool
isEnd(const NodePtr& node)
{
    for (const LoopEndKind& kind : kinds()) {
        if (kind.isEnd(node)) {
            return true;
        }
    }

    return false;
}

std::vector<NodePtr>
enclosingEnds(const NodePtr& node)
{
    std::vector<NodePtr> ends;
    std::set<Node*> seen;
    std::vector<NodePtr> from;

    if (node) {
        from.push_back(node);
    }
    while (!from.empty()) {
        std::set<Node*> candidates;
        for (const NodePtr& start : from) {
            collectEnds(start, &candidates);
        }
        for (Node* end : seen) {
            candidates.erase(end);
        }
        // A walk of one kind passes the ends of the others, so a solver around a for-each is
        // found along with it: the ends another candidate's walk reaches come at a later level.
        // A lone candidate is never outer to itself, so its walks are skipped: driven knobs ask
        // for their enclosing ends on each read outside a render.
        std::set<Node*> outer;
        if (candidates.size() > 1) {
            for (Node* candidate : candidates) {
                std::set<Node*> above;
                collectEnds(candidate->shared_from_this(), &above);
                for (Node* end : above) {
                    if (end != candidate) {
                        outer.insert(end);
                    }
                }
            }
        }
        std::vector<NodePtr> next;
        for (Node* candidate : candidates) {
            if (!outer.count(candidate)) {
                next.push_back(candidate->shared_from_this());
            }
        }
        // Blocks wired into a cycle enclose one another: take them all at once.
        if (next.empty()) {
            for (Node* candidate : candidates) {
                next.push_back(candidate->shared_from_this());
            }
        }
        for (const NodePtr& end : next) {
            seen.insert(end.get());
        }
        std::sort(next.begin(), next.end(), [](const NodePtr& a, const NodePtr& b) {
            return a->getScriptName_mt_safe() < b->getScriptName_mt_safe();
        });
        ends.insert(ends.end(), next.begin(), next.end());
        from.swap(next);
    }

    return ends;
}

std::vector<NodePtr>
bodyNodes(const NodePtr& end)
{
    std::vector<NodePtr> body;
    const NodeCollectionPtr group = end ? end->getGroup() : NodeCollectionPtr();

    if (!group) {
        return body;
    }
    const NodesList nodes = group->getNodes();
    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if (*it == end) {
            continue;
        }
        const std::vector<NodePtr> ends = enclosingEnds(*it);
        if (std::find(ends.begin(), ends.end(), end) != ends.end()) {
            body.push_back(*it);
        }
    }

    return body;
}

RenderContextPtr
previewContextOf(const NodePtr& node,
                 double time)
{
    const std::vector<NodePtr> ends = enclosingEnds(node);
    RenderContextPtr context;

    for (std::vector<NodePtr>::const_reverse_iterator it = ends.rbegin(); it != ends.rend(); ++it) {
        const LoopBlockEnd* block = LoopBlockEnd::of(*it);
        if (block) {
            context = block->getBlockPreviewContext(context, time);
        }
    }

    return context;
}

const std::vector<std::string>&
allVariables()
{
    static const std::vector<std::string> names = [] {
        std::vector<std::string> all;
        const std::vector<std::string>* lists[] = { &LoopEnd::loopVariables(), &GeoForEachEnd::loopVariables(), &GeoSolverEnd::loopVariables() };
        for (const std::vector<std::string>* list : lists) {
            for (const std::string& name : *list) {
                if (std::find(all.begin(), all.end(), name) == all.end()) {
                    all.push_back(name);
                }
            }
        }

        return all;
    }();

    return names;
}

std::string
freeName(const NodePtr& end,
         const std::string& current)
{
    const NodeCollectionPtr group = end ? end->getGroup() : NodeCollectionPtr();
    std::set<std::string> taken;

    if (group) {
        const NodesList nodes = group->getNodes();
        for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
            if ((*it != end) && LoopBlockEnd::of(*it)) {
                taken.insert(nameOf(*it));
            }
        }
    }
    if (!current.empty() && !taken.count(current)) {
        return current;
    }
    for (int i = 1;; ++i) {
        const std::string candidate = "loop" + std::to_string(i);
        if (!taken.count(candidate)) {
            return candidate;
        }
    }
}

void
assignFreeName(const EffectInstance* end)
{
    const LoopBlockEnd* block = dynamic_cast<const LoopBlockEnd*>(end);
    const KnobStringPtr name = block ? block->getBlockNameKnob() : KnobStringPtr();
    const AppInstancePtr app = end ? end->getApp() : AppInstancePtr();

    if (!name || !app || app->getProject()->isLoadingProject()) {
        return;
    }
    const std::string current = name->getValue();
    const std::string free = freeName(end->getNode(), current);
    if (free != current) {
        name->setValue(free);
    }
}
} // namespace LoopBlocks

NATRON_NAMESPACE_EXIT
