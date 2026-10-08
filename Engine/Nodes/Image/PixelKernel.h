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

#ifndef Engine_Nodes_Image_PixelKernel_h
#define Engine_Nodes_Image_PixelKernel_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <bitset>
#include <memory>

#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#define kPixelKernelMaxInputs 4

NATRON_NAMESPACE_ENTER

/**
 * @brief The bit of std::bitset<4> standing for channel c of an nComps-channel pixel: R=0, G=1,
 * B=2, A=3, a one-channel pixel being an alpha pixel whose only channel is bit 3 (the convention
 * of Image::copyUnProcessedChannels() and ResolvedLayer::channelBit()).
 **/
inline int
pixelKernelChannelBit(int nComps,
                      int c)
{
    return (nComps == 1) ? 3 : c;
}

/**
 * @brief What a PixelKernel is built for: the render's time, view, mapped render scale (spatial
 * parameters scale by it) and the channels it processes, in the bit space of
 * pixelKernelChannelBit().
 **/
struct KernelContext {
    double time;
    ViewIdx view;
    RenderScale mappedScale;
    std::bitset<4> processChannels;

    KernelContext()
        : time(0.)
        , view(0)
        , mappedScale()
        , processChannels()
    {
    }
};

/**
 * @brief One row of pixels handed to PixelKernel::processRow(): `width` pixels starting at
 * pixel (x0, y), every buffer holding `nComps` interleaved floats per pixel unless noted.
 *
 * src[i] is input i's row (nSrc of them), zero where the input has no pixel, and already divided
 * by the "(Un)premult by" channel wherever NativeImageEffect divides. mask holds one value per
 * pixel, the mask channel before maskInvert, or is null when no mask applies. divisor holds one
 * value per pixel, the "(Un)premult by" channel, or is null when none is selected, and
 * divisorSkipChannel is the channel of the row that is that divisor itself (-1 when none). The
 * base multiplies back and applies mask and mix after processRow() returns, so a point op
 * normally ignores mask and divisor; they are here for a kernel whose maths depends on them.
 *
 * dst receives the result: processRow() must write every channel whose bit is set in `channels`
 * and may leave the others untouched.
 **/
struct RowIO {
    const float* src[kPixelKernelMaxInputs];
    int nSrc;
    const float* mask;
    const float* divisor;
    int divisorSkipChannel;
    float* dst;
    int x0;
    int y;
    int width;
    int nComps;
    std::bitset<4> channels;

    RowIO()
        : nSrc(0)
        , mask(0)
        , divisor(0)
        , divisorSkipChannel(-1)
        , dst(0)
        , x0(0)
        , y(0)
        , width(0)
        , nComps(0)
        , channels()
    {
        for (int i = 0; i < kPixelKernelMaxInputs; ++i) {
            src[i] = 0;
        }
    }
};

/**
 * @brief The per-pixel maths of a point operator, built once per render call from knob values
 * at a KernelContext and immutable afterwards. processRow() is pure on its RowIO: no knob access,
 * no allocation and no I/O, so rows can be run in any order, on any thread, or chained with
 * other kernels over a strip.
 **/
class PixelKernel {
public:
    virtual ~PixelKernel()
    {
    }

    virtual void processRow(const RowIO& io) const = 0;
};

typedef std::shared_ptr<const PixelKernel> PixelKernelPtr;

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Image_PixelKernel_h
