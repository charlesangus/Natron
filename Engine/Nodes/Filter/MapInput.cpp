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
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
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

namespace {

float
floatFromBits(uint32_t bits)
{
    return std::bit_cast<float>(bits);
}

// The coefficients of log2(f) = s * (c1 + c3 s^2 + c5 s^4 + c7 s^6 + c9 s^8), s = (f - 1) / (f + 1),
// that is 2 / (ln 2 (2i + 1)), and of 2^r = sum of (ln 2)^i / i! r^i up to r^7, by their bits so
// that the kernels use the same floats.
constexpr uint32_t kLog2Coefficients[] = { 0x4038aa3bu, 0x3f76384fu, 0x3f13bb63u, 0x3ed30bb1u, 0x3ea4258au };
constexpr uint32_t kExp2Coefficients[] = { 0x3f800000u, 0x3f317218u, 0x3e75fdf0u, 0x3d635847u, 0x3c1d955bu, 0x3aaec3ffu, 0x39218489u, 0x377fe5feu };
// The smallest normal float, and the float nearest sqrt(2).
constexpr uint32_t kSmallestNormal = 0x00800000u;
constexpr uint32_t kSqrt2 = 0x3fb504f3u;

} // namespace

float
mapGammaPow(float m,
            float exponent)
{
    if (m < floatFromBits(kSmallestNormal)) {
        return 0.f;
    }
    // m = f * 2^k, with f in [sqrt(2)/2, sqrt(2)].
    const uint32_t bits = std::bit_cast<uint32_t>(m);
    int k = (int)(bits >> 23) - 127;
    float f = floatFromBits((bits & 0x007fffffu) | 0x3f800000u);
    if (f > floatFromBits(kSqrt2)) {
        f = f * 0.5f;
        k += 1;
    }
    const float num = f - 1.f;
    const float den = f + 1.f;
    const float s = num / den;
    const float s2 = s * s;
    const int nLog = (int)(sizeof(kLog2Coefficients) / sizeof(kLog2Coefficients[0]));
    float p = floatFromBits(kLog2Coefficients[nLog - 1]);
    for (int i = nLog - 2; i >= 0; --i) {
        const float t = p * s2;
        p = t + floatFromBits(kLog2Coefficients[i]);
    }
    const float logF = p * s;
    const float log2M = (float)k + logF;
    const float y = exponent * log2M;
    if (!(y >= -125.f)) {
        return 0.f;
    }
    // 2^y = 2^r * 2^n, with r in [-1/2, 1/2] and n in [-125, 0].
    const float shifted = y + 0.5f;
    const float n = std::floor(shifted);
    const float r = y - n;
    const int nExp = (int)(sizeof(kExp2Coefficients) / sizeof(kExp2Coefficients[0]));
    float q = floatFromBits(kExp2Coefficients[nExp - 1]);
    for (int i = nExp - 2; i >= 0; --i) {
        const float t = q * r;
        q = t + floatFromBits(kExp2Coefficients[i]);
    }
    const float scale = floatFromBits((uint32_t)((int)n + 127) << 23);
    const float result = q * scale;

    return (result < 1.f) ? result : 1.f;
} // mapGammaPow

float
MapInputParams::remap(float raw) const
{
    const float v = invert ? 1.f - raw : raw;
    // A NaN sample reads as black.
    if (!(v > 0.f)) {
        return 0.f;
    }
    if (v >= 1.f) {
        return 1.f;
    }
    float m = v;
    if (exponent != 1.f) {
        m = mapGammaPow(m, exponent);
    }
    if (smooth) {
        // Smoothstep, 3m^2 - 2m^3, in the order the kernels use.
        const float square = m * m;
        const float twice = m * 2.f;
        const float slope = 3.f - twice;
        m = square * slope;
    }

    return m;
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
MapInput::createSizeAtBlackKnob(KnobHolder* holder,
                                const KnobPagePtr& page,
                                int sizeDimensions)
{
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
}

void
MapInput::createKnobs(KnobHolder* holder,
                      const KnobPagePtr& page)
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
    invert->setHintToolTip(EffectInstance::tr("Use 1 - Map before Map Gamma and Map Response apply."));
    page->addKnob(invert);
    _invert = invert;

    KnobChoicePtr response = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("Map Response"));
    response->setName(kMapInputParamResponse);
    response->setHintToolTip(EffectInstance::tr("How Map values between 0 and 1, after Map Gamma, become sizes between Size At "
                                                "Black and Size."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kMapInputParamResponseLinear, EffectInstance::tr("Linear").toStdString(),
                                       EffectInstance::tr("The size follows the Map value in proportion.").toStdString()));
        options.push_back(ChoiceOption(kMapInputParamResponseSmooth, EffectInstance::tr("Smooth").toStdString(),
                                       EffectInstance::tr("Smoothstep, 3m^2 - 2m^3: the size eases out of Size At Black and into "
                                                          "Size.")
                                           .toStdString()));
        response->populateChoices(options);
    }
    response->setDefaultValue(0);
    response->setAddNewLine(false);
    page->addKnob(response);
    _response = response;

    KnobDoublePtr gamma = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Map Gamma"));
    gamma->setName(kMapInputParamGamma);
    gamma->setDefaultValue(1.);
    gamma->setMinimum(0.01);
    gamma->setDisplayMinimum(0.2);
    gamma->setDisplayMaximum(5.);
    gamma->setHintToolTip(EffectInstance::tr("Raises the Map, clamped to [0,1], to the power 1/gamma, as the Gamma node does: "
                                             "above 1 the sizes lean towards Size, below 1 towards Size At Black. 0 and 1 "
                                             "stay put."));
    page->addKnob(gamma);
    _gamma = gamma;
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
    params.smooth = (reader.value(_response) == 1);
    params.exponent = (float)(1. / std::max(0.01, reader.value(_gamma)));
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
