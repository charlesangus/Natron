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

#include "USDMerge.h"

#include <cstddef>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

USDMerge::USDMerge(NodePtr node)
    : NativeEffectBase(node)
{
}

USDMerge::~USDMerge()
{
}

const NodeSpec*
USDMerge::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

ScenePayload
USDMerge::mergeStacks(const std::vector<ScenePayload>& inputs)
{
    // A layer reached through several inputs is a common upstream of them: every layer stacked
    // over it in any input was authored downstream of it, over it. Keeping its weakest position
    // keeps it under all of those, so each input's own order survives the union.
    // A layer is context only if every input reaching it does so through a break: a branch
    // without one relies on it, so an export must write it.
    // A region open on an input closes there, so another branch's layers never land in its file.
    // Below the merge, a USDConfigureLayer gathering the preceding layers reaches into every
    // input up to that input's own boundary: an entry is sealed unless an input reaching it could
    // claim it.
    std::size_t nonEmpty = 0;
    const ScenePayload* only = NULL;
    for (const ScenePayload& input : inputs) {
        if (!input.isEmpty()) {
            ++nonEmpty;
            only = &input;
        }
    }
    // Gathering below the merge then reaches what it would above it.
    if (nonEmpty == 1) {
        return SceneOps::closeAllRegions(*only);
    }

    std::vector<SceneLayerEntry> merged;
    std::unordered_map<const PXR_NS::SdfLayer*, std::size_t> seen;
    ScenePayload first;
    for (const ScenePayload& rawInput : inputs) {
        if (rawInput.isEmpty()) {
            continue;
        }
        const ScenePayload input = SceneOps::closeAllRegions(rawInput);
        if (first.isEmpty()) {
            first = input;
        }
        const std::vector<SceneLayerEntry> layers = SceneOps::getLayers(input);
        std::vector<bool> claimable(layers.size(), false);
        for (std::size_t i : SceneOps::getPrecedingRun(input)) {
            claimable[i] = true;
        }
        for (std::size_t i = 0; i < layers.size(); ++i) {
            const SceneLayerEntry& entry = layers[i];
            if (!entry.layer) {
                continue;
            }
            const std::pair<std::unordered_map<const PXR_NS::SdfLayer*, std::size_t>::iterator, bool> inserted = seen.emplace(entry.layer.operator->(), merged.size());
            if (inserted.second) {
                merged.push_back(entry);
                merged.back().sealed = !claimable[i];
                merged.back().mergeTop = false;
                continue;
            }
            SceneLayerEntry& kept = merged[inserted.first->second];
            if (!entry.context) {
                kept.context = false;
            }
            if (claimable[i]) {
                kept.sealed = false;
            }
        }
    }
    if (first.isEmpty()) {
        return ScenePayload();
    }
    merged.back().mergeTop = true;

    // Build on the first input's stack when the union starts with it, to share its entries.
    ScenePayload result;
    std::size_t next = 0;
    const std::vector<SceneLayerEntry> firstLayers = SceneOps::getLayers(first);
    bool isPrefix = firstLayers.size() <= merged.size();
    for (std::size_t i = 0; isPrefix && i < firstLayers.size(); ++i) {
        isPrefix = (firstLayers[i].layer == merged[i].layer) && (firstLayers[i].context == merged[i].context)
            && (firstLayers[i].sealed == merged[i].sealed) && (firstLayers[i].mergeTop == merged[i].mergeTop);
    }
    if (isPrefix) {
        result = first;
        next = firstLayers.size();
    }
    for (; next < merged.size(); ++next) {
        result = SceneOps::appendEntry(result, merged[next]);
    }

    return result;
} // USDMerge::mergeStacks

StatusEnum
USDMerge::renderScene(const SceneRenderActionArgs& args,
                      ScenePayload* outputScene,
                      std::string* /*errorMessage*/)
{
    std::vector<ScenePayload> inputs;
    for (int i = 0; i < kUSDMergeInputCount; ++i) {
        inputs.push_back(args.getInputScene(i));
    }
    *outputScene = mergeStacks(inputs);

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
