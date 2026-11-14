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

#include <atomic>
#include <cstddef>
#include <cstdint>
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
#endif // NATRON_HAVE_GPU

/**
 * @brief An AOV image built from samples, in host memory or, for the colour of a frame rendered on
 * the GPU, in device memory until Map() asks for it. Its pixels are resolved as samples land, so a
 * frame part way through its samples is a picture.
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
    virtual void* Map() OVERRIDE;
    virtual void Unmap() OVERRIDE;
    virtual bool IsMapped() const OVERRIDE;
    virtual void Resolve() OVERRIDE;
    virtual bool IsConverged() const OVERRIDE;

    // A UtilityDeviceResource while the device holds the pixels, else empty.
    virtual PXR_NS::VtValue GetResource(bool multiSampled) const OVERRIDE;

    /**
     * @brief Fills every pixel with value: a scalar or a vector of float, double or int. Components
     * the value does not have are written as zero. An empty value leaves the buffer as it is.
     **/
    void clear(const PXR_NS::VtValue& value);
    void setConverged(bool converged);

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
     * records into context the clear to its value; a later one has the samples add to those the film
     * holds from the frame's earlier calls, and fails if the film is gone. The host pixels are stale
     * from here until Map() or clear().
     **/
    gpu::GpuStatus beginDeviceFrame(gpu::GpuRecordContext& context,
                                    gpu::GpuKernelRegistry& registry,
                                    gpu::GpuBufferPool& pool,
                                    const std::shared_ptr<const UtilityFilmKernels>& kernels,
                                    const std::shared_ptr<void>& runtimeHold);

    /**
     * @brief Records into context the adding of a batch's colours, for the rays of the width x
     * height pixels from (x, y), to their pixels with the rays' filter weights, as addSample() does.
     * hitIds and color are laid out as the shading kernel reads and writes them, weights as the ray
     * generation writes them. After beginDeviceFrame() only.
     **/
    gpu::GpuStatus recordDeviceBatch(gpu::GpuRecordContext& context,
                                     const gpu::GpuBuffer& hitIds,
                                     const gpu::GpuBuffer& color,
                                     const gpu::GpuBuffer& weights,
                                     int x,
                                     int y,
                                     int width,
                                     int height);
#endif

private:
    // Fills the host pixels with the device film's, under _deviceMutex.
    void downloadLocked();

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
    mutable std::mutex _deviceMutex;
#ifdef NATRON_HAVE_GPU
    std::shared_ptr<UtilityDeviceFilm> _film;
#endif
    // Set while the film holds the frame and the host pixels are stale.
    bool _onDevice = false;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_Utility_UtilityRenderBuffer_h
