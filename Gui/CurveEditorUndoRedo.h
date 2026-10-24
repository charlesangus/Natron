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

#ifndef CURVEEDITORUNDOREDO_H
#define CURVEEDITORUNDOREDO_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <memory>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QCoreApplication>
#include <QUndoCommand> // in QtGui on Qt4, in QtWidgets on Qt5
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/Curve.h"
#include "Engine/Transform.h"

#include "Gui/GuiFwd.h"

NATRON_NAMESPACE_ENTER

class SelectedKey
{
public:
    CurveGuiPtr curve;
    KeyFrame key, prevKey, nextKey;
    bool hasPrevious;
    bool hasNext;
    std::pair<double, double> leftTan, rightTan;

    SelectedKey()
        : hasPrevious(false)
        , hasNext(false)
    {
    }

    SelectedKey(const CurveGuiPtr& c,
                const KeyFrame & k,
                const bool hasPrevious,
                const KeyFrame & previous,
                const bool hasNext,
                const KeyFrame & next)
        : curve(c)
        , key(k)
        , prevKey(previous)
        , nextKey(next)
        , hasPrevious(hasPrevious)
        , hasNext(hasNext)
    {
    }

    SelectedKey(const SelectedKey & o)
        : curve(o.curve)
        , key(o.key)
        , prevKey(o.prevKey)
        , nextKey(o.nextKey)
        , hasPrevious(o.hasPrevious)
        , hasNext(o.hasNext)
        , leftTan(o.leftTan)
        , rightTan(o.rightTan)
    {
    }
};

struct SelectedKey_compare_time
{
    bool operator() (const SelectedKey & lhs,
                     const SelectedKey & rhs) const
    {
        return lhs.key.getTime() < rhs.key.getTime();
    }
};

typedef SelectedKeyPtr KeyPtr;
typedef std::map<CurveGuiPtr, std::list<KeyPtr> > SelectedKeys;

/**
 * @brief Base of the commands that edit curves in a curve widget, so that the widget can
 * tell which curves a command edits before pushing it.
 **/
class CurveEditorCommand
    : public QUndoCommand {
public:
    explicit CurveEditorCommand(QUndoCommand* parent = 0)
        : QUndoCommand(parent)
    {
    }

    virtual ~CurveEditorCommand() OVERRIDE
    {
    }

    virtual void getEditedCurves(std::list<CurveGuiPtr>* curves) const = 0;
};

/**
 * @brief Wraps a command that edits the dimension 0 curve of one or more folded knobs, the only
 * curve the curve editor shows for such a knob, and copies that curve into every other dimension
 * so that they keep the same animation. Undo restores the other dimensions as they were before
 * the first redo, then undoes the wrapped command.
 **/
class FoldedCurveEditCommand
    : public QUndoCommand {
public:
    FoldedCurveEditCommand(CurveEditor* editor,
                           QUndoCommand* inner,
                           const std::list<CurveGuiPtr>& foldedDimensionZeroCurves,
                           QUndoCommand* parent = 0);

    virtual ~FoldedCurveEditCommand() OVERRIDE;

private:
    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;
    virtual int id() const OVERRIDE FINAL;
    virtual bool mergeWith(const QUndoCommand* command) OVERRIDE FINAL;

    void copyDimensionZeroToOthers();
    void restoreOtherDimensions();

    struct FoldedKnob {
        KnobIPtr knob;
        CurveGuiPtr dimensionZeroCurve;

        // Indexed by dimension; entry 0 stays null.
        std::vector<CurvePtr> curvesBeforeFirstRedo;
    };

    CurveEditor* _editor;
    std::unique_ptr<QUndoCommand> _inner;
    std::list<FoldedKnob> _knobs;
    std::list<KnobGuiWPtr> _knobGuis;
    bool _snapshotsTaken;
};

//////////////////////////////ADD MULTIPLE KEYS COMMAND//////////////////////////////////////////////

class AddKeysCommand
    : public CurveEditorCommand {
    Q_DECLARE_TR_FUNCTIONS(AddKeysCommand)

public:

    struct KeyToAdd
    {
        CurveGuiPtr curveUI;
        KnobGuiWPtr knobUI;
        int dimension;
        std::vector<KeyFrame> keyframes;
    };

    typedef std::list<KeyToAdd > KeysToAddList;

    AddKeysCommand(CurveWidget *editor,
                   const KeysToAddList & keys,
                   QUndoCommand *parent = 0);

    AddKeysCommand(CurveWidget *editor,
                   const CurveGuiPtr& curve,
                   const std::vector<KeyFrame> & keys,
                   QUndoCommand *parent = 0);

    virtual ~AddKeysCommand() OVERRIDE
    {
    }

    virtual void getEditedCurves(std::list<CurveGuiPtr>* curves) const OVERRIDE FINAL;

protected:

    void addOrRemoveKeyframe(bool isSetKeyCommand, bool add);


    virtual void undo() OVERRIDE;
    virtual void redo() OVERRIDE;

private:
    KeysToAddList _keys;
    CurveWidget *_curveWidget;
};

class SetKeysCommand
    : public AddKeysCommand
{
public:


    SetKeysCommand(CurveWidget *editor,
                   const AddKeysCommand::KeysToAddList & keys,
                   QUndoCommand *parent = 0);

    SetKeysCommand(CurveWidget *editor,
                   const CurveGuiPtr& curve,
                   const std::vector<KeyFrame> & keys,
                   QUndoCommand *parent = 0);

    virtual ~SetKeysCommand() OVERRIDE
    {
    }

private:
    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;

private:
    CurveGuiPtr _guiCurve;
    CurvePtr _oldCurve;
};

//////////////////////////////REMOVE  MULTIPLE KEYS COMMAND//////////////////////////////////////////////

class RemoveKeysCommand
    : public CurveEditorCommand {
    Q_DECLARE_TR_FUNCTIONS(RemoveKeysCommand)

public:
    RemoveKeysCommand(CurveWidget* editor,
                      const std::map<CurveGuiPtr, std::vector<KeyFrame> > & curveEditorElement
                      ,
                      QUndoCommand *parent = 0);
    virtual ~RemoveKeysCommand() OVERRIDE
    {
    }

    virtual void getEditedCurves(std::list<CurveGuiPtr>* curves) const OVERRIDE FINAL;

private:
    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;

    void addOrRemoveKeyframe(bool add);

private:
    std::map<CurveGuiPtr, std::vector<KeyFrame> > _keys;
    CurveWidget* _curveWidget;
};

//////////////////////////////MOVE KEY COMMAND//////////////////////////////////////////////

class MoveKeysCommand
    : public CurveEditorCommand {
    Q_DECLARE_TR_FUNCTIONS(MoveKeysCommand)

public:

    struct KeyToMove
    {
        KeyPtr key;
        bool prevIsSelected, nextIsSelected;
    };

    MoveKeysCommand(CurveWidget* widget,
                    const std::map<CurveGuiPtr, std::vector<MoveKeysCommand::KeyToMove> > & keys,
                    double dt,
                    double dv,
                    bool updateOnFirstRedo,
                    QUndoCommand *parent = 0);
    virtual ~MoveKeysCommand() OVERRIDE
    {
    }

    virtual void getEditedCurves(std::list<CurveGuiPtr>* curves) const OVERRIDE FINAL;

private:
    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;
    virtual int id() const OVERRIDE FINAL;
    virtual bool mergeWith(const QUndoCommand * command) OVERRIDE FINAL;

    void move(double dt, double dv);

private:
    bool _firstRedoCalled;
    bool _updateOnFirstRedo;
    double _dt;
    double _dv;
    std::map<CurveGuiPtr, std::vector<MoveKeysCommand::KeyToMove> > _keys;
    CurveWidget* _widget;
};

//////////////////////////////SET MULTIPLE KEYS INTERPOLATION COMMAND//////////////////////////////////////////////

struct KeyInterpolationChange
{
    KeyframeTypeEnum oldInterp;
    KeyframeTypeEnum newInterp;
    KeyPtr key;


    KeyInterpolationChange(KeyframeTypeEnum oldType,
                           KeyframeTypeEnum newType,
                           const KeyPtr & k)
        : oldInterp(oldType)
        , newInterp(newType)
        , key(k)
    {
    }
};

class SetKeysInterpolationCommand
    : public CurveEditorCommand {
    Q_DECLARE_TR_FUNCTIONS(SetKeysInterpolationCommand)

public:

    SetKeysInterpolationCommand(CurveWidget* widget,
                                const std::list<KeyInterpolationChange > & keys,
                                QUndoCommand *parent = 0);
    virtual ~SetKeysInterpolationCommand() OVERRIDE
    {
    }

    virtual void getEditedCurves(std::list<CurveGuiPtr>* curves) const OVERRIDE FINAL;

private:
    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;


    void setNewInterpolation(bool undo);

private:
    std::list<KeyInterpolationChange > _keys;
    CurveWidget* _widget;
};

class MoveTangentCommand
    : public CurveEditorCommand {
    Q_DECLARE_TR_FUNCTIONS(MoveTangentCommand)

public:

    enum SelectedTangentEnum
    {
        eSelectedTangentLeft = 0,
        eSelectedTangentRight
    };

    /**
     * @brief A drag of one handle to (dx, dy) from its key. On a handle that reaches into a
     * segment the drag sets both the slope and the weight, |dx| as a fraction of the segment;
     * an unbroken key's other handle takes the same slope and scales its weight by the same
     * factor. A handle with no segment beyond it sets the slope only.
     **/
    MoveTangentCommand(CurveWidget* widget,
                       SelectedTangentEnum deriv,
                       const KeyPtr& key,
                       double dx,
                       double dy,
                       bool updateOnFirstRedo,
                       bool forceBreak = false,
                       QUndoCommand* parent = 0);

    MoveTangentCommand(CurveWidget* widget,
                       SelectedTangentEnum deriv,
                       const KeyPtr& key,
                       double derivative,
                       QUndoCommand *parent = 0);

    virtual ~MoveTangentCommand() OVERRIDE
    {
    }

    virtual void getEditedCurves(std::list<CurveGuiPtr>* curves) const OVERRIDE FINAL;

private:

    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;
    virtual int id() const OVERRIDE FINAL;
    virtual bool mergeWith(const QUndoCommand * command) OVERRIDE FINAL;

    void setNewDerivatives(bool undo);

private:

    CurveWidget* _widget;
    KeyPtr _key;
    SelectedTangentEnum _deriv;
    KeyframeTypeEnum _oldInterp, _newInterp;
    double _oldLeft, _oldRight, _newLeft, _newRight;
    double _oldLeftWeight, _oldRightWeight, _newLeftWeight, _newRightWeight;
    bool _setBoth;
    bool _updateOnFirstRedo;
    bool _firstRedoCalled;
};

// The slope of the mean of the two tangent angles, so joining a 45-degree rise with a
// flat run gives a 22.5-degree slope rather than splitting the difference in run/rise space.
double joinedDerivative(double left, double right);

class JoinTangentsCommand
    : public CurveEditorCommand {
    Q_DECLARE_TR_FUNCTIONS(JoinTangentsCommand)

public:
    JoinTangentsCommand(CurveWidget* widget,
                        const SelectedKeys& keys,
                        QUndoCommand* parent = 0);

    virtual ~JoinTangentsCommand() OVERRIDE
    {
    }

    virtual void getEditedCurves(std::list<CurveGuiPtr>* curves) const OVERRIDE FINAL;

private:
    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;

    void setNewDerivatives(bool undo);

private:
    struct JoinedTangentChange {
        KeyPtr key;
        KeyframeTypeEnum oldInterp;
        double oldLeft;
        double oldRight;
        double joined;
    };

    std::list<JoinedTangentChange> _keys;
    CurveWidget* _widget;
};

class TransformKeysCommand
    : public CurveEditorCommand {
public:

    TransformKeysCommand(CurveWidget* widget,
                         const SelectedKeys & keys,
                         double centerX,
                         double centerY,
                         double tx,
                         double ty,
                         double sx,
                         double sy,
                         bool updateOnFirstRedo,
                         QUndoCommand *parent = 0);
    virtual ~TransformKeysCommand();

    virtual void getEditedCurves(std::list<CurveGuiPtr>* curves) const OVERRIDE FINAL;

private:
    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;
    virtual int id() const OVERRIDE FINAL;
    virtual bool mergeWith(const QUndoCommand * command) OVERRIDE FINAL;


    void transformKeys(const Transform::Matrix3x3& matrix);

private:
    bool _firstRedoCalled;
    bool _updateOnFirstRedo;
    SelectedKeys _keys;
    CurveWidget* _widget;
    Transform::Matrix3x3 _matrix, _invMatrix;
};

NATRON_NAMESPACE_EXIT

#endif // CURVEEDITORUNDOREDO_H
