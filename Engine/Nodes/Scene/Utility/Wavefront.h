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
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec4f.h>

#include "Global/Macros.h"

#include "Engine/Nodes/Scene/Utility/Tracer.h"
#include "Engine/Nodes/Scene/Utility/UtilityKernels.h"
#include "Engine/Nodes/Scene/Utility/UtilityScene.h"
#include "Engine/Nodes/Scene/Utility/UtilityTextures.h"

NATRON_NAMESPACE_ENTER

class UtilityDeviceAovs;
class UtilityRenderBuffer;

// A rectangle of pixels in the render buffers. Row 0 is the bottom of the image.
struct WavefrontRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// The default for WavefrontFrame::maxCutouts.
const std::uint32_t kWavefrontDefaultMaxCutouts = 16;
// The defaults for WavefrontFrame's ray depths.
const std::uint32_t kWavefrontDefaultMaxRayDepth = 10;
const std::uint32_t kWavefrontDefaultReflectionDepth = 1;
const std::uint32_t kWavefrontDefaultRefractionDepth = 2;
// How a pixel is sampled: samplesPerSide squared samples over a filterWidth-pixel square, and which of
// them a render draws. One sample per pixel goes through its centre with a weight of one.
struct WavefrontSampling {
    int samplesPerSide = 1;
    KernelPixelFilter filter = eKernelPixelFilterBox;
    float filterWidth = 1.f;
    std::uint32_t seed = 0;
    int sampleIndex = 0;

    int samplesPerPixel() const
    {
        return samplesPerSide * samplesPerSide;
    }
};

// The one sample of every pixel that a pass of Raster mode draws: its offset from the pixel's centre in pixels,
// drawn from the pixel filter so that every pass weighs 1, and its time, from 0 at the shutter's opening to 1
// at its closing, as a ray's.
struct WavefrontRasterPass {
    float jitter[2] = { 0.f, 0.f };
    float weight = 1.f;
    float time = 0.5f;
};

// The default for WavefrontFrame::rasterSamples.
const int kWavefrontDefaultRasterSamples = 32;

// How pixels map to rays. Camera is the USD camera's frustum. Spherical is a 360 by 180 degree
// lat-long and Cylindrical a 360 degree band of the camera's vertical view, both about the camera's
// position in its orientation, the display window's centre looking along the camera. UvUnwrap gives
// each texel of one prim's UV tile a ray onto the surface at it.
enum class WavefrontMapping {
    Camera,
    Spherical,
    Cylindrical,
    UvUnwrap,
};

// The layout of WavefrontUnwrap::grid, as Kernels/RayGenUv.slang reads it: a header, then for each cell
// in rows from the bottom the offset of its first candidate, one more offset ending the last cell's, and
// then the candidates, each a triangle's index in Geometry::triangles.
enum WavefrontUnwrapGridField {
    eWavefrontUnwrapGridX = 0,
    eWavefrontUnwrapGridY,
    eWavefrontUnwrapGridColumns,
    eWavefrontUnwrapGridRows,
    eWavefrontUnwrapGridCellSize,
    eWavefrontUnwrapGridGeometry,
    eWavefrontUnwrapGridInstance,
    eWavefrontUnwrapGridReserved,
    eWavefrontUnwrapGridHeader,
};

// The floats per triangle of WavefrontUnwrap::triangles: its corners' pixel positions in the window,
// then how far off the surface its rays start, a spare float, and its corners' world positions, each
// followed by one component of its unit normal.
const std::size_t kWavefrontUnwrapTriangleFloats = 20;

// Where a UV unwrap's pixels lie on its prim, at one time. Every sample finds the triangle under it among
// the candidates of the grid cell it lands in, so that coverage and the surface position are the
// sample's own rather than its pixel centre's.
struct WavefrontUnwrap {
    WavefrontRect window;
    std::vector<std::int32_t> grid;
    // Indexed by triangle; those no cell lists are zero.
    std::vector<float> triangles;
    // The world size of a pixel on the surface, averaged over the prim.
    double footprint = 0.;
};
typedef std::shared_ptr<const WavefrontUnwrap> WavefrontUnwrapConstPtr;

// What one render draws.
struct WavefrontFrame {
    PXR_NS::GfMatrix4d worldToView = PXR_NS::GfMatrix4d(1.);
    PXR_NS::GfMatrix4d projection = PXR_NS::GfMatrix4d(1.);
    WavefrontRect window;
    // The part of the window the image's format covers, in the window's pixels; the rest is overscan.
    // Empty means all of it. The projection already maps the window, so only the other mappings use it.
    WavefrontRect display;
    WavefrontMapping mapping = WavefrontMapping::Camera;
    // A UvUnwrap's prim, the primvar its geometry's unwrapSt came from and its UDIM tile; the display
    // window spans the tile's 0 to 1. wavefrontDrawSamples() fills in unwrap from the scene it draws.
    PXR_NS::SdfPath unwrapPrim;
    std::string unwrapPrimvar;
    int udimTile = 1001;
    WavefrontUnwrapConstPtr unwrap;
    // Whether a UvUnwrap's paths go on past the baked surface; off, wavefrontBounceLimit() is 0.
    bool unwrapBounces = false;
    // The most holes a ray goes on through, in surfaces under their opacityThreshold, Project3D discards
    // and casters a shadow ray's light ignores; past them the ray ends as a miss.
    std::uint32_t maxCutouts = kWavefrontDefaultMaxCutouts;
    // The most bounces a path from the camera takes after its first hit: in all, by reflection and by
    // refraction, a total internal reflection counting as refraction. Plain transparency, at an ior
    // of 1, counts against the total only. A path with no depth left ends black.
    std::uint32_t maxRayDepth = kWavefrontDefaultMaxRayDepth;
    std::uint32_t reflectionDepth = kWavefrontDefaultReflectionDepth;
    std::uint32_t refractionDepth = kWavefrontDefaultRefractionDepth;
    WavefrontSampling sampling;
    // A camera that moves over the shutter, as RayGen.slang's cameraSteps binding takes it; empty for one
    // that does not. wavefrontDrawSamples() fills it in from the motion it draws with.
    std::vector<float> cameraSteps;
    // Raster mode, for the Camera mapping on a device that draws visibility buffers: each pass rasterizes which
    // surface every pixel's sample sees, with one jitter and one time for the whole frame, and rebuilds the
    // camera rays' hits from it; the rest of the path traces as Ray mode does. rasterSamples passes draw a frame.
    // wavefrontDrawSamples() fills in rasterPass, and moves the camera to its time rather than filling in
    // cameraSteps.
    bool raster = false;
    int rasterSamples = kWavefrontDefaultRasterSamples;
    WavefrontRasterPass rasterPass;
};

// The outputs a film takes in tiles. An executor may leave the arrays of the others null.
struct WavefrontAovs {
    bool color = true;
    bool depth = true;
    bool primId = true;
    bool instanceId = true;

    bool any() const { return color || depth || primId || instanceId; }
};

// One sample of each pixel of a tile, in rows from its bottom. Where hitGeometry is Tracer::kNoHit
// the arrays other than weight hold nothing.
struct WavefrontTile {
    WavefrontRect rect;
    const std::uint32_t* hitGeometry = nullptr;
    // The filter weight of each pixel's sample, misses included.
    const float* weight = nullptr;
    // Four per pixel; null when the colour went to the film's deviceColor() on the device.
    const float* color = nullptr;
    const float* depth = nullptr;
    const std::int32_t* primId = nullptr;
    const std::int32_t* instanceId = nullptr;
    // Whether a miss's colour is the background a dome light shows rather than the film's clear
    // colour; a miss's other AOVs stay clear either way.
    bool background = false;
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

    /**
     * @brief Where an executor on a device may keep the depth and ids, as long as it also keeps the colour on
     * the device, rather than hand them over in tiles; null when they must come in tiles. Taking a frame's first
     * pass there, the executor keeps every pass of the frame there.
     **/
    virtual UtilityDeviceAovs* deviceAovs() const
    {
        return nullptr;
    }

    // What writeTile() reads, the colour included even when deviceColor() may take it instead.
    virtual WavefrontAovs aovs() const
    {
        return WavefrontAovs();
    }

    // Each time a sample pass was drawn in full, before the next one starts: readers may be shown it.
    virtual void passDrawn()
    {
    }

    // Once the frame's last pass was drawn, after its passDrawn(): readers must be shown it.
    virtual void frameDrawn()
    {
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
     * overlap. Throws std::runtime_error, through wavefrontRequireUnwrap(), for a UvUnwrap frame
     * without its unwrap.
     **/
    virtual bool render(const UtilityScene::Snapshot& snapshot,
                        const WavefrontFrame& frame,
                        WavefrontFilm& film,
                        const std::atomic<bool>& stop,
                        int threads)
        = 0;

    // The bounces after the camera rays' that the last render() traced with any ray left: the deepest
    // any ray took.
    std::uint32_t bouncesTraced() const
    {
        return _bouncesTraced.load(std::memory_order_relaxed);
    }

    /**
     * @brief The scene at the ends of the shutter's segments, as UtilityScene::atShutter() takes it. The last
     * one is kept and given again for the same scene version and shutter, since taking it walks every moving
     * instance and deforming mesh, and a frame that changes only its sampling would take it whole again. Null
     * when stop ended it. Valid until the next call; calls must not overlap.
     **/
    const UtilityScene::Snapshot* shutterSnapshot(const UtilityScene::Snapshot& snapshot,
                                                  double open,
                                                  double close,
                                                  int segments,
                                                  const std::atomic<bool>& stop);

    // How many times shutterSnapshot() has taken the scene rather than given the kept one.
    std::size_t shutterSnapshotsTaken() const
    {
        return _shutterSnapshotsTaken;
    }

protected:
    void resetBouncesTraced()
    {
        _bouncesTraced.store(0, std::memory_order_relaxed);
    }

    void noteBounceTraced(std::uint32_t bounce)
    {
        std::uint32_t seen = _bouncesTraced.load(std::memory_order_relaxed);

        while (seen < bounce && !_bouncesTraced.compare_exchange_weak(seen, bounce, std::memory_order_relaxed)) {
        }
    }

private:
    std::atomic<std::uint32_t> _bouncesTraced { 0 };
    bool _shutterKept = false;
    std::uint64_t _shutterVersion = 0;
    double _shutterOpen = 0.;
    double _shutterClose = 0.;
    int _shutterSegments = 0;
    UtilityScene::Snapshot _shuttered;
    std::size_t _shutterSnapshotsTaken = 0;
};

// The shutter a frame is drawn over, in frames relative to it, and the camera within it.
struct WavefrontMotion {
    double open = 0.;
    double close = 0.;
    // How many spans the shutter is split into: the scene and the camera are taken at the ends of each
    // and every ray interpolates between the ends of the span its time falls in.
    int segments = 1;
    // The camera at increasing times, the frame's among them; empty for a camera that does not move.
    std::vector<double> cameraTimes;
    std::vector<PXR_NS::GfMatrix4d> cameraViews;
    std::vector<PXR_NS::GfMatrix4d> cameraProjections;

    bool isOpen() const
    {
        return close > open;
    }

    bool operator==(const WavefrontMotion& other) const
    {
        return open == other.open && close == other.close && segments == other.segments && cameraTimes == other.cameraTimes
            && cameraViews == other.cameraViews && cameraProjections == other.cameraProjections;
    }
};

/**
 * @brief Pass index of passes in Raster mode. Its jitter comes from the Halton sequence in bases 2 and 3, turned
 * by the seed, then spread through the inverse of the pixel filter's cumulative weight over its width, so that
 * every pass weighs the same and so does every time; one pass alone sits at the pixel centre. Its time is the centre of one of passes equal spans of the shutter, a
 * permutation under the seed choosing which, so that no jitter goes with the same time from frame to frame and
 * the spans, together, cover the shutter once.
 **/
WavefrontRasterPass wavefrontRasterPass(const WavefrontSampling& sampling,
                                        int index,
                                        int passes);

// The frame's camera moved by how the motion's camera at time differs from its camera at the frame.
WavefrontFrame wavefrontFrameAt(const WavefrontFrame& frame,
                                const WavefrontMotion& motion,
                                double time);

/**
 * @brief The rows of RayGen.slang's cameraSteps binding for the frame's camera over the motion's
 * shutter, at the ends of its segments; empty when the shutter is closed, the camera does not move or
 * the frame is a UvUnwrap.
 **/
std::vector<float> wavefrontCameraSteps(const WavefrontFrame& frame,
                                        const WavefrontMotion& motion);

/**
 * @brief Draws every sample of the frame, one per pixel per executor call, from one scene taken at the
 * ends of the shutter's segments and the camera at the same ends; each ray has its own time over the
 * shutter. progress, when set, hears how many samples are done after each. Returns false when stop
 * ended it. A UvUnwrap frame traces the snapshot as it stands, without blur. Throws std::runtime_error
 * when a UvUnwrap frame's prim cannot be unwrapped.
 **/
bool wavefrontDrawSamples(WavefrontExecutor& executor,
                          const UtilityScene::Snapshot& snapshot,
                          const WavefrontFrame& frame,
                          const WavefrontMotion& motion,
                          WavefrontFilm& film,
                          const std::atomic<bool>& stop,
                          int threads,
                          const std::function<void(std::size_t)>& progress);

/**
 * @brief Lays the frame's window out over the UV tile of frame.unwrapPrim, as the scene places its first
 * instance. Where triangles overlap in UV the later one wins. Returns null with *error set when the
 * scene has no such prim, the prim has no frame.unwrapPrimvar or its layout has more grid entries than
 * an int indexes, and null when stop ended it.
 **/
WavefrontUnwrapConstPtr wavefrontUnwrap(const UtilityScene::Snapshot& snapshot,
                                        const WavefrontFrame& frame,
                                        const std::atomic<bool>& stop,
                                        std::string* error);

// Throws std::runtime_error when frame is a UvUnwrap without its unwrap, which it cannot draw.
void wavefrontRequireUnwrap(const WavefrontFrame& frame);

/**
 * @brief Appends to data the importance table of a lat-long dome image, sized width by height bins over
 * the whole image: a conditional CDF per row, then the CDF over rows. Returns false, appending nothing,
 * when the image has no light in it.
 **/
bool wavefrontAppendDomeCdf(const UtilityTextureImage& image,
                            std::vector<float>* data,
                            std::uint32_t* width,
                            std::uint32_t* height);

/**
 * @brief Whether the frame's camera can make rays at all: its matrices invert to a finite frustum.
 **/
bool wavefrontCameraIsUsable(const WavefrontFrame& frame);

KernelRayGenParams makeRayGenParams(const WavefrontFrame& frame,
                                    const WavefrontRect& tile);

KernelShadeParams makeShadeParams(const WavefrontFrame& frame,
                                  std::uint32_t count);

/**
 * @brief The bounces after the camera rays' a frame traces at most: without transmissive surfaces only
 * reflections continue a path, so no more than the reflection depth. None for a UvUnwrap without
 * unwrapBounces.
 **/
std::uint32_t wavefrontBounceLimit(const WavefrontFrame& frame,
                                   bool transmission);

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
    std::uint32_t lightCount() const;
    // Whether a dome light shows where camera rays miss.
    bool hasBackground() const;
    // Whether any material can leave holes in its surface, which rays then need ResolveHits to go
    // through.
    bool hasHoles() const;
    // Whether any surface may let light through it, which then refracts camera rays, and shadow rays
    // need ResolveHits to go on through.
    bool hasTransmission() const;
    // The 32-bit words of each light-link and shadow-link mask.
    std::uint32_t linkWords() const;
    // What lets an occlusion query skip the casters a shadow ray's light is not shadow-linked to;
    // without casterMasks when every light is shadowed by everything.
    OcclusionFilter occlusionFilter(const std::uint32_t* rayLights) const;

private:
    // Returns false when stop ended it, leaving the texels incomplete.
    bool buildTexels(const std::atomic<bool>* stop);

    struct SourceVersion {
        UtilityScene::GeometryId id;
        std::uint64_t colorVersion;
        std::uint64_t topologyVersion;
        std::uint64_t pointsVersion;
        std::uint64_t stVersion;
        std::uint64_t normalsVersion;
        std::size_t pointCount;

        bool operator==(const SourceVersion& other) const
        {
            return id == other.id && colorVersion == other.colorVersion && topologyVersion == other.topologyVersion
                && pointsVersion == other.pointsVersion && stVersion == other.stVersion && normalsVersion == other.normalsVersion
                && pointCount == other.pointCount;
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
    // False while a stopped buildTexels() left the texels short of _textureAssets.
    bool _texelsComplete = false;
    std::vector<PXR_NS::GfVec4f> _points;
    std::vector<PXR_NS::GfVec4f> _instanceTransforms;
    // Per instance, the first row of its steps in _motionRows, kKernelNoMotion for one that does not move.
    std::vector<std::uint32_t> _instanceMotion;
    // Four floats per row: a header row holding the step count, then kKernelMotionRowsPerStep rows per step.
    std::vector<float> _motionRows;
    std::vector<PXR_NS::GfVec2f> _sts;
    std::vector<PXR_NS::GfVec4f> _normals;
    std::vector<SourceVersion> _sources;
    std::vector<KernelLightRecord> _lights;
    // The dome importance tables and IES tables the light records point into.
    std::vector<float> _lightData;
    // Each light's light-link and shadow-link category; empty for all of the scene.
    std::vector<std::pair<PXR_NS::TfToken, PXR_NS::TfToken>> _lightLinks;
    std::uint32_t _linkWords = 0;
    // Whether any light's shadows fall from only some of the scene, which needs filtered occlusion.
    bool _shadowLinked = false;
    bool _background = false;
    bool _holes = false;
    bool _transmission = false;
    std::vector<std::uint32_t> _linkMasks;
    // Material paths in the order of _materials from index 1.
    std::map<PXR_NS::SdfPath, std::uint32_t> _materialIndices;

    // Returns false when stop ended it, the textures it had not reached left unloaded.
    bool updateMaterials(const UtilityScene::Snapshot& snapshot,
                         const std::atomic<bool>* stop);
    std::uint64_t _version = 0;
    bool _synced = false;
};

/**
 * @brief Whether the frame's misses show the dome lights' background. A UV unwrap's misses are texels
 * no surface covers, which a bake leaves empty rather than filling with the environment.
 **/
bool wavefrontShowsBackground(const WavefrontFrame& frame,
                              const WavefrontShadeTables& tables);

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_Utility_Wavefront_h
