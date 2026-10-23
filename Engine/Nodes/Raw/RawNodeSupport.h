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

#ifndef Engine_Nodes_Raw_RawNodeSupport_h
#define Engine_Nodes_Raw_RawNodeSupport_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Image.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Raw/RawKernels.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

#define PLUGIN_GROUP_RAW "Raw"

NATRON_NAMESPACE_ENTER

/**
 * @brief What the raw develop nodes share: the move between Natron's y-up images and the
 * top-down buffers RawKernels works on, the source's region at render scale 1, and the
 * persistent message a node posts about its source.
 **/
namespace RawNodeSupport {
/**
 * @brief A region of an image held top-down: row r is pixel row rect.y2 - 1 - r, nComps floats
 * per pixel.
 **/
struct TopDownBuffer {
    RectI rect;
    int nComps = 0;
    std::vector<float> data;

    void allocate(const RectI& r,
                  int comps);

    int width() const
    {
        return rect.width();
    }

    int height() const
    {
        return rect.height();
    }

    std::size_t offset(int x,
                       int y) const
    {
        return ((std::size_t)(rect.y2 - 1 - y) * (std::size_t)rect.width() + (std::size_t)(x - rect.x1)) * (std::size_t)nComps;
    }

    // The buffer row holding the top of rect, and its first column.
    int topRow() const
    {
        return rect.y2 - 1;
    }
};

/**
 * @brief A float image with its bounds and read access, taken on the calling thread.
 **/
struct LockedSource {
    ImagePtr image;
    RectI bounds;
    int nComps = 0;
    std::shared_ptr<Image::ReadAccess> access;

    void lock();
};

/**
 * @brief Fills buffer from source: channel c of a pixel is channel c of the image, zero for a
 * channel or pixel the image does not have.
 **/
void readTopDown(const LockedSource& source,
                 TopDownBuffer* buffer,
                 const RawKernels::ParallelFor& parallel);

/**
 * @brief Writes window of dst from buffer, which covers it: channel c of each output pixel from
 * channel c of the buffer, and every channel past the buffer's set to fill.
 **/
void writeTopDown(const TopDownBuffer& buffer,
                  Image* dst,
                  Image::WriteAccess* access,
                  const RectI& window,
                  float fill,
                  const RawKernels::ParallelFor& parallel);

/**
 * @brief A ParallelFor spreading bodies over at most nThreads threads of the global pool, which
 * stops starting bodies once cancel is set.
 **/
RawKernels::ParallelFor makeParallelFor(RenderCancellation* cancel,
                                        int nThreads);

/**
 * @brief The region of definition of input inputNb at render scale 1, in pixels, false when it
 * is not connected or has none.
 **/
bool getInputPixelRoD(EffectInstance& effect,
                      int inputNb,
                      double time,
                      ViewIdx view,
                      RectI* rod);

/**
 * @brief window grown by halo on every side, then by up to period - 1 more on the left and the
 * top so that its left column and top row sit on the CFA period counted from the left column
 * and top row of rod, then grown to at least minSize in each direction where rod allows, and
 * clipped to rod. A kernel run over it then sees the pattern from the same phase as over the
 * whole of rod.
 **/
RectI expandForKernel(const RectI& window,
                      int halo,
                      int period,
                      int minSize,
                      const RectI& rod);

/**
 * @brief The dcraw `filters` word of layout seen from the top-left photosite of rect.
 **/
std::uint32_t bayerFiltersAt(const RawLayout& layout,
                             const RectI& rect);

/**
 * @brief The X-Trans colour array of layout seen from the top-left photosite of rect.
 **/
void xtransAt(const RawLayout& layout,
              const RectI& rect,
              std::uint8_t xtrans[6][6]);

/**
 * @brief The key a node caches a whole-frame analysis of its source under: its render hash, which
 * covers its knobs and everything upstream, combined with the time and the view.
 **/
U64 frameKey(U64 renderHash,
             double time,
             ViewIdx view);

/**
 * @brief One persistent message a node posts about its source and takes back itself, leaving any
 * other message on the node alone. Safe to use from render threads.
 **/
class OwnedMessage {
public:
    void post(const NodePtr& node,
              MessageTypeEnum type,
              const std::string& text);

    void withdraw(const NodePtr& node);

private:
    std::mutex _mutex;
    std::string _posted;
};
} // namespace RawNodeSupport

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Raw_RawNodeSupport_h
