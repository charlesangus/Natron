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


#include "KnobGui.h"

#include <cassert>
#include <stdexcept>

#include <QDebug>
#include <QStringList>

#include "Engine/Knob.h"
#include "Engine/KnobLinkGroup.h"
#include "Engine/KnobTypes.h"
#include "Engine/Project.h"
#include "Engine/Utils.h" // convertFromPlainText
#include "Engine/ViewIdx.h"

#include "Gui/KnobGuiPrivate.h"
#include "Gui/KnobUndoCommand.h" // SetExpressionCommand...

NATRON_NAMESPACE_ENTER


void
KnobGui::onCreateAliasOnGroupActionTriggered()
{
    KnobHolder* holder = getKnob()->getHolder();
    EffectInstance* isEffect = dynamic_cast<EffectInstance*>(holder);

    assert(isEffect);
    if (!isEffect) {
        throw std::logic_error("");
    }
    NodeCollectionPtr collec = isEffect->getNode()->getGroup();
    NodeGroup* isCollecGroup = dynamic_cast<NodeGroup*>( collec.get() );
    assert(isCollecGroup);
    if (isCollecGroup) {
        createDuplicateOnNode(isCollecGroup, true, KnobPagePtr(), KnobGroupPtr(), -1);
    }
}

void
KnobGui::onUnlinkActionTriggered()
{
    QAction* action = qobject_cast<QAction*>(sender());

    if (!action) {
        return;
    }
    int dim = action->data().toInt();
    KnobIPtr thisKnob = getKnob();
    int dims = thisKnob->getDimension();
    thisKnob->beginChanges();
    for (int i = 0; i < dims; ++i) {
        if ((dim == -1) || (i == dim)) {
            std::pair<int, KnobIPtr> other = thisKnob->getMaster(i);
            thisKnob->onKnobUnSlaved(i);
            onKnobSlavedChanged(i, false);
        }
    }
    thisKnob->endChanges();
    getKnob()->getHolder()->getApp()->triggerAutoSave();
}

/**
 * @brief The members of a non-permanent link group of the knob's dimension other than that dimension, on
 * active nodes.
 **/
static std::vector<KnobLinkGroup::Member>
getLinkPartners(const KnobIPtr& knob,
                int dimension)
{
    std::vector<KnobLinkGroup::Member> ret;
    KnobLinkGroupPtr group = knob->getLinkGroup(dimension);

    if (!group || group->isPermanent()) {
        return ret;
    }
    std::vector<KnobLinkGroup::Member> members = group->getMembers();
    for (std::vector<KnobLinkGroup::Member>::const_iterator it = members.begin(); it != members.end(); ++it) {
        KnobIPtr member = it->knob.lock();
        if (!member || ((member == knob) && (it->dimension == dimension))) {
            continue;
        }
        EffectInstance* effect = dynamic_cast<EffectInstance*>(member->getHolder());
        if (effect && !effect->getNode()->isActivated()) {
            continue;
        }
        ret.push_back(*it);
    }

    return ret;
}

/**
 * @brief "Node.knob", followed by ".dimension" when withDimension is set and the knob has several.
 **/
static QString
getLinkMemberName(const KnobLinkGroup::Member& member,
                  bool withDimension)
{
    KnobIPtr knob = member.knob.lock();
    QString name;

    if (!knob) {
        return name;
    }
    EffectInstance* effect = dynamic_cast<EffectInstance*>(knob->getHolder());
    if (effect) {
        name += QString::fromUtf8(effect->getNode()->getFullyQualifiedName().c_str());
        name += QLatin1Char('.');
    } else if (dynamic_cast<Project*>(knob->getHolder())) {
        name += QString::fromUtf8("Project.");
    }
    name += QString::fromUtf8(knob->getName().c_str());
    if (withDimension && (knob->getDimension() > 1)) {
        name += QLatin1Char('.');
        name += QString::fromUtf8(knob->getDimensionName(member.dimension).c_str());
    }

    return name;
}

/**
 * @brief The names of the members, without repeats, the unlinked ones marked.
 **/
static QString
joinLinkMemberNames(const std::vector<KnobLinkGroup::Member>& members,
                    bool withDimension)
{
    QStringList names;

    for (std::vector<KnobLinkGroup::Member>::const_iterator it = members.begin(); it != members.end(); ++it) {
        QString name = getLinkMemberName(*it, withDimension);
        if (name.isEmpty()) {
            continue;
        }
        if (!it->linked) {
            name += QLatin1Char(' ');
            name += KnobGui::tr("(unlinked)");
        }
        if (!names.contains(name)) {
            names.push_back(name);
        }
    }

    return names.join(QString::fromUtf8(", ")).toHtmlEscaped();
}

bool
KnobGui::hasLinkPartners() const
{
    KnobIPtr knob = getKnob();

    if (!knob || knob->getAliasMaster()) {
        return false;
    }
    for (int i = 0; i < knob->getDimension(); ++i) {
        if (!getLinkPartners(knob, i).empty()) {
            return true;
        }
    }

    return false;
}

bool
KnobGui::isLinkedToPartners() const
{
    KnobIPtr knob = getKnob();

    if (!knob || knob->getAliasMaster()) {
        return false;
    }
    for (int i = 0; i < knob->getDimension(); ++i) {
        if (knob->isLinked(i) && !knob->isLinkPermanent(i) && !getLinkPartners(knob, i).empty()) {
            return true;
        }
    }

    return false;
}

/**
 * @brief True if every dimension has partners and they are all the same knobs.
 **/
static bool
sameLinkPartnerNames(const std::vector<std::vector<KnobLinkGroup::Member>>& partners)
{
    for (std::size_t i = 0; i < partners.size(); ++i) {
        if (partners[i].empty() || (joinLinkMemberNames(partners[i], false) != joinLinkMemberNames(partners[0], false))) {
            return false;
        }
    }

    return !partners.empty();
}

/**
 * @brief The tool tip lines about the knob's link groups, as HTML, or an empty string when it has no
 * link partner. A knob linked in some dimensions only gets one line per dimension.
 **/
static QString
getLinkToolTip(const KnobIPtr& knob)
{
    if (!knob || knob->getAliasMaster()) {
        return QString();
    }
    const int nDims = knob->getDimension();
    std::vector<std::vector<KnobLinkGroup::Member>> partners(nDims);
    int nLinked = 0;
    int nUnlinked = 0;
    for (int i = 0; i < nDims; ++i) {
        partners[i] = getLinkPartners(knob, i);
        if (partners[i].empty()) {
            continue;
        }
        if (knob->isLinked(i) && !knob->isLinkPermanent(i)) {
            ++nLinked;
        } else {
            ++nUnlinked;
        }
    }
    if ((nLinked == 0) && (nUnlinked == 0)) {
        return QString();
    }

    QString ret;
    if (nLinked == nDims) {
        std::vector<KnobLinkGroup::Member> all;
        for (int i = 0; i < nDims; ++i) {
            all.insert(all.end(), partners[i].begin(), partners[i].end());
        }
        ret = KnobGui::tr("Linked to %1.").arg(joinLinkMemberNames(all, false));
    } else if ((nUnlinked == nDims) && sameLinkPartnerNames(partners)) {
        ret = KnobGui::tr("Unlinked from %1.").arg(joinLinkMemberNames(partners[0], false));
    } else {
        ret = (nLinked > 0) ? KnobGui::tr("Partially linked:") : KnobGui::tr("Unlinked:");
        for (int i = 0; i < nDims; ++i) {
            const QString dimName = QString::fromUtf8(knob->getDimensionName(i).c_str()).toHtmlEscaped();
            QString line;
            if (partners[i].empty()) {
                line = KnobGui::tr("%1 is not linked").arg(dimName);
            } else if (knob->isLinked(i) && !knob->isLinkPermanent(i)) {
                line = KnobGui::tr("%1 is linked to %2").arg(dimName, joinLinkMemberNames(partners[i], true));
            } else {
                line = KnobGui::tr("%1 is unlinked from %2").arg(dimName, joinLinkMemberNames(partners[i], true));
            }
            ret += QString::fromUtf8("<br />");
            ret += line;
        }
    }
    ret += QString::fromUtf8("<br />");
    if (nLinked > 0) {
        ret += KnobGui::tr("Right-click and choose Unlink to edit it on its own.");
    } else {
        ret += KnobGui::tr("Right-click and choose Re-link to take the linked value again.");
    }

    return ret;
} // getLinkToolTip

void
KnobGui::onLinkStateChanged()
{
    if (_imp->guiRemoved || _imp->isInViewerUIKnob) {
        return;
    }
    if (_imp->descriptionLabel) {
        _imp->descriptionLabel->setLinked(isLinkedToPartners());
        _imp->descriptionLabel->setToolTip(toolTip());
    }
    if (_imp->widgetCreated) {
        updateToolTip();
    }
} // KnobGui::onLinkStateChanged

void
KnobGui::onUnlinkFromGroupsActionTriggered()
{
    KnobIPtr knob = getKnob();

    if (knob) {
        pushUndoCommand(new KnobLinkUndoCommand(knob, false));
    }
}

void
KnobGui::onRelinkToGroupsActionTriggered()
{
    KnobIPtr knob = getKnob();

    if (!knob || !knob->hasRelinkableGroups()) {
        return;
    }
    StandardButtonEnum reply = Dialogs::questionDialog(tr("Re-link").toStdString(),
                                                       tr("Re-linking will discard this knob's local values and use the linked value.").toStdString(),
                                                       false,
                                                       StandardButtons(eStandardButtonOk | eStandardButtonCancel),
                                                       eStandardButtonOk);
    if (reply != eStandardButtonOk) {
        return;
    }
    pushUndoCommand(new KnobLinkUndoCommand(knob, true));
}

void
KnobGui::onSetExprActionTriggered()
{
    QAction* action = qobject_cast<QAction*>( sender() );

    assert(action);

    int dim = action->data().toInt();
    EditExpressionDialog* dialog = new EditExpressionDialog(getGui(), dim, shared_from_this(), _imp->field);
    dialog->create(QString::fromUtf8( getKnob()->getExpression(dim == -1 ? 0 : dim).c_str() ), true);
    QObject::connect( dialog, SIGNAL(accepted()), this, SLOT(onEditExprDialogFinished()) );
    QObject::connect( dialog, SIGNAL(rejected()), this, SLOT(onEditExprDialogFinished()) );

    dialog->show();
}

void
KnobGui::onClearExprActionTriggered()
{
    QAction* act = qobject_cast<QAction*>( sender() );

    assert(act);
    int dim = act->data().toInt();
    pushUndoCommand( new SetExpressionCommand(getKnob(), false, dim, "") );
}

void
KnobGui::onEditExprDialogFinished()
{
    EditExpressionDialog* dialog = dynamic_cast<EditExpressionDialog*>( sender() );

    if (dialog) {
        QDialog::DialogCode ret = (QDialog::DialogCode)dialog->result();

        switch (ret) {
        case QDialog::Accepted: {
            bool hasRetVar;
            QString expr = dialog->getExpression(&hasRetVar);
            std::string stdExpr = expr.toStdString();
            int dim = dialog->getDimension();
            std::string existingExpr = getKnob()->getExpression(dim);
            if (existingExpr != stdExpr) {
                pushUndoCommand( new SetExpressionCommand(getKnob(), hasRetVar, dim, stdExpr) );
            }
            break;
        }
        case QDialog::Rejected:
            break;
        }

        dialog->deleteLater();
    }
}

void
KnobGui::setSecret()
{
    bool showit = !isSecretRecursive();

    if (showit) {
        show(); //
    } else {
        hide();
    }
    KnobGuiGroup* isGrp = dynamic_cast<KnobGuiGroup*>(this);
    if (isGrp) {
        const std::list<KnobGuiWPtr>& children = isGrp->getChildren();
        for (std::list<KnobGuiWPtr>::const_iterator it = children.begin(); it != children.end(); ++it) {
            KnobGuiPtr k = it->lock();
            if (!k) {
                continue;
            }
            k->setSecret();
        }
    }
}

void
KnobGui::onViewerContextSecretChanged()
{
    setSecret();
}

bool
KnobGui::isSecretRecursive() const
{
    // If the Knob is within a group, only show it if the group is unfolded!
    // To test it:
    // try TuttlePinning: fold all groups, then switch from perspective to affine to perspective.
    //  VISIBILITY is different from SECRETNESS. The code considers that both things are equivalent, which is wrong.
    // Of course, this check has to be *recursive* (in case the group is within a folded group)
    KnobIPtr knob = getKnob();
    bool isViewerKnob = _imp->container->isInViewerUIKnob();
    bool showit = isViewerKnob ? !knob->getInViewerContextSecret() : !knob->getIsSecret();
    KnobIPtr parentKnob = knob->getParentKnob();
    KnobGroup* parentIsGroup = dynamic_cast<KnobGroup*>( parentKnob.get() );

    while (showit && parentKnob && parentIsGroup) {
        KnobGuiGroup* parentGui = dynamic_cast<KnobGuiGroup*>( _imp->container->getKnobGui(parentKnob).get() );
        // check for secretness and visibility of the group
        bool parentSecret = isViewerKnob ? parentKnob->getInViewerContextSecret() : parentKnob->getIsSecret();
        if ( parentSecret || ( parentGui && !parentGui->isChecked() ) ) {
            showit = false; // one of the including groups is folder, so this item is hidden
        }
        // prepare for next loop iteration
        parentKnob = parentKnob->getParentKnob();
        parentIsGroup = dynamic_cast<KnobGroup*>( parentKnob.get() );
    }

    return !showit;
}

void
KnobGui::onShowInCurveEditorActionTriggered()
{
    KnobIPtr knob = getKnob();

    assert( knob->getHolder()->getApp() );
    getGui()->setCurveEditorOnTop();
    std::vector<CurvePtr> curves;
    for (int i = 0; i < knob->getDimension(); ++i) {
        CurvePtr c = getCurve(ViewIdx(0), i);
        if ( c->isAnimated() ) {
            curves.push_back(c);
        }
    }
    if ( !curves.empty() ) {
        getGui()->getCurveEditor()->centerOn(curves);
    }
}

void
KnobGui::onRemoveAnimationActionTriggered()
{
    QAction* action = qobject_cast<QAction*>( sender() );

    assert(action);
    int dim = action->data().toInt();
    KnobIPtr knob = getKnob();
    std::map<CurveGuiPtr, std::vector<KeyFrame > > toRemove;
    KnobGuiPtr thisShared = shared_from_this();
    for (int i = 0; i < knob->getDimension(); ++i) {
        if ( (dim == -1) || (dim == i) ) {
            std::list<CurveGuiPtr> curves = getGui()->getCurveEditor()->findCurve(thisShared, i);
            for (std::list<CurveGuiPtr>::iterator it = curves.begin(); it != curves.end(); ++it) {
                KeyFrameSet keys = (*it)->getInternalCurve()->getKeyFrames_mt_safe();
                std::vector<KeyFrame > vect;
                for (KeyFrameSet::const_iterator it2 = keys.begin(); it2 != keys.end(); ++it2) {
                    vect.push_back(*it2);
                }
                toRemove.insert( std::make_pair(*it, vect) );
            }
        }
    }
    pushUndoCommand( new RemoveKeysCommand(getGui()->getCurveEditor()->getCurveWidget(),
                                           toRemove) );
    //refresh the gui so it doesn't indicate the parameter is animated anymore
    updateGUI(dim);
}

void
KnobGui::setInterpolationForDimensions(QAction* action,
                                       KeyframeTypeEnum interp)
{
    if (!action) {
        return;
    }
    int dimension = action->data().toInt();
    KnobIPtr knob = getKnob();


    for (int i = 0; i < knob->getDimension(); ++i) {
        if ( (dimension == -1) || (dimension == i) ) {
            CurvePtr c = knob->getCurve(ViewIdx(0), i);
            if (c) {
                int kfCount = c->getKeyFramesCount();
                for (int j = 0; j < kfCount; ++j) {
                    c->setKeyFrameInterpolation(interp, j);
                }
                CurvePtr guiCurve = getCurve(ViewIdx(0), i);
                if (guiCurve) {
                    guiCurve->clone(*c);
                }
            }
        }
    }
    if ( knob->getHolder() ) {
        knob->getHolder()->incrHashAndEvaluate(knob->getEvaluateOnChange(), false);
    }
    Q_EMIT keyInterpolationChanged();
}

void
KnobGui::onConstantInterpActionTriggered()
{
    setInterpolationForDimensions(qobject_cast<QAction*>( sender() ), eKeyframeTypeConstant);
}

void
KnobGui::onLinearInterpActionTriggered()
{
    setInterpolationForDimensions(qobject_cast<QAction*>( sender() ), eKeyframeTypeLinear);
}

void
KnobGui::onSmoothInterpActionTriggered()
{
    setInterpolationForDimensions(qobject_cast<QAction*>( sender() ), eKeyframeTypeSmooth);
}

void
KnobGui::onCatmullromInterpActionTriggered()
{
    setInterpolationForDimensions(qobject_cast<QAction*>( sender() ), eKeyframeTypeCatmullRom);
}

void
KnobGui::onCubicInterpActionTriggered()
{
    setInterpolationForDimensions(qobject_cast<QAction*>( sender() ), eKeyframeTypeCubic);
}

void
KnobGui::onHorizontalInterpActionTriggered()
{
    setInterpolationForDimensions(qobject_cast<QAction*>( sender() ), eKeyframeTypeHorizontal);
}

void
KnobGui::setKnobKeyframe(const KnobIPtr& knob,
                         double time,
                         int dimension,
                         ViewSpec view)
{
    assert( knob->getHolder()->getApp() );

    bool keyAdded = knob->onKeyFrameSet(time, view, dimension);
    KnobGuiPtr gui = getLiveGui(knob);
    if (gui) {
        Q_EMIT gui->keyFrameSet();
    }

    if ( !knob->getIsSecret() && keyAdded && knob->isDeclaredByPlugin() ) {
        knob->getHolder()->getApp()->addKeyframeIndicator(time);
    }
}

void
KnobGui::setKnobKeyframe(const KnobIPtr& knob,
                         double time,
                         const KeyFrame& key,
                         int dimension,
                         ViewSpec view)
{
    assert( knob->getHolder()->getApp() );

    bool keyAdded = knob->onKeyFrameSet(time, view, key, dimension);
    KnobGuiPtr gui = getLiveGui(knob);
    if (gui) {
        Q_EMIT gui->keyFrameSet();
    }
    if ( !knob->getIsSecret() && keyAdded && knob->isDeclaredByPlugin() ) {
        knob->getHolder()->getApp()->addKeyframeIndicator(time);
    }
}

void
KnobGui::onSetKeyActionTriggered()
{
    QAction* action = qobject_cast<QAction*>( sender() );

    assert(action);
    int dim = action->data().toInt();
    KnobIPtr knob = getKnob();

    assert( knob->getHolder()->getApp() );
    //get the current time on the global timeline
    SequenceTime time = knob->getHolder()->getApp()->getTimeLine()->currentFrame();
    AddKeysCommand::KeysToAddList toAdd;
    KnobGuiPtr thisShared = shared_from_this();
    for (int i = 0; i < knob->getDimension(); ++i) {
        if ( (dim == -1) || (i == dim) ) {
            std::list<CurveGuiPtr> curves = getGui()->getCurveEditor()->findCurve(thisShared, i);
            for (std::list<CurveGuiPtr>::iterator it = curves.begin(); it != curves.end(); ++it) {
                AddKeysCommand::KeyToAdd keyToAdd;
                KeyFrame kf;
                kf.setTime(time);
                KnobIntBase* isInt = dynamic_cast<KnobIntBase*>( knob.get() );
                KnobBoolBase* isBool = dynamic_cast<KnobBoolBase*>( knob.get() );
                AnimatingKnobStringHelper* isString = dynamic_cast<AnimatingKnobStringHelper*>( knob.get() );
                KnobDoubleBase* isDouble = dynamic_cast<KnobDoubleBase*>( knob.get() );

                if (isInt) {
                    kf.setValue( isInt->getValue(i) );
                } else if (isBool) {
                    kf.setValue( isBool->getValue(i) );
                } else if (isDouble) {
                    kf.setValue( isDouble->getValue(i) );
                } else if (isString) {
                    std::string v = isString->getValue(i);
                    double dv;
                    isString->stringToKeyFrameValue(time, ViewIdx(0), v, &dv);
                    kf.setValue(dv);
                }

                keyToAdd.keyframes.push_back(kf);
                keyToAdd.curveUI = *it;
                keyToAdd.knob = knob;
                keyToAdd.dimension = i;
                toAdd.push_back(keyToAdd);
            }
        }
    }
    pushUndoCommand( new AddKeysCommand(getGui()->getCurveEditor()->getCurveWidget(), toAdd) );
}

void
KnobGui::removeKnobKeyframe(const KnobIPtr& knob,
                            double time,
                            int dimension,
                            ViewSpec view)
{
    CurvePtr curve = knob->getCurve(view, dimension);
    bool copyCurveValueAtTimeToInternalValue = false;
    if (curve) {
        copyCurveValueAtTimeToInternalValue = curve->getKeyFramesCount() == 1;
    }
    knob->onKeyFrameRemoved(time, view, dimension, copyCurveValueAtTimeToInternalValue);
    KnobGuiPtr gui = getLiveGui(knob);
    if (gui) {
        Q_EMIT gui->keyFrameRemoved();
    }

    assert( knob->getHolder()->getApp() );
    if ( !knob->getIsSecret() ) {
        knob->getHolder()->getApp()->removeKeyFrameIndicator(time);
    }
    refreshLiveGui(knob, dimension);
}

void
KnobGui::setKnobKeyframes(const KnobIPtr& knob,
                          const std::vector<KeyFrame>& keys,
                          int dimension,
                          ViewSpec view)
{
    assert( knob->getHolder()->getApp() );
    knob->beginChanges();
    std::list<SequenceTime> times;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        bool keyAdded = knob->onKeyFrameSet(keys[i].getTime(), view, keys[i], dimension);
        if (keyAdded) {
            times.push_back( keys[i].getTime() );
        }
    }
    knob->endChanges();
    KnobGuiPtr gui = getLiveGui(knob);
    if (gui) {
        Q_EMIT gui->keyFrameSet();
    }
    if ( !knob->getIsSecret() && knob->isDeclaredByPlugin() ) {
        knob->getHolder()->getApp()->addMultipleKeyframeIndicatorsAdded(times, true);
    }
}

void
KnobGui::removeKnobKeyframes(const KnobIPtr& knob,
                             const std::vector<KeyFrame>& keys,
                             int dimension,
                             ViewSpec view)
{
    knob->beginChanges();
    for (std::size_t i = 0; i < keys.size(); ++i) {
        knob->onKeyFrameRemoved(keys[i].getTime(), view, dimension, i == 0);
    }
    knob->endChanges();

    KnobGuiPtr gui = getLiveGui(knob);
    if (gui) {
        Q_EMIT gui->keyFrameRemoved();
    }
    refreshLiveGui(knob, dimension);
}

QString
KnobGui::getScriptNameHtml() const
{
    return QString::fromUtf8("<font size = 4><b>%1</b></font>").arg( QString::fromUtf8( getKnob()->getName().c_str() ) );
}

QString
KnobGui::toolTip() const
{
    KnobIPtr knob = getKnob();
    KnobChoice* isChoice = dynamic_cast<KnobChoice*>( knob.get() );
    bool isMarkdown = knob.get()->isHintInMarkdown();
    QString tt;
    QString realTt;

    if (isMarkdown) {
        tt = QString::fromUtf8( knob.get()->getName().c_str() );
        tt.append( QString::fromUtf8("\n==========\n\n") );
    } else {
        tt = getScriptNameHtml();
    }

    if (!isChoice) {
        realTt.append( QString::fromUtf8( knob->getHintToolTip().c_str() ) );
    } else {
        realTt.append( QString::fromUtf8( isChoice->getHintToolTipFull().c_str() ) );
    }

    std::vector<std::string> expressions;
    bool exprAllSame = true;
    for (int i = 0; i < knob->getDimension(); ++i) {
        expressions.push_back( knob->getExpression(i) );
        if ( (i > 0) && (expressions[i] != expressions[0]) ) {
            exprAllSame = false;
        }
    }

    QString exprTt;
    if (exprAllSame) {
        if ( !expressions[0].empty() ) {
            if (isMarkdown) {
                exprTt = QString::fromUtf8("ret = **%1**\n\n").arg( QString::fromUtf8( expressions[0].c_str() ).trimmed() );
            } else {
                exprTt = QString::fromUtf8("<br />ret = <b>%1</b>").arg( QString::fromUtf8( expressions[0].c_str() ) );
            }
        }
    } else {
        for (int i = 0; i < knob->getDimension(); ++i) {
            std::string dimName = knob->getDimensionName(i);
            QString toAppend;
            if (isMarkdown) {
                toAppend = QString::fromUtf8("%1 = **%2**\n\n").arg( QString::fromUtf8( dimName.c_str() ) ).arg( QString::fromUtf8( expressions[i].c_str() ).trimmed() );
            } else {
                toAppend = QString::fromUtf8("<br />%1 = <b>%2</b>").arg( QString::fromUtf8( dimName.c_str() ) ).arg( QString::fromUtf8( expressions[i].c_str() ) );
            }
            exprTt.append(toAppend);
        }
    }

    if ( !exprTt.isEmpty() ) {
        tt.append(exprTt);
    }

    if ( !realTt.isEmpty() ) {
        if (!isMarkdown) {
            realTt = NATRON_NAMESPACE::convertFromPlainText(realTt.trimmed(), NATRON_NAMESPACE::WhiteSpaceNormal);
        }
        tt.append(realTt);
    }

    if (isMarkdown) {
        tt = Markdown::convert2html(tt);
        // Shrink H1/H2 (Can't do it in qt stylesheet)
        tt.replace( QString::fromUtf8("<h1>"), QString::fromUtf8("<h1 style=\"font-size:large;\">") );
        tt.replace( QString::fromUtf8("<h2>"), QString::fromUtf8("<h2 style=\"font-size:large;\">") );
    }

    const QString linkTt = getLinkToolTip(knob);
    if (!linkTt.isEmpty()) {
        tt += isMarkdown ? QString::fromUtf8("<p>%1</p>").arg(linkTt) : QString::fromUtf8("<br /><br />%1").arg(linkTt);
    }

    return tt;
} // KnobGui::toolTip

bool
KnobGui::hasToolTip() const
{
    //Always true now that we display the script name in the tooltip
    return true; //!getKnob()->getHintToolTip().empty();
}

void
KnobGui::onRemoveKeyActionTriggered()
{
    QAction* action = qobject_cast<QAction*>( sender() );

    assert(action);
    int dim = action->data().toInt();
    KnobIPtr knob = getKnob();

    assert( knob->getHolder()->getApp() );
    //get the current time on the global timeline
    SequenceTime time = knob->getHolder()->getApp()->getTimeLine()->currentFrame();
    std::map<CurveGuiPtr, std::vector<KeyFrame > > toRemove;
    KnobGuiPtr thisShared = shared_from_this();

    for (int i = 0; i < knob->getDimension(); ++i) {
        if ( (dim == -1) || (i == dim) ) {
            std::list<CurveGuiPtr> curves = getGui()->getCurveEditor()->findCurve(thisShared, i);
            for (std::list<CurveGuiPtr>::iterator it = curves.begin(); it != curves.end(); ++it) {
                KeyFrame kf;
                bool foundKey = knob->getCurve(ViewIdx(0), i)->getKeyFrameWithTime(time, &kf);

                if (foundKey) {
                    std::vector<KeyFrame > vect;
                    vect.push_back(kf);
                    toRemove.insert( std::make_pair(*it, vect) );
                }
            }
        }
    }
    pushUndoCommand( new RemoveKeysCommand(getGui()->getCurveEditor()->getCurveWidget(),
                                           toRemove) );
}

int
KnobGui::getKnobsCountOnSameLine() const
{
    return _imp->knobsOnSameLine.size();
}

void
KnobGui::hide()
{
    if (!_imp->customInteract) {
        _hide();
    } else {
        _imp->customInteract->hide();
    }

    //also  hide the curve from the curve editor if there's any and the knob is not inside a group
    KnobIPtr knob = getKnob();
    if ( knob && knob->getHolder() && knob->getHolder()->getApp() ) {
        KnobIPtr parent = getKnob()->getParentKnob();
        bool isSecret = true;
        while (parent) {
            if ( !parent->getIsSecret() ) {
                isSecret = false;
                break;
            }
            parent = parent->getParentKnob();
        }
        if (isSecret) {
            getGui()->getCurveEditor()->hideCurves( shared_from_this() );
        }
    }

    ////In order to remove the row of the layout we have to make sure ALL the knobs on the row
    ////are hidden.
    bool shouldRemoveWidget = true;
    for (U32 i = 0; i < _imp->knobsOnSameLine.size(); ++i) {
        KnobGuiPtr sibling = _imp->container->getKnobGui( _imp->knobsOnSameLine[i].lock() );
        if ( sibling && !sibling->isSecretRecursive() ) {
            shouldRemoveWidget = false;
        }
    }

    if (shouldRemoveWidget) {
        if (_imp->field) {
            _imp->field->hide();
        }
        if (_imp->container) {
            _imp->container->refreshTabWidgetMaxHeight();
        }
    } else {
        if ( _imp->field && !_imp->field->isVisible() ) {
            _imp->field->show();
        }
    }
    if (_imp->labelContainer) {
        _imp->labelContainer->hide();
    } else if (_imp->descriptionLabel) {
        _imp->descriptionLabel->hide();
    }
} // KnobGui::hide

void
KnobGui::show(int /*index*/)
{
    if ( !getGui() ) {
        return;
    }
    if (!_imp->customInteract) {
        _show();
    } else {
        _imp->customInteract->show();
    }
    onLinkStateChanged();

    //also show the curve from the curve editor if there's any
    KnobIPtr knob = getKnob();
    if ( knob && knob->getHolder() && knob->getHolder()->getApp() ) {
        getGui()->getCurveEditor()->showCurves( shared_from_this() );
    }

    //if (_imp->isOnNewLine) {
    if (_imp->field) {
        _imp->field->show();
    }
    if (_imp->container) {
        _imp->container->refreshTabWidgetMaxHeight();
    }
    //}

    if (_imp->labelContainer) {
        _imp->labelContainer->show();
    } else if (_imp->descriptionLabel) {
        _imp->descriptionLabel->show();
    }
}

int
KnobGui::getActualIndexInLayout() const
{
    QWidget* parent = isLabelOnSameColumn() ? _imp->labelContainer->parentWidget() : _imp->field->parentWidget();

    if (!parent) {
        return -1;
    }
    QGridLayout* layout = dynamic_cast<QGridLayout*>( parent->layout() );
    if (!layout) {
        return -1;
    }
    for (int i = 0; i < layout->rowCount(); ++i) {
        QLayoutItem* leftItem = layout->itemAtPosition(i, 0);
        QLayoutItem* rightItem = layout->itemAtPosition(i, 1);
        QWidget* widgets[2] = {0, 0};
        if (leftItem) {
            widgets[0] = leftItem->widget();
        }
        if (rightItem) {
            widgets[1] = rightItem->widget();
        }
        for (int j = 0; j  < 2; ++j) {
            if (widgets[j] == _imp->labelContainer ||
                widgets[j] == _imp->field) {
                return i;
            }
        }
    }

    return -1;
}

bool
KnobGui::isOnNewLine() const
{
    return _imp->isOnNewLine;
}

void
KnobGui::setEnabledSlot()
{
    // A toolbar page's knobs get a KnobGui in the settings panel but no widgets.
    if (!getGui() || !_imp->widgetCreated) {
        return;
    }
    if (!_imp->customInteract) {
        setEnabled();
    }
    KnobIPtr knob = getKnob();
    if (_imp->descriptionLabel) {
        _imp->descriptionLabel->setReadOnly( !knob->isEnabled(0) );
    }
    KnobGuiPtr thisShared = shared_from_this();
    if ( knob->getHolder() && knob->getHolder()->getApp() ) {
        for (int i = 0; i < knob->getDimension(); ++i) {
            if ( !knob->isEnabled(i) ) {
                getGui()->getCurveEditor()->hideCurve(thisShared, i);
            } else {
                getGui()->getCurveEditor()->showCurve(thisShared, i);
            }
        }
    }
}

QWidget*
KnobGui::getFieldContainer() const
{
    return _imp->field;
}

QWidget*
KnobGui::getLabelContainer() const
{
    return _imp->labelContainer;
}

NATRON_NAMESPACE_EXIT
