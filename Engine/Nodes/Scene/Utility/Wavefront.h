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
#include <vector>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec4f.h>

#include "Global/Macros.h"

#include "Engine/Nodes/Scene/Utility/UtilityKernels.h"
#include "Engine/Nodes/Scene/Utility/UtilityScene.h"

NATRON_NAMESPACE_ENTER

class UtilityRenderBuffer;

// A rectangle of pixels in the render buffers. Row 0 is the bottom of the image.
struct WavefrontRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

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

// What one render draws.
struct WavefrontFrame {
    PXR_NS::GfMatrix4d worldToView = PXR_NS::GfMatrix4d(1.);
    PXR_NS::GfMatrix4d projection = PXR_NS::GfMatrix4d(1.);
    WavefrontRect window;
    WavefrontSampling sampling;
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

// The shutter a frame is drawn over, in frames relative to it, and the camera within it.
struct WavefrontMotion {
    double open = 0.;
    double close = 0.;
    // How many times within the shutter the scene is traced at; at most the pixel's samples.
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
 * @brief How many time buckets a frame of samplesPerPixel samples is drawn in: one when the shutter
 * is closed or nothing within it moves, else the segments, at most one per sample.
 **/
int wavefrontBucketCount(const WavefrontMotion& motion,
                         const UtilityScene::Snapshot& snapshot,
                         int samplesPerPixel);

// The frame-relative time bucket of buckets traces at: the middle of its share of the shutter.
double wavefrontBucketTime(const WavefrontMotion& motion,
                           int bucket,
                           int buckets);

/**
 * @brief The sample indices of a pixel in the order they are drawn: buckets runs of consecutive
 * entries, the first sampling the first bucket. Over more than one bucket each run takes cells
 * spread across the pixel, so that where in the pixel a sample falls says nothing of its time.
 **/
std::vector<int> wavefrontSampleOrder(int samplesPerPixel,
                                      int buckets);

// The frame's camera moved by how the motion's camera at time differs from its camera at the frame.
WavefrontFrame wavefrontFrameAt(const WavefrontFrame& frame,
                                const WavefrontMotion& motion,
                                double time);

/**
 * @brief Draws every sample of the frame, one per pixel per executor call, in time buckets: the
 * snapshot, evaluated at each bucket's time, and the camera at it trace that bucket's samples.
 * progress, when set, hears how many samples are done after each. bucketSeconds, when not null,
 * receives how long each bucket took, its scene's evaluation included. Returns false when stop ended it.
 **/
bool wavefrontDrawSamples(WavefrontExecutor& executor,
                          const UtilityScene::Snapshot& snapshot,
                          const WavefrontFrame& frame,
                          const WavefrontMotion& motion,
                          WavefrontFilm& film,
                          const std::atomic<bool>& stop,
                          int threads,
                          const std::function<void(std::size_t)>& progress,
                          std::vector<double>* bucketSeconds);

/**
 * @brief Whether the frame's camera can make rays at all: its matrices invert to a finite frustum.
 **/
bool wavefrontCameraIsUsable(const WavefrontFrame& frame);

KernelRayGenParams makeRayGenParams(const WavefrontFrame& frame,
                                    const WavefrontRect& tile);

KernelShadeParams makeShadeParams(const WavefrontFrame& frame,
                                  std::uint32_t count);

/**
 * @brief The scene's shading data as the flat tables ShadeFlat binds. A geometry's position in the
 * tables is its position in the snapshot, the index Tracer reports. Colours and triangles are only
 * recopied when a geometry's colours or topology changed.
 **/
class WavefrontShadeTables {
public:
    // Returns false when stop ended it early, leaving the tables for the next update to redo.
    bool update(const UtilityScene::Snapshot& snapshot,
                const std::atomic<bool>* stop = nullptr);

    // The tables as the kernel's buffers; the caller fills in what is per-tile.
    KernelShadeBuffers buffers() const;

private:
    struct SourceVersion {
        UtilityScene::GeometryId id;
        std::uint64_t colorVersion;
        std::uint64_t topologyVersion;
        std::size_t pointCount;

        bool operator==(const SourceVersion& other) const
        {
            return id == other.id && colorVersion == other.colorVersion && topologyVersion == other.topologyVersion && pointCount == other.pointCount;
        }
    };

    std::vector<KernelGeometryRecord> _geometries;
    std::vector<PXR_NS::GfVec4f> _colors;
    std::vector<std::int32_t> _triangles;
    std::vector<std::int32_t> _instanceIds;
    std::vector<SourceVersion> _sources;
    std::uint64_t _version = 0;
    bool _synced = false;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_Utility_Wavefront_h
