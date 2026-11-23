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

#ifndef NATRON_ENGINE_KNOBLAYERCUSTOMDATA_H
#define NATRON_ENGINE_KNOBLAYERCUSTOMDATA_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include <QtCore/QCoreApplication>

#include "Engine/EngineFwd.h"
#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Rows of name, type and value, edited in place, that a node turns into custom data
 * entries. The type cell holds one of typeNames(); the node parses the value by it.
 **/
class KnobLayerCustomData
    : public KnobTable {
    Q_DECLARE_TR_FUNCTIONS(KnobLayerCustomData)

public:
    static KnobHelper* BuildKnob(KnobHolder* holder,
                                 const std::string& label,
                                 int dimension,
                                 bool declaredByPlugin = true)
    {
        return new KnobLayerCustomData(holder, label, dimension, declaredByPlugin);
    }

    KnobLayerCustomData(KnobHolder* holder,
                        const std::string& description,
                        int dimension,
                        bool declaredByPlugin);

    virtual ~KnobLayerCustomData();

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
        return true;
    }

    virtual bool useEditButton() const OVERRIDE FINAL
    {
        return false;
    }

    // The types a row may have, as the type cell spells them.
    static const std::vector<std::string>& typeNames() WARN_UNUSED_RETURN;

    static const std::string& typeNameStatic() WARN_UNUSED_RETURN;

private:
    virtual const std::string& typeName() const OVERRIDE FINAL
    {
        return typeNameStatic();
    }

    static const std::string _typeNameStr;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_KNOBLAYERCUSTOMDATA_H
