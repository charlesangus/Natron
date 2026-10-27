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

#ifndef Gui_KnobDriverRow_h
#define Gui_KnobDriverRow_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QDialog>
#include <QUndoCommand>
#include <QWidget>
#include <QtCore/QPointF>
#include <QtCore/QStringList>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/EngineFwd.h"
#include "Engine/KnobDriver.h"
#include "Engine/UndoKnobRef.h"

#include "Gui/GuiFwd.h"

class QPushButton;

NATRON_NAMESPACE_ENTER

/**
 * @brief Sets the drivers of every dimension of a knob, the knob's own value showing again
 * through a dimension given none. Consecutive edits within one spin box gesture merge.
 **/
class SetKnobDriverCommand
    : public QUndoCommand {
public:
    SetKnobDriverCommand(const KnobIPtr& knob,
                         const std::vector<KnobDriverPtr>& drivers,
                         const QString& text,
                         quint64 gesture = 0);

    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;
    virtual int id() const OVERRIDE FINAL;
    virtual bool mergeWith(const QUndoCommand* other) OVERRIDE FINAL;

    /**
     * @brief Pushes the command onto gui's history, or applies it without one.
     **/
    static void push(Gui* gui,
                     const KnobIPtr& knob,
                     const std::vector<KnobDriverPtr>& drivers,
                     const QString& text,
                     quint64 gesture = 0);

private:
    void apply(const std::vector<KnobDriverPtr>& drivers);

    UndoKnobRef _knob;
    std::vector<KnobDriverPtr> _oldDrivers;
    std::vector<KnobDriverPtr> _newDrivers;
    quint64 _gesture;
};

/**
 * @brief What the driver editors share: the loop variables a knob's node may read, its drivers,
 * and its value at the preview iteration of the loops around it.
 **/
namespace LoopDriverEdit {
/**
 * @brief The node holding the knob, null for a knob of no node.
 **/
NodePtr nodeOf(const KnobIPtr& knob);

/**
 * @brief The variables of the loops around node: loop.* of the nearest, then loops.<name>.* of
 * each, nearest first. Empty outside any loop.
 **/
QStringList loopVariables(const NodePtr& node);

bool canDrive(const KnobIPtr& knob);

std::vector<KnobDriverPtr> driversOf(const KnobIPtr& knob);

/**
 * @brief Whether dimension of the knob, or any of them for -1, has a driver.
 **/
bool isDriven(const KnobIPtr& knob,
              int dimension = -1);

/**
 * @brief Whether the driver names a loop that does not enclose the knob's node, so it reads the
 * knob's own value.
 **/
bool isStale(const KnobDriverPtr& driver,
             const QStringList& variables);

/**
 * @brief The knob's value in dimension, at the preview iteration of the loops around its node.
 **/
double previewValue(const KnobIPtr& knob,
                    int dimension);

/**
 * @brief One field of a driver set to a new value, the others kept.
 **/
struct DriverChange {
    enum FieldEnum {
        eFieldVariable = 0,
        eFieldStart,
        eFieldEnd,
        eFieldEasing,
    };

    explicit DriverChange(FieldEnum f)
        : field(f)
    {
    }

    FieldEnum field;
    std::string variable;
    double value = 0.;
    KnobDriver::EasingEnum easing = KnobDriver::eEasingLinear;
    // The easing curve, for eEasingCurve; null keeps the driver's.
    std::shared_ptr<const Curve> curve;
};

KnobDriverPtr applyChange(const KnobDriverPtr& driver,
                          const DriverChange& change);

/**
 * @brief Applies change to the driver of dimension, or of every driven dimension for -1, as one
 * undoable edit; edits within one spin box gesture merge.
 **/
void editDrivers(Gui* gui,
                 const KnobIPtr& knob,
                 int dimension,
                 const DriverChange& change,
                 quint64 gesture);

KnobDriverPtr makeDriver(const std::string& variable,
                         double start,
                         double end,
                         KnobDriver::EasingEnum easing,
                         const Curve* curve);

/**
 * @brief Drives the dimension of the knob, or all of them for -1, from the nearest loop's
 * loop.t, from its default to its current value, after confirming when it has an expression or
 * animation. False if declined or not possible.
 **/
bool driveFromLoop(Gui* gui,
                   const KnobIPtr& knob,
                   int dimension);

void removeLoopDriver(Gui* gui,
                      const KnobIPtr& knob,
                      int dimension);

/**
 * @brief The knob's script name, with the dimension's name for one dimension of several.
 **/
QString knobLabel(const KnobIPtr& knob,
                  int dimension);
} // namespace LoopDriverEdit

/**
 * @brief The easing curve, over [0, 1] on both axes, edited by dragging its points; a double
 * click adds one and a right click removes one. The end points move only vertically.
 **/
class EasingCurveWidget
    : public QWidget {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    explicit EasingCurveWidget(QWidget* parent = 0);

    void setPoints(const std::vector<QPointF>& points);

    const std::vector<QPointF>& getPoints() const
    {
        return _points;
    }

    /**
     * @brief A curve through the points.
     **/
    std::shared_ptr<Curve> makeCurve() const;

    /**
     * @brief The points of curve, or a straight line when it has none.
     **/
    static std::vector<QPointF> pointsOf(const Curve* curve);

    virtual QSize sizeHint() const OVERRIDE FINAL;

Q_SIGNALS:

    void pointsChanged();

protected:
    virtual void paintEvent(QPaintEvent* e) OVERRIDE FINAL;
    virtual void mousePressEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseMoveEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseReleaseEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseDoubleClickEvent(QMouseEvent* e) OVERRIDE FINAL;

private:
    QRectF plotRect() const;
    QPointF toWidget(const QPointF& p) const;
    QPointF toCurve(const QPointF& p) const;
    int pointAt(const QPointF& pos) const;

    std::vector<QPointF> _points;
    int _dragged;
};

class EasingCurveDialog
    : public QDialog {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    EasingCurveDialog(const Curve* curve,
                      QWidget* parent);

    std::shared_ptr<Curve> makeCurve() const;

private:
    EasingCurveWidget* _editor;
};

/**
 * @brief Stands in for a driven knob's widgets on its panel row: the loop variable, the
 * dimensions driven, start and end, the easing and the value at the preview iteration. The
 * knob's warning badge says when the driver's loop no longer encloses the node.
 **/
class KnobDriverRow
    : public QWidget {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    KnobDriverRow(const KnobGuiPtr& knobGui,
                  QWidget* parent);

    virtual ~KnobDriverRow() OVERRIDE;

    bool isDriving() const
    {
        return _driving;
    }

public Q_SLOTS:

    /**
     * @brief Rebuilds the row from the knob's drivers once control returns to the event loop, so
     * that an edit made from one of its widgets never rebuilds that widget under its own signal.
     **/
    void scheduleRefresh();

    void refresh();

    void refreshPreview();

private Q_SLOTS:

    void onVariableChanged(int index);
    void onDimensionChanged(int index);
    void onStartChanged(double value);
    void onEndChanged(double value);
    void onEasingChanged(int index);
    void onEditCurveClicked();
    void onContextMenuRequested(const QPoint& pos);

protected:
    virtual void showEvent(QShowEvent* e) OVERRIDE FINAL;

private:
    // The driver the row shows: that of the dimension it edits, or of the first driven one.
    KnobDriverPtr currentDriver() const;

    void edit(const LoopDriverEdit::DriverChange& change,
              quint64 gesture);

    void connectLoopEnds(const NodePtr& node);

    std::weak_ptr<KnobGui> _knobGui;
    KnobIWPtr _knob;
    bool _driving;
    bool _refreshPending;
    // The dimension the row edits, -1 for all of them; _scopes lists the dimension choice's.
    int _scope;
    std::vector<int> _scopes;
    std::vector<NodeWPtr> _watchedLoopEnds;
    QStringList _variables;
    ComboBox* _variable;
    ComboBox* _dimension;
    SpinBox* _start;
    SpinBox* _end;
    ComboBox* _easing;
    QPushButton* _editCurve;
    Label* _preview;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_KnobDriverRow_h
