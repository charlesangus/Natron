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

#include "USDCube.h"

#include <string>

#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Scene/PrimitiveMeshes.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

USDCube::USDCube(NodePtr node)
    : ScenePrimitiveNode(node)
{
}

USDCube::~USDCube()
{
}

const NodeSpec*
USDCube::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

std::string
USDCube::getFallbackPrimName() const
{
    return "Cube";
}

void
USDCube::initializeShapeKnobs(const KnobPagePtr& page)
{
    _size = createSizeKnob(page, tr("Size"), kUSDCubeParamSize, tr("The cube's size along the X, Y and Z axes, in scene units."), 1., 3);
    _subdivisions = createSegmentsKnob(page, tr("Subdivisions"), kUSDCubeParamSubdivisions,
                                       tr("The number of faces along the X, Y and Z axes, on every side the axis crosses."),
                                       1, PrimitiveMeshes::kCubeMinSegments, 3);
}

bool
USDCube::isShapeAnimated() const
{
    return SceneNodeAuthoring::isAnimated(_size.lock());
}

bool
USDCube::buildMesh(double frame,
                   SceneMetadata::UpAxisEnum upAxis,
                   PrimitiveMesh* mesh) const
{
    KnobDoublePtr size = _size.lock();
    KnobIntPtr subdivisions = _subdivisions.lock();
    if (!size || !subdivisions) {
        return false;
    }
    const int segments[3] = { subdivisions->getValue(0), subdivisions->getValue(1), subdivisions->getValue(2) };
    *mesh = PrimitiveMeshes::buildCube(PXR_NS::GfVec3d(size->getValueAtTime(frame, 0), size->getValueAtTime(frame, 1), size->getValueAtTime(frame, 2)),
                                       segments, upAxis);

    return true;
}

NATRON_NAMESPACE_EXIT
