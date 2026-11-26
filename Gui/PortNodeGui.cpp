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

#include "PortNodeGui.h"

#include <algorithm>
#include <cmath>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QApplication>
#include <QFontMetricsF>
#include <QGraphicsItem>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QStringList>
#include <QTextDocument>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Network/NetworkNode.h"
#include "Engine/Settings.h"

#include "Gui/Edge.h"
#include "Gui/GuiDefines.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGraphTextItem.h"
#include "Gui/PortListLayout.h"

NATRON_NAMESPACE_ENTER

NATRON_NAMESPACE_ANONYMOUS_ENTER

const int kTextMinPixelHeight = 4;

double
clamp01(double value)
{
    return std::min(1., std::max(0., value));
}

QColor
toQColor(const PortColor& color)
{
    return QColor(color.r, color.g, color.b);
}

double
stripeHeight()
{
    return TO_DPIY(3);
}

double
textPadding()
{
    return TO_DPIX(6);
}

double
columnGap()
{
    return TO_DPIX(16);
}

double
summaryColumnWidth()
{
    return TO_DPIX(52);
}

QFont
portFont()
{
    QFont f = qApp->font();

    if (!appPTR->getCurrentSettings()->isNodeGraphAntiAliasingEnabled()) {
        f.setStyleStrategy(QFont::NoAntialias);
    }

    return f;
}

QColor
portTextColor()
{
    double r, g, b;

    appPTR->getCurrentSettings()->getTextColor(&r, &g, &b);

    return QColor::fromRgbF(clamp01(r), clamp01(g), clamp01(b));
}

double
linearToDisplay(double value)
{
    const double c = clamp01(value);

    return (c <= 0.0031308) ? (12.92 * c) : (1.055 * std::pow(c, 1. / 2.4) - 0.055);
}

// The height in device pixels of a line of text in font, under the painter's transform.
double
paintedTextHeight(const QPainter* painter,
                  const QFont& font)
{
    const double height = QFontMetricsF(font).height();
    const QTransform& t = painter->worldTransform();

    return std::abs(t.map(QPointF(0., height)).y() - t.map(QPointF(0., 0.)).y());
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

/**
 * @brief Draws a port node's body: the header, the target stripes, the port labels and the value
 * summaries. The title is the node's own label item, placed in the header.
 **/
class PortNodeBody
    : public QGraphicsItem {
public:
    explicit PortNodeBody(PortNodeGui* node)
        : QGraphicsItem(node)
        , _node(node)
    {
    }

    void geometryChanged()
    {
        prepareGeometryChange();
        update();
    }

    virtual QRectF boundingRect() const OVERRIDE FINAL
    {
        return _node->bodyRect();
    }

    virtual void paint(QPainter* painter,
                       const QStyleOptionGraphicsItem* option,
                       QWidget* widget) OVERRIDE FINAL;

private:
    void paintSummary(QPainter* painter,
                      const PortNodeRow& row,
                      const QRectF& slot) const;

    PortNodeGui* _node;
};

void
PortNodeBody::paint(QPainter* painter,
                    const QStyleOptionGraphicsItem* /*option*/,
                    QWidget* /*widget*/)
{
    const QRectF body = _node->bodyRect();
    const double headerHeight = PortListLayout::headerHeight();
    const double rowHeight = PortListLayout::rowHeight();

    painter->fillRect(body, QColor(42, 42, 42));
    painter->fillRect(QRectF(body.left(), body.top(), body.width(), std::min(headerHeight, body.height())), _node->getHeaderColor());

    const std::vector<QColor>& stripes = _node->getTargetColors();
    if (!stripes.empty()) {
        const double stripeWidth = body.width() / stripes.size();
        for (std::size_t i = 0; i < stripes.size(); ++i) {
            painter->fillRect(QRectF(body.left() + stripeWidth * i, body.top(), stripeWidth, stripeHeight()), stripes[i]);
        }
    }

    NodeGraph* graph = _node->getDagGui();
    const QFont font = portFont();
    if ((graph && (graph->isDoingNavigatorRender() || graph->isLowDetail())) || (paintedTextHeight(painter, font) < kTextMinPixelHeight)) {
        return;
    }

    painter->setFont(font);
    painter->setPen(portTextColor());
    const double labelLeft = body.left() + PortListLayout::portRadius() + textPadding();
    const double summaryLeft = labelLeft + _node->getInputLabelWidth() + textPadding();
    const double summaryWidth = _node->getSummaryWidth();
    NodePtr node = _node->getNode();
    const std::vector<PortNodeRow>& inputs = _node->getInputRows();
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const double top = body.top() + headerHeight + rowHeight * i;
        const QRectF labelRect(labelLeft, top, summaryLeft - labelLeft, rowHeight);
        painter->drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter, inputs[i].label);
        if (inputs[i].knob && (summaryWidth > 0.) && node && !node->getInput((int)i)) {
            paintSummary(painter, inputs[i], QRectF(summaryLeft, top, summaryWidth, rowHeight));
            painter->setPen(portTextColor());
        }
    }

    const double labelRight = body.right() - PortListLayout::portRadius() - textPadding();
    const std::vector<PortNodeRow>& outputs = _node->getOutputRows();
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        const double top = body.top() + headerHeight + rowHeight * i;
        painter->drawText(QRectF(body.left(), top, labelRight - body.left(), rowHeight), Qt::AlignRight | Qt::AlignVCenter, outputs[i].label);
    }
} // PortNodeBody::paint

void
PortNodeBody::paintSummary(QPainter* painter,
                           const PortNodeRow& row,
                           const QRectF& slot) const
{
    QColor swatch;
    if (PortNodeGui::valueSummaryColor(row.knob, &swatch)) {
        const double side = slot.height() * 0.6;
        const QRectF rect(slot.left(), slot.center().y() - side / 2., side * 1.6, side);
        painter->fillRect(rect, swatch);
        painter->setPen(QColor(20, 20, 20));
        painter->drawRect(rect);

        return;
    }
    const QString text = PortNodeGui::valueSummaryText(row.knob);
    if (text.isEmpty()) {
        return;
    }
    QColor dim = portTextColor();
    dim.setAlphaF(0.6);
    painter->setPen(dim);
    const QString elided = QFontMetricsF(painter->font()).elidedText(text, Qt::ElideRight, slot.width());
    painter->drawText(slot, Qt::AlignLeft | Qt::AlignVCenter, elided);
}

/**
 * @brief One port: its shape filled with the colour of its type, ringed for a custom type.
 **/
class PortItem
    : public QGraphicsItem {
public:
    PortItem(QGraphicsItem* parent,
             const PortLook& look)
        : QGraphicsItem(parent)
        , _look(look)
        , _highlight(ePortHighlightNone)
    {
    }

    void setHighlight(PortHighlightEnum highlight)
    {
        if (highlight == _highlight) {
            return;
        }
        _highlight = highlight;
        setOpacity((highlight == ePortHighlightDimmed) ? 0.3 : 1.);
        const char* name = (highlight == ePortHighlightDimmed) ? "dimmed" : (highlight == ePortHighlightHalo) ? "halo"
                                                                                                              : "";
        setData(kPortItemDataHighlight, QString::fromUtf8(name));
        update();
    }

    virtual QRectF boundingRect() const OVERRIDE FINAL
    {
        const double r = PortListLayout::portRadius() * 1.6 + 6.;

        return QRectF(-r, -r, 2 * r, 2 * r);
    }

    virtual void paint(QPainter* painter,
                       const QStyleOptionGraphicsItem* option,
                       QWidget* widget) OVERRIDE FINAL;

private:
    PortLook _look;
    PortHighlightEnum _highlight;
};

void
PortItem::paint(QPainter* painter,
                const QStyleOptionGraphicsItem* /*option*/,
                QWidget* /*widget*/)
{
    const double r = PortListLayout::portRadius();
    QPainterPath path;

    switch (_look.shape) {
    case PortShape::Circle:
        path.addEllipse(QPointF(0., 0.), r, r);
        break;
    case PortShape::Pill:
        path.addRoundedRect(QRectF(-r * 1.6, -r * 0.8, r * 3.2, r * 1.6), r * 0.8, r * 0.8);
        break;
    case PortShape::Diamond: {
        QPolygonF diamond;
        diamond << QPointF(0., -r * 1.3) << QPointF(r * 1.3, 0.) << QPointF(0., r * 1.3) << QPointF(-r * 1.3, 0.);
        path.addPolygon(diamond);
        path.closeSubpath();
        break;
    }
    }
    painter->setRenderHint(QPainter::Antialiasing, true);
    if (_highlight == ePortHighlightHalo) {
        const double haloRadius = ((_look.shape == PortShape::Circle) ? r : r * 1.6) + 3.;
        QPen halo(QColor(255, 230, 140, 200));
        halo.setWidthF(2.5);
        painter->setPen(halo);
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(QPointF(0., 0.), haloRadius, haloRadius);
    }
    painter->fillPath(path, toQColor(_look.fill));
    QPen pen(_look.hasRing ? toQColor(_look.ring) : QColor(20, 20, 20));
    pen.setWidthF(_look.hasRing ? 2. : 1.);
    painter->strokePath(path, pen);
}

PortNodeGui::PortNodeGui(QGraphicsItem* parent)
    : NodeGui(parent)
    , _body(0)
    , _portItems()
    , _inputs()
    , _outputs()
    , _targetColors()
    , _knobConnections()
    , _headerColor(Qt::gray)
    , _inputLabelWidth(0.)
    , _outputLabelWidth(0.)
    , _hasSummaries(false)
{
    setConnectorLayout(std::unique_ptr<ConnectorLayout>(new PortListLayout(this)));
}

PortNodeGui::~PortNodeGui()
{
    for (const QMetaObject::Connection& connection : _knobConnections) {
        QObject::disconnect(connection);
    }
}

double
PortNodeGui::rowPadding()
{
    return textPadding();
}

PortListLayout*
PortNodeGui::portLayout() const
{
    return static_cast<PortListLayout*>(getConnectorLayout());
}

void
PortNodeGui::createGui()
{
    NodeGui::createGui();

    _body = new PortNodeBody(this);
    _body->setZValue(getBaseDepth() + 0.5);

    NodePtr node = getNode();
    if (node) {
        QObject::connect(node.get(), &Node::inputChanged, this, [this](int) {
            if (_body) {
                _body->update();
            }
        });
    }
    refreshPorts();
}

void
PortNodeGui::refreshPorts()
{
    if (!_body) {
        return;
    }
    _inputs.clear();
    _outputs.clear();
    _targetColors.clear();

    NetworkNode* network = portLayout()->getNetworkNode();
    if (network) {
        const std::vector<std::string> ports = network->getPorts();
        for (std::size_t i = 0; i < ports.size(); ++i) {
            PortNodeRow row;
            row.name = ports[i];
            const NetworkAttribute* attribute = network->findAttribute(ports[i]);
            row.label = QString::fromUtf8((attribute && !attribute->label.empty()) ? attribute->label.c_str() : ports[i].c_str());
            const std::optional<PortDescriptor> descriptor = network->getInputPortDescriptor((int)i);
            if (descriptor) {
                row.look = portLookFor(*descriptor);
            }
            row.knob = network->getAttributeKnob(ports[i]);
            _inputs.push_back(row);
        }
        for (const OutputPortDescriptor& output : network->getOutputPorts()) {
            PortNodeRow row;
            row.name = output.name;
            row.label = QString::fromUtf8(output.name.c_str());
            row.look = portLookFor(output.typeClass, output.type, output.array, false);
            _outputs.push_back(row);
        }
        for (const std::string& target : network->layout().targets) {
            PortColor color;
            if (networkTargetColor(target, &color)) {
                _targetColors.push_back(toQColor(color));
            }
        }
    }

    const QFontMetricsF metrics(portFont());
    _inputLabelWidth = 0.;
    _outputLabelWidth = 0.;
    _hasSummaries = false;
    for (const PortNodeRow& row : _inputs) {
        _inputLabelWidth = std::max(_inputLabelWidth, metrics.horizontalAdvance(row.label));
        if (row.knob && (valueSummaryColor(row.knob, 0) || !valueSummaryText(row.knob).isEmpty())) {
            _hasSummaries = true;
        }
    }
    for (const PortNodeRow& row : _outputs) {
        _outputLabelWidth = std::max(_outputLabelWidth, metrics.horizontalAdvance(row.label));
    }

    rebuildPortItems();
    connectPortKnobs();

    int w, h;
    computeSize(&w, &h);
    resize(w, h);
    _body->geometryChanged();
} // PortNodeGui::refreshPorts

void
PortNodeGui::rebuildPortItems()
{
    for (PortItem* item : _portItems) {
        delete item;
    }
    _portItems.clear();

    NodePtr node = getNode();
    const QString nodeName = node ? QString::fromUtf8(node->getFullyQualifiedName().c_str()) : QString();
    const QString inputSide = QString::fromUtf8("input");
    const QString outputSide = QString::fromUtf8("output");
    for (int side = 0; side < 2; ++side) {
        const std::vector<PortNodeRow>& rows = side ? _outputs : _inputs;
        for (const PortNodeRow& row : rows) {
            PortItem* item = new PortItem(_body, row.look);
            item->setZValue(1.);
            item->setData(kPortItemDataSide, side ? outputSide : inputSide);
            item->setData(kPortItemDataName, QString::fromUtf8(row.name.c_str()));
            item->setData(kPortItemDataNode, nodeName);
            item->setToolTip(row.label);
            _portItems.push_back(item);
        }
    }
    placePortItems();
}

void
PortNodeGui::setInputPortHighlight(int inputNb,
                                   PortHighlightEnum highlight)
{
    if ((inputNb >= 0) && (inputNb < (int)_inputs.size()) && (inputNb < (int)_portItems.size())) {
        _portItems[inputNb]->setHighlight(highlight);
    }
}

void
PortNodeGui::setOutputPortHighlight(int outputIndex,
                                    PortHighlightEnum highlight)
{
    const std::size_t index = _inputs.size() + (std::size_t)outputIndex;

    if ((outputIndex >= 0) && (outputIndex < (int)_outputs.size()) && (index < _portItems.size())) {
        _portItems[index]->setHighlight(highlight);
    }
}

void
PortNodeGui::clearPortHighlights()
{
    for (PortItem* item : _portItems) {
        item->setHighlight(ePortHighlightNone);
    }
}

void
PortNodeGui::placePortItems()
{
    PortListLayout* layout = portLayout();
    std::size_t index = 0;

    for (std::size_t i = 0; (i < _inputs.size()) && (index < _portItems.size()); ++i, ++index) {
        _portItems[index]->setPos(layout->inputPortPosition((int)i));
    }
    for (std::size_t i = 0; (i < _outputs.size()) && (index < _portItems.size()); ++i, ++index) {
        _portItems[index]->setPos(layout->outputPortPosition((int)i));
    }
}

void
PortNodeGui::connectPortKnobs()
{
    for (const QMetaObject::Connection& connection : _knobConnections) {
        QObject::disconnect(connection);
    }
    _knobConnections.clear();

    for (const PortNodeRow& row : _inputs) {
        KnobSignalSlotHandlerPtr handler = row.knob ? row.knob->getSignalSlotHandler() : KnobSignalSlotHandlerPtr();
        if (!handler) {
            continue;
        }
        _knobConnections.push_back(QObject::connect(handler.get(), &KnobSignalSlotHandler::valueChanged, this, [this](ViewSpec, int, int) {
            if (_body) {
                _body->update();
            }
        }));
    }

    NodePtr node = getNode();
    KnobIPtr inputOutputs = node ? node->getKnobByName(kNetworkNodeParamInputOutputs) : KnobIPtr();
    KnobSignalSlotHandlerPtr handler = inputOutputs ? inputOutputs->getSignalSlotHandler() : KnobSignalSlotHandlerPtr();
    if (handler) {
        _knobConnections.push_back(QObject::connect(handler.get(), &KnobSignalSlotHandler::valueChanged, this, [this](ViewSpec, int, int) {
            refreshEdges();
        }));
    }
}

double
PortNodeGui::getSummaryWidth() const
{
    return _hasSummaries ? summaryColumnWidth() : 0.;
}

double
PortNodeGui::getInputLabelWidth() const
{
    return _inputLabelWidth;
}

QString
PortNodeGui::valueSummaryText(const KnobIPtr& knob)
{
    if (!knob || std::dynamic_pointer_cast<KnobColor>(knob)) {
        return QString();
    }
    const int dimensions = knob->getDimension();
    if (dimensions > 4) {
        return QString();
    }
    QStringList values;
    if (Knob<double>* doubleKnob = dynamic_cast<Knob<double>*>(knob.get())) {
        for (int i = 0; i < dimensions; ++i) {
            values.push_back(QString::number(doubleKnob->getValue(i), 'g', 3));
        }
    } else if (std::dynamic_pointer_cast<KnobInt>(knob)) {
        Knob<int>* intKnob = dynamic_cast<Knob<int>*>(knob.get());
        for (int i = 0; intKnob && (i < dimensions); ++i) {
            values.push_back(QString::number(intKnob->getValue(i)));
        }
    }

    return values.join(QString::fromUtf8(" "));
}

bool
PortNodeGui::valueSummaryColor(const KnobIPtr& knob,
                               QColor* color)
{
    KnobColorPtr colorKnob = std::dynamic_pointer_cast<KnobColor>(knob);
    if (!colorKnob) {
        return false;
    }
    if (color) {
        const int dimensions = colorKnob->getDimension();
        const double r = colorKnob->getValue(0);
        const double g = (dimensions > 1) ? colorKnob->getValue(1) : r;
        const double b = (dimensions > 2) ? colorKnob->getValue(2) : r;
        *color = QColor::fromRgbF(linearToDisplay(r), linearToDisplay(g), linearToDisplay(b));
    }

    return true;
}

void
PortNodeGui::computeSize(int* w,
                         int* h) const
{
    const double portSpace = PortListLayout::portRadius() + textPadding();
    double left = portSpace + _inputLabelWidth;
    if (_hasSummaries) {
        left += textPadding() + summaryColumnWidth();
    }
    const double right = _outputLabelWidth + portSpace;
    double width = left + right + columnGap();

    NodeGraphTextItem* title = getNameItem();
    if (title) {
        width = std::max(width, title->boundingRect().width() + 2 * textPadding());
    }
    width = std::max(width, (double)TO_DPIX(NODE_WIDTH) * 1.5);

    const std::size_t rows = std::max(_inputs.size(), _outputs.size());
    const double height = PortListLayout::headerHeight() + PortListLayout::rowHeight() * rows + (rows ? TO_DPIY(4) : 0);

    *w = (int)std::ceil(width);
    *h = (int)std::ceil(height);
}

void
PortNodeGui::getInitialSize(int* w,
                            int* h) const
{
    computeSize(w, h);
}

void
PortNodeGui::adjustSizeToContent(int* w,
                                 int* h,
                                 bool /*adjustToTextSize*/)
{
    computeSize(w, h);
}

void
PortNodeGui::resizeExtraContent(int /*w*/,
                                int /*h*/,
                                bool /*forceResize*/)
{
    const QRectF body = bodyRect();
    NodeGraphTextItem* title = getNameItem();
    if (title) {
        const double textHeight = title->boundingRect().height();
        const double headerTop = body.top() + stripeHeight();
        const double headerHeight = PortListLayout::headerHeight() - stripeHeight();
        title->setPos(body.left() + textPadding() - title->document()->documentMargin(), headerTop + (headerHeight - textHeight) / 2.);
        title->setZValue(getBaseDepth() + 1);
    }
    if (_body) {
        placePortItems();
        _body->geometryChanged();
    }
}

void
PortNodeGui::applyBrush(const QBrush& brush)
{
    NodeGui::applyBrush(brush);
    _headerColor = brush.color();
    if (_body) {
        _body->update();
    }
}

NATRON_NAMESPACE_EXIT
