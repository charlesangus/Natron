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
#include <cstddef>
#include <cstring>
#include <memory>
#include <type_traits>
#include <vector>

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
 * @brief One row of pixels handed to a point op's row function: `width` pixels starting at
 * pixel (x0, y), every buffer holding `nComps` interleaved floats per pixel unless noted.
 *
 * src[i] is input i's row (nSrc of them), zero where the input has no pixel, and already divided
 * by the "(Un)premult by" channel wherever the divisor is applied. mask holds one value per
 * pixel, the mask channel before maskInvert, or is null when no mask applies. divisor holds one
 * value per pixel, the "(Un)premult by" channel, or is null when none is selected, and
 * divisorSkipChannel is the channel of the row that is that divisor itself (-1 when none). The
 * stage multiplies back and applies mask and mix after the row function returns, so a point op
 * normally ignores mask and divisor; they are here for a kernel whose maths depends on them.
 *
 * dst receives the result: the row function must write every channel whose bit is set in `channels`
 * and may leave the others untouched.
 *
 * alpha[i] is src[i]'s alpha, for a Composite op's step whose inputs have alpha sources (see
 * CompositeOp), with dstAlpha receiving the result's alpha in dst's layout; both are null
 * otherwise.
 **/
struct RowIO {
    /**
     * @brief The alpha of a row's channels: that of channel c of pixel i (from x0) is
     * data[i * pixelStride + c * channelStride]. A channelStride of 0 gives every channel of a
     * pixel the same alpha; data is null for a row with none.
     **/
    struct Alpha {
        const float* data = nullptr;
        int pixelStride = 0;
        int channelStride = 0;

        float at(int i,
                 int c) const
        {
            return data[i * pixelStride + c * channelStride];
        }
    };

    const float* src[kPixelKernelMaxInputs];
    Alpha alpha[kPixelKernelMaxInputs];
    int nSrc;
    const float* mask;
    const float* divisor;
    int divisorSkipChannel;
    float* dst;
    float* dstAlpha;
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
        , dstAlpha(0)
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
 * @brief The GPU description of a point operator: a kernel of the GPU registry, a
 * gpu::GpuKernelId held as an int so that this header builds without the GPU backend (-1 for
 * none), and the parameter block that kernel's op loads. The stage wraps it with mask, mix,
 * processed channels and "(Un)premult by" exactly as the CPU row function is wrapped.
 **/
struct PixelKernelGpu {
    int kernel = -1;
    std::vector<std::byte> params;

    template <typename P>
    void setParams(const P& p)
    {
        static_assert(std::is_trivially_copyable<P>::value, "GPU parameters are copied byte for byte");
        params.resize(sizeof(P));
        std::memcpy(params.data(), &p, sizeof(P));
    }
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Image_PixelKernel_h
