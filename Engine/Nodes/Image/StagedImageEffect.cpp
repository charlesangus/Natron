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

#include "StagedImageEffect.h"

#include <string>

#include "Engine/KnobTypes.h"
#include "Engine/Node.h"

NATRON_NAMESPACE_ENTER

StagedImageEffect::StagedImageEffect(NodePtr node)
    : NativeEffectBase(node)
    , _stageKnobs()
{
}

StagedImageEffect::~StagedImageEffect()
{
}

void
StagedImageEffect::addAcceptedComponents(int inputNb,
                                         std::list<ImageLayerDesc>* comps)
{
    if (isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getAlphaComponents());

        return;
    }
    NativeEffectBase::addAcceptedComponents(inputNb, comps);
}

void
StagedImageEffect::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
StagedImageEffect::wantsHostUnPremultSelector() const
{
    return imagePipelineSpec().stages.has(Stage::Premult);
}

bool
StagedImageEffect::rendersUnprocessedChannels() const
{
    return imagePipelineSpec().stages.has(Stage::PassThroughUnprocessed);
}

LayerKnobSpec
StagedImageEffect::getLayerKnobSpec() const
{
    switch (imagePipelineSpec().layers.kind) {
    case LayerPolicy::Kind::TargetSelect:

        return LayerKnobSpec(LayerKnobSpec::eKindLayerSelect, LayerKnobSpec::eRoleTarget, true);
    case LayerPolicy::Kind::None:

        return LayerKnobSpec();
    case LayerPolicy::Kind::Color:
    case LayerPolicy::Kind::All:
        break;
    }

    return EffectInstance::getLayerKnobSpec();
}

bool
StagedImageEffect::defaultProcessesAllLayers() const
{
    return imagePipelineSpec().layers.kind == LayerPolicy::Kind::All;
}

bool
StagedImageEffect::isHostChannelSelectorSupported(bool* defaultR,
                                                  bool* defaultG,
                                                  bool* defaultB,
                                                  bool* defaultA) const
{
    const LayerPolicy& layers = imagePipelineSpec().layers;

    *defaultR = layers.defaults[0];
    *defaultG = layers.defaults[1];
    *defaultB = layers.defaults[2];
    *defaultA = layers.defaults[3];

    return layers.kind != LayerPolicy::Kind::None;
}

int
StagedImageEffect::getSourceInput() const
{
    const NodeSpec* spec = nodeSpec();

    return spec ? nodeSpecSourceInput(*spec) : 0;
}

int
StagedImageEffect::getMaskInput() const
{
    return stageMaskInput(*this);
}

void
StagedImageEffect::addMaskMixKnobs(const KnobPagePtr& page)
{
    KnobBoolPtr maskInvert = createKnob<KnobBool>(std::string("Invert Mask"));

    maskInvert->setName(kOfxMaskInvertParamName);
    maskInvert->setHintToolTip(tr("When checked, the effect is fully applied where the mask is 0."));
    maskInvert->setDefaultValue(false);
    if (page) {
        page->addKnob(maskInvert);
    }
    _stageKnobs.maskInvert = maskInvert;

    KnobDoublePtr mix = createKnob<KnobDouble>(std::string("Mix"));
    mix->setName(kOfxMixParamName);
    mix->setHintToolTip(tr("Mix factor between the original and the transformed image."));
    mix->setDefaultValue(1.);
    mix->setIncrement(0.01);
    mix->setMinimum(0.);
    mix->setMaximum(1.);
    mix->setDisplayMinimum(0.);
    mix->setDisplayMaximum(1.);
    if (page) {
        page->addKnob(mix);
    }
    _stageKnobs.mix = mix;
}

KnobPagePtr
StagedImageEffect::createControlsPage()
{
    return createKnob<KnobPage>(tr("Controls"));
}

void
StagedImageEffect::addStageKnobs(const KnobPagePtr& page)
{
    if (imagePipelineSpec().stages.has(Stage::MaskMix)) {
        addMaskMixKnobs(page);
    }
}

double
StagedImageEffect::getMixValue(double time,
                               ViewIdx view) const
{
    const KnobReader reader(time, view);

    return reader.valueOr(_stageKnobs.mix, 1.);
}

bool
StagedImageEffect::getMaskInvertValue(double time,
                                      ViewIdx view) const
{
    const KnobReader reader(time, view);

    return reader.valueOr(_stageKnobs.maskInvert, false);
}

bool
StagedImageEffect::isMaskApplied() const
{
    return isStageMaskApplied(*this);
}

bool
StagedImageEffect::isIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view,
                              double* inputTime,
                              ViewIdx* inputView,
                              int* inputNb)
{
    if (NativeEffectBase::isIdentity(time, scale, roi, view, inputTime, inputView, inputNb)) {
        return true;
    }

    *inputTime = time;
    *inputView = view;
    *inputNb = getSourceInput();

    const bool maskMix = imagePipelineSpec().stages.has(Stage::MaskMix);
    if (maskMix && (getMixValue(time, view) == 0.)) {
        return true;
    }
    if (isOpIdentity(time, scale, roi, view)) {
        return true;
    }

    // A mask that does not reach the window leaves the source unchanged there; an inverted
    // one applies the effect in full instead.
    if (maskMix && isMaskApplied() && !getMaskInvertValue(time, view) && stageMaskMissesWindow(*this, getMaskInput(), time, scale, roi, view)) {
        return true;
    }

    return false;
} // StagedImageEffect::isIdentity

StatusEnum
StagedImageEffect::render(const RenderActionArgs& args)
{
    const StagePlan plan = resolveStagePlan(*this, imagePipelineSpec().stages, _stageKnobs, getSourceInput(), args.time, args.view, args.processChannels);

    return renderOp(args, plan);
}

NATRON_NAMESPACE_EXIT
