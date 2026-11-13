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

#include "TexturePaintCache.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include <OpenImageIO/imageio.h>

#include "Engine/ContentHasher.h"
#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/Nodes/IO/OiioWriteSupport.h"
#include "Engine/Nodes/Scene/MaterialBake.h"
#include "Engine/Nodes/Scene/PrimSelector.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/TextureProjection.h"

NATRON_NAMESPACE_ENTER

namespace TexturePaintCache {
namespace {
    // UsdPreviewSurface's own default diffuseColor, so a mesh with no usable colour looks as a
    // renderer would show it.
    const float kNeutralGrey = 0.18f;

    const char* const kNoteAttribute = "natron:texturePaintNote";

    bool
    fail(std::string* error,
         const std::string& message)
    {
        if (error) {
            *error = message;
        }

        return false;
    }

    std::string
    hex(U64 value)
    {
        char text[17];

        std::snprintf(text, sizeof(text), "%016llx", (unsigned long long)value);

        return std::string(text);
    }

    // Keeps a plane or frame name to the characters every file system takes.
    std::string
    fileSafe(const std::string& name)
    {
        std::string safe;

        for (char c : name) {
            safe += (std::isalnum((unsigned char)c) || (c == '-')) ? c : '_';
        }

        return safe;
    }

    bool
    isCancelled(const TileArgs& args,
                std::string* error)
    {
        if (args.cancelled && args.cancelled()) {
            if (error) {
                error->clear();
            }

            return true;
        }

        return false;
    }

    // Lays the projection of mask, the touched texels as alpha, over *rgba: what is below is scaled
    // by what the mask leaves of it.
    void
    attenuateBelow(const TextureProjection::Result& mask,
                   std::vector<float>* rgba)
    {
        const std::size_t count = mask.rgba.size() / 4;

        for (std::size_t i = 0; i < count; ++i) {
            const float a = mask.rgba[i * 4 + 3];
            if (a <= 0.f) {
                continue;
            }
            const float keep = std::max(0.f, 1.f - a);
            float* out = rgba->data() + i * 4;
            for (int c = 0; c < 4; ++c) {
                out[c] *= keep;
            }
        }
    }

    void
    addOver(const TextureProjection::Result& paint,
            std::vector<float>* rgba)
    {
        const std::size_t n = std::min(paint.rgba.size(), rgba->size());

        for (std::size_t i = 0; i < n; ++i) {
            (*rgba)[i] += paint.rgba[i];
        }
    }
} // anon namespace

PXR_NS::GfVec3f
meshBaseColor(const PXR_NS::UsdPrim& prim,
              PXR_NS::UsdTimeCode time)
{
    const PXR_NS::GfVec3f neutral(kNeutralGrey, kNeutralGrey, kNeutralGrey);

    if (!prim) {
        return neutral;
    }
    const PXR_NS::UsdShadeMaterial material = PXR_NS::UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial(PXR_NS::UsdShadeTokens->preview);
    if (!material) {
        return neutral;
    }
    const PXR_NS::UsdShadeShader surface = material.ComputeSurfaceSource();
    PXR_NS::TfToken shaderId;
    if (!surface || !surface.GetShaderId(&shaderId) || (shaderId != PXR_NS::TfToken("UsdPreviewSurface"))) {
        return neutral;
    }
    const PXR_NS::UsdShadeInput diffuse = surface.GetInput(PXR_NS::TfToken("diffuseColor"));
    if (!diffuse || diffuse.HasConnectedSource()) {
        return neutral;
    }
    PXR_NS::GfVec3f color;
    if (!diffuse.Get(&color, time)) {
        return neutral;
    }

    return color;
}

bool
matchedMeshes(const PXR_NS::UsdStageRefPtr& stage,
              const std::string& pattern,
              const PXR_NS::TfToken& stName,
              std::vector<PXR_NS::SdfPath>* meshes,
              std::string* error)
{
    meshes->clear();
    if (!stage) {
        return true;
    }
    if (!PrimSelector::validate(pattern, error)) {
        return false;
    }
    std::vector<PXR_NS::SdfPath> selected;
    if (!SceneOps::selectPrims(stage, pattern, &selected, error)) {
        return false;
    }
    for (const PXR_NS::SdfPath& path : selected) {
        const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(path);
        if (!prim || !prim.IsA<PXR_NS::UsdGeomMesh>()) {
            continue;
        }
        const PXR_NS::UsdGeomPrimvar primvar = PXR_NS::UsdGeomPrimvarsAPI(prim).GetPrimvar(stName);
        if (!primvar || !primvar.HasValue()) {
            continue;
        }
        meshes->push_back(path);
    }

    return true;
}

bool
composeTile(const TileArgs& args,
            bool colour,
            const std::vector<PaintSession>& sessions,
            std::vector<float>* rgba,
            std::vector<MeshPair>* overlaps,
            std::string* error)
{
    if (args.size <= 0) {
        return fail(error, "The tile is empty.");
    }
    const std::size_t count = (std::size_t)args.size * (std::size_t)args.size;

    rgba->assign(count * 4, 0.f);
    if (overlaps) {
        overlaps->clear();
    }
    if (args.meshes.empty()) {
        return true;
    }

    {
        std::vector<std::int32_t> owner;
        std::set<std::pair<std::int32_t, std::int32_t>> pairs;
        if (overlaps) {
            owner.assign(count, -1);
        }
        for (std::size_t m = 0; m < args.meshes.size(); ++m) {
            if (isCancelled(args, error)) {
                return false;
            }
            SceneRaster::BakeArgs bake;
            bake.primPath = args.meshes[m];
            bake.stName = args.stName;
            bake.time = args.time;
            bake.width = args.size;
            bake.height = args.size;
            bake.udimTile = args.udimTile;
            bake.cancelled = args.cancelled;
            SceneRaster::BakeBuffers buffers;
            if (!SceneRaster::bakeUV(args.stage, bake, &buffers, error)) {
                return false;
            }
            if (buffers.coverage.size() != count) {
                return fail(error, args.meshes[m].GetString() + " was baked at the wrong size.");
            }
            const PXR_NS::GfVec3f color = colour ? meshBaseColor(args.stage->GetPrimAtPath(args.meshes[m]), args.time) : PXR_NS::GfVec3f(0.f);
            for (std::size_t i = 0; i < count; ++i) {
                if (!buffers.coverage[i]) {
                    continue;
                }
                if (overlaps) {
                    if ((owner[i] >= 0) && (owner[i] != (std::int32_t)m)) {
                        pairs.insert(std::make_pair(owner[i], (std::int32_t)m));
                    }
                    owner[i] = (std::int32_t)m;
                }
                if (colour) {
                    float* out = rgba->data() + i * 4;
                    out[0] = color[0];
                    out[1] = color[1];
                    out[2] = color[2];
                    out[3] = 1.f;
                }
            }
        }
        if (overlaps) {
            for (const std::pair<std::int32_t, std::int32_t>& pair : pairs) {
                overlaps->push_back(MeshPair(args.meshes[(std::size_t)pair.first], args.meshes[(std::size_t)pair.second]));
            }
        }
    }
    if (!args.basePath.empty()) {
        std::vector<float> base;
        if (!MaterialBake::readBaseTile(args.basePath, args.size, &base, 0, error)) {
            return false;
        }
        *rgba = std::move(base);
    }

    for (const PaintSession& session : sessions) {
        if (!session.renderBuffer) {
            continue;
        }
        if (isCancelled(args, error)) {
            return false;
        }
        TexturePaint::SessionBuffer buffer;
        if (!session.renderBuffer(&buffer, error)) {
            return false;
        }
        if (isCancelled(args, error)) {
            return false;
        }
        const std::size_t texels = (std::size_t)std::max(0, buffer.width) * (std::size_t)std::max(0, buffer.height);
        if ((texels == 0) || (buffer.rgba.size() != texels * 4) || (buffer.touched.size() != texels)) {
            return fail(error, "A session's paint buffer is malformed.");
        }
        if (std::find(buffer.touched.begin(), buffer.touched.end(), (std::uint8_t)1) == buffer.touched.end()) {
            continue;
        }

        // The buffer holds the view under its strokes too; only what the strokes changed lands.
        std::vector<float> mask(texels * 4, 0.f);
        std::vector<float> masked(texels * 4, 0.f);
        for (std::size_t j = 0; j < texels; ++j) {
            if (!buffer.touched[j]) {
                continue;
            }
            mask[j * 4 + 3] = 1.f;
            for (int c = 0; c < 4; ++c) {
                masked[j * 4 + (std::size_t)c] = buffer.rgba[j * 4 + (std::size_t)c];
            }
        }
        buffer.rgba.clear();
        buffer.rgba.shrink_to_fit();

        TextureProjection::SourceImage source;
        source.width = buffer.width;
        source.height = buffer.height;
        source.pixelAspect = 1.;

        TextureProjection::Args project;
        project.explicitCamera = session.camera;
        project.stName = args.stName;
        project.time = args.time;
        project.udimTile = args.udimTile;
        project.width = args.size;
        project.height = args.size;
        project.occlusion = true;
        project.projectBackFaces = false;
        project.edgeDilation = 0;
        project.cancelled = args.cancelled;

        for (const PXR_NS::SdfPath& mesh : args.meshes) {
            if (isCancelled(args, error)) {
                return false;
            }
            project.meshPath = mesh;
            {
                TextureProjection::Result result;
                source.rgba = mask.data();
                if (!TextureProjection::project(args.stage, source, project, &result, error)) {
                    return false;
                }
                if (result.rgba.size() != count * 4) {
                    return fail(error, "A session was projected at the wrong size.");
                }
                attenuateBelow(result, rgba);
            }
            {
                TextureProjection::Result result;
                source.rgba = masked.data();
                if (!TextureProjection::project(args.stage, source, project, &result, error)) {
                    return false;
                }
                addOver(result, rgba);
            }
        }
    }

    return true;
} // composeTile

std::string
tileDirectory(const std::string& root,
              const std::string& projectFile,
              const std::string& nodeName)
{
    ContentHasher hasher;

    hasher.appendString(projectFile);
    hasher.appendString(nodeName);

    return root + "/TexturePaint/" + hex(hasher.finalize());
}

std::string
tilePath(const std::string& directory,
         U64 key,
         const std::string& frame,
         const std::string& plane,
         int udimTile)
{
    std::string name = hex(key) + "_";

    if (!frame.empty()) {
        name += "f" + fileSafe(frame) + "_";
    }
    name += fileSafe(plane) + "." + std::to_string(udimTile) + ".exr";

    return directory + "/" + name;
}

bool
writeTile(const std::string& path,
          int size,
          const std::vector<float>& rgba,
          const std::string& note,
          std::string* error)
{
    if ((size <= 0) || (rgba.size() != (std::size_t)size * (std::size_t)size * 4)) {
        return fail(error, "The tile to cache is malformed.");
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    if (ec) {
        return fail(error, "Could not create the texture cache directory: " + ec.message());
    }

    OIIO::ImageSpec spec(size, size, 4, OiioWriteSupport::dataTypeOiio(OiioWriteSupport::DataType::Half));
    spec.channelnames = { "R", "G", "B", "A" };
    // The texels are written as they are, not converted between premultiplied and straight.
    spec.alpha_channel = -1;
    spec.z_channel = -1;
    spec.attribute("oiio:UnassociatedAlpha", 1);
    spec.attribute("compression", "zip");
    spec.attribute(kNoteAttribute, note);

    std::vector<OiioWriteSupport::WritePart> parts(1);
    parts[0].spec = spec;
    const std::size_t rowFloats = (std::size_t)size * 4;
    const OiioWriteSupport::RowSource rows = [&](int /*part*/, int rowBegin, int rowEnd, float* band, OIIO::stride_t rowStride) -> bool {
        for (int r = rowBegin; r < rowEnd; ++r) {
            const float* src = rgba.data() + (std::size_t)(size - 1 - r) * rowFloats;
            float* dst = (float*)((char*)band + (std::ptrdiff_t)(r - rowBegin) * rowStride);
            std::copy(src, src + rowFloats, dst);
        }

        return true;
    };
    const OiioWriteSupport::CancelCheck never = []() -> bool {
        return false;
    };
    if (!OiioWriteSupport::writeFile(path, parts, rows, never, error)) {
        return false;
    }
    OiioReadSupport::evictHeaders(std::vector<std::string>(1, path));

    return true;
} // writeTile

bool
readTileNote(const std::string& path,
             int size,
             std::string* note)
{
    std::error_code ec;

    if (!std::filesystem::is_regular_file(path, ec)) {
        return false;
    }
    std::string error;
    const std::shared_ptr<const OiioReadSupport::Header> header = OiioReadSupport::readHeader(path, &error, FilePin::eDisk);
    if (!header || header->subimages.empty()) {
        return false;
    }
    const OIIO::ImageSpec& spec = header->subimages.front();
    if ((spec.width != size) || (spec.height != size) || (spec.nchannels != 4)) {
        return false;
    }
    *note = spec.get_string_attribute(kNoteAttribute);

    return true;
}

bool
readTile(const std::string& path,
         const RectI& window,
         std::vector<float>* rgba,
         std::string* error)
{
    const std::size_t rowFloats = (std::size_t)window.width() * 4;

    rgba->assign(rowFloats * (std::size_t)window.height(), 0.f);
    if (window.isNull()) {
        return true;
    }

    return OiioReadSupport::decode(path, 0, 0, 4, window, rgba->data(), rowFloats, error, FilePin::eDisk);
}

void
removeStaleTiles(const std::string& directory,
                 const std::set<U64>& keep)
{
    std::vector<std::string> prefixes;
    for (U64 key : keep) {
        prefixes.push_back(hex(key) + "_");
    }
    std::error_code ec;
    std::vector<std::filesystem::path> stale;

    for (std::filesystem::directory_iterator it(directory, ec), end; !ec && (it != end); it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (!it->is_regular_file(ec)) {
            continue;
        }
        bool kept = false;
        for (const std::string& prefix : prefixes) {
            if (name.compare(0, prefix.size(), prefix) == 0) {
                kept = true;
                break;
            }
        }
        if (!kept) {
            stale.push_back(it->path());
        }
    }
    for (const std::filesystem::path& path : stale) {
        std::filesystem::remove(path, ec);
    }
    if (!stale.empty()) {
        std::vector<std::string> paths;
        for (const std::filesystem::path& path : stale) {
            paths.push_back(path.string());
        }
        OiioReadSupport::evictHeaders(paths);
    }
}
} // namespace TexturePaintCache

NATRON_NAMESPACE_EXIT
