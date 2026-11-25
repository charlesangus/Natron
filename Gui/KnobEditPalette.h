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

#ifndef NATRON_GUI_KNOBEDITPALETTE_H
#define NATRON_GUI_KNOBEDITPALETTE_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QString>
#include <QWidget>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Gui/GuiFwd.h"

#define NATRON_KNOB_EDITOR_MIME_TYPE "application/x-natron-knob-editor"
#define NATRON_KNOB_EDITOR_LINK_ENTRY "LinkKnob"

NATRON_NAMESPACE_ENTER

/**
 * @brief A kind of user knob the editor can add: the palette lists them, and the type field of
 * the property section offers those that are not pages, groups or separators.
 **/
struct KnobEditTypeEntry {
    // Stable identifier, used in the palette button names and in palette drags.
    QString id;
    QString label;
    std::string typeName;
    // The dimension of a new knob of this kind.
    int dimension;
    // A String knob shown as plain text.
    bool isLabel;
    // The script name of a new knob of this kind, before its number.
    std::string baseName;
};

const std::vector<KnobEditTypeEntry>& knobEditTypeEntries();

const KnobEditTypeEntry* findKnobEditTypeEntry(const QString& id);

/**
 * @brief The entry describing an existing knob of any dimension, or null if the editor has none for it.
 **/
const KnobEditTypeEntry* findKnobEditTypeEntry(const std::string& typeName, bool isLabel);

/**
 * @brief The label of a knob named name: its words split at case changes and digits, each
 * capitalised ("float1" gives "Float 1", "matrixSize" gives "Matrix Size").
 **/
std::string knobEditReadableLabel(const std::string& name);

/**
 * @brief The strip of buttons at the top of a panel in edit mode, one per kind of user knob plus
 * "Link knob...". A click asks for a knob after the selection; a drag carries the entry to a
 * position in the panel.
 **/
class KnobEditPalette
    : public QWidget {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    explicit KnobEditPalette(KnobEditMode* mode, QWidget* parent);

    virtual ~KnobEditPalette() OVERRIDE;

    /**
     * @brief The data of a palette drag for entry, which only the panel of mode accepts.
     **/
    static QByteArray dragPayload(const KnobEditMode* mode, const QString& entryId);

Q_SIGNALS:

    void entryClicked(QString entryId);

private:
    KnobEditMode* _mode;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_KNOBEDITPALETTE_H
