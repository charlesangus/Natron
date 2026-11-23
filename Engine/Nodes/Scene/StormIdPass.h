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

#ifndef NATRON_ENGINE_NODES_SCENE_STORMIDPASS_H
#define NATRON_ENGINE_NODES_SCENE_STORMIDPASS_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <cstdint>
#include <memory>

// See Engine/Nodes/Scene/UsdIncludes.h: oneTBB, which these include, declares members named emit().
#ifdef emit
#define NATRON_STORMIDPASS_RESTORE_EMIT
#undef emit
#endif
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/imaging/hdx/renderSetupTask.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>
#ifdef NATRON_STORMIDPASS_RESTORE_EMIT
#undef NATRON_STORMIDPASS_RESTORE_EMIT
#define emit
#endif

NATRON_NAMESPACE_ENTER

/**
 * @brief The Hydra prims of the translucent id pass, free of GL. Storm draws translucent materials
 * through its OIT tasks, which write colour alone, so they leave no primId. The pass draws the
 * opaque and masked prims again into a depth buffer of its own, which then holds the engine's
 * opaque depth without the engine's depth AOV being touched, and then the translucent prims with
 * blending off, depth writes on and the engine's depth test, into that depth and the engine's
 * primId and instanceId AOVs: the nearest translucent fragment in front of the opaque surface
 * leaves its ids. When no translucent prim has anything to draw, a probe task finds that before
 * the depth buffer is made, the buffer is released, none of the drawing tasks runs, and the
 * engine's AOVs keep Storm's ids.
 **/
class StormIdTasks {
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
        std::shared_ptr<StormIdTasks> take();

    private:
        friend class StormIdTasks;

        Arming(const Arming&) = delete;
        Arming& operator=(const Arming&) = delete;

        Arming* _previous;
        std::shared_ptr<StormIdTasks> _tasks;
    };

    static void registerSceneIndex();

    /**
     * @brief What the plugin does with the scene index it is given: input itself when the thread is
     * not armed, otherwise input merged with the prims of new tasks, which the Arming then holds.
     **/
    static PXR_NS::HdSceneIndexBaseRefPtr append(const PXR_NS::HdSceneIndexBaseRefPtr& input);

    // The tasks and the depth buffer under prefix, an absolute path no scene prim lives under.
    explicit StormIdTasks(const PXR_NS::SdfPath& prefix);

    ~StormIdTasks();

    PXR_NS::HdSceneIndexBaseRefPtr getSceneIndex() const;

    const PXR_NS::SdfPathVector& getTaskPaths() const;

    // Executed alone, it only finds whether any translucent prim has draw items.
    const PXR_NS::SdfPath& getProbeTaskPath() const;

    const PXR_NS::SdfPath& getDepthBufferPath() const;

    // The engine's render collection, which carries the repr of its draw mode and its root paths.
    void setCollection(const PXR_NS::HdRprimCollection& collection);

    void setRenderTags(const PXR_NS::TfTokenVector& renderTags);

    /**
     * @brief The engine's own render task parameters, and its primId and instanceId buffers, which
     * the id task draws into. Blending, depth writes and the AOV bindings are the pass's own.
     **/
    void setRenderParams(const PXR_NS::HdxRenderTaskParams& params,
                         const PXR_NS::SdfPath& primIdBuffer,
                         const PXR_NS::SdfPath& instanceIdBuffer);

    /**
     * @brief The depth buffer's size, format and multisampling, those of the engine's AOVs, which
     * the id task's attachments must all share. A call while there is none adds the buffer.
     **/
    void setSize(const PXR_NS::GfVec3i& dimensions,
                 PXR_NS::HdFormat depthFormat,
                 bool multiSampled,
                 std::uint32_t sampleCount);

    void releaseDepthBuffer();

    // An estimate, as Storm's own count may leave render buffers out.
    std::size_t getDepthBufferBytes() const;

    static std::uint64_t getDepthBufferAddCount();

    void beginPass();

    bool hasDrawn() const;

private:
    StormIdTasks(const StormIdTasks&) = delete;
    StormIdTasks& operator=(const StormIdTasks&) = delete;

    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

/**
 * @brief A UsdImagingGLEngine whose renderer, under Storm, holds the StormIdTasks, so that after a
 * Render() the translucent prims' ids can be written into its primId and instanceId AOVs. Under
 * any other renderer there is no pass.
 *
 * Needs what the engine needs: the GL context current. It is deleted as its own type, because the
 * base's destructor is not virtual.
 **/
class StormIdEngine final
    : public PXR_NS::UsdImagingGLEngine {
public:
    explicit StormIdEngine(const Parameters& parameters);

    ~StormIdEngine();

    /**
     * @brief Whether the renderer got the id tasks: true under Storm, until SetRendererPlugin()
     * replaces the renderer the tasks were merged into.
     **/
    bool hasIdPass() const;

    /**
     * @brief Writes the translucent prims' ids into the primId and instanceId AOVs, after a
     * Render() with the same params and with no change to the stage since. False, with the AOVs
     * left as Storm wrote them, when no translucent prim has anything to draw or there is no pass.
     **/
    bool renderTranslucentIds(const PXR_NS::UsdImagingGLRenderParams& params);

    std::size_t getIdPassBytes() const;

private:
    StormIdEngine(StormIdTasks::Arming&& arming,
                  const Parameters& parameters);

    std::shared_ptr<StormIdTasks> _tasks;
    PXR_NS::HdSceneIndexBasePtr _armedController;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_NODES_SCENE_STORMIDPASS_H
