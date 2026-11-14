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
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
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

namespace {
    /**
     * The directories this process holds. Where flock() is emulated by POSIX record locks, as on
     * NFS, a process never conflicts with its own locks and closing any descriptor of the file
     * drops them all, so flock() alone cannot keep two holders of one process apart.
     **/
    std::mutex heldMutex;
    std::set<std::string> heldDirectories;
    std::atomic<bool> recordLocks(false);

    // An exclusive lock on all of fd's file, without waiting.
    bool
    lockFileNow(int fd)
    {
        if (!recordLocks.load()) {
            return ::flock(fd, LOCK_EX | LOCK_NB) == 0;
        }
        struct flock whole = {};
        whole.l_type = F_WRLCK;
        whole.l_whence = SEEK_SET;
        whole.l_start = 0;
        whole.l_len = 0;

        return ::fcntl(fd, F_SETLK, &whole) == 0;
    }

    bool
    holdInProcess(const std::string& directory)
    {
        std::lock_guard<std::mutex> lock(heldMutex);

        return heldDirectories.insert(directory).second;
    }

    void
    releaseInProcess(const std::string& directory)
    {
        std::lock_guard<std::mutex> lock(heldMutex);

        heldDirectories.erase(directory);
    }

    int
    lockDirectoryFile(const std::string& directory)
    {
        const std::string path = directory + "/.lock";

        for (int attempt = 0; attempt < 2; ++attempt) {
            std::error_code ec;
            std::filesystem::create_directories(directory, ec);
            if (ec) {
                return -1;
            }
            const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
            if (fd < 0) {
                return -1;
            }
            if (!lockFileNow(fd)) {
                ::close(fd);

                return -1;
            }
            // The holder before may have moved the directory aside to delete it between the open and
            // the lock, leaving this lock on a file under another name.
            struct stat held;
            struct stat named;
            if ((::fstat(fd, &held) == 0) && (::stat(path.c_str(), &named) == 0) && (held.st_dev == named.st_dev) && (held.st_ino == named.st_ino)) {
                return fd;
            }
            ::close(fd);
        }

        return -1;
    }

    // Locks directory's lock file, made with the directory when missing, and holds directory in
    // this process until unlockDirectory(). -1 when another node or process holds it, or it cannot
    // be made.
    int
    lockDirectory(const std::string& directory)
    {
        if (!holdInProcess(directory)) {
            return -1;
        }
        const int fd = lockDirectoryFile(directory);
        if (fd < 0) {
            releaseInProcess(directory);
        }

        return fd;
    }

    void
    unlockDirectory(const std::string& directory,
                    int fd)
    {
        if (fd < 0) {
            return;
        }
        ::close(fd);
        releaseInProcess(directory);
    }

    // pruneDeadProcessDirectories() judges a shared directory's age by its modification time,
    // which reading the tiles in it never changes.
    void
    touchDirectory(const std::string& directory)
    {
        std::error_code ec;

        std::filesystem::last_write_time(directory, std::filesystem::file_time_type::clock::now(), ec);
    }

    // Names that pruneDeadProcessDirectories() takes for the process's own, so that what a process
    // that died left aside is deleted with the rest of its directories.
    std::filesystem::path
    asideName(const std::filesystem::path& path)
    {
        static std::atomic<unsigned int> count(0);

        return path.parent_path() / (".removing_" + std::to_string(++count) + "_p" + std::to_string((long)::getpid()));
    }

    // Moves path aside and deletes it in the background, releasing claim once it is moved, or once it
    // is deleted when it cannot be moved.
    void
    removeAside(const std::filesystem::path& path,
                std::shared_ptr<TileDirectoryClaim> claim)
    {
        const std::filesystem::path aside = asideName(path);
        std::error_code ec;

        std::filesystem::rename(path, aside, ec);
        const std::filesystem::path doomed = ec ? path : aside;
        if (!ec) {
            claim.reset();
        }
        std::thread([doomed, claim]() {
            std::error_code ignored;
            std::filesystem::remove_all(doomed, ignored);
        }).detach();
    }
} // anon namespace

TileDirectoryClaim::TileDirectoryClaim(const std::string& path,
                                       bool shared,
                                       int lockFd)
    : _path(path)
    , _shared(shared)
    , _lockFd(lockFd)
{
}

TileDirectoryClaim::~TileDirectoryClaim()
{
    unlockDirectory(_path, _lockFd);
}

std::shared_ptr<TileDirectoryClaim>
claimTileDirectory(const std::string& root,
                   const std::string& nodeId,
                   bool shared)
{
    if (shared) {
        const std::string path = tileDirectory(root, nodeId, 0);
        const int fd = lockDirectory(path);
        if (fd >= 0) {
            touchDirectory(path);

            return std::make_shared<TileDirectoryClaim>(path, true, fd);
        }
    }
    const long pid = (long)::getpid();
    // A node of this process can still hold the first name: one deleted, whose cache id another
    // node took before its effect was destroyed.
    for (int n = 0; n < 64; ++n) {
        const std::string path = tileDirectory(root, (n == 0) ? nodeId : (nodeId + "_" + std::to_string(n)), pid);
        const int fd = lockDirectory(path);
        if (fd >= 0) {
            touchDirectory(path);

            return std::make_shared<TileDirectoryClaim>(path, false, fd);
        }
    }

    return std::shared_ptr<TileDirectoryClaim>();
}

void
setRecordLocksForTesting(bool locks)
{
    recordLocks = locks;
}

void
forgetProcessClaimsForTesting()
{
    std::lock_guard<std::mutex> lock(heldMutex);

    heldDirectories.clear();
}

void
removeTileDirectory(std::shared_ptr<TileDirectoryClaim> claim)
{
    if (!claim) {
        return;
    }
    const std::filesystem::path path(claim->getPath());

    removeAside(path, std::move(claim));
}

void
pruneDeadProcessDirectories(const std::string& root,
                            int olderThanDays)
{
    std::error_code ec;
    std::vector<std::filesystem::path> dead;
    std::vector<std::filesystem::path> old;
    const std::filesystem::file_time_type now = std::filesystem::file_time_type::clock::now();
    const std::filesystem::file_time_type::duration age = std::chrono::hours(24 * (long)olderThanDays);

    for (std::filesystem::directory_iterator it(root + "/TexturePaint", ec), end; !ec && (it != end); it.increment(ec)) {
        const std::string name = it->path().filename().string();
        const std::size_t mark = name.rfind("_p");
        const std::string digits = ((mark == std::string::npos) || (mark + 2 >= name.size())) ? std::string() : name.substr(mark + 2);
        if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](char c) { return std::isdigit((unsigned char)c) != 0; })) {
            std::error_code timeError;
            const std::filesystem::file_time_type modified = std::filesystem::last_write_time(it->path(), timeError);
            if (!timeError && (now - modified > age)) {
                old.push_back(it->path());
            }
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
    // A shared directory a process holds is in use, however old.
    for (const std::filesystem::path& path : old) {
        std::error_code lockError;
        const bool hadLockFile = std::filesystem::exists(path / ".lock", lockError);
        const int fd = lockDirectory(path.string());
        if (fd < 0) {
            continue;
        }
        // A claim made and released since the scan touched the directory. Making the lock file,
        // which every claim makes before its lock, touches it too, and says nothing of a claim.
        if (hadLockFile) {
            std::error_code timeError;
            const std::filesystem::file_time_type modified = std::filesystem::last_write_time(path, timeError);
            if (timeError || (std::filesystem::file_time_type::clock::now() - modified <= age)) {
                unlockDirectory(path.string(), fd);
                continue;
            }
        }
        const std::filesystem::path aside = asideName(path);
        std::error_code renameError;
        std::filesystem::rename(path, aside, renameError);
        unlockDirectory(path.string(), fd);
        if (!renameError) {
            std::filesystem::remove_all(aside, ec);
        }
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
          std::string* error,
          bool direct)
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
    if (direct) {
        return OiioWriteSupport::writeFile(path, parts, rows, never, error);
    }
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

std::map<U64, std::vector<std::string>>
listTiles(const std::string& directory)
{
    std::map<U64, std::vector<std::string>> tiles;
    std::error_code ec;

    for (std::filesystem::directory_iterator it(directory, ec), end; !ec && (it != end); it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if ((name.size() < 17) || (name[16] != '_') || !it->is_regular_file(ec)) {
            continue;
        }
        const std::string digits = name.substr(0, 16);
        if (!std::all_of(digits.begin(), digits.end(), [](char c) { return std::isxdigit((unsigned char)c) != 0; })) {
            continue;
        }
        tiles[(U64)std::strtoull(digits.c_str(), 0, 16)].push_back(it->path().string());
    }

    return tiles;
}

void
removeTiles(const std::vector<std::string>& paths)
{
    if (paths.empty()) {
        return;
    }
    for (const std::string& path : paths) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    OiioReadSupport::evictHeaders(paths);
}
} // namespace TexturePaintCache

NATRON_NAMESPACE_EXIT
