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

#include "KnobChannelSet.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <stdexcept>

#include <QtCore/QMutexLocker>
#include <QtCore/QRegularExpressionMatch>

#include "Engine/LayerRegistry.h"

NATRON_NAMESPACE_ENTER

static const char* const kModeNoneTag = "none";
static const char* const kModeAllTag = "all";
static const char* const kModeLayerTag = "layer";
static const char* const kModeRegexTag = "regex";

const std::string KnobChannelSet::_typeNameStr("ChannelSet");

const std::string&
KnobChannelSet::typeNameStatic()
{
    return _typeNameStr;
}

KnobChannelSet::KnobChannelSet(KnobHolder* holder,
                               const std::string& description,
                               int dimension,
                               bool declaredByPlugin)
    : KnobTable(holder, description, dimension, declaredByPlugin)
    , _cacheMutex()
    , _cacheValid(false)
    , _cachedRaw()
    , _cachedRows()
    , _cachedPatterns()
{
}

KnobChannelSet::~KnobChannelSet()
{
}

std::string
KnobChannelSet::getColumnLabel(int col) const
{
    switch (col) {
    case 0:
        return tr("Mode").toStdString();
    case 1:
        return tr("Layer").toStdString();
    case 2:
        return tr("Channels").toStdString();
    default:
        return std::string();
    }
}

std::string
KnobChannelSet::getColumnTag(int col) const
{
    switch (col) {
    case 0:
        return "Mode";
    case 1:
        return "Layer";
    case 2:
        return "Channels";
    default:
        return std::string();
    }
}

std::vector<ChannelSetRow>
KnobChannelSet::defaultRows()
{
    std::vector<ChannelSetRow> rows(1);

    rows[0].mode = ChannelSetRow::eModeLayer;
    rows[0].layerOrPattern = kNatronColorLayerID;

    return rows;
}

static const char*
modeToTag(ChannelSetRow::ModeEnum mode)
{
    switch (mode) {
    case ChannelSetRow::eModeNone:
        return kModeNoneTag;
    case ChannelSetRow::eModeAll:
        return kModeAllTag;
    case ChannelSetRow::eModeLayer:
        return kModeLayerTag;
    case ChannelSetRow::eModeRegex:
        return kModeRegexTag;
    }

    return kModeLayerTag;
}

static bool
tagToMode(const std::string& tag,
          ChannelSetRow::ModeEnum* mode)
{
    if (tag == kModeNoneTag) {
        *mode = ChannelSetRow::eModeNone;
    } else if (tag == kModeAllTag) {
        *mode = ChannelSetRow::eModeAll;
    } else if (tag == kModeLayerTag) {
        *mode = ChannelSetRow::eModeLayer;
    } else if (tag == kModeRegexTag) {
        *mode = ChannelSetRow::eModeRegex;
    } else {
        return false;
    }

    return true;
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

std::string
KnobChannelSet::encodeRows(const std::vector<ChannelSetRow>& rows)
{
    std::list<std::vector<std::string>> table;

    for (std::size_t i = 0; i < rows.size(); ++i) {
        std::vector<std::string> cells(3);
        cells[0] = modeToTag(rows[i].mode);
        if (rows[i].mode == ChannelSetRow::eModeLayer || rows[i].mode == ChannelSetRow::eModeRegex) {
            cells[1] = rows[i].layerOrPattern;
        }
        if (rows[i].mode == ChannelSetRow::eModeLayer) {
            cells[2] = joinChannels(rows[i].channels);
        }
        table.push_back(cells);
    }

    return encodeToKnobTableFormat(table);
}

std::vector<ChannelSetRow>
KnobChannelSet::decodeRows(const std::string& raw)
{
    std::list<std::vector<std::string>> table;

    decodeFromKnobTableFormat(raw, &table);

    std::vector<ChannelSetRow> rows;
    for (std::list<std::vector<std::string>>::const_iterator it = table.begin(); it != table.end(); ++it) {
        ChannelSetRow row;
        if (!tagToMode((*it)[0], &row.mode)) {
            continue;
        }
        if (row.mode == ChannelSetRow::eModeLayer || row.mode == ChannelSetRow::eModeRegex) {
            row.layerOrPattern = (*it)[1];
        }
        if (row.mode == ChannelSetRow::eModeLayer) {
            row.channels = splitChannels((*it)[2]);
        }
        rows.push_back(row);
    }

    if (rows.empty()) {
        return defaultRows();
    }

    return rows;
}

void
KnobChannelSet::getRowsAndPatterns(std::vector<ChannelSetRow>* rows,
                                   std::vector<QRegularExpression>* patterns) const
{
    // Knob<T>::getValue is not const although reading is guarded by the value mutex.
    KnobChannelSet* self = const_cast<KnobChannelSet*>(this);
    const std::string raw = self->getValue();
    QMutexLocker locker(&_cacheMutex);

    // Keyed on the raw string rather than on a value-changed hook: Knob::clone(), the
    // project-load path, never calls onInternalValueChanged.
    if (!_cacheValid || _cachedRaw != raw) {
        _cachedRows = self->decodeRows(raw);
        _cachedPatterns.clear();
        _cachedPatterns.reserve(_cachedRows.size());
        for (std::size_t i = 0; i < _cachedRows.size(); ++i) {
            QRegularExpression re;
            if (_cachedRows[i].mode == ChannelSetRow::eModeRegex) {
                re.setPattern(QRegularExpression::anchoredPattern(QString::fromUtf8(_cachedRows[i].layerOrPattern.c_str())));
            }
            _cachedPatterns.push_back(re);
        }
        _cachedRaw = raw;
        _cacheValid = true;
    }
    if (rows) {
        *rows = _cachedRows;
    }
    if (patterns) {
        *patterns = _cachedPatterns;
    }
}

std::vector<ChannelSetRow>
KnobChannelSet::getRows() const
{
    std::vector<ChannelSetRow> rows;

    getRowsAndPatterns(&rows, 0);

    return rows;
}

static void
checkRowsInvariants(const std::vector<ChannelSetRow>& rows)
{
    if (rows.empty()) {
        throw std::invalid_argument("A channel set needs at least one row");
    }
    for (std::size_t i = 1; i < rows.size(); ++i) {
        if (rows[i].mode == ChannelSetRow::eModeNone || rows[i].mode == ChannelSetRow::eModeAll) {
            throw std::invalid_argument("\"none\" and \"all\" are only legal on the first row of a channel set");
        }
    }
}

void
KnobChannelSet::setRows(const std::vector<ChannelSetRow>& rows,
                        ValueChangedReasonEnum reason)
{
    checkRowsInvariants(rows);
    setValue(encodeRows(rows), ViewSpec::all(), 0, reason, 0);
}

void
KnobChannelSet::setRowAt(int row,
                         const ChannelSetRow& value)
{
    std::vector<ChannelSetRow> rows = getRows();

    if (row < 0 || row >= (int)rows.size()) {
        throw std::invalid_argument("Channel set row index out of range");
    }
    rows[row] = value;
    setRows(rows);
}

void
KnobChannelSet::setNone()
{
    ChannelSetRow row;

    row.mode = ChannelSetRow::eModeNone;
    setRowAt(0, row);
}

void
KnobChannelSet::setAll()
{
    ChannelSetRow row;

    row.mode = ChannelSetRow::eModeAll;
    setRowAt(0, row);
}

void
KnobChannelSet::setLayer(int row,
                         const std::string& layerID,
                         const std::vector<std::string>* channelsOrAll)
{
    ChannelSetRow value;

    value.mode = ChannelSetRow::eModeLayer;
    value.layerOrPattern = layerID;
    if (channelsOrAll) {
        value.channels = *channelsOrAll;
    }
    setRowAt(row, value);
}

void
KnobChannelSet::setChannels(int row,
                            const std::vector<std::string>& channels)
{
    std::vector<ChannelSetRow> rows = getRows();

    if (row < 0 || row >= (int)rows.size()) {
        throw std::invalid_argument("Channel set row index out of range");
    }
    if (rows[row].mode != ChannelSetRow::eModeLayer) {
        throw std::invalid_argument("Channels can only be set on a layer row");
    }
    rows[row].channels = channels;
    setRows(rows);
}

void
KnobChannelSet::setRegex(int row,
                         const std::string& pattern)
{
    ChannelSetRow value;

    value.mode = ChannelSetRow::eModeRegex;
    value.layerOrPattern = pattern;
    setRowAt(row, value);
}

int
KnobChannelSet::addLayer(const std::string& layerID,
                         const std::vector<std::string>* channelsOrAll)
{
    std::vector<ChannelSetRow> rows = getRows();
    ChannelSetRow value;

    value.mode = ChannelSetRow::eModeLayer;
    value.layerOrPattern = layerID;
    if (channelsOrAll) {
        value.channels = *channelsOrAll;
    }
    rows.push_back(value);
    setRows(rows);

    return (int)rows.size() - 1;
}

int
KnobChannelSet::addRegex(const std::string& pattern)
{
    std::vector<ChannelSetRow> rows = getRows();
    ChannelSetRow value;

    value.mode = ChannelSetRow::eModeRegex;
    value.layerOrPattern = pattern;
    rows.push_back(value);
    setRows(rows);

    return (int)rows.size() - 1;
}

void
KnobChannelSet::removeRow(int row)
{
    std::vector<ChannelSetRow> rows = getRows();

    if (row < 1 || row >= (int)rows.size()) {
        throw std::invalid_argument("Only rows after the first row of a channel set can be removed");
    }
    rows.erase(rows.begin() + row);
    setRows(rows);
}

static std::bitset<4>
allChannelBits(const ImageLayerDesc& desc)
{
    std::bitset<4> bits;
    const int count = std::min(desc.getNumComponents(), int(LayerRegistry::kLayerMaxChannels));

    for (int c = 0; c < count; ++c) {
        bits.set(ResolvedLayer::channelBit(desc, c));
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
            bits.set(ResolvedLayer::channelBit(desc, c));
        }
    }

    return bits;
}

std::vector<ResolvedLayer>
KnobChannelSet::resolve(const std::list<ImageLayerDesc>& present) const
{
    std::vector<ChannelSetRow> rows;
    std::vector<QRegularExpression> patterns;

    getRowsAndPatterns(&rows, &patterns);

    std::vector<ResolvedLayer> out;
    std::map<std::string, std::size_t> indexByID;
    auto accumulate = [&out, &indexByID](const ImageLayerDesc& desc, const std::bitset<4>& bits) {
        if (bits.none()) {
            return;
        }
        std::map<std::string, std::size_t>::const_iterator found = indexByID.find(desc.getLayerID());
        if (found != indexByID.end()) {
            out[found->second].channels |= bits;
            return;
        }
        ResolvedLayer resolved;
        resolved.desc = desc;
        resolved.channels = bits;
        indexByID[desc.getLayerID()] = out.size();
        out.push_back(resolved);
    };

    if (rows.empty() || rows[0].mode == ChannelSetRow::eModeNone) {
        return out;
    }

    if (rows[0].mode == ChannelSetRow::eModeAll) {
        for (std::list<ImageLayerDesc>::const_iterator it = present.begin(); it != present.end(); ++it) {
            accumulate(*it, allChannelBits(*it));
        }
    } else {
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const ChannelSetRow& row = rows[i];
            if (row.mode == ChannelSetRow::eModeLayer) {
                for (std::list<ImageLayerDesc>::const_iterator it = present.begin(); it != present.end(); ++it) {
                    if (it->getLayerID() != row.layerOrPattern) {
                        continue;
                    }
                    accumulate(*it, row.channels.empty() ? allChannelBits(*it) : namedChannelBits(*it, row.channels));
                }
            } else if (row.mode == ChannelSetRow::eModeRegex) {
                if (!patterns[i].isValid()) {
                    continue;
                }
                for (std::list<ImageLayerDesc>::const_iterator it = present.begin(); it != present.end(); ++it) {
                    if (patterns[i].match(QString::fromUtf8(it->getLayerLabel().c_str())).hasMatch()) {
                        accumulate(*it, allChannelBits(*it));
                    }
                }
            }
        }
    }

    std::stable_partition(out.begin(), out.end(), [](const ResolvedLayer& layer) {
        return layer.desc.isColorLayer();
    });

    return out;
}

bool
KnobChannelSet::isPatternValid(int row,
                               QString* error) const
{
    std::vector<ChannelSetRow> rows;
    std::vector<QRegularExpression> patterns;

    getRowsAndPatterns(&rows, &patterns);
    if (row < 0 || row >= (int)rows.size()) {
        if (error) {
            *error = tr("Row %1 does not exist").arg(row);
        }

        return false;
    }
    if (rows[row].mode != ChannelSetRow::eModeRegex) {
        if (error) {
            error->clear();
        }

        return true;
    }
    if (patterns[row].isValid()) {
        if (error) {
            error->clear();
        }

        return true;
    }
    if (error) {
        *error = patterns[row].errorString();
        if (error->isEmpty()) {
            *error = tr("Invalid regular expression");
        }
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
KnobChannelSet::getSummary() const
{
    std::vector<ChannelSetRow> rows = getRows();

    if (rows.empty()) {
        return std::string();
    }
    if (rows[0].mode == ChannelSetRow::eModeNone) {
        return tr("None").toStdString();
    }
    if (rows[0].mode == ChannelSetRow::eModeAll) {
        return tr("All").toStdString();
    }

    std::string summary;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        std::string item;
        if (rows[i].mode == ChannelSetRow::eModeLayer) {
            item = layerLabelForID(rows[i].layerOrPattern);
            if (!rows[i].channels.empty()) {
                item += '.';
                for (std::size_t c = 0; c < rows[i].channels.size(); ++c) {
                    if (!rows[i].channels[c].empty()) {
                        item += (char)std::tolower((unsigned char)rows[i].channels[c][0]);
                    }
                }
            }
        } else if (rows[i].mode == ChannelSetRow::eModeRegex) {
            item = '/' + rows[i].layerOrPattern + '/';
        } else {
            continue;
        }
        if (!summary.empty()) {
            summary += ", ";
        }
        summary += item;
    }

    return summary;
}

void
KnobChannelSet::getReferencedLayerIDs(std::set<std::string>* layerIDs) const
{
    std::vector<ChannelSetRow> rows = getRows();

    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].mode == ChannelSetRow::eModeLayer && !rows[i].layerOrPattern.empty()) {
            layerIDs->insert(rows[i].layerOrPattern);
        }
    }
}

NATRON_NAMESPACE_EXIT
