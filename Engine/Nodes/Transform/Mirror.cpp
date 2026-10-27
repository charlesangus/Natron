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

#include "Mirror.h"

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

using TransformMath::Mat3;

void
MirrorOp::createKnobs(KnobHolder* holder,
                      const KnobPagePtr& page)
{
    KnobBoolPtr flipKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Flip"));
    flipKnob->setName(kMirrorParamFlip);
    flipKnob->setHintToolTip(EffectInstance::tr("Reverse the rows: the top of the image becomes the bottom."));
    flipKnob->setDefaultValue(false);
    page->addKnob(flipKnob);
    flip = flipKnob;

    KnobBoolPtr flopKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Flop"));
    flopKnob->setName(kMirrorParamFlop);
    flopKnob->setHintToolTip(EffectInstance::tr("Reverse the columns: the left of the image becomes the right."));
    flopKnob->setDefaultValue(false);
    page->addKnob(flopKnob);
    flop = flopKnob;
}

MirrorOp::Params
MirrorOp::bake(const KnobReader& reader) const
{
    Params params;

    params.flip = reader.value(flip);
    params.flop = reader.value(flop);
    params.time = reader.time();
    params.view = reader.view();

    return params;
}

bool
MirrorOp::isIdentity(const Params& params,
                     const PassContext& /*context*/) const
{
    return !params.flip && !params.flop;
}

ResampleSettings
MirrorOp::settings(const Params& params,
                   const PassContext& /*context*/) const
{
    ResampleSettings settings;

    settings.filter = Resampler::eFilterImpulse;
    settings.clamp = false;
    settings.blackOutside = true;
    settings.invert = false;
    settings.transformIsIdentity = !params.flip && !params.flop;

    return settings;
}

bool
MirrorOp::transform(const Params& params,
                    const PassContext& context,
                    double /*amount*/,
                    bool /*invert*/,
                    Mat3* matrix) const
{
    *matrix = Mat3::identity();
    if ((!params.flip && !params.flop) || !sourceRegion) {
        return true;
    }
    RectD rod;
    if (!sourceRegion(params.time, params.view, context.scale, &rod) || rod.isNull() || rod.isInfinite()) {
        return true;
    }

    // The reversal runs on the pixel grid at the render scale: pixel i goes to x1 + x2 - 1 - i,
    // so a pixel centre lands on a pixel centre whatever the size or origin of the region.
    const RectI pixels = rod.toPixelEnclosing(context.scale, context.pixelAspectRatio);
    const OfxPointD s = context.scale.toOfxPointD();
    double sumX = 0.;
    double sumY = 0.;
    double signX = 1.;
    double signY = 1.;

    if (params.flop) {
        sumX = (double)(pixels.x1 + pixels.x2) * context.pixelAspectRatio / s.x;
        signX = -1.;
    }
    if (params.flip) {
        sumY = (double)(pixels.y1 + pixels.y2) / s.y;
        signY = -1.;
    }
    *matrix = Mat3(signX, 0., sumX, 0., signY, sumY, 0., 0., 1.);

    return true;
}

Mirror::Mirror(NodePtr node)
    : ImageNode<MirrorOp>(node)
{
    op().sourceRegion = [this](double time, ViewIdx view, const RenderScale& scale, RectD* rod) {
        EffectInstancePtr input = getInput(getSourceInput());

        if (!input) {
            return false;
        }
        const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
        bool isProjectFormat = false;

        return input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, rod, &isProjectFormat) != eStatusFailed;
    };
}

Mirror::~Mirror()
{
}

void
Mirror::addAcceptedComponents(int inputNb,
                              std::list<ImageLayerDesc>* comps)
{
    ImageNode<MirrorOp>::addAcceptedComponents(inputNb, comps);
    comps->push_back(ImageLayerDesc::getXYComponents());
}

StatusEnum
Mirror::getRegionOfDefinition(U64 hash,
                              double time,
                              const RenderScale& scale,
                              ViewIdx view,
                              RectD* rod)
{
    // The reflection maps the source's pixels onto themselves, pixel centre onto pixel centre, so
    // the region is the source's. The generic resample region would grow it by the one-pixel
    // black edge a filtered warp needs.
    RectD srcRoD;
    if (op().sourceRegion && op().sourceRegion(time, view, scale, &srcRoD)) {
        *rod = srcRoD;

        return eStatusOK;
    }

    return ImageNode<MirrorOp>::getRegionOfDefinition(hash, time, scale, view, rod);
}

NATRON_NAMESPACE_EXIT
