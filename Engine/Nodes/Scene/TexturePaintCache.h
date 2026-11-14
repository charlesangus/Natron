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

#ifndef Engine_Nodes_Scene_TexturePaintCache_h
#define Engine_Nodes_Scene_TexturePaintCache_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "Engine/Nodes/Scene/SceneRaster.h"
#include "Engine/Nodes/Scene/TexturePaint.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/RectI.h"
#include "Global/GlobalDefines.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief How TexturePaint turns its sessions into UDIM tiles, and the disk cache it keeps them in.
 *
 * A tile of one plane starts from the base: the file TileArgs::basePath names when set (the
 * material's look baked by MaterialBake), else, on the texels a matched mesh's UV layout covers,
 * the colour plane holds the constant diffuseColor of the mesh's UsdPreviewSurface
 * (meshBaseColor(), the colour the session views show) with alpha 1, and any other plane holds
 * zero. Each session,
 * oldest first, is projected into the tile through its camera with occlusion, back faces culled,
 * and laid over what is below with the projected touched mask as its alpha, so that a texel the
 * strokes did not change keeps the value below it.
 *
 * Tiles are cached on disk, one half-float RGBA OpenEXR file per plane, tile and frame, and read
 * back by region, so no more than the tile being baked is ever held in memory.
 **/
namespace TexturePaintCache {
/**
 * @brief The constant diffuseColor of the UsdPreviewSurface bound to prim, or UsdPreviewSurface's
 * default grey (0.18) when there is none, it is not a UsdPreviewSurface, or the colour comes
 * from a connection such as a texture.
 **/
PXR_NS::GfVec3f meshBaseColor(const PXR_NS::UsdPrim& prim, PXR_NS::UsdTimeCode time) WARN_UNUSED_RETURN;

/**
 * @brief The meshes of stage pattern selects that carry the primvar stName, in selection order.
 * Other prims, and meshes without the primvar, have no texels to paint and are left out. False
 * with *error set when the pattern is invalid.
 **/
bool matchedMeshes(const PXR_NS::UsdStageRefPtr& stage,
                   const std::string& pattern,
                   const PXR_NS::TfToken& stName,
                   std::vector<PXR_NS::SdfPath>* meshes,
                   std::string* error) WARN_UNUSED_RETURN;

struct TileArgs {
    PXR_NS::UsdStageRefPtr stage;
    std::vector<PXR_NS::SdfPath> meshes;
    PXR_NS::TfToken stName = PXR_NS::TfToken("st");
    PXR_NS::UsdTimeCode time = PXR_NS::UsdTimeCode::Default();
    int udimTile = 1001;
    // The tile's width and height in texels.
    int size = 0;
    // When set, the base of the plane is this file's texels, resampled to size when it is of
    // another size, in place of the constant base.
    std::string basePath;
    // When it returns true the composition stops and fails, between meshes and projections.
    std::function<bool()> cancelled;
};

// A session to lay over the tile: its camera, and how to get its buffer of the plane baked.
struct PaintSession {
    SceneRaster::ExplicitCamera camera;
    std::function<bool(TexturePaint::SessionBuffer* buffer, std::string* error)> renderBuffer;
};

typedef std::pair<PXR_NS::SdfPath, PXR_NS::SdfPath> MeshPair;

/**
 * @brief The tile args gives, of the colour plane when colour is set and of another plane
 * otherwise, with sessions laid over the base in order, into *rgba: size * size RGBA texels,
 * row-major and bottom-up. When overlaps is given it receives each pair of meshes whose UV
 * layouts cover a same texel of the tile, the earlier mesh first. False with *error set when a
 * mesh cannot be baked, a buffer cannot be rendered, or a projection fails; false with *error
 * empty once args.cancelled returns true.
 *
 * Memory: the tile, plus a band of one mesh's layout at a time (about 6.5 MB per 1K of tile
 * width), and while a session is projected its buffer, a float mask of it and the camera's depth;
 * with overlaps, two bytes per texel more.
 **/
bool composeTile(const TileArgs& args,
                 bool colour,
                 const std::vector<PaintSession>& sessions,
                 std::vector<float>* rgba,
                 std::vector<MeshPair>* overlaps,
                 std::string* error) WARN_UNUSED_RETURN;

/**
 * @brief The directory the tiles of one node are cached in: under root, named by the node's
 * cache id, which no other node shares, so that nodes whose content hashes agree but whose
 * relative paths resolve elsewhere never share files. pid is that of the process when the
 * project is unsaved, whose files no later process can name; 0 otherwise.
 **/
std::string tileDirectory(const std::string& root,
                          const std::string& nodeId,
                          long pid) WARN_UNUSED_RETURN;

/**
 * @brief A tile directory held by one node of one process, so that no other node or process uses
 * or prunes it while it is held. Releasing it deletes nothing.
 *
 * Within a process the directory is held in a registry of its own. Across processes it is held
 * by flock() on a file in it, which keeps the processes of one host apart. Across hosts sharing
 * the cache, exclusion holds only where the mount makes that lock one the server shares: NFS
 * does, by POSIX record locks, unless mounted with nolock or local_lock; some other network file
 * systems never do.
 **/
class TileDirectoryClaim {
public:
    TileDirectoryClaim(const std::string& path, bool shared, int lockFd);
    ~TileDirectoryClaim();

    TileDirectoryClaim(const TileDirectoryClaim&) = delete;
    TileDirectoryClaim& operator=(const TileDirectoryClaim&) = delete;

    const std::string& getPath() const { return _path; }

    // Whether the directory is the one tileDirectory() names for any process, to be reused.
    bool isShared() const { return _shared; }

private:
    std::string _path;
    bool _shared;
    int _lockFd;
};

/**
 * @brief Claims the directory tileDirectory() names for nodeId under root: with shared, the one
 * any process names, when no other node or process holds it, else, and without shared, one of
 * this process alone that nothing else holds. The directory's modification time is set to now.
 * Null when no directory can be made.
 **/
std::shared_ptr<TileDirectoryClaim> claimTileDirectory(const std::string& root,
                                                       const std::string& nodeId,
                                                       bool shared) WARN_UNUSED_RETURN;

/**
 * @brief While set, claims lock with POSIX record locks, fcntl(F_SETLK), as flock() becomes on
 * NFS, so that tests on a local disk exercise what keeps claims of one process apart there.
 **/
void setRecordLocksForTesting(bool recordLocks);

/**
 * @brief Forgets the directories this process holds, without unlocking them: for a child process
 * made by fork(), which inherits its parent's registry but none of its record locks.
 **/
void forgetProcessClaimsForTesting();

/**
 * @brief Deletes claim's directory and releases claim: the directory is moved aside at once, so
 * that a later claim of its name starts empty, and deleted in the background.
 **/
void removeTileDirectory(std::shared_ptr<TileDirectoryClaim> claim);

/**
 * @brief Deletes the directories under root that tileDirectory() named for a process that no
 * longer runs, and the shared ones nothing holds that have been neither changed nor claimed for
 * olderThanDays days.
 **/
void pruneDeadProcessDirectories(const std::string& root,
                                 int olderThanDays = 30);

/**
 * @brief The file in directory caching plane of tile under key; frame is empty for a texture
 * that is the same at every frame.
 **/
std::string tilePath(const std::string& directory,
                     U64 key,
                     const std::string& frame,
                     const std::string& plane,
                     int udimTile) WARN_UNUSED_RETURN;

/**
 * @brief Writes rgba, size * size RGBA texels bottom-up, to path as half floats, with note
 * stored beside the pixels. The file appears complete or not at all, unless direct: then it is
 * written straight to path, for a caller that already writes to a temporary file of its own.
 * False with *error set on failure.
 **/
bool writeTile(const std::string& path,
               int size,
               const std::vector<float>& rgba,
               const std::string& note,
               std::string* error,
               bool direct = false) WARN_UNUSED_RETURN;

/**
 * @brief Whether path holds a cached tile of size, with *note the note it was written with.
 **/
bool readTileNote(const std::string& path, int size, std::string* note) WARN_UNUSED_RETURN;

/**
 * @brief The texels of the tile at path over window, which must lie inside it, as RGBA floats
 * into *rgba, row-major and bottom-up. False with *error set on failure.
 **/
bool readTile(const std::string& path,
              const RectI& window,
              std::vector<float>* rgba,
              std::string* error) WARN_UNUSED_RETURN;

// The tile files in directory, by the key they were cached under, partly written ones included.
std::map<U64, std::vector<std::string>> listTiles(const std::string& directory) WARN_UNUSED_RETURN;

void removeTiles(const std::vector<std::string>& paths);
} // namespace TexturePaintCache

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_TexturePaintCache_h
