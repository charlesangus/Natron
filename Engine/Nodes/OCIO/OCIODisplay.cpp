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

#include "OCIODisplay.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <exception>
#include <optional>
#include <vector>

#include "Global/FloatingPointExceptions.h"

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/ProjectColorManagement.h"

NATRON_NAMESPACE_ENTER

void
OCIODisplayOp::createKnobs(KnobHolder* holder,
                           const KnobPagePtr& page)
{
    OcioSpaceKnobs::Description in;
    in.prefix = kOCIODisplayParamInputSpace;
    in.label = EffectInstance::tr("Input Colorspace");
    in.spaceHint = EffectInstance::tr("The OpenColorIO colourspace the input is in. Empty is the working space.");
    in.menuHint = EffectInstance::tr("The OpenColorIO colourspace the input is in.");
    in.setLabel = EffectInstance::tr("Input Colorspace Set");
    // The working space until the user picks one, and again on a reset.
    ocio.addSpace(holder, page, in, []() { return std::optional<std::string>(std::string()); });

    KnobChoicePtr displayKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("Display").toStdString());
    displayKnob->setName(kOCIODisplayParamDisplay);
    displayKnob->setHintToolTip(EffectInstance::tr("The display device of the project's OpenColorIO config to show the image on.").toStdString());
    displayKnob->setAnimationEnabled(false);
    ocio.addKnob(page, displayKnob);
    display = displayKnob;

    KnobChoicePtr viewKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("View").toStdString());
    viewKnob->setName(kOCIODisplayParamView);
    viewKnob->setHintToolTip(EffectInstance::tr("The view transform of the display to apply.").toStdString());
    viewKnob->setAnimationEnabled(false);
    ocio.addKnob(page, viewKnob);
    view = viewKnob;

    KnobDoublePtr gainKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Gain").toStdString());
    gainKnob->setName(kOCIODisplayParamGain);
    gainKnob->setHintToolTip(EffectInstance::tr("Multiplies the input's RGB before the display transform, as the viewer's gain does.").toStdString());
    gainKnob->setDefaultValue(1.);
    gainKnob->setMinimum(0.);
    gainKnob->setMaximum(DBL_MAX);
    gainKnob->setDisplayMinimum(1. / 64.);
    gainKnob->setDisplayMaximum(64.);
    ocio.addKnob(page, gainKnob);
    gain = gainKnob;

    KnobDoublePtr gammaKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Gamma").toStdString());
    gammaKnob->setName(kOCIODisplayParamGamma);
    gammaKnob->setHintToolTip(EffectInstance::tr("Gamma applied after the display transform, as the viewer's gamma: each of RGB "
                                                 "becomes max(c, 0)^(1/gamma). A gamma of 0 shows 1 where c is at least 1 and 0 "
                                                 "elsewhere.")
                                  .toStdString());
    gammaKnob->setDefaultValue(1.);
    gammaKnob->setMinimum(0.);
    gammaKnob->setMaximum(DBL_MAX);
    gammaKnob->setDisplayMinimum(0.);
    gammaKnob->setDisplayMaximum(4.);
    ocio.addKnob(page, gammaKnob);
    gamma = gammaKnob;

    KnobChoicePtr channelKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("Channel View").toStdString());
    channelKnob->setName(kOCIODisplayParamChannel);
    channelKnob->setHintToolTip(EffectInstance::tr("The channels to view, taken after the gain and before the display transform. "
                                                   "A single channel or the luminance is shown in R, G and B.")
                                    .toStdString());
    std::vector<ChoiceOption> options;
    options.push_back(ChoiceOption("rgb", EffectInstance::tr("RGB").toStdString(), EffectInstance::tr("Color.").toStdString()));
    options.push_back(ChoiceOption("r", EffectInstance::tr("R").toStdString(), EffectInstance::tr("Red.").toStdString()));
    options.push_back(ChoiceOption("g", EffectInstance::tr("G").toStdString(), EffectInstance::tr("Green.").toStdString()));
    options.push_back(ChoiceOption("b", EffectInstance::tr("B").toStdString(), EffectInstance::tr("Blue.").toStdString()));
    options.push_back(ChoiceOption("a", EffectInstance::tr("A").toStdString(), EffectInstance::tr("Alpha; 1 for an RGB input.").toStdString()));
    options.push_back(ChoiceOption("luminance", EffectInstance::tr("Luminance").toStdString(), EffectInstance::tr("The luminance of RGB with the working space's weights.").toStdString()));
    channelKnob->populateChoices(options);
    channelKnob->setDefaultValue((int)eChannelRGB);
    channelKnob->setAnimationEnabled(false);
    ocio.addKnob(page, channelKnob);
    channel = channelKnob;

    refreshDisplayMenus();
    if (ProjectColorManagementPtr colorManagement = ocio.colorManagement()) {
        const std::string defaultDisplay = colorManagement->getDefaultDisplay();
        if (!defaultDisplay.empty()) {
            displayKnob->setDefaultValueFromID(defaultDisplay);
            OcioNodeKnobs::populateMenu(viewKnob, colorManagement->getViews(defaultDisplay));
            const std::string defaultView = colorManagement->getDefaultView(defaultDisplay);
            if (!defaultView.empty()) {
                viewKnob->setDefaultValueFromID(defaultView);
            }
        }
    }
    ocio.addMenuRefresher([this]() { refreshDisplayMenus(); });
}

std::string
OCIODisplayOp::chosenDisplay() const
{
    KnobChoicePtr knob = display.lock();

    return knob ? knob->getActiveEntry().id : std::string();
}

std::string
OCIODisplayOp::chosenView() const
{
    KnobChoicePtr knob = view.lock();

    return knob ? knob->getActiveEntry().id : std::string();
}

void
OCIODisplayOp::refreshDisplayMenus()
{
    ProjectColorManagementPtr colorManagement = ocio.colorManagement();

    if (!colorManagement) {
        return;
    }
    OcioNodeKnobs::populateMenu(display.lock(), colorManagement->getDisplays());
    OcioNodeKnobs::populateMenu(view.lock(), colorManagement->getViews(chosenDisplay()));
}

void
OCIODisplayOp::displayChanged()
{
    ProjectColorManagementPtr colorManagement = ocio.colorManagement();
    KnobChoicePtr viewKnob = view.lock();

    if (!colorManagement || !viewKnob) {
        return;
    }
    const std::string chosen = chosenDisplay();
    OcioNodeKnobs::populateMenu(viewKnob, colorManagement->getViews(chosen));
    if (!viewKnob->isActiveEntryPresentInEntries()) {
        viewKnob->setValueFromID(colorManagement->getDefaultView(chosen), 0);
    }
}

OCIODisplayOp::Params
OCIODisplayOp::bake(const KnobReader& reader) const
{
    Params p;

    p.gain = (float)reader.value(gain);
    p.gamma = reader.value(gamma);
    int channelIndex = reader.value(channel);
    if ((channelIndex < (int)eChannelRGB) || (channelIndex > (int)eChannelLuminance)) {
        channelIndex = (int)eChannelRGB;
    }
    p.channel = (ChannelEnum)channelIndex;

    double weights[3] = { 0.2126, 0.7152, 0.0722 };
    if (ProjectColorManagementPtr colorManagement = ocio.colorManagement()) {
        colorManagement->getWorkingLuma(weights);
    }
    for (int c = 0; c < 3; ++c) {
        p.luma[c] = (float)weights[c];
    }

    const std::string displayName = chosenDisplay();
    const std::string viewName = chosenView();
    if (!displayName.empty() && !viewName.empty()) {
        OCIO_NAMESPACE::DisplayViewTransformRcPtr transform = OCIO_NAMESPACE::DisplayViewTransform::Create();
        transform->setSrc(ocio.spaceOrWorking(0).c_str());
        transform->setDisplay(displayName.c_str());
        transform->setView(viewName.c_str());
        p.ocio = ocio.processor(transform, reader.time());
    }

    return p;
}

bool
OCIODisplayOp::isIdentity(const Params& params) const
{
    // A transform that cannot be built passes the input through, gain and gamma included.
    if (!params.ocio.error.empty()) {
        return true;
    }

    return !params.ocio.cpu && (params.gain == 1.f) && (params.gamma == 1.) && (params.channel == eChannelRGB);
}

void
OCIODisplayOp::row(const Params& params,
                   const RowIO& io) const
{
    const int nComps = io.nComps;

    if (io.dst != io.src[0]) {
        std::memcpy(io.dst, io.src[0], sizeof(float) * io.width * nComps);
    }
    if ((io.width <= 0) || ((nComps != 3) && (nComps != 4))) {
        return;
    }
    const float invGamma = (params.gamma > 0.) ? (float)(1. / params.gamma) : 0.f;
    try {
        const boost_adaptbx::floating_point::FpTrapGuard fpGuard;
        float* p = io.dst;
        for (int x = 0; x < io.width; ++x, p += nComps) {
            // As applyViewerDisplayTransform does, around the channel view.
            if (params.gain != 1.f) {
                p[0] *= params.gain;
                p[1] *= params.gain;
                p[2] *= params.gain;
            }
            float shown = 0.f;
            switch (params.channel) {
            case eChannelRGB:
                break;
            case eChannelR:
            case eChannelG:
            case eChannelB:
                shown = p[params.channel - eChannelR];
                p[0] = p[1] = p[2] = shown;
                break;
            case eChannelA:
                shown = (nComps == 4) ? p[3] : 1.f;
                p[0] = p[1] = p[2] = shown;
                break;
            case eChannelLuminance:
                shown = params.luma[0] * p[0] + params.luma[1] * p[1] + params.luma[2] * p[2];
                p[0] = p[1] = p[2] = shown;
                break;
            }
            if (params.ocio.cpu) {
                params.ocio.cpu->applyRGB(p);
            }
            if (params.gamma <= 0.) {
                for (int c = 0; c < 3; ++c) {
                    p[c] = (p[c] >= 1.f) ? 1.f : 0.f;
                }
            } else if (params.gamma != 1.) {
                for (int c = 0; c < 3; ++c) {
                    p[c] = std::pow(std::max(p[c], 0.f), invGamma);
                }
            }
        }
    } catch (const std::exception&) {
        // A row that fails keeps the source, as the node does when no processor can be built.
        if (io.dst != io.src[0]) {
            std::memcpy(io.dst, io.src[0], sizeof(float) * io.width * nComps);
        }
    }
}

GpuOpRef
OCIODisplayOp::gpu(const Params& params) const
{
    OcioDisplayAdjust adjust;

    adjust.gain = params.gain;
    adjust.gamma = params.gamma;
    adjust.channel = (int)params.channel;
    for (int c = 0; c < 3; ++c) {
        adjust.luma[c] = params.luma[c];
    }

    return ocio.gpu(params.ocio, &adjust);
}

OCIODisplay::OCIODisplay(NodePtr node)
    : OcioNode<OCIODisplayOp>(node)
{
}

OCIODisplay::~OCIODisplay()
{
}

OCIODisplayOp::Params
OCIODisplay::bakeDisplay(double time,
                         ViewIdx view) const
{
    return bakeParams(op(), time, view);
}

bool
OCIODisplay::knobChanged(KnobI* k,
                         ValueChangedReasonEnum reason,
                         ViewSpec view,
                         double time,
                         bool originatedFromMainThread)
{
    KnobChoicePtr displayKnob = op().display.lock();

    if (displayKnob && (k == displayKnob.get()) && (reason != eValueChangedReasonTimeChanged)) {
        op().displayChanged();
    }

    return OcioNode<OCIODisplayOp>::knobChanged(k, reason, view, time, originatedFromMainThread);
}

NATRON_NAMESPACE_EXIT
