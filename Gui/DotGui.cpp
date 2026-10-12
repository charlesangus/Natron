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

#include "DotGui.h"

#include <algorithm> // min, max
#include <cassert>
#include <stdexcept>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QAction>
#include <QApplication>
#include <QFile>
#include <QFontMetrics>
#include <QGridLayout>
#include <QLayout>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QtConcurrentRun> // QtCore on Qt4, QtConcurrent on Qt5
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include <ofxNatron.h>

#include "Engine/Backdrop.h"
#include "Engine/Image.h"
#include "Engine/Knob.h"
#include "Engine/Lut.h" // Color::floatToInt
#include "Engine/MergingEnum.h"
#include "Engine/Node.h"
#include "Engine/OfxEffectInstance.h"
#include "Engine/OfxImageEffectInstance.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/RotoLayer.h"
#include "Engine/Settings.h"
#include "Engine/ViewerInstance.h"

#include "Gui/BackdropGui.h"
#include "Gui/Button.h"
#include "Gui/CurveEditor.h"
#include "Gui/HostOverlay.h"
#include "Gui/DockablePanel.h"
#include "Gui/DopeSheetEditor.h"
#include "Gui/Edge.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/GuiDefines.h"
#include "Gui/KnobGui.h"
#include "Gui/KnobGuiString.h"
#include "Gui/Label.h"
#include "Gui/LineEdit.h"
#include "Gui/MultiInstancePanel.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGraphUndoRedo.h"
#include "Gui/NodeGraphTextItem.h"
#include "Gui/NodeSettingsPanel.h"
#include "Gui/SequenceFileDialog.h"
#include "Gui/SequenceFileDialog.h"
#include "Gui/SpinBox.h"
#include "Gui/ViewerGL.h"
#include "Gui/ViewerTab.h"

#define NATRON_STATE_INDICATOR_OFFSET 5

#define NATRON_EDGE_DROP_TOLERANCE 15

#define NATRON_MAGNETIC_GRID_GRIP_TOLERANCE 20

#define NATRON_MAGNETIC_GRID_RELEASE_DISTANCE 30

#define NATRON_ELLIPSE_WARN_DIAMETER 10

#define DOT_GUI_DIAMETER 15

#define DOT_GUI_LABEL_GAP 6

#define NATRON_PLUGIN_ICON_SIZE 20
#define PLUGIN_ICON_OFFSET 2

NATRON_NAMESPACE_ENTER

using std::make_pair;

#ifndef M_PI
#define M_PI        3.14159265358979323846264338327950288   /* pi             */
#endif

//////////Dot node gui
DotGui::DotGui(QGraphicsItem* parent)
    : NodeGui(parent)
    , diskShape(NULL)
    , ellipseIndicator(NULL)
    , _labelItem(NULL)
{
}

void
DotGui::createGui()
{
    double depth = getBaseDepth();

    setZValue(depth);

    diskShape = new QGraphicsEllipseItem(this);
    diskShape->setZValue(depth);
    QPointF topLeft = mapFromParent( pos() );
    int diam = TO_DPIX(DOT_GUI_DIAMETER);
    int stateOffset = TO_DPIX(NATRON_STATE_INDICATOR_OFFSET);

    diskShape->setRect( QRectF(topLeft.x(), topLeft.y(), diam, diam) );

    ellipseIndicator = new QGraphicsEllipseItem(this);
    ellipseIndicator->setRect( QRectF(topLeft.x() - stateOffset,
                                      topLeft.y() - stateOffset,
                                      diam + stateOffset * 2,
                                      diam + stateOffset * 2) );
    ellipseIndicator->hide();

    // The disk alone identifies a Dot, so unlike other nodes it shows no name, only a user label.
    _labelItem = new NodeGraphTextItem(getDagGui(), this, false);
    _labelItem->setZValue(depth + 1);
    _labelItem->hide();
    refreshExtraLabelColor();

    NodePtr node = getNode();
    if (node) {
        QObject::connect(node.get(), &Node::nodeExtraLabelChanged, this, [this](const QString& label) {
            refreshLabelText(label);
        });
        refreshLabelText(QString::fromUtf8(node->getNodeExtraLabel().c_str()));
    }
}

void
DotGui::refreshLabelText(const QString& label)
{
    if (!_labelItem) {
        return;
    }

    QString html = label.trimmed();
    html.replace(QString::fromUtf8("\n"), QString::fromUtf8("<br />"));
    _labelItem->setHtml(html);
    // The label editor wraps even an empty label in a font tag, so only visible text counts.
    if (_labelItem->document()->toPlainText().trimmed().isEmpty()) {
        _labelItem->hide();

        return;
    }
    _labelItem->adjustSize();

    const QRectF diskBbox = diskShape->boundingRect();
    const QRectF labelBbox = _labelItem->boundingRect();
    const qreal gap = TO_DPIX(DOT_GUI_LABEL_GAP);

    _labelItem->setPos(diskBbox.right() + gap, diskBbox.center().y() - labelBbox.height() / 2.);
    _labelItem->show();
}

// The label is drawn beside the disk, over the node graph, so it contrasts with the graph's
// background (the stylesheet's base colour) rather than with the disk.
void
DotGui::refreshExtraLabelColor()
{
    if (!_labelItem) {
        return;
    }
    double r, g, b;
    appPTR->getCurrentSettings()->getBaseColor(&r, &g, &b);
    const QColor background(Color::floatToInt<256>(r), Color::floatToInt<256>(g), Color::floatToInt<256>(b));
    _labelItem->setDefaultTextColor(NodeGui::contrastingLabelTextColor(background));
}

void
DotGui::applyDisabledState(bool disabled)
{
    const qreal opacity = disabled ? 0.35 : 1.;
    if (diskShape) {
        diskShape->setOpacity(opacity);
    }
    if (_labelItem) {
        _labelItem->setOpacity(opacity);
    }
}

void
DotGui::refreshStateIndicator()
{
    bool showIndicator = true;

    if ( getIsSelected() ) {
        ellipseIndicator->setBrush( QColor(255, 255, 255, 128) );
    } else {
        showIndicator = false;
    }

    if ( showIndicator && !ellipseIndicator->isVisible() ) {
        ellipseIndicator->show();
    } else if ( !showIndicator && ellipseIndicator->isVisible() ) {
        ellipseIndicator->hide();
    } else {
        update();
    }
}

void
DotGui::applyBrush(const QBrush & brush)
{
    diskShape->setBrush(brush);
}

QRectF
DotGui::boundingRect() const
{
    QTransform t;
    QRectF bbox = diskShape->boundingRect();
    QPointF center = bbox.center();

    t.translate( center.x(), center.y() );
    t.scale( scale(), scale() );
    t.translate( -center.x(), -center.y() );

    return t.mapRect(bbox);
}

QPainterPath
DotGui::shape() const
{
    return diskShape->shape();
}

NATRON_NAMESPACE_EXIT

//NATRON_NAMESPACE_USING
//#include "moc_DotGui.cpp"
