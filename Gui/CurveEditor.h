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

#ifndef CURVEEDITOR_H
#define CURVEEDITOR_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QWidget>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Global/GlobalDefines.h"
#include "Global/QtCompat.h"

#include "Engine/EngineFwd.h"

#include "Gui/PanelWidget.h"
#include "Gui/CurveSelection.h"
#include "Gui/CurveEditorUndoRedo.h"
#include "Gui/GuiFwd.h"


NATRON_NAMESPACE_ENTER

/**
 * All nodes are tracked in the CurveEditor and they all have a NodeCurveEditorContext.
 * Each node context owns a list of NodeCurveEditorElement which corresponds to the animation
 * for one parameter (knob). You can show/hide the parameter's animation by calling
 * checkVisibleState() which will automatically show/hide the curve from the tree if it has (or hasn't) an animation.
 **/

class NodeCurveEditorElement
    : public QObject
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    NodeCurveEditorElement(QTreeWidget* tree,
                           CurveEditor* curveWidget,
                           const KnobGuiPtr& knob,
                           int dimension,
                           QTreeWidgetItem* item,
                           const CurveGuiPtr& curve);

    NodeCurveEditorElement(QTreeWidget* tree,
                           CurveEditor* curveWidget,
                           const KnobIPtr& internalKnob,
                           int dimension,
                           QTreeWidgetItem* item,
                           const CurveGuiPtr& curve);

    NodeCurveEditorElement()
        : _treeItem()
        , _curve()
        , _curveDisplayed(false)
        , _curveWidget(NULL)
        , _dimensionElements()
        , _knobFolded(false)
        , _hiddenByFold(false)
        , _foldedKnobDimensionZero(false)
    {
    }

    virtual ~NodeCurveEditorElement() OVERRIDE;

    QTreeWidgetItem* getTreeItem() const WARN_UNUSED_RETURN
    {
        return _treeItem;
    }

    CurveGuiPtr  getCurve() const WARN_UNUSED_RETURN
    {
        return _curve;
    }

    bool isCurveVisible() const WARN_UNUSED_RETURN
    {
        return _curveDisplayed;
    }

    void setVisible(bool visible);

    int getDimension() const WARN_UNUSED_RETURN
    {
        return _dimension;
    }

    KnobGuiPtr getKnobGui() const WARN_UNUSED_RETURN
    {
        return _knob.lock();
    }

    KnobIPtr getInternalKnob() const WARN_UNUSED_RETURN;

    void checkVisibleState(bool autoSelectOnShow);

    /**
     * @brief Called on the knob-level element of a multi-dimensional knob with the elements of each of its
     * dimensions, so that it can show a single curve while the knob is folded.
     **/
    void setKnobDimensionElements(const std::vector<NodeCurveEditorElement*>& dimensionElements);

    /**
     * @brief True for dimensions 1..n of a folded knob: they stay hidden whatever their animation.
     **/
    bool isHiddenByFold() const WARN_UNUSED_RETURN
    {
        return _hiddenByFold;
    }

    /**
     * @brief True for the dimension 0 curve of a folded knob, which stands for all of its dimensions.
     **/
    bool isFoldedKnobDimensionZero() const WARN_UNUSED_RETURN
    {
        return _foldedKnobDimensionZero;
    }

public Q_SLOTS:


    /**
     * @brief This is invoked everytimes the knob has a keyframe set or removed, to determine whether we need
     * to keep this element in the tree or not.
     **/
    void checkVisibleState();

    void onExpressionChanged();

    void onDimensionsFoldedChanged(bool folded);

private:
    void applyFoldState(bool folded);

    QTreeWidgetItem* _treeItem;
    CurveGuiPtr _curve;
    bool _curveDisplayed;
    CurveEditor* _curveWidget;
    QTreeWidget* _treeWidget;
    KnobGuiWPtr _knob;
    KnobIWPtr _internalKnob;
    int _dimension;
    std::vector<NodeCurveEditorElement*> _dimensionElements;
    bool _knobFolded;
    bool _hiddenByFold;
    bool _foldedKnobDimensionZero;
};

class NodeCurveEditorContext
    : public QObject
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    typedef std::list<NodeCurveEditorElement* > Elements;

    NodeCurveEditorContext(QTreeWidget *tree,
                           CurveEditor* curveWidget,
                           const NodeGuiPtr &node);

    virtual ~NodeCurveEditorContext() OVERRIDE;

    NodeGuiPtr getNode() const WARN_UNUSED_RETURN
    {
        return _node;
    }

    QTreeWidgetItem* getItem() const
    {
        return _nameItem;
    }

    const Elements & getElements() const WARN_UNUSED_RETURN
    {
        return _nodeElements;
    }

    bool isVisible() const;

    void setVisible(bool visible);

    NodeCurveEditorElement* findElement(CurveGui* curve) const WARN_UNUSED_RETURN;
    NodeCurveEditorElement* findElement(const KnobGuiPtr& knob, int dimension) const WARN_UNUSED_RETURN;
    NodeCurveEditorElement* findElement(QTreeWidgetItem* item) const WARN_UNUSED_RETURN;

public Q_SLOTS:

    void onNameChanged(const QString & name);

private:
    // FIXME: PIMPL
    NodeGuiPtr _node;
    Elements _nodeElements;
    QTreeWidgetItem* _nameItem;
};


class RotoCurveEditorContext;

struct RotoItemEditorContextPrivate;
class RotoItemEditorContext
    : public QObject
{
    Q_OBJECT

public:

    RotoItemEditorContext(QTreeWidget* tree,
                          CurveEditor* widget,
                          const RotoDrawableItemPtr& curve,
                          RotoCurveEditorContext* context);

    virtual ~RotoItemEditorContext();

    QTreeWidgetItem* getItem() const;
    RotoDrawableItemPtr getRotoItem() const;

    QString getName() const;

    RotoCurveContextPtr getContext() const;
    const std::list<NodeCurveEditorElement*>& getElements() const;
    NodeCurveEditorElement* findElement(const KnobGuiPtr& knob, int dimension) const;

    void recursiveSelect(QTreeWidgetItem* cur, bool mustSelect,
                         std::vector<CurveGuiPtr> *curves);

    CurveEditor* getWidget() const;

public Q_SLOTS:

    void onNameChanged(const QString & name);

    void onKeyframeAdded();

    void onKeyframeRemoved();

    void onShapeCloned();

protected:
    /**
     * @brief Selects the control point rows under 'cur', or all of them when 'all' is set,
     * adding their curves. Returns true when 'cur' is one of those rows.
     **/
    virtual bool recursiveSelectPoints(QTreeWidgetItem* /*cur*/,
                                       bool /*all*/,
                                       std::vector<CurveGuiPtr>* /*curves*/) { return false; }

private:

    std::unique_ptr<RotoItemEditorContextPrivate> _imp;
};

struct BezierEditorContextPrivate;
class BezierEditorContext
    : public RotoItemEditorContext
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    BezierEditorContext(QTreeWidget* tree,
                        CurveEditor* widget,
                        const BezierPtr& curve,
                        RotoCurveEditorContext* context);

    virtual ~BezierEditorContext() OVERRIDE;

    /**
     * @brief Adds (or removes) a row per control point holding its x and y curves.
     * Feather points and points of other shapes are ignored.
     **/
    void showControlPoints(const std::list<BezierCPPtr>& points, bool show);

    std::list<BezierCPCurveGuiPtr> getControlPointCurves() const;

public Q_SLOTS:

    void onShapeAboutToClone();

    void onShapeClonedRebindPoints();

    void onControlPointRemoved();

protected:
    virtual bool recursiveSelectPoints(QTreeWidgetItem* cur, bool all, std::vector<CurveGuiPtr>* curves) OVERRIDE FINAL;

private:
    void refreshPointLabels();

    std::unique_ptr<BezierEditorContextPrivate> _imp;
};


struct RotoCurveEditorContextPrivate;
class RotoCurveEditorContext
    : public QObject
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    // The item rows go under 'nodeItem', the host node's row, which this context does not own.
    RotoCurveEditorContext(CurveEditor* widget,
                           QTreeWidget* tree,
                           const NodeGuiPtr& node,
                           QTreeWidgetItem* nodeItem);

    virtual ~RotoCurveEditorContext() OVERRIDE;

    NodeGuiPtr getNode() const WARN_UNUSED_RETURN;

    QTreeWidgetItem* getItem() const;

    void recursiveSelectRoto(QTreeWidgetItem* cur,
                             std::vector<CurveGuiPtr> *curves);

    void setVisible(bool visible);

    const std::list<RotoItemEditorContext*>& getElements() const;
    std::list<NodeCurveEditorElement*> findElement(const KnobGuiPtr& knob, int dimension) const;

public Q_SLOTS:

    void onItemNameChanged(const RotoItemPtr& item);

    void itemInserted(int, int);

    void onItemRemoved(const RotoItemPtr& item, int);

private:

    std::unique_ptr<RotoCurveEditorContextPrivate> _imp;
};

struct CurveEditorPrivate;
class CurveEditor
    : public QWidget
      , public CurveSelection
      , public PanelWidget
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    CurveEditor(Gui* gui,
                TimeLinePtr timeline,
                QWidget* parent = 0);

    virtual ~CurveEditor() OVERRIDE;

    /**
     * @brief Creates a new NodeCurveEditorContext and stores it until the CurveEditor is destroyed.
     **/
    void addNode(NodeGuiPtr node);

    void removeNode(NodeGui* node);

    void setTreeWidgetWidth(int width);

    void centerOn(const std::vector<CurvePtr> & curves);

    std::list<CurveGuiPtr> findCurve(const KnobGuiPtr& knob, int dimension) const WARN_UNUSED_RETURN;

    void hideCurves(const KnobGuiPtr& knob);

    void hideCurve(const KnobGuiPtr& knob, int dimension);

    void showCurves(const KnobGuiPtr& knob);

    void showCurve(const KnobGuiPtr& knob, int dimension);

    CurveWidget* getCurveWidget() const WARN_UNUSED_RETURN;
    virtual void getSelectedCurves(std::vector<CurveGuiPtr>* selection) OVERRIDE FINAL;

    void setSelectedCurve(const CurveGuiPtr& curve);

    CurveGuiPtr getSelectedCurve() const;

    /**
     * @brief True when the curve is dimension 0 of a knob folded to a single value in its properties panel.
     **/
    bool isFoldedKnobDimensionZero(const CurveGui* curve) const WARN_UNUSED_RETURN;

    /**
     * @brief While set, the editor keeps every knob folded or expanded as it is, even when the
     * dimensions of a folded knob stop holding the same curve.
     **/
    void setFoldStateHeld(bool held);

    bool isFoldStateHeld() const WARN_UNUSED_RETURN;

    void refreshCurrentExpression();

    void setSelectedCurveExpression(const QString& expression);

    void onInputEventCalled();

public Q_SLOTS:

    void onFilterTextChanged(const QString& filter);

    void onItemSelectionChanged();

    void onItemDoubleClicked(QTreeWidgetItem* item, int);

    void onExprLineEditFinished();

private:
    virtual void enterEvent(QEnterEvent* e) OVERRIDE FINAL;
    virtual void leaveEvent(QEvent* e) OVERRIDE FINAL;
    virtual void keyPressEvent(QKeyEvent* e) OVERRIDE FINAL;
    virtual void keyReleaseEvent(QKeyEvent* e) OVERRIDE FINAL;


    void recursiveSelect(QTreeWidgetItem* cur, std::vector<CurveGuiPtr> *curves, bool inspectRotos = true);

    std::unique_ptr<CurveEditorPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // CURVEEDITOR_H
