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

#include "LiveTextureSceneIndex.h"

#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#ifdef emit
#define NATRON_LIVETEXTURESCENEINDEX_RESTORE_EMIT
#undef emit
#endif
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/materialFilteringSceneIndexBase.h>
#include <pxr/imaging/hd/sceneIndexPluginRegistry.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/path.h>
#ifdef NATRON_LIVETEXTURESCENEINDEX_RESTORE_EMIT
#undef NATRON_LIVETEXTURESCENEINDEX_RESTORE_EMIT
#define emit
#endif

#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
const TfToken&
textureType()
{
    static const TfToken token("UsdUVTexture");

    return token;
}

const TfToken&
fileParameter()
{
    static const TfToken token("file");

    return token;
}

/*
 * A prim's file stays held after it moves on to another texture, since a frame's render can still be
 * reading the file while the next frame's network is filtered. Two per texture node bound what a plate
 * sequence keeps in RAM outside the store's budget.
 */
class TextureHolds {
public:
    LiveTextureBufferConstPtr get(const SdfPath& prim,
                                  const TfToken& node,
                                  const std::string& uri)
    {
        const Key key(prim, node);
        {
            std::lock_guard<std::mutex> lock(_mutex);
            const std::map<Key, Generations>::const_iterator found = _holds.find(key);
            if (found != _holds.end()) {
                if (found->second.current.uri == uri) {
                    return found->second.current.buffer;
                }
                if (found->second.previous.uri == uri) {
                    return found->second.previous.buffer;
                }
            }
        }
        // Outside the lock: the first export of a large plate must not stall other prims' filtering.
        const LiveTextureBufferConstPtr buffer = LiveTextureStore::instance().exportToSharedMemory(uri);
        if (buffer) {
            LiveTextureBufferConstPtr dropped;
            std::lock_guard<std::mutex> lock(_mutex);
            Generations& generations = _holds[key];
            dropped = std::move(generations.previous.buffer);
            generations.previous = std::move(generations.current);
            generations.current.uri = uri;
            generations.current.buffer = buffer;
        }

        return buffer;
    }

private:
    typedef std::pair<SdfPath, TfToken> Key;

    struct Held {
        std::string uri;
        LiveTextureBufferConstPtr buffer;
    };

    struct Generations {
        Held current;
        Held previous;
    };

    std::mutex _mutex;
    std::map<Key, Generations> _holds;
};

VtValue
fallbackOutput(const GfVec4f& fallback,
               const TfToken& output)
{
    static const TfToken rgb("rgb");
    static const char* const channels = "rgba";

    if (output == rgb) {
        return VtValue(GfVec3f(fallback[0], fallback[1], fallback[2]));
    }
    if (output.size() == 1) {
        const char* channel = std::strchr(channels, output.GetString()[0]);
        if (channel) {
            return VtValue(fallback[int(channel - channels)]);
        }
    }

    return VtValue();
}

void
replaceWithFallback(HdMaterialNetworkInterface* network,
                    const TfToken& texture)
{
    // A surface's normal has no value meaning "unperturbed" a texture's fallback could hold: all
    // zeros, the default, would blacken it, so with the texture gone it keeps the geometry's.
    static const TfToken normalInput("normal");
    static const TfToken fallbackName("fallback");

    const VtValue authored = network->GetNodeParameterValue(texture, fallbackName);
    const GfVec4f fallback = authored.IsHolding<GfVec4f>() ? authored.UncheckedGet<GfVec4f>() : GfVec4f(0.f, 0.f, 0.f, 1.f);

    for (const TfToken& node : network->GetNodeNames()) {
        if (node == texture) {
            continue;
        }
        for (const TfToken& input : network->GetNodeInputConnectionNames(node)) {
            const HdMaterialNetworkInterface::InputConnectionVector connections = network->GetNodeInputConnection(node, input);
            HdMaterialNetworkInterface::InputConnectionVector kept;
            VtValue value;
            for (const HdMaterialNetworkInterface::InputConnection& connection : connections) {
                if (connection.upstreamNodeName == texture) {
                    value = fallbackOutput(fallback, connection.upstreamOutputName);
                } else {
                    kept.push_back(connection);
                }
            }
            if (kept.size() == connections.size()) {
                continue;
            }
            if (kept.empty()) {
                network->DeleteNodeInputConnection(node, input);
            } else {
                network->SetNodeInputConnection(node, input, kept);
            }
            const bool isUnsetNormal = (input == normalInput) && !authored.IsHolding<GfVec4f>();
            if (!value.IsEmpty() && !isUnsetNormal) {
                network->SetNodeParameterValue(node, input, value);
            }
        }
    }
    for (const TfToken& terminal : network->GetTerminalNames()) {
        if (network->GetTerminalConnection(terminal).second.upstreamNodeName == texture) {
            network->DeleteTerminal(terminal);
        }
    }
    network->DeleteNode(texture);
} // replaceWithFallback

void
filterNetwork(HdMaterialNetworkInterface* network,
              TextureHolds& holds)
{
    const std::string prefix = kLiveTextureScheme "://";

    for (const TfToken& node : network->GetNodeNames()) {
        if (network->GetNodeType(node) != textureType()) {
            continue;
        }
        const VtValue file = network->GetNodeParameterValue(node, fileParameter());
        if (!file.IsHolding<SdfAssetPath>()) {
            continue;
        }
        // Not the resolved path: that is empty whenever the store has let the texture go.
        const std::string& uri = file.UncheckedGet<SdfAssetPath>().GetAssetPath();
        if (uri.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }
        const LiveTextureBufferConstPtr buffer = holds.get(network->GetMaterialPrimPath(), node, uri);
        if (buffer) {
            network->SetNodeParameterValue(node, fileParameter(), VtValue(SdfAssetPath(uri, buffer->getFilePath())));
        } else {
            replaceWithFallback(network, node);
        }
    }
}

class LiveTextureMaterialSceneIndex;
typedef TfRefPtr<LiveTextureMaterialSceneIndex> LiveTextureMaterialSceneIndexRefPtr;

class LiveTextureMaterialSceneIndex
    : public HdMaterialFilteringSceneIndexBase {
public:
    static LiveTextureMaterialSceneIndexRefPtr New(const HdSceneIndexBaseRefPtr& input)
    {
        return TfCreateRefPtr(new LiveTextureMaterialSceneIndex(input));
    }

protected:
    FilteringFnc _GetFilteringFunction() const override
    {
        return _filter;
    }

private:
    explicit LiveTextureMaterialSceneIndex(const HdSceneIndexBaseRefPtr& input)
        : HdMaterialFilteringSceneIndexBase(input)
        , _filter(LiveTextureSceneIndex::makeMaterialFilter())
    {
    }

    FilteringFnc _filter;
};

void
registerFor(const std::string& rendererDisplayName)
{
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        rendererDisplayName,
        [](const std::string& /*renderInstanceId*/, const HdSceneIndexBaseRefPtr& input, const HdContainerDataSourceHandle& /*args*/) -> HdSceneIndexBaseRefPtr {
            return LiveTextureMaterialSceneIndex::New(input);
        },
        nullptr,
        0,
        HdSceneIndexPluginRegistry::InsertionOrderAtStart);
}
} // anonymous namespace

namespace LiveTextureSceneIndex {
void
registerSceneIndices()
{
    static std::once_flag once;

    std::call_once(once, []() {
        registerFor(kRenderDelegateCyclesDisplayName);
        registerFor("Moonray");
    });
}

std::function<void(HdMaterialNetworkInterface*)>
makeMaterialFilter()
{
    const std::shared_ptr<TextureHolds> holds = std::make_shared<TextureHolds>();

    return [holds](HdMaterialNetworkInterface* network) { filterNetwork(network, *holds); };
}
} // namespace LiveTextureSceneIndex

NATRON_NAMESPACE_EXIT
