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


#ifndef GPUCONTEXTPOOL_H
#define GPUCONTEXTPOOL_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <optional>
#include <string>

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

// If set, multiple frame renders may use the same GPU context instead of locking it for the whole render of a frame.
#define NATRON_RENDER_SHARED_CONTEXT

struct GPUContextPoolPrivate;
class GPUContextPool
{
public:

    GPUContextPool();

    ~GPUContextPool();

    // The number of contexts handed out when no test override is set.
    static constexpr int kMaxRenderGLContexts = 2;

    /**
     * @brief Records whether the GL suite may render, on which GL renderer, and the status line describing why.
     * Called once at startup, before any render. The reads below are thread-safe.
     **/
    void setRenderingState(bool enabled, const GLRendererID& rendererID, const std::string& status);

    bool isRenderingEnabled() const;

    GLRendererID getRendererID() const;

    std::string getRendererStatus() const;

    /**
     * @brief Overrides isRenderingEnabled() until reset with std::nullopt. For tests only.
     **/
    void setRenderingEnabledForTests(std::optional<bool> enabled);

    /**
     * @brief Overrides kMaxRenderGLContexts until reset with std::nullopt. Values below 1 count as 1. For tests only.
     **/
    void setMaxContextsForTests(std::optional<int> maxContexts);

    //////////////////////////////// OpenGL related /////////////////////////////////

    /**
     * @brief Attaches one of the OpenGL context in the pool to a specific frame render.
     * The context will not be available to other renders until releaseGLContextFromRender() is called
     * for on the same thread that called this function.
     * This function is blocking and the calling thread will wait until one context is made available
     * in the pool again.
     * If a context is already attached on the thread, this function returns immediately
     * After returning this function, the context must be made current before
     * OpenGL calls can be made.
     * If during the render, other threads are participating and needs to make the context current, they
     * may call makeContextCurrent with the context returned from this function.
     *
     * @param renderUniqueAbortInfo These are the infos that uniquely identify the render attached to the context
     **/
    OSGLContextPtr attachGLContextToRender(bool checkIfGLLoaded = true);

    /**
     * @brief Releases the given OpenGL context from a render. After this call the context may be re-used
     * by other threads waiting in attachContextToThread().
     * This function MUST be call when a render is finished and the argument passed should be the context returned
     * from the function attachGLContextToRender().
     **/
    void releaseGLContextFromRender(const OSGLContextPtr& context);

    /**
     * @brief Returns the thread-exclusive OpenGL context of the calling thread, creating it on first use, or null when
     * OpenGL rendering is unavailable or disabled, or when the maximum number of contexts already have one. These contexts are not part of the pool handed out by
     * attachGLContextToRender() and share nothing with any other context, so a texture made on one is only valid on it.
     * The caller must leave the context unbound when it is done rendering, so that the context is never current on a
     * thread other than the one it belongs to.
     **/
    OSGLContextPtr getOrCreateContextForCurrentThread();

    /**
     * @brief The context getOrCreateContextForCurrentThread() returned on the calling thread, or null if it never ran
     * there. Never creates one.
     **/
    OSGLContextPtr getContextForCurrentThread() const;

    /**
     * @brief Number of live contexts created by getOrCreateContextForCurrentThread() that were bound for a render at
     * least once.
     **/
    std::size_t getNumThreadContextsUsedForRender() const;

    /**
     * @brief Returns the max texture size, i.e: the value returned by glGetIntegerv(GL_MAX_TEXTURE_SIZE,&v)
     * This does not call glGetIntegerv and does not require a context to be bound.
     **/
    int getCurrentOpenGLRendererMaxTextureSize() const;

    ////////////////////////////////////////////////////////////////

    /**
     * @brief Clear all created contexts
     **/
    void clear();

private:


    std::unique_ptr<GPUContextPoolPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // GPUCONTEXTPOOL_H
