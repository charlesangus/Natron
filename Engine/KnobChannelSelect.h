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

#ifndef NATRON_ENGINE_KNOBCHANNELSELECT_H
#define NATRON_ENGINE_KNOBCHANNELSELECT_H

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
#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A single "layerID.channelName" selection, or none.
 *
 * Persisted as one KnobTable row with a single Channel column, in the same
 * "<layerID>.<channelName>" format ImageLayerDesc::getChannelOption() already produces
 * for mask choices. An empty cell means no channel is selected.
 *
 * The knob owns no layer list: resolve() is a pure function of the stored value and of
 * the caller's list of present layers.
 **/
class KnobChannelSelect
    : public KnobTable {
    Q_DECLARE_TR_FUNCTIONS(KnobChannelSelect)

public:
    static KnobHelper* BuildKnob(KnobHolder* holder,
                                 const std::string& label,
                                 int dimension,
                                 bool declaredByPlugin = true)
    {
        return new KnobChannelSelect(holder, label, dimension, declaredByPlugin);
    }

    KnobChannelSelect(KnobHolder* holder,
                      const std::string& description,
                      int dimension,
                      bool declaredByPlugin);

    virtual ~KnobChannelSelect();

    virtual int getColumnsCount() const OVERRIDE FINAL
    {
        return 1;
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
     * @brief The stored value, "<layerID>.<channelName>", or empty for none.
     **/
    std::string get() const;

    void set(const std::string& value);

    /**
     * @brief The stored form of a value, for callers that apply the change through an
     * undo command rather than set().
     **/
    std::string encode(const std::string& value);

    void setNone();

    bool isNone() const;

    bool resolve(const std::list<ImageLayerDesc>& present, ImageLayerDesc* layer, int* channelIndex) const;

    std::string getSummary() const;

    void getReferencedLayerIDs(std::set<std::string>* layerIDs) const;

    static const std::string& typeNameStatic() WARN_UNUSED_RETURN;

private:
    virtual const std::string& typeName() const OVERRIDE FINAL
    {
        return typeNameStatic();
    }

    void getLayerIDAndChannel(std::string* layerID, std::string* channelName) const;

    static const std::string _typeNameStr;

    mutable QMutex _cacheMutex;
    mutable bool _cacheValid;
    mutable std::string _cachedRaw;
    mutable std::string _cachedValue;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_KNOBCHANNELSELECT_H
