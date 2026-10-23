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

/*
 * The maths are a port of darktable's hotpixels module, DARKTABLE_REF release-5.6.2 (commit
 * 20891f6c6fa7), src/iop/hotpixels.c, C path only, Copyright (C) 2011-2026 darktable
 * developers, distributed under the GNU GPL version 3 or later.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "RawHotPixels.h"

#include <algorithm>
#include <cstddef>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/KnobReader.h"

NATRON_NAMESPACE_ENTER

namespace {
// darktable's search order for the nearest photosites of the same colour on an X-Trans sensor,
// as (column, row) offsets with rows counted downwards.
const int kXTransSearch[20][2] = {
    { -1, 0 },
    { 1, 0 },
    { 0, -1 },
    { 0, 1 },
    { -1, -1 },
    { -1, 1 },
    { 1, -1 },
    { 1, 1 },
    { -2, 0 },
    { 2, 0 },
    { 0, -2 },
    { 0, 2 },
    { -2, -1 },
    { -2, 1 },
    { 2, -1 },
    { 2, 1 },
    { -1, -2 },
    { 1, -2 },
    { -1, 2 },
    { 1, 2 },
};

inline int
wrapSix(int v)
{
    const int m = v % 6;

    return (m < 0) ? (m + 6) : m;
}

inline int
xtransCell(const RawLayout& layout,
           int x,
           int y)
{
    return wrapSix(y - layout.originY()) * 6 + wrapSix(x - layout.originX());
}

RawHotPixelsOp::XTransNeighbours
xtransNeighbours(const RawLayout& layout)
{
    RawHotPixelsOp::XTransNeighbours n;

    for (int row = 0; row < 6; ++row) {
        for (int col = 0; col < 6; ++col) {
            const int x = layout.originX() + col;
            const int y = layout.originY() + row;
            const int cell = xtransCell(layout, x, y);
            const RawCfaColorEnum c = layout.colourAt(x, y);
            int found = 0;
            for (int s = 0; (s < 20) && (found < 4); ++s) {
                const int dx = kXTransSearch[s][0];
                const int dy = -kXTransSearch[s][1];
                if (layout.colourAt(x + dx, y + dy) == c) {
                    n.offsets[cell][found] = { dx, dy };
                    ++found;
                }
            }
            n.count[cell] = found;
        }
    }

    return n;
}

// One row of one plane: slot 0 copied to slot 1, then each hot photosite in slot 1 replaced.
std::function<PassRowFn(const PassScratch&)>
hotPixelsWorker(const RawHotPixelsOp::Params& params)
{
    return [params](const PassScratch& scratch) -> PassRowFn {
        const int width = scratch.width;
        const int height = scratch.height;
        const RectI rect = scratch.rect;
        const std::size_t planeSize = scratch.planeSize;
        const float* const source = scratch.buffers[0];
        float* const result = scratch.buffers[1];
        const int period = params.layout.period();
        const int border = (period > 0) ? 2 : 1;

        return [params, width, height, rect, planeSize, source, result, period, border](int line) {
            const int p = line / height;
            const int row = line % height;
            const float* in = source + (std::size_t)p * planeSize;
            float* out = result + (std::size_t)p * planeSize;
            const std::size_t base = (std::size_t)row * (std::size_t)width;

            std::copy(in + base, in + base + width, out + base);
            if ((row < border) || (row >= height - border)) {
                return;
            }
            const std::size_t stride = (std::size_t)width;
            const int y = rect.y1 + row;
            for (int i = border; i < width - border; ++i) {
                const std::size_t index = base + (std::size_t)i;
                const float value = in[index];
                if (!(value > params.threshold)) {
                    continue;
                }
                const float mid = value * params.multiplier;
                int count = 0;
                float maxin = 0.0f;
                const auto test = [&](float other) {
                    if (mid > other) {
                        ++count;
                        if (other > maxin) {
                            maxin = other;
                        }
                    }
                };
                if (period == 2) {
                    test(in[index - 2]);
                    test(in[index - 2 * stride]);
                    test(in[index + 2]);
                    test(in[index + 2 * stride]);
                } else if (period == 6) {
                    const int cell = xtransCell(params.layout, rect.x1 + i, y);
                    for (int n = 0; n < params.xtrans.count[cell]; ++n) {
                        const int dx = params.xtrans.offsets[cell][n][0];
                        const int dy = params.xtrans.offsets[cell][n][1];
                        test(in[(std::size_t)(row + dy) * stride + (std::size_t)(i + dx)]);
                    }
                } else {
                    test(in[index - 1]);
                    test(in[index - stride]);
                    test(in[index + 1]);
                    test(in[index + stride]);
                }
                if (count >= params.minNeighbours) {
                    out[index] = maxin;
                }
            }
        };
    };
} // hotPixelsWorker
} // namespace

void
RawHotPixelsOp::createKnobs(KnobHolder* holder,
                            const KnobPagePtr& page)
{
    KnobDoublePtr thresholdKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Threshold").toStdString());
    thresholdKnob->setName(kRawHotPixelsParamThreshold);
    thresholdKnob->setHintToolTip(EffectInstance::tr("Only photosites above this value can be hot, 1 being the white level.").toStdString());
    thresholdKnob->setDefaultValue(0.05);
    thresholdKnob->setMinimum(0.);
    thresholdKnob->setMaximum(1.);
    thresholdKnob->setDisplayMinimum(0.);
    thresholdKnob->setDisplayMaximum(1.);
    page->addKnob(thresholdKnob);
    threshold = thresholdKnob;

    KnobDoublePtr strengthKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Strength").toStdString());
    strengthKnob->setName(kRawHotPixelsParamStrength);
    strengthKnob->setHintToolTip(EffectInstance::tr("How readily a photosite counts as hot: it is hot when strength / 2 times its value is above its "
                                                    "neighbours. 0 turns the node off.")
                                     .toStdString());
    strengthKnob->setDefaultValue(0.25);
    strengthKnob->setMinimum(0.);
    strengthKnob->setMaximum(1.);
    strengthKnob->setDisplayMinimum(0.);
    strengthKnob->setDisplayMaximum(1.);
    page->addKnob(strengthKnob);
    strength = strengthKnob;

    KnobBoolPtr permissiveKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Detect By 3 Neighbours").toStdString());
    permissiveKnob->setName(kRawHotPixelsParamPermissive);
    permissiveKnob->setHintToolTip(EffectInstance::tr("A photosite is hot when it is above three of its four neighbours instead of all four, "
                                                      "which also catches pairs of adjacent hot pixels.")
                                       .toStdString());
    permissiveKnob->setDefaultValue(false);
    permissiveKnob->setAnimationEnabled(false);
    page->addKnob(permissiveKnob);
    permissive = permissiveKnob;
}

RawHotPixelsOp::Params
RawHotPixelsOp::bake(const KnobReader& reader,
                     const ImageMetadata& source) const
{
    Params params;

    params.layout = RawLayout::fromMetadata(source).value_or(RawLayout());
    params.threshold = (float)reader.value(threshold);
    params.multiplier = (float)((double)(float)reader.value(strength) / 2.0);
    params.minNeighbours = reader.value(permissive) ? 3 : 4;
    if (params.layout.period() == 6) {
        params.xtrans = xtransNeighbours(params.layout);
    }

    return params;
}

bool
RawHotPixelsOp::isIdentity(const Params& params,
                           const PassContext& /*context*/) const
{
    return params.multiplier == 0.f;
}

PassList
RawHotPixelsOp::passes(const Params& params,
                       const PassContext& /*context*/) const
{
    PassList list;
    const int halo = params.layout.hasCfa() ? 2 : 1;

    list.boundary = PassExec::Boundary::Zero;
    list.scratchSlots = 2;
    list.resultSlot = 1;
    Pass pass = Pass::gather(halo, halo, true, hotPixelsWorker(params));
    pass.growX = 0;
    pass.growY = 0;
    pass.tapsPerSample = 5;
    list.passes.push_back(pass);

    return list;
}

RawHotPixels::RawHotPixels(NodePtr node)
    : ImageNode<RawHotPixelsOp>(node)
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

RawHotPixels::~RawHotPixels()
{
}

NATRON_NAMESPACE_EXIT
