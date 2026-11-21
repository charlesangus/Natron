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

#include "Serialization/Merge/ThreeWayMerge.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <optional>
#include <random>
#include <tuple>

#include "Serialization/Merge/MergePrivate.h"
#include "Serialization/Merge/MergeRules.h"
#include "Serialization/Merge/MergeScope.h"
#include "Serialization/Yaml/BlobSink.h"
#include "Serialization/Yaml/ProjectYaml.h"
#include "Serialization/Yaml/YamlScalars.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

using MergeDetail::knobNamed;
using MergeDetail::knobsByName;

namespace {

    std::string
    randomNodeId()
    {
        static thread_local std::mt19937_64 generator(std::random_device {}());
        char text[33];
        std::snprintf(text, sizeof(text), "%016llx%016llx", (unsigned long long)generator(), (unsigned long long)generator());

        return std::string(text);
    }

    void
    walkTree(const std::list<NodeSerializationPtr>& nodes,
             std::map<std::string, int>* order)
    {
        for (const NodeSerializationPtr& node : nodes) {
            if (!node->id.empty()) {
                order->emplace(node->id, int(order->size()));
            }
            walkTree(node->children, order);
        }
    }

    std::map<std::string, std::string>
    reverse(const std::map<std::string, std::string>& map)
    {
        std::map<std::string, std::string> result;
        for (const std::pair<const std::string, std::string>& entry : map) {
            result[entry.second] = entry.first;
        }

        return result;
    }

    // Name renames, by group, that drop any name two nodes of a group would rename differently.
    class RenameBuilder {
    public:
        void add(const std::string& group,
                 const std::string& from,
                 const std::string& to)
        {
            std::set<std::string>& ambiguous = _ambiguous[group];
            if (ambiguous.count(from)) {
                return;
            }
            NodeRenames& renames = _renames[group];
            std::pair<NodeRenames::iterator, bool> inserted = renames.emplace(from, to);
            if (!inserted.second && (inserted.first->second != to)) {
                renames.erase(inserted.first);
                ambiguous.insert(from);
            }
        }

        ScopedNodeRenames take()
        {
            for (ScopedNodeRenames::iterator scope = _renames.begin(); scope != _renames.end();) {
                for (NodeRenames::iterator it = scope->second.begin(); it != scope->second.end();) {
                    if (it->first == it->second) {
                        it = scope->second.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (scope->second.empty()) {
                    scope = _renames.erase(scope);
                } else {
                    ++scope;
                }
            }

            return std::move(_renames);
        }

    private:
        ScopedNodeRenames _renames;
        std::map<std::string, std::set<std::string>> _ambiguous;
    };

    std::vector<std::string>
    defaultedKnobs(const std::list<KnobSerialization>& base,
                   const std::list<KnobSerialization>& side)
    {
        const std::map<std::string, const KnobSerialization*> sideKnobs = knobsByName(side);
        std::vector<std::string> result;
        for (const KnobSerialization& knob : base) {
            const KnobSerialization* other = knobNamed(sideKnobs, knob.name);
            if (!isDefaultKnob(knob) && (!other || isDefaultKnob(*other))) {
                result.push_back(knob.name);
            }
        }

        return result;
    }

    const TemplateImportSerialization*
    templateImportNamed(const std::list<TemplateImportSerialization>& imports,
                        const std::string& key)
    {
        for (const TemplateImportSerialization& entry : imports) {
            if (entry.key == key) {
                return &entry;
            }
        }

        return nullptr;
    }

    std::string
    templateBaseText(const TemplateImportSerialization& entry)
    {
        if (!entry.base) {
            return std::string();
        }
        try {
            InlineBlobSink sink;

            return encodeProjectYaml(*entry.base, sink);
        } catch (const YamlError& e) {
            return std::string("unencodable: ") + e.what();
        }
    }

    bool
    sameTemplateImport(const TemplateImportSerialization* a,
                       const TemplateImportSerialization* b)
    {
        if (!a || !b) {
            return !a && !b;
        }

        return (a->path == b->path) && (a->contentHash == b->contentHash) && (a->importedAt == b->importedAt) && (templateBaseText(*a) == templateBaseText(*b));
    }

    // Each import as a whole: theirs' when mine left it as base had it, else mine's. The result
    // keeps mine's order, with the imports only theirs has after them.
    std::list<TemplateImportSerialization>
    mergeTemplateImports(const std::list<TemplateImportSerialization>& base,
                         const std::list<TemplateImportSerialization>& mine,
                         const std::list<TemplateImportSerialization>& theirs)
    {
        std::vector<std::string> keys;
        for (const std::list<TemplateImportSerialization>* side : { &mine, &theirs, &base }) {
            for (const TemplateImportSerialization& entry : *side) {
                if (std::find(keys.begin(), keys.end(), entry.key) == keys.end()) {
                    keys.push_back(entry.key);
                }
            }
        }
        std::list<TemplateImportSerialization> merged;
        for (const std::string& key : keys) {
            const TemplateImportSerialization* mineEntry = templateImportNamed(mine, key);
            const TemplateImportSerialization* chosen = sameTemplateImport(mineEntry, templateImportNamed(base, key)) ? templateImportNamed(theirs, key) : mineEntry;
            if (chosen) {
                merged.push_back(*chosen);
            }
        }

        return merged;
    }

} // namespace

namespace MergeDetail {

    MergeContext::MergeContext(const ProjectSerialization& baseProject,
                               const ProjectSerialization& mineProject_,
                               const ProjectSerialization& theirsProject_,
                               const MergePlan& plan_,
                               const MergeOptions& options)
        : mineProject(mineProject_)
        , theirsProject(theirsProject_)
        , base(baseProject)
        , mine(mineProject_)
        , theirs(theirsProject_)
        , plan(plan_)
        , handlers(options.handlers ? *options.handlers : MergeHandlerRegistry::defaults())
        , newNodeId(options.newNodeId ? options.newNodeId : std::function<std::string()>(randomNodeId))
        , mineToBase(reverse(plan_.baseToMine.baseToOther))
        , theirsToBase(reverse(plan_.baseToTheirs.baseToOther))
    {
        walkTree(mineProject.nodes.nodes, &mineOrder);
        walkTree(theirsProject.nodes.nodes, &theirsOrder);
        for (const std::pair<const std::string, MergeGraph::Node>& entry : mine.nodes()) {
            OutNode node;
            node.node = std::make_shared<NodeSerialization>(*entry.second.node);
            node.node->children.clear();
            node.node->inputs.clear();
            node.parent = entry.second.parentId;
            node.inputIds = entry.second.inputs;
            node.order = std::make_pair(0, mineOrder[entry.first]);
            node.fromMine = true;
            names[node.parent].insert(node.node->scriptName);
            out.emplace(entry.first, std::move(node));
        }
    }

    std::string
    MergeContext::baseOfMine(const std::string& mineId) const
    {
        std::map<std::string, std::string>::const_iterator found = mineToBase.find(mineId);

        return (found == mineToBase.end()) ? std::string() : found->second;
    }

    std::string
    MergeContext::baseOfTheirs(const std::string& theirsId) const
    {
        std::map<std::string, std::string>::const_iterator found = theirsToBase.find(theirsId);

        return (found == theirsToBase.end()) ? std::string() : found->second;
    }

    std::string
    MergeContext::uniqueName(const std::string& group,
                             const std::string& wanted) const
    {
        static const std::set<std::string> none;
        std::map<std::string, std::set<std::string>>::const_iterator found = names.find(group);
        const std::set<std::string>& taken = (found == names.end()) ? none : found->second;
        if (!taken.count(wanted)) {
            return wanted;
        }
        std::size_t stemEnd = wanted.size();
        while ((stemEnd > 0) && std::isdigit((unsigned char)wanted[stemEnd - 1])) {
            --stemEnd;
        }
        const std::string stem = wanted.substr(0, stemEnd);
        int number = (stemEnd < wanted.size()) ? std::atoi(wanted.c_str() + stemEnd) + 1 : 2;
        while (taken.count(stem + std::to_string(number))) {
            ++number;
        }

        return stem + std::to_string(number);
    }

    std::string
    MergeContext::addFromTheirs(const std::string& theirsId)
    {
        const MergeGraph::Node* source = theirs.find(theirsId);
        OutNode node;
        node.node = std::make_shared<NodeSerialization>(*source->node);
        node.node->children.clear();
        node.node->inputs.clear();
        if (plan.templateKey.empty()) {
            // Both sides of a project share its IDs, so the node keeps the one later merges match on.
            node.node->id = (out.count(theirsId) || mine.find(theirsId)) ? newNodeId() : theirsId;
        } else {
            node.node->id = newNodeId();
            node.node->origin.templateKey = plan.templateKey;
            node.node->origin.nodeId = theirsId;
        }
        if (!source->parentId.empty()) {
            std::map<std::string, std::string>::const_iterator parent = theirsToOut.find(source->parentId);
            if (parent != theirsToOut.end()) {
                node.parent = parent->second;
            }
        }
        node.node->scriptName = uniqueName(node.parent, source->node->scriptName);
        if (source->node->label == source->node->scriptName) {
            node.node->label = node.node->scriptName;
        }
        node.order = std::make_pair(1, theirsOrder[theirsId]);
        node.fromMine = false;
        node.theirsId = theirsId;
        const std::string id = node.node->id;
        names[node.parent].insert(node.node->scriptName);
        theirsToOut[theirsId] = id;
        out.emplace(id, std::move(node));

        return id;
    }

    void
    MergeContext::addConflict(MergeConflictKind kind,
                              const std::string& baseId,
                              MergeSide side)
    {
        MergeConflict conflict;
        conflict.kind = kind;
        conflict.baseNodeId = baseId;
        std::map<std::string, std::string>::const_iterator found = baseToOut.find(baseId);
        if (found != baseToOut.end()) {
            conflict.nodeId = found->second;
        }
        conflict.side = side;
        conflicts.push_back(conflict);
    }

    std::string
    inBaseTerms(const MergeContext& context,
                const std::string& id,
                bool fromTheirs)
    {
        if (id.empty()) {
            return std::string();
        }
        const std::string baseId = fromTheirs ? context.baseOfTheirs(id) : context.baseOfMine(id);
        if (!baseId.empty()) {
            return baseId;
        }

        return (fromTheirs ? "theirs:" : "mine:") + id;
    }

    std::string
    groupPath(const MergeGraph& graph,
              const MergeGraph::Node& node)
    {
        const MergeGraph::Node* parent = node.parentId.empty() ? nullptr : graph.find(node.parentId);

        return parent ? parent->path : std::string();
    }

    ScopedNodeRenames
    sideNames(const MergeGraph& from,
              const MergeGraph& to,
              const std::map<std::string, std::string>& fromToTo)
    {
        RenameBuilder builder;
        for (const std::pair<const std::string, std::string>& entry : fromToTo) {
            const MergeGraph::Node* a = from.find(entry.first);
            const MergeGraph::Node* b = to.find(entry.second);
            if (a && b) {
                builder.add(groupPath(from, *a), a->node->scriptName, b->node->scriptName);
            }
        }

        return builder.take();
    }

    std::list<KnobSerialization>
    renamedKnobs(const MergeGraph& graph,
                 const MergeGraph::Node& node,
                 const ScopedNodeRenames& renames)
    {
        std::list<KnobSerialization> knobs = node.node->knobs;
        if (!renames.empty()) {
            renameNodeReferences(&knobs, renames, groupPath(graph, node), node.node->scriptName);
        }

        return knobs;
    }

    namespace {

        ScopedNodeRenames
        namesToOut(const MergeContext& context,
                   const MergeGraph& graph,
                   const std::map<std::string, std::string>& toOut)
        {
            RenameBuilder builder;
            for (const std::pair<const std::string, std::string>& entry : toOut) {
                const MergeGraph::Node* a = graph.find(entry.first);
                std::map<std::string, OutNode>::const_iterator b = context.out.find(entry.second);
                if (a && (b != context.out.end())) {
                    builder.add(groupPath(graph, *a), a->node->scriptName, b->second.node->scriptName);
                }
            }

            return builder.take();
        }

    } // namespace

    ScopedNodeRenames
    baseNamesToOut(const MergeContext& context)
    {
        return namesToOut(context, context.base, context.baseToOut);
    }

    ScopedNodeRenames
    theirsNamesToOut(const MergeContext& context)
    {
        return namesToOut(context, context.theirs, context.theirsToOut);
    }

    void
    settleNames(MergeContext* context)
    {
        std::vector<std::string> ordered;
        for (const std::pair<const std::string, OutNode>& entry : context->out) {
            ordered.push_back(entry.first);
        }
        std::sort(ordered.begin(), ordered.end(), [context](const std::string& a, const std::string& b) {
            return context->out[a].order < context->out[b].order;
        });
        context->names.clear();
        for (const std::string& id : ordered) {
            OutNode& node = context->out[id];
            const std::string was = node.node->scriptName;
            const std::string name = context->uniqueName(node.parent, was);
            // Only a move into another group makes siblings clash, and a sibling's bare reference
            // to the moved node broke with the move, so references keep naming the node that stayed.
            if (name != was) {
                node.node->scriptName = name;
                if (node.node->label == was) {
                    node.node->label = name;
                }
            }
            context->names[node.parent].insert(name);
        }
    }

    void
    mergeAppearance(MergeContext* context)
    {
        for (std::pair<const std::string, OutNode>& entry : context->out) {
            OutNode& node = entry.second;
            if (!node.fromMine || node.baseId.empty() || node.theirsId.empty()) {
                continue;
            }
            const NodeSerialization& base = *context->base.find(node.baseId)->node;
            const NodeSerialization& theirs = *context->theirs.find(node.theirsId)->node;
            NodeSerialization& mine = *node.node;

            std::set<std::string>& siblings = context->names[node.parent];
            if ((mine.scriptName == base.scriptName) && (theirs.scriptName != base.scriptName) && !siblings.count(theirs.scriptName)) {
                siblings.erase(mine.scriptName);
                siblings.insert(theirs.scriptName);
                context->mineRenames[groupPath(context->mine, *context->mine.find(entry.first))][mine.scriptName] = theirs.scriptName;
                mine.scriptName = theirs.scriptName;
            }
            takeChanged(base.label, &mine.label, theirs.label);

            NodeLayoutSerialization& layout = mine.layout;
            const NodeLayoutSerialization& was = base.layout;
            const NodeLayoutSerialization& now = theirs.layout;
            std::tuple<bool, double, double> position(layout.hasPosition, layout.x, layout.y);
            takeChanged(std::make_tuple(was.hasPosition, was.x, was.y), &position, std::make_tuple(now.hasPosition, now.x, now.y));
            std::tie(layout.hasPosition, layout.x, layout.y) = position;
            std::tuple<bool, double, double> size(layout.hasSize, layout.width, layout.height);
            takeChanged(std::make_tuple(was.hasSize, was.width, was.height), &size, std::make_tuple(now.hasSize, now.width, now.height));
            std::tie(layout.hasSize, layout.width, layout.height) = size;
            std::tuple<bool, float, float, float> color(layout.hasColor, layout.colorR, layout.colorG, layout.colorB);
            takeChanged(std::make_tuple(was.hasColor, was.colorR, was.colorG, was.colorB), &color, std::make_tuple(now.hasColor, now.colorR, now.colorG, now.colorB));
            std::tie(layout.hasColor, layout.colorR, layout.colorG, layout.colorB) = color;
            std::tuple<bool, float, float, float> overlay(layout.hasOverlayColor, layout.overlayR, layout.overlayG, layout.overlayB);
            takeChanged(std::make_tuple(was.hasOverlayColor, was.overlayR, was.overlayG, was.overlayB), &overlay, std::make_tuple(now.hasOverlayColor, now.overlayR, now.overlayG, now.overlayB));
            std::tie(layout.hasOverlayColor, layout.overlayR, layout.overlayG, layout.overlayB) = overlay;
        }
    }

    void
    mergeKnobs(MergeContext* context)
    {
        for (const std::pair<const std::string, std::string>& entry : context->plan.baseToMine.baseToOther) {
            if (context->plan.baseToMine.adopted.count(entry.second)) {
                MergeConflict conflict;
                conflict.kind = MergeConflictKind::eAdopted;
                conflict.baseNodeId = entry.first;
                conflict.nodeId = context->out.count(entry.second) ? entry.second : std::string();
                conflict.side = MergeSide::eMine;
                conflict.defaultedKnobs = defaultedKnobs(context->base.find(entry.first)->node->knobs, context->mine.find(entry.second)->node->knobs);
                context->conflicts.push_back(conflict);
            }
        }
        for (const std::pair<const std::string, std::string>& entry : context->plan.baseToTheirs.baseToOther) {
            if (context->plan.baseToTheirs.adopted.count(entry.second)) {
                MergeConflict conflict;
                conflict.kind = MergeConflictKind::eAdopted;
                conflict.baseNodeId = entry.first;
                std::map<std::string, std::string>::const_iterator found = context->baseToOut.find(entry.first);
                conflict.nodeId = (found == context->baseToOut.end()) ? std::string() : found->second;
                conflict.side = MergeSide::eTheirs;
                conflict.defaultedKnobs = defaultedKnobs(context->base.find(entry.first)->node->knobs, context->theirs.find(entry.second)->node->knobs);
                context->conflicts.push_back(conflict);
            }
        }

        const ScopedNodeRenames baseNames = baseNamesToOut(*context);
        const ScopedNodeRenames theirsNames = theirsNamesToOut(*context);
        for (std::pair<const std::string, OutNode>& entry : context->out) {
            OutNode& node = entry.second;
            if (!node.fromMine) {
                node.node->knobs = renamedKnobs(context->theirs, *context->theirs.find(node.theirsId), theirsNames);
                continue;
            }
            if (!context->mineRenames.empty()) {
                const MergeGraph::Node& mineNode = *context->mine.find(entry.first);
                renameNodeReferences(&node.node->knobs, context->mineRenames, groupPath(context->mine, mineNode), mineNode.node->scriptName);
            }
            if (node.theirsId.empty()) {
                continue;
            }
            const std::list<KnobSerialization> theirs = renamedKnobs(context->theirs, *context->theirs.find(node.theirsId), theirsNames);
            std::list<KnobSerialization> base;
            if (!node.baseId.empty()) {
                base = renamedKnobs(context->base, *context->base.find(node.baseId), baseNames);
            }
            std::vector<MergeConflict> conflicts;
            node.node->knobs = mergeKnobLists(context->handlers, node.node->pluginID, node.baseId.empty() ? nullptr : &base, node.node->knobs, theirs, &conflicts);
            const NodeSerialization* baseNode = node.baseId.empty() ? nullptr : context->base.find(node.baseId)->node.get();
            context->handlers.nodeHandlerFor(node.node->pluginID).merge(baseNode, *context->theirs.find(node.theirsId)->node, context->handlers, node.node.get(), &conflicts);
            for (MergeConflict& conflict : conflicts) {
                conflict.baseNodeId = node.baseId;
                conflict.nodeId = entry.first;
                context->conflicts.push_back(std::move(conflict));
            }
        }
    }

} // namespace MergeDetail

MergePlan
planMerge(const ProjectSerialization& base,
          const ProjectSerialization& mine,
          const ProjectSerialization& theirs,
          const std::string& templateKey)
{
    const MergeGraph baseGraph(base);
    const MergeGraph mineGraph(mine);
    const MergeGraph theirsGraph(theirs);
    MergePlan plan;
    plan.templateKey = templateKey;
    plan.baseToMine = matchNodes(baseGraph, mineGraph, templateKey);
    plan.baseToTheirs = matchNodes(baseGraph, theirsGraph, std::string());
    plan.scope = mergeScope(mineGraph, plan.baseToMine);

    return plan;
}

MergePlan
planProjectMerge(const ProjectSerialization& base,
                 const ProjectSerialization& mine,
                 const ProjectSerialization& theirs)
{
    const MergeGraph baseGraph(base);
    const MergeGraph mineGraph(mine);
    const MergeGraph theirsGraph(theirs);
    MergePlan plan;
    plan.baseToMine = matchNodes(baseGraph, mineGraph, std::string());
    plan.baseToTheirs = matchNodes(baseGraph, theirsGraph, std::string());
    for (const std::pair<const std::string, MergeGraph::Node>& entry : mineGraph.nodes()) {
        plan.scope.insert(entry.first);
    }

    return plan;
}

MergeResult
threeWayMerge(const ProjectSerialization& base,
              const ProjectSerialization& mine,
              const ProjectSerialization& theirs,
              const MergePlan& plan,
              const MergeOptions& options)
{
    MergeDetail::MergeContext context(base, mine, theirs, plan, options);
    MergeDetail::mergeExistence(&context);
    MergeDetail::mergeParents(&context);
    MergeDetail::settleNames(&context);
    MergeDetail::mergeAppearance(&context);
    MergeDetail::mergeKnobs(&context);
    MergeDetail::mergeConnections(&context);

    MergeResult result;
    result.merged = MergeDetail::assemble(&context);
    if (plan.templateKey.empty()) {
        std::vector<MergeConflict> projectConflicts;
        result.merged.projectKnobs = mergeKnobLists(context.handlers, std::string(), &base.projectKnobs, mine.projectKnobs, theirs.projectKnobs, &projectConflicts);
        result.merged.templates = mergeTemplateImports(base.templates, mine.templates, theirs.templates);
        result.conflicts = std::move(context.conflicts);
        result.conflicts.insert(result.conflicts.end(), projectConflicts.begin(), projectConflicts.end());

        return result;
    }
    result.conflicts = std::move(context.conflicts);
    for (MergeConflict& conflict : result.conflicts) {
        conflict.templateKey = plan.templateKey;
    }

    std::shared_ptr<ProjectSerialization> newBase = std::make_shared<ProjectSerialization>(theirs);
    newBase->templates.clear();
    bool found = false;
    for (TemplateImportSerialization& entry : result.merged.templates) {
        if (entry.key == plan.templateKey) {
            found = true;
        }
    }
    if (!found) {
        TemplateImportSerialization entry;
        entry.key = plan.templateKey;
        result.merged.templates.push_back(entry);
    }
    for (TemplateImportSerialization& entry : result.merged.templates) {
        if (entry.key == plan.templateKey) {
            if (!options.templatePath.empty()) {
                entry.path = options.templatePath;
            }
            if (!options.templateContentHash.empty()) {
                entry.contentHash = options.templateContentHash;
            }
            if (!options.templateImportedAt.empty()) {
                entry.importedAt = options.templateImportedAt;
            }
            entry.base = newBase;
        }
    }

    return result;
}

std::vector<MergeConflict>
conflictsToSave(const ProjectSerialization& base,
                const ProjectSerialization& mine,
                const ProjectSerialization& theirs,
                const MergePlan& plan,
                const MergeResult& result)
{
    typedef std::tuple<MergeConflictKind, std::string, std::string, std::string, int, std::string, std::string, std::optional<double>> Place;
    const auto placeOf = [](const MergeConflict& conflict) {
        return Place(conflict.kind, conflict.templateKey, conflict.nodeId, conflict.knob, conflict.dimension, conflict.item, conflict.input, conflict.time);
    };
    const auto placesOf = [&](const std::vector<MergeConflict>& conflicts) {
        std::set<Place> places;
        for (const MergeConflict& conflict : conflicts) {
            places.insert(placeOf(conflict));
        }

        return places;
    };
    std::set<std::string> present;
    std::map<std::string, int> order;
    walkTree(result.merged.nodes.nodes, &order);
    for (const std::pair<const std::string, int>& entry : order) {
        present.insert(entry.first);
    }

    std::set<Place> taken = placesOf(result.conflicts);
    std::vector<MergeConflict> saved;
    // A template merge's base and theirs are template snapshots, which hold none of the shot's
    // conflicts, so only mine's are carried.
    const bool wholeProject = plan.templateKey.empty();
    const std::set<Place> inBase = wholeProject ? placesOf(base.mergeConflicts) : std::set<Place>();
    const std::set<Place> inMine = placesOf(mine.mergeConflicts);
    const std::set<Place> inTheirs = wholeProject ? placesOf(theirs.mergeConflicts) : std::set<Place>();
    std::vector<const std::vector<MergeConflict>*> earlier(1, &mine.mergeConflicts);
    if (wholeProject) {
        earlier.push_back(&theirs.mergeConflicts);
    }
    for (const std::vector<MergeConflict>* side : earlier) {
        const std::set<Place>& other = (side == &mine.mergeConflicts) ? inTheirs : inMine;
        for (const MergeConflict& conflict : *side) {
            const Place place = placeOf(conflict);
            const bool removedByOther = wholeProject && inBase.count(place) > 0 && other.count(place) == 0;
            const bool open = conflict.nodeId.empty() ? conflict.baseNodeId.empty() : present.count(conflict.nodeId) > 0;
            if (open && !removedByOther && taken.insert(place).second) {
                saved.push_back(conflict);
            }
        }
    }
    saved.insert(saved.end(), result.conflicts.begin(), result.conflicts.end());

    return saved;
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
