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

#include "GeoSdfFileFormat.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef emit
#define NATRON_GEOSDFFILEFORMAT_RESTORE_EMIT
#undef emit
#endif
#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/type.h>
#include <pxr/usd/ar/defineResolver.h>
#include <pxr/usd/ar/timestamp.h>
#include <pxr/usd/ar/writableAsset.h>
#ifdef NATRON_GEOSDFFILEFORMAT_RESTORE_EMIT
#undef NATRON_GEOSDFFILEFORMAT_RESTORE_EMIT
#define emit
#endif

#include "Engine/Nodes/Geometry/GeoSdfData.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {

// The scheme, format id and extension of every geometry layer, and the plug-in declaring them.
const char* const kScheme = "natrongeo";
const char* const kPluginName = "natronGeo";
const char* const kRootArgument = "root";
const char* const kPackedArgument = "packed";

bool
keyFromPath(const std::string& path,
            std::string* key)
{
    const std::string prefix = std::string(kScheme) + ":";
    const std::string suffix = std::string(".") + kScheme;
    if (path.size() <= prefix.size() + suffix.size() || path.compare(0, prefix.size(), prefix) != 0 || path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }
    *key = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());

    return true;
}

bool
optionsFromArguments(const SdfFileFormat::FileFormatArguments& args,
                     GeoUsdConvertOptions* options,
                     std::string* errorMessage)
{
    const auto root = args.find(kRootArgument);
    if (root != args.end()) {
        const SdfPath path(root->second);
        if (!path.IsAbsolutePath() || !(path.IsPrimPath() || path.IsAbsoluteRootPath())) {
            *errorMessage = "The geometry layer root \"" + root->second + "\" is not an absolute prim path";

            return false;
        }
        options->rootPath = path;
    }
    const auto packed = args.find(kPackedArgument);
    if (packed != args.end()) {
        if (packed->second == "instancer") {
            options->packedMode = eGeoUsdPackedPointInstancer;
        } else if (packed->second == "native") {
            options->packedMode = eGeoUsdPackedNativeInstances;
        } else if (packed->second == "unpack") {
            options->packedMode = eGeoUsdPackedUnpack;
        } else {
            *errorMessage = "Unknown packed geometry mode \"" + packed->second + "\"";

            return false;
        }
    }

    return true;
}

struct Provider {
    std::function<GeoDetailConstPtr()> provide;
    U64 generation = 0;
};

struct Registry {
    std::mutex mutex;
    std::map<std::string, Provider> providers;
    U64 generation = 0;
};

Registry&
registry()
{
    static Registry r;

    return r;
}

U64
providerGeneration(const std::string& key)
{
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    const auto it = r.providers.find(key);

    return it == r.providers.end() ? 0 : it->second.generation;
}

class SpecCollector
    : public SdfAbstractDataSpecVisitor {
public:
    std::vector<SdfPath> paths;

    virtual bool VisitSpec(const SdfAbstractData& /*data*/,
                           const SdfPath& path) OVERRIDE
    {
        paths.push_back(path);

        return true;
    }

    virtual void Done(const SdfAbstractData& /*data*/) OVERRIDE
    {
    }
};

} // namespace

/**
 * @brief Ar's resolver for natrongeo: identifiers, which resolve to themselves while a provider
 * serves their key. The content never comes from an asset: GeoSdfFileFormat reads the provider.
 **/
class GeoLayerResolver
    : public ArResolver {
public:
    GeoLayerResolver()
    {
    }

    virtual ~GeoLayerResolver() OVERRIDE
    {
    }

protected:
    virtual std::string _CreateIdentifier(const std::string& assetPath,
                                          const ArResolvedPath& /*anchorAssetPath*/) const OVERRIDE
    {
        return assetPath;
    }

    virtual std::string _CreateIdentifierForNewAsset(const std::string& assetPath,
                                                     const ArResolvedPath& /*anchorAssetPath*/) const OVERRIDE
    {
        return assetPath;
    }

    virtual ArResolvedPath _Resolve(const std::string& assetPath) const OVERRIDE
    {
        std::string key;

        return keyFromPath(assetPath, &key) && GeoLayers::hasProvider(key) ? ArResolvedPath(assetPath) : ArResolvedPath();
    }

    virtual ArResolvedPath _ResolveForNewAsset(const std::string& /*assetPath*/) const OVERRIDE
    {
        return ArResolvedPath();
    }

    virtual std::string _GetExtension(const std::string& /*assetPath*/) const OVERRIDE
    {
        return kScheme;
    }

    virtual ArTimestamp _GetModificationTimestamp(const std::string& assetPath,
                                                  const ArResolvedPath& /*resolvedPath*/) const OVERRIDE
    {
        std::string key;
        if (!keyFromPath(assetPath, &key)) {
            return ArTimestamp();
        }

        return ArTimestamp(double(providerGeneration(key) + 1));
    }

    virtual std::shared_ptr<ArAsset> _OpenAsset(const ArResolvedPath& /*resolvedPath*/) const OVERRIDE
    {
        return std::shared_ptr<ArAsset>();
    }

    virtual bool _CanWriteAssetToPath(const ArResolvedPath& /*resolvedPath*/,
                                      std::string* whyNot) const OVERRIDE
    {
        if (whyNot) {
            *whyNot = "Geometry layers are generated by Natron and cannot be written";
        }

        return false;
    }

    virtual std::shared_ptr<ArWritableAsset> _OpenAssetForWrite(const ArResolvedPath& /*resolvedPath*/,
                                                                WriteMode /*writeMode*/) const OVERRIDE
    {
        return std::shared_ptr<ArWritableAsset>();
    }
};

/**
 * @brief The natrongeo file format: a layer's data is a GeoSdfData over its provider's detail.
 * Read-only: every write and export fails.
 **/
class GeoSdfFileFormat
    : public SdfFileFormat {
public:
    virtual ~GeoSdfFileFormat() OVERRIDE
    {
    }

    /** @brief The data of an open geometry layer, null for another layer. */
    static const GeoSdfData* dataOf(const SdfLayer& layer)
    {
        const SdfAbstractDataConstPtr data = _GetLayerData(layer);

        return dynamic_cast<const GeoSdfData*>(get_pointer(data));
    }

    virtual bool CanRead(const std::string& /*file*/) const OVERRIDE
    {
        return true;
    }

    virtual bool Read(SdfLayer* layer,
                      const std::string& resolvedPath,
                      bool /*metadataOnly*/) const OVERRIDE
    {
        std::string key;
        if (!keyFromPath(resolvedPath, &key)) {
            TF_RUNTIME_ERROR("\"%s\" is not a geometry layer", resolvedPath.c_str());

            return false;
        }
        GeoUsdConvertOptions options;
        std::string error;
        if (!optionsFromArguments(layer->GetFileFormatArguments(), &options, &error)) {
            TF_RUNTIME_ERROR("%s: %s", resolvedPath.c_str(), error.c_str());

            return false;
        }
        const GeoDetailConstPtr detail = GeoLayers::currentDetail(key);
        if (!detail) {
            TF_RUNTIME_ERROR("No geometry is served as \"%s\"", resolvedPath.c_str());

            return false;
        }
        // On a reload the layer still holds its previous data, whose built values carry over.
        const GeoSdfData* previous = dataOf(*layer);
        std::shared_ptr<const GeoUsdConvert> convert;
        if (previous && previous->source() == detail && !previous->hasEdits()) {
            convert = previous->convert();
        } else {
            std::shared_ptr<GeoUsdConvert> fresh = std::make_shared<GeoUsdConvert>(detail, options);
            if (previous) {
                fresh->adoptUnchangedPartitions(*previous->convert());
            }
            convert = std::move(fresh);
        }
        SdfAbstractDataRefPtr data = TfCreateRefPtr(new GeoSdfData(std::move(convert), detail));
        _SetLayerData(layer, data);

        return true;
    }

    virtual bool WriteToFile(const SdfLayer& /*layer*/,
                             const std::string& filePath,
                             const std::string& /*comment*/,
                             const FileFormatArguments& /*args*/) const OVERRIDE
    {
        TF_RUNTIME_ERROR("Geometry layers cannot be written to \"%s\"", filePath.c_str());

        return false;
    }

    virtual bool SaveToFile(const SdfLayer& /*layer*/,
                            const std::string& filePath,
                            const std::string& /*comment*/,
                            const FileFormatArguments& /*args*/) const OVERRIDE
    {
        TF_RUNTIME_ERROR("Geometry layers cannot be saved to \"%s\"", filePath.c_str());

        return false;
    }

    virtual bool WriteToString(const SdfLayer& /*layer*/,
                               std::string* /*str*/,
                               const std::string& /*comment*/) const OVERRIDE
    {
        return false;
    }

    virtual bool WriteToStream(const SdfSpecHandle& /*spec*/,
                               std::ostream& /*out*/,
                               size_t /*indent*/) const OVERRIDE
    {
        return false;
    }

protected:
    SDF_FILE_FORMAT_FACTORY_ACCESS;

    GeoSdfFileFormat()
        : SdfFileFormat(TfToken(kScheme), TfToken("1.0"), TfToken("usd"), std::string(kScheme))
    {
    }
};

std::shared_ptr<const GeoUsdConvert>
geoLayerConvert(const SdfLayerHandle& layer)
{
    const GeoSdfData* data = layer ? GeoSdfFileFormat::dataOf(*get_pointer(layer)) : nullptr;

    return data ? data->convert() : std::shared_ptr<const GeoUsdConvert>();
}

namespace {

TfTokenVector
childrenField(const VtValue& v)
{
    return v.IsHolding<TfTokenVector>() ? v.UncheckedGet<TfTokenVector>() : TfTokenVector();
}

bool
hasErasedAncestor(const SdfPath& path,
                  const std::unordered_set<SdfPath, SdfPath::Hash>& erased)
{
    for (SdfPath p = path.GetParentPath(); !p.IsEmpty() && p != SdfPath::AbsoluteRootPath(); p = p.GetParentPath()) {
        if (erased.count(p)) {
            return true;
        }
    }

    return false;
}

void
eraseSpec(const SdfLayerHandle& layer,
          const SdfPath& path)
{
    if (path.IsPrimPath()) {
        const SdfPrimSpecHandle prim = layer->GetPrimAtPath(path);
        if (!prim) {
            return;
        }
        SdfPrimSpecHandle parent = prim->GetRealNameParent();
        if (!parent) {
            parent = layer->GetPseudoRoot();
        }
        parent->RemoveNameChild(prim);
    } else if (path.IsPropertyPath()) {
        const SdfPropertySpecHandle property = layer->GetPropertyAtPath(path);
        const SdfPrimSpecHandle owner = layer->GetPrimAtPath(path.GetPrimPath());
        if (property && owner) {
            owner->RemoveProperty(property);
        }
    }
}

// Whether the partition's prim needs its properties and child prims authored again: a topology
// change invalidates every per-element value, so it is a structural change, not a value edit.
bool
topologyChanged(const SdfLayerHandle& layer,
                const GeoUsdConvert& after,
                const SdfPath& primPath)
{
    for (const TfToken& name : { UsdGeomTokens->faceVertexCounts, UsdGeomTokens->faceVertexIndices, UsdGeomTokens->curveVertexCounts }) {
        const SdfPath path = primPath.AppendProperty(name);
        VtValue next;
        after.field(path, SdfFieldKeys->Default, &next);
        if (layer->GetField(path, SdfFieldKeys->Default) != next) {
            return true;
        }
    }
    const SdfPath points = primPath.AppendProperty(UsdGeomTokens->points);
    VtValue next;
    after.field(points, SdfFieldKeys->Default, &next);
    const VtValue current = layer->GetField(points, SdfFieldKeys->Default);

    return current.IsEmpty() != next.IsEmpty() || current.GetArraySize() != next.GetArraySize();
}

} // namespace

namespace GeoLayers {

bool
registerPlugin(std::string* errorMessage)
{
    static std::once_flag once;
    static std::string error;

    std::call_once(once, []() {
        // Defined before the plugInfo.json declares them, so Ar and Sdf find their factories
        // without loading a library: the types live in this executable.
        Ar_DefineResolver<GeoLayerResolver, ArResolver>();
        TfType::Define<GeoSdfFileFormat, TfType::Bases<SdfFileFormat>>()
            .SetFactory<Sdf_FileFormatFactory<GeoSdfFileFormat>>();

        std::error_code ec;
        const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
        const std::filesystem::path dir = ec ? std::filesystem::path() : (exe.parent_path() / ".." / "Resources" / "usd" / kPluginName).lexically_normal();
        if (dir.empty() || !std::filesystem::is_regular_file(dir / "plugInfo.json", ec)) {
            error = "Geometry layers are unavailable: Resources/usd/natronGeo/plugInfo.json is missing from this installation";

            return;
        }
        PlugRegistry::GetInstance().RegisterPlugins(dir.string());
        if (!PlugRegistry::GetInstance().GetPluginWithName(kPluginName)) {
            error = "Geometry layers are unavailable: OpenUSD did not register " + (dir / "plugInfo.json").string();
        }
    });
    if (!error.empty() && errorMessage) {
        *errorMessage = error;
    }

    return error.empty();
}

std::string
identifier(const std::string& key,
           const std::string& rootPath,
           const std::string& packedMode)
{
    const std::string path = std::string(kScheme) + ":" + key + "." + kScheme;
    SdfLayer::FileFormatArguments args;
    if (!rootPath.empty()) {
        args[kRootArgument] = rootPath;
    }
    if (!packedMode.empty()) {
        args[kPackedArgument] = packedMode;
    }

    return args.empty() ? path : SdfLayer::CreateIdentifier(path, args);
}

void
setProvider(const std::string& key,
            std::function<GeoDetailConstPtr()> provider)
{
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    Provider& p = r.providers[key];
    p.provide = std::move(provider);
    p.generation = ++r.generation;
}

void
removeProvider(const std::string& key)
{
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    r.providers.erase(key);
}

bool
hasProvider(const std::string& key)
{
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);

    return r.providers.count(key) > 0;
}

GeoDetailConstPtr
currentDetail(const std::string& key)
{
    std::function<GeoDetailConstPtr()> provide;
    {
        Registry& r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        const auto it = r.providers.find(key);
        if (it == r.providers.end()) {
            return GeoDetailConstPtr();
        }
        provide = it->second.provide;
    }

    return provide ? provide() : GeoDetailConstPtr();
}

bool
refresh(const std::string& identifier,
        std::string* errorMessage)
{
    const auto fail = [errorMessage](const std::string& message) {
        if (errorMessage) {
            *errorMessage = message;
        }

        return false;
    };
    std::string layerPath;
    SdfLayer::FileFormatArguments args;
    std::string key;
    if (!SdfLayer::SplitIdentifier(identifier, &layerPath, &args) || !keyFromPath(layerPath, &key)) {
        return fail("\"" + identifier + "\" is not a geometry layer");
    }
    const SdfLayerHandle layer = SdfLayer::Find(identifier);
    if (!layer) {
        return fail("The geometry layer \"" + identifier + "\" is not open");
    }
    // The layer owns its data and only this module edits it outside SdfLayer.
    GeoSdfData* data = const_cast<GeoSdfData*>(GeoSdfFileFormat::dataOf(*get_pointer(layer)));
    if (!data) {
        return fail("The layer \"" + identifier + "\" no longer holds geometry");
    }
    const GeoDetailConstPtr detail = currentDetail(key);
    if (!detail) {
        return fail("No geometry is served as \"" + layerPath + "\"");
    }
    if (detail == data->source() || (!data->hasEdits() && data->source() && detail->contentHash() == data->source()->contentHash())) {
        return true;
    }

    const std::shared_ptr<const GeoUsdConvert> before = data->convert();
    const std::shared_ptr<GeoUsdConvert> after = std::make_shared<GeoUsdConvert>(detail, before->options());
    after->adoptUnchangedPartitions(*before);

    // A partition with the same path, kind and signature has identical specs and values, so its
    // specs are skipped; that only holds while the layer is exactly the previous conversion.
    std::vector<char> kept(after->partitionCount(), 0);
    std::vector<SdfPath> rebuilt;
    {
        std::map<SdfPath, std::size_t> beforeByPath;
        for (std::size_t i = 0; i < before->partitionCount(); ++i) {
            beforeByPath.emplace(before->partitionPath(i), i);
        }
        for (std::size_t i = 0; i < after->partitionCount(); ++i) {
            const auto it = beforeByPath.find(after->partitionPath(i));
            if (it == beforeByPath.end()) {
                continue;
            }
            if (!data->hasEdits() && before->partitionKind(it->second) == after->partitionKind(i) && before->partitionSignature(it->second) == after->partitionSignature(i)) {
                kept[i] = 1;
            } else if (topologyChanged(layer, *after, after->partitionPath(i))) {
                rebuilt.push_back(after->partitionPath(i));
            }
        }
    }
    const auto isKept = [&](const SdfPath& path) {
        const int partition = after->specPartition(path);

        return partition >= 0 && kept[std::size_t(partition)];
    };
    const auto underRebuilt = [&](const SdfPath& path) {
        for (const SdfPath& prim : rebuilt) {
            if (path != prim && path.HasPrefix(prim)) {
                return true;
            }
        }

        return false;
    };

    SpecCollector current;
    data->VisitSpecs(&current);
    std::vector<SdfPath> next;
    after->visitSpecs([&next](const SdfPath& path) {
        next.push_back(path);

        return true;
    });

    const SdfSchemaBase& schema = layer->GetSchema();
    std::string error;
    {
        SdfChangeBlock block;

        std::unordered_set<SdfPath, SdfPath::Hash> erased;
        for (const SdfPath& path : current.paths) {
            if (path == SdfPath::AbsoluteRootPath() || hasErasedAncestor(path, erased)) {
                continue;
            }
            if (!after->hasSpec(path) || after->specType(path) != layer->GetSpecType(path) || underRebuilt(path)) {
                eraseSpec(layer, path);
                erased.insert(path);
            }
        }

        std::unordered_set<SdfPath, SdfPath::Hash> authored;
        for (const SdfPath& path : next) {
            if (path == SdfPath::AbsoluteRootPath() || layer->HasSpec(path)) {
                continue;
            }
            if (!after->authorSpec(layer, path, UsdTimeCode::Default(), &error)) {
                break;
            }
            authored.insert(path);
        }

        if (error.empty()) {
            for (const SdfPath& path : next) {
                if (authored.count(path) || isKept(path)) {
                    continue;
                }
                std::set<TfToken> names;
                for (const TfToken& name : layer->ListFields(path)) {
                    names.insert(name);
                }
                for (const TfToken& name : after->fieldNames(path)) {
                    names.insert(name);
                }
                for (const TfToken& name : names) {
                    if (schema.HoldsChildren(name)) {
                        continue;
                    }
                    VtValue value;
                    if (!after->field(path, name, &value)) {
                        layer->EraseField(path, name);
                    } else if (layer->GetField(path, name) != value) {
                        layer->SetField(path, name, value);
                    }
                }
            }
        }
    }
    if (!error.empty()) {
        return fail(error);
    }

    // Removing and re-adding specs appends them to their parent's children, so the order can
    // differ from the conversion's; the edits then stay in the data rather than reorder silently.
    bool sameOrder = true;
    for (const SdfPath& path : next) {
        if (after->specType(path) != SdfSpecTypePrim && after->specType(path) != SdfSpecTypePseudoRoot) {
            continue;
        }
        for (const TfToken& children : { SdfChildrenKeys->PrimChildren, SdfChildrenKeys->PropertyChildren }) {
            VtValue expected;
            after->field(path, children, &expected);
            if (childrenField(layer->GetField(path, children)) != childrenField(expected)) {
                sameOrder = false;
                break;
            }
        }
        if (!sameOrder) {
            break;
        }
    }
    if (sameOrder) {
        data->rebase(after, detail);
    }

    return true;
}

} // namespace GeoLayers

NATRON_NAMESPACE_EXIT
