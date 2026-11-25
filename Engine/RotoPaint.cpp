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

#include "RotoPaint.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <sstream> // stringstream
#include <stdexcept>
#include <vector>

#include <ofxNatron.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/MergingEnum.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/EdgeModeKnob.h"
#include "Engine/Nodes/Image/EdgeSample.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoDrawableItem.h"
#include "Engine/RotoLatencyProbe.h"
#include "Engine/RotoObjectSelect.h"
#include "Engine/RotoPaintInteract.h"
#include "Engine/RotoPoint.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/RotoUndoCommand.h"
#include "Engine/TimeLine.h"
#include "Engine/Transform.h"
#include "Engine/VectorFollowKnobs.h"
#include "Engine/ViewIdx.h"

#include "Global/GLIncludes.h"
#include "Global/GlobalDefines.h"


#define ROTOPAINT_MASK_INPUT_INDEX 10
#define ROTOPAINT_VECTORS_INPUT_INDEX 11

#define ROTOPAINT_VIEWER_UI_SECTIONS_SPACING_PX 5

NATRON_NAMESPACE_ENTER


std::string
RotoPaint::getPluginDescription() const
{
    return "RotoPaint is a vector based free-hand drawing node that helps for tasks such as rotoscoping, matting, etc...";
}

RotoPaint::RotoPaint(NodePtr node,
                     bool isPaintByDefault)
    : EffectInstance(node)
    , _imp( new RotoPaintPrivate(this, isPaintByDefault) )
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

RotoPaint::~RotoPaint()
{
}

bool
RotoPaint::isDefaultBehaviourPaintContext() const
{
    return _imp->isPaintByDefault;
}

std::string
RotoPaint::getPluginID() const
{
    return PLUGINID_NATRON_ROTOPAINT;
}

std::string
RotoPaint::getPluginLabel() const
{
    return "RotoPaint";
}

std::string
RotoNode::getPluginID() const
{
    return PLUGINID_NATRON_ROTO;
}

std::string
RotoNode::getPluginLabel() const
{
    return "Roto";
}

std::string
RotoNode::getPluginDescription() const
{
    return "Create masks and shapes";
}

bool
RotoPaint::isHostChannelSelectorSupported(bool* defaultR,
                                          bool* defaultG,
                                          bool* defaultB,
                                          bool* defaultA) const
{
    *defaultR = true;
    *defaultG = true;
    *defaultB = true;
    *defaultA = true;

    return true;
}

bool
RotoNode::isHostChannelSelectorSupported(bool* defaultR,
                                         bool* defaultG,
                                         bool* defaultB,
                                         bool* defaultA) const
{
    *defaultR = false;
    *defaultG = false;
    *defaultB = false;
    *defaultA = true;

    return true;
}

std::string
RotoPaint::getInputLabel (int inputNb) const
{
    if (inputNb == ROTOPAINT_MASK_INPUT_INDEX) {
        return "Mask";
    } else if (inputNb == ROTOPAINT_VECTORS_INPUT_INDEX) {
        return kVectorFollowInputLabel;
    } else if (inputNb == 0) {
        return "Bg";
    } else {
        std::stringstream ss;
        ss << "Bg" << inputNb + 1;

        return ss.str();
    }
}

std::string
RotoPaint::getInputHint(int inputNb) const
{
    if (inputNb == ROTOPAINT_VECTORS_INPUT_INDEX) {
        return tr(kVectorFollowInputHint).toStdString();
    }

    return EffectInstance::getInputHint(inputNb);
}

bool
RotoPaint::isInputMask(int inputNb) const
{
    return inputNb == ROTOPAINT_MASK_INPUT_INDEX;
}

bool
RotoPaint::isInputFirstOnSide(int inputNb) const
{
    return inputNb == ROTOPAINT_VECTORS_INPUT_INDEX;
}

int
RotoPaint::getHostMaskInputNb() const
{
    return ROTOPAINT_MASK_INPUT_INDEX;
}

void
RotoPaint::addAcceptedComponents(int inputNb,
                                 std::list<ImageLayerDesc>* comps)
{
    if (inputNb != ROTOPAINT_MASK_INPUT_INDEX) {
        comps->push_back(ImageLayerDesc::getRGBAComponents());
        comps->push_back(ImageLayerDesc::getRGBComponents());
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
    comps->push_back(ImageLayerDesc::getAlphaComponents());
}

void
RotoPaint::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

void
RotoPaint::initializeKnobs()
{
    //This page is created in the RotoContext, before initializeKnobs() is called.
    KnobPagePtr generalPage = std::dynamic_pointer_cast<KnobPage>( getKnobByName("General") );

    assert(generalPage);

    RotoContextPtr context = getNode()->getRotoContext();
    assert(context);
    QObject::connect( context.get(), SIGNAL(breakMultiStroke()), this, SLOT(onBreakMultiStrokeTriggered()) );
    _imp->ui->connectToCurveContext();

    // The curve viewer buttons come first: the viewer shows its knobs in creation order.
    _imp->ui->createViewerButtonKnobs(generalPage);

    // RotoPaint

    KnobBoolPtr multiStroke = AppManager::createKnob<KnobBool>( this, tr(kRotoUIParamMultiStrokeEnabledLabel) );
    multiStroke->setName(kRotoUIParamMultiStrokeEnabled);
    multiStroke->setInViewerContextLabel( tr(kRotoUIParamMultiStrokeEnabledLabel) );
    multiStroke->setHintToolTip( tr(kRotoUIParamMultiStrokeEnabledHint) );
    multiStroke->setEvaluateOnChange(false);
    multiStroke->setDefaultValue(true);
    multiStroke->setSecretByDefault(true);
    generalPage->addKnob(multiStroke);
    _imp->ui->multiStrokeEnabled = multiStroke;

    KnobColorPtr colorWheel = AppManager::createKnob<KnobColor>(this, tr(kRotoUIParamColorWheelLabel), 3);
    colorWheel->setName(kRotoUIParamColorWheel);
    colorWheel->setHintToolTip( tr(kRotoUIParamColorWheelHint) );
    colorWheel->setEvaluateOnChange(false);
    colorWheel->setDefaultValue(1., 0);
    colorWheel->setDefaultValue(1., 1);
    colorWheel->setDefaultValue(1., 2);
    colorWheel->setSecretByDefault(true);
    generalPage->addKnob(colorWheel);
    _imp->ui->colorWheelButton = colorWheel;

    KnobChoicePtr blendingModes = AppManager::createKnob<KnobChoice>( this, tr(kRotoUIParamBlendingOpLabel) );
    blendingModes->setName(kRotoUIParamBlendingOp);
    blendingModes->setHintToolTip( tr(kRotoUIParamBlendingOpHint) );
    blendingModes->setEvaluateOnChange(false);
    blendingModes->setSecretByDefault(true);
    {
        std::vector<ChoiceOption> choices;
        Merge::getOperatorStrings(&choices);
        blendingModes->populateChoices(choices);
    }
    blendingModes->setDefaultValue( (int)eMergeOver );
    generalPage->addKnob(blendingModes);
    _imp->ui->compositingOperatorChoice = blendingModes;


    KnobDoublePtr opacityKnob = AppManager::createKnob<KnobDouble>( this, tr(kRotoUIParamOpacityLabel) );
    opacityKnob->setName(kRotoUIParamOpacity);
    opacityKnob->setInViewerContextLabel( tr(kRotoUIParamOpacityLabel) );
    opacityKnob->setHintToolTip( tr(kRotoUIParamOpacityHint) );
    opacityKnob->setEvaluateOnChange(false);
    opacityKnob->setSecretByDefault(true);
    opacityKnob->setDefaultValue(1.);
    opacityKnob->setMinimum(0.);
    opacityKnob->setMaximum(1.);
    opacityKnob->disableSlider();
    generalPage->addKnob(opacityKnob);
    _imp->ui->opacitySpinbox = opacityKnob;

    KnobButtonPtr pressureOpacity = AppManager::createKnob<KnobButton>( this, tr(kRotoUIParamPressureOpacityLabel) );
    pressureOpacity->setName(kRotoUIParamPressureOpacity);
    pressureOpacity->setHintToolTip( tr(kRotoUIParamPressureOpacityHint) );
    pressureOpacity->setEvaluateOnChange(false);
    pressureOpacity->setCheckable(true);
    pressureOpacity->setDefaultValue(true);
    pressureOpacity->setSecretByDefault(true);
    pressureOpacity->setInViewerContextCanHaveShortcut(true);
    pressureOpacity->setIconLabel(NATRON_ICONS_PATH "natron/pressure.svg");
    generalPage->addKnob(pressureOpacity);
    _imp->ui->pressureOpacityButton = pressureOpacity;

    KnobDoublePtr sizeKnob = AppManager::createKnob<KnobDouble>( this, tr(kRotoUIParamSizeLabel) );
    sizeKnob->setName(kRotoUIParamSize);
    sizeKnob->setInViewerContextLabel( tr(kRotoUIParamSizeLabel) );
    sizeKnob->setHintToolTip( tr(kRotoUIParamSizeHint) );
    sizeKnob->setEvaluateOnChange(false);
    sizeKnob->setSecretByDefault(true);
    sizeKnob->setDefaultValue(25.);
    sizeKnob->setMinimum(0.);
    sizeKnob->setMaximum(1000.);
    sizeKnob->disableSlider();
    generalPage->addKnob(sizeKnob);
    _imp->ui->sizeSpinbox = sizeKnob;

    KnobButtonPtr pressureSize = AppManager::createKnob<KnobButton>( this, tr(kRotoUIParamPressureSizeLabel) );
    pressureSize->setName(kRotoUIParamPressureSize);
    pressureSize->setHintToolTip( tr(kRotoUIParamPressureSizeHint) );
    pressureSize->setEvaluateOnChange(false);
    pressureSize->setCheckable(true);
    pressureSize->setDefaultValue(false);
    pressureSize->setSecretByDefault(true);
    pressureSize->setInViewerContextCanHaveShortcut(true);
    pressureSize->setIconLabel(NATRON_ICONS_PATH "natron/pressure.svg");
    generalPage->addKnob(pressureSize);
    _imp->ui->pressureSizeButton = pressureSize;

    KnobDoublePtr hardnessKnob = AppManager::createKnob<KnobDouble>( this, tr(kRotoUIParamHardnessLabel) );
    hardnessKnob->setName(kRotoUIParamHardness);
    hardnessKnob->setInViewerContextLabel( tr(kRotoUIParamHardnessLabel) );
    hardnessKnob->setHintToolTip( tr(kRotoUIParamHardnessHint) );
    hardnessKnob->setEvaluateOnChange(false);
    hardnessKnob->setSecretByDefault(true);
    hardnessKnob->setDefaultValue(.2);
    hardnessKnob->setMinimum(0.);
    hardnessKnob->setMaximum(1.);
    hardnessKnob->disableSlider();
    generalPage->addKnob(hardnessKnob);
    _imp->ui->hardnessSpinbox = hardnessKnob;

    KnobButtonPtr pressureHardness = AppManager::createKnob<KnobButton>( this, tr(kRotoUIParamPressureHardnessLabel) );
    pressureHardness->setName(kRotoUIParamPressureHardness);
    pressureHardness->setHintToolTip( tr(kRotoUIParamPressureHardnessHint) );
    pressureHardness->setEvaluateOnChange(false);
    pressureHardness->setCheckable(true);
    pressureHardness->setDefaultValue(false);
    pressureHardness->setSecretByDefault(true);
    pressureHardness->setInViewerContextCanHaveShortcut(true);
    pressureHardness->setIconLabel(NATRON_ICONS_PATH "natron/pressure.svg");
    generalPage->addKnob(pressureHardness);
    _imp->ui->pressureHardnessButton = pressureHardness;

    KnobButtonPtr buildUp = AppManager::createKnob<KnobButton>( this, tr(kRotoUIParamBuildUpLabel) );
    buildUp->setName(kRotoUIParamBuildUp);
    buildUp->setInViewerContextLabel( tr(kRotoUIParamBuildUpLabel) );
    buildUp->setHintToolTip( tr(kRotoUIParamBuildUpHint) );
    buildUp->setEvaluateOnChange(false);
    buildUp->setCheckable(true);
    buildUp->setDefaultValue(true);
    buildUp->setSecretByDefault(true);
    buildUp->setInViewerContextCanHaveShortcut(true);
    buildUp->setIconLabel(NATRON_ICONS_PATH "natron/buildup.svg");
    generalPage->addKnob(buildUp);
    _imp->ui->buildUpButton = buildUp;

    KnobButtonPtr autoConnectViewer = AppManager::createKnob<KnobButton>( this, tr(kRotoUIParamAutoConnectViewerLabel) );
    autoConnectViewer->setName(kRotoUIParamAutoConnectViewer);
    autoConnectViewer->setHintToolTip( tr(kRotoUIParamAutoConnectViewerHint) );
    autoConnectViewer->setEvaluateOnChange(false);
    autoConnectViewer->setCheckable(true);
    autoConnectViewer->setDefaultValue(false);
    autoConnectViewer->setSecretByDefault(true);
    autoConnectViewer->setInViewerContextCanHaveShortcut(true);
    autoConnectViewer->setIconLabel(NATRON_ICONS_PATH "lucide/eye.svg", true);
    autoConnectViewer->setIconLabel(NATRON_ICONS_PATH "lucide/eye-off.svg", false);
    generalPage->addKnob(autoConnectViewer);
    _imp->ui->autoConnectViewerButton = autoConnectViewer;

    KnobDoublePtr effectStrength = AppManager::createKnob<KnobDouble>( this, tr(kRotoUIParamEffectLabel) );
    effectStrength->setName(kRotoUIParamEffect);
    effectStrength->setInViewerContextLabel( tr(kRotoUIParamEffectLabel) );
    effectStrength->setHintToolTip( tr(kRotoUIParamEffectHint) );
    effectStrength->setEvaluateOnChange(false);
    effectStrength->setSecretByDefault(true);
    effectStrength->setDefaultValue(15);
    effectStrength->setMinimum(0.);
    effectStrength->setMaximum(100.);
    effectStrength->disableSlider();
    generalPage->addKnob(effectStrength);
    _imp->ui->effectSpinBox = effectStrength;

    KnobIntPtr timeOffsetSb = AppManager::createKnob<KnobInt>( this, tr(kRotoUIParamTimeOffsetLabel) );
    timeOffsetSb->setName(kRotoUIParamTimeOffset);
    timeOffsetSb->setInViewerContextLabel( tr(kRotoUIParamTimeOffsetLabel) );
    timeOffsetSb->setHintToolTip( tr(kRotoUIParamTimeOffsetHint) );
    timeOffsetSb->setEvaluateOnChange(false);
    timeOffsetSb->setSecretByDefault(true);
    timeOffsetSb->setDefaultValue(0);
    timeOffsetSb->disableSlider();
    generalPage->addKnob(timeOffsetSb);
    _imp->ui->timeOffsetSpinBox = timeOffsetSb;

    KnobChoicePtr timeOffsetMode = AppManager::createKnob<KnobChoice>( this, tr(kRotoUIParamTimeOffsetLabel) );
    timeOffsetMode->setName(kRotoUIParamTimeOffset);
    timeOffsetMode->setHintToolTip( tr(kRotoUIParamTimeOffsetHint) );
    timeOffsetMode->setEvaluateOnChange(false);
    timeOffsetMode->setSecretByDefault(true);
    {
        std::vector<ChoiceOption> choices;
        choices.push_back(ChoiceOption("Relative", "", tr("The time offset is a frame number in the source").toStdString()));
        choices.push_back(ChoiceOption("Absolute", "", tr("The time offset is a relative amount of frames relative to the current frame").toStdString()));
        timeOffsetMode->populateChoices(choices);
    }
    timeOffsetMode->setDefaultValue(0);
    generalPage->addKnob(timeOffsetMode);
    _imp->ui->timeOffsetModeChoice = timeOffsetMode;

    KnobChoicePtr sourceType = AppManager::createKnob<KnobChoice>( this, tr(kRotoUIParamSourceTypeLabel) );
    sourceType->setName(kRotoUIParamSourceType);
    sourceType->setHintToolTip( tr(kRotoUIParamSourceTypeHint) );
    sourceType->setEvaluateOnChange(false);
    sourceType->setSecretByDefault(true);
    {
        std::vector<ChoiceOption> choices;
        choices.push_back(ChoiceOption("foreground", "", ""));
        choices.push_back(ChoiceOption("background", "", ""));
        for (int i = 1; i < 10; ++i) {
            QString str = QString::fromUtf8("background") + QString::number(i + 1);
            choices.push_back( ChoiceOption(str.toStdString(), "", ""));
        }
        sourceType->populateChoices(choices);
    }
    sourceType->setDefaultValue(1);
    generalPage->addKnob(sourceType);
    _imp->ui->sourceTypeChoice = sourceType;

    KnobButtonPtr resetCloneOffset = _imp->ui->createResetCloneOffsetKnob(generalPage);

    addKnobToViewerUI(multiStroke);
    multiStroke->setInViewerContextItemSpacing(ROTOPAINT_VIEWER_UI_SECTIONS_SPACING_PX);
    addKnobToViewerUI(colorWheel);
    colorWheel->setInViewerContextItemSpacing(ROTOPAINT_VIEWER_UI_SECTIONS_SPACING_PX);
    addKnobToViewerUI(blendingModes);
    blendingModes->setInViewerContextItemSpacing(ROTOPAINT_VIEWER_UI_SECTIONS_SPACING_PX);
    addKnobToViewerUI(opacityKnob);
    opacityKnob->setInViewerContextItemSpacing(1);
    addKnobToViewerUI(pressureOpacity);
    pressureOpacity->setInViewerContextItemSpacing(ROTOPAINT_VIEWER_UI_SECTIONS_SPACING_PX);
    addKnobToViewerUI(sizeKnob);
    sizeKnob->setInViewerContextItemSpacing(1);
    addKnobToViewerUI(pressureSize);
    pressureSize->setInViewerContextItemSpacing(ROTOPAINT_VIEWER_UI_SECTIONS_SPACING_PX);
    addKnobToViewerUI(hardnessKnob);
    hardnessKnob->setInViewerContextItemSpacing(1);
    addKnobToViewerUI(pressureHardness);
    pressureHardness->setInViewerContextItemSpacing(ROTOPAINT_VIEWER_UI_SECTIONS_SPACING_PX);
    addKnobToViewerUI(buildUp);
    buildUp->setInViewerContextItemSpacing(ROTOPAINT_VIEWER_UI_SECTIONS_SPACING_PX);
    addKnobToViewerUI(autoConnectViewer);
    autoConnectViewer->setInViewerContextItemSpacing(ROTOPAINT_VIEWER_UI_SECTIONS_SPACING_PX);
    addKnobToViewerUI(effectStrength);
    effectStrength->setInViewerContextItemSpacing(ROTOPAINT_VIEWER_UI_SECTIONS_SPACING_PX);
    addKnobToViewerUI(timeOffsetSb);
    timeOffsetSb->setInViewerContextItemSpacing(1);
    addKnobToViewerUI(timeOffsetMode);
    addKnobToViewerUI(sourceType);
    addKnobToViewerUI(resetCloneOffset);

    _vectorFollow.reset(new VectorFollowControls(this, _imp->ui.get(), ROTOPAINT_VECTORS_INPUT_INDEX));
    KnobPagePtr shapePage = std::dynamic_pointer_cast<KnobPage>(getKnobByName("Shape"));
    assert(shapePage);
    _vectorFollow->createKnobs(generalPage, shapePage ? shapePage : generalPage);

    KnobPagePtr toolbar = _imp->ui->createToolbarKnobs(eRotoCurveToolSetAll | eRotoCurveToolSetObjectSelect);
    _imp->ui->createObjectSelectKnobs(generalPage);

    KnobGroupPtr paintToolButton = AppManager::createKnob<KnobGroup>( this, tr(kRotoUIParamPaintBrushToolButtonLabel) );
    paintToolButton->setName(kRotoUIParamPaintBrushToolButton);
    paintToolButton->setAsToolButton(true);
    paintToolButton->setEvaluateOnChange(false);
    paintToolButton->setSecretByDefault(true);
    paintToolButton->setInViewerContextCanHaveShortcut(true);
    paintToolButton->setIsPersistent(false);
    toolbar->addKnob(paintToolButton);
    _imp->ui->paintBrushToolGroup = paintToolButton;

    KnobGroupPtr cloneToolButton, effectToolButton, mergeToolButton;
    if (_imp->isPaintByDefault) {
        cloneToolButton = AppManager::createKnob<KnobGroup>( this, tr(kRotoUIParamCloneBrushToolButtonLabel) );
        cloneToolButton->setName(kRotoUIParamCloneBrushToolButton);
        cloneToolButton->setAsToolButton(true);
        cloneToolButton->setEvaluateOnChange(false);
        cloneToolButton->setSecretByDefault(true);
        cloneToolButton->setInViewerContextCanHaveShortcut(true);
        cloneToolButton->setIsPersistent(false);
        toolbar->addKnob(cloneToolButton);
        _imp->ui->cloneBrushToolGroup = cloneToolButton;
        effectToolButton = AppManager::createKnob<KnobGroup>( this, tr(kRotoUIParamEffectBrushToolButtonLabel) );
        effectToolButton->setName(kRotoUIParamEffectBrushToolButton);
        effectToolButton->setAsToolButton(true);
        effectToolButton->setEvaluateOnChange(false);
        effectToolButton->setSecretByDefault(true);
        effectToolButton->setInViewerContextCanHaveShortcut(true);
        effectToolButton->setIsPersistent(false);
        toolbar->addKnob(effectToolButton);
        _imp->ui->effectBrushToolGroup = effectToolButton;
        mergeToolButton = AppManager::createKnob<KnobGroup>( this, tr(kRotoUIParamMergeBrushToolButtonLabel) );
        mergeToolButton->setName(kRotoUIParamMergeBrushToolButton);
        mergeToolButton->setAsToolButton(true);
        mergeToolButton->setEvaluateOnChange(false);
        mergeToolButton->setSecretByDefault(true);
        mergeToolButton->setInViewerContextCanHaveShortcut(true);
        mergeToolButton->setIsPersistent(false);
        toolbar->addKnob(mergeToolButton);
        _imp->ui->mergeBrushToolGroup = mergeToolButton;
    }

    {
        KnobButtonPtr tool = AppManager::createKnob<KnobButton>( this, tr(kRotoUIParamDrawBrushToolButtonActionLabel) );
        tool->setName(kRotoUIParamDrawBrushToolButtonAction);
        tool->setHintToolTip( tr(kRotoUIParamDrawBrushToolButtonActionHint) );
        tool->setCheckable(true);
        tool->setDefaultValue(true);
        tool->setSecretByDefault(true);
        tool->setEvaluateOnChange(false);
        tool->setIconLabel(NATRON_ICONS_PATH "lucide/brush.svg");
        tool->setIsPersistent(false);
        paintToolButton->addKnob(tool);
        _imp->ui->brushAction = tool;
    }
    {
        KnobButtonPtr tool = AppManager::createKnob<KnobButton>( this, tr(kRotoUIParamPencilToolButtonActionLabel) );
        tool->setName(kRotoUIParamPencilToolButtonAction);
        tool->setHintToolTip( tr(kRotoUIParamPencilToolButtonAction) );
        tool->setCheckable(true);
        tool->setDefaultValue(false);
        tool->setSecretByDefault(true);
        tool->setEvaluateOnChange(false);
        tool->setIconLabel(NATRON_ICONS_PATH "lucide/pencil.svg");
        tool->setIsPersistent(false);
        paintToolButton->addKnob(tool);
        _imp->ui->pencilAction = tool;
    }

    {
        KnobButtonPtr tool = AppManager::createKnob<KnobButton>( this, tr(kRotoUIParamEraserToolButtonActionLabel) );
        tool->setName(kRotoUIParamEraserToolButtonAction);
        tool->setHintToolTip( tr(kRotoUIParamEraserToolButtonActionHint) );
        tool->setCheckable(true);
        tool->setDefaultValue(false);
        tool->setSecretByDefault(true);
        tool->setEvaluateOnChange(false);
        tool->setIconLabel(NATRON_ICONS_PATH "lucide/eraser.svg");
        tool->setIsPersistent(false);
        paintToolButton->addKnob(tool);
        _imp->ui->eraserAction = tool;
    }
    if (_imp->isPaintByDefault) {
        {
            KnobButtonPtr tool = AppManager::createKnob<KnobButton>( this, tr(kRotoUIParamCloneToolButtonActionLabel) );
            tool->setName(kRotoUIParamCloneToolButtonAction);
            tool->setHintToolTip( tr(kRotoUIParamCloneToolButtonActionHint) );
            tool->setCheckable(true);
            tool->setDefaultValue(true);
            tool->setSecretByDefault(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel(NATRON_ICONS_PATH "natron/clone-stamp.svg");
            tool->setIsPersistent(false);
            cloneToolButton->addKnob(tool);
            _imp->ui->cloneAction = tool;
        }
        {
            KnobButtonPtr tool = AppManager::createKnob<KnobButton>( this, tr(kRotoUIParamRevealToolButtonAction) );
            tool->setName(kRotoUIParamRevealToolButtonAction);
            tool->setHintToolTip( tr(kRotoUIParamRevealToolButtonActionHint) );
            tool->setCheckable(true);
            tool->setDefaultValue(false);
            tool->setSecretByDefault(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel(NATRON_ICONS_PATH "lucide/stamp.svg");
            tool->setIsPersistent(false);
            cloneToolButton->addKnob(tool);
            _imp->ui->revealAction = tool;
        }


        {
            KnobButtonPtr tool = AppManager::createKnob<KnobButton>( this, tr(kRotoUIParamBlurToolButtonActionLabel) );
            tool->setName(kRotoUIParamBlurToolButtonAction);
            tool->setHintToolTip( tr(kRotoUIParamBlurToolButtonActionHint) );
            tool->setCheckable(true);
            tool->setDefaultValue(true);
            tool->setSecretByDefault(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel(NATRON_ICONS_PATH "lucide/droplet.svg");
            tool->setIsPersistent(false);
            effectToolButton->addKnob(tool);
            _imp->ui->blurAction = tool;
        }
        {
            KnobButtonPtr tool = AppManager::createKnob<KnobButton>( this, tr(kRotoUIParamSmearToolButtonActionLabel) );
            tool->setName(kRotoUIParamSmearToolButtonAction);
            tool->setHintToolTip( tr(kRotoUIParamSmearToolButtonActionHint) );
            tool->setCheckable(true);
            tool->setDefaultValue(false);
            tool->setSecretByDefault(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel(NATRON_ICONS_PATH "lucide/pointer.svg");
            tool->setIsPersistent(false);
            effectToolButton->addKnob(tool);
            _imp->ui->smearAction = tool;
        }
        {
            KnobButtonPtr tool = AppManager::createKnob<KnobButton>( this, tr(kRotoUIParamDodgeToolButtonActionLabel) );
            tool->setName(kRotoUIParamDodgeToolButtonAction);
            tool->setHintToolTip( tr(kRotoUIParamDodgeToolButtonActionHint) );
            tool->setCheckable(true);
            tool->setDefaultValue(true);
            tool->setSecretByDefault(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel(NATRON_ICONS_PATH "lucide/sun.svg");
            tool->setIsPersistent(false);
            mergeToolButton->addKnob(tool);
            _imp->ui->dodgeAction = tool;
        }
        {
            KnobButtonPtr tool = AppManager::createKnob<KnobButton>( this, tr(kRotoUIParamBurnToolButtonActionLabel) );
            tool->setName(kRotoUIParamBurnToolButtonAction);
            tool->setHintToolTip( tr(kRotoUIParamBurnToolButtonActionHint) );
            tool->setCheckable(true);
            tool->setDefaultValue(false);
            tool->setSecretByDefault(true);
            tool->setEvaluateOnChange(false);
            tool->setIconLabel(NATRON_ICONS_PATH "lucide/flame.svg");
            tool->setIsPersistent(false);
            mergeToolButton->addKnob(tool);
            _imp->ui->burnAction = tool;
        }
    } // if (_imp->isPaintByDefault) {

    _imp->ui->createRightClickMenuKnobs(generalPage);

    KnobButtonPtr defaultAction;
    KnobGroupPtr defaultRole;
    if (_imp->isPaintByDefault) {
        defaultAction = _imp->ui->brushAction.lock();
        defaultRole = _imp->ui->paintBrushToolGroup.lock();
    } else {
        defaultAction = _imp->ui->drawBezierAction.lock();
        defaultRole = _imp->ui->bezierEditionToolGroup.lock();
    }
    _imp->ui->setCurrentTool(defaultAction);
    _imp->ui->onRoleChangedInternal(defaultRole);
    _imp->ui->onToolChangedInternal(defaultAction);
} // RotoPaint::initializeKnobs

void
RotoPaint::getPluginShortcuts(std::list<PluginActionShortcut>* shortcuts)
{
    RotoCurveInteract::getViewerButtonShortcuts(shortcuts);

    shortcuts->push_back( PluginActionShortcut(kRotoUIParamPressureOpacity, kRotoUIParamPressureOpacityLabel) );
    shortcuts->push_back( PluginActionShortcut(kRotoUIParamPressureSize, kRotoUIParamPressureSizeLabel) );
    shortcuts->push_back( PluginActionShortcut(kRotoUIParamPressureHardness, kRotoUIParamPressureHardnessLabel) );
    shortcuts->push_back( PluginActionShortcut(kRotoUIParamBuildUp, kRotoUIParamBuildUpLabel) );
    shortcuts->push_back( PluginActionShortcut(kRotoUIParamResetCloneOffset, kRotoUIParamResetCloneOffsetLabel) );
    VectorFollowControls::getShortcuts(shortcuts);

    RotoCurveInteract::getToolbarShortcuts(eRotoCurveToolSetAll | eRotoCurveToolSetObjectSelect, shortcuts);
    shortcuts->push_back( PluginActionShortcut(kRotoUIParamPaintBrushToolButton, kRotoUIParamPaintBrushToolButtonLabel, Key_N) );
    shortcuts->push_back( PluginActionShortcut(kRotoUIParamCloneBrushToolButton, kRotoUIParamCloneBrushToolButtonLabel, Key_C) );
    shortcuts->push_back( PluginActionShortcut(kRotoUIParamEffectBrushToolButton, kRotoUIParamEffectBrushToolButtonLabel, Key_X) );
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamMergeBrushToolButton, kRotoUIParamMergeBrushToolButtonLabel, Key_D));

    RotoCurveInteract::getRightClickMenuShortcuts(shortcuts);
}

bool
RotoPaint::shouldPreferPluginOverlayOverHostOverlay() const
{
    // Ctrl-drags on the points' transform box (pivot, corners) go to the roto overlay before the
    // shape's transform jack.
    return !_imp->ui->ctrlDown || _imp->ui->showCpsBbox;
}

bool
RotoPaint::shouldDrawHostOverlay() const
{
    KnobButtonPtr b = _imp->ui->showTransformHandle.lock();
    if (!b) {
        return true;
    }
    return b->getValue();
}

void
RotoPaint::onEffectCreated(bool /*mayCreateFileDialog*/,
                           const CreateNodeArgs& args)
{
    _imp->ui->onHostEffectCreated(args);
}

void
RotoPaint::onKnobsLoaded()
{
    _imp->ui->selectedItems = getNode()->getRotoContext()->getSelectedCurves();

    // When loading a roto node, always switch to selection mode by default
    KnobButtonPtr defaultAction = _imp->ui->selectAllAction.lock();
    KnobGroupPtr defaultRole = _imp->ui->selectToolGroup.lock();;

    _imp->ui->setCurrentTool(defaultAction);
    _imp->ui->onRoleChangedInternal(defaultRole);
    _imp->ui->onToolChangedInternal(defaultAction);
    if (_vectorFollow) {
        _vectorFollow->onKnobsLoaded();
    }
}

bool
RotoPaint::knobChanged(KnobI* k,
                       ValueChangedReasonEnum reason,
                       ViewSpec view,
                       double time,
                       bool originatedFromMainThread)
{
    if (!k) {
        return false;
    }


    RotoContextPtr ctx = getNode()->getRotoContext();

    if (!ctx) {
        return false;
    }
    bool ret = false;
    if (_imp->ui->onKnobChanged(k, time, &ret)) {
        return ret;
    }
    if (_vectorFollow && _vectorFollow->onKnobChanged(k)) {
        return true;
    }

    return ctx->knobChanged(k, reason, view, time, originatedFromMainThread);
} // RotoPaint::knobChanged

void
RotoPaint::refreshExtraStateAfterTimeChanged(bool isPlayback,
                                             double time)
{
    EffectInstance::refreshExtraStateAfterTimeChanged(isPlayback, time);
    _imp->ui->refreshAfterTimeChanged(time);
}

StatusEnum
RotoPaint::getPreferredMetadataAt(double /*time*/,
                                  ViewIdx /*view*/,
                                  NodeMetadata& metadata)
{
    metadata.setNComps( -1, 4 );
    metadata.setComponentsType(-1, eImageComponentsTypeColor);

    return eStatusOK;
}

void
RotoPaint::onInputChanged(int inputNb)
{
    NodePtr inputNode = getNode()->getInput(0);

    getNode()->getRotoContext()->onRotoPaintInputChanged(inputNode);
    _imp->ui->onHostInputChanged();
    EffectInstance::onInputChanged(inputNb);
}

StatusEnum
RotoPaint::getRegionOfDefinition(U64 /*hash*/,
                                 double time,
                                 const RenderScale& scale,
                                 ViewIdx view,
                                 RectD* rod)
{
    // The background inputs' union, without the Vectors input: only the follow tools read it.
    // An input whose region fails adds nothing instead of failing the whole union: an empty
    // region (a disabled node, an unconnected Read, an empty Roto) answers as a failure, and
    // one such extra Bg would otherwise drop the main Bg and leave only the shapes' region.
    rod->clear();
    bool first = true;
    for (int i = 0; i < getNInputs(); ++i) {
        if (isInputMask(i) || (i == ROTOPAINT_VECTORS_INPUT_INDEX)) {
            continue;
        }
        EffectInstancePtr input = getInput(i);
        if (!input) {
            continue;
        }
        RectD inputRod;
        bool isProjectFormat;
        StatusEnum st;
        {
            RenderContextPtr inputContext;
            const bool overlaid = getInputActionContext(i, time, view, &inputContext);
            ActionContextScope acting(overlaid, inputContext);
            st = input->getRegionOfDefinition_public(input->getRenderHash(), time, scale, view, &inputRod, &isProjectFormat);
        }
        if ((st == eStatusFailed) || inputRod.isNull()) {
            continue;
        }
        if (first) {
            *rod = inputRod;
            first = false;
        } else {
            rod->merge(inputRod);
        }
    }
    RectD maskRod;
    getNode()->getRotoContext()->getMaskRegionOfDefinition(time, view, &maskRod);
    // The shapes are zero past their outlines, so their region keeps a zero border: reading
    // past it under Stretch then repeats zero instead of a shape.
    addOutsideColourBorder(getRenderMetadata(time, view)->getPixelAspectRatio(-1), scale, &maskRod);
    if ( rod->isNull() ) {
        *rod = maskRod;
    } else {
        rod->merge(maskRod);
    }

    return eStatusOK;
}

FramesNeededMap
RotoPaint::getFramesNeeded(double time,
                           ViewIdx view)
{
    FramesNeededMap ret;
    FrameRangesMap views;
    OfxRangeD range;

    range.min = range.max = time;
    views[view].push_back(range);
    ret.insert( std::make_pair(0, views) );

    return ret;
}

void
RotoPaint::getRegionsOfInterest(double time,
                                const RenderScale & scale,
                                const RectD & outputRoD, //!< the RoD of the effect, in canonical coordinates
                                const RectD & renderWindow, //!< the region to be rendered in the output image, in Canonical Coordinates
                                ViewIdx view,
                                RoIMap* ret)
{
    RotoContextPtr roto = getNode()->getRotoContext();
    NodePtr bottomMerge = roto->getRotoPaintBottomMergeNode();

    if (bottomMerge) {
        ret->insert( std::make_pair(bottomMerge->getEffectInstance(), renderWindow) );
    }
    EffectInstance::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    // The vectors are read by the follow tools, never rendered with the shapes.
    EffectInstancePtr vectors = getInput(ROTOPAINT_VECTORS_INPUT_INDEX);
    if (!vectors) {
        return;
    }
    for (int i = 0; i < getNInputs(); ++i) {
        if ((i != ROTOPAINT_VECTORS_INPUT_INDEX) && (getInput(i) == vectors)) {
            return;
        }
    }
    if (bottomMerge && (bottomMerge->getEffectInstance() == vectors)) {
        return;
    }
    ret->erase(vectors);
}

bool
RotoPaint::isIdentity(double time,
                      const RenderScale & scale,
                      const RectI & roi,
                      ViewIdx view,
                      double* inputTime,
                      ViewIdx* inputView,
                      int* inputNb)
{
    *inputView = view;
    NodePtr node = getNode();
    EffectInstancePtr maskInput = getInput(ROTOPAINT_MASK_INPUT_INDEX);
    if (maskInput) {
        RectD maskRod;
        bool isProjectFormat;
        StatusEnum s = maskInput->getRegionOfDefinition_public(maskInput->getRenderHash(), time, scale, view, &maskRod, &isProjectFormat);
        Q_UNUSED(s);
        const RectI maskPixelRod = maskRod.toPixelEnclosing(scale, getRenderMetadata(time, view)->getPixelAspectRatio(ROTOPAINT_MASK_INPUT_INDEX));
        if ( !maskPixelRod.intersects(roi) ) {
            *inputTime = time;
            *inputNb = 0;

            return true;
        }
    }

    std::list<RotoDrawableItemPtr> items = node->getRotoContext()->getCurvesByRenderOrder();
    if ( items.empty() ) {
        *inputNb = 0;
        *inputTime = time;

        return true;
    }

    return false;
}

namespace {

std::atomic<bool> gDirectCompositeEnabled(true);
std::atomic<unsigned long long> gDirectCompositeRenders(0);

// Rows between abort checks of a direct composite band.
const int kDirectAbortCheckRows = 16;

// An input of one Merge's fold as the Merge reads it. An item's input is its mask: its Roto
// effect, having no source, outputs the mask with its source merged under, which leaves a
// non-negative mask value unchanged, and zero where the mask has no pixel. A Merge reads that
// output past the effect's region by edgeMode, so the region, not the mask's bounds, is where
// reads clamp to or stop.
struct DirectFoldInput {
    ImagePtr image;
    std::shared_ptr<Image::ReadAccess> access;
    RectI bounds;
    int nComps = 0;
    EdgeMode edgeMode = EdgeMode::Black;
    bool isItem = false;
    RectI region;
    // The channel of the input that is the alpha of every channel, -1 for an alpha of zero.
    int alphaChannel = -1;

    void lock()
    {
        if (image) {
            bounds = image->getBounds();
            nComps = (int)image->getComponentsCount();
            access = std::make_shared<Image::ReadAccess>(image.get());
        }
    }

    void readItemSpan(int x0,
                      int y,
                      int width,
                      float* row) const
    {
        PassExec::readSourceRow(image.get(), access.get(), bounds, nComps, x0, y, width, 4, EdgeMode::Black, row);
    }

    void readRow(int x0,
                 int y,
                 int width,
                 float* row) const
    {
        const std::size_t rowSize = (std::size_t)width * 4;
        if (!isItem) {
            PassExec::readSourceRow(image.get(), access.get(), bounds, nComps, x0, y, width, 4, edgeMode, row);

            return;
        }
        int sy = 0;
        if (!image || !EdgeSample::coord(y, region.y1, region.y2, edgeMode, &sy) || (region.x2 <= region.x1)) {
            std::fill(row, row + rowSize, 0.f);

            return;
        }
        const int x2 = x0 + width;
        const int inX1 = std::max(x0, region.x1);
        const int inX2 = std::min(x2, region.x2);
        if (inX1 < inX2) {
            readItemSpan(inX1, sy, inX2 - inX1, row + (std::size_t)(inX1 - x0) * 4);
        }
        const auto fillOutside = [&](int from, int to, int edgeX) {
            if (from >= to) {
                return;
            }
            float* out = row + (std::size_t)(from - x0) * 4;
            const std::size_t count = (std::size_t)(to - from) * 4;
            if (edgeMode == EdgeMode::Black) {
                std::fill(out, out + count, 0.f);

                return;
            }
            float edge[4];
            readItemSpan(edgeX, sy, 1, edge);
            for (std::size_t i = 0; i < count; i += 4) {
                std::copy(edge, edge + 4, out + i);
            }
        };
        fillOutside(x0, std::min(x2, region.x1), region.x1);
        fillOutside(std::max(x0, region.x2), x2, region.x2 - 1);
    }

    RowIO::Alpha alphaOf(const float* row,
                         const float* zeroAlpha) const
    {
        RowIO::Alpha span;
        if (alphaChannel < 0) {
            span.data = zeroAlpha;
            span.pixelStride = 1;
        } else {
            span.data = row + alphaChannel;
            span.pixelStride = 4;
        }

        return span;
    }
};

// One global Merge of the tree as its CPU render folds its inputs over window.
struct DirectMerge {
    NativeMerge* merge = nullptr;
    NativeMergeOp::Params params;
    std::vector<DirectFoldInput> inputs;
    bool foldAlpha = false;
    RectI window;
    RectD rod;
    double par = 1.;
};

bool
isFloatRGBA(const ImagePtr& image)
{
    return !image || (PassExec::isFloatImage(image) && (image->getComponentsCount() == 4));
}

// Whether the Roto effect of an item writes every channel of its output.
bool
rotoEffectProcessesEveryChannel(const NodePtr& effectNode,
                                double time)
{
    const char* names[] = { kNatronOfxParamProcessR, kNatronOfxParamProcessG, kNatronOfxParamProcessB, kNatronOfxParamProcessA };
    for (const char* name : names) {
        KnobBoolPtr knob = std::dynamic_pointer_cast<KnobBool>(effectNode->getKnobByName(name));
        if (knob && !knob->getValueAtTime(time)) {
            return false;
        }
    }

    return true;
}

enum class DirectTreeResult {
    NodePath,
    Done,
    Aborted
};

/*
 * Composites a concatenated tree as its global Merges would, folding each item's cached mask in
 * place of rendering the item's Roto effect and each Merge. Anything the fold below does not
 * reproduce exactly answers NodePath before any pixel is written.
 */
DirectTreeResult
renderConcatenatedTree(RotoPaint& self,
                       const EffectInstance::RenderActionArgs& args,
                       const RotoContextPtr& roto,
                       const std::list<RotoDrawableItemPtr>& items,
                       std::map<ImageLayerDesc, ImagePtr>* images)
{
    if (!RotoPaint::isDirectCompositeEnabled() || (args.outputLayers.size() != 1) || roto->isDoingNeatRender() || self.isDuringPaintStrokeCreationThreadLocal()) {
        return DirectTreeResult::NodePath;
    }
    const ImageLayerDesc& plane = args.outputLayers.front().first;
    const ImagePtr& output = args.outputLayers.front().second;
    if (!output || !plane.isColorLayer() || (plane.getNumComponents() != 4) || !PassExec::isFloatImage(output) || (output->getComponentsCount() != 4)) {
        return DirectTreeResult::NodePath;
    }
    const NodesList chain = roto->getRotoPaintMergeChain(items);
    if (chain.empty()) {
        return DirectTreeResult::NodePath;
    }
    const double time = args.time;
    const ViewIdx view = args.view;
    const unsigned int mipmapLevel = args.mappedScale.toMipmapLevel();

    std::map<const EffectInstance*, RotoDrawableItemPtr> itemOfEffect;
    for (const RotoDrawableItemPtr& item : items) {
        const NodePtr effectNode = item->getEffectNode();
        if (!effectNode) {
            return DirectTreeResult::NodePath;
        }
        itemOfEffect[effectNode->getEffectInstance().get()] = item;
    }

    bool bReads = false;
    std::vector<DirectMerge> merges;
    NodePtr previousNode;
    for (const NodePtr& node : chain) {
        DirectMerge direct;
        direct.merge = dynamic_cast<NativeMerge*>(node->getEffectInstance().get());
        if (!direct.merge || direct.merge->isDuringPaintStrokeCreationThreadLocal()) {
            return DirectTreeResult::NodePath;
        }
        NativeMerge& merge = *direct.merge;
        const U64 hash = merge.getRenderHash();
        const std::bitset<4> processChannels = merge.getProcessChannelsForPlane(hash, time, view, plane);
        if (!processChannels.all()) {
            return DirectTreeResult::NodePath;
        }
        std::optional<std::string> error;
        const StagePlan plan = merge.resolveCompositeRenderPlan(time, view, plane, processChannels, &error);
        if (error || plan.applyMask || (plan.mix != 1.) || (plan.divisorChannel >= 0) || plan.compositeResultIsAlpha || (plan.composite.size() < 2) || (plan.composite[0].inputNb != 0)) {
            return DirectTreeResult::NodePath;
        }
        const NodeMetadataConstPtr metadata = merge.getRenderMetadata(time, view);
        bool isProjectFormat = false;
        if (!metadata || (merge.getRegionOfDefinition_public(hash, time, args.mappedScale, view, &direct.rod, &isProjectFormat) == eStatusFailed)) {
            return DirectTreeResult::NodePath;
        }
        direct.par = metadata->getPixelAspectRatio(-1);
        direct.window = args.roi.intersect(direct.rod.toPixelEnclosing(mipmapLevel, direct.par));
        if (direct.window.isNull()) {
            return DirectTreeResult::NodePath;
        }
        direct.params = merge.mergeParams(time, view);
        direct.foldAlpha = stageSourcesHaveAlpha(plan);
        const EffectInstancePtr expectedB = previousNode ? previousNode->getEffectInstance() : self.getInput(0);
        if (merge.getInput(0) != expectedB) {
            return DirectTreeResult::NodePath;
        }
        previousNode = node;

        for (std::size_t s = 0; s < plan.composite.size(); ++s) {
            const StageSource& source = plan.composite[s];
            const StageSourceLayers layers = resolveStageSourceLayers(merge, source, plan.plane, time, view);
            DirectFoldInput input;
            input.edgeMode = source.edgeMode;
            if (layers.read && (!layers.layer.isColorLayer() || (layers.layer.getNumComponents() != 4))) {
                return DirectTreeResult::NodePath;
            }
            if (direct.foldAlpha && layers.hasAlpha()) {
                if (!layers.read || layers.alphaPerChannel() || !(layers.alphaLayer == layers.layer) || (layers.alphaChannel >= 4)) {
                    return DirectTreeResult::NodePath;
                }
                input.alphaChannel = layers.alphaChannel;
            }
            if (s == 0) {
                // A later Merge's B is the Merge before it, folded first; the first one's is
                // RotoPaint's own input, fetched below unless it is disconnected.
                if (!layers.read && !merges.empty()) {
                    return DirectTreeResult::NodePath;
                }
                if (merges.empty()) {
                    bReads = layers.read;
                }
                direct.inputs.push_back(input);
                continue;
            }
            const EffectInstancePtr effect = merge.getInput(source.inputNb);
            std::map<const EffectInstance*, RotoDrawableItemPtr>::const_iterator found = effect ? itemOfEffect.find(effect.get()) : itemOfEffect.end();
            if (!layers.read || (found == itemOfEffect.end())) {
                return DirectTreeResult::NodePath;
            }
            input.isItem = true;
            const RotoDrawableItemPtr& item = found->second;
            const NodePtr effectNode = effect->getNode();
            if (!item->isActivated(time) || effectNode->isNodeDisabled(time)) {
                // Bypassed, the effect shows its disconnected source: nothing.
                direct.inputs.push_back(input);
                continue;
            }
            const NodeMetadataConstPtr effectMetadata = effect->getRenderMetadata(time, view);
            const int rotoInput = effect->getRotoBrushInputIndex();
            if (!effectMetadata || (rotoInput < 0) || (effectMetadata->getNComps(-1) != 4) || ((mipmapLevel != 0) && !effect->supportsRenderScale()) || !rotoEffectProcessesEveryChannel(effectNode, time)) {
                return DirectTreeResult::NodePath;
            }
            const ImageBitDepthEnum maskDepth = effectMetadata->getBitDepth(rotoInput);
            ImageLayerDesc maskComps, maskPairedComps;
            EffectInstance::getLayersFromMetadata(*effectMetadata, rotoInput, &maskComps, &maskPairedComps);
            if ((maskDepth != eImageBitDepthFloat) || (maskComps.getNumComponents() != 4)) {
                return DirectTreeResult::NodePath;
            }
            RectD effectRod;
            if (effect->getRegionOfDefinition_public(effect->getRenderHash(), time, args.mappedScale, view, &effectRod, &isProjectFormat) == eStatusFailed) {
                return DirectTreeResult::NodePath;
            }
            input.region = effectRod.toPixelEnclosing(mipmapLevel, effectMetadata->getPixelAspectRatio(-1));
            RectD rotoSrcRod;
            if (item->getInverted(time)) {
                const EffectInstancePtr rotoSource = effect->getInput(0);
                if (rotoSource) {
                    bool sourceIsProjectFormat = false;
                    StatusEnum st = rotoSource->getRegionOfDefinition_public(rotoSource->getRenderHash(), time, args.mappedScale, view, &rotoSrcRod, &sourceIsProjectFormat);
                    (void)st;
                }
            }
            if (self.aborted()) {
                return DirectTreeResult::Aborted;
            }
            input.image = item->renderMaskFromStroke(maskComps, time, view, maskDepth, mipmapLevel, rotoSrcRod);
            if (!isFloatRGBA(input.image)) {
                return DirectTreeResult::NodePath;
            }
            direct.inputs.push_back(input);
        }
        merges.push_back(direct);
    }

    if (bReads) {
        RectI bgRoI;
        DirectFoldInput& b = merges.front().inputs.front();
        b.image = self.getImage(0, time, args.mappedScale, view, 0, &plane, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, 0, &bgRoI);
        if (!isFloatRGBA(b.image)) {
            return DirectTreeResult::NodePath;
        }
        // Read past its edge, B clamps to the bounds of what was rendered of it, its own region's
        // edge only while that rendering meets the window.
        if ((b.edgeMode == EdgeMode::Stretch) && b.image && b.image->getBounds().intersect(merges.front().window).isNull()) {
            return DirectTreeResult::NodePath;
        }
    }

    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    RenderCancellation cancel(&self);
    ImagePtr previous;
    for (DirectMerge& direct : merges) {
        if (previous) {
            direct.inputs.front().image = previous;
        }
        for (DirectFoldInput& input : direct.inputs) {
            input.lock();
        }
        const RectI& window = direct.window;
        ImagePtr result = std::make_shared<Image>(output->getComponents(), direct.rod, window, mipmapLevel, direct.par, eImageBitDepthFloat, output->getFieldingOrder(), false, eStorageModeRAM);
        {
            Image::WriteAccess resultAccess(result.get());
            std::vector<RectI> bands;
            makeStageRowBands(window, nThreads, &bands);
            const int width = window.width();
            const std::size_t rowSize = (std::size_t)width * 4;
            const std::size_t nFolded = direct.inputs.size();
            const std::function<void(int)> renderBand = [&](int bandIndex) {
                const RectI& band = bands[bandIndex];
                std::vector<float> foldRows[2] = { std::vector<float>(rowSize), std::vector<float>(rowSize) };
                std::vector<float> inputRow(rowSize);
                std::vector<float> alphaRows[2];
                const std::vector<float> zeroAlpha(direct.foldAlpha ? width : 0, 0.f);
                if (direct.foldAlpha) {
                    alphaRows[0].resize(rowSize);
                    alphaRows[1].resize(rowSize);
                }
                RowIO io;
                io.x0 = window.x1;
                io.width = width;
                io.nComps = 4;
                io.channels.set();
                for (int y = band.y1; y < band.y2; ++y) {
                    if ((((y - band.y1) % kDirectAbortCheckRows) == 0) && cancel.check()) {
                        return;
                    }
                    float* dstPix = (float*)resultAccess.pixelAt(window.x1, y);
                    direct.inputs[0].readRow(window.x1, y, width, foldRows[0].data());
                    const float* resultRow = foldRows[0].data();
                    if (direct.foldAlpha) {
                        io.alpha[0] = direct.inputs[0].alphaOf(resultRow, zeroAlpha.data());
                    }
                    io.y = y;
                    int next = 1;
                    for (std::size_t s = 1; s < nFolded; ++s) {
                        direct.inputs[s].readRow(window.x1, y, width, inputRow.data());
                        float* stepRow = ((s + 1) == nFolded) ? dstPix : foldRows[next].data();
                        io.src[0] = resultRow;
                        io.src[1] = inputRow.data();
                        io.nSrc = 2;
                        io.dst = stepRow;
                        if (direct.foldAlpha) {
                            io.alpha[1] = direct.inputs[s].alphaOf(inputRow.data(), zeroAlpha.data());
                            io.dstAlpha = alphaRows[s & 1].data();
                        }
                        direct.merge->step(direct.params, io);
                        resultRow = stepRow;
                        next = 1 - next;
                        if (direct.foldAlpha) {
                            io.alpha[0].data = io.dstAlpha;
                            io.alpha[0].pixelStride = 4;
                            io.alpha[0].channelStride = 1;
                        }
                    }
                }
            };
            parallelForCancellable((int)bands.size(), nThreads, cancel, renderBand);
        }
        if (cancel.check()) {
            return DirectTreeResult::Aborted;
        }
        for (DirectFoldInput& input : direct.inputs) {
            input.access.reset();
        }
        previous = result;
    }
    (*images)[plane] = previous;
    ++gDirectCompositeRenders;

    return DirectTreeResult::Done;
} // renderConcatenatedTree
} // anonymous namespace

void
RotoPaint::setDirectCompositeEnabled(bool enabled)
{
    gDirectCompositeEnabled = enabled;
}

bool
RotoPaint::isDirectCompositeEnabled()
{
    return gDirectCompositeEnabled;
}

unsigned long long
RotoPaint::getDirectCompositeRenderCount()
{
    return gDirectCompositeRenders;
}

StatusEnum
RotoPaint::render(const RenderActionArgs& args)
{
    RotoRenderProfile::Scope profile(RotoRenderProfile::eKindRotoPaint, args.time, args.mappedScale.toMipmapLevel());
    std::optional<RotoRenderProfile::StageTimer> setupTimer;
    setupTimer.emplace(RotoRenderProfile::eStageSetup);
    RotoContextPtr roto = getNode()->getRotoContext();
    std::list<RotoDrawableItemPtr> items = roto->getCurvesByRenderOrder(false /*onlyActiveItems*/);
    setupTimer.reset();
    if (profile.active()) {
        profile.setNames(getNode()->getScriptName_mt_safe(), std::string(), std::string());
        profile.setItems((int)items.size());
        profile.setPixels((long long)args.roi.area());
    }
    ImageBitDepthEnum bgDepth = getRenderMetadata(args.time, args.view)->getBitDepth(0);
    std::list<ImageLayerDesc> neededComps;

    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator layer = args.outputLayers.begin();
         layer != args.outputLayers.end(); ++layer) {
        neededComps.push_back(layer->first);
    }

    if (items.empty()) {
        for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator layer = args.outputLayers.begin();
             layer != args.outputLayers.end(); ++layer) {
            RectI bgImgRoI;
            ImagePtr bgImg = getImage(0, args.time, args.mappedScale, args.view, 0, &layer->first, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM /*returnOpenGLtexture*/, 0 /*textureDepth*/, &bgImgRoI);
            if (bgImg) {
                if (bgImg->getComponents() != layer->second->getComponents()) {
                    bgImg->convertToFormat(args.roi, 3, false, layer->second.get());
                } else {
                    layer->second->pasteFrom(*bgImg, args.roi, false);
                }
            } else {
                layer->second->fillZero(args.roi);
            }
        }
    } else {
        NodesList rotoPaintNodes;
        setupTimer.emplace(RotoRenderProfile::eStageSetup);
        {
            bool ok = getThreadLocalRotoPaintTreeNodes(&rotoPaintNodes);
            if (!ok) {
                throw std::logic_error("RotoPaint::render(): getThreadLocalRotoPaintTreeNodes() failed");
            }
        }
        NodePtr bottomMerge = roto->getRotoPaintBottomMergeNode();
        setupTimer.reset();
        RenderingFlagSetter flagIsRendering(bottomMerge);

        unsigned int mipmapLevel = args.mappedScale.toMipmapLevel();
        RenderRoIArgs rotoPaintArgs(args.time,
                                    args.mappedScale,
                                    mipmapLevel,
                                    args.view,
                                    args.byPassCache,
                                    args.roi,
                                    RectD(),
                                    neededComps,
                                    bgDepth,
                                    false,
                                    this,
                                    eStorageModeRAM /*returnOpenGLtex*/,
                                    args.time);
        std::map<ImageLayerDesc, ImagePtr> rotoPaintImages;
        RenderRoIRetCode code = eRenderRoIRetCodeOk;
        {
            RotoRenderProfile::StageTimer tree(RotoRenderProfile::eStageTree);
            const DirectTreeResult direct = renderConcatenatedTree(*this, args, roto, items, &rotoPaintImages);
            if (direct == DirectTreeResult::Aborted) {
                return eStatusOK;
            }
            if (direct == DirectTreeResult::NodePath) {
                rotoPaintImages.clear();
                code = renderInputOrTakeFromStore(bottomMerge->getEffectInstance(), -1, &rotoPaintArgs, &rotoPaintImages, this);
            }
        }
        if (code == eRenderRoIRetCodeFailed) {
            return eStatusFailed;
        } else if (code == eRenderRoIRetCodeAborted) {
            return eStatusOK;
        } else if ( rotoPaintImages.empty() ) {
            for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator layer = args.outputLayers.begin();
                 layer != args.outputLayers.end(); ++layer) {
                layer->second->fillZero(args.roi);
            }

            return eStatusOK;
        }
        assert(rotoPaintImages.size() == args.outputLayers.size());

        const U64 hash = getRenderHash();

        for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator layer = args.outputLayers.begin();
             layer != args.outputLayers.end(); ++layer) {
            std::map<ImageLayerDesc, ImagePtr>::iterator rotoImagesIt = rotoPaintImages.find(layer->first);
            assert( rotoImagesIt != rotoPaintImages.end() );
            if ( rotoImagesIt == rotoPaintImages.end() ) {
                continue;
            }
            RectI bgImgRoI;
            ImagePtr bgImg = getImage(0, args.time, args.mappedScale, args.view, 0, &layer->first, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM /*returnOpenGLtexture*/, 0 /*textureDepth*/, &bgImgRoI);
            std::bitset<4> copyChannels = getProcessChannelsForPlane(hash, args.time, args.view, layer->first);
            // The bits follow the plane's own layout, where a one-channel plane's channel is bit 3,
            // but the host reads a one-channel non-Color plane back from channel 0 of the wider
            // image it hands this render (see channelForAlphaForPlane), so that is the channel the
            // bit must guard; the others are discarded by that conversion.
            if ((layer->first.getNumComponents() == 1) && !layer->first.isColorLayer() && (layer->second->getComponentsCount() > 1)) {
                const bool processed = copyChannels[3];
                copyChannels.set();
                copyChannels[0] = processed;
            }
            if ( !rotoImagesIt->second->getBounds().contains(args.roi) ) {
                // We first fill with the bg image because the bounds of the image produced by the last merge of the rotopaint tree
                // might not be equal to the bounds of the image produced by the rotopaint. This is because the RoD of the rotopaint is the
                // union of all the mask strokes bounds, whereas all nodes inside the rotopaint tree don't take the mask RoD into account.
                if (bgImg) {
                    RectI bgBounds = bgImg->getBounds();

                    // The bg bounds might not be inside the roi, but yet we need to fill the whole roi, so just fill borders
                    // with black and transparent, e.g:
                    /*
                        AAAAAAAAA
                        DDXXXXXBB
                        DDXXXXXBB
                        DDXXXXXBB
                        CCCCCCCCC
                     */
                    RectI merge = bgBounds;
                    merge.merge(args.roi);
                    RectI aRect;
                    aRect.x1 = merge.x1;
                    aRect.y1 = bgBounds.y2;
                    aRect.y2 = merge.y2;
                    aRect.x2 = merge.x2;

                    RectI bRect;
                    bRect.x1 = bgBounds.x2;
                    bRect.y1 = bgBounds.y1;
                    bRect.x2 = merge.x2;
                    bRect.y2 = bgBounds.y2;

                    RectI cRect;
                    cRect.x1 = merge.x1;
                    cRect.y1 = merge.y1;
                    cRect.x2 = merge.x2;
                    cRect.y2 = bgBounds.y1;

                    RectI dRect;
                    dRect.x1 = merge.x1;
                    dRect.y1 = bgBounds.y1;
                    dRect.x2 = bgBounds.x1;
                    dRect.y2 = bgBounds.y2;

                    layer->second->fillZero(aRect);
                    layer->second->fillZero(bRect);
                    layer->second->fillZero(cRect);
                    layer->second->fillZero(dRect);

                    if (bgImg->getComponents() != layer->second->getComponents()) {
                        const RectI intersection = args.roi.intersect(bgImg->getBounds());
                        if (!intersection.isNull()) {
                            bgImg->convertToFormat(intersection, 3, false, layer->second.get());
                        }
                    } else {
                        layer->second->pasteFrom(*bgImg, args.roi, false);
                    }
                } else {
                    layer->second->fillZero(args.roi);
                }
            }

            if (rotoImagesIt->second->getComponents() != layer->second->getComponents()) {
                rotoImagesIt->second->convertToFormat(args.roi, 3, false, layer->second.get());
            } else {
                layer->second->pasteFrom(*(rotoImagesIt->second), args.roi, false);
            }
            layer->second->copyUnProcessedChannels(args.roi, copyChannels, bgImg);
        }
    } // RenderingFlagSetter

    return eStatusOK;
} // RotoPaint::render

void
RotoPaint::clearLastRenderedImage()
{
    EffectInstance::clearLastRenderedImage();
    NodesList rotoPaintNodes;
    NodePtr node = getNode();

    if (node) {
        RotoContextPtr roto = node->getRotoContext();
        assert(roto);
        roto->getRotoPaintTreeNodes(&rotoPaintNodes);
        for (NodesList::iterator it = rotoPaintNodes.begin(); it != rotoPaintNodes.end(); ++it) {
            (*it)->clearLastRenderedImage();
        }
    }
}

void
RotoPaint::drawOverlay(double time,
                       const RenderScale & /*renderScale*/,
                       ViewIdx /*view*/)
{
    _imp->ui->drawOverlay(time);
}

void
RotoPaint::onInteractViewportSelectionCleared()
{
    _imp->ui->onViewportSelectionCleared();
}

void
RotoPaint::onInteractViewportSelectionUpdated(const RectD& rectangle,
                                              bool onRelease)
{
    _imp->ui->onViewportSelectionUpdated(rectangle, onRelease);
}

bool
RotoPaint::onOverlayPenDoubleClicked(double /*time*/,
                                     const RenderScale& /*renderScale*/,
                                     ViewIdx /*view*/,
                                     const QPointF& /*viewportPos*/,
                                     const QPointF& pos)
{
    return _imp->ui->onOverlayPenDoubleClicked(pos);
}

bool
RotoPaint::onOverlayPenDown(double time,
                            const RenderScale& /*renderScale*/,
                            ViewIdx /*view*/,
                            const QPointF& /*viewportPos*/,
                            const QPointF& pos,
                            double pressure,
                            double timestamp,
                            PenType pen)
{
    return _imp->ui->onOverlayPenDown(time, pos, pressure, timestamp, pen);
}

bool
RotoPaint::onOverlayClaimsAltPenDown(double time,
                                     const QPointF& pos)
{
    return _imp->ui->claimsAltPenDown(time, pos);
}

void
RotoPaint::onOverlayPenDownModifiers(KeyboardModifiers modifiers)
{
    _imp->ui->syncModifiers(modifiers);
}

bool
RotoPaint::onOverlayPenMotion(double time,
                              const RenderScale& /*renderScale*/,
                              ViewIdx /*view*/,
                              const QPointF& /*viewportPos*/,
                              const QPointF& pos,
                              double pressure,
                              double timestamp)
{
    return _imp->ui->onOverlayPenMotion(time, pos, pressure, timestamp);
}

bool
RotoPaint::onOverlayPenUp(double /*time*/,
                          const RenderScale& /*renderScale*/,
                          ViewIdx /*view*/,
                          const QPointF& /*viewportPos*/,
                          const QPointF& /*pos*/,
                          double /*pressure*/,
                          double /*timestamp*/)
{
    return _imp->ui->onOverlayPenUp();
}

bool
RotoPaint::onOverlayKeyDown(double /*time*/,
                            const RenderScale& /*renderScale*/,
                            ViewIdx /*view*/,
                            Key key,
                            KeyboardModifiers /*modifiers*/)
{
    return _imp->ui->onOverlayKeyDown(key);
}

bool
RotoPaint::onOverlayKeyUp(double /*time*/,
                          const RenderScale& /*renderScale*/,
                          ViewIdx /*view*/,
                          Key key,
                          KeyboardModifiers /*modifiers*/)
{
    return _imp->ui->onOverlayKeyUp(key);
}

bool
RotoPaint::onOverlayKeyRepeat(double /*time*/,
                              const RenderScale& /*renderScale*/,
                              ViewIdx /*view*/,
                              Key /*key*/,
                              KeyboardModifiers /*modifiers*/)
{
    return false;
} // onOverlayKeyRepeat

bool
RotoPaint::onOverlayFocusGained(double /*time*/,
                                const RenderScale& /*renderScale*/,
                                ViewIdx /*view*/)
{
    return false;
} // onOverlayFocusGained

bool
RotoPaint::onOverlayFocusLost(double /*time*/,
                              const RenderScale& /*renderScale*/,
                              ViewIdx /*view*/)
{
    return _imp->ui->onOverlayFocusLost();
}

RotoPaintInteractPtr
RotoPaint::getInteract() const
{
    return _imp->ui;
}

void
RotoPaint::abortObjectSelectEncoding(bool wait)
{
    if (_imp->ui && _imp->ui->objectSelect) {
        _imp->ui->objectSelect->abortEncoding(wait);
    }
}

bool
RotoPaint::isObjectSelectEncoding() const
{
    return _imp->ui && _imp->ui->objectSelect && _imp->ui->objectSelect->isEncoding();
}

RotoCurvePanelHook*
RotoPaint::getCurvePanelHook() const
{
    return _vectorFollow.get();
}

void
RotoPaint::onBreakMultiStrokeTriggered()
{
    _imp->ui->onBreakMultiStrokeTriggered();
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING
#include "moc_RotoPaint.cpp"

