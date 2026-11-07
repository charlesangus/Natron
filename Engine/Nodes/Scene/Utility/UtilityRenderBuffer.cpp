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

#include "UtilityRenderBuffer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <pxr/base/gf/half.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/diagnostic.h>
#include <pxr/imaging/hd/types.h>

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
std::atomic<int> gLiveBuffers { 0 };

// Clear values arrive as whichever type the AOV's descriptor was given.
std::vector<double>
clearComponents(const VtValue& value)
{
    if (value.IsHolding<float>()) {
        return { value.UncheckedGet<float>() };
    }
    if (value.IsHolding<double>()) {
        return { value.UncheckedGet<double>() };
    }
    if (value.IsHolding<int>()) {
        return { static_cast<double>(value.UncheckedGet<int>()) };
    }
    if (value.IsHolding<GfVec2f>()) {
        const GfVec2f& v = value.UncheckedGet<GfVec2f>();

        return { v[0], v[1] };
    }
    if (value.IsHolding<GfVec3f>()) {
        const GfVec3f& v = value.UncheckedGet<GfVec3f>();

        return { v[0], v[1], v[2] };
    }
    if (value.IsHolding<GfVec4f>()) {
        const GfVec4f& v = value.UncheckedGet<GfVec4f>();

        return { v[0], v[1], v[2], v[3] };
    }
    TF_CODING_ERROR("A render buffer cannot be cleared with a %s", value.GetTypeName().c_str());

    return {};
}

template <class T>
void
writeComponent(std::uint8_t* destination,
               T value)
{
    std::memcpy(destination, &value, sizeof(T));
}

void
encodeComponent(HdFormat componentFormat,
                double value,
                std::uint8_t* destination)
{
    switch (componentFormat) {
    case HdFormatUNorm8:
        writeComponent<std::uint8_t>(destination, static_cast<std::uint8_t>(std::lround(std::clamp(value, 0., 1.) * 255.)));
        break;
    case HdFormatSNorm8:
        writeComponent<std::int8_t>(destination, static_cast<std::int8_t>(std::lround(std::clamp(value, -1., 1.) * 127.)));
        break;
    case HdFormatFloat16:
        writeComponent<GfHalf>(destination, GfHalf(static_cast<float>(value)));
        break;
    case HdFormatFloat32:
        writeComponent<float>(destination, static_cast<float>(value));
        break;
    case HdFormatInt32:
        writeComponent<std::int32_t>(destination, static_cast<std::int32_t>(value));
        break;
    default:
        TF_CODING_ERROR("A render buffer cannot be cleared in format %d", static_cast<int>(componentFormat));
        break;
    }
}
} // anon namespace

UtilityRenderBuffer::UtilityRenderBuffer(const SdfPath& id)
    : HdRenderBuffer(id)
{
    ++gLiveBuffers;
}

UtilityRenderBuffer::~UtilityRenderBuffer()
{
    --gLiveBuffers;
}

int
UtilityRenderBuffer::liveCount()
{
    return gLiveBuffers.load();
}

bool
UtilityRenderBuffer::Allocate(const GfVec3i& dimensions,
                              HdFormat format,
                              bool multiSampled)
{
    _Deallocate();
    if (dimensions[0] < 0 || dimensions[1] < 0 || dimensions[2] != 1) {
        TF_WARN("A render buffer must be a 2D image, not %dx%dx%d", dimensions[0], dimensions[1], dimensions[2]);

        return false;
    }
    if (format == HdFormatInvalid) {
        TF_WARN("A render buffer needs a pixel format");

        return false;
    }
    _width = static_cast<unsigned int>(dimensions[0]);
    _height = static_cast<unsigned int>(dimensions[1]);
    _format = format;
    _multiSampled = multiSampled;
    _pixels.assign(static_cast<std::size_t>(_width) * _height * HdDataSizeOfFormat(format), 0);

    return true;
}

unsigned int
UtilityRenderBuffer::GetWidth() const
{
    return _width;
}

unsigned int
UtilityRenderBuffer::GetHeight() const
{
    return _height;
}

unsigned int
UtilityRenderBuffer::GetDepth() const
{
    return 1;
}

HdFormat
UtilityRenderBuffer::GetFormat() const
{
    return _format;
}

bool
UtilityRenderBuffer::IsMultiSampled() const
{
    return _multiSampled;
}

void*
UtilityRenderBuffer::Map()
{
    ++_mappers;

    return _pixels.empty() ? nullptr : _pixels.data();
}

void
UtilityRenderBuffer::Unmap()
{
    --_mappers;
}

bool
UtilityRenderBuffer::IsMapped() const
{
    return _mappers.load() != 0;
}

void
UtilityRenderBuffer::Resolve()
{
}

bool
UtilityRenderBuffer::IsConverged() const
{
    return _converged.load();
}

void
UtilityRenderBuffer::clear(const VtValue& value)
{
    if (value.IsEmpty() || _pixels.empty()) {
        return;
    }
    const std::vector<double> components = clearComponents(value);

    if (components.empty()) {
        return;
    }
    const HdFormat componentFormat = HdGetComponentFormat(_format);
    const std::size_t componentCount = HdGetComponentCount(_format);
    const std::size_t pixelSize = HdDataSizeOfFormat(_format);
    const std::size_t componentSize = pixelSize / componentCount;
    std::vector<std::uint8_t> pixel(pixelSize, 0);

    for (std::size_t c = 0; c < std::min(componentCount, components.size()); ++c) {
        encodeComponent(componentFormat, components[c], pixel.data() + c * componentSize);
    }
    for (std::size_t offset = 0; offset < _pixels.size(); offset += pixelSize) {
        std::memcpy(_pixels.data() + offset, pixel.data(), pixelSize);
    }
}

void
UtilityRenderBuffer::setConverged(bool converged)
{
    _converged.store(converged);
}

void
UtilityRenderBuffer::setPixel(unsigned int x,
                              unsigned int y,
                              const float* components,
                              std::size_t count)
{
    if (x >= _width || y >= _height) {
        return;
    }
    const HdFormat componentFormat = HdGetComponentFormat(_format);
    const std::size_t componentCount = HdGetComponentCount(_format);
    const std::size_t pixelSize = HdDataSizeOfFormat(_format);
    const std::size_t written = std::min(componentCount, count);
    std::uint8_t* pixel = _pixels.data() + (static_cast<std::size_t>(y) * _width + x) * pixelSize;

    if (componentFormat == HdFormatFloat32) {
        std::memcpy(pixel, components, written * sizeof(float));

        return;
    }
    const std::size_t componentSize = pixelSize / componentCount;

    for (std::size_t c = 0; c < written; ++c) {
        encodeComponent(componentFormat, components[c], pixel + c * componentSize);
    }
}

void
UtilityRenderBuffer::setPixel(unsigned int x,
                              unsigned int y,
                              std::int32_t value)
{
    if (x >= _width || y >= _height) {
        return;
    }
    const HdFormat componentFormat = HdGetComponentFormat(_format);
    const std::size_t pixelSize = HdDataSizeOfFormat(_format);
    std::uint8_t* pixel = _pixels.data() + (static_cast<std::size_t>(y) * _width + x) * pixelSize;

    // A double holds every int32 exactly, so this is the same integer whichever format it lands in.
    encodeComponent(componentFormat, static_cast<double>(value), pixel);
}

void
UtilityRenderBuffer::_Deallocate()
{
    TF_VERIFY(!IsMapped());
    _width = 0;
    _height = 0;
    _format = HdFormatInvalid;
    _multiSampled = false;
    std::vector<std::uint8_t>().swap(_pixels);
    _mappers.store(0);
    _converged.store(false);
}

NATRON_NAMESPACE_EXIT
