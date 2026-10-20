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

#include "Position.h"

#include <cfloat>
#include <cmath>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

void
PositionOp::createKnobs(KnobHolder* holder,
                        const KnobPagePtr& page)
{
    KnobDoublePtr translateKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Translate"), 2);
    translateKnob->setName(kPositionParamTranslate);
    translateKnob->setHintToolTip(EffectInstance::tr("New position of the bottom-left pixel. Rounded to the closest pixel."));
    translateKnob->setSpatial(true);
    translateKnob->disableSlider();
    translateKnob->setDefaultValuesAreNormalized(true);
    for (int d = 0; d < 2; ++d) {
        translateKnob->setMinimum(-DBL_MAX, d);
        translateKnob->setMaximum(DBL_MAX, d);
        translateKnob->setDisplayMinimum(-10000., d);
        translateKnob->setDisplayMaximum(10000., d);
        translateKnob->setDefaultValue(0., d);
    }
    page->addKnob(translateKnob);
    translateKnob->setHasHostOverlayHandle(true);
    translate = translateKnob;

    KnobBoolPtr interactive = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Interactive"));
    interactive->setName(kPositionParamInteractive);
    interactive->setHintToolTip(EffectInstance::tr("When checked the image will be rendered whenever moving the overlay interact instead of when releasing the mouse button."));
    interactive->setDefaultValue(false);
    interactive->setAnimationEnabled(false);
    page->addKnob(interactive);
}

PositionOp::Params
PositionOp::bake(const KnobReader& reader) const
{
    Params params;

    params.translateX = reader.value(translate, 0);
    params.translateY = reader.value(translate, 1);

    return params;
}

PixelOffset
PositionOp::offset(const Params& params,
                   const PassContext& context) const
{
    const double s = context.scale.toOfxPointD().x;
    PixelOffset shift;

    shift.x = (int)std::floor(params.translateX * s / context.pixelAspectRatio + 0.5);
    shift.y = (int)std::floor(params.translateY * s + 0.5);

    return shift;
}

bool
PositionOp::isIdentity(const Params& params,
                       const PassContext& context) const
{
    const PixelOffset shift = offset(params, context);

    return (shift.x == 0) && (shift.y == 0);
}

Position::Position(NodePtr node)
    : ImageNode<PositionOp>(node)
{
}

Position::~Position()
{
}

void
Position::addAcceptedComponents(int inputNb,
                                std::list<ImageLayerDesc>* comps)
{
    ImageNode<PositionOp>::addAcceptedComponents(inputNb, comps);
    comps->push_back(ImageLayerDesc::getXYComponents());
}

NATRON_NAMESPACE_EXIT
