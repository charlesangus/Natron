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

#ifndef Engine_Nodes_Scene_Utility_Wavefront_h
#define Engine_Nodes_Scene_Utility_Wavefront_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec4f.h>

#include "Global/Macros.h"

#include "Engine/Nodes/Scene/Utility/UtilityKernels.h"
#include "Engine/Nodes/Scene/Utility/UtilityScene.h"
#include "Engine/Nodes/Scene/Utility/UtilityTextures.h"

NATRON_NAMESPACE_ENTER

class UtilityRenderBuffer;

// A rectangle of pixels in the render buffers. Row 0 is the bottom of the image.
struct WavefrontRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// What one render draws.
struct WavefrontFrame {
    PXR_NS::GfMatrix4d worldToView = PXR_NS::GfMatrix4d(1.);
    PXR_NS::GfMatrix4d projection = PXR_NS::GfMatrix4d(1.);
    WavefrontRect window;
};

// The outputs a film takes in tiles. An executor may leave the arrays of the others null.
struct WavefrontAovs {
    bool color = true;
    bool depth = true;
    bool primId = true;
    bool instanceId = true;

    bool any() const { return color || depth || primId || instanceId; }
};

// Shaded pixels of one tile, in rows from its bottom. Where hitGeometry is Tracer::kNoHit the other
// arrays hold nothing.
struct WavefrontTile {
    WavefrontRect rect;
    const std::uint32_t* hitGeometry = nullptr;
    // Four per pixel; null when the colour went to the film's deviceColor() on the device.
    const float* color = nullptr;
    const float* depth = nullptr;
    const std::int32_t* primId = nullptr;
    const std::int32_t* instanceId = nullptr;
};

/**
 * @brief Receives what an executor renders.
 **/
class WavefrontFilm {
public:
    virtual ~WavefrontFilm() { }

    // MT-safe for tiles that do not overlap.
    virtual void writeTile(const WavefrontTile& tile) = 0;

    /**
     * @brief The one buffer the colour goes to, when an executor on a device may keep it there
     * rather than hand it over in tiles; null when the colour must come in tiles.
     **/
    virtual UtilityRenderBuffer* deviceColor() const
    {
        return nullptr;
    }

    // What writeTile() reads, the colour included even when deviceColor() may take it instead.
    virtual WavefrontAovs aovs() const
    {
        return WavefrontAovs();
    }
};

/**
 * @brief Runs ray generation, intersection, shading and film writing for a frame. Render passes see only
 * this, so a device's executor can replace the CPU's.
 **/
class WavefrontExecutor {
public:
    virtual ~WavefrontExecutor() { }

    /**
     * @brief Draws the frame into the film using at most threads threads, the caller included, polling
     * stop throughout, the scene's preparation included. Returns false when stop ended it early, leaving
     * the film partly written; the next call picks the scene up from a consistent state. Calls must not
     * overlap.
     **/
    virtual bool render(const UtilityScene::Snapshot& snapshot,
                        const WavefrontFrame& frame,
                        WavefrontFilm& film,
                        const std::atomic<bool>& stop,
                        int threads)
        = 0;
};

/**
 * @brief Whether the frame's camera can make rays at all: its matrices invert to a finite frustum.
 **/
bool wavefrontCameraIsUsable(const WavefrontFrame& frame);

KernelRayGenParams makeRayGenParams(const WavefrontFrame& frame,
                                    const WavefrontRect& tile);

KernelShadeParams makeShadeParams(const WavefrontFrame& frame,
                                  std::uint32_t count);

/**
 * @brief The scene's shading data as the flat tables ShadeSurface binds. A geometry's position in the
 * tables is its position in the snapshot, the index Tracer reports. Per-geometry arrays are only
 * recopied when a geometry's colours, coordinates, points or topology changed, and texels only when
 * the set of textures read changed. Material 0 is what geometry without a usable material shades as.
 **/
class WavefrontShadeTables {
public:
    // Returns false when stop ended it early, leaving the tables for the next update to redo.
    bool update(const UtilityScene::Snapshot& snapshot,
                const std::atomic<bool>* stop = nullptr);

    // The tables as the kernel's buffers; the caller fills in what is per-tile.
    KernelShadeBuffers buffers() const;
    // Changes whenever the texels do, so a device copy of them can be kept otherwise.
    std::uint64_t texelVersion() const;

private:
    struct SourceVersion {
        UtilityScene::GeometryId id;
        std::uint64_t colorVersion;
        std::uint64_t topologyVersion;
        std::uint64_t pointsVersion;
        std::uint64_t stVersion;
        std::size_t pointCount;

        bool operator==(const SourceVersion& other) const
        {
            return id == other.id && colorVersion == other.colorVersion && topologyVersion == other.topologyVersion
                && pointsVersion == other.pointsVersion && stVersion == other.stVersion && pointCount == other.pointCount;
        }
    };

    std::vector<KernelGeometryRecord> _geometries;
    std::vector<PXR_NS::GfVec4f> _colors;
    std::vector<std::int32_t> _triangles;
    std::vector<std::int32_t> _instanceIds;
    std::vector<KernelMaterialRecord> _materials;
    std::vector<KernelTextureBinding> _textureBindings;
    std::vector<KernelProjectionRecord> _projections;
    std::vector<std::uint32_t> _images;
    std::vector<PXR_NS::GfVec4f> _texels;
    std::vector<UtilityTextureAssetConstPtr> _textureAssets;
    std::uint64_t _texelVersion = 0;
    std::vector<PXR_NS::GfVec4f> _points;
    std::vector<PXR_NS::GfVec4f> _instanceTransforms;
    std::vector<PXR_NS::GfVec2f> _sts;
    std::vector<SourceVersion> _sources;
    // Material paths in the order of _materials from index 1.
    std::map<PXR_NS::SdfPath, std::uint32_t> _materialIndices;

    void updateMaterials(const UtilityScene::Snapshot& snapshot);
    std::uint64_t _version = 0;
    bool _synced = false;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_Utility_Wavefront_h
