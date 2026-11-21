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

#ifndef NATRON_GUI_SELECTIONENGINE_H
#define NATRON_GUI_SELECTIONENGINE_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>

#include "Gui/SceneSelectionTasks.h"

// See Engine/Nodes/Scene/UsdIncludes.h: oneTBB, which these include, declares members named emit().
#ifdef emit
#define NATRON_SELECTIONENGINE_RESTORE_EMIT
#undef emit
#endif
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hgi/texture.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>
#ifdef NATRON_SELECTIONENGINE_RESTORE_EMIT
#undef NATRON_SELECTIONENGINE_RESTORE_EMIT
#define emit
#endif

NATRON_NAMESPACE_ENTER

/**
 * @brief A UsdImagingGLEngine that also draws the selected prims alone, for the scene view to
 * tint them after its own pass. Under Storm, the engine's render index holds the
 * SceneSelectionTasks, so the pass draws the engine's own rprims, with their materials and their
 * depths, into a colour and a depth buffer of its own. Under any other renderer there is no pass.
 *
 * Needs what the engine needs: the GL context current, inside the caller's StormGLSection. It is
 * held by its own type, because the base's destructor is not virtual.
 **/
class SelectionEngine final
    : public PXR_NS::UsdImagingGLEngine {
public:
    explicit SelectionEngine(const Parameters& parameters);

    ~SelectionEngine();

    /**
     * @brief Whether the renderer got the selection tasks: true under Storm, until
     * SetRendererPlugin() replaces the renderer the tasks were merged into.
     **/
    bool hasSelectionPass() const;

    // The prims renderSelection() draws: those at and under these stage paths.
    void setSelection(const PXR_NS::SdfPathVector& selection);

    /**
     * @brief Draws the selected prims into the pass's buffers, after a Render() with the same
     * params: the camera, framing, lighting and draw mode are those the engine last rendered with,
     * and wireframe draws lines. False, with the buffers left as they were, when nothing selected
     * has anything to draw or there is no pass.
     **/
    bool renderSelection(const PXR_NS::UsdImagingGLRenderParams& params);

    // The selected surfaces' colour; its alpha is how much of each pixel they cover.
    PXR_NS::HgiTextureHandle colorTexture() const;

    // Cleared to 1 where nothing selected was drawn.
    PXR_NS::HgiTextureHandle depthTexture() const;

private:
    SelectionEngine(SceneSelectionTasks::Arming&& arming,
                    const Parameters& parameters);

    PXR_NS::HgiTextureHandle bufferTexture(const PXR_NS::SdfPath& path) const;

    std::shared_ptr<SceneSelectionTasks> _tasks;
    // The task controller the tasks were merged beside.
    PXR_NS::HdSceneIndexBasePtr _armedController;
    // The selection under the engine's scene delegate id, sorted.
    PXR_NS::SdfPathVector _roots;
    bool _selectionChanged = false;
    // The render collection the tasks' collection was last made from.
    PXR_NS::HdRprimCollection _selectedFrom;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_SELECTIONENGINE_H
