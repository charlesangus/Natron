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

#ifndef Engine_Nodes_Scene_Utility_UtilityRenderBuffer_h
#define Engine_Nodes_Scene_Utility_UtilityRenderBuffer_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <pxr/imaging/hd/renderBuffer.h>

#include "Global/Macros.h"

#ifdef NATRON_HAVE_GPU
namespace gpu {
class GpuBuffer;
class GpuBufferHandle;
class GpuBufferPool;
class GpuDevice;
class GpuKernel;
class GpuKernelRegistry;
class GpuRecordContext;
struct GpuStatus;
}
#endif

NATRON_NAMESPACE_ENTER

#ifdef NATRON_HAVE_GPU
class UtilityDeviceFilm;
struct UtilityFilmKernels;

// The kernels a device film runs, made once per device. Null with *status set when the device cannot make them.
std::shared_ptr<const UtilityFilmKernels> makeUtilityFilmKernels(gpu::GpuDevice& device,
                                                                 gpu::GpuStatus* status);

// Every pipeline of `kernels`.
std::vector<std::shared_ptr<gpu::GpuKernel>> utilityFilmKernelList(const UtilityFilmKernels& kernels);

/**
 * @brief Releases every device film whose device is lost, wherever it is held, so that none keeps the runtime from
 * recovering. GpuContext::recoverIfLost() calls it once a film was made. MT-safe.
 **/
void releaseLostUtilityDeviceFilms();

/**
 * @brief What UtilityRenderBuffer::GetResource() returns while a frame's colour is on the device: the
 * film, whose pixels are RGBA floats, row by row from the bottom row.
 **/
class UtilityDeviceResource {
public:
    UtilityDeviceResource();
    explicit UtilityDeviceResource(const std::shared_ptr<UtilityDeviceFilm>& film);

    // Null without a film or once it was released.
    gpu::GpuDevice* device() const;
    unsigned int width() const;
    unsigned int height() const;

    /**
     * @brief Records into context a copy of the film's window [x1, x2) x [y1, y2) into out, as
     * HydraRender::PlaneWindow lays out a colour window of nComps components, from out's float
     * outOffset with outRowStride floats from row to row. context must be on the film's device.
     * The film is kept until context's work is done.
     **/
    gpu::GpuStatus recordWindow(gpu::GpuRecordContext& context,
                                int x1,
                                int y1,
                                int x2,
                                int y2,
                                int nComps,
                                const gpu::GpuBuffer& out,
                                std::uint64_t outOffset,
                                std::uint32_t outRowStride) const;

    /**
     * @brief The film's pixels, width() x height() RGBA floats with no padding, and the hold on the runtime that keeps
     * them valid while the caller uses them. False without a film or once it was released.
     **/
    bool lease(gpu::GpuBufferHandle* pixels,
               std::shared_ptr<void>* runtimeHold) const;

    // Releases the film, and with it every copy of this resource, when its device is lost.
    void releaseIfLost() const;

    /**
     * @brief Releases the film, and with it every copy of this resource; work already recorded on it
     * keeps what it reads until it is done.
     **/
    void release() const;

    /**
     * @brief Copies the film's pixels into *out, as pixels() lays them out, and waits for the copy.
     * False with *error set when the device cannot.
     **/
    bool download(std::vector<float>* out,
                  std::string* error) const;

    bool operator==(const UtilityDeviceResource& other) const
    {
        return _film == other._film;
    }

    bool operator!=(const UtilityDeviceResource& other) const
    {
        return _film != other._film;
    }

private:
    std::shared_ptr<UtilityDeviceFilm> _film;
};

class UtilityRenderBuffer;
class UtilityPickSnapshot;

enum class UtilityPickAov {
    Depth,
    PrimId,
    InstanceId
};

#ifdef NATRON_HAVE_GPU
class UtilityDeviceIds;

/**
 * @brief A published frame's depth or prim ids still on the device, as UtilityRenderBuffer::GetResource() gives it
 * for those AOVs: the frame's picks, read as aov() over a buffer of width() x height() pixels. Copies share the picks.
 **/
class UtilityDevicePicks {
public:
    UtilityDevicePicks();
    UtilityDevicePicks(const std::shared_ptr<UtilityPickSnapshot>& picks,
                       UtilityPickAov aov,
                       unsigned int width,
                       unsigned int height);

    // Null without picks or once they were released.
    gpu::GpuDevice* device() const;
    UtilityPickAov aov() const;
    unsigned int width() const;
    unsigned int height() const;

    /**
     * @brief The frame's prim ids on the device: every distinct (prim, instance) pair its hits show, given to hashOf
     * once each on the calling thread, which returns the pair's prim path hash, or 0 for a pixel left at 0. Waits
     * for the device. False with *error set when the frame shows more distinct pairs than the device keeps, or the
     * device cannot, so that the caller decodes the ids on the CPU instead.
     **/
    bool resolveIds(const std::function<std::uint32_t(std::int32_t prim, std::int32_t instance)>& hashOf,
                    std::shared_ptr<const UtilityDeviceIds>* out,
                    std::string* error) const;

    /**
     * @brief Records into context the window [x1, x2) x [y1, y2) of the buffer into out, as HydraRender::PlaneWindow
     * lays out a depth or prim id window of nComps components, from out's float outOffset with outRowStride floats
     * from row to row. Depth goes from the window depth to the distance along the view axis through depthFromWindow:
     * columns 2 and 3 of the inverse projection, rows 0 to 3 of each. A prim id window needs ids, which resolveIds()
     * made from these picks. context must be on the picks' device; they and ids are kept until its work is done.
     **/
    gpu::GpuStatus recordWindow(gpu::GpuRecordContext& context,
                                int x1,
                                int y1,
                                int x2,
                                int y2,
                                int nComps,
                                const gpu::GpuBuffer& out,
                                std::uint64_t outOffset,
                                std::uint32_t outRowStride,
                                const std::array<float, 8>& depthFromWindow,
                                const std::shared_ptr<const UtilityDeviceIds>& ids) const;

    // Releases the picks, and with them every copy of this resource, when their device is lost.
    void releaseIfLost() const;

    bool operator==(const UtilityDevicePicks& other) const
    {
        return (_picks == other._picks) && (_aov == other._aov) && (_width == other._width) && (_height == other._height);
    }

    bool operator!=(const UtilityDevicePicks& other) const
    {
        return !(*this == other);
    }

private:
    std::shared_ptr<UtilityPickSnapshot> _picks;
    UtilityPickAov _aov = UtilityPickAov::Depth;
    unsigned int _width = 0;
    unsigned int _height = 0;
};
#endif

/**
 * @brief One frame's depth, prim id and instance id kept on the device: for each pixel of the window the frame
 * draws, the heaviest of its samples so far, as an unfiltered UtilityRenderBuffer::addSample() keeps it. The
 * frame's thread alone uses it, from its first pass on; publish() hands the buffers copies of it.
 **/
class UtilityDeviceAovs {
public:
    UtilityDeviceAovs();
    ~UtilityDeviceAovs();

    UtilityDeviceAovs(const UtilityDeviceAovs&) = delete;
    UtilityDeviceAovs& operator=(const UtilityDeviceAovs&) = delete;

    // Whether a window of width x height pixels fits the one binding of context's device the picks take.
    static bool fits(const gpu::GpuRecordContext& context,
                     int width,
                     int height);

    /**
     * @brief Starts a pass over the window from (x, y). The frame's first pass over a window writes every pixel's
     * sample without reading what the device held, so the picks need no clear. runtimeHold keeps pool and
     * context's device valid while the picks live.
     **/
    gpu::GpuStatus beginPass(gpu::GpuRecordContext& context,
                             gpu::GpuBufferPool& pool,
                             const std::shared_ptr<const UtilityFilmKernels>& kernels,
                             const std::shared_ptr<void>& runtimeHold,
                             int x,
                             int y,
                             int width,
                             int height);

    /**
     * @brief Records into context the keeping of a batch's samples, for the rays of the window's whole rows from
     * y on, rows of them; the buffers are laid out as the shading kernel writes them, weights as the ray
     * generation does. After beginPass() only.
     **/
    gpu::GpuStatus recordBatch(gpu::GpuRecordContext& context,
                               const gpu::GpuBuffer& hitIds,
                               const gpu::GpuBuffer& weights,
                               const gpu::GpuBuffer& depth,
                               const gpu::GpuBuffer& primId,
                               const gpu::GpuBuffer& instanceId,
                               int y,
                               int rows);

    // The pass begun last was drawn over the whole window.
    void endPass();

    // Whether a whole pass was drawn since the picks were made or dropped.
    bool drawn() const;

    /**
     * @brief What a miss leaves in each AOV: the clear value of the first of its buffers, when that is one 32-bit
     * component; zero otherwise. Before the frame's first pass.
     **/
    void setMisses(const std::vector<UtilityRenderBuffer*>& depth,
                   const std::vector<UtilityRenderBuffer*>& primId,
                   const std::vector<UtilityRenderBuffer*>& instanceId);

    /**
     * @brief Hands every buffer its AOV of the picks as they are, in a copy on the device made once the passes so
     * far are done; each buffer reads back only its own AOV, on the first Map() that asks for it. Without a pass
     * drawn the buffers publish their host pixels instead. False with *error set when the device cannot.
     **/
    bool publish(const std::vector<UtilityRenderBuffer*>& depth,
                 const std::vector<UtilityRenderBuffer*>& primId,
                 const std::vector<UtilityRenderBuffer*>& instanceId,
                 std::string* error);

    // Lets go of the device memory; the next pass starts the picks over.
    void drop();

private:
    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};
#endif // NATRON_HAVE_GPU

/**
 * @brief An AOV image built from samples, in host memory or, for the colour of a frame rendered on
 * the GPU, in device memory until Map() asks for it. Its pixels are resolved as samples land, so a
 * frame part way through its samples is a picture. Readers see a copy made between passes, never the
 * pixels a running frame writes, so a read can overlap the next pass without blocking it.
 **/
class UtilityRenderBuffer : public PXR_NS::HdRenderBuffer {
public:
    explicit UtilityRenderBuffer(const PXR_NS::SdfPath& id);
    virtual ~UtilityRenderBuffer();

    // How many buffers exist right now, for the tests' leak checks. MT-safe.
    static int liveCount();

    // A running frame writes straight into the pixels, so both of these end it first.
    virtual void Sync(PXR_NS::HdSceneDelegate* sceneDelegate,
                      PXR_NS::HdRenderParam* renderParam,
                      PXR_NS::HdDirtyBits* dirtyBits) OVERRIDE;
    virtual void Finalize(PXR_NS::HdRenderParam* renderParam) OVERRIDE;

    virtual bool Allocate(const PXR_NS::GfVec3i& dimensions,
                          PXR_NS::HdFormat format,
                          bool multiSampled) OVERRIDE;
    virtual unsigned int GetWidth() const OVERRIDE;
    virtual unsigned int GetHeight() const OVERRIDE;
    virtual unsigned int GetDepth() const OVERRIDE;
    virtual PXR_NS::HdFormat GetFormat() const OVERRIDE;
    virtual bool IsMultiSampled() const OVERRIDE;
    // The pixels as of the last publishPass() or clear(); every Map() until the last Unmap() gets the same ones.
    virtual void* Map() OVERRIDE;
    virtual void Unmap() OVERRIDE;
    virtual bool IsMapped() const OVERRIDE;
    virtual void Resolve() OVERRIDE;
    virtual bool IsConverged() const OVERRIDE;

    // A UtilityDeviceResource while the device holds the last published colour, a UtilityDevicePicks while it holds the
    // last published depth or prim ids, else empty.
    virtual PXR_NS::VtValue GetResource(bool multiSampled) const OVERRIDE;

    /**
     * @brief Fills every pixel with value: a scalar or a vector of float, double or int. Components
     * the value does not have are written as zero. An empty value leaves the buffer as it is.
     **/
    void clear(const PXR_NS::VtValue& value);
    /**
     * @brief clear(), but with deferHost the host pixels and sample sums are only filled on the first need of
     * them: ensureHost() before writing them with setPixel() or addSample(), or a reader's Map(). A frame
     * drawn on the device never needs them.
     **/
    void clear(const PXR_NS::VtValue& value,
               bool deferHost);
    // MT-safe; returns at once when the host pixels are filled.
    void ensureHost();
    // Whether the last clear() value is one 32-bit component, and its bits in *word if so.
    bool clearWord(std::uint32_t* word) const;
    void setConverged(bool converged);

    /**
     * @brief Hands readers the pixels as they are: for the thread drawing the frame, once a pass is
     * complete and before the next one starts writing.
     **/
    void publishPass();

    /**
     * @brief Writes one pixel; one outside the buffer is ignored. Components the buffer's format does
     * not have are dropped and the ones count does not supply are left as they are. MT-safe for
     * distinct pixels.
     **/
    void setPixel(unsigned int x,
                  unsigned int y,
                  const float* components,
                  std::size_t count);
    void setPixel(unsigned int x,
                  unsigned int y,
                  std::int32_t value);

    /**
     * @brief Adds a sample of weight to a pixel; null components is a miss, which counts as the last
     * clear() value. Filtered, the pixel is the weighted mean of its samples since that clear();
     * otherwise it is the value of the heaviest of them, as a mean of depths or ids would be a value
     * no surface has. One outside the buffer is ignored. MT-safe for distinct pixels.
     **/
    void addSample(unsigned int x,
                   unsigned int y,
                   const float* components,
                   std::size_t count,
                   float weight,
                   bool filtered);
    void addSample(unsigned int x,
                   unsigned int y,
                   const std::int32_t* value,
                   float weight);

    // How many times Map() has copied a device frame to the host, over every buffer. MT-safe.
    static std::uint64_t deviceDownloadCount();
    // How many times an AOV was read back from the device picks, process-wide.
    static std::uint64_t pickReadbackCount();
    // How many times a buffer has been set converged, over every buffer; what waitForConverged() waits past. MT-safe.
    static std::uint64_t convergedCount();
    /**
     * @brief Returns once convergedCount() is past seen, wakeConvergedWaiters() is called, or bound has passed,
     * whichever comes first, so a caller waiting for a frame to converge sees it at once and can still see a
     * cancel. MT-safe.
     **/
    static void waitForConverged(std::uint64_t seen,
                                 std::chrono::milliseconds bound);
    static void wakeConvergedWaiters();

#ifdef NATRON_HAVE_GPU
    /**
     * @brief Releases the device film, for every holder of a copy of it: the frame is read back to the host pixels
     * first unless the device is lost. Not while a frame runs.
     **/
    void releaseDevice();

    /**
     * @brief Whether a frame can keep these pixels on the device: they are RGBA floats and the last
     * clear() gave a value, which the device frame starts from.
     **/
    bool canRenderOnDevice() const;

    /**
     * @brief Moves the pixels to the device for a frame's samples: the first call after clear()
     * records the clear to its value; a later one has the samples add to those the film holds from
     * the frame's earlier calls, and fails if the film is gone. The host pixels are stale from here
     * until Map() or clear().
     *
     * record gives the context to record the next `work` units of work into, a unit costing about
     * one traced ray, having submitted and waited for what was recorded first when it asks to, or
     * null when the frame was stopped or a submission failed. The film's clear, or its copy when
     * another holder still has it, goes in pieces of at most maxWork units each, or the fewest rows
     * the device's offset alignment lets a band start on, so that a stop never waits for a whole
     * film. A frame stopped part way keeps the film as it was: the next call clears again, or
     * copies the samples of the earlier calls again. The submissions run outside the buffer's lock,
     * so that no reader waits on them; a clear() or release meanwhile fails the call with
     * *superseded set, the frame having been stopped rather than failed.
     **/
    gpu::GpuStatus beginDeviceFrame(const std::function<gpu::GpuRecordContext*(std::uint64_t work)>& record,
                                    std::uint64_t maxWork,
                                    gpu::GpuKernelRegistry& registry,
                                    gpu::GpuBufferPool& pool,
                                    const std::shared_ptr<const UtilityFilmKernels>& kernels,
                                    const std::shared_ptr<void>& runtimeHold,
                                    bool* superseded);

    /**
     * @brief Records into context the adding of a batch's colours, for the rays of the width x
     * height pixels from (x, y), to their pixels with the rays' filter weights, as addSample() does;
     * a miss adds the clear value or, with background, the batch's colour. hitIds and color are laid
     * out as the shading kernel reads and writes them, weights as the ray generation writes them.
     * After beginDeviceFrame() only.
     **/
    /**
     * @brief Readers get aov of picks from here on, read back on the first Map() that asks for it. For the thread
     * drawing the frame, between passes.
     **/
    void publishPicks(const std::shared_ptr<UtilityPickSnapshot>& picks,
                      UtilityPickAov aov);

    gpu::GpuStatus recordDeviceBatch(gpu::GpuRecordContext& context,
                                     const gpu::GpuBuffer& hitIds,
                                     const gpu::GpuBuffer& color,
                                     const gpu::GpuBuffer& weights,
                                     int x,
                                     int y,
                                     int width,
                                     int height,
                                     bool background = false);
#endif

private:
    // Fills the host pixels with the device film's, under _deviceMutex.
    void downloadLocked();
    // Under _deviceMutex: the host pixels become what readers get.
    void publishHostLocked();
    // Under _deviceMutex: drops the readers' host copy, keeping its storage for the next one.
    void retireFrontLocked();
    // Under _deviceMutex: makes the readers' host copy from the published device pixels if it has none.
    void frontToHostLocked();
    // Under _deviceMutex: fills the host pixels and sample sums a deferred clear() left unfilled.
    void ensureHostLocked();
#ifdef NATRON_HAVE_GPU
    // Under _deviceMutex: *out becomes the published picks' AOV over the clear value.
    gpu::GpuStatus picksToHostLocked(std::vector<std::uint8_t>* out);
#endif

    virtual void _Deallocate() OVERRIDE;

    unsigned int _width = 0;
    unsigned int _height = 0;
    PXR_NS::HdFormat _format = PXR_NS::HdFormatInvalid;
    bool _multiSampled = false;
    std::vector<std::uint8_t> _pixels;
    std::atomic<int> _mappers { 0 };
    std::atomic<bool> _converged { false };

    // The last clear() value as one encoded pixel, empty when that clear() gave none.
    std::vector<std::uint8_t> _clearPixel;
    // The last clear() value's components, zero where it gave none.
    std::vector<float> _clearComponents;
    // Per pixel since the last clear(): the weighted sums of a filtered buffer's samples, and the sum
    // of their weights or, unfiltered, the heaviest weight; negative before the first sample.
    std::vector<float> _sampleSums;
    std::vector<float> _sampleWeights;
    bool _deviceRestart = true;
    // Counts clear(), release and deallocation, under _deviceMutex, for a frame starting outside the lock to see
    // that the film it filled is no longer the one asked for.
    std::uint64_t _deviceEpoch = 0;
    mutable std::mutex _deviceMutex;
#ifdef NATRON_HAVE_GPU
    std::shared_ptr<UtilityDeviceFilm> _film;
#endif
    // Set while the film holds the frame and the host pixels are stale.
    bool _onDevice = false;
    // What readers get, under _deviceMutex: a copy no frame writes to. _front is null while only
    // _frontFilm holds it. _mapped is what the Map() calls not yet unmapped share.
    std::shared_ptr<std::vector<std::uint8_t>> _front;
    std::shared_ptr<std::vector<std::uint8_t>> _spare;
    std::shared_ptr<std::vector<std::uint8_t>> _mapped;
#ifdef NATRON_HAVE_GPU
    std::shared_ptr<UtilityDeviceFilm> _frontFilm;
    // Published depth or ids, still on the device, when _front is null.
    std::shared_ptr<UtilityPickSnapshot> _frontPicks;
    UtilityPickAov _frontPickAov = UtilityPickAov::Depth;
#endif
    // Cleared by a deferred clear() until the host pixels and sample sums are filled; written under _deviceMutex.
    std::atomic<bool> _hostReady { true };
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_Utility_UtilityRenderBuffer_h
