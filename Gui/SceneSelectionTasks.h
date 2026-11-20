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

#ifndef NATRON_GUI_SCENESELECTIONTASKS_H
#define NATRON_GUI_SCENESELECTIONTASKS_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>

// See Engine/Nodes/Scene/UsdIncludes.h: oneTBB, which these include, declares members named emit().
#ifdef emit
#define NATRON_SCENESELECTIONTASKS_RESTORE_EMIT
#undef emit
#endif
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/imaging/hdx/renderSetupTask.h>
#include <pxr/usd/sdf/path.h>
#ifdef NATRON_SCENESELECTIONTASKS_RESTORE_EMIT
#undef NATRON_SCENESELECTIONTASKS_RESTORE_EMIT
#define emit
#endif

NATRON_NAMESPACE_ENTER

/**
 * @brief The Hydra prims of the scene view's selection pass, free of GL: one HdxRenderTask per
 * Storm material tag (default, masked, additive, translucent), and the colour and depth render
 * buffers they draw into. The tasks draw the selected prims with their own materials, blending
 * off, depth writes on and a LESS test, so a translucent surface leaves its own alpha and depth
 * where an opaque one would. Only the first task clears the buffers.
 *
 * A scene-index plugin for Storm merges these prims into the render index of the engine being
 * constructed while an Arming is alive on the thread, so the tasks draw that engine's own rprims.
 * Every other Storm engine, USDRender's among them, gets its input scene index back.
 *
 * Each setter compares what it is given with what the tasks hold and dirties only its own data
 * source locator, on the tasks that changed.
 **/
class SceneSelectionTasks {
public:
    /**
     * @brief Arms the plugin on the calling thread for its lifetime. Construct the engine while one
     * is alive, then take() the tasks the plugin made for it. Armings nest.
     **/
    class Arming {
    public:
        Arming();

        ~Arming();

        // The tasks merged into the engine built while armed, null if no Storm engine was.
        std::shared_ptr<SceneSelectionTasks> take();

    private:
        friend class SceneSelectionTasks;

        Arming(const Arming&) = delete;
        Arming& operator=(const Arming&) = delete;

        Arming* _previous;
        std::shared_ptr<SceneSelectionTasks> _tasks;
    };

    // Registers the plugin for Storm, once per process.
    static void registerSceneIndex();

    /**
     * @brief What the plugin does with the scene index it is given: input itself when the thread is
     * not armed, otherwise input merged with the prims of new tasks, which the Arming then holds.
     **/
    static PXR_NS::HdSceneIndexBaseRefPtr append(const PXR_NS::HdSceneIndexBaseRefPtr& input);

    // The tasks and buffers under prefix, an absolute path no scene prim lives under.
    explicit SceneSelectionTasks(const PXR_NS::SdfPath& prefix);

    ~SceneSelectionTasks();

    // The retained scene index holding the prims, to merge into the engine's scene.
    PXR_NS::HdSceneIndexBaseRefPtr getSceneIndex() const;

    // The tasks in the order they run.
    const PXR_NS::SdfPathVector& getTaskPaths() const;

    const PXR_NS::SdfPath& getColorBufferPath() const;

    const PXR_NS::SdfPath& getDepthBufferPath() const;

    /**
     * @brief The rprims drawn: the engine's render collection, which carries the repr of its draw
     * mode, rooted at the selected paths. Each task keeps its own material tag.
     **/
    void setSelection(const PXR_NS::HdRprimCollection& collection);

    void setRenderTags(const PXR_NS::TfTokenVector& renderTags);

    /**
     * @brief The engine's own render task parameters: camera, framing, lighting, clipping, cull
     * style and colours. Blending, depth, multisampling and the AOV bindings are the pass's own.
     **/
    void setRenderParams(const PXR_NS::HdxRenderTaskParams& params);

    // The buffers' size and formats, those of the engine's colour and depth buffers. The first call
    // adds the buffers.
    void setSize(const PXR_NS::GfVec3i& dimensions,
                 PXR_NS::HdFormat colorFormat,
                 PXR_NS::HdFormat depthFormat);

private:
    SceneSelectionTasks(const SceneSelectionTasks&) = delete;
    SceneSelectionTasks& operator=(const SceneSelectionTasks&) = delete;

    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_SCENESELECTIONTASKS_H
