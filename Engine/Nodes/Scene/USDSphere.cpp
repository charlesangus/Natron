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

#include "USDSphere.h"

#include <string>

#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Scene/PrimitiveMeshes.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

USDSphere::USDSphere(NodePtr node)
    : ScenePrimitiveNode(node)
{
}

USDSphere::~USDSphere()
{
}

const NodeSpec*
USDSphere::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

std::string
USDSphere::getFallbackPrimName() const
{
    return "Sphere";
}

void
USDSphere::initializeShapeKnobs(const KnobPagePtr& page)
{
    _radius = createSizeKnob(page, tr("Radius"), kUSDSphereParamRadius, tr("The sphere's radius, in scene units."), 1.);
    _rows = createSegmentsKnob(page, tr("Rows"), kUSDSphereParamRows, tr("The number of bands from pole to pole."),
                               30, PrimitiveMeshes::kSphereMinRows);
    _columns = createSegmentsKnob(page, tr("Columns"), kUSDSphereParamColumns, tr("The number of faces around each band."),
                                  30, PrimitiveMeshes::kSphereMinColumns);
}

bool
USDSphere::isShapeAnimated() const
{
    return SceneNodeAuthoring::isAnimated(_radius.lock());
}

bool
USDSphere::buildMesh(double frame,
                     SceneMetadata::UpAxisEnum upAxis,
                     PrimitiveMesh* mesh) const
{
    KnobDoublePtr radius = _radius.lock();
    KnobIntPtr rows = _rows.lock();
    KnobIntPtr columns = _columns.lock();
    if (!radius || !rows || !columns) {
        return false;
    }
    *mesh = PrimitiveMeshes::buildSphere(radius->getValueAtTime(frame), rows->getValue(), columns->getValue(), upAxis);

    return true;
}

NATRON_NAMESPACE_EXIT
