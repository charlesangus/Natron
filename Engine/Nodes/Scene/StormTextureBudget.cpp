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

#include "StormTextureBudget.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <dlfcn.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QString>
#include <QtCore/QTimer>

#ifdef emit
#define NATRON_STORMTEXTUREBUDGET_RESTORE_EMIT
#undef emit
#endif
#include <Ptexture.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/garch/glApi.h>
#include <pxr/imaging/hd/instancerTopologySchema.h>
#include <pxr/imaging/hd/materialBindingSchema.h>
#include <pxr/imaging/hd/materialBindingsSchema.h>
#include <pxr/imaging/hd/materialFilteringSceneIndexBase.h>
#include <pxr/imaging/hd/materialNetworkInterface.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/sceneIndexObserver.h>
#include <pxr/imaging/hd/sceneIndexPluginRegistry.h>
#include <pxr/imaging/hdSt/textureUtils.h>
#include <pxr/imaging/hdSt/udimTextureObject.h>
#include <pxr/imaging/hgi/types.h>
#include <pxr/imaging/hio/glslfx.h>
#include <pxr/imaging/hio/image.h>
#include <pxr/imaging/hio/types.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdr/registry.h>
#ifdef NATRON_STORMTEXTUREBUDGET_RESTORE_EMIT
#undef NATRON_STORMTEXTUREBUDGET_RESTORE_EMIT
#define emit
#endif

#include "Engine/AppManager.h"
#include "Engine/Nodes/Scene/GpuTextureBudget.h"
#include "Engine/Nodes/Scene/StormPtexBudget.h"
#include "Engine/Settings.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
// The plugInfo displayName of HdStormRendererPlugin, which is what the registry keys renderers by.
const char kStormDisplayName[] = "GL";

std::atomic<std::uint64_t> g_configuredBytes(0);
std::atomic<std::uint64_t> g_videoMemoryBytes(0);
std::atomic<std::uint64_t> g_nextScope(1);

// What one frame may add to the textures it streams in: Storm reads and uploads a texture in the
// render that first draws it, so a frame taking many would stall on the files.
const std::size_t kStreamBatchCount = 64;

void
refreshLimit()
{
    const std::uint64_t configured = g_configuredBytes.load();
    if (configured) {
        StormTextureBudget::budget().setLimit(configured);

        return;
    }
    const std::uint64_t videoMemory = g_videoMemoryBytes.load();
    StormTextureBudget::budget().setAutomatic(automaticGpuTextureBudget(videoMemory), automaticGpuTextureHeadroom(videoMemory));
}

bool
hasGLExtension(const char* name)
{
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; ++i) {
        const GLubyte* extension = glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (extension && (std::strcmp(reinterpret_cast<const char*>(extension), name) == 0)) {
            return true;
        }
    }

    return false;
}

bool
hasATIMeminfo()
{
    // Looked up once: walking the extension list on every frame is a few hundred GL calls.
    static std::atomic<int> known(-1);
    int has = known.load();
    if (has < 0) {
        has = hasGLExtension("GL_ATI_meminfo") ? 1 : 0;
        known = has;
    }

    return has != 0;
}

struct VideoMemory {
    std::uint64_t totalBytes = 0;
    std::uint64_t freeBytes = 0;
};

VideoMemory
queryVideoMemory(bool withTotal)
{
    VideoMemory memory;
    // Mesa's radeonsi answers the NVIDIA query too.
    if (GARCH_GLAPI_HAS(NVX_gpu_memory_info)) {
        GLint kilobytes = 0;
        if (withTotal) {
            glGetIntegerv(GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX, &kilobytes);
            memory.totalBytes = (std::uint64_t)std::max(0, kilobytes) * 1024;
        }
        kilobytes = 0;
        glGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &kilobytes);
        memory.freeBytes = (std::uint64_t)std::max(0, kilobytes) * 1024;
    }
    if ((memory.freeBytes == 0) && hasATIMeminfo()) {
        const GLenum textureFreeMemoryATI = 0x87FC;
        GLint free[4] = { 0, 0, 0, 0 };
        glGetIntegerv(textureFreeMemoryATI, free);
        memory.freeBytes = (std::uint64_t)std::max(0, free[0]) * 1024;
    }
    if (withTotal && (memory.totalBytes == 0)) {
        typedef void* (*GetProcAddress)(const GLubyte*);
        typedef int (*QueryRendererInteger)(int, unsigned int*);
        const GetProcAddress getProcAddress = reinterpret_cast<GetProcAddress>(dlsym(RTLD_DEFAULT, "glXGetProcAddressARB"));
        const QueryRendererInteger query = getProcAddress ? reinterpret_cast<QueryRendererInteger>(getProcAddress(reinterpret_cast<const GLubyte*>("glXQueryCurrentRendererIntegerMESA"))) : nullptr;
        const int videoMemoryMESA = 0x8187;
        unsigned int megabytes = 0;
        if (query && query(videoMemoryMESA, &megabytes)) {
            memory.totalBytes = (std::uint64_t)megabytes << 20;
        }
    }
    while (glGetError() != GL_NO_ERROR) {
    }

    return memory;
}

void
noteFreeMemory(const VideoMemory& memory)
{
    if (memory.freeBytes == 0) {
        return;
    }
    StormTextureBudget::budget().noteFreeMemory(memory.freeBytes);
    if (memory.totalBytes != 0) {
        return;
    }
    // With only the free memory to go by, the most ever seen free stands in for the card's size,
    // which sizes the headroom.
    std::uint64_t seen = g_videoMemoryBytes.load();
    bool grew = false;
    while ((memory.freeBytes > seen) && !(grew = g_videoMemoryBytes.compare_exchange_weak(seen, memory.freeBytes))) {
    }
    if (grew && !g_configuredBytes.load()) {
        refreshLimit();
    }
}

// The refused count keeps changing while a scene loads; it is reported once it has held still.
const std::chrono::milliseconds kReportQuiet(3000);
std::atomic<bool> g_reportPending(false);
std::atomic<std::int64_t> g_lastRefusal(0);

std::int64_t
nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void armReport(std::int64_t delayMs);

void
report()
{
    const std::int64_t quiet = (std::int64_t)kReportQuiet.count();
    const std::int64_t elapsed = nowMs() - g_lastRefusal.load();
    if (elapsed < quiet) {
        armReport(quiet - elapsed);

        return;
    }
    GpuTextureBudget& budget = StormTextureBudget::budget();
    // What a held texture comes to is still open, so the count would soon be out of date.
    if (budget.getPendingCount() > 0) {
        armReport(quiet);

        return;
    }
    g_reportPending = false;
    std::size_t refused = 0;
    if (!budget.takeReport(&refused)) {
        return;
    }
    const GpuTextureBudget::Basis basis = budget.getBasis();
    const qulonglong usedMB = (qulonglong)(basis.usedBytes >> 20);
    QString message = (refused == 1)
        ? QCoreApplication::translate("StormTextureBudget", "1 texture is not loaded, and its material uses the texture's fallback value.")
        : QCoreApplication::translate("StormTextureBudget", "%1 textures are not loaded, and their materials use the textures' fallback values.")
              .arg((qulonglong)refused);
    message += QLatin1Char(' ');
    if (basis.automatic && basis.freeMemoryKnown) {
        message += QCoreApplication::translate("StormTextureBudget", "Loading more would leave less than %1 MB of the graphics card's memory free: "
                                                                     "%2 MB of textures are loaded and about %3 MB is free. "
                                                                     "A GPU Texture Budget preference other than 0 sets a fixed budget instead.")
                       .arg((qulonglong)(basis.headroomBytes >> 20))
                       .arg(usedMB)
                       .arg((qulonglong)(basis.freeBytes >> 20));
    } else if (basis.automatic) {
        message += QCoreApplication::translate("StormTextureBudget", "The graphics card does not tell how much of its memory is free, so the textures are kept within "
                                                                     "half its memory, %1 MB, of which %2 MB are loaded. "
                                                                     "A GPU Texture Budget preference other than 0 sets a fixed budget instead.")
                       .arg((qulonglong)(basis.limitBytes >> 20))
                       .arg(usedMB);
    } else {
        message += QCoreApplication::translate("StormTextureBudget", "They do not fit in the GPU texture budget of %1 MB, of which %2 MB are loaded. "
                                                                     "The GPU Texture Budget preference sets the budget.")
                       .arg((qulonglong)(basis.limitBytes >> 20))
                       .arg(usedMB);
    }
    qWarning().noquote() << "Viewer:" << message;
    if (appPTR) {
        appPTR->writeToErrorLog_mt_safe(QString::fromUtf8("Viewer"), QDateTime::currentDateTime(), message);
    }
}

void
armReport(std::int64_t delayMs)
{
    QTimer::singleShot((int)delayMs, QCoreApplication::instance(), report);
}

void
scheduleReport()
{
    g_lastRefusal = nowMs();
    if (g_reportPending.exchange(true)) {
        return;
    }
    QCoreApplication* app = QCoreApplication::instance();
    if (!app) {
        g_reportPending = false;

        return;
    }
    QMetaObject::invokeMethod(app, []() { armReport((std::int64_t)kReportQuiet.count()); }, Qt::QueuedConnection);
}

struct TextureNodeKind {
    TfToken fileInput;
    VtValue fallback;
    bool ptex = false;
};

// What Storm's hdSt/materialNetwork.cpp reads a texture node by: the Sdr node it finds for the
// glslfx and mtlx source types, its first asset input, and isPtex.
const TextureNodeKind*
findTextureNodeKind(const TfToken& nodeType)
{
    static std::mutex mutex;
    static std::map<TfToken, std::unique_ptr<TextureNodeKind>> kinds;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const std::map<TfToken, std::unique_ptr<TextureNodeKind>>::const_iterator found = kinds.find(nodeType);
        if (found != kinds.end()) {
            return found->second.get();
        }
    }
    static const TfToken isPtex("isPtex");
    static const TfToken fallbackInput("fallback");
    std::unique_ptr<TextureNodeKind> kind;
    const SdrShaderNodeConstPtr node = SdrRegistry::GetInstance().GetShaderNodeByIdentifier(nodeType, { HioGlslfxTokens->glslfx, TfToken("mtlx") });
    if (node && !node->GetAssetIdentifierInputNames().empty()) {
        kind.reset(new TextureNodeKind);
        kind->fileInput = node->GetAssetIdentifierInputNames().front();
        kind->ptex = (node->GetMetadata().count(isPtex) != 0);
        if (const SdrShaderPropertyConstPtr fallback = node->GetShaderInput(fallbackInput)) {
            kind->fallback = fallback->GetDefaultValue();
        }
    }
    std::lock_guard<std::mutex> lock(mutex);
    std::unique_ptr<TextureNodeKind>& slot = kinds[nodeType];
    if (!slot && kind) {
        slot = std::move(kind);
    }

    return slot.get();
}

std::string
texturePath(const VtValue& file)
{
    if (file.IsHolding<SdfAssetPath>()) {
        const SdfAssetPath& asset = file.UncheckedGet<SdfAssetPath>();
        if (!gpuTextureIsFile(asset.GetAssetPath()) || !gpuTextureIsFile(asset.GetResolvedPath())) {
            return std::string();
        }

        return asset.GetResolvedPath().empty() ? asset.GetAssetPath() : asset.GetResolvedPath();
    }
    if (file.IsHolding<std::string>()) {
        const std::string& path = file.UncheckedGet<std::string>();

        return gpuTextureIsFile(path) ? path : std::string();
    }

    return std::string();
}

std::uint64_t
mipChainBytes(const std::vector<HioImageSharedPtr>& mips,
              std::size_t tileCount,
              std::size_t targetMemory)
{
    if (mips.empty()) {
        return 0;
    }
    const HgiFormat format = HdStTextureUtils::GetHgiFormat(mips.front()->GetFormat(), false);
    if (format == HgiFormatInvalid) {
        return 0;
    }
    const GfVec3i dimensions = HdStTextureUtils::ComputeDimensionsFromTargetMemory(mips, format, tileCount, targetMemory);
    const std::vector<HgiMipInfo> mipInfos = HgiGetMipInfos(format, dimensions, tileCount);
    if (mipInfos.empty()) {
        return 0;
    }

    return mipInfos.back().byteOffset + tileCount * mipInfos.back().byteSizePerLayer;
}

std::uint64_t
udimBytes(const std::string& pattern,
          std::size_t targetMemory)
{
    static const std::string token("<UDIM>");
    const std::size_t at = pattern.find(token);
    if (at == std::string::npos) {
        return 0;
    }
    const std::string prefix = pattern.substr(0, at);
    const std::string suffix = pattern.substr(at + token.size());
    ArResolver& resolver = ArGetResolver();
    std::string first;
    std::size_t tiles = 0;
    for (int tile = 1001; tile <= 1100; ++tile) {
        const std::string resolved = resolver.Resolve(prefix + std::to_string(tile) + suffix);
        if (resolved.empty()) {
            continue;
        }
        if (first.empty()) {
            first = resolved;
        }
        ++tiles;
    }
    if (tiles == 0) {
        return 0;
    }

    return mipChainBytes(HdStTextureUtils::GetAllMipImages(first, HioImage::Raw), tiles, targetMemory);
}

std::uint64_t
ptexBytes(const std::string& path,
          std::size_t targetMemory)
{
    Ptex::String error;
    PtexPtr<PtexTexture> texture(PtexTexture::open(path.c_str(), error, false));
    if (!texture) {
        return 0;
    }
    HioType type;
    switch (texture->dataType()) {
    case Ptex::dt_uint8:
        type = HioTypeUnsignedByte;
        break;
    case Ptex::dt_uint16:
        type = HioTypeUnsignedShort;
        break;
    case Ptex::dt_half:
        type = HioTypeHalfFloat;
        break;
    case Ptex::dt_float:
        type = HioTypeFloat;
        break;
    default:

        return 0;
    }
    const HgiFormat format = HdStTextureUtils::GetHgiFormat(HioGetFormat((uint32_t)texture->numChannels(), type, false), false);
    if (format == HgiFormatInvalid) {
        return 0;
    }
    std::vector<std::pair<int, int>> faces((std::size_t)std::max(0, texture->numFaces()));
    for (std::size_t i = 0; i < faces.size(); ++i) {
        const Ptex::Res& res = texture->getFaceInfo((int)i).res;
        faces[i] = std::make_pair((int)res.ulog2, (int)res.vlog2);
    }
    const std::size_t fileBytesPerTexel = (std::size_t)texture->numChannels() * (std::size_t)Ptex::DataSize(texture->dataType());

    return stormPtexUploadBytes(faces, fileBytesPerTexel, HgiGetDataSizeOfFormat(format), targetMemory);
}

std::uint64_t
measure(const std::string& path,
        bool ptex,
        std::size_t targetMemory)
{
    if (ptex) {
        return ptexBytes(path, targetMemory);
    }
    if (HdStIsSupportedUdimTexture(path)) {
        return udimBytes(path, targetMemory);
    }

    return mipChainBytes(HdStTextureUtils::GetAllMipImages(path, HioImage::Raw), 1, targetMemory);
}

VtValue
fallbackForOutput(const VtValue& fallback,
                  const TfToken& output)
{
    GfVec4f colour(0.f, 0.f, 0.f, 1.f);
    if (fallback.IsHolding<GfVec4f>()) {
        colour = fallback.UncheckedGet<GfVec4f>();
    } else if (fallback.IsHolding<GfVec3f>()) {
        const GfVec3f& rgb = fallback.UncheckedGet<GfVec3f>();
        colour = GfVec4f(rgb[0], rgb[1], rgb[2], 1.f);
    } else if (fallback.IsHolding<float>()) {
        const float value = fallback.UncheckedGet<float>();
        colour = GfVec4f(value, value, value, 1.f);
    }
    static const TfToken rgb("rgb");
    static const TfToken rgba("rgba");
    static const char* const channels = "rgba";
    if (output == rgb) {
        return VtValue(GfVec3f(colour[0], colour[1], colour[2]));
    }
    if (output == rgba) {
        return VtValue(colour);
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
replaceWithFallback(HdMaterialNetworkInterface* network,
                    const TfToken& texture,
                    const VtValue& fallback)
{
    // A colour is no normal: with the texture gone the surface keeps the geometry's.
    static const TfToken normalInput("normal");
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
                    value = fallbackForOutput(fallback, connection.upstreamOutputName);
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
}

void
filterNetwork(HdMaterialNetworkInterface* network,
              std::uint64_t scope,
              bool defer)
{
    static const TfToken textureMemoryInput("textureMemory");
    struct Texture {
        TfToken node;
        VtValue fallback;
    };
    std::vector<Texture> textures;
    std::vector<GpuTextureBudget::Request> requests;
    GpuTextureBudget& budget = StormTextureBudget::budget();
    for (const TfToken& node : network->GetNodeNames()) {
        const TextureNodeKind* kind = findTextureNodeKind(network->GetNodeType(node));
        if (!kind) {
            continue;
        }
        // An SdfPath names a render buffer, an HdStTextureIdentifier a texture its client uploads
        // and a URI (a natron:// live texture) what its resolver serves: none is Storm reading a
        // file, so each stays connected and uncounted.
        const std::string path = texturePath(network->GetNodeParameterValue(node, kind->fileInput));
        if (path.empty()) {
            continue;
        }
        const VtValue memory = network->GetNodeParameterValue(node, textureMemoryInput);
        std::size_t targetMemory = memory.IsHolding<float>() ? (std::size_t)(std::max(0.f, memory.UncheckedGet<float>()) * 1048576.f) : 0;
        if (kind->ptex && (targetMemory == 0)) {
            targetMemory = kStormPtexMemoryRequest;
        }
        GpuTextureBudget::Request request;
        request.key = std::string(kind->ptex ? "ptex:" : "uv:") + std::to_string(targetMemory) + ":" + path;
        if (!budget.findBytes(request.key, &request.bytes)) {
            request.bytes = measure(path, kind->ptex, targetMemory);
        }
        if (request.bytes == 0) {
            continue;
        }
        VtValue fallback = network->GetNodeParameterValue(node, TfToken("fallback"));
        textures.push_back(Texture { node, fallback.IsEmpty() ? kind->fallback : fallback });
        requests.push_back(std::move(request));
    }
    // Called with no textures too, so an edited material lets go of the ones it no longer reads.
    const std::size_t refusedBefore = budget.getRefusedCount();
    const std::vector<bool> admitted = budget.claim(scope, network->GetMaterialPrimPath().GetString(), requests, defer);
    for (std::size_t i = 0; i < admitted.size(); ++i) {
        if (!admitted[i]) {
            replaceWithFallback(network, textures[i].node, textures[i].fallback);
        }
    }
    if (budget.getRefusedCount() > refusedBefore) {
        scheduleReport();
    }
} // filterNetwork

// Material prims removed or edited let go of their textures, or have the next read replace them.
class MaterialChanges final
    : public HdSceneIndexObserver {
public:
    explicit MaterialChanges(std::uint64_t scope)
        : _scope(scope)
    {
    }

    void PrimsAdded(const HdSceneIndexBase& /*sender*/,
                    const AddedPrimEntries& /*entries*/) override
    {
    }

    void PrimsRemoved(const HdSceneIndexBase& /*sender*/,
                      const RemovedPrimEntries& entries) override
    {
        for (const RemovedPrimEntry& entry : entries) {
            StormTextureBudget::budget().release(_scope, entry.primPath.GetString());
        }
    }

    void PrimsDirtied(const HdSceneIndexBase& /*sender*/,
                      const DirtiedPrimEntries& entries) override
    {
        for (const DirtiedPrimEntry& entry : entries) {
            if (entry.dirtyLocators.Intersects(HdMaterialSchema::GetDefaultLocator())) {
                StormTextureBudget::budget().markStale(_scope, entry.primPath.GetString());
            }
        }
    }

    void PrimsRenamed(const HdSceneIndexBase& /*sender*/,
                      const RenamedPrimEntries& entries) override
    {
        for (const RenamedPrimEntry& entry : entries) {
            StormTextureBudget::budget().release(_scope, entry.oldPrimPath.GetString());
        }
    }

private:
    const std::uint64_t _scope;
};

class TextureBudgetSceneIndex;
typedef TfRefPtr<TextureBudgetSceneIndex> TextureBudgetSceneIndexRefPtr;

// Held while a scene index is used through it, so it cannot be destroyed meanwhile.
std::mutex g_indicesMutex;
std::map<std::string, TextureBudgetSceneIndex*> g_indices;

class TextureBudgetSceneIndex
    : public HdMaterialFilteringSceneIndexBase {
public:
    static TextureBudgetSceneIndexRefPtr New(const std::string& renderInstanceId,
                                             const HdSceneIndexBaseRefPtr& input)
    {
        return TfCreateRefPtr(new TextureBudgetSceneIndex(renderInstanceId, input));
    }

    ~TextureBudgetSceneIndex() override
    {
        {
            std::lock_guard<std::mutex> lock(g_indicesMutex);
            const std::map<std::string, TextureBudgetSceneIndex*>::iterator found = g_indices.find(_instanceId);
            if ((found != g_indices.end()) && (found->second == this)) {
                g_indices.erase(found);
            }
        }
        _GetInputSceneIndex()->RemoveObserver(HdSceneIndexObserverPtr(&_changes));
        StormTextureBudget::budget().releaseScope(_scope);
    }

    std::uint64_t getScope() const
    {
        return _scope;
    }

    // On the thread that renders with this index, between renders.
    StormTextureBudget::Streamed stream(const StormTextureBudget::StreamPace& pace)
    {
        *_defers = true;
        GpuTextureBudget& budget = StormTextureBudget::budget();
        GpuTextureBudget::Decided decided;
        if (pace.rows) {
            rankMaterials(pace.rows, pace.reached);
            const std::unordered_map<std::string, std::size_t>& ranks = _materialRanks;
            decided = budget.decidePending(_scope, pace.batchBytes, kStreamBatchCount, [&ranks](const std::string& owner) {
                const std::unordered_map<std::string, std::size_t>::const_iterator found = ranks.find(owner);

                return (found == ranks.end()) ? GpuTextureBudget::kUnranked : found->second;
            });
        } else {
            decided = budget.decidePending(_scope, pace.batchBytes, kStreamBatchCount);
        }
        StormTextureBudget::Streamed streamed;
        streamed.admitted = decided.admitted;
        streamed.bytes = decided.bytes;
        if (decided.refused > 0) {
            scheduleReport();
        }
        const std::vector<std::string> owners = budget.takeOwnersToReread(_scope);
        if (owners.empty()) {
            return streamed;
        }
        HdSceneIndexObserver::DirtiedPrimEntries entries;
        entries.reserve(owners.size());
        for (const std::string& owner : owners) {
            entries.push_back(HdSceneIndexObserver::DirtiedPrimEntry(SdfPath(owner), HdDataSourceLocatorSet(HdMaterialSchema::GetDefaultLocator())));
        }
        _SendPrimsDirtied(entries);

        return streamed;
    }

protected:
    FilteringFnc _GetFilteringFunction() const override
    {
        const std::uint64_t scope = _scope;
        // Shared rather than read through this, which the material data sources can outlive.
        const std::shared_ptr<std::atomic<bool>> defers = _defers;

        return [scope, defers](HdMaterialNetworkInterface* network) {
            filterNetwork(network, scope, defers->load());
        };
    }

private:
    // Gives each material bound under rows[0, reached) the index of the first row binding it, going
    // on from the rows already looked into while rows stay the same.
    void rankMaterials(const std::shared_ptr<const std::vector<std::string>>& rows,
                       std::size_t reached)
    {
        if (rows != _rankedRows) {
            _rankedRows = rows;
            _rowsRanked = 0;
            _materialRanks.clear();
        }
        const HdSceneIndexBaseRefPtr input = _GetInputSceneIndex();
        reached = (std::min)(reached, rows->size());
        std::vector<SdfPath> stack;
        for (; _rowsRanked < reached; ++_rowsRanked) {
            const std::size_t rank = _rowsRanked;
            std::unordered_map<SdfPath, bool, SdfPath::Hash> seen;
            stack.assign(1, SdfPath((*rows)[rank]));
            while (!stack.empty()) {
                const SdfPath path = stack.back();
                stack.pop_back();
                if (!seen.emplace(path, true).second) {
                    continue;
                }
                const HdSceneIndexPrim prim = input->GetPrim(path);
                if (prim.dataSource) {
                    if (const HdPathDataSourceHandle bound = HdMaterialBindingsSchema::GetFromParent(prim.dataSource).GetMaterialBinding().GetPath()) {
                        const SdfPath material = bound->GetTypedValue(0.f);
                        if (!material.IsEmpty()) {
                            _materialRanks.emplace(material.GetString(), rank);
                        }
                    }
                    if (const HdPathArrayDataSourceHandle prototypes = HdInstancerTopologySchema::GetFromParent(prim.dataSource).GetPrototypes()) {
                        for (const SdfPath& prototype : prototypes->GetTypedValue(0.f)) {
                            stack.push_back(prototype);
                        }
                    }
                }
                for (const SdfPath& child : input->GetChildPrimPaths(path)) {
                    stack.push_back(child);
                }
            }
        }
    }

    TextureBudgetSceneIndex(const std::string& renderInstanceId,
                            const HdSceneIndexBaseRefPtr& input)
        : HdMaterialFilteringSceneIndexBase(input)
        , _scope(g_nextScope++)
        , _instanceId(renderInstanceId)
        , _defers(std::make_shared<std::atomic<bool>>(false))
        , _changes(_scope)
    {
        // The base class's observer of the input cannot be overridden, so this one is added beside it.
        _GetInputSceneIndex()->AddObserver(HdSceneIndexObserverPtr(&_changes));
        std::lock_guard<std::mutex> lock(g_indicesMutex);
        g_indices[_instanceId] = this;
    }

    const std::uint64_t _scope;
    const std::string _instanceId;
    // Set once something renders with this index and so will read its materials again: only then
    // can a texture be held without being left out for good.
    const std::shared_ptr<std::atomic<bool>> _defers;
    MaterialChanges _changes;
    // The render thread's own.
    std::shared_ptr<const std::vector<std::string>> _rankedRows;
    std::size_t _rowsRanked = 0;
    std::unordered_map<std::string, std::size_t> _materialRanks;
};
} // anonymous namespace

namespace StormTextureBudget {
void
registerForStorm()
{
    static std::once_flag once;
    std::call_once(once, []() {
        HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
            kStormDisplayName,
            [](const std::string& renderInstanceId, const HdSceneIndexBaseRefPtr& input, const HdContainerDataSourceHandle& /*args*/) -> HdSceneIndexBaseRefPtr {
                return TextureBudgetSceneIndex::New(renderInstanceId, input);
            },
            HdContainerDataSourceHandle(),
            0,
            HdSceneIndexPluginRegistry::InsertionOrderAtStart);
    });
}

void
setConfiguredBudgetMB(std::uint64_t megabytes)
{
    g_configuredBytes = megabytes << 20;
    refreshLimit();
}

void
noteCurrentGLContext()
{
    const VideoMemory memory = queryVideoMemory(true);
    if (memory.totalBytes) {
        g_videoMemoryBytes = memory.totalBytes;
    }
    if (appPTR && appPTR->getCurrentSettings()) {
        g_configuredBytes = appPTR->getCurrentSettings()->getGpuTextureBudgetMB() << 20;
    }
    refreshLimit();
    noteFreeMemory(memory);
}

void
sampleFreeVideoMemory()
{
    const VideoMemory memory = queryVideoMemory(false);
    if (memory.freeBytes == 0) {
        budget().noteFreeMemoryUnchanged();

        return;
    }
    noteFreeMemory(memory);
}

void
streamTextures(const std::string& renderInstanceId)
{
    streamTextures(renderInstanceId, StreamPace());
}

Streamed
streamTextures(const std::string& renderInstanceId,
               const StreamPace& pace)
{
    if (renderInstanceId.empty()) {
        return Streamed();
    }
    std::lock_guard<std::mutex> lock(g_indicesMutex);
    const std::map<std::string, TextureBudgetSceneIndex*>::const_iterator found = g_indices.find(renderInstanceId);
    if (found != g_indices.end()) {
        return found->second->stream(pace);
    }

    return Streamed();
}

bool
texturesPending(const std::string& renderInstanceId)
{
    if (renderInstanceId.empty()) {
        return false;
    }
    std::uint64_t scope = 0;
    {
        std::lock_guard<std::mutex> lock(g_indicesMutex);
        const std::map<std::string, TextureBudgetSceneIndex*>::const_iterator found = g_indices.find(renderInstanceId);
        if (found == g_indices.end()) {
            return false;
        }
        scope = found->second->getScope();
    }

    return budget().hasPending(scope);
}

GpuTextureBudget&
budget()
{
    static GpuTextureBudget instance(automaticGpuTextureBudget(0));

    return instance;
}
} // namespace StormTextureBudget

NATRON_NAMESPACE_EXIT
