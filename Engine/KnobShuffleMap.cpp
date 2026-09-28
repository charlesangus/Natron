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

#include "KnobShuffleMap.h"

#include <bitset>
#include <cctype>
#include <cstdlib>
#include <list>
#include <sstream>

#include <QtCore/QMutexLocker>

#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Channel/Shuffle.h"

NATRON_NAMESPACE_ENTER

namespace {

// The position (0-based, ascending) outIndex occupies within viewID's colour channels sits
// on the bit ImageLayerDesc::colorViewMask(viewID) sets for it; -1 past the view's channels.
// This is the same bit a 1-channel view's one channel gets (colorViewMask puts it on bit 3),
// derived here from the mask alone so it needs no access to Shuffle's own, private version.
int
bitAtViewIndex(const std::string& viewID,
               int index)
{
    if (index < 0) {
        return -1;
    }

    const std::bitset<4> mask = ImageLayerDesc::colorViewMask(viewID);
    int seen = -1;

    for (int b = 0; b < 4; ++b) {
        if (mask.test(b)) {
            ++seen;
            if (seen == index) {
                return b;
            }
        }
    }

    return -1;
}

// The inverse of bitAtViewIndex(): viewID's channel index sitting on bit, -1 when viewID has
// no channel there.
int
indexAtViewBit(const std::string& viewID,
               int bit)
{
    if (bit < 0) {
        return -1;
    }

    const std::bitset<4> mask = ImageLayerDesc::colorViewMask(viewID);
    if (!mask.test(bit)) {
        return -1;
    }

    int index = 0;
    for (int b = 0; b < bit; ++b) {
        if (mask.test(b)) {
            ++index;
        }
    }

    return index;
}

} // namespace

const std::string KnobShuffleMap::_typeNameStr("ShuffleMap");

const std::string&
KnobShuffleMap::typeNameStatic()
{
    return _typeNameStr;
}

KnobShuffleMap::KnobShuffleMap(KnobHolder* holder,
                               const std::string& description,
                               int dimension,
                               bool declaredByPlugin)
    : KnobTable(holder, description, dimension, declaredByPlugin)
    , _cacheMutex()
    , _cacheValid(false)
    , _cachedRaw()
    , _cachedRows()
{
}

KnobShuffleMap::~KnobShuffleMap()
{
}

std::string
KnobShuffleMap::getColumnLabel(int col) const
{
    switch (col) {
    case 0:
        return tr("Out").toStdString();
    case 1:
        return tr("Src").toStdString();
    default:
        return std::string();
    }
}

std::string
KnobShuffleMap::getColumnTag(int col) const
{
    switch (col) {
    case 0:
        return "Out";
    case 1:
        return "Src";
    default:
        return std::string();
    }
}

/**
 * @brief A non-negative, all-digit integer. Empty or non-digit input fails, so a cell
 * like "out1.3x" or "out1." is rejected rather than silently truncated.
 **/
static bool
parseUInt(const std::string& text,
          int* value)
{
    if (text.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (!std::isdigit((unsigned char)text[i])) {
            return false;
        }
    }
    *value = std::atoi(text.c_str());

    return true;
}

static bool
parseOut(const std::string& text,
         int* outSlot,
         int* outIndex)
{
    if (text.compare(0, 3, "out") != 0) {
        return false;
    }
    const std::string::size_type dot = text.find('.', 3);
    if (dot == std::string::npos) {
        return false;
    }

    int slot = 0;
    int index = 0;
    if (!parseUInt(text.substr(3, dot - 3), &slot) || (slot != 1 && slot != 2)) {
        return false;
    }
    if (!parseUInt(text.substr(dot + 1), &index)) {
        return false;
    }
    *outSlot = slot;
    *outIndex = index;

    return true;
}

static bool
parseSrc(const std::string& text,
         ShuffleSource* src)
{
    if (text == "0") {
        *src = ShuffleSource::makeZero();

        return true;
    }
    if (text == "1") {
        *src = ShuffleSource::makeOne();

        return true;
    }
    if (text.compare(0, 2, "in") != 0) {
        return false;
    }
    const std::string::size_type dot = text.find('.', 2);
    if (dot == std::string::npos) {
        return false;
    }

    int slot = 0;
    int index = 0;
    if (!parseUInt(text.substr(2, dot - 2), &slot) || (slot != 1 && slot != 2)) {
        return false;
    }
    if (!parseUInt(text.substr(dot + 1), &index)) {
        return false;
    }
    *src = ShuffleSource::makeInput(slot, index);

    return true;
}

static std::string
encodeOut(int outSlot,
          int outIndex)
{
    std::ostringstream oss;

    oss << "out" << outSlot << '.' << outIndex;

    return oss.str();
}

static std::string
encodeSrc(const ShuffleSource& src)
{
    switch (src.kind) {
    case ShuffleSource::eZero:
        return "0";
    case ShuffleSource::eOne:
        return "1";
    case ShuffleSource::eInput: {
        std::ostringstream oss;
        oss << "in" << src.slot << '.' << src.index;

        return oss.str();
    }
    }

    return std::string();
}

std::string
KnobShuffleMap::encodeRows(const std::vector<ShuffleMapRow>& rows)
{
    std::list<std::vector<std::string>> table;

    for (std::size_t i = 0; i < rows.size(); ++i) {
        std::vector<std::string> cells(2);
        cells[0] = encodeOut(rows[i].outSlot, rows[i].outIndex);
        cells[1] = encodeSrc(rows[i].src);
        table.push_back(cells);
    }

    return encodeToKnobTableFormat(table);
}

std::vector<ShuffleMapRow>
KnobShuffleMap::decodeRows(const std::string& raw)
{
    std::list<std::vector<std::string>> table;

    decodeFromKnobTableFormat(raw, &table);

    std::vector<ShuffleMapRow> rows;
    for (std::list<std::vector<std::string>>::const_iterator it = table.begin(); it != table.end(); ++it) {
        ShuffleMapRow row;
        if (!parseOut((*it)[0], &row.outSlot, &row.outIndex)) {
            continue;
        }
        if (!parseSrc((*it)[1], &row.src)) {
            continue;
        }
        rows.push_back(row);
    }

    return rows;
}

std::vector<ShuffleMapRow>
KnobShuffleMap::getRows() const
{
    // Knob<T>::getValue is not const although reading is guarded by the value mutex.
    KnobShuffleMap* self = const_cast<KnobShuffleMap*>(this);
    const std::string raw = self->getValue();
    QMutexLocker locker(&_cacheMutex);

    // Keyed on the raw string rather than on a value-changed hook: Knob::clone(), the
    // project-load path, never calls onInternalValueChanged.
    if (!_cacheValid || _cachedRaw != raw) {
        _cachedRows = self->decodeRows(raw);
        _cachedRaw = raw;
        _cacheValid = true;
    }

    return _cachedRows;
}

ShuffleSource
KnobShuffleMap::defaultSource(int outSlot,
                              int outIndex)
{
    return ShuffleSource::makeInput(outSlot, outIndex);
}

ShuffleSource
KnobShuffleMap::getSource(int outSlot,
                          int outIndex) const
{
    std::vector<ShuffleMapRow> rows = getRows();

    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].outSlot == outSlot && rows[i].outIndex == outIndex) {
            return rows[i].src;
        }
    }

    return defaultSource(outSlot, outIndex);
}

bool
KnobShuffleMap::hasExplicitSource(int outSlot,
                                  int outIndex) const
{
    std::vector<ShuffleMapRow> rows = getRows();

    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].outSlot == outSlot && rows[i].outIndex == outIndex) {
            return true;
        }
    }

    return false;
}

ShuffleSource
KnobShuffleMap::implicitDefault(int outSlot,
                                int outIndex) const
{
    const Shuffle* shuffle = dynamic_cast<const Shuffle*>(getHolder());

    if (shuffle) {
        const std::string outLayer = shuffle->getOutputLayer(outSlot);
        const std::string slotLayer = shuffle->getSlotLayer(outSlot);
        if (ImageLayerDesc::isColorViewID(outLayer) && ImageLayerDesc::isColorViewID(slotLayer)) {
            const int slotIndex = indexAtViewBit(slotLayer, bitAtViewIndex(outLayer, outIndex));

            return (slotIndex < 0) ? ShuffleSource::makeZero() : ShuffleSource::makeInput(outSlot, slotIndex);
        }
    }

    return defaultSource(outSlot, outIndex);
}

void
KnobShuffleMap::setSource(int outSlot,
                          int outIndex,
                          const ShuffleSource& src)
{
    std::vector<ShuffleMapRow> rows = getRows();
    const bool isDefault = (src == implicitDefault(outSlot, outIndex));
    bool found = false;

    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].outSlot != outSlot || rows[i].outIndex != outIndex) {
            continue;
        }
        if (isDefault) {
            rows.erase(rows.begin() + i);
        } else {
            rows[i].src = src;
        }
        found = true;
        break;
    }
    if (!found && !isDefault) {
        ShuffleMapRow row;
        row.outSlot = outSlot;
        row.outIndex = outIndex;
        row.src = src;
        rows.push_back(row);
    }

    setValue(encodeRows(rows), ViewSpec::all(), 0, eValueChangedReasonNatronInternalEdited, 0);
}

void
KnobShuffleMap::clear(int outSlot,
                      int outIndex)
{
    std::vector<ShuffleMapRow> rows = getRows();

    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].outSlot == outSlot && rows[i].outIndex == outIndex) {
            rows.erase(rows.begin() + i);
            break;
        }
    }

    setValue(encodeRows(rows), ViewSpec::all(), 0, eValueChangedReasonNatronInternalEdited, 0);
}

void
KnobShuffleMap::reset()
{
    resetToDefaultValue(0);
}

NATRON_NAMESPACE_EXIT
