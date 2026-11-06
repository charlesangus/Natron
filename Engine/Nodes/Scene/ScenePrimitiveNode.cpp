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

#include "ScenePrimitiveNode.h"

#include <algorithm>
#include <string>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Scene/PrimitiveMeshes.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

ScenePrimitiveNode::ScenePrimitiveNode(NodePtr node)
    : NativeEffectBase(node)
{
}

ScenePrimitiveNode::~ScenePrimitiveNode()
{
}

KnobDoublePtr
ScenePrimitiveNode::getPivotKnob() const
{
    return _pivot.lock();
}

KnobDoublePtr
ScenePrimitiveNode::createSizeKnob(const KnobPagePtr& page,
                                   const QString& label,
                                   const char* name,
                                   const QString& toolTip,
                                   double defaultValue,
                                   int dimension)
{
    KnobDoublePtr knob = createKnob<KnobDouble>(label, dimension);

    knob->setName(name);
    knob->setHintToolTip(toolTip);
    for (int d = 0; d < dimension; ++d) {
        knob->setDefaultValue(defaultValue, d);
        knob->setMinimum(0., d);
    }
    page->addKnob(knob);

    return knob;
}

KnobIntPtr
ScenePrimitiveNode::createSegmentsKnob(const KnobPagePtr& page,
                                       const QString& label,
                                       const char* name,
                                       const QString& toolTip,
                                       int defaultValue,
                                       int minimum,
                                       int dimension)
{
    KnobIntPtr knob = createKnob<KnobInt>(label, dimension);

    knob->setName(name);
    knob->setHintToolTip(toolTip);
    for (int d = 0; d < dimension; ++d) {
        knob->setDefaultValue(defaultValue, d);
        knob->setMinimum(minimum, d);
        knob->setDisplayMinimum(minimum, d);
        knob->setDisplayMaximum(std::max(minimum, 64), d);
    }
    knob->setAnimationEnabled(false);
    page->addKnob(knob);

    return knob;
}

void
ScenePrimitiveNode::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    _primPath = createScenePrimPathKnob(this, page, getSceneNodeDefaultPrimPath(this, getFallbackPrimName()), tr("The path of the mesh prim the node authors."));

    initializeShapeKnobs(page);

    KnobBoolPtr doubleSided = createKnob<KnobBool>(tr("Double Sided"));
    doubleSided->setName(kScenePrimitiveParamDoubleSided);
    doubleSided->setHintToolTip(tr("Whether the back of each face renders too (doubleSided). Off, renderers may cull faces "
                                   "seen from behind."));
    doubleSided->setDefaultValue(getDefaultDoubleSided());
    doubleSided->setAnimationEnabled(false);
    page->addKnob(doubleSided);
    _doubleSided = doubleSided;

    _transform.create(this, page);
    initializeTransformDefaults(_transform);

    KnobDoublePtr pivot = createKnob<KnobDouble>(tr("Pivot"), 3);
    pivot->setName(kScenePrimitiveParamPivot);
    pivot->setHintToolTip(tr("The point the rotation and scale are about, in the prim's local space."));
    for (int d = 0; d < 3; ++d) {
        pivot->setDefaultValue(0., d);
    }
    page->addKnob(pivot);
    _pivot = pivot;
} // ScenePrimitiveNode::initializeKnobs

void
ScenePrimitiveNode::onScriptNameChanged(const std::string& /*fullyQualifiedName*/)
{
    followSceneNodeRename(_primPath.lock(), getSceneNodeDefaultPrimPath(this, getFallbackPrimName()));
}

bool
ScenePrimitiveNode::authorLayer(const SceneLayerEdit& edit,
                                const SceneMaterialGraft& graft,
                                std::string* errorMessage) const
{
    PXR_NS::SdfPath path;

    if (!SceneNodeAuthoring::getPrimPath(_primPath.lock(), &path, errorMessage)) {
        return false;
    }
    KnobBoolPtr doubleSided = _doubleSided.lock();
    KnobDoublePtr pivot = _pivot.lock();
    if (!doubleSided || !pivot) {
        *errorMessage = tr("The node is being destroyed").toStdString();

        return false;
    }
    PXR_NS::UsdStageRefPtr stage = SceneNodeAuthoring::openLayerStage(edit, errorMessage);
    if (!stage) {
        return false;
    }
    PXR_NS::UsdGeomMesh mesh = PXR_NS::UsdGeomMesh::Define(stage, path);
    const QString pathString = QString::fromStdString(path.GetString());
    if (!mesh) {
        *errorMessage = tr("USD could not define a mesh at %1.").arg(pathString).toStdString();

        return false;
    }

    PrimitiveMesh shape;
    if (!buildMesh(edit.firstFrame, edit.upAxis, &shape)) {
        *errorMessage = tr("The node is being destroyed").toStdString();

        return false;
    }
    const PXR_NS::UsdGeomPrimvar st = PXR_NS::UsdGeomPrimvarsAPI(mesh).CreatePrimvar(PXR_NS::TfToken("st"), PXR_NS::SdfValueTypeNames->TexCoord2fArray, PXR_NS::UsdGeomTokens->faceVarying);
    bool authored = mesh.CreateFaceVertexCountsAttr().Set(shape.faceVertexCounts)
        && mesh.CreateFaceVertexIndicesAttr().Set(shape.faceVertexIndices)
        && mesh.CreateNormalsAttr().Set(shape.normals)
        && mesh.SetNormalsInterpolation(PXR_NS::UsdGeomTokens->faceVarying)
        && st && st.Set(shape.st) && st.SetIndices(shape.stIndices)
        && mesh.CreateSubdivisionSchemeAttr().Set(PXR_NS::UsdGeomTokens->none)
        && mesh.CreateDoubleSidedAttr().Set(doubleSided->getValue());

    const PXR_NS::UsdAttribute points = mesh.CreatePointsAttr();
    const PXR_NS::UsdAttribute extent = mesh.CreateExtentAttr();
    const auto authorPoints = [&points, &extent](const PrimitiveMesh& m, PXR_NS::UsdTimeCode time) {
        PXR_NS::VtVec3fArray bounds;

        return PXR_NS::UsdGeomPointBased::ComputeExtent(m.points, &bounds) && points.Set(m.points, time) && extent.Set(bounds, time);
    };
    if (authored && !isShapeAnimated()) {
        authored = authorPoints(shape, PXR_NS::UsdTimeCode::Default());
    } else if (authored) {
        for (double frame : SceneNodeAuthoring::getSampleFrames(edit)) {
            PrimitiveMesh atFrame;
            authored = buildMesh(frame, edit.upAxis, &atFrame) && authorPoints(atFrame, SceneNodeAuthoring::frameToTimeCode(edit, frame));
            if (!authored) {
                break;
            }
        }
    }
    if (!authored) {
        *errorMessage = tr("USD could not author the mesh at %1.").arg(pathString).toStdString();

        return false;
    }

    const PXR_NS::UsdGeomXformOp op = mesh.AddTransformOp();
    const bool animated = SceneNodeAuthoring::isTransformAnimated(_transform, pivot);
    const auto transformAt = [this, &pivot](double frame) {
        return SceneNodeAuthoring::getTransformMatrixAt(_transform, pivot, frame);
    };
    if (!op || !SceneNodeAuthoring::authorValue(op.GetAttr(), edit, animated, transformAt)) {
        *errorMessage = tr("USD could not author the transform of %1.").arg(pathString).toStdString();

        return false;
    }
    // The mesh's ancestors are typeless, so its transform op is its local to world transform.
    if (!SceneNodeAuthoring::authorProjectionGeometryFrame(stage, edit, graft, animated, transformAt)) {
        *errorMessage = tr("USD could not author the projection's geometry frame for %1.").arg(pathString).toStdString();

        return false;
    }

    return SceneNodeAuthoring::authorMaterialBinding(mesh.GetPrim(), graft.materialPath, errorMessage);
} // ScenePrimitiveNode::authorLayer

StatusEnum
ScenePrimitiveNode::renderScene(const SceneRenderActionArgs& args,
                                ScenePayload* outputScene,
                                std::string* errorMessage)
{
    KnobStringPtr primPath = _primPath.lock();
    if (!primPath) {
        *errorMessage = tr("The node is being destroyed").toStdString();

        return eStatusFailed;
    }

    SceneMaterialGraft graft;
    if (!SceneNodeAuthoring::graftMaterialInput(args.getInputScene(0), primPath->getValue(), getScriptName_mt_safe(), &_graftMemo, &graft, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }

    SceneLayerReads reads;
    reads.upstream = SceneLayerMemo::Key::eUpstreamReadFacts;
    reads.factsDigest = graft.factsDigest;
    reads.frameRange = isShapeAnimated() || SceneNodeAuthoring::isTransformAnimated(_transform, _pivot.lock());
    // What the layer reads of the graft, without the payload it is stacked on.
    SceneMaterialGraft binding;
    binding.materialPath = graft.materialPath;
    binding.geometryProjectionShader = graft.geometryProjectionShader;
    binding.projectionHeld = graft.projectionHeld;
    binding.projectionHoldFrame = graft.projectionHoldFrame;
    if (!emitSceneLayer(graft.payload, reads, [this, binding](const SceneLayerEdit& edit, std::string* error) { return authorLayer(edit, binding, error); }, outputScene, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }

    if (graft.unmappedLinks.empty()) {
        clearPersistentMessage(false);

        return eStatusOK;
    }
    QString warning = tr("These relationship targets and connections of the material lie under no root prim of the "
                         "Material input, so the grafted properties are authored without them:");
    for (const SceneUnmappedLink& link : graft.unmappedLinks) {
        warning += QLatin1Char('\n') + tr("%1 -> %2").arg(QString::fromStdString(link.propertyPath)).arg(QString::fromStdString(link.targetPath));
    }
    setPersistentMessage(eMessageTypeWarning, warning.toStdString());

    return eStatusOK;
} // ScenePrimitiveNode::renderScene

NATRON_NAMESPACE_EXIT
