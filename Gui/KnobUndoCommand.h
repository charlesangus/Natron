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

#ifndef NATRON_GUI_KNOBUNDOCOMMAND_H
#define NATRON_GUI_KNOBUNDOCOMMAND_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <vector>

#include <QDebug>
#include <QPointer>
CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QUndoCommand>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Global/GlobalDefines.h"

#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/Node.h"
#include "Engine/TimeLine.h"
#include "Engine/UndoKnobRef.h"
#include "Engine/Variant.h"
#include "Engine/ViewIdx.h"

#include "Gui/CurveEditor.h"
#include "Gui/CurveWidget.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiFwd.h"
#include "Gui/KnobGui.h"
#include "Gui/UndoGesture.h"

NATRON_NAMESPACE_ENTER

//================================================================

/**
 * @brief Sets a knob's value. Consecutive commands on the same knob and dimensions fold into
 * one undo step only when they belong to the same gesture (see UndoGesture): the widget that
 * pushes them passes its gesture's key, or leaves it at 0 for a discrete commit.
 **/
template <typename T>
class KnobUndoCommand
    : public QUndoCommand,
      public UndoGestureMember {
    Q_DECLARE_TR_FUNCTIONS(KnobUndoCommand)

public:
    KnobUndoCommand(const KnobGuiPtr& knob,
                    const T& oldValue,
                    const T& newValue,
                    int dimension = 0,
                    bool refreshGui = true,
                    QUndoCommand* parent = 0)
        : QUndoCommand(parent)
        , _dimension(dimension)
        , _oldValue()
        , _newValue()
        , _knob(knob->getKnob())
        , _valueChangedReturnCode(1)
        , _newKeys(1)
        , _oldKeys(1)
        , _merge(true)
        , _refreshGuiFirstTime(refreshGui)
        , _firstRedoCalled(false)
    {
        _oldValue.push_back(oldValue);
        _newValue.push_back(newValue);
    }

    /// std::list rather than std::vector: std::vector<bool> is a bit field (std::_Bit_reference)
    /// and produces an unresolved symbol error.
    ///
    /// The lists cover the knob's first oldValue.size() dimensions, so a caller can leave the
    /// trailing ones untouched. mergeKey is the key of the gesture the edit belongs to, 0 for a
    /// discrete commit that never folds with another command.
    KnobUndoCommand(const KnobGuiPtr& knob,
                    const std::list<T>& oldValue,
                    const std::list<T>& newValue,
                    bool refreshGui = true,
                    quint64 mergeKey = 0,
                    QUndoCommand* parent = 0)
        : QUndoCommand(parent)
        , _dimension(-1)
        , _oldValue(oldValue)
        , _newValue(newValue)
        , _knob(knob->getKnob())
        , _valueChangedReturnCode(oldValue.size())
        , _newKeys(oldValue.size())
        , _oldKeys(oldValue.size())
        , _merge(true)
        , _refreshGuiFirstTime(refreshGui)
        , _firstRedoCalled(false)
    {
        setGestureKey(mergeKey);
    }

    virtual ~KnobUndoCommand() OVERRIDE
    {
    }

    /**
     * @brief When false, the command never folds with another, even one of its own gesture.
     **/
    void setMergeable(bool mergeable)
    {
        _merge = mergeable;
    }

private:
    virtual void undo() OVERRIDE FINAL
    {
        bool modifiedKeyFrame = false;
        int i = 0;
        KnobIPtr knob = _knob.lock();

        if (!knob) {
            setObsolete(true);

            return;
        }

        KnobHolder* holder = knob->getHolder();

        // If the knob has several UI (one in the viewer and one in the settings panel) refreshUI
        bool refreshUI = holder ? holder->isInViewerUIKnob(knob) : false;

        knob->beginChanges();

        assert((int)_oldValue.size() <= knob->getDimension() || _dimension != -1);

        typename std::list<T>::iterator next = _oldValue.end();
        if ( next != _oldValue.end() ) {
            ++next;
        }

        for (typename std::list<T>::iterator it = _oldValue.begin(); it != _oldValue.end(); ++it) {
            int dimension = _dimension == -1 ? i : _dimension;

            if ( ( it == _oldValue.begin() ) && (_oldValue.size() > 1) ) {
                knob->blockValueChanges();
            }

            if ( ( next == _oldValue.end() ) && (_oldValue.size() > 1) ) {
                knob->unblockValueChanges();
            }

            KnobGui::setKnobValue(knob, dimension, *it, NULL, refreshUI, !refreshUI ? eValueChangedReasonUserEdited : eValueChangedReasonNatronGuiEdited);

            if (holder && holder->getApp()) {
                if (_valueChangedReturnCode[i] == KnobHelper::eValueChangedReturnCodeKeyframeAdded) {
                    try {
                        KnobGui::removeKnobKeyframe(knob, _newKeys[i].getTime(), dimension, ViewIdx(0));
                    } catch (std::exception&) {
                        qDebug() << "KnobUndoCommand::undo: could not remove keyframe from knob" << knob->getName().c_str() << "at time" << _newKeys[i].getTime() << "dimension" << dimension;
                        // ignore
                    }
                    modifiedKeyFrame = true;
                } else if (_valueChangedReturnCode[i] == KnobHelper::eValueChangedReturnCodeKeyframeModified) {
                    KnobGui::removeKnobKeyframe(knob, _newKeys[i].getTime(), dimension, ViewIdx(0));
                    KnobGui::setKnobKeyframe(knob, _oldKeys[i].getTime(), dimension, ViewIdx(0));
                    modifiedKeyFrame = true;
                }
            }

            if ( next != _oldValue.end() ) {
                ++next;
            }
            ++i;
        }

        ///This will refresh all dimensions
        KnobGuiPtr knobUI = KnobGui::getLiveGui(knob);
        if (knobUI) {
            knobUI->onInternalValueChanged(ViewSpec::all(), -1, eValueChangedReasonNatronGuiEdited);
        }

        knob->endChanges();
        Gui* gui = KnobGui::getGuiForKnob(knob);
        if (modifiedKeyFrame && gui) {
            gui->getCurveEditor()->getCurveWidget()->refreshSelectedKeysAndUpdate();
        }

        EffectInstance* effect = dynamic_cast<EffectInstance*>(holder);
        QString holderName;
        if (effect) {
            holderName = QString::fromUtf8( effect->getNode()->getLabel().c_str() );
        }

        // A caller-supplied text (a colour gesture's "White: trackball") survives undo and redo.
        if (text().isEmpty()) {
            setText(tr("Set value of %1.%2").arg(holderName).arg(QString::fromUtf8(knob->getLabel().c_str())));
        }
    } // undo

    virtual void redo() OVERRIDE FINAL
    {
        double time = 0;
        KnobIPtr knob = _knob.lock();

        if (!knob) {
            setObsolete(true);

            return;
        }

        KnobHolder* holder = knob->getHolder();

        // If the knob has several UI (one in the viewer and one in the settings panel) refreshUI
        bool refreshUI = holder ? holder->isInViewerUIKnob(knob) : false;

        if ( holder && holder->getApp() ) {
            time = holder->getApp()->getTimeLine()->currentFrame();
        }

        assert((int)_oldValue.size() <= knob->getDimension() || _dimension != -1);

        bool modifiedKeyFrames = false;

        knob->beginChanges();
        int i = 0;
        typename std::list<T>::iterator next = _newValue.end();
        if ( next != _newValue.end() ) {
            ++next;
        }
        for (typename std::list<T>::iterator it = _newValue.begin(); it != _newValue.end(); ++it) {
            int dimension = _dimension == -1 ? i : _dimension;

            if ( ( it == _newValue.begin() ) && (_newValue.size() > 1) ) {
                knob->blockValueChanges();
            }

            CurvePtr c = knob->getCurve(ViewIdx(0), dimension);
            //find out if there's already an existing keyframe before calling setValue
            if (c) {
                bool found = c->getKeyFrameWithTime(time, &_oldKeys[i]);
                Q_UNUSED(found); // we don't care if it existed or not
            }

            if ( ( next == _newValue.end() ) && (_newValue.size() > 1) ) {
                knob->unblockValueChanges();
            }

            _valueChangedReturnCode[i] = KnobGui::setKnobValue(knob, dimension, *it, &_newKeys[i], refreshUI, !refreshUI ? eValueChangedReasonUserEdited : eValueChangedReasonNatronGuiEdited);
            if ((_valueChangedReturnCode[i] == KnobHelper::eValueChangedReturnCodeKeyframeAdded) || (_valueChangedReturnCode[i] == KnobHelper::eValueChangedReturnCodeKeyframeModified)) {
                modifiedKeyFrames = true;
            }

            ///if we added a keyframe, prevent this command to merge with any other command
            if (_valueChangedReturnCode[i] == KnobHelper::eValueChangedReturnCodeKeyframeAdded) {
                _merge = false;
            }
            ++i;
            if ( next != _newValue.end() ) {
                ++next;
            }
        }


        ///This will refresh all dimensions
        if (_firstRedoCalled || _refreshGuiFirstTime) {
            KnobGuiPtr knobUI = KnobGui::getLiveGui(knob);
            if (knobUI) {
                knobUI->onInternalValueChanged(ViewSpec::all(), -1, eValueChangedReasonNatronGuiEdited);
            }
        }


        knob->endChanges();

        Gui* gui = KnobGui::getGuiForKnob(knob);
        if (modifiedKeyFrames && gui) {
            gui->getCurveEditor()->getCurveWidget()->refreshSelectedKeysAndUpdate();
        }

        EffectInstance* effect = dynamic_cast<EffectInstance*>(holder);
        QString holderName;
        if (effect) {
            holderName = QString::fromUtf8( effect->getNode()->getLabel().c_str() );
        }

        // A caller-supplied text (a colour gesture's "White: trackball") survives undo and redo.
        if (text().isEmpty()) {
            setText(tr("Set value of %1.%2").arg(holderName).arg(QString::fromUtf8(knob->getLabel().c_str())));
        }

        _firstRedoCalled = true;
    } // redo

    virtual int id() const OVERRIDE FINAL
    {
        return kKnobUndoChangeCommandCompressionID;
    }

    virtual bool mergeWith(const QUndoCommand *command) OVERRIDE FINAL
    {
        const KnobUndoCommand *knobCommand = dynamic_cast<const KnobUndoCommand *>(command);

        if ( !knobCommand || ( command->id() != id() ) ) {
            return false;
        }

        KnobIPtr knob = knobCommand->_knob.lock();
        if (!knob || (_knob.lock() != knob) || !_merge || !knobCommand->_merge || (_dimension != knobCommand->_dimension)) {
            return false;
        }

        // Two separate edits of the same knob, typed values or two drags, stay two undo steps.
        if (!inSameGestureAs(*knobCommand)) {
            return false;
        }
        if (knobCommand->_newValue.size() < _newValue.size()) {
            return false;
        }

        // A later step of the same gesture may write more dimensions than the first did: the
        // extra ones take their old values and keyframe bookkeeping from that step, which is
        // the one that first wrote them.
        for (std::size_t i = _oldValue.size(); i < knobCommand->_oldValue.size(); ++i) {
            typename std::list<T>::const_iterator it = knobCommand->_oldValue.begin();

            std::advance(it, i);
            _oldValue.push_back(*it);
            _valueChangedReturnCode.push_back(knobCommand->_valueChangedReturnCode[i]);
            _newKeys.push_back(knobCommand->_newKeys[i]);
            _oldKeys.push_back(knobCommand->_oldKeys[i]);
        }
        _newValue = knobCommand->_newValue;
        if (!knobCommand->text().isEmpty()) {
            setText(knobCommand->text());
        }

        return true;
    }

private:
    // TODO: PIMPL
    int _dimension;
    std::list<T> _oldValue;
    std::list<T> _newValue;
    UndoKnobRef _knob;
    std::vector<int> _valueChangedReturnCode;
    std::vector<KeyFrame> _newKeys;
    std::vector<KeyFrame>  _oldKeys;
    bool _merge;
    bool _refreshGuiFirstTime;
    bool _firstRedoCalled;
};

/**
 * @brief This class is used by the internal knob when it wants to group multiple edits into a single undo/redo action.
 * It is not used by the GUI
 **/
class MultipleKnobEditsUndoCommand
    : public QUndoCommand
{
    Q_DECLARE_TR_FUNCTIONS(MultipleKnobEditsUndoCommand)

private:
    struct ValueToSet
    {
        Variant newValue, oldValue;
        int dimension;
        double time;
        bool setKeyFrame;
        int setValueRetCode;
    };

    ///For each knob, the second member points to a clone of the knob before the first redo() call was made
    typedef std::map<UndoKnobRef, std::list<ValueToSet>> ParamsMap;
    ParamsMap knobs;
    bool createNew;
    bool firstRedoCalled;
    ValueChangedReasonEnum _reason;

    // The holder of the first knob that still resolves, or null when none does.
    KnobHolder* liveHolder() const;

    static bool isCurrentFrame(const KnobIPtr& knob, double time);

public:

    /**
     * @brief Make a new command
     * @param createNew If true this command will not merge with a previous same command
     * @param setKeyFrame if true, the command will use setValueAtTime instead of setValue in the redo() command.
     **/
    MultipleKnobEditsUndoCommand(const KnobGuiPtr& knob,
                                 ValueChangedReasonEnum reason,
                                 bool createNew,
                                 bool setKeyFrame,
                                 const Variant & value,
                                 int dimension,
                                 double time);

    virtual ~MultipleKnobEditsUndoCommand();

    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;
    virtual int id() const OVERRIDE FINAL;
    virtual bool mergeWith(const QUndoCommand *command) OVERRIDE FINAL;
    static KnobIPtr createCopyForKnob(const KnobIPtr & originalKnob);
};

/**
 * @brief A dimension of a knob that is linked two-way, and a member of its link group that it can
 * be linked back to after it has left the group.
 **/
struct KnobLinkPartner {
    UndoKnobRef knob;
    int dimension;
    bool nodeLink;
    UndoKnobRef partner;
    int partnerDimension;
    bool partnerNodeLink;
};

/**
 * @brief Appends to partners the partner of each linked dimension of knob, or of the given dimension
 * only, skipping permanent groups. The partner is a member on another holder when the group has
 * one, so that linking back also rejoins the group of a knob linked to knobs of its own node.
 **/
void getKnobLinkPartners(const KnobIPtr& knob, int dimension, std::vector<KnobLinkPartner>* partners);

/**
 * @brief Links each recorded dimension back to its partner, which still holds the group's shared
 * state, and restores both members' node-link flags.
 **/
void restoreKnobLinkPartners(const std::vector<KnobLinkPartner>& partners);

struct PasteUndoCommandPrivate;
class PasteUndoCommand
    : public QUndoCommand
{
    Q_DECLARE_TR_FUNCTIONS(PasteUndoCommand)

private:
    std::unique_ptr<PasteUndoCommandPrivate> _imp;

public:

    PasteUndoCommand(const KnobGuiPtr& knob,
                     KnobClipBoardType type,
                     int fromDimension,
                     int targetDimension,
                     const KnobIPtr& fromKnob);

    virtual ~PasteUndoCommand();

    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;

private:
    // Whether a link paste links the given dimension of the target.
    bool isLinkPasteTarget(int dimension) const;
};

/**
 * @brief Unlinks a knob from its link groups, the knob keeping its current state, or re-links it to
 * them, discarding its own state for theirs. Undoing a re-link gives the knob back its own state.
 **/
class KnobLinkUndoCommand
    : public QUndoCommand {
    Q_DECLARE_TR_FUNCTIONS(KnobLinkUndoCommand)

public:
    KnobLinkUndoCommand(const KnobIPtr& knob,
                        bool relink);

    virtual ~KnobLinkUndoCommand();

    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;

private:
    void setLinked(bool linked);

    UndoKnobRef _knob;
    bool _relink;

    // For a re-link: a copy of the knob before it, and the dimensions it re-links, whose state undo restores.
    KnobIPtr _localState;
    std::vector<int> _relinkedDimensions;
};

class RestoreDefaultsCommand
    : public QUndoCommand
{
    Q_DECLARE_TR_FUNCTIONS(RestoreDefaultsCommand)

public:

    RestoreDefaultsCommand(bool isNodeReset,
                           const std::list<KnobIPtr> & knobs,
                           int targetDim,
                           QUndoCommand *parent = 0);
    virtual void undo();
    virtual void redo();

private:
    // The holder of the first knob that still resolves, or null when none does.
    KnobHolder* liveHolder() const;

    bool _isNodeReset;
    int _targetDim;
    std::list<UndoKnobRef> _knobs;
    std::list<KnobIPtr> _clones;  // owned by this
};

class SetExpressionCommand
    : public QUndoCommand
{
    Q_DECLARE_TR_FUNCTIONS(SetExpressionCommand)

public:

    SetExpressionCommand(const KnobIPtr & knob,
                         bool hasRetVar,
                         int dimension,
                         const std::string& expr,
                         QUndoCommand *parent = 0);
    virtual void undo();
    virtual void redo();

private:
    UndoKnobRef _knob;
    std::vector<std::string> _oldExprs;
    std::vector<bool> _hadRetVar;
    std::string _newExpr;
    bool _hasRetVar;
    int _dimension;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_KNOBUNDOCOMMAND_H
