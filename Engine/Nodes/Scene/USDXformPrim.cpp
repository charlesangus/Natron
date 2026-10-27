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

#include "USDXformPrim.h"

#include <string>

#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

USDXformPrim::USDXformPrim(NodePtr node)
    : SceneReparentNode(node)
{
}

USDXformPrim::~USDXformPrim()
{
}

const NodeSpec*
USDXformPrim::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

std::string
USDXformPrim::getPrimTypeName() const
{
    return "Xform";
}

KnobDoublePtr
USDXformPrim::getPivotKnob() const
{
    return _pivot.lock();
}

void
USDXformPrim::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    createReparentKnobs(page);
    _transform.create(this, page);

    KnobDoublePtr pivot = createKnob<KnobDouble>(tr("Pivot"), 3);
    pivot->setName(kUSDXformPrimParamPivot);
    pivot->setHintToolTip(tr("The point the rotation and scale are about, in the prim's local space."));
    for (int d = 0; d < 3; ++d) {
        pivot->setDefaultValue(0., d);
    }
    page->addKnob(pivot);
    _pivot = pivot;
}

bool
USDXformPrim::overridesReadFrameRange() const
{
    return SceneNodeAuthoring::isTransformAnimated(_transform, _pivot.lock());
}

bool
USDXformPrim::authorPrimOverrides(const SceneLayerEdit& edit,
                                  const std::string& primPath,
                                  std::string* errorMessage) const
{
    KnobDoublePtr pivot = _pivot.lock();
    if (!pivot) {
        *errorMessage = tr("The node is being destroyed").toStdString();

        return false;
    }
    PXR_NS::UsdStageRefPtr stage = SceneNodeAuthoring::openLayerStage(edit, errorMessage);
    if (!stage) {
        return false;
    }
    const PXR_NS::SdfPath path(primPath);
    PXR_NS::UsdPrim prim = stage->OverridePrim(path);
    if (!prim) {
        *errorMessage = tr("USD could not author over %1.").arg(QString::fromStdString(primPath)).toStdString();

        return false;
    }
    const PXR_NS::TfToken opName = PXR_NS::UsdGeomXformOp::GetOpName(PXR_NS::UsdGeomXformOp::TypeTransform);
    const PXR_NS::UsdAttribute op = prim.CreateAttribute(opName, PXR_NS::SdfValueTypeNames->Matrix4d, false);
    const bool animated = SceneNodeAuthoring::isTransformAnimated(_transform, pivot);
    const bool authored = SceneNodeAuthoring::authorValue(op, edit, animated, [this, &pivot](double frame) {
        return SceneNodeAuthoring::getTransformMatrixAt(_transform, pivot, frame);
    });
    if (!authored) {
        *errorMessage = tr("USD could not author the transform of %1.").arg(QString::fromStdString(primPath)).toStdString();

        return false;
    }
    PXR_NS::VtTokenArray opOrder;
    opOrder.push_back(opName);
    const PXR_NS::UsdAttribute orderAttr = prim.CreateAttribute(PXR_NS::UsdGeomTokens->xformOpOrder, PXR_NS::SdfValueTypeNames->TokenArray, false, PXR_NS::SdfVariabilityUniform);
    if (!orderAttr || !orderAttr.Set(opOrder)) {
        *errorMessage = tr("USD could not author the op order of %1.").arg(QString::fromStdString(primPath)).toStdString();

        return false;
    }

    return true;
} // USDXformPrim::authorPrimOverrides

NATRON_NAMESPACE_EXIT
