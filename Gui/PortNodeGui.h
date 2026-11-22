// Scratch prototype, not for landing: copy into Gui/ and apply NodeGraph.patch to try it.
#ifndef NATRON_GUI_PORTNODEGUI_H
#define NATRON_GUI_PORTNODEGUI_H

#include <Python.h>

#include "Global/Macros.h"

#include <vector>

#include <QGraphicsPathItem>

#include "Gui/NodeGui.h"

NATRON_NAMESPACE_ENTER

class PortNodeGui;

// A pipe anchored to one output dot and one port dot rather than to node centres and an angle.
class PortEdge
    : public QGraphicsPathItem
{
public:
    PortEdge(PortNodeGui* src, int outputIndex, PortNodeGui* dst, int inputIndex, QGraphicsItem* parent);

    void updatePath();

    PortNodeGui* src;
    int outputIndex;
    PortNodeGui* dst;
    int inputIndex;
};

class PortNodeGui
    : public NodeGui
{
    Q_OBJECT

public:
    PortNodeGui(QGraphicsItem* parent = 0);
    virtual ~PortNodeGui();

    QPointF inputAnchor(int inputIndex) const;
    QPointF outputAnchor(int outputIndex) const;
    int outputIndexOf(const std::string& outputName) const;

    void rebuildPorts();
    void rebuildEdges();

    std::vector<PortEdge*> attachedEdges;

private:
    virtual void createGui() OVERRIDE FINAL;
    virtual bool canMakePreview() OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual void getInitialSize(int* w, int* h) const OVERRIDE FINAL;
    virtual QVariant itemChange(GraphicsItemChange change, const QVariant& value) OVERRIDE FINAL;

    QRectF bodyRect() const;
    void clearPortItems();

    std::vector<QGraphicsItem*> _portItems;
    std::vector<PortEdge*> _inputEdges;
    std::vector<std::string> _outputNames;
};

NATRON_NAMESPACE_EXIT

#endif
