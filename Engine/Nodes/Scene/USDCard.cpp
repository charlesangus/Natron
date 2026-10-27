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

#include "USDCard.h"

#include <string>

#include "Engine/AppInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Scene/PrimitiveMeshes.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_ENTER

USDCard::USDCard(NodePtr node)
    : ScenePrimitiveNode(node)
{
}

USDCard::~USDCard()
{
}

const NodeSpec*
USDCard::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

std::string
USDCard::getFallbackPrimName() const
{
    return "Card";
}

void
USDCard::initializeShapeKnobs(const KnobPagePtr& page)
{
    double aspect = 1.;
    Format format;
    getApp()->getProject()->getProjectDefaultFormat(&format);
    if (format.height() > 0) {
        aspect = format.width() * format.getPixelAspectRatio() / format.height();
    }
    _aspect = createSizeKnob(page, tr("Aspect"), kUSDCardParamAspect,
                             tr("The card's width over its height; it is one unit high. Defaults to the project format's "
                                "aspect, pixel aspect included."),
                             aspect);
    _rows = createSegmentsKnob(page, tr("Rows"), kUSDCardParamRows, tr("The number of faces from the bottom of the card to its top."),
                               8, PrimitiveMeshes::kCardMinSegments);
    _columns = createSegmentsKnob(page, tr("Columns"), kUSDCardParamColumns, tr("The number of faces from the left of the card to its right."),
                                  8, PrimitiveMeshes::kCardMinSegments);
}

bool
USDCard::isShapeAnimated() const
{
    return SceneNodeAuthoring::isAnimated(_aspect.lock());
}

bool
USDCard::buildMesh(double frame,
                   SceneMetadata::UpAxisEnum upAxis,
                   PrimitiveMesh* mesh) const
{
    KnobDoublePtr aspect = _aspect.lock();
    KnobIntPtr rows = _rows.lock();
    KnobIntPtr columns = _columns.lock();
    if (!aspect || !rows || !columns) {
        return false;
    }
    *mesh = PrimitiveMeshes::buildCard(aspect->getValueAtTime(frame), 1., rows->getValue(), columns->getValue(), upAxis);

    return true;
}

NATRON_NAMESPACE_EXIT
