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

#include "KnobChannelSelect.h"

#include <QtCore/QMutexLocker>

NATRON_NAMESPACE_ENTER

static const char* const kDefaultChannelName = "A";

const std::string KnobChannelSelect::_typeNameStr("ChannelSelect");

const std::string&
KnobChannelSelect::typeNameStatic()
{
    return _typeNameStr;
}

KnobChannelSelect::KnobChannelSelect(KnobHolder* holder,
                                     const std::string& description,
                                     int dimension,
                                     bool declaredByPlugin)
    : KnobTable(holder, description, dimension, declaredByPlugin)
    , _cacheMutex()
    , _cacheValid(false)
    , _cachedRaw()
    , _cachedValue()
{
}

KnobChannelSelect::~KnobChannelSelect()
{
}

std::string
KnobChannelSelect::getColumnLabel(int /*col*/) const
{
    return tr("Channel").toStdString();
}

std::string
KnobChannelSelect::getColumnTag(int /*col*/) const
{
    return "Channel";
}

std::string
KnobChannelSelect::get() const
{
    // Knob<T>::getValue is not const although reading is guarded by the value mutex.
    KnobChannelSelect* self = const_cast<KnobChannelSelect*>(this);
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
            _cachedValue = std::string(kNatronColorLayerID) + "." + kDefaultChannelName;
        } else {
            _cachedValue = table.front()[0];
        }
        _cachedRaw = raw;
        _cacheValid = true;
    }

    return _cachedValue;
}

std::string
KnobChannelSelect::encode(const std::string& value)
{
    std::list<std::vector<std::string>> table;
    std::vector<std::string> row(1, value);

    table.push_back(row);

    return encodeToKnobTableFormat(table);
}

void
KnobChannelSelect::set(const std::string& value)
{
    setValue(encode(value), ViewSpec::all(), 0, eValueChangedReasonNatronInternalEdited, 0);
}

void
KnobChannelSelect::setNone()
{
    set(std::string());
}

bool
KnobChannelSelect::isNone() const
{
    return get().empty();
}

void
KnobChannelSelect::getLayerIDAndChannel(std::string* layerID,
                                        std::string* channelName) const
{
    const std::string value = get();
    const std::string::size_type dot = value.rfind('.');

    if (dot == std::string::npos) {
        return;
    }
    if (layerID) {
        *layerID = value.substr(0, dot);
    }
    if (channelName) {
        *channelName = value.substr(dot + 1);
    }
}

bool
KnobChannelSelect::resolve(const std::list<ImageLayerDesc>& present,
                           ImageLayerDesc* layer,
                           int* channelIndex) const
{
    std::string layerID, channelName;

    getLayerIDAndChannel(&layerID, &channelName);
    if (layerID.empty()) {
        return false;
    }

    for (std::list<ImageLayerDesc>::const_iterator it = present.begin(); it != present.end(); ++it) {
        if (it->getLayerID() != layerID) {
            continue;
        }
        const std::vector<std::string>& channels = it->getChannels();
        for (std::size_t c = 0; c < channels.size(); ++c) {
            if (channels[c] == channelName) {
                if (layer) {
                    *layer = *it;
                }
                if (channelIndex) {
                    *channelIndex = (int)c;
                }

                return true;
            }
        }

        return false;
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
KnobChannelSelect::getSummary() const
{
    if (isNone()) {
        return tr("None").toStdString();
    }

    std::string layerID, channelName;
    getLayerIDAndChannel(&layerID, &channelName);

    return layerLabelForID(layerID) + "." + channelName;
}

void
KnobChannelSelect::getReferencedLayerIDs(std::set<std::string>* layerIDs) const
{
    std::string layerID;

    getLayerIDAndChannel(&layerID, 0);
    if (!layerID.empty()) {
        layerIDs->insert(layerID);
    }
}

NATRON_NAMESPACE_EXIT
