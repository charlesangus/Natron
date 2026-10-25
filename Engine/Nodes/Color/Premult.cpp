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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Premult.h"

#include <cstdint>

#include "Engine/Image.h"
#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

template <bool DIVIDE>
typename PremultOp<DIVIDE>::Params
PremultOp<DIVIDE>::bake(const KnobReader& /*reader*/) const
{
    return Params();
}

template <bool DIVIDE>
bool
PremultOp<DIVIDE>::isIdentity(const Params& /*params*/) const
{
    // With no divisor picked the stage makes the node an identity; with one, it is not.
    return false;
}

template <bool DIVIDE>
void
PremultOp<DIVIDE>::row(const Params& /*params*/,
                       const RowIO& io) const
{
    const int nComps = io.nComps;
    bool process[4] = { false, false, false, false };
    for (int c = 0; (c < nComps) && (c < 4); ++c) {
        process[c] = io.channels[pixelKernelChannelBit(nComps, c)];
    }
    const float* src = io.src[0];
    float* dst = io.dst;
    for (int x = 0; x < io.width; ++x, src += nComps, dst += nComps) {
        const float d = io.divisor ? io.divisor[x] : 1.f;
        // A plain product, negative or NaN divisors included, as the OFX Premult does.
        const bool divides = DIVIDE && Image::unPremultDivisorIsUsable(d);
        for (int c = 0; (c < nComps) && (c < 4); ++c) {
            if (!process[c]) {
                continue;
            }
            if (!DIVIDE) {
                dst[c] = src[c] * d;
            } else if (divides) {
                dst[c] = Image::unPremultiplyValue(src[c], d);
            } else {
                dst[c] = src[c];
            }
        }
    }
}

template <bool DIVIDE>
GpuOpRef
PremultOp<DIVIDE>::gpu(const Params& /*params*/) const
{
#ifdef NATRON_HAVE_GPU
    // Neither kernel reads parameters, the divisor being the wrap's; the one word keeps the
    // parameter binding from being empty.
    const std::uint32_t unused = 0;
    return GpuOpRef::compiled((int)(DIVIDE ? gpu::GpuKernelId::unpremult_wrapped : gpu::GpuKernelId::premult_wrapped), unused);
#else
    return GpuOpRef::none();
#endif
}

template struct PremultOp<false>;
template struct PremultOp<true>;

NATRON_NAMESPACE_EXIT
