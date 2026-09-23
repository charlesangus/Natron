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

#ifndef NATRON_ENGINE_KNOBLAYERSELECT_H
#define NATRON_ENGINE_KNOBLAYERSELECT_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <set>
#include <string>
#include <vector>

#include <QtCore/QCoreApplication>
#include <QtCore/QMutex>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A single layer selection, with an optional per-channel enable set.
 *
 * Persisted as one KnobTable row with the columns Layer/Channels. An empty Channels
 * cell means every channel of the layer, which is the only state a knob created
 * without channel buttons (withChannelButtons() == false) can ever be in.
 *
 * The knob owns no layer list: resolve() is a pure function of the stored value and of
 * the caller's list of present layers, using the same ID-match / name-intersection /
 * one-channel-layer-maps-to-bit-3 rules as a single "layer" row of KnobChannelSet.
 **/
class KnobLayerSelect
    : public KnobTable {
    Q_DECLARE_TR_FUNCTIONS(KnobLayerSelect)

public:
    static KnobHelper* BuildKnob(KnobHolder* holder,
                                 const std::string& label,
                                 int dimension,
                                 bool declaredByPlugin = true)
    {
        return new KnobLayerSelect(holder, label, dimension, declaredByPlugin);
    }

    KnobLayerSelect(KnobHolder* holder,
                    const std::string& description,
                    int dimension,
                    bool declaredByPlugin);

    virtual ~KnobLayerSelect();

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

    /**
     * @brief Whether this knob shows per-channel enable buttons. Set once by the node
     * that creates the knob: it describes the node kind, not a project value, so it is
     * a plain member and is never persisted.
     **/
    void setWithChannelButtons(bool withChannelButtons)
    {
        _withChannelButtons = withChannelButtons;
    }

    bool getWithChannelButtons() const
    {
        return _withChannelButtons;
    }

    std::string getLayer() const;

    /**
     * @brief Resets the Channels cell to empty (every channel of the new layer), the
     * shared "reset on layer change" rule.
     **/
    void setLayer(const std::string& layerID);

    std::vector<std::string> getChannels() const;

    /**
     * @brief Throws std::invalid_argument when the knob was created without channel
     * buttons: an empty Channels cell is the only state such a knob can hold.
     **/
    void setChannels(const std::vector<std::string>& channels);

    /**
     * @brief The stored form of a layer plus channel selection, for callers that apply
     * the change through an undo command rather than setLayer()/setChannels().
     **/
    std::string encode(const std::string& layerID, const std::vector<std::string>& channels);

    bool resolve(const std::list<ImageLayerDesc>& present, ResolvedLayer* resolved) const;

    std::string getSummary() const;

    void getReferencedLayerIDs(std::set<std::string>* layerIDs) const;

    static const std::string& typeNameStatic() WARN_UNUSED_RETURN;

private:
    virtual const std::string& typeName() const OVERRIDE FINAL
    {
        return typeNameStatic();
    }

    void getLayerAndChannels(std::string* layerID, std::vector<std::string>* channels) const;

    static const std::string _typeNameStr;

    bool _withChannelButtons;

    mutable QMutex _cacheMutex;
    mutable bool _cacheValid;
    mutable std::string _cachedRaw;
    mutable std::string _cachedLayerID;
    mutable std::vector<std::string> _cachedChannels;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_KNOBLAYERSELECT_H
