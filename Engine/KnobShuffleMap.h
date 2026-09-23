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

#ifndef NATRON_ENGINE_KNOBSHUFFLEMAP_H
#define NATRON_ENGINE_KNOBSHUFFLEMAP_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include <QtCore/QCoreApplication>
#include <QtCore/QMutex>

#include "Engine/EngineFwd.h"
#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief One output channel's source: another input's channel, a constant 0 or 1, or
 * keep (the in-place value already flowing through). eKeep is never persisted as a row;
 * it is the value getSource() returns for an output channel that has none.
 **/
struct ShuffleSource {
    enum Kind {
        eKeep,
        eInput,
        eZero,
        eOne
    };

    Kind kind;
    int slot; // 1 or 2; meaningful only when kind == eInput
    int index; // channel index within the slot's layer; meaningful only when kind == eInput

    ShuffleSource()
        : kind(eKeep)
        , slot(0)
        , index(0)
    {
    }

    static ShuffleSource makeInput(int slot, int index)
    {
        ShuffleSource src;

        src.kind = eInput;
        src.slot = slot;
        src.index = index;

        return src;
    }

    static ShuffleSource makeZero()
    {
        ShuffleSource src;

        src.kind = eZero;

        return src;
    }

    static ShuffleSource makeOne()
    {
        ShuffleSource src;

        src.kind = eOne;

        return src;
    }

    bool operator==(const ShuffleSource& other) const
    {
        if (kind != other.kind) {
            return false;
        }
        if (kind == eInput) {
            return slot == other.slot && index == other.index;
        }

        return true;
    }

    bool operator!=(const ShuffleSource& other) const
    {
        return !(*this == other);
    }
};

/**
 * @brief One row of a KnobShuffleMap: the source wired to output slot outSlot's channel
 * outIndex. A channel with no row is keep, so a row's src.kind is never eKeep.
 **/
struct ShuffleMapRow {
    int outSlot;
    int outIndex;
    ShuffleSource src;

    ShuffleMapRow()
        : outSlot(0)
        , outIndex(0)
        , src()
    {
    }

    bool operator==(const ShuffleMapRow& other) const
    {
        return outSlot == other.outSlot && outIndex == other.outIndex && src == other.src;
    }

    bool operator!=(const ShuffleMapRow& other) const
    {
        return !(*this == other);
    }
};

/**
 * @brief A table mapping Shuffle output channels to their sources.
 *
 * Rows are persisted as one KnobTable string with the columns Out/Src, e.g.
 * "<Out>out1.3</Out><Src>in2.0</Src>": Out is "out<K>.<channelIndex>" with K in {1,2},
 * Src is "in<J>.<index>" with J in {1,2}, or "0", or "1". Keep is the absence of a row
 * for that output channel, so an empty table is a true identity mapping.
 *
 * The knob stores overrides only; it has no notion of which layers or channels are
 * actually present at render time; readability of a stored source is resolved by the
 * node that reads the knob, not by this class.
 **/
class KnobShuffleMap
    : public KnobTable {
    Q_DECLARE_TR_FUNCTIONS(KnobShuffleMap)

public:
    static KnobHelper* BuildKnob(KnobHolder* holder,
                                 const std::string& label,
                                 int dimension,
                                 bool declaredByPlugin = true)
    {
        return new KnobShuffleMap(holder, label, dimension, declaredByPlugin);
    }

    KnobShuffleMap(KnobHolder* holder,
                   const std::string& description,
                   int dimension,
                   bool declaredByPlugin);

    virtual ~KnobShuffleMap();

    virtual int getColumnsCount() const OVERRIDE FINAL
    {
        return 2;
    }

    virtual std::string getColumnLabel(int col) const OVERRIDE FINAL;

    virtual std::string getColumnTag(int col) const OVERRIDE FINAL;

    virtual bool isCellEnabled(int /*row*/,
                               int /*col*/,
                               const QStringList& /*values*/) const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool isColumnEditable(int /*col*/) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return false;
    }

    virtual bool useEditButton() const OVERRIDE FINAL
    {
        return false;
    }

    std::vector<ShuffleMapRow> getRows() const;

    ShuffleSource getSource(int outSlot, int outIndex) const;

    /**
     * @brief Setting eKeep removes the row for (outSlot, outIndex), if any; any other
     * kind adds or replaces it. Always exactly one setValue() call, hence one undo step.
     **/
    void setSource(int outSlot, int outIndex, const ShuffleSource& src);

    /**
     * @brief Equivalent to setSource(outSlot, outIndex, ShuffleSource()).
     **/
    void clear(int outSlot, int outIndex);

    /**
     * @brief Empties the table: every output channel becomes keep.
     **/
    void reset();

    std::string encodeRows(const std::vector<ShuffleMapRow>& rows);
    std::vector<ShuffleMapRow> decodeRows(const std::string& raw);

    static const std::string& typeNameStatic() WARN_UNUSED_RETURN;

private:
    virtual const std::string& typeName() const OVERRIDE FINAL
    {
        return typeNameStatic();
    }

    static const std::string _typeNameStr;

    mutable QMutex _cacheMutex;
    mutable bool _cacheValid;
    mutable std::string _cachedRaw;
    mutable std::vector<ShuffleMapRow> _cachedRows;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_KNOBSHUFFLEMAP_H
