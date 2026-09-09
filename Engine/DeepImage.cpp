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

#include "DeepImage.h"

NATRON_NAMESPACE_ENTER

void
SampleTable::recomputeOffsets()
{
    U64 running = 0;

    for (std::size_t i = 0; i < _counts.size(); ++i) {
        _offsets[i] = running;
        running += _counts[i];
    }
    _totalSampleCount = running;
}

void
DeepChannelBuffer::allocate(std::size_t sampleCount)
{
    _data = std::make_shared<std::vector<float>>(sampleCount, 0.f);
}

float*
DeepChannelBuffer::dataForWriting()
{
    if (!_data) {
        return nullptr;
    }
    if (_data.use_count() > 1) {
        _data = std::make_shared<std::vector<float>>(*_data);
    }

    return _data->data();
}

DeepImage::DeepImage(const RectI& bounds,
                     const RenderScale& scale,
                     ViewIdx view)
    : _bounds(bounds)
    , _scale(scale)
    , _view(view)
    , _sampleTable(std::make_shared<SampleTable>((std::size_t)bounds.area()))
    , _channels()
    , _tidy(false)
{
}

SampleTable&
DeepImage::getSampleTableForWriting()
{
    if (_sampleTable.use_count() > 1) {
        _sampleTable = std::make_shared<SampleTable>(*_sampleTable);
    }

    return *_sampleTable;
}

DeepChannelBuffer&
DeepImage::getChannelForWriting(const std::string& name)
{
    DeepChannelBuffer& buf = _channels[name];

    if (buf.empty()) {
        buf.allocate((std::size_t)_sampleTable->getTotalSampleCount());
    } else {
        buf.dataForWriting();
    }

    return buf;
}

std::size_t
DeepImage::getSizeInBytes() const
{
    std::size_t total = _sampleTable ? _sampleTable->getSizeInBytes() : 0;

    for (std::map<std::string, DeepChannelBuffer>::const_iterator it = _channels.begin(); it != _channels.end(); ++it) {
        total += it->second.getSizeInBytes();
    }

    return total;
}

std::size_t
DeepImage::getUniquelyOwnedSizeInBytes() const
{
    std::size_t total = 0;

    if (_sampleTable && (_sampleTable.use_count() == 1)) {
        total += _sampleTable->getSizeInBytes();
    }
    for (std::map<std::string, DeepChannelBuffer>::const_iterator it = _channels.begin(); it != _channels.end(); ++it) {
        if (it->second.isUniquelyOwned()) {
            total += it->second.getSizeInBytes();
        }
    }

    return total;
}

NATRON_NAMESPACE_EXIT
