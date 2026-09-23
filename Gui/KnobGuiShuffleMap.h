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

#ifndef NATRON_GUI_KNOBGUISHUFFLEMAP_H
#define NATRON_GUI_KNOBGUISHUFFLEMAP_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>

#include "Engine/EngineFwd.h"

#include "Gui/GuiFwd.h"
#include "Gui/KnobGuiLayerChannelBase.h"

NATRON_NAMESPACE_ENTER

struct ShuffleSource;
struct KnobGuiShuffleMapPrivate;

/**
 * @brief The GUI of the Shuffle node's KnobShuffleMap: a toggle matrix with one exclusive
 * row per output channel of out1 (and of out2 when set), and one column per channel of
 * in1, per channel of in2, then keep, 0 and 1. A slot whose layer its input does not carry
 * keeps its columns, greyed out, and its header shows the "(not in input)" marker.
 **/
class KnobGuiShuffleMap
    : public KnobGuiLayerChannelBase {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    static KnobGui* BuildKnobGui(KnobIPtr knob,
                                 KnobGuiContainerI* container)
    {
        return new KnobGuiShuffleMap(knob, container);
    }

    KnobGuiShuffleMap(KnobIPtr knob,
                      KnobGuiContainerI* container);

    virtual ~KnobGuiShuffleMap() OVERRIDE;

    virtual void removeSpecificGui() OVERRIDE FINAL;

    int getOutputRowCount() const;
    int getSourceColumnCount() const;

    /// The output channel a matrix row sets. Returns false for an out-of-range row.
    bool getOutputRow(int row, int* outSlot, int* outIndex) const;

    /// The column that selects src, or -1 when the matrix has none.
    int findSourceColumn(const ShuffleSource& src) const;

    Button* getCellButton(int row, int column) const;

    /// The header over slot's columns, empty when the slot is None.
    QString getSlotHeaderText(int slot) const;

    Button* getResetButton() const;

protected:
    virtual void createWidget(QHBoxLayout* layout) OVERRIDE FINAL;
    virtual void refreshWidgets() OVERRIDE FINAL;

private:
    void rebuildMatrix();
    void syncCheckedButtons();
    void onCellClicked(int row, int column);
    void onResetClicked();

    std::unique_ptr<KnobGuiShuffleMapPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_KNOBGUISHUFFLEMAP_H
