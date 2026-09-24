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
#include <string>

#include "Engine/EngineFwd.h"

#include "Gui/GuiFwd.h"
#include "Gui/KnobGuiLayerChannelBase.h"

NATRON_NAMESPACE_ENTER

struct ShuffleSource;
struct KnobGuiShuffleMapPrivate;

/**
 * @brief The GUI of the Shuffle node's KnobShuffleMap: one grid holding the in1 and in2
 * layer dropdowns over their channel columns, then the 0 and 1 columns, and one exclusive
 * row of cells per output channel of out1 (and of out2 when set), with the out1 and out2
 * dropdowns on the right of their rows. The dropdowns drive the node's hidden in/out
 * KnobLayerSelects. A slot whose layer its input does not carry keeps its columns, greyed
 * out, and its dropdown shows the "(not in input)" marker.
 **/
class KnobGuiShuffleMap
    : public KnobGuiLayerChannelBase {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    enum LayerRowEnum {
        eLayerRowIn1,
        eLayerRowIn2,
        eLayerRowOut1,
        eLayerRowOut2
    };

    static KnobGui* BuildKnobGui(KnobIPtr knob,
                                 KnobGuiContainerI* container)
    {
        return new KnobGuiShuffleMap(knob, container);
    }

    KnobGuiShuffleMap(KnobIPtr knob,
                      KnobGuiContainerI* container);

    virtual ~KnobGuiShuffleMap() OVERRIDE;

    virtual void removeSpecificGui() OVERRIDE FINAL;

    virtual bool shouldCreateLabel() const OVERRIDE FINAL
    {
        return false;
    }

    int getOutputRowCount() const;
    int getSourceColumnCount() const;

    /// The output channel a matrix row sets. Returns false for an out-of-range row.
    bool getOutputRow(int row, int* outSlot, int* outIndex) const;

    /// The column that selects src, or -1 when the matrix has none.
    int findSourceColumn(const ShuffleSource& src) const;

    Button* getCellButton(int row, int column) const;

    /// The dropdown bound to the in1, in2, out1 or out2 knob.
    LayerChannelRow* getLayerRow(LayerRowEnum which) const;

    /// The widget the grid lays out: cells, labels and dropdowns are its direct children.
    QWidget* getMatrixWidget() const;

    Button* getResetButton() const;

protected:
    virtual void createWidget(QHBoxLayout* layout) OVERRIDE FINAL;
    virtual void refreshWidgets() OVERRIDE FINAL;

private:
    void createLayerRows();
    void refreshLayerRows();
    void rebuildMatrix();
    void syncCheckedButtons();
    void onCellClicked(int row, int column);
    void onResetClicked();
    void onLayerRowChosen(LayerRowEnum which, const QString& layerID);
    void onLayerRowNewLayerRequested(LayerRowEnum which);

    /**
     * @brief Applies newValue to one of the node's in/out knobs as one non-mergeable undo
     * step, through that knob's own GUI so undo drives it like a direct edit.
     **/
    void pushLayerValue(LayerRowEnum which, const std::string& newValue);

    std::unique_ptr<KnobGuiShuffleMapPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_KNOBGUISHUFFLEMAP_H
