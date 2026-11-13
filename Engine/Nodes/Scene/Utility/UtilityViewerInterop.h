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

#ifndef Engine_Nodes_Scene_Utility_UtilityViewerInterop_h
#define Engine_Nodes_Scene_Utility_UtilityViewerInterop_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#ifdef NATRON_HAVE_GPU

#include <functional>
#include <memory>
#include <string>

NATRON_NAMESPACE_ENTER

class UtilityDeviceResource;

/**
 * @brief Brings the Natron Utility renderer's GPU film into a GL texture through the Vulkan to GL
 * hand-off, without the film passing through a mapped host buffer the viewer uploads. Lives on
 * the GL thread of one context: every call, the destructor and release() included, needs that
 * context current. The hand-off path is the best one the context and device build, from the one
 * NATRON_VIEWER_HANDOFF names when it names one; "cpu" there refuses every upload.
 **/
class UtilityViewerInterop {
public:
    // Resolves a GL entry point of the context, as QOpenGLContext::getProcAddress() does.
    typedef std::function<void*(const char*)> ProcLoader;

    explicit UtilityViewerInterop(const ProcLoader& loader);
    ~UtilityViewerInterop();

    UtilityViewerInterop(const UtilityViewerInterop&) = delete;
    UtilityViewerInterop& operator=(const UtilityViewerInterop&) = delete;

    /**
     * @brief Copies film's pixels into level 0 of texture, a GL_TEXTURE_2D of the film's size
     * whose internal format takes RGBA floats, ordered after the frame's writes on the device.
     * The film is kept until the device is done reading it. False with *error set when the
     * hand-off cannot be built or fails; the texture is then left as it was.
     **/
    bool upload(const std::shared_ptr<const UtilityDeviceResource>& film,
                unsigned int texture,
                std::string* error);

    // "zerocopy", "hostsync" or "readback": the path of the last upload, empty before one.
    std::string path() const;

    /**
     * @brief Whether the last failed upload failed because no hand-off path builds between this GL context and the
     * device, or NATRON_VIEWER_HANDOFF refuses them, so that asking again on this context is pointless. Other
     * failures, as a lost device or a failed copy, may pass on a later frame.
     **/
    bool unsupported() const;

    // Frees the hand-off and the films it keeps; the next upload builds it again.
    void release();

    // release() once the device is lost, so that the hand-off keeps no hold on the runtime that recovery waits for.
    void releaseIfLost();

private:
    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_HAVE_GPU

#endif // Engine_Nodes_Scene_Utility_UtilityViewerInterop_h
