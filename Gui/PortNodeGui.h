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

#ifndef NATRON_GUI_PORTNODEGUI_H
#define NATRON_GUI_PORTNODEGUI_H

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
#include <QColor>
#include <QMetaObject>
#include <QString>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Network/PortTypes.h"

#include "Gui/GuiFwd.h"
#include "Gui/NodeGui.h"

NATRON_NAMESPACE_ENTER

class PortListLayout;
class PortNodeBody;
class PortItem;

// The data() keys a port item holds: its side ("input" or "output"), its port name and the fully
// qualified name of its node, so that scripts can find a node's ports in the scene.
#define kPortItemDataSide 0
#define kPortItemDataName 1
#define kPortItemDataNode 2
// "dimmed", "halo" or empty: how the port is highlighted while a pipe is dragged.
#define kPortItemDataHighlight 3

enum PortHighlightEnum {
    ePortHighlightNone,
    // A dragged pipe cannot connect to the port.
    ePortHighlightDimmed,
    // A dragged pipe can connect to the port.
    ePortHighlightHalo,
};

/**
 * @brief One port row of a port node.
 **/
struct PortNodeRow {
    std::string name;
    QString label;
    PortLook look;
    // The knob the port's value lives in when it is not connected; null for a port-only
    // attribute or an output.
    KnobIPtr knob;
};

/**
 * @brief The graph item of a network node: a header in the node's category colour with a stripe
 * per target, then one row per port, inputs on the left and named outputs on the right. An
 * unconnected input with a value shows it read-only beside its label.
 **/
class PortNodeGui
    : public NodeGui {
public:
    explicit PortNodeGui(QGraphicsItem* parent = 0);

    virtual ~PortNodeGui() OVERRIDE;

    /**
     * @brief Rebuilds the rows from the node's ports and outputs, and resizes the node to hold
     * them.
     **/
    void refreshPorts();

    QColor getHeaderColor() const
    {
        return _headerColor;
    }

    const std::vector<PortNodeRow>& getInputRows() const
    {
        return _inputs;
    }

    const std::vector<PortNodeRow>& getOutputRows() const
    {
        return _outputs;
    }

    void setInputPortHighlight(int inputNb, PortHighlightEnum highlight);

    void setOutputPortHighlight(int outputIndex, PortHighlightEnum highlight);

    void clearPortHighlights();

    // The height of the header the rows start under.
    double getHeaderHeight() const;

    // Whether the node stands in for a network node whose plug-in is missing.
    bool isMissingPlugin() const;

    const std::vector<QColor>& getTargetColors() const
    {
        return _targetColors;
    }

    // The width of the column the value summaries are drawn in, 0 when no input has a value.
    double getSummaryWidth() const;

    // The width of the widest input label.
    double getInputLabelWidth() const;

    static double rowPadding();

    // The text of the value summary of a knob that is not a colour, empty when it has none.
    static QString valueSummaryText(const KnobIPtr& knob);

    // Whether knob's value is shown as a swatch, written to *color in display space.
    static bool valueSummaryColor(const KnobIPtr& knob, QColor* color);

private:
    virtual void createGui() OVERRIDE FINAL;

    virtual bool canMakePreview() OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return false;
    }

    virtual bool wantsLabelWrap() const OVERRIDE FINAL
    {
        return false;
    }

    virtual void getInitialSize(int* w, int* h) const OVERRIDE FINAL;

    virtual void adjustSizeToContent(int* w, int* h, bool adjustToTextSize) OVERRIDE FINAL;

    virtual void resizeExtraContent(int w, int h, bool forceResize) OVERRIDE FINAL;

    virtual void applyBrush(const QBrush& brush) OVERRIDE FINAL;

    void computeSize(int* w, int* h) const;

    void connectPortKnobs();

    void refreshMissingPluginRows();

    void refreshDownstreamPipes();

    void rebuildPortItems();

    void placePortItems();

    PortListLayout* portLayout() const;

    PortNodeBody* _body;
    std::vector<PortItem*> _portItems;
    std::vector<PortNodeRow> _inputs;
    std::vector<PortNodeRow> _outputs;
    std::vector<QColor> _targetColors;
    std::vector<QMetaObject::Connection> _knobConnections;
    QColor _headerColor;
    double _inputLabelWidth;
    double _outputLabelWidth;
    bool _hasSummaries;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_PORTNODEGUI_H
