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

#include "OCIOFileTransform.h"

#include <string>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

void
OCIOFileTransformOp::createKnobs(KnobHolder* holder,
                                 const KnobPagePtr& page)
{
    ocio.attach(holder);
    files.create(holder, page, ocio, EffectInstance::tr("The file holding the transform, in any format OpenColorIO reads."));

    KnobChoicePtr directionKnob = OcioNodeKnobs::createDirectionKnob(holder);
    directionKnob->setName(kOCIOFileTransformParamDirection);
    page->addKnob(directionKnob);
    direction = directionKnob;

    KnobChoicePtr interpolationKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("Interpolation").toStdString());
    interpolationKnob->setName(kOCIOFileTransformParamInterpolation);
    interpolationKnob->setHintToolTip(EffectInstance::tr("How a LUT is interpolated between its entries. Ignored for files that "
                                                         "are not LUTs.")
                                          .toStdString());
    std::vector<ChoiceOption> options;
    options.push_back(ChoiceOption("nearest", EffectInstance::tr("Nearest").toStdString(), std::string()));
    options.push_back(ChoiceOption("linear", EffectInstance::tr("Linear").toStdString(), std::string()));
    options.push_back(ChoiceOption("tetrahedral", EffectInstance::tr("Tetrahedral").toStdString(), std::string()));
    options.push_back(ChoiceOption("best", EffectInstance::tr("Best").toStdString(), std::string()));
    interpolationKnob->populateChoices(options);
    interpolationKnob->setDefaultValue(1);
    page->addKnob(interpolationKnob);
    interpolation = interpolationKnob;
}

OCIO_NAMESPACE::ConstTransformRcPtr
OCIOFileTransformOp::transform(const KnobReader& reader) const
{
    const std::string src = files.path();

    if (src.empty()) {
        return OCIO_NAMESPACE::ConstTransformRcPtr();
    }
    static const OCIO_NAMESPACE::Interpolation kInterpolations[4] = {
        OCIO_NAMESPACE::INTERP_NEAREST, OCIO_NAMESPACE::INTERP_LINEAR, OCIO_NAMESPACE::INTERP_TETRAHEDRAL, OCIO_NAMESPACE::INTERP_BEST
    };
    int interpolationIndex = reader.value(interpolation);
    if ((interpolationIndex < 0) || (interpolationIndex > 3)) {
        interpolationIndex = 1;
    }
    OCIO_NAMESPACE::FileTransformRcPtr transform = OCIO_NAMESPACE::FileTransform::Create();
    transform->setSrc(src.c_str());
    transform->setCCCId(files.cccid().c_str());
    transform->setInterpolation(kInterpolations[interpolationIndex]);
    transform->setDirection(OcioNodeKnobs::direction(reader.value(direction)));

    return transform;
}

OCIOFileTransform::OCIOFileTransform(NodePtr node)
    : OcioNode<OCIOFileTransformOp>(node)
{
}

OCIOFileTransform::~OCIOFileTransform()
{
}

void
OCIOFileTransform::onKnobsLoaded()
{
    op().files.refreshCCCIdMenu();
    OcioNode<OCIOFileTransformOp>::onKnobsLoaded();
}

bool
OCIOFileTransform::knobChanged(KnobI* k,
                               ValueChangedReasonEnum reason,
                               ViewSpec view,
                               double time,
                               bool originatedFromMainThread)
{
    const bool handled = op().files.knobChanged(k, reason, op().ocio);

    return OcioNode<OCIOFileTransformOp>::knobChanged(k, reason, view, time, originatedFromMainThread) || handled;
}

NATRON_NAMESPACE_EXIT
