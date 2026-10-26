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

#ifndef Engine_Nodes_Scene_HydraHandoff_h
#define Engine_Nodes_Scene_HydraHandoff_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#ifdef NATRON_HAVE_GPU

#include <memory>
#include <optional>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Gpu/GlInterop.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief How USDRender's Storm pixels reach the GPU runtime: the ladder of the viewer's hand-off,
 * run the other way. ZeroCopy packs a plane window, on the GL side, into Vulkan memory exported to
 * GL and orders the Vulkan copy after it with an exported semaphore; ZeroCopyHostSync does the same
 * with a glFinish() in place of the semaphore; Readback is USDRender's CPU render, whose image the
 * placement uploads. Every rung packs with the same shader, so every rung gives the same bits.
 *
 * The rung is picked for each runtime of the GPU context, by device pairing and extension support
 * as GlInterop::probe() reads them, stepping down past any rung that fails to build; a rung that
 * fails later steps down too, silently. While the runtime is lost or GPU compute is unavailable
 * the rung is Readback, and the ladder is built again for the runtime recoverIfLost() makes.
 * NATRON_USDRENDER_HANDOFF=zerocopy|hostsync|readback, or setForcedPathForTests(), makes the
 * ladder start at that rung.
 **/
class HydraHandoff {
public:
    // A hand-off for context, or for whatever GPU context the application has at each call when null.
    explicit HydraHandoff(GpuContext* context);
    ~HydraHandoff();

    // The hand-off of the application's GPU context, made on first use and never destroyed.
    static HydraHandoff& instance();

    // The rung renders take now, building the ladder for a runtime it has not seen.
    gpu::GlHandoffPath path();

    // Why the rung is below ZeroCopy, empty when it is not.
    std::string reason() const;

    /**
     * @brief Records into context the copy of window of request's render into output over
     * outputRect, whose size is the window's: the pack and its GL-side synchronisation happen now,
     * on the render thread; the copy waits for them on the device. False with *error set when the
     * rung is Readback or the hand-off failed, in which case the rung has stepped down if the
     * failure was the rung's.
     **/
    bool record(const HydraRender::Request& request,
                const HydraRender::PlaneWindow& window,
                gpu::GpuRecordContext& context,
                const GpuImageView& output,
                const RectI& outputRect,
                std::string* error) WARN_UNUSED_RETURN;

    // Makes every hand-off's ladder start at path, or follow NATRON_USDRENDER_HANDOFF again when empty.
    static void setForcedPathForTests(std::optional<gpu::GlHandoffPath> path);

    struct State;
    struct Implementation;

private:
    std::shared_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_HAVE_GPU

#endif // Engine_Nodes_Scene_HydraHandoff_h
