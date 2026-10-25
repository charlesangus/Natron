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

#include "MapInput.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/KnobReader.h"

NATRON_NAMESPACE_ENTER

float
MapInputParams::remap(float raw) const
{
    // Float precision throughout, so a sample equal to the black or white knob maps to exactly 0 or 1.
    const float v = invert ? 1.f - raw : raw;
    const float blackF = (float)black;
    const float whiteF = (float)white;
    float m;
    if (whiteF == blackF) {
        m = (v >= whiteF) ? 1.f : 0.f;
    } else {
        m = (v - blackF) / (whiteF - blackF);
    }
    // A NaN sample reads as black.
    if (!(m > 0.f)) {
        return 0.f;
    }

    return (m >= 1.f) ? 1.f : m;
}

double
MapInputParams::sizeAt(float map,
                       int dimension,
                       double sizeAtWhite) const
{
    // Written so that a Map of exactly 0 or 1 gives exactly sizeAtBlack or sizeAtWhite.
    const double m = map;

    return (1. - m) * sizeAtBlack[dimension] + m * sizeAtWhite;
}

void
MapPlane::reset(const RectI& window,
                float fill)
{
    _window = window;
    _outside = fill;
    const std::size_t count = window.isNull() ? 0 : (std::size_t)window.width() * (std::size_t)window.height();
    _values.assign(count, fill);
}

float
MapPlane::at(int x,
             int y) const
{
    if (!_window.contains(x, y)) {
        return _outside;
    }

    return _values[(std::size_t)(y - _window.y1) * (std::size_t)_window.width() + (std::size_t)(x - _window.x1)];
}

float*
MapPlane::row(int y)
{
    return _values.data() + (std::size_t)(y - _window.y1) * (std::size_t)_window.width();
}

const float*
MapPlane::row(int y) const
{
    return _values.data() + (std::size_t)(y - _window.y1) * (std::size_t)_window.width();
}

MapRange
MapPlane::tileRange(const RectI& rect) const
{
    MapRange range { _outside, _outside };
    if (rect.isNull()) {
        return range;
    }
    const RectI inside = _window.isNull() ? RectI() : _window.intersect(rect);
    if (inside.isNull()) {
        return range;
    }
    const bool reachesOutside = !_window.contains(rect);
    range.min = reachesOutside ? _outside : row(inside.y1)[inside.x1 - _window.x1];
    range.max = range.min;
    for (int y = inside.y1; y < inside.y2; ++y) {
        const float* values = row(y) + (inside.x1 - _window.x1);
        const auto [lo, hi] = std::minmax_element(values, values + inside.width());
        range.min = std::min(range.min, *lo);
        range.max = std::max(range.max, *hi);
    }

    return range;
}

void
MapInput::createKnobs(KnobHolder* holder,
                      const KnobPagePtr& page,
                      int sizeDimensions)
{
    KnobChannelSelectPtr channel = AppManager::createKnob<KnobChannelSelect>(holder, EffectInstance::tr("Map Channel"));
    channel->setName(kMapInputParamChannel);
    channel->setAnimationEnabled(false);
    channel->setDefaultValue(channel->encode(std::string(kNatronColorViewRGBA) + ".A"));
    channel->setHintToolTip(EffectInstance::tr("The channel of the Map input, in any of its layers, that scales the effect per "
                                               "pixel. A channel the input does not have, or None, reads 0."));
    page->addKnob(channel);
    _channel = channel;

    KnobBoolPtr invert = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Invert Map"));
    invert->setName(kMapInputParamInvert);
    invert->setDefaultValue(false);
    invert->setHintToolTip(EffectInstance::tr("Use 1 - Map before Map Black and Map White apply."));
    page->addKnob(invert);
    _invert = invert;

    KnobDoublePtr black = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Map Black"));
    black->setName(kMapInputParamBlack);
    black->setDefaultValue(0.);
    black->setDisplayMinimum(0.);
    black->setDisplayMaximum(1.);
    black->setAddNewLine(false);
    black->setHintToolTip(EffectInstance::tr("The Map value that gives Size At Black. Values between Map Black and Map White "
                                             "give sizes in between; values beyond them clamp."));
    page->addKnob(black);
    _black = black;

    KnobDoublePtr white = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Map White"));
    white->setName(kMapInputParamWhite);
    white->setDefaultValue(1.);
    white->setDisplayMinimum(0.);
    white->setDisplayMaximum(1.);
    white->setHintToolTip(EffectInstance::tr("The Map value that gives the full Size."));
    page->addKnob(white);
    _white = white;

    KnobDoublePtr sizeAtBlack = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Size At Black"), sizeDimensions);
    sizeAtBlack->setName(kMapInputParamSizeAtBlack);
    for (int d = 0; d < sizeDimensions; ++d) {
        sizeAtBlack->setDefaultValue(0., d);
        sizeAtBlack->setMinimum(0., d);
        sizeAtBlack->setDisplayMinimum(0., d);
        sizeAtBlack->setDisplayMaximum(100., d);
    }
    sizeAtBlack->setHintToolTip(EffectInstance::tr("The size where the Map is black. Size is the size where it is white. At 0 "
                                                   "a black Map leaves the input unchanged."));
    page->addKnob(sizeAtBlack);
    _sizeAtBlack = sizeAtBlack;
    _sizeDimensions = sizeDimensions;
} // MapInput::createKnobs

void
MapInput::declareLayerKnob(const NodePtr& node,
                           int inputNb) const
{
    KnobChannelSelectPtr channel = _channel.lock();
    if (node && channel) {
        node->declareLayerKnob(channel, inputNb, LayerKnobSpec::eRoleInputBound);
    }
}

MapInputParams
MapInput::bake(const KnobReader& reader) const
{
    MapInputParams params;
    params.invert = reader.value(_invert);
    params.black = reader.value(_black);
    params.white = reader.value(_white);
    for (int d = 0; d < 2; ++d) {
        params.sizeAtBlack[d] = reader.value(_sizeAtBlack, std::min(d, _sizeDimensions - 1));
    }

    return params;
}

bool
MapInput::resolveChannel(EffectInstance* effect,
                         int inputNb,
                         double time,
                         ViewIdx view,
                         ImageLayerDesc* layer,
                         int* channel) const
{
    KnobChannelSelectPtr knob = _channel.lock();
    if (!effect || !knob || knob->isNone() || !effect->getInput(inputNb)) {
        return false;
    }
    std::list<ImageLayerDesc> present;
    effect->getPresentLayers(time, view, inputNb, &present);

    return knob->resolve(present, layer, channel) && (*channel >= 0);
}

bool
MapInput::getInputLayersRead(EffectInstance* effect,
                             int inputNb,
                             double time,
                             ViewIdx view,
                             std::list<ImageLayerDesc>* layers) const
{
    ImageLayerDesc layer;
    int channel = -1;
    if (!resolveChannel(effect, inputNb, time, view, &layer, &channel)) {
        return false;
    }
    layers->push_back(layer);

    return true;
}

StatusEnum
MapInput::read(EffectInstance* effect,
               int inputNb,
               const MapInputParams& params,
               double time,
               ViewIdx view,
               const RenderScale& scale,
               const RectI& window,
               MapPlane* plane) const
{
    if (!effect || !effect->getInput(inputNb)) {
        plane->reset(window, 1.f);

        return eStatusOK;
    }
    ImageLayerDesc layer;
    int channel = -1;
    ImagePtr image;
    if (resolveChannel(effect, inputNb, time, view, &layer, &channel)) {
        RectI roi;
        // A selected layer is read as it is stored, not in the clip's colour layout.
        image = effect->getImage(inputNb, time, scale, view, NULL, &layer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &roi);
        if (image && !PassExec::isFloatImage(image)) {
            return eStatusFailed;
        }
    }
    fill(image.get(), channel, params, window, plane);

    return eStatusOK;
}

void
MapInput::fill(const Image* image,
               int channel,
               const MapInputParams& params,
               const RectI& window,
               MapPlane* plane)
{
    const float outside = params.remap(0.f);
    plane->reset(window, outside);
    if (!image || window.isNull() || (channel < 0) || (channel >= (int)image->getComponentsCount())) {
        return;
    }
    const RectI bounds = image->getBounds();
    const int nComps = (int)image->getComponentsCount();
    Image::ReadAccess access(image);
    const int width = window.width();
    for (int y = window.y1; y < window.y2; ++y) {
        float* row = plane->row(y);
        PassExec::readChannelRow(image, &access, bounds, nComps, channel, window.x1, y, width, 0.f, row);
        for (int x = 0; x < width; ++x) {
            row[x] = params.remap(row[x]);
        }
    }
}

NATRON_NAMESPACE_EXIT
