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


#include "RotoPaintInteract.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QCoreApplication>
#include <QLineF>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/ChoiceOption.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/MergingEnum.h"
#include "Engine/Node.h"
#include "Engine/OverlaySupport.h"
#include "Engine/PluginActionShortcut.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoLatencyProbe.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoPaint.h"
#include "Engine/RotoPoint.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/RotoUndoCommand.h"
#include "Engine/Transform.h"
#include "Engine/ViewerInstance.h"
#include "Global/GLIncludes.h"

#define ROTOPAINT_VIEWER_UI_SECTIONS_SPACING_PX 5

NATRON_NAMESPACE_ENTER

RotoPaintInteract::RotoPaintInteract(EffectInstance* host,
                                     bool paintByDefault)
    : RotoCurveInteract(host)
    , strokeBeingPaint()
    , strokeBeingPaintedTimelineFrame(0)
    , lastPaintToolAction()
    , lastTabletDownTriggeredEraser(false)
    , paintByDefault(paintByDefault)
    , strokePreviews()
    , engineStrokePreview(0)
    , deferredPenEvents()
    , neatRenderPending(false)
{
}

// make_shared enabler (because make_shared needs access to the private constructor)
// see https://stackoverflow.com/a/20961251/2607517
struct RotoPaintInteract::MakeSharedEnabler: public RotoPaintInteract
{
    MakeSharedEnabler(EffectInstance* host,
                      bool paintByDefault)
        : RotoPaintInteract(host, paintByDefault)
    {
    }
};

RotoPaintInteractPtr
RotoPaintInteract::create(EffectInstance* host,
                          bool paintByDefault)
{
    return std::make_shared<RotoPaintInteract::MakeSharedEnabler>(host, paintByDefault);
}

namespace {
QString
trBrush(const char* sourceText)
{
    return QCoreApplication::translate("RotoPaint", sourceText);
}
} // anonymous namespace

void
RotoPaintBrushKnobs::createSettingKnobs(EffectInstance* host,
                                        const KnobPagePtr& page)
{
    KnobBoolPtr multiStroke = AppManager::createKnob<KnobBool>(host, trBrush(kRotoUIParamMultiStrokeEnabledLabel));
    multiStroke->setName(kRotoUIParamMultiStrokeEnabled);
    multiStroke->setInViewerContextLabel(trBrush(kRotoUIParamMultiStrokeEnabledLabel));
    multiStroke->setHintToolTip(trBrush(kRotoUIParamMultiStrokeEnabledHint));
    multiStroke->setEvaluateOnChange(false);
    multiStroke->setDefaultValue(true);
    multiStroke->setSecretByDefault(true);
    page->addKnob(multiStroke);
    multiStrokeEnabled = multiStroke;

    KnobColorPtr colorWheel = AppManager::createKnob<KnobColor>(host, trBrush(kRotoUIParamColorWheelLabel), 3);
    colorWheel->setName(kRotoUIParamColorWheel);
    colorWheel->setHintToolTip(trBrush(kRotoUIParamColorWheelHint));
    colorWheel->setEvaluateOnChange(false);
    colorWheel->setDefaultValue(1., 0);
    colorWheel->setDefaultValue(1., 1);
    colorWheel->setDefaultValue(1., 2);
    colorWheel->setSecretByDefault(true);
    page->addKnob(colorWheel);
    colorWheelButton = colorWheel;

    KnobChoicePtr blendingModes = AppManager::createKnob<KnobChoice>(host, trBrush(kRotoUIParamBlendingOpLabel));
    blendingModes->setName(kRotoUIParamBlendingOp);
    blendingModes->setHintToolTip(trBrush(kRotoUIParamBlendingOpHint));
    blendingModes->setEvaluateOnChange(false);
    blendingModes->setSecretByDefault(true);
    {
        std::vector<ChoiceOption> choices;
        Merge::getOperatorStrings(&choices);
        blendingModes->populateChoices(choices);
    }
    blendingModes->setDefaultValue((int)eMergeOver);
    page->addKnob(blendingModes);
    compositingOperatorChoice = blendingModes;

    KnobDoublePtr opacityKnob = AppManager::createKnob<KnobDouble>(host, trBrush(kRotoUIParamOpacityLabel));
    opacityKnob->setName(kRotoUIParamOpacity);
    opacityKnob->setInViewerContextLabel(trBrush(kRotoUIParamOpacityLabel));
    opacityKnob->setHintToolTip(trBrush(kRotoUIParamOpacityHint));
    opacityKnob->setEvaluateOnChange(false);
    opacityKnob->setSecretByDefault(true);
    opacityKnob->setDefaultValue(1.);
    opacityKnob->setMinimum(0.);
    opacityKnob->setMaximum(1.);
    opacityKnob->disableSlider();
    page->addKnob(opacityKnob);
    opacitySpinbox = opacityKnob;

    KnobButtonPtr pressureOpacity = AppManager::createKnob<KnobButton>(host, trBrush(kRotoUIParamPressureOpacityLabel));
    pressureOpacity->setName(kRotoUIParamPressureOpacity);
    pressureOpacity->setHintToolTip(trBrush(kRotoUIParamPressureOpacityHint));
    pressureOpacity->setEvaluateOnChange(false);
    pressureOpacity->setCheckable(true);
    pressureOpacity->setDefaultValue(true);
    pressureOpacity->setSecretByDefault(true);
    pressureOpacity->setInViewerContextCanHaveShortcut(true);
    pressureOpacity->setIconLabel(NATRON_ICONS_PATH "natron/pressure.svg");
    page->addKnob(pressureOpacity);
    pressureOpacityButton = pressureOpacity;

    KnobDoublePtr sizeKnob = AppManager::createKnob<KnobDouble>(host, trBrush(kRotoUIParamSizeLabel));
    sizeKnob->setName(kRotoUIParamSize);
    sizeKnob->setInViewerContextLabel(trBrush(kRotoUIParamSizeLabel));
    sizeKnob->setHintToolTip(trBrush(kRotoUIParamSizeHint));
    sizeKnob->setEvaluateOnChange(false);
    sizeKnob->setSecretByDefault(true);
    sizeKnob->setDefaultValue(25.);
    sizeKnob->setMinimum(0.);
    sizeKnob->setMaximum(1000.);
    sizeKnob->disableSlider();
    page->addKnob(sizeKnob);
    sizeSpinbox = sizeKnob;

    KnobButtonPtr pressureSize = AppManager::createKnob<KnobButton>(host, trBrush(kRotoUIParamPressureSizeLabel));
    pressureSize->setName(kRotoUIParamPressureSize);
    pressureSize->setHintToolTip(trBrush(kRotoUIParamPressureSizeHint));
    pressureSize->setEvaluateOnChange(false);
    pressureSize->setCheckable(true);
    pressureSize->setDefaultValue(false);
    pressureSize->setSecretByDefault(true);
    pressureSize->setInViewerContextCanHaveShortcut(true);
    pressureSize->setIconLabel(NATRON_ICONS_PATH "natron/pressure.svg");
    page->addKnob(pressureSize);
    pressureSizeButton = pressureSize;

    KnobDoublePtr hardnessKnob = AppManager::createKnob<KnobDouble>(host, trBrush(kRotoUIParamHardnessLabel));
    hardnessKnob->setName(kRotoUIParamHardness);
    hardnessKnob->setInViewerContextLabel(trBrush(kRotoUIParamHardnessLabel));
    hardnessKnob->setHintToolTip(trBrush(kRotoUIParamHardnessHint));
    hardnessKnob->setEvaluateOnChange(false);
    hardnessKnob->setSecretByDefault(true);
    hardnessKnob->setDefaultValue(.2);
    hardnessKnob->setMinimum(0.);
    hardnessKnob->setMaximum(1.);
    hardnessKnob->disableSlider();
    page->addKnob(hardnessKnob);
    hardnessSpinbox = hardnessKnob;

    KnobButtonPtr pressureHardness = AppManager::createKnob<KnobButton>(host, trBrush(kRotoUIParamPressureHardnessLabel));
    pressureHardness->setName(kRotoUIParamPressureHardness);
    pressureHardness->setHintToolTip(trBrush(kRotoUIParamPressureHardnessHint));
    pressureHardness->setEvaluateOnChange(false);
    pressureHardness->setCheckable(true);
    pressureHardness->setDefaultValue(false);
    pressureHardness->setSecretByDefault(true);
    pressureHardness->setInViewerContextCanHaveShortcut(true);
    pressureHardness->setIconLabel(NATRON_ICONS_PATH "natron/pressure.svg");
    page->addKnob(pressureHardness);
    pressureHardnessButton = pressureHardness;

    KnobButtonPtr buildUp = AppManager::createKnob<KnobButton>(host, trBrush(kRotoUIParamBuildUpLabel));
    buildUp->setName(kRotoUIParamBuildUp);
    buildUp->setInViewerContextLabel(trBrush(kRotoUIParamBuildUpLabel));
    buildUp->setHintToolTip(trBrush(kRotoUIParamBuildUpHint));
    buildUp->setEvaluateOnChange(false);
    buildUp->setCheckable(true);
    buildUp->setDefaultValue(true);
    buildUp->setSecretByDefault(true);
    buildUp->setInViewerContextCanHaveShortcut(true);
    buildUp->setIconLabel(NATRON_ICONS_PATH "natron/buildup.svg");
    page->addKnob(buildUp);
    buildUpButton = buildUp;

    KnobButtonPtr autoConnectViewer = AppManager::createKnob<KnobButton>(host, trBrush(kRotoUIParamAutoConnectViewerLabel));
    autoConnectViewer->setName(kRotoUIParamAutoConnectViewer);
    autoConnectViewer->setHintToolTip(trBrush(kRotoUIParamAutoConnectViewerHint));
    autoConnectViewer->setEvaluateOnChange(false);
    autoConnectViewer->setCheckable(true);
    autoConnectViewer->setDefaultValue(false);
    autoConnectViewer->setSecretByDefault(true);
    autoConnectViewer->setInViewerContextCanHaveShortcut(true);
    autoConnectViewer->setIconLabel(NATRON_ICONS_PATH "lucide/eye.svg", true);
    autoConnectViewer->setIconLabel(NATRON_ICONS_PATH "lucide/eye-off.svg", false);
    page->addKnob(autoConnectViewer);
    autoConnectViewerButton = autoConnectViewer;

    KnobDoublePtr effectStrength = AppManager::createKnob<KnobDouble>(host, trBrush(kRotoUIParamEffectLabel));
    effectStrength->setName(kRotoUIParamEffect);
    effectStrength->setInViewerContextLabel(trBrush(kRotoUIParamEffectLabel));
    effectStrength->setHintToolTip(trBrush(kRotoUIParamEffectHint));
    effectStrength->setEvaluateOnChange(false);
    effectStrength->setSecretByDefault(true);
    effectStrength->setDefaultValue(15);
    effectStrength->setMinimum(0.);
    effectStrength->setMaximum(100.);
    effectStrength->disableSlider();
    page->addKnob(effectStrength);
    effectSpinBox = effectStrength;

    KnobIntPtr timeOffsetSb = AppManager::createKnob<KnobInt>(host, trBrush(kRotoUIParamTimeOffsetLabel));
    timeOffsetSb->setName(kRotoUIParamTimeOffset);
    timeOffsetSb->setInViewerContextLabel(trBrush(kRotoUIParamTimeOffsetLabel));
    timeOffsetSb->setHintToolTip(trBrush(kRotoUIParamTimeOffsetHint));
    timeOffsetSb->setEvaluateOnChange(false);
    timeOffsetSb->setSecretByDefault(true);
    timeOffsetSb->setDefaultValue(0);
    timeOffsetSb->disableSlider();
    page->addKnob(timeOffsetSb);
    timeOffsetSpinBox = timeOffsetSb;

    KnobChoicePtr timeOffsetMode = AppManager::createKnob<KnobChoice>(host, trBrush(kRotoUIParamTimeOffsetLabel));
    timeOffsetMode->setName(kRotoUIParamTimeOffset);
    timeOffsetMode->setHintToolTip(trBrush(kRotoUIParamTimeOffsetHint));
    timeOffsetMode->setEvaluateOnChange(false);
    timeOffsetMode->setSecretByDefault(true);
    {
        std::vector<ChoiceOption> choices;
        choices.push_back(ChoiceOption("Relative", "", trBrush("The time offset is a frame number in the source").toStdString()));
        choices.push_back(ChoiceOption("Absolute", "", trBrush("The time offset is a relative amount of frames relative to the current frame").toStdString()));
        timeOffsetMode->populateChoices(choices);
    }
    timeOffsetMode->setDefaultValue(0);
    page->addKnob(timeOffsetMode);
    timeOffsetModeChoice = timeOffsetMode;

    KnobChoicePtr sourceType = AppManager::createKnob<KnobChoice>(host, trBrush(kRotoUIParamSourceTypeLabel));
    sourceType->setName(kRotoUIParamSourceType);
    sourceType->setHintToolTip(trBrush(kRotoUIParamSourceTypeHint));
    sourceType->setEvaluateOnChange(false);
    sourceType->setSecretByDefault(true);
    {
        std::vector<ChoiceOption> choices;
        choices.push_back(ChoiceOption("foreground", "", ""));
        choices.push_back(ChoiceOption("background", "", ""));
        for (int i = 1; i < 10; ++i) {
            QString str = QString::fromUtf8("background") + QString::number(i + 1);
            choices.push_back(ChoiceOption(str.toStdString(), "", ""));
        }
        sourceType->populateChoices(choices);
    }
    sourceType->setDefaultValue(1);
    page->addKnob(sourceType);
    sourceTypeChoice = sourceType;
} // RotoPaintBrushKnobs::createSettingKnobs

void
RotoPaintBrushKnobs::addSettingKnobsToViewerUI(EffectInstance* host,
                                               const KnobButtonPtr& resetCloneOffset) const
{
    const KnobIPtr multiStroke = multiStrokeEnabled.lock();
    const KnobIPtr colorWheel = colorWheelButton.lock();
    const KnobIPtr blendingModes = compositingOperatorChoice.lock();
    const KnobIPtr opacityKnob = opacitySpinbox.lock();
    const KnobIPtr pressureOpacity = pressureOpacityButton.lock();
    const KnobIPtr sizeKnob = sizeSpinbox.lock();
    const KnobIPtr pressureSize = pressureSizeButton.lock();
    const KnobIPtr hardnessKnob = hardnessSpinbox.lock();
    const KnobIPtr pressureHardness = pressureHardnessButton.lock();
    const KnobIPtr buildUp = buildUpButton.lock();
    const KnobIPtr autoConnectViewer = autoConnectViewerButton.lock();
    const KnobIPtr effectStrength = effectSpinBox.lock();
    const KnobIPtr timeOffsetSb = timeOffsetSpinBox.lock();
    const KnobIPtr timeOffsetMode = timeOffsetModeChoice.lock();
    const KnobIPtr sourceType = sourceTypeChoice.lock();
    host->addKnobToViewerUI(multiStroke);
    multiStroke->setInViewerContextItemSpacing(ROTOPAINT_VIEWER_UI_SECTIONS_SPACING_PX);
    host->addKnobToViewerUI(colorWheel);
    colorWheel->setInViewerContextItemSpacing(ROTOPAINT_VIEWER_UI_SECTIONS_SPACING_PX);
    host->addKnobToViewerUI(blendingModes);
    blendingModes->setInViewerContextItemSpacing(ROTOPAINT_VIEWER_UI_SECTIONS_SPACING_PX);
    host->addKnobToViewerUI(opacityKnob);
    opacityKnob->setInViewerContextItemSpacing(1);
    host->addKnobToViewerUI(pressureOpacity);
    pressureOpacity->setInViewerContextItemSpacing(ROTOPAINT_VIEWER_UI_SECTIONS_SPACING_PX);
    host->addKnobToViewerUI(sizeKnob);
    sizeKnob->setInViewerContextItemSpacing(1);
    host->addKnobToViewerUI(pressureSize);
    pressureSize->setInViewerContextItemSpacing(ROTOPAINT_VIEWER_UI_SECTIONS_SPACING_PX);
    host->addKnobToViewerUI(hardnessKnob);
    hardnessKnob->setInViewerContextItemSpacing(1);
    host->addKnobToViewerUI(pressureHardness);
    pressureHardness->setInViewerContextItemSpacing(ROTOPAINT_VIEWER_UI_SECTIONS_SPACING_PX);
    host->addKnobToViewerUI(buildUp);
    buildUp->setInViewerContextItemSpacing(ROTOPAINT_VIEWER_UI_SECTIONS_SPACING_PX);
    host->addKnobToViewerUI(autoConnectViewer);
    autoConnectViewer->setInViewerContextItemSpacing(ROTOPAINT_VIEWER_UI_SECTIONS_SPACING_PX);
    host->addKnobToViewerUI(effectStrength);
    effectStrength->setInViewerContextItemSpacing(ROTOPAINT_VIEWER_UI_SECTIONS_SPACING_PX);
    host->addKnobToViewerUI(timeOffsetSb);
    timeOffsetSb->setInViewerContextItemSpacing(1);
    host->addKnobToViewerUI(timeOffsetMode);
    host->addKnobToViewerUI(sourceType);
    if (resetCloneOffset) {
        host->addKnobToViewerUI(resetCloneOffset);
    }
}

void
RotoPaintBrushKnobs::createToolKnobs(EffectInstance* host,
                                     const KnobPagePtr& toolbar,
                                     bool allTools)
{
    KnobGroupPtr paintToolButton = AppManager::createKnob<KnobGroup>(host, trBrush(kRotoUIParamPaintBrushToolButtonLabel));
    paintToolButton->setName(kRotoUIParamPaintBrushToolButton);
    paintToolButton->setAsToolButton(true);
    paintToolButton->setEvaluateOnChange(false);
    paintToolButton->setSecretByDefault(true);
    paintToolButton->setInViewerContextCanHaveShortcut(true);
    paintToolButton->setIsPersistent(false);
    toolbar->addKnob(paintToolButton);
    paintBrushToolGroup = paintToolButton;

    KnobGroupPtr cloneToolButton, effectToolButton, mergeToolButton;
    if (allTools) {
        cloneToolButton = AppManager::createKnob<KnobGroup>(host, trBrush(kRotoUIParamCloneBrushToolButtonLabel));
        cloneToolButton->setName(kRotoUIParamCloneBrushToolButton);
        cloneToolButton->setAsToolButton(true);
        cloneToolButton->setEvaluateOnChange(false);
        cloneToolButton->setSecretByDefault(true);
        cloneToolButton->setInViewerContextCanHaveShortcut(true);
        cloneToolButton->setIsPersistent(false);
        toolbar->addKnob(cloneToolButton);
        cloneBrushToolGroup = cloneToolButton;
        effectToolButton = AppManager::createKnob<KnobGroup>(host, trBrush(kRotoUIParamEffectBrushToolButtonLabel));
        effectToolButton->setName(kRotoUIParamEffectBrushToolButton);
        effectToolButton->setAsToolButton(true);
        effectToolButton->setEvaluateOnChange(false);
        effectToolButton->setSecretByDefault(true);
        effectToolButton->setInViewerContextCanHaveShortcut(true);
        effectToolButton->setIsPersistent(false);
        toolbar->addKnob(effectToolButton);
        effectBrushToolGroup = effectToolButton;
        mergeToolButton = AppManager::createKnob<KnobGroup>(host, trBrush(kRotoUIParamMergeBrushToolButtonLabel));
        mergeToolButton->setName(kRotoUIParamMergeBrushToolButton);
        mergeToolButton->setAsToolButton(true);
        mergeToolButton->setEvaluateOnChange(false);
        mergeToolButton->setSecretByDefault(true);
        mergeToolButton->setInViewerContextCanHaveShortcut(true);
        mergeToolButton->setIsPersistent(false);
        toolbar->addKnob(mergeToolButton);
        mergeBrushToolGroup = mergeToolButton;
    }

    {
        KnobButtonPtr tool = AppManager::createKnob<KnobButton>(host, trBrush(kRotoUIParamDrawBrushToolButtonActionLabel));
        tool->setName(kRotoUIParamDrawBrushToolButtonAction);
        tool->setHintToolTip(trBrush(kRotoUIParamDrawBrushToolButtonActionHint));
        tool->setCheckable(true);
        tool->setDefaultValue(true);
        tool->setSecretByDefault(true);
        tool->setEvaluateOnChange(false);
        tool->setIconLabel(NATRON_ICONS_PATH "lucide/brush.svg");
        tool->setIsPersistent(false);
        paintToolButton->addKnob(tool);
        brushAction = tool;
    }
    {
        KnobButtonPtr tool = AppManager::createKnob<KnobButton>(host, trBrush(kRotoUIParamPencilToolButtonActionLabel));
        tool->setName(kRotoUIParamPencilToolButtonAction);
        tool->setHintToolTip(trBrush(kRotoUIParamPencilToolButtonAction));
        tool->setCheckable(true);
        tool->setDefaultValue(false);
        tool->setSecretByDefault(true);
        tool->setEvaluateOnChange(false);
        tool->setIconLabel(NATRON_ICONS_PATH "lucide/pencil.svg");
        tool->setIsPersistent(false);
        paintToolButton->addKnob(tool);
        pencilAction = tool;
    }

    {
        KnobButtonPtr tool = AppManager::createKnob<KnobButton>(host, trBrush(kRotoUIParamEraserToolButtonActionLabel));
        tool->setName(kRotoUIParamEraserToolButtonAction);
        tool->setHintToolTip(trBrush(kRotoUIParamEraserToolButtonActionHint));
        tool->setCheckable(true);
        tool->setDefaultValue(false);
        tool->setSecretByDefault(true);
        tool->setEvaluateOnChange(false);
        tool->setIconLabel(NATRON_ICONS_PATH "lucide/eraser.svg");
        tool->setIsPersistent(false);
        paintToolButton->addKnob(tool);
        eraserAction = tool;
    }
    if (allTools) {
        {
            KnobButtonPtr tool = AppManager::createKnob<KnobButton>(host, trBrush(kRotoUIParamCloneToolButtonActionLabel));
            tool->setName(kRotoUIParamCloneToolButtonAction);
            tool->setHintToolTip(trBrush(kRotoUIParamCloneToolButtonActionHint));
            tool->setCheckable(true);
            tool->setDefaultValue(true);
            tool->setSecretByDefault(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel(NATRON_ICONS_PATH "natron/clone-stamp.svg");
            tool->setIsPersistent(false);
            cloneToolButton->addKnob(tool);
            cloneAction = tool;
        }
        {
            KnobButtonPtr tool = AppManager::createKnob<KnobButton>(host, trBrush(kRotoUIParamRevealToolButtonAction));
            tool->setName(kRotoUIParamRevealToolButtonAction);
            tool->setHintToolTip(trBrush(kRotoUIParamRevealToolButtonActionHint));
            tool->setCheckable(true);
            tool->setDefaultValue(false);
            tool->setSecretByDefault(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel(NATRON_ICONS_PATH "lucide/stamp.svg");
            tool->setIsPersistent(false);
            cloneToolButton->addKnob(tool);
            revealAction = tool;
        }

        {
            KnobButtonPtr tool = AppManager::createKnob<KnobButton>(host, trBrush(kRotoUIParamBlurToolButtonActionLabel));
            tool->setName(kRotoUIParamBlurToolButtonAction);
            tool->setHintToolTip(trBrush(kRotoUIParamBlurToolButtonActionHint));
            tool->setCheckable(true);
            tool->setDefaultValue(true);
            tool->setSecretByDefault(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel(NATRON_ICONS_PATH "lucide/droplet.svg");
            tool->setIsPersistent(false);
            effectToolButton->addKnob(tool);
            blurAction = tool;
        }
        {
            KnobButtonPtr tool = AppManager::createKnob<KnobButton>(host, trBrush(kRotoUIParamSmearToolButtonActionLabel));
            tool->setName(kRotoUIParamSmearToolButtonAction);
            tool->setHintToolTip(trBrush(kRotoUIParamSmearToolButtonActionHint));
            tool->setCheckable(true);
            tool->setDefaultValue(false);
            tool->setSecretByDefault(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel(NATRON_ICONS_PATH "lucide/pointer.svg");
            tool->setIsPersistent(false);
            effectToolButton->addKnob(tool);
            smearAction = tool;
        }
        {
            KnobButtonPtr tool = AppManager::createKnob<KnobButton>(host, trBrush(kRotoUIParamDodgeToolButtonActionLabel));
            tool->setName(kRotoUIParamDodgeToolButtonAction);
            tool->setHintToolTip(trBrush(kRotoUIParamDodgeToolButtonActionHint));
            tool->setCheckable(true);
            tool->setDefaultValue(true);
            tool->setSecretByDefault(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel(NATRON_ICONS_PATH "lucide/sun.svg");
            tool->setIsPersistent(false);
            mergeToolButton->addKnob(tool);
            dodgeAction = tool;
        }
        {
            KnobButtonPtr tool = AppManager::createKnob<KnobButton>(host, trBrush(kRotoUIParamBurnToolButtonActionLabel));
            tool->setName(kRotoUIParamBurnToolButtonAction);
            tool->setHintToolTip(trBrush(kRotoUIParamBurnToolButtonActionHint));
            tool->setCheckable(true);
            tool->setDefaultValue(false);
            tool->setSecretByDefault(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel(NATRON_ICONS_PATH "lucide/flame.svg");
            tool->setIsPersistent(false);
            mergeToolButton->addKnob(tool);
            burnAction = tool;
        }
    }
} // RotoPaintBrushKnobs::createToolKnobs

void
RotoPaintBrushKnobs::getSettingShortcuts(std::list<PluginActionShortcut>* shortcuts)
{
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamPressureOpacity, kRotoUIParamPressureOpacityLabel));
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamPressureSize, kRotoUIParamPressureSizeLabel));
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamPressureHardness, kRotoUIParamPressureHardnessLabel));
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamBuildUp, kRotoUIParamBuildUpLabel));
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamResetCloneOffset, kRotoUIParamResetCloneOffsetLabel));
}

void
RotoPaintBrushKnobs::getToolShortcuts(std::list<PluginActionShortcut>* shortcuts)
{
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamPaintBrushToolButton, kRotoUIParamPaintBrushToolButtonLabel, Key_N));
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamCloneBrushToolButton, kRotoUIParamCloneBrushToolButtonLabel, Key_C));
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamEffectBrushToolButton, kRotoUIParamEffectBrushToolButtonLabel, Key_X));
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamMergeBrushToolButton, kRotoUIParamMergeBrushToolButtonLabel, Key_D));
}

RotoContextPtr
RotoPaintInteract::getContext()
{
    return getNode()->getRotoContext();
}

bool
RotoPaintInteract::isMultiStrokeEnabled() const
{
    KnobBoolPtr b =  multiStrokeEnabled.lock();

    if (b) {
        return b->getValue();
    } else {
        return false;
    }
}

bool
RotoPaintInteract::getRoleForGroup(const KnobGroupPtr& k,
                                   RotoRoleEnum* role) const
{
    if (RotoCurveInteract::getRoleForGroup(k, role)) {
        return true;
    }

    bool ret = true;

    if (k == paintBrushToolGroup.lock()) {
        *role = eRotoRolePaintBrush;
    } else if (k == cloneBrushToolGroup.lock()) {
        *role = eRotoRoleCloneBrush;
    } else if (k == effectBrushToolGroup.lock()) {
        *role = eRotoRoleEffectBrush;
    } else if (k == mergeBrushToolGroup.lock()) {
        *role = eRotoRoleMergeBrush;
    } else {
        ret = false;
    }

    return ret;
}

bool
RotoPaintInteract::getToolForAction(const KnobButtonPtr& k,
                                    RotoToolEnum* tool) const
{
    if (RotoCurveInteract::getToolForAction(k, tool)) {
        return true;
    }

    bool ret = true;

    if (k == brushAction.lock()) {
        *tool = eRotoToolSolidBrush;
    } else if (k == pencilAction.lock()) {
        *tool = eRotoToolOpenBezier;
    } else if (k == eraserAction.lock()) {
        *tool = eRotoToolEraserBrush;
    } else if (k == cloneAction.lock()) {
        *tool = eRotoToolClone;
    } else if (k == revealAction.lock()) {
        *tool = eRotoToolReveal;
    } else if (k == blurAction.lock()) {
        *tool = eRotoToolBlur;
    } else if (k == smearAction.lock()) {
        *tool = eRotoToolSmear;
    } else if (k == dodgeAction.lock()) {
        *tool = eRotoToolDodge;
    } else if (k == burnAction.lock()) {
        *tool = eRotoToolBurn;
    } else {
        ret = false;
    }

    return ret;
} // RotoPaintInteract::getToolForAction

bool
RotoPaintInteract::onKnobChanged(KnobI* k,
                                 double time,
                                 bool* ret)
{
    if (RotoCurveInteract::onKnobChanged(k, time, ret)) {
        return true;
    }

    *ret = true;
    if ((k == colorWheelButton.lock().get()) || (k == pressureOpacityButton.lock().get()) || (k == pressureSizeButton.lock().get()) || (k == hardnessSpinbox.lock().get()) || (k == buildUpButton.lock().get())) {
        onBreakMultiStrokeTriggered();
    } else if (k == resetCloneOffsetButton.lock().get()) {
        onBreakMultiStrokeTriggered();
        cloneOffset.first = cloneOffset.second = 0;
    } else {
        return false;
    }

    return true;
}

void
RotoPaintInteract::onRoleSelected(RotoRoleEnum role)
{
    bool isPaintRole = (role == eRotoRolePaintBrush) || (role == eRotoRoleCloneBrush) || (role == eRotoRoleMergeBrush) ||
                       (role == eRotoRoleEffectBrush);

    // RotoPaint action bar
    const KnobIWPtr paintKnobs[] = {
        colorWheelButton, compositingOperatorChoice, opacitySpinbox, pressureOpacityButton, sizeSpinbox, pressureSizeButton,
        hardnessSpinbox, pressureHardnessButton, buildUpButton, autoConnectViewerButton, effectSpinBox, timeOffsetSpinBox,
        timeOffsetModeChoice, sourceTypeChoice, resetCloneOffsetButton, multiStrokeEnabled
    };
    for (const KnobIWPtr& knob : paintKnobs) {
        if (KnobIPtr k = knob.lock()) {
            k->setInViewerContextSecret(!isPaintRole);
        }
    }
}

void
RotoPaintInteract::onToolSelected(const KnobButtonPtr& actionButton,
                                  RotoToolEnum tool)
{
    // The tool's role is the group it sits in. selectedRole can still be the previous role here:
    // while a project loads, setCurrentTool() switches the role group without onKnobChanged()
    // running, so a non-paint tool would otherwise get a paint tool's compositing operator.
    RotoRoleEnum toolRole = selectedRole;
    if (KnobGroupPtr toolGroup = std::dynamic_pointer_cast<KnobGroup>(actionButton->getParentKnob())) {
        getRoleForGroup(toolGroup, &toolRole);
    }
    bool isPaintRole = (toolRole == eRotoRolePaintBrush) || (toolRole == eRotoRoleCloneBrush) || (toolRole == eRotoRoleMergeBrush) || (toolRole == eRotoRoleEffectBrush);
    if (!isPaintRole) {
        return;
    }

    const KnobIWPtr cloneKnobs[] = {
        timeOffsetModeChoice, timeOffsetSpinBox, sourceTypeChoice, resetCloneOffsetButton
    };
    for (const KnobIWPtr& knob : cloneKnobs) {
        if (KnobIPtr k = knob.lock()) {
            k->setInViewerContextSecret(toolRole != eRotoRoleCloneBrush);
        }
    }
    if (KnobDoublePtr effect = effectSpinBox.lock()) {
        effect->setInViewerContextSecret(tool != eRotoToolBlur);
    }
    if (KnobChoicePtr sourceType = sourceTypeChoice.lock()) {
        if (tool == eRotoToolClone) {
            sourceType->setValue(1);
        } else if (tool == eRotoToolReveal) {
            sourceType->setValue(2);
        }
    }
    if (KnobChoicePtr compositingOperator = compositingOperatorChoice.lock()) {
        if ((tool == eRotoToolSolidBrush) || (tool == eRotoToolOpenBezier) || (tool == eRotoToolClone) || (tool == eRotoToolReveal)) {
            compositingOperator->setValue((int)eMergeOver);
        } else if (tool == eRotoToolBurn) {
            compositingOperator->setValue((int)eMergeColorBurn);
        } else if (tool == eRotoToolDodge) {
            compositingOperator->setValue((int)eMergeColorDodge);
        } else {
            compositingOperator->setValue((int)eMergeCopy);
        }
    }

    if (tool != eRotoToolEraserBrush) {
        lastPaintToolAction = actionButton;
    }

    if ( (selectedTool == eRotoToolBlur) ||
         ( selectedTool == eRotoToolBurn) ||
         ( selectedTool == eRotoToolDodge) ||
         ( selectedTool == eRotoToolClone) ||
         ( selectedTool == eRotoToolEraserBrush) ||
         ( selectedTool == eRotoToolSolidBrush) ||
         ( selectedTool == eRotoToolReveal) ||
         ( selectedTool == eRotoToolSmear) ||
         ( selectedTool == eRotoToolSharpen) ) {
        ++multiStrokeBreak;
        makeStroke( true, RotoPoint() );
    }
} // RotoPaintInteract::onToolSelected

void
RotoPaintInteract::onBreakMultiStrokeTriggered()
{
    ++multiStrokeBreak;
    makeStroke(true, RotoPoint());
}

void
RotoPaintInteract::refreshAfterTimeChanged(double time)
{
    if (time != strokeBeingPaintedTimelineFrame) {
        if ((selectedTool == eRotoToolBlur) || (selectedTool == eRotoToolBurn) || (selectedTool == eRotoToolDodge) || (selectedTool == eRotoToolClone) || (selectedTool == eRotoToolEraserBrush) || (selectedTool == eRotoToolSolidBrush) || (selectedTool == eRotoToolReveal) || (selectedTool == eRotoToolSmear) || (selectedTool == eRotoToolSharpen)) {
            onBreakMultiStrokeTriggered();
        }
    }
    RotoCurveInteract::refreshAfterTimeChanged(time);
}

static bool
isBranchConnectedToRotoNodeRecursive(Node* node,
                                     const Node* rotoNode,
                                     int* recursion,
                                     std::list<Node*>& markedNodes)
{
    assert(recursion);
    if (!node) {
        return false;
    }
    if (rotoNode == node) {
        return true;
    }
    markedNodes.push_back(node);
    int maxInputs = node->getNInputs();
    *recursion = *recursion + 1;
    for (int i = 0; i < maxInputs; ++i) {
        NodePtr inp = node->getInput(i);
        if (inp) {
            if ( isBranchConnectedToRotoNodeRecursive(inp.get(), rotoNode, recursion, markedNodes) ) {
                return true;
            }
        }
    }

    return false;
}

void
RotoPaintInteract::checkViewersAreDirectlyConnected()
{
    if (!autoConnectViewerButton.lock()->getValue()) {
        return;
    }
    NodePtr rotoNode = getNode();
    std::list<ViewerInstance*> viewers;

    rotoNode->hasViewersConnected(&viewers);
    for (std::list<ViewerInstance*>::iterator it = viewers.begin(); it != viewers.end(); ++it) {
        NodePtr viewerNode = (*it)->getNode();
        int maxInputs = viewerNode->getNInputs();
        int hasBranchConnectedToRoto = -1;
        for (int i = 0; i < maxInputs; ++i) {
            NodePtr input = viewerNode->getInput(i);
            if (input) {
                std::list<Node*> markedNodes;
                int recursion = 0;
                if ( isBranchConnectedToRotoNodeRecursive(input.get(), rotoNode.get(), &recursion, markedNodes) ) {
                    if (recursion == 0) {
                        //This viewer is already connected to the Roto node directly.
                        break;
                    }
                    viewerNode->disconnectInput(i);
                    if (hasBranchConnectedToRoto == -1) {
                        viewerNode->connectInput(rotoNode, i);
                        hasBranchConnectedToRoto = i;
                    }
                }
            }
        }
    }
}

bool
RotoPaintInteract::StrokeConfig::paintsSameItemAs(const StrokeConfig& other) const
{
    return (tool == other.tool) && (color[0] == other.color[0]) && (color[1] == other.color[1]) && (color[2] == other.color[2]) && (compOp == other.compOp) && (opacity == other.opacity) && (size == other.size) && (hardness == other.hardness) && (pressureOpacity == other.pressureOpacity) && (pressureSize == other.pressureSize) && (pressureHardness == other.pressureHardness) && (buildUp == other.buildUp) && (timeOffset == other.timeOffset) && (timeOffsetMode == other.timeOffsetMode) && (sourceType == other.sourceType) && (effect == other.effect) && (cloneOffset == other.cloneOffset) && (multiStrokeBreak == other.multiStrokeBreak) && (targetLayer.lock() == other.targetLayer.lock());
}

RotoPaintInteract::StrokeConfig
RotoPaintInteract::captureStrokeConfig()
{
    StrokeConfig config;
    config.tool = selectedTool;
    KnobColorPtr colorWheel = colorWheelButton.lock();
    for (int i = 0; i < 3; ++i) {
        config.color[i] = colorWheel->getValue(i);
    }
    config.compOp = (MergingFunctionEnum)compositingOperatorChoice.lock()->getValue();
    config.opacity = opacitySpinbox.lock()->getValue();
    config.size = sizeSpinbox.lock()->getValue();
    config.hardness = hardnessSpinbox.lock()->getValue();
    config.pressureOpacity = pressureOpacityButton.lock()->getValue();
    config.pressureSize = pressureSizeButton.lock()->getValue();
    config.pressureHardness = pressureHardnessButton.lock()->getValue();
    config.buildUp = buildUpButton.lock()->getValue();
    config.timeOffset = timeOffsetSpinBox.lock()->getValue();
    config.timeOffsetMode = timeOffsetModeChoice.lock()->getValue();
    config.sourceType = sourceTypeChoice.lock()->getValue();
    config.effect = effectSpinBox.lock()->getValue();
    config.cloneOffset = cloneOffset;
    RotoContextPtr context = getContext();
    config.frame = context->getTimelineCurrentTime();
    config.layer = context->findDeepestSelectedLayer();
    config.multiStroke = isMultiStrokeEnabled();
    config.multiStrokeBreak = multiStrokeBreak;

    return config;
}

void
RotoPaintInteract::makeStroke(bool prepareForLater,
                              const RotoPoint& point)
{
    switch (selectedTool) {
    case eRotoToolSolidBrush:
    case eRotoToolEraserBrush:
    case eRotoToolClone:
    case eRotoToolReveal:
    case eRotoToolBlur:
    case eRotoToolSharpen:
    case eRotoToolSmear:
    case eRotoToolDodge:
    case eRotoToolBurn:
        makeStroke(prepareForLater, point, captureStrokeConfig());
        break;
    default:
        break;
    }
}

void
RotoPaintInteract::makeStroke(bool prepareForLater,
                              const RotoPoint& point,
                              const StrokeConfig& config)
{
    RotoStrokeType strokeType;
    std::string itemName;

    switch (config.tool) {
    case eRotoToolSolidBrush:
        strokeType = eRotoStrokeTypeSolid;
        itemName = kRotoPaintBrushBaseName;
        break;
    case eRotoToolEraserBrush:
        strokeType = eRotoStrokeTypeEraser;
        itemName = kRotoPaintEraserBaseName;
        break;
    case eRotoToolClone:
        strokeType = eRotoStrokeTypeClone;
        itemName = kRotoPaintCloneBaseName;
        break;
    case eRotoToolReveal:
        strokeType = eRotoStrokeTypeReveal;
        itemName = kRotoPaintRevealBaseName;
        break;
    case eRotoToolBlur:
        strokeType = eRotoStrokeTypeBlur;
        itemName = kRotoPaintBlurBaseName;
        break;
    case eRotoToolSharpen:
        strokeType = eRotoStrokeTypeSharpen;
        itemName = kRotoPaintSharpenBaseName;
        break;
    case eRotoToolSmear:
        strokeType = eRotoStrokeTypeSmear;
        itemName = kRotoPaintSmearBaseName;
        break;
    case eRotoToolDodge:
        strokeType = eRotoStrokeTypeDodge;
        itemName = kRotoPaintDodgeBaseName;
        break;
    case eRotoToolBurn:
        strokeType = eRotoStrokeTypeBurn;
        itemName = kRotoPaintBurnBaseName;
        break;
    default:

        return;
    }

    RotoContextPtr context = getContext();

    // A stroke prepared for another tool can still be here when a queued pen-down replays.
    if (prepareForLater || !strokeBeingPaint || (strokeBeingPaint->getBrushType() != strokeType)) {
        if ( strokeBeingPaint &&
             ( strokeBeingPaint->getBrushType() == strokeType) &&
             strokeBeingPaint->isEmpty() ) {
            ///We already have a fresh stroke prepared for that type
            return;
        }
        std::string name = context->generateUniqueName(itemName);
        strokeBeingPaint.reset( new RotoStrokeItem( strokeType, context, name, RotoLayerPtr() ) );
        strokeBeingPaint->createNodes(false);
    }

    assert(strokeBeingPaint);
    KnobColorPtr colorKnob = strokeBeingPaint->getColorKnob();
    KnobChoicePtr operatorKnob = strokeBeingPaint->getOperatorKnob();
    KnobDoublePtr opacityKnob = strokeBeingPaint->getOpacityKnob();
    KnobDoublePtr sizeKnob = strokeBeingPaint->getBrushSizeKnob();
    KnobDoublePtr hardnessKnob = strokeBeingPaint->getBrushHardnessKnob();
    KnobBoolPtr pressureOpaKnob = strokeBeingPaint->getPressureOpacityKnob();
    KnobBoolPtr pressureSizeKnob = strokeBeingPaint->getPressureSizeKnob();
    KnobBoolPtr pressureHardnessKnob = strokeBeingPaint->getPressureHardnessKnob();
    KnobBoolPtr buildUpKnob = strokeBeingPaint->getBuildupKnob();
    KnobChoicePtr timeOffsetModeKnob = strokeBeingPaint->getTimeOffsetModeKnob();
    KnobChoicePtr sourceTypeKnob = strokeBeingPaint->getBrushSourceTypeKnob();
    KnobIntPtr timeOffsetKnob = strokeBeingPaint->getTimeOffsetKnob();
    KnobDoublePtr translateKnob = strokeBeingPaint->getBrushCloneTranslateKnob();
    KnobDoublePtr effectKnob = strokeBeingPaint->getBrushEffectKnob();
    strokeBeingPaintedTimelineFrame = config.frame;

    colorKnob->setValues(config.color[0], config.color[1], config.color[2], ViewSpec::all(), eValueChangedReasonNatronGuiEdited);
    operatorKnob->setValueFromID(Merge::getOperatorString(config.compOp), 0);
    opacityKnob->setValue(config.opacity);
    sizeKnob->setValue(config.size);
    hardnessKnob->setValue(config.hardness);
    pressureOpaKnob->setValue(config.pressureOpacity);
    pressureSizeKnob->setValue(config.pressureSize);
    pressureHardnessKnob->setValue(config.pressureHardness);
    buildUpKnob->setValue(config.buildUp);
    effectKnob->setValue(config.effect);
    if (!prepareForLater) {
        strokeBeingPaint->getLifeTimeFrameKnob()->setValue(config.frame);
    }
    if ( (strokeType == eRotoStrokeTypeClone) || (strokeType == eRotoStrokeTypeReveal) ) {
        timeOffsetKnob->setValue(config.timeOffset);
        timeOffsetModeKnob->setValue(config.timeOffsetMode);
        sourceTypeKnob->setValue(config.sourceType);
        translateKnob->setValues(-config.cloneOffset.first, -config.cloneOffset.second, ViewSpec::all(), eValueChangedReasonNatronGuiEdited);
    }
    if (!prepareForLater) {
        strokeBeingPaintConfig = config;
        RotoLayerPtr layer = config.layer.lock();
        if (!layer) {
            layer = context->getOrCreateBaseLayer();
        }
        assert(layer);
        context->addItem(layer, 0, strokeBeingPaint, RotoItem::eSelectionReasonOther);
        context->getNode()->getApp()->setUserIsPainting(context->getNode(), strokeBeingPaint, true);
        strokeBeingPaint->appendPoint(true, point);
        if (RotoLatencyProbe* probe = RotoLatencyProbe::instance()) {
            probe->notePointAppended();
        }
    }
} // RotoGui::RotoGuiPrivate::makeStroke

void
RotoPaintInteract::removeCurve(const RotoDrawableItemPtr& curve)
{
    if (curve == strokeBeingPaint) {
        strokeBeingPaint.reset();
    }
    RotoCurveInteract::removeCurve(curve);
}

void
RotoPaintInteract::beginStrokePreview(const QPointF& pos,
                                      double pressure)
{
    StrokePreview preview;

    preview.drawsDabs = (selectedTool == eRotoToolSolidBrush);
    preview.opacity = opacitySpinbox.lock()->getValue();
    preview.size = sizeSpinbox.lock()->getValue();
    preview.hardness = hardnessSpinbox.lock()->getValue();
    preview.pressureOpacity = pressureOpacityButton.lock()->getValue();
    preview.pressureSize = pressureSizeButton.lock()->getValue();
    preview.pressureHardness = pressureHardnessButton.lock()->getValue();
    preview.buildUp = buildUpButton.lock()->getValue();
    // The toolbar has no spacing; the stroke keeps its own, which the prepared stroke carries.
    preview.spacing = strokeBeingPaint ? strokeBeingPaint->getBrushSpacingKnob()->getValue() : 0.1;

    float rgba[4] = { 1.f, 1.f, 1.f, 1.f };
    if (preview.drawsDabs) {
        KnobColorPtr colorWheel = colorWheelButton.lock();
        for (int i = 0; i < 3; ++i) {
            rgba[i] = (float)colorWheel->getValue(i);
        }
        OverlaySupport* overlay = getHost()->getCurrentViewportForOverlays();
        ViewerInstance* viewer = overlay ? overlay->getInternalViewerNode() : 0;
        ProjectColorManagement::DisplayProcessorPtr processor = viewer ? viewer->getDisplayProcessor() : ProjectColorManagement::DisplayProcessorPtr();
        if (viewer && processor) {
            applyViewerDisplayTransform(*processor, rgba, 1, viewer->getGain(), 0., viewer->getGamma());
        }
    }
    for (int i = 0; i < 3; ++i) {
        preview.colour[i] = (unsigned char)std::floor(std::max(0.f, std::min(1.f, rgba[i])) * 255.f + 0.5f);
    }

    preview.pointCount = 0;
    preview.lastX = preview.lastY = preview.lastPressure = preview.distToNext = 0.;
    preview.bbox[0] = preview.bbox[1] = std::numeric_limits<double>::infinity();
    preview.bbox[2] = preview.bbox[3] = -std::numeric_limits<double>::infinity();
    preview.engineStarted = false;
    preview.multiStrokeIndex = -1;
    preview.neatRenderRequested = false;
    preview.tessellationScale = 0.;

    strokePreviews.push_back(preview);
    addStrokePreviewPoint(strokePreviews.back(), pos.x(), pos.y(), pressure);
}

void
RotoPaintInteract::addStrokePreviewPoint(StrokePreview& preview,
                                         double x,
                                         double y,
                                         double pressure)
{
    const int pointIndex = preview.pointCount++;

    // Same rules as RotoContextPrivate::renderStroke, so the dabs land where the render puts them.
    if (!preview.drawsDabs || (preview.spacing == 0.)) {
        return;
    }
    const double brushSpacing = std::max(preview.spacing, 0.05);

    // Appends the dab at (dabX, dabY) and returns the distance to the next one.
    auto addDab = [&preview, brushSpacing, pointIndex](double dabX, double dabY, double dabPressure) -> double {
        StrokePreview::Dab dab;
        double spacing;
        std::vector<std::pair<double, double>> stops;
        getRenderDotParams(preview.opacity, preview.size, preview.hardness, brushSpacing, dabPressure,
                           preview.pressureOpacity, preview.pressureSize, preview.pressureHardness,
                           &dab.innerRadius, &dab.outerRadius, &spacing, &stops);
        dab.x = dabX;
        dab.y = dabY;
        dab.pointIndex = pointIndex;
        dab.solid = stops.empty();
        if (dab.solid) {
            // A flat dab takes the stroke opacity whatever the pressure, as in RotoContextPrivate::renderDot.
            const float flat = (float)preview.opacity;
            std::fill(dab.opacities, dab.opacities + 9, flat);
        } else {
            for (int i = 0; i < 9; ++i) {
                dab.opacities[i] = (float)stops[std::min((std::size_t)i, stops.size() - 1)].second;
            }
        }
        preview.dabs.push_back(dab);
        preview.bbox[0] = std::min(preview.bbox[0], dabX - dab.outerRadius);
        preview.bbox[1] = std::min(preview.bbox[1], dabY - dab.outerRadius);
        preview.bbox[2] = std::max(preview.bbox[2], dabX + dab.outerRadius);
        preview.bbox[3] = std::max(preview.bbox[3], dabY + dab.outerRadius);

        return spacing;
    };

    if (pointIndex == 0) {
        preview.distToNext = addDab(x, y, pressure);
    } else {
        const double dist = std::sqrt((x - preview.lastX) * (x - preview.lastX) + (y - preview.lastY) * (y - preview.lastY));
        while (preview.distToNext <= dist) {
            const double a = dist == 0. ? 0. : preview.distToNext / dist;
            preview.distToNext += addDab(preview.lastX * (1 - a) + x * a,
                                         preview.lastY * (1 - a) + y * a,
                                         preview.lastPressure * (1 - a) + pressure * a);
        }
        preview.distToNext -= dist;
    }
    preview.lastX = x;
    preview.lastY = y;
    preview.lastPressure = pressure;
} // RotoPaintInteract::addStrokePreviewPoint

void
RotoPaintInteract::tessellateStrokePreview(StrokePreview& preview,
                                           std::size_t firstDab,
                                           double pixelScale)
{
    std::vector<StrokePreview::Vertex>& vertices = preview.vertices;
    auto pushVertex = [&vertices, &preview](double x, double y, double opacity) {
        StrokePreview::Vertex v;
        v.x = (float)x;
        v.y = (float)y;
        v.rgba[0] = preview.colour[0];
        v.rgba[1] = preview.colour[1];
        v.rgba[2] = preview.colour[2];
        v.rgba[3] = (unsigned char)std::floor(std::max(0., std::min(1., opacity)) * 255. + 0.5);
        vertices.push_back(v);
    };

    std::vector<double> cosines, sines;
    for (std::size_t i = firstDab; i < preview.dabs.size(); ++i) {
        const StrokePreview::Dab& dab = preview.dabs[i];
        preview.dabFirstVertex.push_back(vertices.size());

        // Enough segments to keep the rim within a quarter pixel of the circle.
        const double radiusPixels = dab.outerRadius / pixelScale;
        const int segments = std::max(8, std::min(64, (int)std::ceil(M_PI * std::sqrt(2. * std::max(0., radiusPixels)))));
        if ((int)cosines.size() != segments + 1) {
            cosines.resize(segments + 1);
            sines.resize(segments + 1);
            for (int j = 0; j <= segments; ++j) {
                const double angle = 2. * M_PI * j / segments;
                cosines[j] = std::cos(angle);
                sines[j] = std::sin(angle);
            }
        }

        auto fan = [&](double radius, double opacity) {
            for (int j = 0; j < segments; ++j) {
                pushVertex(dab.x, dab.y, opacity);
                pushVertex(dab.x + radius * cosines[j], dab.y + radius * sines[j], opacity);
                pushVertex(dab.x + radius * cosines[j + 1], dab.y + radius * sines[j + 1], opacity);
            }
        };
        auto ring = [&](double r0, double r1, double o0, double o1) {
            for (int j = 0; j < segments; ++j) {
                pushVertex(dab.x + r0 * cosines[j], dab.y + r0 * sines[j], o0);
                pushVertex(dab.x + r1 * cosines[j], dab.y + r1 * sines[j], o1);
                pushVertex(dab.x + r1 * cosines[j + 1], dab.y + r1 * sines[j + 1], o1);
                pushVertex(dab.x + r0 * cosines[j], dab.y + r0 * sines[j], o0);
                pushVertex(dab.x + r1 * cosines[j + 1], dab.y + r1 * sines[j + 1], o1);
                pushVertex(dab.x + r0 * cosines[j + 1], dab.y + r0 * sines[j + 1], o0);
            }
        };

        if (dab.solid) {
            fan(dab.outerRadius, dab.opacities[0]);
            continue;
        }
        // Inside the inner radius the ramp holds its first stop.
        fan(dab.innerRadius, dab.opacities[0]);
        // Gouraud rings follow the eight linear pieces of the ramp; narrow ramps merge them, at least a pixel wide each.
        const double width = dab.outerRadius - dab.innerRadius;
        int rings = 8;
        while ((rings > 1) && (width / pixelScale < rings)) {
            rings /= 2;
        }
        const int step = 8 / rings;
        for (int k = 0; k < rings; ++k) {
            const int s0 = k * step;
            const int s1 = s0 + step;
            ring(dab.innerRadius + width * s0 / 8., dab.innerRadius + width * s1 / 8., dab.opacities[s0], dab.opacities[s1]);
        }
    }
} // RotoPaintInteract::tessellateStrokePreview

void
RotoPaintInteract::drawStrokePreviews()
{
    if (strokePreviews.empty()) {
        return;
    }
    OverlaySupport* overlay = getHost()->getCurrentViewportForOverlays();
    if (!overlay) {
        return;
    }
    double pixelScaleX, pixelScaleY;
    overlay->getPixelScale(pixelScaleX, pixelScaleY);
    const double pixelScale = std::max(1e-6, (pixelScaleX + pixelScaleY) / 2.);
    RotoContextPtr context = getContext();

    // The mask of each stroke is built in destination alpha, so that its dabs combine as the renderer combines them
    // (max, or over with build-up) before the colour goes on once. Without destination alpha, dabs blend one by one.
    GLint alphaBits = 0;
    glGetIntegerv(GL_ALPHA_BITS, &alphaBits);

    GLProtectAttrib a(GL_COLOR_BUFFER_BIT | GL_ENABLE_BIT | GL_CURRENT_BIT);
    glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
    GLint vertexArray = 0, arrayBuffer = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertexArray);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
    if (vertexArray) {
        glBindVertexArray(0);
    }
    if (arrayBuffer) {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);

    for (std::list<StrokePreview>::iterator it = strokePreviews.begin(); it != strokePreviews.end(); ++it) {
        StrokePreview& preview = *it;
        if (preview.dabs.empty()) {
            continue;
        }
        if ((preview.tessellationScale <= 0.) || (pixelScale > preview.tessellationScale * 2.) || (pixelScale < preview.tessellationScale / 2.)) {
            preview.vertices.clear();
            preview.dabFirstVertex.clear();
            preview.tessellationScale = pixelScale;
        }
        if (preview.dabFirstVertex.size() < preview.dabs.size()) {
            tessellateStrokePreview(preview, preview.dabFirstVertex.size(), preview.tessellationScale);
        }

        // What the viewer already shows of the stroke is not drawn again.
        std::size_t firstDab = 0;
        RotoStrokeItemPtr item = preview.item.lock();
        if (item) {
            const int shownAge = context->getDisplayedStrokeAge(item, preview.multiStrokeIndex);
            firstDab = std::partition_point(preview.dabs.begin(), preview.dabs.end(),
                                            [shownAge](const StrokePreview::Dab& dab) { return dab.pointIndex <= shownAge; })
                - preview.dabs.begin();
        }
        if (firstDab >= preview.dabs.size()) {
            continue;
        }
        const GLint firstVertex = (GLint)preview.dabFirstVertex[firstDab];
        const GLsizei vertexCount = (GLsizei)(preview.vertices.size() - preview.dabFirstVertex[firstDab]);
        glVertexPointer(2, GL_FLOAT, sizeof(StrokePreview::Vertex), &preview.vertices[0].x);
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(StrokePreview::Vertex), &preview.vertices[0].rgba[0]);

        if (alphaBits <= 0) {
            glEnable(GL_BLEND);
            glBlendEquation(GL_FUNC_ADD);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDrawArrays(GL_TRIANGLES, firstVertex, vertexCount);
            continue;
        }

        const double* box = preview.bbox;
        auto fillBox = [box]() {
            glBegin(GL_QUADS);
            glVertex2d(box[0], box[1]);
            glVertex2d(box[2], box[1]);
            glVertex2d(box[2], box[3]);
            glVertex2d(box[0], box[3]);
            glEnd();
        };

        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
        glDisable(GL_BLEND);
        glColor4f(0.f, 0.f, 0.f, 0.f);
        fillBox();

        glEnable(GL_BLEND);
        if (preview.buildUp) {
            glBlendEquation(GL_FUNC_ADD);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        } else {
            glBlendEquation(GL_MAX);
        }
        glDrawArrays(GL_TRIANGLES, firstVertex, vertexCount);

        glBlendEquation(GL_FUNC_ADD);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
        glBlendFunc(GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA);
        glColor4ub(preview.colour[0], preview.colour[1], preview.colour[2], 255);
        fillBox();

        // The viewer's framebuffer is opaque again for whatever draws next.
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
        glDisable(GL_BLEND);
        glColor4f(0.f, 0.f, 0.f, 1.f);
        fillBox();
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }

    if (arrayBuffer) {
        glBindBuffer(GL_ARRAY_BUFFER, arrayBuffer);
    }
    if (vertexArray) {
        glBindVertexArray(vertexArray);
    }
    glPopClientAttrib();
    glCheckError();
} // RotoPaintInteract::drawStrokePreviews

void
RotoPaintInteract::drawPaintToolOverlay(double screenPixelRatio,
                                        const std::pair<double, double>& viewportSize)
{
    drawStrokePreviews();

    if (paintByDefault && ((selectedRole == eRotoRoleMergeBrush) || (selectedRole == eRotoRolePaintBrush) || (selectedRole == eRotoRoleEffectBrush) || (selectedRole == eRotoRoleCloneBrush)) && (selectedTool != eRotoToolOpenBezier)) {
        Point cursorPos;
        getHost()->getCurrentViewportForOverlays()->getCursorPosition(cursorPos.x, cursorPos.y);
        RectD viewportRect = getHost()->getCurrentViewportForOverlays()->getViewportRect();

        if (viewportRect.contains(cursorPos.x, cursorPos.y)) {
            // Draw a circle  around the cursor
            double brushSize = sizeSpinbox.lock()->getValue();
            GLdouble projection[16];
            glGetDoublev(GL_PROJECTION_MATRIX, projection);
            Point shadow; // how much to translate GL_PROJECTION to get exactly one pixel on screen
            shadow.x = 2. / (projection[0] * viewportSize.first);
            shadow.y = 2. / (projection[5] * viewportSize.second);

            double halfBrush = brushSize / 2.;
            QPointF ellipsePos;
            if ((state == eEventStateDraggingBrushSize) || (state == eEventStateDraggingBrushOpacity)) {
                ellipsePos = mouseCenterOnSizeChange;
            } else {
                ellipsePos = lastMousePos;
            }
            double opacity = opacitySpinbox.lock()->getValue();

            for (int l = 0; l < 2; ++l) {
                glMatrixMode(GL_PROJECTION);
                int direction = (l == 0) ? 1 : -1;
                // translate (1,-1) pixels
                glTranslated(direction * shadow.x, -direction * shadow.y, 0);
                glMatrixMode(GL_MODELVIEW);
                drawEllipse(ellipsePos.x(), ellipsePos.y(), halfBrush, halfBrush, l, 1.f, 1.f, 1.f, opacity);
            }
            if (((selectedTool == eRotoToolClone) || (selectedTool == eRotoToolReveal)) && ((cloneOffset.first != 0) || (cloneOffset.second != 0))) {
                drawCloneSource(ellipsePos, cloneOffset, halfBrush, opacity, screenPixelRatio, viewportSize);
            }
        }
    }
} // RotoPaintInteract::drawPaintToolOverlay

void
RotoPaintInteract::onPenDownStarted(PenType pen)
{
    lastTabletDownTriggeredEraser = false;
    if (paintByDefault && ((pen == ePenTypeEraser) || (pen == ePenTypePen) || (pen == ePenTypeCursor))) {
        if ((pen == ePenTypeEraser) && (selectedTool != eRotoToolEraserBrush)) {
            setCurrentTool(eraserAction.lock());
            lastTabletDownTriggeredEraser = true;
        }
    }
}

bool
RotoPaintInteract::onPaintToolPenDown(double /*time*/,
                                      const QPointF& pos,
                                      double pressure,
                                      double timestamp)
{
    bool didSomething = false;

    switch (selectedTool) {
    case eRotoToolSolidBrush:
    case eRotoToolEraserBrush:
    case eRotoToolClone:
    case eRotoToolReveal:
    case eRotoToolBlur:
    case eRotoToolSharpen:
    case eRotoToolSmear:
    case eRotoToolDodge:
    case eRotoToolBurn: {
        if (((selectedTool == eRotoToolClone) || (selectedTool == eRotoToolReveal)) && isCloneOffsetDragModifier()) {
            startCloneOffsetDrag(pos);
        } else if (isBrushSizeDragModifier()) {
            startBrushSizeDrag(pos);
        } else if (ctrlDown && shiftDown) {
            state = eEventStateDraggingBrushOpacity;
            mouseCenterOnSizeChange = pos;
        } else {
            beginStrokePreview(pos, pressure);
            if (mustDeferPenEvents()) {
                DeferredPenEvent event = { DeferredPenEvent::eTypeDown, pos, pressure, timestamp, selectedTool, captureStrokeConfig() };
                deferredPenEvents.push_back(event);
            } else {
                beginStroke(pos, pressure, timestamp, captureStrokeConfig());
            }
            state = eEventStateBuildingStroke;
            getHost()->setCurrentCursor(eCursorBlank);
        }
        didSomething = true;
        break;
    }
    default:
        assert(false);
        break;
    } // switch

    return didSomething;
} // RotoPaintInteract::onPaintToolPenDown

bool
RotoPaintInteract::setPaintToolCursor(bool* redraw)
{
    RotoContextPtr context = getContext();
    bool cursorSet = false;

    if (context->isRotoPaint() && ((selectedRole == eRotoRoleMergeBrush) || (selectedRole == eRotoRoleCloneBrush) || (selectedRole == eRotoRolePaintBrush) || (selectedRole == eRotoRoleEffectBrush))) {
        if (state != eEventStateBuildingStroke) {
            getHost()->setCurrentCursor(eCursorCross);
        } else {
            getHost()->setCurrentCursor(eCursorBlank);
        }
        cursorSet = true;
    }

    switch (selectedTool) {
    case eRotoToolSolidBrush:
    case eRotoToolEraserBrush:
    case eRotoToolClone:
    case eRotoToolReveal:
    case eRotoToolBlur:
    case eRotoToolSharpen:
    case eRotoToolSmear:
    case eRotoToolDodge:
    case eRotoToolBurn: {
        *redraw = true; // Those tools use the overlay to draw the tool
    }
    default:
        break;
    } // switch

    return cursorSet;
}

bool
RotoPaintInteract::onPaintStatePenMotion(const QPointF& pos,
                                         double pressure,
                                         double timestamp,
                                         double dx,
                                         double dy)
{
    bool didSomething = false;

    switch (state) {
    case eEventStateBuildingStroke: {
        if (!strokePreviews.empty()) {
            addStrokePreviewPoint(strokePreviews.back(), pos.x(), pos.y(), pressure);
        }
        lastMousePos = pos;
        if (mustDeferPenEvents()) {
            DeferredPenEvent event = { DeferredPenEvent::eTypeMotion, pos, pressure, timestamp, selectedTool, StrokeConfig() };
            deferredPenEvents.push_back(event);
            didSomething = true;
        } else {
            didSomething = continueStroke(pos, pressure, timestamp);
        }
        break;
    }
    case eEventStateDraggingCloneOffset: {
        dragCloneOffset(dx, dy);
        onBreakMultiStrokeTriggered();
        break;
    }
    case eEventStateDraggingBrushSize: {
        dragBrushSize(sizeSpinbox.lock(), dx, dy);
        onBreakMultiStrokeTriggered();
        didSomething = true;
        break;
    }
    case eEventStateDraggingBrushOpacity: {
        KnobDoublePtr opaSb = sizeSpinbox.lock();
        double opa = 0;
        if (opaSb) {
            opa = opaSb->getValue();
        }
        double newOpa = opa + ((dx + dy) / 2.);
        if (opa != 0) {
            newOpa = std::max(0., std::min(1., newOpa / opa));
            newOpa = newOpa > 0 ? std::min(1., opa + 0.05) : std::max(0., opa - 0.05);
        } else {
            newOpa = newOpa < 0 ? .0 : 0.05;
        }
        const double scale = 0.01; // i.e. round to nearest one-hundreth
        newOpa = std::floor(newOpa / scale + 0.5) * scale;
        if (opaSb) {
            opaSb->setValue(newOpa);
        }
        onBreakMultiStrokeTriggered();
        didSomething = true;
        break;
    }
    default:
        break;
    } // switch

    return didSomething;
} // RotoPaintInteract::onPaintStatePenMotion

bool
RotoPaintInteract::onPaintStatePenUp()
{
    RotoContextPtr context = getContext();
    bool ret = false;

    if (state == eEventStateBuildingStroke) {
        if (mustDeferPenEvents()) {
            DeferredPenEvent event = { DeferredPenEvent::eTypeUp, lastMousePos, 0., 0., selectedTool, StrokeConfig() };
            deferredPenEvents.push_back(event);
        } else {
            finishStroke();
        }
        if (context->isRotoPaint() && ((selectedRole == eRotoRoleMergeBrush) || (selectedRole == eRotoRoleCloneBrush) || (selectedRole == eRotoRolePaintBrush) || (selectedRole == eRotoRoleEffectBrush))) {
            getHost()->setCurrentCursor(eCursorCross);
        } else {
            getHost()->setCurrentCursor(eCursorDefault);
        }
        ret = true;
    }

    return ret;
} // RotoPaintInteract::onPaintStatePenUp

void
RotoPaintInteract::beginStroke(const QPointF& pos,
                               double pressure,
                               double timestamp,
                               const StrokeConfig& config)
{
    RotoContextPtr context = getContext();

    /*
       Check that all viewers downstream are connected directly to the RotoPaint to avoid glitches and bugs
     */
    checkViewersAreDirectlyConnected();
    if (config.multiStroke && strokeBeingPaint && strokeBeingPaint->getParentLayer() && config.paintsSameItemAs(strokeBeingPaintConfig)) {
        RotoLayerPtr layer = strokeBeingPaint->getParentLayer();
        if (!layer) {
            layer = context->findDeepestSelectedLayer();
            if (!layer) {
                layer = context->getOrCreateBaseLayer();
            }
            assert(layer);
            context->addItem(layer, 0, strokeBeingPaint, RotoItem::eSelectionReasonOther);
        }

        context->getNode()->getApp()->setUserIsPainting(context->getNode(), strokeBeingPaint, true);

        strokeBeingPaint->getLifeTimeFrameKnob()->setValue(config.frame);

        strokeBeingPaint->appendPoint(true, RotoPoint(pos.x(), pos.y(), pressure, timestamp));
        if (RotoLatencyProbe* probe = RotoLatencyProbe::instance()) {
            probe->notePointAppended();
        }
    } else {
        if (strokeBeingPaint && (config.multiStroke || strokeBeingPaint->getParentLayer())) {
            strokeBeingPaint.reset();
        }
        makeStroke(false, RotoPoint(pos.x(), pos.y(), pressure, timestamp), config);
    }

    // Pen-downs and their previews come in the same order.
    engineStrokePreview = 0;
    for (std::list<StrokePreview>::iterator it = strokePreviews.begin(); it != strokePreviews.end(); ++it) {
        if (!it->engineStarted) {
            it->engineStarted = true;
            if (strokeBeingPaint) {
                it->item = strokeBeingPaint;
                it->multiStrokeIndex = (int)strokeBeingPaint->getXControlPoints().size() - 1;
            }
            engineStrokePreview = &*it;
            break;
        }
    }

    context->evaluateChange();
} // RotoPaintInteract::beginStroke

bool
RotoPaintInteract::continueStroke(const QPointF& pos,
                                  double pressure,
                                  double timestamp)
{
    if (!strokeBeingPaint) {
        return false;
    }
    // disable draft during painting
    if (getHost()->getApp()->isDraftRenderEnabled()) {
        getHost()->getApp()->setDraftRenderEnabled(false);
    }

    RotoPoint p(pos.x(), pos.y(), pressure, timestamp);
    if (!strokeBeingPaint->appendPoint(false, p)) {
        return false;
    }
    if (RotoLatencyProbe* probe = RotoLatencyProbe::instance()) {
        probe->notePointAppended();
    }
    getContext()->evaluateChange_noIncrement();

    return true;
}

void
RotoPaintInteract::finishStroke()
{
    RotoContextPtr context = getContext();

    assert(strokeBeingPaint);
    if (!strokeBeingPaint) {
        return;
    }
    context->getNode()->getApp()->setUserIsPainting(context->getNode(), strokeBeingPaint, false);
    assert(strokeBeingPaint->getParentLayer());

    // With Multi-Stroke off strokeBeingPaint is replaced by a fresh stroke below. The painted one must still leave
    // painting mode: its nodes would otherwise keep reading the app's last-stroke image, which the next stroke
    // takes over, and draw half-drawn, black or flickering.
    const RotoStrokeItemPtr paintedStroke = strokeBeingPaint;
    if (!strokeBeingPaintConfig.multiStroke) {
        pushUndoCommand(new AddStrokeUndoCommand(shared_from_this_paint(), strokeBeingPaint));
        makeStroke(true, RotoPoint());
    } else {
        pushUndoCommand(new AddMultiStrokeUndoCommand(shared_from_this_paint(), strokeBeingPaint));
    }

    if (engineStrokePreview) {
        engineStrokePreview->neatRenderRequested = true;
        engineStrokePreview = 0;
    }

    /*
       Re-render the whole stroke from its curves (better interpolation). Strokes begun before it lands wait in
       deferredPenEvents, so the engine never paints two strokes at once.
     */
    neatRenderPending = true;
    std::weak_ptr<RotoPaintInteract> weakThis = shared_from_this_paint();
    context->evaluateNeatStrokeRender([weakThis]() {
        if (RotoPaintInteractPtr self = weakThis.lock()) {
            self->onNeatRenderLanded();
        }
    });
    paintedStroke->setStrokeFinished();
} // RotoPaintInteract::finishStroke

bool
RotoPaintInteract::beginPaintStroke(const RotoLayerPtr& layer,
                                    const QPointF& pos,
                                    double pressure,
                                    double time,
                                    double timestamp,
                                    double sizeScale)
{
    switch (selectedTool) {
    case eRotoToolSolidBrush:
    case eRotoToolEraserBrush:
    case eRotoToolClone:
    case eRotoToolReveal:
    case eRotoToolBlur:
    case eRotoToolSharpen:
    case eRotoToolSmear:
    case eRotoToolDodge:
    case eRotoToolBurn:
        break;
    default:

        return false;
    }

    StrokeConfig config = captureStrokeConfig();
    config.frame = (int)time;
    config.size *= sizeScale;
    if (layer) {
        config.layer = layer;
        config.targetLayer = layer;
    }
    paintStrokeOpen = true;
    if (mustDeferPenEvents()) {
        DeferredPenEvent event = { DeferredPenEvent::eTypeDown, pos, pressure, timestamp, selectedTool, config };
        deferredPenEvents.push_back(event);
    } else {
        beginStroke(pos, pressure, timestamp, config);
    }

    return true;
} // RotoPaintInteract::beginPaintStroke

bool
RotoPaintInteract::continuePaintStroke(const QPointF& pos,
                                       double pressure,
                                       double timestamp)
{
    if (!paintStrokeOpen) {
        return false;
    }
    if (mustDeferPenEvents()) {
        DeferredPenEvent event = { DeferredPenEvent::eTypeMotion, pos, pressure, timestamp, selectedTool, StrokeConfig() };
        deferredPenEvents.push_back(event);

        return true;
    }

    return continueStroke(pos, pressure, timestamp);
}

RotoStrokeItemPtr
RotoPaintInteract::finishPaintStroke()
{
    if (!paintStrokeOpen) {
        return RotoStrokeItemPtr();
    }
    paintStrokeOpen = false;
    if (mustDeferPenEvents()) {
        DeferredPenEvent event = { DeferredPenEvent::eTypeUp, QPointF(), 0., 0., selectedTool, StrokeConfig() };
        deferredPenEvents.push_back(event);

        return RotoStrokeItemPtr();
    }
    const RotoStrokeItemPtr painted = strokeBeingPaint;
    if (!painted) {
        return RotoStrokeItemPtr();
    }
    finishStroke();

    return painted;
}

void
RotoPaintInteract::onNeatRenderLanded()
{
    neatRenderPending = false;
    for (std::list<StrokePreview>::iterator it = strokePreviews.begin(); it != strokePreviews.end();) {
        if (it->neatRenderRequested) {
            it = strokePreviews.erase(it);
        } else {
            ++it;
        }
    }
    replayDeferredPenEvents();
    redrawOverlays();
}

void
RotoPaintInteract::replayDeferredPenEvents()
{
    // The stroke type comes from the tool, which may have changed since these events.
    const RotoToolEnum currentTool = selectedTool;

    while (!deferredPenEvents.empty() && !neatRenderPending) {
        const DeferredPenEvent event = deferredPenEvents.front();
        deferredPenEvents.pop_front();
        selectedTool = event.tool;
        switch (event.type) {
        case DeferredPenEvent::eTypeDown:
            beginStroke(event.pos, event.pressure, event.timestamp, event.config);
            break;
        case DeferredPenEvent::eTypeMotion:
            continueStroke(event.pos, event.pressure, event.timestamp);
            break;
        case DeferredPenEvent::eTypeUp:
            finishStroke();
            break;
        }
    }
    selectedTool = currentTool;
}

bool
RotoPaintInteract::onPenUpFinished()
{
    if (lastTabletDownTriggeredEraser) {
        setCurrentTool(lastPaintToolAction.lock());

        return true;
    }

    return false;
}

NATRON_NAMESPACE_EXIT
