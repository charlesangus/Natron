#include "PortNodeGui.h"

#include <algorithm>

#include <QBrush>
#include <QGraphicsEllipseItem>
#include <QGraphicsSimpleTextItem>
#include <QPainterPath>
#include <QPen>
#include <QTimer>

#include "Engine/Node.h"
#include "Engine/Nodes/Test/PortTestNode.h"
#include "Gui/Edge.h"

NATRON_NAMESPACE_ENTER

namespace {
const double kHeaderH = 22.;
const double kRowH = 16.;
const double kWidth = 170.;
const double kDot = 8.;

PortTestNode* portEffect(const NodePtr& node)
{
    return node ? dynamic_cast<PortTestNode*>(node->getEffectInstance().get()) : 0;
}
}

PortEdge::PortEdge(PortNodeGui* s, int o, PortNodeGui* d, int i, QGraphicsItem* parent)
    : QGraphicsPathItem(parent)
    , src(s)
    , outputIndex(o)
    , dst(d)
    , inputIndex(i)
{
    setPen(QPen(QColor(200, 200, 200), 1.5));
    setZValue(1);
    src->attachedEdges.push_back(this);
    dst->attachedEdges.push_back(this);
    updatePath();
}

void
PortEdge::updatePath()
{
    QPointF a = src->outputAnchor(outputIndex);
    QPointF b = dst->inputAnchor(inputIndex);
    double dx = std::max(40., (b.x() - a.x()) * 0.5);
    QPainterPath p(a);
    p.cubicTo(a + QPointF(dx, 0), b - QPointF(dx, 0), b);
    setPath(p);
}

PortNodeGui::PortNodeGui(QGraphicsItem* parent)
    : NodeGui(parent)
{
}

PortNodeGui::~PortNodeGui()
{
    for (PortEdge* e : attachedEdges) {
        PortNodeGui* other = e->src == this ? e->dst : e->src;
        other->attachedEdges.erase(std::remove(other->attachedEdges.begin(), other->attachedEdges.end(), e),
                                   other->attachedEdges.end());
        delete e;
    }
}

void
PortNodeGui::getInitialSize(int* w, int* h) const
{
    PortTestNode* fx = portEffect(getNode());
    int rows = fx ? (int)std::max(fx->getPorts().size(), fx->layout().outputs.size()) : 1;
    *w = (int)kWidth;
    *h = (int)(kHeaderH + rows * kRowH + 6);
}

QRectF
PortNodeGui::bodyRect() const
{
    double w, h;
    getSize(&w, &h);
    return QRectF(mapFromParent(pos()), QSizeF(w, h));
}

QPointF
PortNodeGui::inputAnchor(int i) const
{
    QRectF r = bodyRect();
    return mapToScene(QPointF(r.left(), r.top() + kHeaderH + (i + 0.5) * kRowH));
}

QPointF
PortNodeGui::outputAnchor(int o) const
{
    QRectF r = bodyRect();
    return mapToScene(QPointF(r.right(), r.top() + kHeaderH + (o + 0.5) * kRowH));
}

int
PortNodeGui::outputIndexOf(const std::string& name) const
{
    for (std::size_t i = 0; i < _outputNames.size(); ++i) {
        if (_outputNames[i] == name) {
            return (int)i;
        }
    }
    return 0;
}

void
PortNodeGui::createGui()
{
    NodeGui::createGui();
    rebuildPorts();
    NodePtr node = getNode();
    if (node) {
        QObject::connect(node.get(), &Node::inputsInitialized, this, [this]() {
            QTimer::singleShot(0, this, [this]() { rebuildPorts(); rebuildEdges(); });
        });
        QObject::connect(node.get(), &Node::inputChanged, this, [this](int) {
            QTimer::singleShot(0, this, [this]() { rebuildEdges(); });
        });
    }
    QTimer::singleShot(0, this, [this]() { rebuildEdges(); });
}

void
PortNodeGui::clearPortItems()
{
    for (QGraphicsItem* it : _portItems) {
        delete it;
    }
    _portItems.clear();
}

void
PortNodeGui::rebuildPorts()
{
    clearPortItems();
    PortTestNode* fx = portEffect(getNode());
    if (!fx) {
        return;
    }
    int w, h;
    getInitialSize(&w, &h);
    setSize(w, h);
    QRectF r = bodyRect();
    std::vector<std::string> ports = fx->getPorts();
    for (std::size_t i = 0; i < ports.size(); ++i) {
        const PortTestAttribute* a = fx->findAttribute(ports[i]);
        double y = r.top() + kHeaderH + i * kRowH;
        QGraphicsEllipseItem* dot = new QGraphicsEllipseItem(r.left() - kDot / 2, y + (kRowH - kDot) / 2, kDot, kDot, this);
        dot->setBrush(a && a->type == "closure" ? QColor(90, 200, 120) : QColor(220, 200, 90));
        dot->setZValue(zValue() + 2);
        QGraphicsSimpleTextItem* t = new QGraphicsSimpleTextItem(QString::fromStdString(getNode()->getInputLabel((int)i)), this);
        t->setPos(r.left() + 8, y + 1);
        t->setZValue(zValue() + 2);
        _portItems.push_back(dot);
        _portItems.push_back(t);
    }
    _outputNames.clear();
    const std::vector<PortTestOutput>& outs = fx->layout().outputs;
    for (std::size_t o = 0; o < outs.size(); ++o) {
        _outputNames.push_back(outs[o].name);
        double y = r.top() + kHeaderH + o * kRowH;
        QGraphicsEllipseItem* dot = new QGraphicsEllipseItem(r.right() - kDot / 2, y + (kRowH - kDot) / 2, kDot, kDot, this);
        dot->setBrush(QColor(120, 170, 230));
        dot->setZValue(zValue() + 2);
        QGraphicsSimpleTextItem* t = new QGraphicsSimpleTextItem(QString::fromStdString(outs[o].name), this);
        t->setPos(r.right() - 10 - t->boundingRect().width(), y + 1);
        t->setZValue(zValue() + 2);
        _portItems.push_back(dot);
        _portItems.push_back(t);
    }
}

void
PortNodeGui::rebuildEdges()
{
    for (PortEdge* e : _inputEdges) {
        e->src->attachedEdges.erase(std::remove(e->src->attachedEdges.begin(), e->src->attachedEdges.end(), e),
                                    e->src->attachedEdges.end());
        attachedEdges.erase(std::remove(attachedEdges.begin(), attachedEdges.end(), e), attachedEdges.end());
        delete e;
    }
    _inputEdges.clear();
    NodePtr node = getNode();
    PortTestNode* fx = portEffect(node);
    if (!fx || !parentItem()) {
        return;
    }
    for (int i = 0; i < node->getNInputs(); ++i) {
        // The positional Edge still exists for selection and dragging; the prototype only draws over it.
        if (Edge* arrow = getInputArrow(i)) {
            arrow->setVisible(false);
        }
        NodePtr up = node->getInput(i);
        if (!up) {
            continue;
        }
        PortNodeGui* upGui = dynamic_cast<PortNodeGui*>(up->getNodeGui().get());
        if (!upGui) {
            continue;
        }
        int o = upGui->outputIndexOf(fx->getOutputForInput(node->getInputLabel(i)));
        _inputEdges.push_back(new PortEdge(upGui, o, this, i, parentItem()));
    }
}

QVariant
PortNodeGui::itemChange(GraphicsItemChange change, const QVariant& value)
{
    QVariant ret = NodeGui::itemChange(change, value);
    if (change == QGraphicsItem::ItemPositionHasChanged) {
        for (PortEdge* e : attachedEdges) {
            e->updatePath();
        }
    }
    return ret;
}

NATRON_NAMESPACE_EXIT
