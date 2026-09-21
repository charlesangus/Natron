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

#include "KnobLayerSelect.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include <QtCore/QMutexLocker>

#include "Engine/LayerRegistry.h"

NATRON_NAMESPACE_ENTER

const std::string KnobLayerSelect::_typeNameStr("LayerSelect");

const std::string&
KnobLayerSelect::typeNameStatic()
{
    return _typeNameStr;
}

KnobLayerSelect::KnobLayerSelect(KnobHolder* holder,
                                 const std::string& description,
                                 int dimension,
                                 bool declaredByPlugin)
    : KnobTable(holder, description, dimension, declaredByPlugin)
    , _withChannelButtons(false)
    , _cacheMutex()
    , _cacheValid(false)
    , _cachedRaw()
    , _cachedLayerID()
    , _cachedChannels()
{
}

KnobLayerSelect::~KnobLayerSelect()
{
}

std::string
KnobLayerSelect::getColumnLabel(int col) const
{
    switch (col) {
    case 0:
        return tr("Layer").toStdString();
    case 1:
        return tr("Channels").toStdString();
    default:
        return std::string();
    }
}

std::string
KnobLayerSelect::getColumnTag(int col) const
{
    switch (col) {
    case 0:
        return "Layer";
    case 1:
        return "Channels";
    default:
        return std::string();
    }
}

static std::string
joinChannels(const std::vector<std::string>& channels)
{
    std::string joined;

    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (i > 0) {
            joined += ',';
        }
        joined += channels[i];
    }

    return joined;
}

static std::vector<std::string>
splitChannels(const std::string& joined)
{
    std::vector<std::string> channels;
    std::string::size_type start = 0;

    while (start <= joined.size()) {
        std::string::size_type comma = joined.find(',', start);
        if (comma == std::string::npos) {
            comma = joined.size();
        }
        std::string name = joined.substr(start, comma - start);
        if (!name.empty()) {
            channels.push_back(name);
        }
        start = comma + 1;
    }

    return channels;
}

void
KnobLayerSelect::getLayerAndChannels(std::string* layerID,
                                     std::vector<std::string>* channels) const
{
    // Knob<T>::getValue is not const although reading is guarded by the value mutex.
    KnobLayerSelect* self = const_cast<KnobLayerSelect*>(this);
    const std::string raw = self->getValue();
    QMutexLocker locker(&_cacheMutex);

    // Keyed on the raw string rather than on a value-changed hook: Knob::clone(), the
    // project-load path, never calls onInternalValueChanged.
    if (!_cacheValid || _cachedRaw != raw) {
        std::list<std::vector<std::string>> table;
        self->decodeFromKnobTableFormat(raw, &table);
        if (table.empty()) {
            // Knob<T>::populate() resets the value to an empty string after construction,
            // so the type's own default cannot live in the constructor.
            _cachedLayerID = kNatronColorLayerID;
            _cachedChannels.clear();
        } else {
            _cachedLayerID = table.front()[0];
            _cachedChannels = splitChannels(table.front()[1]);
        }
        _cachedRaw = raw;
        _cacheValid = true;
    }
    if (layerID) {
        *layerID = _cachedLayerID;
    }
    if (channels) {
        *channels = _cachedChannels;
    }
}

std::string
KnobLayerSelect::encode(const std::string& layerID,
                        const std::vector<std::string>& channels)
{
    std::list<std::vector<std::string>> table;
    std::vector<std::string> row(2);

    row[0] = layerID;
    row[1] = joinChannels(channels);
    table.push_back(row);

    return encodeToKnobTableFormat(table);
}

static void
setLayerAndChannels(KnobLayerSelect* knob,
                    const std::string& layerID,
                    const std::vector<std::string>& channels)
{
    knob->setValue(knob->encode(layerID, channels), ViewSpec::all(), 0, eValueChangedReasonNatronInternalEdited, 0);
}

std::string
KnobLayerSelect::getLayer() const
{
    std::string layerID;

    getLayerAndChannels(&layerID, 0);

    return layerID;
}

void
KnobLayerSelect::setLayer(const std::string& layerID)
{
    setLayerAndChannels(this, layerID, std::vector<std::string>());
}

std::vector<std::string>
KnobLayerSelect::getChannels() const
{
    std::vector<std::string> channels;

    getLayerAndChannels(0, &channels);

    return channels;
}

void
KnobLayerSelect::setChannels(const std::vector<std::string>& channels)
{
    if (!_withChannelButtons) {
        throw std::invalid_argument("This layer selection has no channel buttons: its channel set is always \"every channel\"");
    }

    std::string layerID;
    getLayerAndChannels(&layerID, 0);
    setLayerAndChannels(this, layerID, channels);
}

static int
channelBit(const ImageLayerDesc& desc,
           int channelIndex)
{
    // A one-channel plane is an alpha plane: see Image::canCallCopyUnProcessedChannels.
    return desc.getNumComponents() == 1 ? 3 : channelIndex;
}

static std::bitset<4>
allChannelBits(const ImageLayerDesc& desc)
{
    std::bitset<4> bits;
    const int count = std::min(desc.getNumComponents(), int(LayerRegistry::kLayerMaxChannels));

    for (int c = 0; c < count; ++c) {
        bits.set(channelBit(desc, c));
    }

    return bits;
}

static std::bitset<4>
namedChannelBits(const ImageLayerDesc& desc,
                 const std::vector<std::string>& names)
{
    std::bitset<4> bits;
    const std::vector<std::string>& channels = desc.getChannels();
    const int count = std::min((int)channels.size(), int(LayerRegistry::kLayerMaxChannels));

    for (int c = 0; c < count; ++c) {
        if (std::find(names.begin(), names.end(), channels[c]) != names.end()) {
            bits.set(channelBit(desc, c));
        }
    }

    return bits;
}

bool
KnobLayerSelect::resolve(const std::list<ImageLayerDesc>& present,
                         ResolvedLayer* resolved) const
{
    std::string layerID;
    std::vector<std::string> channels;

    getLayerAndChannels(&layerID, &channels);

    for (std::list<ImageLayerDesc>::const_iterator it = present.begin(); it != present.end(); ++it) {
        if (it->getLayerID() != layerID) {
            continue;
        }
        if (resolved) {
            resolved->desc = *it;
            resolved->channels = channels.empty() ? allChannelBits(*it) : namedChannelBits(*it, channels);
        }

        return true;
    }

    return false;
}

static std::string
layerLabelForID(const std::string& layerID)
{
    if (ImageLayerDesc::isColorLayer(layerID)) {
        return kNatronColorLayerLabel;
    }
    ImageLayerDesc desc = ImageLayerDesc::mapOFXPlaneStringToLayer(layerID);
    if (desc) {
        return desc.getLayerLabel();
    }

    return layerID;
}

std::string
KnobLayerSelect::getSummary() const
{
    std::string layerID;
    std::vector<std::string> channels;

    getLayerAndChannels(&layerID, &channels);

    std::string summary = layerLabelForID(layerID);

    if (channels.empty()) {
        return summary;
    }

    summary += '.';
    for (std::size_t c = 0; c < channels.size(); ++c) {
        if (!channels[c].empty()) {
            summary += (char)std::tolower((unsigned char)channels[c][0]);
        }
    }

    return summary;
}

void
KnobLayerSelect::getReferencedLayerIDs(std::set<std::string>* layerIDs) const
{
    std::string layerID;

    getLayerAndChannels(&layerID, 0);
    if (!layerID.empty()) {
        layerIDs->insert(layerID);
    }
}

NATRON_NAMESPACE_EXIT
