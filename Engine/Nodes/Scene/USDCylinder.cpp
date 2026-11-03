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

#include "USDCylinder.h"

#include <string>

#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Scene/PrimitiveMeshes.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

namespace {
KnobBoolPtr
createCapKnob(USDCylinder* holder,
              const KnobPagePtr& page,
              const QString& label,
              const char* name,
              const QString& toolTip)
{
    KnobBoolPtr knob = AppManager::createKnob<KnobBool>(holder, label);

    knob->setName(name);
    knob->setHintToolTip(toolTip);
    knob->setDefaultValue(true);
    knob->setAnimationEnabled(false);
    page->addKnob(knob);

    return knob;
}
} // anonymous namespace

USDCylinder::USDCylinder(NodePtr node)
    : ScenePrimitiveNode(node)
{
}

USDCylinder::~USDCylinder()
{
}

const NodeSpec*
USDCylinder::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

std::string
USDCylinder::getFallbackPrimName() const
{
    return "Cylinder";
}

void
USDCylinder::initializeShapeKnobs(const KnobPagePtr& page)
{
    _radius = createSizeKnob(page, tr("Radius"), kUSDCylinderParamRadius, tr("The cylinder's radius, in scene units."), 1.);
    _height = createSizeKnob(page, tr("Height"), kUSDCylinderParamHeight, tr("The cylinder's length along the up axis, in scene units."), 2.);
    _rows = createSegmentsKnob(page, tr("Rows"), kUSDCylinderParamRows, tr("The number of bands from the bottom to the top."),
                               30, PrimitiveMeshes::kCylinderMinRows);
    _columns = createSegmentsKnob(page, tr("Columns"), kUSDCylinderParamColumns, tr("The number of faces around each band."),
                                  30, PrimitiveMeshes::kCylinderMinColumns);
    _topCap = createCapKnob(this, page, tr("Top Cap"), kUSDCylinderParamTopCap, tr("Closes the top end with a disc."));
    _bottomCap = createCapKnob(this, page, tr("Bottom Cap"), kUSDCylinderParamBottomCap, tr("Closes the bottom end with a disc."));
}

bool
USDCylinder::isShapeAnimated() const
{
    return SceneNodeAuthoring::isAnimated(_radius.lock()) || SceneNodeAuthoring::isAnimated(_height.lock());
}

bool
USDCylinder::buildMesh(double frame,
                       SceneMetadata::UpAxisEnum upAxis,
                       PrimitiveMesh* mesh) const
{
    KnobDoublePtr radius = _radius.lock();
    KnobDoublePtr height = _height.lock();
    KnobIntPtr rows = _rows.lock();
    KnobIntPtr columns = _columns.lock();
    KnobBoolPtr topCap = _topCap.lock();
    KnobBoolPtr bottomCap = _bottomCap.lock();
    if (!radius || !height || !rows || !columns || !topCap || !bottomCap) {
        return false;
    }
    *mesh = PrimitiveMeshes::buildCylinder(radius->getValueAtTime(frame), height->getValueAtTime(frame), rows->getValue(), columns->getValue(),
                                           topCap->getValue(), bottomCap->getValue(), upAxis);

    return true;
}

NATRON_NAMESPACE_EXIT
