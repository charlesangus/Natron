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
#include <unordered_set>
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
    std::vector<SceneLayerEntry> merged;
    std::unordered_set<const PXR_NS::SdfLayer*> seen;
    const ScenePayload* first = NULL;
    for (const ScenePayload& input : inputs) {
        if (input.isEmpty()) {
            continue;
        }
        if (!first) {
            first = &input;
        }
        for (const SceneLayerEntry& entry : SceneOps::getLayers(input)) {
            if (entry.layer && seen.insert(entry.layer.operator->()).second) {
                merged.push_back(entry);
            }
        }
    }
    if (!first) {
        return ScenePayload();
    }

    // Build on the first input's stack when the union starts with it, to share its entries.
    ScenePayload result;
    std::size_t next = 0;
    const std::vector<SceneLayerEntry> firstLayers = SceneOps::getLayers(*first);
    bool isPrefix = firstLayers.size() <= merged.size();
    for (std::size_t i = 0; isPrefix && i < firstLayers.size(); ++i) {
        isPrefix = (firstLayers[i].layer == merged[i].layer);
    }
    if (isPrefix) {
        result = *first;
        next = firstLayers.size();
    }
    for (; next < merged.size(); ++next) {
        result = SceneOps::appendLayer(result, merged[next].layer, merged[next].hash);
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
