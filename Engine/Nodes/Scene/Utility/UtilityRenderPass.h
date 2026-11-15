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

#ifndef Engine_Nodes_Scene_Utility_UtilityRenderPass_h
#define Engine_Nodes_Scene_Utility_UtilityRenderPass_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/renderPass.h>

#include "Global/Macros.h"

#include "Engine/Nodes/Scene/Utility/Wavefront.h"

NATRON_NAMESPACE_ENTER

class UtilityRenderParam;

/**
 * @brief Clears the bound AOVs and has the delegate's executor for the device setting draw the scene
 * into them, on the delegate's frame thread, returning at once. Auto draws on the GPU when it can trace
 * rays, and on the CPU when it cannot or when the GPU runs out of memory or is lost mid-frame. A frame is drawn one sample per pixel at a time,
 * each landing whole in the buffers, and only a frame that drew all its samples converges. A frame
 * already in the buffers is not drawn again. A change of what to draw cancels the running frame, and
 * a later Execute() starts the new one when the old has returned.
 **/
class UtilityRenderPass : public PXR_NS::HdRenderPass {
public:
    UtilityRenderPass(PXR_NS::HdRenderIndex* index,
                      const PXR_NS::HdRprimCollection& collection,
                      UtilityRenderParam* renderParam);
    virtual ~UtilityRenderPass();

    virtual bool IsConverged() const OVERRIDE;

private:
    enum class Device {
        Auto,
        Gpu,
        Cpu,
    };

    struct FrameKey {
        Device device = Device::Auto;
        std::uint64_t version = 0;
        PXR_NS::GfMatrix4d worldToView;
        PXR_NS::GfMatrix4d projection;
        WavefrontRect window;
        WavefrontSampling sampling;
        WavefrontMotion motion;
        PXR_NS::HdRenderPassAovBindingVector bindings;

        bool operator==(const FrameKey& other) const;
    };

    virtual void _Execute(const PXR_NS::HdRenderPassStateSharedPtr& renderPassState,
                          const PXR_NS::TfTokenVector& renderTags) OVERRIDE;

    UtilityRenderParam* _renderParam;
    std::atomic<bool> _converged { false };
    // What the last frame started draws.
    bool _hasFrame = false;
    FrameKey _frameKey;
    // Guards the two below, and makes a frame's landing and Execute()'s cancelling it exclusive: a
    // frame cancelled as it ends must not report itself finished after the cancel.
    std::mutex _landingMutex;
    bool _frameFinished = false;
    std::string _frameFailure;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_Utility_UtilityRenderPass_h
