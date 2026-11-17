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

void
refreshLimit()
{
    const std::uint64_t configured = g_configuredBytes.load();
    StormTextureBudget::budget().setLimit(configured ? configured : automaticGpuTextureBudget(g_videoMemoryBytes.load()));
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

std::uint64_t
queryVideoMemoryBytes(bool* freeOnly)
{
    *freeOnly = false;
    std::uint64_t bytes = 0;
    // Mesa's radeonsi answers the NVIDIA query too, with the card's VRAM.
    if (GARCH_GLAPI_HAS(NVX_gpu_memory_info)) {
        GLint kilobytes = 0;
        glGetIntegerv(GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX, &kilobytes);
        bytes = (std::uint64_t)std::max(0, kilobytes) * 1024;
    }
    if ((bytes == 0) && hasGLExtension("GL_ATI_meminfo")) {
        // The extension has only what is free, which shrinks as scenes load their textures.
        const GLenum textureFreeMemoryATI = 0x87FC;
        GLint free[4] = { 0, 0, 0, 0 };
        glGetIntegerv(textureFreeMemoryATI, free);
        bytes = (std::uint64_t)std::max(0, free[0]) * 1024;
        *freeOnly = (bytes != 0);
    }
    if (bytes == 0) {
        typedef void* (*GetProcAddress)(const GLubyte*);
        typedef int (*QueryRendererInteger)(int, unsigned int*);
        const GetProcAddress getProcAddress = reinterpret_cast<GetProcAddress>(dlsym(RTLD_DEFAULT, "glXGetProcAddressARB"));
        const QueryRendererInteger query = getProcAddress ? reinterpret_cast<QueryRendererInteger>(getProcAddress(reinterpret_cast<const GLubyte*>("glXQueryCurrentRendererIntegerMESA"))) : nullptr;
        const int videoMemoryMESA = 0x8187;
        unsigned int megabytes = 0;
        if (query && query(videoMemoryMESA, &megabytes)) {
            bytes = (std::uint64_t)megabytes << 20;
        }
    }
    while (glGetError() != GL_NO_ERROR) {
    }

    return bytes;
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
    g_reportPending = false;
    std::size_t refused = 0;
    GpuTextureBudget& budget = StormTextureBudget::budget();
    if (!budget.takeReport(&refused)) {
        return;
    }
    const qulonglong limitMB = (qulonglong)(budget.getLimit() >> 20);
    const QString message = (refused == 1)
        ? QCoreApplication::translate("StormTextureBudget", "1 texture is not loaded: it does not fit in the GPU texture budget of %1 MB. "
                                                            "Its material uses the texture's fallback value. The GPU Texture Budget preference sets the budget.")
              .arg(limitMB)
        : QCoreApplication::translate("StormTextureBudget", "%1 textures are not loaded: they do not fit in the GPU texture budget of %2 MB. "
                                                            "Their materials use the textures' fallback values. The GPU Texture Budget preference sets the budget.")
              .arg((qulonglong)refused)
              .arg(limitMB);
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
              std::uint64_t scope)
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
    const std::vector<bool> admitted = budget.claim(scope, network->GetMaterialPrimPath().GetString(), requests);
    bool refused = false;
    for (std::size_t i = 0; i < admitted.size(); ++i) {
        if (!admitted[i]) {
            replaceWithFallback(network, textures[i].node, textures[i].fallback);
            refused = true;
        }
    }
    if (refused) {
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

class TextureBudgetSceneIndex
    : public HdMaterialFilteringSceneIndexBase {
public:
    static TextureBudgetSceneIndexRefPtr New(const HdSceneIndexBaseRefPtr& input)
    {
        return TfCreateRefPtr(new TextureBudgetSceneIndex(input));
    }

    ~TextureBudgetSceneIndex() override
    {
        _GetInputSceneIndex()->RemoveObserver(HdSceneIndexObserverPtr(&_changes));
        StormTextureBudget::budget().releaseScope(_scope);
    }

protected:
    FilteringFnc _GetFilteringFunction() const override
    {
        const std::uint64_t scope = _scope;

        return [scope](HdMaterialNetworkInterface* network) {
            filterNetwork(network, scope);
        };
    }

private:
    explicit TextureBudgetSceneIndex(const HdSceneIndexBaseRefPtr& input)
        : HdMaterialFilteringSceneIndexBase(input)
        , _scope(g_nextScope++)
        , _changes(_scope)
    {
        // The base class's observer of the input cannot be overridden, so this one is added beside it.
        _GetInputSceneIndex()->AddObserver(HdSceneIndexObserverPtr(&_changes));
    }

    const std::uint64_t _scope;
    MaterialChanges _changes;
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
            [](const std::string& /*renderInstanceId*/, const HdSceneIndexBaseRefPtr& input, const HdContainerDataSourceHandle& /*args*/) -> HdSceneIndexBaseRefPtr {
                return TextureBudgetSceneIndex::New(input);
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
    bool freeOnly = false;
    const std::uint64_t bytes = queryVideoMemoryBytes(&freeOnly);
    if (freeOnly) {
        std::uint64_t seen = g_videoMemoryBytes.load();
        while ((bytes > seen) && !g_videoMemoryBytes.compare_exchange_weak(seen, bytes)) {
        }
    } else if (bytes) {
        g_videoMemoryBytes = bytes;
    }
    if (appPTR && appPTR->getCurrentSettings()) {
        g_configuredBytes = appPTR->getCurrentSettings()->getGpuTextureBudgetMB() << 20;
    }
    refreshLimit();
}

GpuTextureBudget&
budget()
{
    static GpuTextureBudget instance(automaticGpuTextureBudget(0));

    return instance;
}
} // namespace StormTextureBudget

NATRON_NAMESPACE_EXIT
