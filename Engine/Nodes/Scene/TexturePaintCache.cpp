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
#include <atomic>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include <signal.h>
#include <unistd.h>

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

    // Rows per band when meshes are laid out to colour the tile or find overlaps.
    const int kBandRows = 256;
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
    const bool hasBase = !args.basePath.empty();

    if (overlaps) {
        overlaps->clear();
    }
    if (hasBase) {
        if (!MaterialBake::readBaseTile(args.basePath, args.size, rgba, 0, error)) {
            return false;
        }
        if (rgba->size() != count * 4) {
            return fail(error, args.basePath + " was read at the wrong size.");
        }
    } else {
        rgba->assign(count * 4, 0.f);
    }
    if (args.meshes.empty()) {
        return true;
    }

    const bool paintMeshes = colour && !hasBase;
    if (paintMeshes || overlaps) {
        // The index of the last mesh covering each texel, plus one; 0 for none.
        std::vector<std::uint16_t> owner16;
        std::vector<std::uint32_t> owner32;
        const bool wide = args.meshes.size() >= 0xffff;
        if (overlaps) {
            if (wide) {
                owner32.assign(count, 0);
            } else {
                owner16.assign(count, 0);
            }
        }
        std::set<std::pair<std::uint32_t, std::uint32_t>> pairs;
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
            SceneRaster::UVLayout layout;
            if (!layout.prepare(args.stage, bake, error)) {
                return false;
            }
            const PXR_NS::GfVec3f color = paintMeshes ? meshBaseColor(args.stage->GetPrimAtPath(args.meshes[m]), args.time) : PXR_NS::GfVec3f(0.f);
            const std::uint32_t self = (std::uint32_t)m + 1;
            SceneRaster::BakeBuffers buffers;
            for (int firstRow = 0; firstRow < args.size; firstRow += kBandRows) {
                const int rows = std::min(kBandRows, args.size - firstRow);
                if (!layout.bakeRows(firstRow, rows, &buffers, error)) {
                    return false;
                }
                const std::size_t offset = (std::size_t)firstRow * (std::size_t)args.size;
                const std::size_t texels = (std::size_t)rows * (std::size_t)args.size;
                if (buffers.coverage.size() != texels) {
                    return fail(error, args.meshes[m].GetString() + " was baked at the wrong size.");
                }
                for (std::size_t j = 0; j < texels; ++j) {
                    if (!buffers.coverage[j]) {
                        continue;
                    }
                    const std::size_t i = offset + j;
                    if (overlaps) {
                        const std::uint32_t previous = wide ? owner32[i] : (std::uint32_t)owner16[i];
                        if ((previous != 0) && (previous != self)) {
                            pairs.insert(std::make_pair(previous - 1, self - 1));
                        }
                        if (wide) {
                            owner32[i] = self;
                        } else {
                            owner16[i] = (std::uint16_t)self;
                        }
                    }
                    if (paintMeshes) {
                        float* out = rgba->data() + i * 4;
                        out[0] = color[0];
                        out[1] = color[1];
                        out[2] = color[2];
                        out[3] = 1.f;
                    }
                }
            }
        }
        if (overlaps) {
            for (const std::pair<std::uint32_t, std::uint32_t>& pair : pairs) {
                overlaps->push_back(MeshPair(args.meshes[pair.first], args.meshes[pair.second]));
            }
        }
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
        std::vector<float> mask(texels, 0.f);
        for (std::size_t j = 0; j < texels; ++j) {
            if (buffer.touched[j]) {
                mask[j] = 1.f;
            } else {
                std::fill_n(buffer.rgba.begin() + (std::ptrdiff_t)(j * 4), 4, 0.f);
            }
        }
        std::vector<std::uint8_t>().swap(buffer.touched);

        TextureProjection::SourceImage source;
        source.rgba = buffer.rgba.data();
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
        if (!TextureProjection::projectOver(args.stage, source, mask.data(), project, args.meshes, rgba->data(), error)) {
            return false;
        }
    }

    return true;
} // composeTile

std::string
tileDirectory(const std::string& root,
              const std::string& nodeId,
              long pid)
{
    std::string name = fileSafe(nodeId);

    if (pid > 0) {
        name += "_p" + std::to_string(pid);
    }

    return root + "/TexturePaint/" + name;
}

void
pruneDeadProcessDirectories(const std::string& root)
{
    std::error_code ec;
    std::vector<std::filesystem::path> dead;

    for (std::filesystem::directory_iterator it(root + "/TexturePaint", ec), end; !ec && (it != end); it.increment(ec)) {
        const std::string name = it->path().filename().string();
        const std::size_t mark = name.rfind("_p");
        if ((mark == std::string::npos) || (mark + 2 >= name.size())) {
            continue;
        }
        const std::string digits = name.substr(mark + 2);
        if (!std::all_of(digits.begin(), digits.end(), [](char c) { return std::isdigit((unsigned char)c) != 0; })) {
            continue;
        }
        const long pid = std::strtol(digits.c_str(), 0, 10);
        if ((pid <= 0) || (pid == (long)::getpid())) {
            continue;
        }
        // EPERM means the process exists under another user.
        if ((::kill((pid_t)pid, 0) == 0) || (errno == EPERM)) {
            continue;
        }
        dead.push_back(it->path());
    }
    for (const std::filesystem::path& path : dead) {
        std::filesystem::remove_all(path, ec);
    }
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
    // Written beside the target and renamed over it, so that a reader or a crash never meets a
    // partial file under the final name; the extension stays last for the writer to pick EXR.
    static std::atomic<unsigned> counter(0);
    const std::filesystem::path target(path);
    std::filesystem::path temp = target;
    temp.replace_filename(target.stem().string() + ".tmp" + std::to_string((long)::getpid()) + "_" + std::to_string(++counter) + target.extension().string());
    if (!OiioWriteSupport::writeFile(temp.string(), parts, rows, never, error)) {
        std::filesystem::remove(temp, ec);

        return false;
    }
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);

        return fail(error, "Could not write " + path + ": " + ec.message());
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
