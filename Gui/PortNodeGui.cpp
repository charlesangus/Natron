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
#include <limits>
#include <map>
#include <numeric>
#include <set>

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
#include "Engine/Nodes/MissingPluginNode.h"
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
const double kPipeStubWidth = 2.;

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

double
selectionGap()
{
    return TO_DPIX(3);
}

double
selectionPenWidth()
{
    return TO_DPIX(2);
}

// The outline keeps at least this many device pixels when zoomed out, so a selected node still
// reads as selected from afar.
const double kSelectionMinDevicePixels = 1.5;
// The zoom down to which the widened outline stays inside the item's bounds.
const double kSelectionMinBoundedZoom = 0.1;

double
portInsetWidth()
{
    return TO_DPIX(8);
}

double
haloWidth()
{
    return TO_DPIX(6);
}

// The halo's rings, from the outermost in; each adds its alpha to those outside it.
const int kHaloSteps = 6;
const int kHaloStepAlpha = 28;

// The main graph's selection colour, the state indicator's selected fill.
QColor
selectionColor()
{
    return QColor(Qt::white);
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

// Draws the inputs in the order of the node's attributes, whatever order they were shown in.
void
assignInputRows(const NetworkNode& network,
                std::vector<PortNodeRow>* inputs)
{
    std::map<std::string, std::size_t> rank;
    const std::vector<NetworkAttribute>& attributes = network.layout().attributes;
    for (std::size_t i = 0; i < attributes.size(); ++i) {
        rank.insert(std::make_pair(attributes[i].name, i));
    }
    std::vector<std::size_t> order(inputs->size());
    std::iota(order.begin(), order.end(), (std::size_t)0);
    const auto rankOf = [&rank](const std::string& name) {
        const std::map<std::string, std::size_t>::const_iterator it = rank.find(name);

        return (it == rank.end()) ? std::numeric_limits<std::size_t>::max() : it->second;
    };
    std::stable_sort(order.begin(), order.end(), [inputs, &rankOf](std::size_t a, std::size_t b) {
        return rankOf((*inputs)[a].name) < rankOf((*inputs)[b].name);
    });
    for (std::size_t r = 0; r < order.size(); ++r) {
        (*inputs)[order[r]].row = (int)r;
    }
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

/**
 * @brief The node's name in the header, elided to the header's width and clipped to it, with the
 * full name in the tool tip. A missing plug-in's caption takes the header's second line.
 **/
class PortNodeTitle
    : public QGraphicsItem {
public:
    explicit PortNodeTitle(PortNodeGui* node)
        : QGraphicsItem(node)
        , _node(node)
        , _rect()
        , _text()
        , _caption()
    {
        setData(kPortItemDataSide, QString::fromUtf8(kPortNodeTitleSide));
    }

    void setContent(const QRectF& rect,
                    const QFont& font,
                    const QString& title,
                    const QString& caption)
    {
        prepareGeometryChange();
        _rect = rect;
        _font = font;
        const QFontMetricsF metrics(font);
        _text = metrics.elidedText(title, Qt::ElideRight, std::max(0., rect.width()));
        _caption = caption.isEmpty() ? QString() : QFontMetricsF(portFont()).elidedText(caption, Qt::ElideRight, std::max(0., rect.width()));
        setData(kPortItemDataName, _text);
        setToolTip(caption.isEmpty() ? title : (title + QLatin1Char('\n') + caption));
        update();
    }

    virtual QRectF boundingRect() const OVERRIDE FINAL
    {
        return _rect;
    }

    virtual void paint(QPainter* painter,
                       const QStyleOptionGraphicsItem* option,
                       QWidget* widget) OVERRIDE FINAL;

private:
    PortNodeGui* _node;
    QRectF _rect;
    QFont _font;
    QString _text;
    QString _caption;
};

void
PortNodeTitle::paint(QPainter* painter,
                     const QStyleOptionGraphicsItem* /*option*/,
                     QWidget* /*widget*/)
{
    NodeGraph* graph = _node->getDagGui();
    if ((graph && (graph->isDoingNavigatorRender() || graph->isLowDetail())) || (paintedTextHeight(painter, _font) < kTextMinPixelHeight)) {
        return;
    }
    NodeGraphTextItem* label = _node->getNameItem();
    painter->save();
    painter->setClipRect(_rect, Qt::IntersectClip);
    const double rowHeight = PortListLayout::rowHeight();
    const QRectF nameRect(_rect.left(), _rect.top(), _rect.width(), _caption.isEmpty() ? _rect.height() : (_rect.height() - rowHeight));
    painter->setFont(_font);
    const bool onSelectionColor = _node->getIsSelected() && (PortNodeGui::selectionStyle() == ePortSelectionStyleHeader);
    painter->setPen(onSelectionColor ? QColor(20, 20, 20) : (label ? label->defaultTextColor() : portTextColor()));
    painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, _text);
    if (!_caption.isEmpty()) {
        painter->setFont(portFont());
        painter->setPen(NodeGui::missingPluginOutline().color());
        painter->drawText(QRectF(_rect.left(), nameRect.bottom(), _rect.width(), rowHeight), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, _caption);
    }
    painter->restore();
}

/**
 * @brief The selection look of a port node, drawn as PortNodeGui::selectionStyle() says: over
 * the body and under the ports for the looks on the body's edge, under the body for the others.
 **/
class PortNodeSelection
    : public QGraphicsItem {
public:
    explicit PortNodeSelection(PortNodeGui* node)
        : QGraphicsItem(node)
        , _body()
        , _overhang(0.)
        , _ports()
    {
        setData(kPortItemDataSide, QString::fromUtf8(kPortNodeSelectionSide));
    }

    void setGeometry(const QRectF& body,
                     double overhang,
                     const std::vector<QPointF>& ports)
    {
        prepareGeometryChange();
        _body = body;
        _overhang = overhang;
        _ports = ports;
        update();
    }

    virtual QRectF boundingRect() const OVERRIDE FINAL
    {
        const double pen = std::max(selectionPenWidth(), kSelectionMinDevicePixels / kSelectionMinBoundedZoom);
        const double reach = std::max(outsideOffset() + pen / 2., haloWidth()) + 1.;

        return _body.adjusted(-reach, -reach, reach, reach);
    }

    // Decoration only: a click on the outline is not a click on the node.
    virtual QPainterPath shape() const OVERRIDE FINAL
    {
        return QPainterPath();
    }

    virtual void paint(QPainter* painter,
                       const QStyleOptionGraphicsItem* option,
                       QWidget* widget) OVERRIDE FINAL;

private:
    // How far out the outline clearing the ports runs, at the middle of its pen.
    double outsideOffset() const
    {
        return _overhang + selectionGap() + selectionPenWidth() / 2.;
    }

    QRectF _body;
    double _overhang;
    std::vector<QPointF> _ports;
};

void
PortNodeSelection::paint(QPainter* painter,
                         const QStyleOptionGraphicsItem* /*option*/,
                         QWidget* /*widget*/)
{
    const QTransform& t = painter->worldTransform();
    const double scale = std::max(1e-6, std::sqrt(std::abs(t.determinant())));
    const PortSelectionStyleEnum style = PortNodeGui::selectionStyle();
    const double minWidth = kSelectionMinDevicePixels / scale;
    const double width = (style == ePortSelectionStyleHeader) ? std::max((double)TO_DPIX(1), minWidth) : std::max(selectionPenWidth(), minWidth);
    QPen pen(selectionColor());
    pen.setWidthF(width);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    // On the edge, outside the body so that none of its content is covered.
    const QRectF edge = _body.adjusted(-width / 2., -width / 2., width / 2., width / 2.);
    switch (style) {
    case ePortSelectionStyleInset:
    case ePortSelectionStyleHeader:
        painter->drawRect(edge);
        break;
    case ePortSelectionStyleBroken: {
        QPainterPath clip;
        clip.addRect(boundingRect());
        const double cut = _overhang + selectionGap();
        for (const QPointF& port : _ports) {
            QPainterPath hole;
            hole.addEllipse(port, cut, cut);
            clip = clip.subtracted(hole);
        }
        painter->setClipPath(clip, Qt::IntersectClip);
        painter->drawRect(edge);
        break;
    }
    case ePortSelectionStyleHalo: {
        painter->setPen(Qt::NoPen);
        QColor glow = selectionColor();
        glow.setAlpha(kHaloStepAlpha);
        for (int i = kHaloSteps; i > 0; --i) {
            const double d = haloWidth() * i / kHaloSteps;
            QPainterPath ring;
            ring.addRoundedRect(_body.adjusted(-d, -d, d, d), d, d);
            painter->fillPath(ring, glow);
        }
        break;
    }
    case ePortSelectionStyleOutside: {
        const double offset = outsideOffset();
        painter->drawRoundedRect(_body.adjusted(-offset, -offset, offset, offset), offset, offset);
        break;
    }
    }
    painter->restore();
} // PortNodeSelection::paint

/**
 * @brief Draws a port node's body: the header, the target stripes, the port labels and the value
 * summaries. The title is a PortNodeTitle over the header.
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
    const double headerHeight = _node->getHeaderHeight();
    const double rowHeight = PortListLayout::rowHeight();

    painter->fillRect(body, QColor(42, 42, 42));
    const bool headerSelected = _node->getIsSelected() && (PortNodeGui::selectionStyle() == ePortSelectionStyleHeader);
    painter->fillRect(QRectF(body.left(), body.top(), body.width(), std::min(headerHeight, body.height())),
                      headerSelected ? selectionColor() : _node->getHeaderColor());
    if (_node->isMissingPlugin()) {
        const QPen outline = NodeGui::missingPluginOutline();
        const double inset = outline.widthF() / 2.;
        painter->save();
        painter->setPen(outline);
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(body.adjusted(inset, inset, -inset, -inset));
        painter->restore();
    }

    const std::vector<QColor>& stripes = _node->getTargetColors();
    if (!stripes.empty()) {
        const double stripeWidth = body.width() / stripes.size();
        for (std::size_t i = 0; i < stripes.size(); ++i) {
            painter->fillRect(QRectF(body.left() + stripeWidth * i, body.top(), stripeWidth, stripeHeight()), stripes[i]);
        }
    }

    NodePtr node = _node->getNode();
    const double inset = PortNodeGui::portInset();
    if (inset > 0.) {
        // Pipes end at the port centres, under the body: these carry them in from the body's
        // sides.
        const double r = PortListLayout::portRadius();
        const std::vector<PortNodeRow>& inputs = _node->getInputRows();
        painter->save();
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            if (node && node->getInput((int)i)) {
                const double y = body.top() + headerHeight + rowHeight * (inputs[i].row + 0.5);
                painter->setPen(QPen(toQColor(inputs[i].look.fill), kPipeStubWidth));
                painter->drawLine(QPointF(body.left(), y), QPointF(body.left() + inset - r, y));
            }
        }
        const std::vector<PortNodeRow>& outputs = _node->getOutputRows();
        for (std::size_t i = 0; i < outputs.size(); ++i) {
            if (_node->isOutputConnected((int)i)) {
                const double y = body.top() + headerHeight + rowHeight * (i + 0.5);
                painter->setPen(QPen(toQColor(outputs[i].look.fill), kPipeStubWidth));
                painter->drawLine(QPointF(body.right() - inset + r, y), QPointF(body.right(), y));
            }
        }
        painter->restore();
    }

    NodeGraph* graph = _node->getDagGui();
    const QFont font = portFont();
    if ((graph && (graph->isDoingNavigatorRender() || graph->isLowDetail())) || (paintedTextHeight(painter, font) < kTextMinPixelHeight)) {
        return;
    }

    painter->setFont(font);
    painter->setPen(portTextColor());
    const double labelLeft = body.left() + inset + PortListLayout::portRadius() + textPadding();
    const double summaryLeft = labelLeft + _node->getInputLabelWidth() + textPadding();
    const double summaryWidth = _node->getSummaryWidth();
    const std::vector<PortNodeRow>& inputs = _node->getInputRows();
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const double top = body.top() + headerHeight + rowHeight * inputs[i].row;
        const QRectF labelRect(labelLeft, top, summaryLeft - labelLeft, rowHeight);
        painter->drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter, inputs[i].label);
        if (inputs[i].knob && (summaryWidth > 0.) && node && !node->getInput((int)i)) {
            paintSummary(painter, inputs[i], QRectF(summaryLeft, top, summaryWidth, rowHeight));
            painter->setPen(portTextColor());
        }
    }

    const double labelRight = body.right() - inset - PortListLayout::portRadius() - textPadding();
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
    , _title(0)
    , _selection(0)
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
    _title = new PortNodeTitle(this);
    _title->setZValue(getBaseDepth() + 1);
    _selection = new PortNodeSelection(this);
    _selection->setZValue(getBaseDepth() - 1);
    _selection->setVisible(getIsSelected());
    // The header draws the name itself, elided to fit; the label item's multi-line HTML would
    // spill out of the header.
    if (NodeGraphTextItem* label = getNameItem()) {
        label->hide();
    }

    NodePtr node = getNode();
    if (node) {
        QObject::connect(node.get(), &Node::inputChanged, this, [this](int inputNb) {
            updateBody();
            if (inputNb < 0) {
                return;
            }
            if ((std::size_t)inputNb >= _drawnUpstreams.size()) {
                _drawnUpstreams.resize(inputNb + 1);
            }
            NodePtr self = getNode();
            NodePtr upstream = self ? self->getInput(inputNb) : NodePtr();
            for (const NodePtr& changed : { _drawnUpstreams[inputNb].lock(), upstream }) {
                PortNodeGui* gui = changed ? dynamic_cast<PortNodeGui*>(changed->getNodeGui().get()) : nullptr;
                if (gui) {
                    gui->updateBody();
                }
            }
            _drawnUpstreams[inputNb] = upstream;
        });
        if (isMissingPlugin()) {
            // A missing plug-in's outputs are only known from the nodes reading them.
            QObject::connect(node.get(), &Node::outputsChanged, this, [this]() {
                refreshPorts();
                refreshDownstreamPipes();
            });
        }
    }
    refreshPorts();
}

double
PortNodeGui::getHeaderHeight() const
{
    return portLayout()->nodeHeaderHeight();
}

bool
PortNodeGui::isMissingPlugin() const
{
    return portLayout()->isMissingPlugin();
}

void
PortNodeGui::refreshMissingPluginRows()
{
    NodePtr node = getNode();
    if (!node) {
        return;
    }
    for (int i = 0; i < node->getNInputs(); ++i) {
        PortNodeRow row;
        row.name = node->getInputLabel(i);
        row.label = QString::fromUtf8(row.name.c_str());
        row.row = i;
        _inputs.push_back(row);
    }
    std::set<std::string> names;
    for (const NodeWPtr& weakOutput : node->getOutputs()) {
        NodePtr output = weakOutput.lock();
        for (int i = 0; output && (i < output->getNInputs()); ++i) {
            if (output->getInput(i) == node) {
                names.insert(PortListLayout::outputReadBy(output, i));
            }
        }
    }
    // With no reader, one unnamed port still shows where the node's output was.
    if (names.empty()) {
        names.insert(std::string());
    }
    for (const std::string& name : names) {
        PortNodeRow row;
        row.name = name;
        row.label = QString::fromUtf8(name.c_str());
        _outputs.push_back(row);
    }
}

void
PortNodeGui::refreshDownstreamPipes()
{
    NodePtr node = getNode();
    if (!node) {
        return;
    }
    for (const NodeWPtr& weakOutput : node->getOutputs()) {
        NodePtr output = weakOutput.lock();
        NodeGuiPtr gui = output ? std::dynamic_pointer_cast<NodeGui>(output->getNodeGui()) : NodeGuiPtr();
        if (!gui) {
            continue;
        }
        for (Edge* edge : gui->getInputsArrows()) {
            if (edge && (edge->getSource().get() == this)) {
                edge->initLine();
            }
        }
    }
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
        assignInputRows(*network, &_inputs);
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
    } else if (isMissingPlugin()) {
        refreshMissingPluginRows();
    }

    for (std::size_t i = 0; i < _outputs.size(); ++i) {
        _outputs[i].row = (int)i;
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
            PortItem* item = new PortItem(this, row.look);
            item->setZValue(getBaseDepth() + 0.8);
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
    const double portSpace = portInset() + PortListLayout::portRadius() + textPadding();
    double left = portSpace + _inputLabelWidth;
    if (_hasSummaries) {
        left += textPadding() + summaryColumnWidth();
    }
    const double right = _outputLabelWidth + portSpace;
    double width = left + right + columnGap();

    // A long name widens the node up to the widest a node gets, then is elided.
    NodeGraphTextItem* label = getNameItem();
    const double titleWidth = QFontMetricsF(label ? label->font() : portFont()).horizontalAdvance(getTitleText()) + 2 * textPadding();
    width = std::max(width, std::min(titleWidth, (double)TO_DPIX(NODE_MAX_WIDTH)));
    width = std::max(width, (double)TO_DPIX(NODE_WIDTH) * 1.5);

    const std::size_t rows = std::max(_inputs.size(), _outputs.size());
    const double height = getHeaderHeight() + PortListLayout::rowHeight() * rows + (rows ? TO_DPIY(4) : 0);

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
    refreshTitle();
    refreshSelectionGeometry();
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
    if (_title) {
        _title->update();
    }
}

QString
PortNodeGui::getTitleText() const
{
    NodePtr node = getNode();

    return node ? QString::fromUtf8(node->getLabel().c_str()) : QString();
}

void
PortNodeGui::refreshTitle()
{
    if (!_title) {
        return;
    }
    const QRectF body = bodyRect();
    const QRectF header(body.left() + textPadding(), body.top() + stripeHeight(), body.width() - 2 * textPadding(), getHeaderHeight() - stripeHeight());
    NodePtr node = getNode();
    const MissingPluginNode* placeholder = node ? dynamic_cast<const MissingPluginNode*>(node->getEffectInstance().get()) : 0;
    NodeGraphTextItem* label = getNameItem();
    _title->setData(kPortItemDataNode, node ? QString::fromUtf8(node->getFullyQualifiedName().c_str()) : QString());
    _title->setContent(header, label ? label->font() : portFont(), getTitleText(), placeholder ? placeholder->getMissingSummary() : QString());
}

double
PortNodeGui::portOverhang() const
{
    const double r = PortListLayout::portRadius();
    double overhang = r;

    for (int side = 0; side < 2; ++side) {
        for (const PortNodeRow& row : side ? _outputs : _inputs) {
            switch (row.look.shape) {
            case PortShape::Circle:
                break;
            case PortShape::Pill:
                overhang = std::max(overhang, r * 1.6);
                break;
            case PortShape::Diamond:
                overhang = std::max(overhang, r * 1.3);
                break;
            }
            if (row.look.hasRing) {
                overhang = std::max(overhang, r + 1.);
            }
        }
    }

    return overhang;
}

void
PortNodeGui::refreshSelectionGeometry()
{
    if (_selection) {
        NodePtr node = getNode();
        _selection->setData(kPortItemDataNode, node ? QString::fromUtf8(node->getFullyQualifiedName().c_str()) : QString());
        const PortSelectionStyleEnum style = selectionStyle();
        const bool underBody = (style == ePortSelectionStyleHalo) || (style == ePortSelectionStyleOutside);
        _selection->setZValue(getBaseDepth() + (underBody ? -1. : 0.6));
        std::vector<QPointF> ports;
        PortListLayout* layout = portLayout();
        for (std::size_t i = 0; i < _inputs.size(); ++i) {
            ports.push_back(layout->inputPortPosition((int)i));
        }
        for (std::size_t i = 0; i < _outputs.size(); ++i) {
            ports.push_back(layout->outputPortPosition((int)i));
        }
        _selection->setGeometry(bodyRect(), portOverhang(), ports);
    }
}

void
PortNodeGui::selectionLookChanged(bool selected)
{
    if (_selection) {
        _selection->setVisible(selected);
    }
    updateBody();
    if (_title) {
        _title->update();
    }
}

PortSelectionStyleEnum
PortNodeGui::selectionStyle()
{
    const QString value = QString::fromLocal8Bit(qgetenv("NATRON_PORT_SELECTION_STYLE")).trimmed().toLower();
    static const char* const names[] = { "inset", "broken", "header", "halo", "outside" };
    for (int i = 0; i < 5; ++i) {
        if ((value == QString::number(i + 1)) || (value == QLatin1String(names[i]))) {
            return (PortSelectionStyleEnum)(i + 1);
        }
    }

    return ePortSelectionStyleInset;
}

double
PortNodeGui::portInset()
{
    return (selectionStyle() == ePortSelectionStyleInset) ? portInsetWidth() : 0.;
}

bool
PortNodeGui::isOutputConnected(int outputIndex) const
{
    NodePtr node = getNode();
    if (!node) {
        return false;
    }
    for (const NodeWPtr& weakOutput : node->getOutputs()) {
        NodePtr output = weakOutput.lock();
        const int inputCount = output ? output->getNInputs() : 0;
        for (int i = 0; i < inputCount; ++i) {
            if ((output->getInput(i) == node) && (portLayout()->outputIndexOf(PortListLayout::outputReadBy(output, i)) == outputIndex)) {
                return true;
            }
        }
    }

    return false;
}

void
PortNodeGui::updateBody()
{
    if (_body) {
        _body->update();
    }
}

int
PortNodeGui::getInputRow(int inputNb) const
{
    if ((inputNb >= 0) && (inputNb < (int)_inputs.size())) {
        return _inputs[inputNb].row;
    }

    return std::max(0, inputNb);
}

NATRON_NAMESPACE_EXIT
