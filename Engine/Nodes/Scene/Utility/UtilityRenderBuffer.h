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
#include <vector>

#include <pxr/imaging/hd/renderBuffer.h>

#include "Global/Macros.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief An AOV image in host memory, holding one sample per pixel.
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

private:
    virtual void _Deallocate() OVERRIDE;

    unsigned int _width = 0;
    unsigned int _height = 0;
    PXR_NS::HdFormat _format = PXR_NS::HdFormatInvalid;
    bool _multiSampled = false;
    std::vector<std::uint8_t> _pixels;
    std::atomic<int> _mappers { 0 };
    std::atomic<bool> _converged { false };
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_Utility_UtilityRenderBuffer_h
