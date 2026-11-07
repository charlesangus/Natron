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

#include <pxr/base/gf/matrix4d.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/renderPass.h>

#include "Global/Macros.h"

#include "Engine/Nodes/Scene/Utility/Wavefront.h"

NATRON_NAMESPACE_ENTER

class UtilityRenderParam;

/**
 * @brief Clears the bound AOVs and has the delegate's executor draw the scene into them. A frame already
 * in the buffers is not drawn again. A stopped render leaves its buffers unconverged, and the next
 * render after a restart draws the frame in full.
 **/
class UtilityRenderPass : public PXR_NS::HdRenderPass {
public:
    UtilityRenderPass(PXR_NS::HdRenderIndex* index,
                      const PXR_NS::HdRprimCollection& collection,
                      UtilityRenderParam* renderParam);
    virtual ~UtilityRenderPass();

    virtual bool IsConverged() const OVERRIDE;

private:
    virtual void _Execute(const PXR_NS::HdRenderPassStateSharedPtr& renderPassState,
                          const PXR_NS::TfTokenVector& renderTags) OVERRIDE;

    UtilityRenderParam* _renderParam;
    std::atomic<bool> _converged { false };
    bool _hasDrawn = false;
    std::uint64_t _drawnVersion = 0;
    PXR_NS::GfMatrix4d _drawnWorldToView;
    PXR_NS::GfMatrix4d _drawnProjection;
    WavefrontRect _drawnWindow;
    PXR_NS::HdRenderPassAovBindingVector _drawnBindings;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_Utility_UtilityRenderPass_h
