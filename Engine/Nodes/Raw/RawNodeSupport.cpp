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

#include "RawNodeSupport.h"

#include <algorithm>
#include <cstring>

#include <QString>

#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"

NATRON_NAMESPACE_ENTER

namespace RawNodeSupport {
void
TopDownBuffer::allocate(const RectI& r,
                        int comps)
{
    rect = r;
    nComps = comps;
    data.assign((std::size_t)std::max(0, r.width()) * (std::size_t)std::max(0, r.height()) * (std::size_t)comps, 0.f);
}

void
LockedSource::lock()
{
    if (!image) {
        return;
    }
    bounds = image->getBounds();
    nComps = (int)image->getComponentsCount();
    access = std::make_shared<Image::ReadAccess>(image.get());
}

void
readTopDown(const LockedSource& source,
            TopDownBuffer* buffer,
            const RawKernels::ParallelFor& parallel)
{
    const RectI& rect = buffer->rect;
    const int nComps = buffer->nComps;

    std::fill(buffer->data.begin(), buffer->data.end(), 0.f);
    if (!source.image || !source.access) {
        return;
    }
    const int xStart = std::max(rect.x1, source.bounds.x1);
    const int xEnd = std::min(rect.x2, source.bounds.x2);
    if (xStart >= xEnd) {
        return;
    }
    const int copyComps = std::min(nComps, source.nComps);
    parallel(rect.height(), [&](int r) {
        const int y = rect.y2 - 1 - r;
        if ((y < source.bounds.y1) || (y >= source.bounds.y2)) {
            return;
        }
        const float* src = (const float*)source.access->pixelAt(xStart, y);
        if (!src) {
            return;
        }
        float* dst = &buffer->data[buffer->offset(xStart, y)];
        if (copyComps == source.nComps && copyComps == nComps) {
            std::memcpy(dst, src, sizeof(float) * (std::size_t)(xEnd - xStart) * nComps);

            return;
        }
        for (int x = xStart; x < xEnd; ++x, src += source.nComps, dst += nComps) {
            for (int c = 0; c < copyComps; ++c) {
                dst[c] = src[c];
            }
        }
    });
}

void
writeTopDown(const TopDownBuffer& buffer,
             Image* dst,
             Image::WriteAccess* access,
             const RectI& window,
             float fill,
             const RawKernels::ParallelFor& parallel)
{
    const int dstComps = (int)dst->getComponentsCount();
    const int copyComps = std::min(dstComps, buffer.nComps);

    parallel(window.height(), [&](int r) {
        const int y = window.y1 + r;
        float* out = (float*)access->pixelAt(window.x1, y);
        if (!out) {
            return;
        }
        const float* in = &buffer.data[buffer.offset(window.x1, y)];
        for (int x = window.x1; x < window.x2; ++x, out += dstComps, in += buffer.nComps) {
            for (int c = 0; c < copyComps; ++c) {
                out[c] = in[c];
            }
            for (int c = copyComps; c < dstComps; ++c) {
                out[c] = fill;
            }
        }
    });
}

RawKernels::ParallelFor
makeParallelFor(RenderCancellation* cancel,
                int nThreads)
{
    return [cancel, nThreads](int count, const std::function<void(int)>& body) {
        parallelForCancellable(count, nThreads, *cancel, body);
    };
}

bool
getInputPixelRoD(EffectInstance& effect,
                 int inputNb,
                 double time,
                 ViewIdx view,
                 RectI* rod)
{
    EffectInstancePtr input = effect.getInput(inputNb);

    if (!input) {
        return false;
    }
    RectD canonical;
    bool isProjectFormat = false;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, RenderScale::identity, view, &canonical, &isProjectFormat) == eStatusFailed) {
        return false;
    }
    *rod = canonical.toPixelEnclosing(0u, input->getAspectRatio(-1));

    return !rod->isNull();
}

RectI
expandForKernel(const RectI& window,
                int halo,
                int period,
                int minSize,
                const RectI& rod)
{
    RectI r(window.x1 - halo, window.y1 - halo, window.x2 + halo, window.y2 + halo);

    if (period > 1) {
        const int left = (r.x1 - rod.x1) % period;
        r.x1 -= (left < 0) ? (left + period) : left;
        const int top = (rod.y2 - r.y2) % period;
        r.y2 += (top < 0) ? (top + period) : top;
    }
    if (r.width() < minSize) {
        r.x2 = r.x1 + minSize;
    }
    if (r.height() < minSize) {
        r.y1 = r.y2 - minSize;
    }
    RectI clipped = r.intersect(rod);
    if (clipped.isNull()) {
        return RectI();
    }
    // Clipping keeps the phase: rod's own left column and top row are on it.
    if (clipped.width() < std::min(minSize, rod.width())) {
        if (clipped.x1 == rod.x1) {
            clipped.x2 = std::min(rod.x2, clipped.x1 + minSize);
        } else {
            const int grow = std::min(minSize, rod.width()) - clipped.width();
            const int step = (period > 1) ? ((grow + period - 1) / period) * period : grow;
            clipped.x1 = std::max(rod.x1, clipped.x1 - step);
        }
    }
    if (clipped.height() < std::min(minSize, rod.height())) {
        if (clipped.y2 == rod.y2) {
            clipped.y1 = std::max(rod.y1, clipped.y2 - minSize);
        } else {
            const int grow = std::min(minSize, rod.height()) - clipped.height();
            const int step = (period > 1) ? ((grow + period - 1) / period) * period : grow;
            clipped.y2 = std::min(rod.y2, clipped.y2 + step);
        }
    }

    return clipped;
}

std::uint32_t
bayerFiltersAt(const RawLayout& layout,
               const RectI& rect)
{
    int colours[2][2];

    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 2; ++col) {
            const RawCfaColorEnum c = layout.colourAt(rect.x1 + col, rect.y2 - 1 - row);
            colours[row][col] = (c == eRawCfaColorNone) ? 1 : (int)c;
        }
    }

    return RawKernels::bayerFilters(colours);
}

void
xtransAt(const RawLayout& layout,
         const RectI& rect,
         std::uint8_t xtrans[6][6])
{
    for (int row = 0; row < 6; ++row) {
        for (int col = 0; col < 6; ++col) {
            const RawCfaColorEnum c = layout.colourAt(rect.x1 + col, rect.y2 - 1 - row);
            xtrans[row][col] = (std::uint8_t)((c == eRawCfaColorNone) ? 1 : (int)c);
        }
    }
}

void
OwnedMessage::post(const NodePtr& node,
                   MessageTypeEnum type,
                   const std::string& text)
{
    if (!node) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_posted == text) {
            return;
        }
        _posted = text;
    }
    node->setPersistentMessage(type, text);
}

void
OwnedMessage::withdraw(const NodePtr& node)
{
    std::string posted;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        posted.swap(_posted);
    }
    if (posted.empty() || !node) {
        return;
    }
    QString current;
    int type = 0;
    node->getPersistentMessage(&current, &type, false);
    if (current == QString::fromUtf8(posted.c_str())) {
        node->clearPersistentMessage(false);
    }
}
} // namespace RawNodeSupport

NATRON_NAMESPACE_EXIT
