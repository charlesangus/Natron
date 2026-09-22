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

#ifndef NATRON_ENGINE_KNOBCHANNELSET_H
#define NATRON_ENGINE_KNOBCHANNELSET_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <bitset>
#include <list>
#include <set>
#include <string>
#include <vector>

#include <QtCore/QCoreApplication>
#include <QtCore/QMutex>
#include <QtCore/QRegularExpression>
#include <QtCore/QString>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

struct ChannelSetRow {
    enum ModeEnum {
        eModeNone,
        eModeAll,
        eModeLayer,
        eModeRegex
    };

    ModeEnum mode;
    std::string layerOrPattern;

    /**
     * @brief eModeLayer: the enabled channels (empty means every channel of the layer at
     * resolve() time). eModeRegex: the excluded channels instead, because a regex's channel
     * universe moves with the graph; encoding exclusion rather than inclusion means a newly
     * matched channel defaults to on without needing a value write from a GUI refresh.
     * Unused for eModeNone/eModeAll.
     **/
    std::vector<std::string> channels;

    ChannelSetRow()
        : mode(eModeLayer)
        , layerOrPattern()
        , channels()
    {
    }

    bool operator==(const ChannelSetRow& other) const
    {
        return mode == other.mode && layerOrPattern == other.layerOrPattern && channels == other.channels;
    }

    bool operator!=(const ChannelSetRow& other) const
    {
        return !(*this == other);
    }
};

struct ResolvedLayer {
    ImageLayerDesc desc;
    std::bitset<4> channels;

    /**
     * @brief The bit of `channels` standing for channel `channelIndex` of `desc`: a one-channel
     * plane is an alpha plane (see Image::canCallCopyUnProcessedChannels), so its only channel
     * is bit 3.
     **/
    static int channelBit(const ImageLayerDesc& desc,
                          int channelIndex)
    {
        return desc.getNumComponents() == 1 ? 3 : channelIndex;
    }

    bool isChannelSelected(int channelIndex) const
    {
        return channels[channelBit(desc, channelIndex)];
    }
};

/**
 * @brief A table of channel-set rows: which channels of which layers a node processes.
 *
 * Rows are persisted as one KnobTable string with the columns Mode/Layer/Channels.
 * Row 0 may be "none" or "all"; any row may name a layer (by ID, with an explicit
 * channel list or an empty list meaning "every channel the layer has at resolve time")
 * or a whole-string, case-sensitive regular expression matched against layer labels.
 *
 * The knob owns no layer list: resolve() is a pure function of the rows and of the
 * caller's list of present layers.
 **/
class KnobChannelSet
    : public KnobTable {
    Q_DECLARE_TR_FUNCTIONS(KnobChannelSet)

public:
    static KnobHelper* BuildKnob(KnobHolder* holder,
                                 const std::string& label,
                                 int dimension,
                                 bool declaredByPlugin = true)
    {
        return new KnobChannelSet(holder, label, dimension, declaredByPlugin);
    }

    KnobChannelSet(KnobHolder* holder,
                   const std::string& description,
                   int dimension,
                   bool declaredByPlugin);

    virtual ~KnobChannelSet();

    virtual int getColumnsCount() const OVERRIDE FINAL
    {
        return 3;
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

    /**
     * @brief The rows an empty value stands for: the Color layer, every channel.
     * Knob<T>::populate() resets the value to an empty string after construction, so the
     * type's own default cannot live in the constructor; getRows() substitutes these
     * rows whenever the stored value decodes to no row at all.
     **/
    static std::vector<ChannelSetRow> defaultRows();

    std::vector<ChannelSetRow> getRows() const;

    /**
     * @brief Throws std::invalid_argument if two eModeLayer rows name the same layer: a
     * layer may be chosen by only one layer row, while regex rows do not consume layers.
     * A value already holding such a duplicate can still be loaded through setValue()
     * directly, since that path bypasses this check.
     **/
    void setRows(const std::vector<ChannelSetRow>& rows,
                 ValueChangedReasonEnum reason = eValueChangedReasonNatronInternalEdited);

    void setNone();
    void setAll();

    /**
     * @brief channelsOrAll == NULL stores an empty channel list, which resolves to every
     * channel of the layer that is present at resolve() time. Throws std::invalid_argument
     * if another eModeLayer row already names layerID.
     **/
    void setLayer(int row, const std::string& layerID, const std::vector<std::string>* channelsOrAll);
    void setChannels(int row, const std::vector<std::string>& channels);

    /**
     * @brief Sets the row's pattern. An existing excluded-channel set is kept when the row
     * was already eModeRegex; otherwise the row starts with nothing excluded.
     **/
    void setRegex(int row, const std::string& pattern);

    /**
     * @brief The channels a regex row excludes from its matched layers' resolve() bits.
     * Valid only on an eModeRegex row; throws std::invalid_argument otherwise.
     **/
    void setExcludedChannels(int row, const std::vector<std::string>& names);
    std::vector<std::string> getExcludedChannels(int row) const;

    int addLayer(const std::string& layerID, const std::vector<std::string>* channelsOrAll);
    int addRegex(const std::string& pattern);

    /**
     * @brief Removes a row; row 0 cannot be removed. Hides KnobTable::removeRow, which has
     * no such guard.
     **/
    void removeRow(int row);

    std::vector<ResolvedLayer> resolve(const std::list<ImageLayerDesc>& present) const;

    bool isPatternValid(int row, QString* error) const;

    std::string getSummary() const;

    /**
     * @brief Same, but a regex row renders the present layers it matches (in present order,
     * with lowercase channel initials appended when its excluded channels leave a subset of a
     * matched layer's channels, or "(no match)" when it matches none) instead of its pattern
     * text, since the pattern alone does not tell a viewer of the node graph what is flowing.
     **/
    std::string getSummary(const std::list<ImageLayerDesc>& present) const;

    void getReferencedLayerIDs(std::set<std::string>* layerIDs) const;

    std::string encodeRows(const std::vector<ChannelSetRow>& rows);
    std::vector<ChannelSetRow> decodeRows(const std::string& raw);

    static const std::string& typeNameStatic() WARN_UNUSED_RETURN;

private:
    virtual const std::string& typeName() const OVERRIDE FINAL
    {
        return typeNameStatic();
    }

    void getRowsAndPatterns(std::vector<ChannelSetRow>* rows, std::vector<QRegularExpression>* patterns) const;

    void setRowAt(int row, const ChannelSetRow& value);

    static const std::string _typeNameStr;

    mutable QMutex _cacheMutex;
    mutable bool _cacheValid;
    mutable std::string _cachedRaw;
    mutable std::vector<ChannelSetRow> _cachedRows;
    mutable std::vector<QRegularExpression> _cachedPatterns;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_KNOBCHANNELSET_H
