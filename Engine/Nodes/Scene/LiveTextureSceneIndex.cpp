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

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifdef emit
#define NATRON_LIVETEXTURESCENEINDEX_RESTORE_EMIT
#undef emit
#endif
#include <pxr/base/gf/math.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/materialFilteringSceneIndexBase.h>
#include <pxr/imaging/hd/sceneIndexObserver.h>
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

namespace LiveTextureSceneIndex {
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
            const LiveTextureBufferConstPtr held = findLocked(key, uri);
            if (held) {
                return held;
            }
        }
        // Outside the lock: the first export of a large plate must not stall other prims' filtering.
        LiveTextureStore& store = LiveTextureStore::instance();
        const LiveTextureBufferConstPtr buffer = store.exportToSharedMemory(uri);
        if (!buffer && !store.getNoRoom(uri).empty()) {
            // The render stops for want of room: the image it is reading stays, not the missing colour.
            std::lock_guard<std::mutex> lock(_mutex);
            const std::map<Key, Generations>::const_iterator held = _holds.find(key);
            if (held != _holds.end()) {
                return held->second.current.buffer;
            }
        }
        if (!buffer) {
            return buffer;
        }
        LiveTextureBufferConstPtr dropped;
        std::lock_guard<std::mutex> lock(_mutex);
        // The prim's network is filtered once per render context, possibly at once on other threads:
        // another can have held the file while this one exported it, and holding it twice would let
        // the one before it go while a render can still read it.
        const LiveTextureBufferConstPtr held = findLocked(key, uri);
        if (held) {
            return held;
        }
        Generations& generations = _holds[key];
        dropped = std::move(generations.previous.buffer);
        generations.previous = std::move(generations.current);
        generations.current.uri = uri;
        generations.current.buffer = buffer;

        return buffer;
    }

    void forget(const SdfPathVector& paths)
    {
        std::vector<LiveTextureBufferConstPtr> released;
        std::lock_guard<std::mutex> lock(_mutex);

        released.swap(_retired);
        for (std::map<Key, Generations>::iterator it = _holds.begin(); it != _holds.end();) {
            const SdfPath& prim = it->first.first;
            if (std::none_of(paths.begin(), paths.end(), [&prim](const SdfPath& path) { return prim.HasPrefix(path); })) {
                ++it;
                continue;
            }
            for (Held* held : { &it->second.current, &it->second.previous }) {
                if (held->buffer) {
                    _retired.push_back(std::move(held->buffer));
                }
            }
            it = _holds.erase(it);
        }
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

    LiveTextureBufferConstPtr findLocked(const Key& key,
                                         const std::string& uri) const
    {
        const std::map<Key, Generations>::const_iterator found = _holds.find(key);

        if (found == _holds.end()) {
            return LiveTextureBufferConstPtr();
        }
        if (found->second.current.uri == uri) {
            return found->second.current.buffer;
        }
        if (found->second.previous.uri == uri) {
            return found->second.previous.buffer;
        }

        return LiveTextureBufferConstPtr();
    }

    std::mutex _mutex;
    std::map<Key, Generations> _holds;
    // The files of the materials the last forget() removed.
    std::vector<LiveTextureBufferConstPtr> _retired;
};
} // namespace LiveTextureSceneIndex

namespace {
using LiveTextureSceneIndex::TextureHolds;

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

VtValue
missingOutput(const GfVec4f& colour,
              const TfToken& output)
{
    static const TfToken rgb("rgb");
    static const char* const channels = "rgba";

    if (output == rgb) {
        return VtValue(GfVec3f(colour[0], colour[1], colour[2]));
    }
    if (output.size() == 1) {
        const char* channel = std::strchr(channels, output.GetString()[0]);
        if (channel) {
            return VtValue(colour[int(channel - channels)]);
        }
    }

    return VtValue();
}

void
replaceWithMissingColour(HdMaterialNetworkInterface* network,
                         const TfToken& texture)
{
    // A colour has no value meaning "unperturbed" for a normal, so with the texture gone it keeps
    // the geometry's. An authored fallback on the texture loses to the colour: it signals an error.
    static const TfToken normalInput("normal");

    const GfVec4f colour(kLiveTextureMissingColour[0], kLiveTextureMissingColour[1], kLiveTextureMissingColour[2], kLiveTextureMissingColour[3]);

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
                    value = missingOutput(colour, connection.upstreamOutputName);
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
            if (!value.IsEmpty() && (input != normalInput)) {
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
} // replaceWithMissingColour

void
filterNetwork(HdMaterialNetworkInterface* network,
              TextureHolds& holds,
              bool cycles)
{
    static const TfToken sourceColorSpace("sourceColorSpace");
    static const TfToken raw("raw");
    // hdCycles hands parameters it does not map to the image node's sockets of the same name.
    static const TfToken alphaType("alpha_type");
    static const TfToken channelPacked("channel_packed");

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
            if (cycles) {
                // Any other space converts from the one it names into Cycles' scene linear, which is
                // the working space only while that is the configuration's scene_linear role.
                network->SetNodeParameterValue(node, sourceColorSpace, VtValue(raw));
                // Cycles premultiplies a file of straight colour as it loads it and divides again only
                // where the alpha output is connected, so with the alpha unused the colour darkens.
                network->SetNodeParameterValue(node, alphaType, VtValue(channelPacked));
            }
        } else {
            replaceWithMissingColour(network, node);
        }
    }
}

/*
 * hdCycles maps no UsdTransform2d, so it would drop the node and with it the connection to the texture's
 * st, misplacing every image whose window is not the format. Cycles' mapping node computes the same
 * scale, rotation and translation, in radians. Socket names match case-insensitively, so scale and
 * rotation keep their names and only change type.
 */
void
transformsToMappingNodes(HdMaterialNetworkInterface* network)
{
    static const TfToken transformType("UsdTransform2d");
    static const TfToken mappingType("cycles_mapping");
    static const TfToken inName("in");
    static const TfToken resultName("result");
    static const TfToken scaleName("scale");
    static const TfToken rotationName("rotation");
    static const TfToken translationName("translation");
    static const TfToken vectorSocket("Vector");
    static const TfToken locationSocket("Location");

    const TfTokenVector nodes = network->GetNodeNames();

    for (const TfToken& node : nodes) {
        if (network->GetNodeType(node) != transformType) {
            continue;
        }
        network->SetNodeType(node, mappingType);

        const VtValue scale = network->GetNodeParameterValue(node, scaleName);
        if (scale.IsHolding<GfVec2f>()) {
            const GfVec2f& value = scale.UncheckedGet<GfVec2f>();
            network->SetNodeParameterValue(node, scaleName, VtValue(GfVec3f(value[0], value[1], 1.f)));
        }
        const VtValue rotation = network->GetNodeParameterValue(node, rotationName);
        if (rotation.IsHolding<float>()) {
            network->SetNodeParameterValue(node, rotationName, VtValue(GfVec3f(0.f, 0.f, float(GfDegreesToRadians(rotation.UncheckedGet<float>())))));
        }
        const VtValue translation = network->GetNodeParameterValue(node, translationName);
        if (translation.IsHolding<GfVec2f>()) {
            const GfVec2f& value = translation.UncheckedGet<GfVec2f>();
            network->SetNodeParameterValue(node, locationSocket, VtValue(GfVec3f(value[0], value[1], 0.f)));
        }
        network->DeleteNodeParameter(node, translationName);

        const HdMaterialNetworkInterface::InputConnectionVector coordinates = network->GetNodeInputConnection(node, inName);
        if (!coordinates.empty()) {
            network->SetNodeInputConnection(node, vectorSocket, coordinates);
            network->DeleteNodeInputConnection(node, inName);
        }

        for (const TfToken& downstream : nodes) {
            for (const TfToken& input : network->GetNodeInputConnectionNames(downstream)) {
                HdMaterialNetworkInterface::InputConnectionVector connections = network->GetNodeInputConnection(downstream, input);
                bool renamed = false;
                for (HdMaterialNetworkInterface::InputConnection& connection : connections) {
                    if ((connection.upstreamNodeName == node) && (connection.upstreamOutputName == resultName)) {
                        connection.upstreamOutputName = vectorSocket;
                        renamed = true;
                    }
                }
                if (renamed) {
                    network->SetNodeInputConnection(downstream, input, connections);
                }
            }
        }
    }
} // transformsToMappingNodes

// Tells the filter which materials the scene no longer has, by the paths they were last at.
class Removals final
    : public HdSceneIndexObserver {
public:
    explicit Removals(const LiveTextureSceneIndex::MaterialFilter& filter)
        : _filter(filter)
    {
    }

    void PrimsAdded(const HdSceneIndexBase& /*sender*/,
                    const AddedPrimEntries& /*entries*/) override
    {
    }

    void PrimsRemoved(const HdSceneIndexBase& /*sender*/,
                      const RemovedPrimEntries& entries) override
    {
        SdfPathVector paths;

        paths.reserve(entries.size());
        for (const RemovedPrimEntry& entry : entries) {
            paths.push_back(entry.primPath);
        }
        _filter.forget(paths);
    }

    void PrimsDirtied(const HdSceneIndexBase& /*sender*/,
                      const DirtiedPrimEntries& /*entries*/) override
    {
    }

    void PrimsRenamed(const HdSceneIndexBase& /*sender*/,
                      const RenamedPrimEntries& entries) override
    {
        SdfPathVector paths;

        paths.reserve(entries.size());
        for (const RenamedPrimEntry& entry : entries) {
            paths.push_back(entry.oldPrimPath);
        }
        _filter.forget(paths);
    }

private:
    const LiveTextureSceneIndex::MaterialFilter _filter;
};

class LiveTextureMaterialSceneIndex;
typedef TfRefPtr<LiveTextureMaterialSceneIndex> LiveTextureMaterialSceneIndexRefPtr;

class LiveTextureMaterialSceneIndex
    : public HdMaterialFilteringSceneIndexBase {
public:
    static LiveTextureMaterialSceneIndexRefPtr New(const HdSceneIndexBaseRefPtr& input,
                                                   const LiveTextureSceneIndex::MaterialFilter& filter)
    {
        return TfCreateRefPtr(new LiveTextureMaterialSceneIndex(input, filter));
    }

    ~LiveTextureMaterialSceneIndex() override
    {
        _GetInputSceneIndex()->RemoveObserver(HdSceneIndexObserverPtr(&_removals));
    }

protected:
    FilteringFnc _GetFilteringFunction() const override
    {
        return _filter;
    }

private:
    LiveTextureMaterialSceneIndex(const HdSceneIndexBaseRefPtr& input,
                                  const LiveTextureSceneIndex::MaterialFilter& filter)
        : HdMaterialFilteringSceneIndexBase(input)
        , _filter(filter)
        , _removals(filter)
    {
        // The base class's observer of the input cannot be overridden, so this one is added beside it.
        _GetInputSceneIndex()->AddObserver(HdSceneIndexObserverPtr(&_removals));
    }

    FilteringFnc _filter;
    Removals _removals;
};

void
registerFor(const std::string& rendererDisplayName,
            LiveTextureSceneIndex::MaterialFilter (*makeFilter)())
{
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        rendererDisplayName,
        [makeFilter](const std::string& /*renderInstanceId*/, const HdSceneIndexBaseRefPtr& input, const HdContainerDataSourceHandle& /*args*/) -> HdSceneIndexBaseRefPtr {
            return LiveTextureMaterialSceneIndex::New(input, makeFilter());
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
        registerFor(kRenderDelegateCyclesDisplayName, makeCyclesMaterialFilter);
        registerFor("Moonray", makeMaterialFilter);
    });
}

MaterialFilter::MaterialFilter(bool cycles)
    : _holds(std::make_shared<TextureHolds>())
    , _cycles(cycles)
{
}

void
MaterialFilter::operator()(HdMaterialNetworkInterface* network) const
{
    filterNetwork(network, *_holds, _cycles);
    if (_cycles) {
        transformsToMappingNodes(network);
    }
}

void
MaterialFilter::forget(const SdfPathVector& paths) const
{
    _holds->forget(paths);
}

MaterialFilter
makeMaterialFilter()
{
    return MaterialFilter(false);
}

MaterialFilter
makeCyclesMaterialFilter()
{
    return MaterialFilter(true);
}
} // namespace LiveTextureSceneIndex

NATRON_NAMESPACE_EXIT
