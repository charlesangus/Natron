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

#ifndef Engine_Nodes_Scene_MaterialBake_h
#define Engine_Nodes_Scene_MaterialBake_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Freezes the look of the materials bound to meshes into UDIM texture files: each texel a
 * mesh's UV layout covers takes the value its material gives the point of the mesh it maps to.
 *
 * What evaluates a material is a Backend, so that other evaluators (a renderer's, MaterialX's)
 * can bake the same files. Files hold one material input each, its purpose, named as
 * UsdPreviewSurface names its inputs.
 *
 * Every image is size * size RGBA floats, row-major and bottom-up; a colour purpose holds its
 * colour with alpha 1, a scalar one its value in R, G and B with alpha 1.
 **/
namespace MaterialBake {
#define kMaterialBakePurposeDiffuseColor "diffuseColor"

// The purposes a backend may bake beyond diffuseColor, in the order their files are written.
const std::vector<std::string>& optionalPurposes() WARN_UNUSED_RETURN;

struct MeshArgs {
    PXR_NS::UsdStageRefPtr stage;
    PXR_NS::SdfPath mesh;
    // The texture coordinate primvar that lays out the tiles, with or without "primvars:".
    PXR_NS::TfToken stName = PXR_NS::TfToken("st");
    PXR_NS::UsdTimeCode time = PXR_NS::UsdTimeCode::Default();
    int udimTile = 1001;
    int size = 0;
    // Checked between meshes and between stages of the evaluation; true stops the bake.
    std::function<bool()> cancelled;
};

class Backend {
public:
    virtual ~Backend() { }

    // Inserts into *purposes the optionalPurposes() the material bound to mesh gives a value of its own.
    virtual void addPurposes(const PXR_NS::UsdStageRefPtr& stage,
                             const PXR_NS::SdfPath& mesh,
                             PXR_NS::UsdTimeCode time,
                             std::set<std::string>* purposes) const
        = 0;

    /**
     * @brief Evaluates the material bound to args.mesh at each texel of the tile the mesh's UV
     * layout covers, for each of purposes into the image (*planes)[i] of purposes[i], which hold
     * args.size * args.size texels already; texels not covered are left as they are. Inputs it
     * stands a fallback in for are named in *warnings. False with *error set when the mesh cannot be
     * baked, and with *error empty once args.cancelled returns true.
     **/
    virtual bool bakeMesh(const MeshArgs& args,
                          const std::vector<std::string>& purposes,
                          std::vector<std::vector<float>>* planes,
                          std::vector<std::string>* warnings,
                          std::string* error)
        = 0;
};

/**
 * @brief The backend that evaluates UsdPreviewSurface on the CPU: constant inputs; UsdUVTexture
 * reading image files (UDIM ones included) or natron:// live textures, which must be in
 * LiveTextureStore already, through a UsdPrimvarReader_float2 and optionally a UsdTransform2d;
 * and a USDProject3D projection as the surface, which gives diffuseColor its projected colour.
 **/
std::unique_ptr<Backend> createPreviewSurfaceBackend() WARN_UNUSED_RETURN;

struct TilesArgs {
    PXR_NS::UsdStageRefPtr stage;
    std::vector<PXR_NS::SdfPath> meshes;
    PXR_NS::TfToken stName = PXR_NS::TfToken("st");
    PXR_NS::UsdTimeCode time = PXR_NS::UsdTimeCode::Default();
    int size = 0;
    // An absolute path holding the <UDIM> token; see purposePath().
    std::string pathPattern;
    std::function<bool()> cancelled;
};

struct BakedFile {
    std::string purpose;
    int udimTile = 0;
    std::string path;
};

/**
 * @brief Bakes every purpose backend gives any of args.meshes, diffuseColor always, into one
 * half-float OpenEXR file per UDIM tile the meshes cover, at the path purposePath() gives. Where
 * meshes' UV layouts overlap the later mesh wins. *files receives the files written, and
 * *warnings the backend's warnings, each once. False with *error set on failure, and with *error
 * empty once args.cancelled returns true; files already written stay.
 **/
bool bakeTiles(Backend& backend,
               const TilesArgs& args,
               std::vector<BakedFile>* files,
               std::vector<std::string>* warnings,
               std::string* error) WARN_UNUSED_RETURN;

/**
 * @brief The file of purpose at udimTile: pattern with <UDIM> replaced by the tile number, and for
 * a purpose other than diffuseColor "_<purpose>" inserted before the '.' or '_' that precedes
 * <UDIM> (or "_<purpose>." right before it when neither does). False with *error set when the
 * pattern holds no <UDIM>.
 **/
bool purposePath(const std::string& pattern,
                 const std::string& purpose,
                 int udimTile,
                 std::string* path,
                 std::string* error) WARN_UNUSED_RETURN;

/**
 * @brief The image at path as size * size RGBA texels into *rgba, resampled bilinearly when the
 * file is of another size, which *resampled tells. A file with fewer than four channels has its
 * colour spread and alpha 1. False with *error naming the path when it cannot be read.
 **/
bool readBaseTile(const std::string& path,
                  int size,
                  std::vector<float>* rgba,
                  bool* resampled,
                  std::string* error) WARN_UNUSED_RETURN;
} // namespace MaterialBake

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_MaterialBake_h
