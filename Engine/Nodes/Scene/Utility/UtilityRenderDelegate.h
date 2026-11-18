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

#ifndef Engine_Nodes_Scene_Utility_UtilityRenderDelegate_h
#define Engine_Nodes_Scene_Utility_UtilityRenderDelegate_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <pxr/base/gf/vec2d.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/usd/sdf/path.h>

#include "Global/Macros.h"

#include "Engine/Nodes/Scene/Utility/UtilityScene.h"
#include "Engine/Nodes/Scene/Utility/Wavefront.h"

// The GetRenderStats keys, each holding a std::size_t.
#define kUtilityStatGeometryCount "utilityGeometryCount"
#define kUtilityStatInstanceCount "utilityInstanceCount"
#define kUtilityStatTriangleCount "utilityTriangleCount"
#define kUtilityStatTopologyBuilds "utilityTopologyBuilds"
#define kUtilityStatPointUpdates "utilityPointUpdates"
#define kUtilityStatColorUpdates "utilityColorUpdates"
#define kUtilityStatSceneVersion "utilitySceneVersion"
// The device the last frame rendered on, as a std::string.
#define kUtilityStatDevice "utilityDevice"
// Why the last frame did not render on the device asked for, as a std::string; empty when it did.
#define kUtilityStatWarning "utilityWarning"
// What the scene draws other than as authored, as a std::string of lines; empty when nothing.
#define kUtilityStatSceneWarning "utilitySceneWarning"
// The samples per pixel the running or last frame has drawn, and the number it draws in all.
#define kUtilityStatSamples "utilitySamples"
#define kUtilityStatSamplesWanted "utilitySamplesWanted"
// The seconds each time bucket of the last frame took to trace, its scene's update included, summed
// over its samples: a VtDoubleArray, one per bucket, the first opening the shutter.
#define kUtilityStatBucketSeconds "utilityBucketSeconds"
// Why some motion does not blur, naming the prims, as a std::string; empty when all of it does.
#define kUtilityStatMotionWarning "utilityMotionWarning"

// Which device draws: a TfToken, one of the three values below.
#define kUtilitySettingDevice "device"
#define kUtilityDeviceAuto "auto"
#define kUtilityDeviceGpu "gpu"
#define kUtilityDeviceCpu "cpu"

// The most cutout or projection-discard surfaces a ray goes through before it is a miss: an int.
#define kUtilitySettingMaxCutouts "maxCutouts"
// The most bounces a path from the camera takes after its first hit, in all, by reflection and by
// refraction: ints.
#define kUtilitySettingMaxRayDepth "maxRayDepth"
#define kUtilitySettingReflectionDepth "reflectionDepth"
#define kUtilitySettingRefractionDepth "refractionDepth"
// How pixels are sampled. Pixel Samples, an int, is the side of the grid of samples a pixel takes; it
// is not named samples, which the progressive viewer would take for its sample cap. Pixel Filter is a
// TfToken, one of the four values below. Filter Width, a float, is in pixels. Seed is an int.
#define kUtilitySettingPixelSamples "pixelSamples"
#define kUtilitySettingPixelFilter "pixelFilter"
#define kUtilitySettingFilterWidth "filterWidth"
#define kUtilitySettingSeed "seed"
#define kUtilityFilterBox "box"
#define kUtilityFilterTriangle "triangle"
#define kUtilityFilterGaussian "gaussian"
#define kUtilityFilterBlackmanHarris "blackmanHarris"
#define kUtilityDefaultPixelSamples 3
#define kUtilityDefaultFilterWidth 1.5f

// The shutter. The camera's shutter:open and shutter:close are used unless Override Shutter, a bool,
// is on; then Shutter, a float, is its length in frames, placed by Shutter Offset, a TfToken, one of
// the four values below; custom opens it Shutter Custom Offset frames, a float, from the frame.
// Shutter Segments, an int, is the number of times within it the scene is traced at. The three
// toggles, bools, choose which motion blurs.
#define kUtilitySettingOverrideShutter "overrideShutter"
#define kUtilitySettingShutter "shutter"
#define kUtilitySettingShutterOffset "shutterOffset"
#define kUtilitySettingShutterCustomOffset "shutterCustomOffset"
#define kUtilitySettingShutterSegments "shutterSegments"
#define kUtilitySettingCameraMotionBlur "cameraMotionBlur"
#define kUtilitySettingTransformMotionBlur "transformMotionBlur"
#define kUtilitySettingDeformationMotionBlur "deformationMotionBlur"
#define kUtilityShutterCentred "centred"
#define kUtilityShutterStart "start"
#define kUtilityShutterEnd "end"
#define kUtilityShutterCustom "custom"
#define kUtilityDefaultShutter 0.5f
#define kUtilityDefaultShutterSegments 8

// How pixels map to rays: Projection, a TfToken, one of the four values below. Overscan, an int, is how
// many pixels the frame grows by on every side, for each projection but uvUnwrap. A uvUnwrap render
// bakes the prim at UV Prim, a std::string path, through its UV Primvar, a std::string, over the UDIM
// Tile, an int, its texel grid the format. UV Bake Bounces, a bool, off by default, lets a uvUnwrap
// bake's paths go on past the surface by reflection, refraction and transparency.
#define kUtilitySettingProjection "projection"
#define kUtilitySettingOverscan "overscan"
#define kUtilitySettingUvPrim "uvPrim"
#define kUtilitySettingUvPrimvar "uvPrimvar"
#define kUtilitySettingUdimTile "udimTile"
#define kUtilitySettingUvBakeBounces "uvBakeBounces"
#define kUtilityProjectionCamera "camera"
#define kUtilityProjectionSpherical "spherical"
#define kUtilityProjectionCylindrical "cylindrical"
#define kUtilityProjectionUvUnwrap "uvUnwrap"
#define kUtilityDefaultUvPrimvar "st"
#define kUtilityDefaultUdimTile 1001

// The renderer command that has the delegate give back what it keeps on the GPU, as Free GPU Memory asks.
#define kUtilityCommandReleaseGpu "releaseGpu"
// The renderer command that has the delegate drop what it keeps on a lost GPU, so that the device can recover.
#define kUtilityCommandReleaseLostGpu "releaseLostGpu"

NATRON_NAMESPACE_ENTER

class UtilityRenderBuffer;
class WavefrontGpu;

/**
 * @brief What the delegate hands its prims when they sync.
 **/
class UtilityRenderParam : public PXR_NS::HdRenderParam {
public:
    UtilityRenderParam(UtilityScene* scene,
                       WavefrontExecutor* executor);
    virtual ~UtilityRenderParam();

    UtilityScene* scene() const;
    // The CPU's executor.
    WavefrontExecutor* executor() const;
    /**
     * @brief The GPU's executor, made on the first call after none or a dropped one, so that a
     * delegate made only to list its settings never touches the device. Null when the GPU cannot
     * render, with unavailable saying why, and null too when cancelFlag() was set while the application's
     * GPU was still starting or the executor still compiling its kernels, which the caller tells by the flag: the
     * compilation goes on, for the next call to wait for. Only the frame thread may call this or dropGpuExecutor(),
     * and the executor lives until dropGpuExecutor() or the param goes.
     **/
    WavefrontGpu* gpuExecutor(std::string* unavailable);
    // Gives the device's memory back, or lets the application recreate a lost device.
    void dropGpuExecutor();

    /**
     * @brief Remembers that scene version could not render on the GPU under budget on the runtime of GPU
     * generation (GpuContext::generation()), as it ran out of memory or has a table over the storage buffer
     * range, so that Auto renders it on the CPU straight away, without building it on the device for every
     * frame, until any of them changes. warning is what such a frame reports. Frame thread only, like
     * gpuExecutor().
     **/
    void rememberGpuFallback(std::uint64_t version,
                             std::uint64_t budget,
                             std::uint64_t generation,
                             const std::string& warning);
    // True with *warning set when version, budget and generation are those remembered.
    bool gpuFallback(std::uint64_t version,
                     std::uint64_t budget,
                     std::uint64_t generation,
                     std::string* warning) const;

    /**
     * @brief Ends the running frame and gives back everything the delegate keeps on the GPU: the executor,
     * with its scene, and the device films of the buffers, read back to the host first. The next frame
     * builds them again. MT-safe; not from the frame thread.
     **/
    void releaseGpu();

    /**
     * @brief Drops the executor when its device is lost, so that its hold no longer keeps the application
     * from recovering the device. Does nothing while a frame runs, since that frame drops it itself. MT-safe;
     * not from the frame thread.
     **/
    void releaseGpuIfLost();

    // The render buffers releaseGpu() reaches. MT-safe.
    void addBuffer(UtilityRenderBuffer* buffer);
    void removeBuffer(UtilityRenderBuffer* buffer);

    // What GetRenderStats() reports of the last frame. MT-safe.
    void setFrameReport(const std::string& device,
                        const std::string& warning);
    void frameReport(std::string* device,
                     std::string* warning) const;
    // What the stats report of the frame's samples per pixel. MT-safe.
    void setSampleProgress(std::size_t done,
                           std::size_t wanted);
    void sampleProgress(std::size_t* done,
                        std::size_t* wanted) const;
    // What the stats report of the frame's time buckets. MT-safe.
    void setBucketSeconds(const std::vector<double>& seconds);
    std::vector<double> bucketSeconds() const;

    // What a running frame polls: set by stop() until restart(), and by cancelFrame() until the next
    // frame starts. MT-safe.
    const std::atomic<bool>& cancelFlag() const;
    bool isStopped() const;
    // Ends the running frame and refuses new ones until restart(); when blocking, waits for the frame
    // to return. MT-safe.
    void stop(bool blocking);
    // Waits for a frame that a non-blocking stop is ending. MT-safe.
    void restart();

    /**
     * @brief Has a prim whose sync a stop cut short synced again, with dirtyBits, by the next render
     * pass sync: Hydra would not call it again on its own, its bits having been handed back clean.
     * MT-safe, since prims sync in parallel.
     **/
    void syncAgain(const PXR_NS::SdfPath& id,
                   PXR_NS::HdDirtyBits dirtyBits);
    std::vector<std::pair<PXR_NS::SdfPath, PXR_NS::HdDirtyBits>> takeSyncsAgain();

    /**
     * @brief Runs body on a thread of its own and returns at once, because Hydra's sync and everything
     * else under Render() holds locks the host's main thread can block on. Returns false, without
     * running it, when stopped. A frame must not be running.
     **/
    bool startFrame(const std::function<void()>& body);
    bool frameRunning() const;
    // Asks the running frame to end, without waiting for it. MT-safe.
    void cancelFrame();
    // For what a frame reads or writes going away. MT-safe.
    void cancelFrameAndWait();

private:
    void waitForFrame();

    UtilityScene* _scene;
    WavefrontExecutor* _executor;
    // Before _gpu, so that the executor goes before the runtime it was made on may.
    std::shared_ptr<void> _gpuHold;
#ifdef NATRON_HAVE_GPU
    std::unique_ptr<WavefrontGpu> _gpu;
    struct PendingGpu;
    std::shared_ptr<PendingGpu> _gpuPending;
#endif
    bool _gpuFallback = false;
    std::uint64_t _gpuFallbackVersion = 0;
    std::uint64_t _gpuFallbackBudget = 0;
    std::uint64_t _gpuFallbackGeneration = 0;
    std::string _gpuFallbackWarning;
    std::mutex _buffersMutex;
    std::set<UtilityRenderBuffer*> _buffers;
    mutable std::mutex _reportMutex;
    std::string _reportDevice;
    std::string _reportWarning;
    std::size_t _samplesDone = 0;
    std::size_t _samplesWanted = 0;
    std::vector<double> _bucketSeconds;
    std::atomic<bool> _stop { false };
    std::atomic<bool> _cancel { false };
    std::atomic<bool> _frameRunning { false };
    std::mutex _syncAgainMutex;
    std::vector<std::pair<PXR_NS::SdfPath, PXR_NS::HdDirtyBits>> _syncAgain;
    // Guards _thread, and is held only for as long as a cancelled frame takes to return.
    std::mutex _threadMutex;
    std::thread _thread;
};

/**
 * @brief The shutter, frame-relative, over which delegate's prims sample the motion toggleKey, one
 * of the motion blur toggles, enables: the host's (kRenderDelegateUtilityShutterSetting), closed
 * (both equal) while that toggle is off or the host set none.
 **/
PXR_NS::GfVec2d utilityMotionShutter(const PXR_NS::HdRenderDelegate& delegate,
                                     const char* toggleKey);

/**
 * @brief The Natron Utility render delegate: meshes and instancers become a UtilityScene that its
 * render passes draw on the CPU or the GPU, as the device setting says, on a thread of the delegate's
 * between Render() calls. Poll
 * IsConverged() for a finished frame, as for any delegate that renders in the background.
 **/
class UtilityRenderDelegate : public PXR_NS::HdRenderDelegate {
public:
    UtilityRenderDelegate();
    explicit UtilityRenderDelegate(const PXR_NS::HdRenderSettingsMap& settings);
    virtual ~UtilityRenderDelegate();

    // How many delegates exist right now, for the tests' leak checks. MT-safe.
    static int liveCount();

    virtual PXR_NS::HdRenderParam* GetRenderParam() const OVERRIDE;
    virtual const PXR_NS::TfTokenVector& GetSupportedRprimTypes() const OVERRIDE;
    virtual const PXR_NS::TfTokenVector& GetSupportedSprimTypes() const OVERRIDE;
    virtual const PXR_NS::TfTokenVector& GetSupportedBprimTypes() const OVERRIDE;
    virtual PXR_NS::HdResourceRegistrySharedPtr GetResourceRegistry() const OVERRIDE;
    virtual PXR_NS::HdRenderPassSharedPtr CreateRenderPass(PXR_NS::HdRenderIndex* index,
                                                           const PXR_NS::HdRprimCollection& collection) OVERRIDE;
    virtual PXR_NS::HdInstancer* CreateInstancer(PXR_NS::HdSceneDelegate* delegate,
                                                 const PXR_NS::SdfPath& id) OVERRIDE;
    virtual void DestroyInstancer(PXR_NS::HdInstancer* instancer) OVERRIDE;
    virtual PXR_NS::HdRprim* CreateRprim(const PXR_NS::TfToken& typeId,
                                         const PXR_NS::SdfPath& rprimId) OVERRIDE;
    virtual void DestroyRprim(PXR_NS::HdRprim* rprim) OVERRIDE;
    virtual PXR_NS::HdSprim* CreateSprim(const PXR_NS::TfToken& typeId,
                                         const PXR_NS::SdfPath& sprimId) OVERRIDE;
    virtual PXR_NS::HdSprim* CreateFallbackSprim(const PXR_NS::TfToken& typeId) OVERRIDE;
    virtual void DestroySprim(PXR_NS::HdSprim* sprim) OVERRIDE;
    virtual PXR_NS::HdBprim* CreateBprim(const PXR_NS::TfToken& typeId,
                                         const PXR_NS::SdfPath& bprimId) OVERRIDE;
    virtual PXR_NS::HdBprim* CreateFallbackBprim(const PXR_NS::TfToken& typeId) OVERRIDE;
    virtual void DestroyBprim(PXR_NS::HdBprim* bprim) OVERRIDE;
    virtual void CommitResources(PXR_NS::HdChangeTracker* tracker) OVERRIDE;
    virtual PXR_NS::HdAovDescriptor GetDefaultAovDescriptor(const PXR_NS::TfToken& name) const OVERRIDE;
    virtual PXR_NS::HdRenderSettingDescriptorList GetRenderSettingDescriptors() const OVERRIDE;
    virtual PXR_NS::VtDictionary GetRenderStats() const OVERRIDE;
    virtual PXR_NS::TfTokenVector GetShaderSourceTypes() const OVERRIDE;
    virtual bool IsStopSupported() const OVERRIDE;
    virtual bool IsStopped() const OVERRIDE;
    virtual bool Stop(bool blocking = true) OVERRIDE;
    virtual bool Restart() OVERRIDE;
    virtual PXR_NS::HdCommandDescriptors GetCommandDescriptors() const OVERRIDE;
    virtual bool InvokeCommand(const PXR_NS::TfToken& command,
                               const PXR_NS::HdCommandArgs& args = PXR_NS::HdCommandArgs()) OVERRIDE;

private:
    void construct();

    PXR_NS::HdResourceRegistrySharedPtr _resourceRegistry;
    std::unique_ptr<UtilityScene> _scene;
    std::unique_ptr<WavefrontExecutor> _executor;
    std::unique_ptr<UtilityRenderParam> _renderParam;
};

// What a uvUnwrap render bakes, as the delegate's settings give it; an empty prim for any other projection.
struct UtilityUnwrapTarget {
    PXR_NS::SdfPath prim;
    std::string primvar;
    int udimTile = kUtilityDefaultUdimTile;
    bool bounces = false;

    bool operator==(const UtilityUnwrapTarget& other) const
    {
        return prim == other.prim && primvar == other.primvar && udimTile == other.udimTile && bounces == other.bounces;
    }
};

UtilityUnwrapTarget utilityUnwrapTarget(const PXR_NS::HdRenderDelegate& delegate);

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_Utility_UtilityRenderDelegate_h
