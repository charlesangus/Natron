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

#ifndef Engine_Nodes_Loop_LoopFrameBake_h
#define Engine_Nodes_Loop_LoopFrameBake_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Global/GlobalDefines.h"

#define kLoopFrameBakeHashAttribute "natron:loopHash"
#define kLoopFrameBakeRegionAttribute "natron:loopRoD"

NATRON_NAMESPACE_ENTER

/**
 * @brief A Cross Frame loop's frames on disk: one float EXR per frame, whose data window is the
 * frame's pixel bounds, beyond the format where the frame reaches past it, and which carries the
 * simulation's hash and the exact region of definition.
 **/
namespace LoopFrameBake {
struct FrameInfo {
    // The hash the frame was baked under; hasHash is false when the file carries none.
    U64 hash = 0;
    bool hasHash = false;
    RectD regionOfDefinition;
    RectI bounds;
    double pixelAspect = 1.;
    int channels = 0;
};

std::string hashText(U64 hash) WARN_UNUSED_RETURN;

/**
 * @brief Writes image, a float image at mipmap level 0, to path as a float EXR: its bounds as the
 * data window, format as the display window, hash and its region of definition as attributes.
 * The file is replaced only once complete. False with error on failure or when cancelled says so.
 **/
bool writeFrame(const std::string& path,
                const Image& image,
                const RectI& format,
                U64 hash,
                const std::function<bool()>& cancelled,
                std::string* error) WARN_UNUSED_RETURN;

/**
 * @brief What the frame baked at path holds; false when there is none or it is unreadable.
 **/
bool readFrameInfo(const std::string& path,
                   FrameInfo* info,
                   std::string* error) WARN_UNUSED_RETURN;

/**
 * @brief The frame baked at path as a float image of layer at mipmap level 0, its channels
 * matched by name; null when the file is missing, unreadable or lacks one of them.
 **/
ImagePtr readFrame(const std::string& path,
                   const ImageLayerDesc& layer,
                   FrameInfo* info,
                   std::string* error) WARN_UNUSED_RETURN;
} // namespace LoopFrameBake

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Loop_LoopFrameBake_h
