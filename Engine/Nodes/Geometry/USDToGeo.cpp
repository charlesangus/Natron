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

#include "USDToGeo.h"

#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <QString>

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include <pxr/usd/usdGeom/basisCurves.h>
#include <pxr/usd/usdGeom/points.h>

#include "Engine/AppInstance.h"
#include "Engine/Geo/GeoPacked.h"
#include "Engine/KnobTypes.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Geometry/GeoUsdConvert.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

namespace {

bool
boolAt(const KnobBoolWPtr& knob,
       double time,
       ViewSpec view)
{
    const KnobBoolPtr locked = knob.lock();

    return locked && locked->getValueAtTime(time, 0, view);
}

GeoBounds
geoBoundsFromRange(const PXR_NS::GfRange3d& range)
{
    GeoBounds bounds;
    if (range.IsEmpty()) {
        return bounds;
    }
    for (int i = 0; i < 3; ++i) {
        bounds.min[i] = range.GetMin()[i];
        bounds.max[i] = range.GetMax()[i];
    }

    return bounds;
}

bool
isReadableUnpacked(const PXR_NS::UsdPrim& prim)
{
    return prim.IsA<PXR_NS::UsdGeomMesh>() || prim.IsA<PXR_NS::UsdGeomPoints>() || prim.IsA<PXR_NS::UsdGeomBasisCurves>();
}

} // anonymous namespace

USDToGeo::USDToGeo(NodePtr node)
    : NativeEffectBase(node)
{
}

USDToGeo::~USDToGeo()
{
}

const NodeSpec*
USDToGeo::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
USDToGeo::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    _primPattern.create(this, page, kUSDToGeoParamPrimPattern, tr("Prim Pattern"), tr("The prims to turn into geometry."));

    KnobBoolPtr unpack = createKnob<KnobBool>(tr("Unpack").toStdString());
    unpack->setName(kUSDToGeoParamUnpack);
    unpack->setHintToolTip(tr("Off, each prim becomes one packed primitive holding its path, transforms and bounds, "
                              "and no point is read. On, meshes, points and curves are read as polygons, points and "
                              "curves sharing one point pool; other prims are skipped.")
                               .toStdString());
    unpack->setDefaultValue(false);
    unpack->setAnimationEnabled(false);
    page->addKnob(unpack);
    _unpack = unpack;

    KnobBoolPtr flatten = createKnob<KnobBool>(tr("Flatten Transforms").toStdString());
    flatten->setName(kUSDToGeoParamFlattenTransforms);
    flatten->setHintToolTip(tr("When unpacked, moves the points into world space by each prim's world transform. "
                               "Off, they stay in the prim's own space.")
                                .toStdString());
    flatten->setDefaultValue(false);
    flatten->setAnimationEnabled(false);
    page->addKnob(flatten);
    _flatten = flatten;

    updateFlattenEnabled();
}

void
USDToGeo::updateFlattenEnabled()
{
    const KnobBoolPtr unpack = _unpack.lock();
    const KnobBoolPtr flatten = _flatten.lock();
    if (unpack && flatten) {
        flatten->setAllDimensionsEnabled(unpack->getValue());
    }
}

void
USDToGeo::onKnobsLoaded()
{
    updateFlattenEnabled();
}

bool
USDToGeo::knobChanged(KnobI* k,
                      ValueChangedReasonEnum /*reason*/,
                      ViewSpec /*view*/,
                      double /*time*/,
                      bool /*originatedFromMainThread*/)
{
    const KnobBoolPtr unpack = _unpack.lock();
    if (unpack && (k == unpack.get())) {
        updateFlattenEnabled();

        return true;
    }

    return false;
}

StatusEnum
USDToGeo::getPreferredMetadataAt(double /*time*/,
                                 ViewIdx /*view*/,
                                 NodeMetadata& metadata)
{
    // The input scene's payload is the same at every frame while its prims may be animated, so
    // the geometry cache must key on the time.
    metadata.setIsFrameVarying(true);

    return eStatusOK;
}

StatusEnum
USDToGeo::renderGeometry(const GeoRenderActionArgs& args,
                         GeoDetailConstPtr* outputDetail,
                         std::string* errorMessage)
{
    const std::string pattern = _primPattern.getExpression();
    const bool unpack = boolAt(_unpack, args.time, args.view);
    const bool flatten = unpack && boolAt(_flatten, args.time, args.view);

    if (!PrimSelector::validate(pattern, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }

    ScenePayload scene;
    const EffectInstancePtr input = getInput(0);
    if (input && !PrimSelector::isBlank(pattern)) {
        if (input->evaluateScene(EvaluateSceneArgs(args.time, args.view), &scene, errorMessage) != eRenderRoIRetCodeOk) {
            if (errorMessage->empty()) {
                *errorMessage = tr("The input scene could not be evaluated.").toStdString();
            }
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
    }

    std::vector<std::string> skipped;
    std::vector<std::string> warnings;
    const StatusEnum status = renderGeometryGenerator([&](GeoDetailBuilder& builder, std::string* error) {
        if (scene.isEmpty()) {
            return eStatusOK;
        }
        SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(scene, error);
        const PXR_NS::UsdStageRefPtr stage = lease.getStage();
        if (!stage) {
            if (error->empty()) {
                *error = tr("USD could not compose the input scene.").toStdString();
            }

            return eStatusFailed;
        }
        std::vector<PXR_NS::SdfPath> matches;
        if (!SceneOps::selectPrims(stage, pattern, &matches, error)) {
            return eStatusFailed;
        }

        const AppInstancePtr app = getApp();
        const double fps = (app && app->getProject()) ? app->getProject()->getProjectFrameRate() : 24.;
        const PXR_NS::UsdTimeCode time(SceneOps::frameToTimeCode(scene, args.time, fps));
        PXR_NS::UsdGeomXformCache xforms(time);

        std::vector<std::pair<GeoOffset, std::string>> packedPaths;
        if (!unpack) {
            PXR_NS::UsdGeomBBoxCache bboxes(time, { PXR_NS::UsdGeomTokens->default_, PXR_NS::UsdGeomTokens->render, PXR_NS::UsdGeomTokens->proxy }, true);
            for (const PXR_NS::SdfPath& path : matches) {
                const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(path);
                if (!prim || prim.IsInstanceProxy() || !prim.IsA<PXR_NS::UsdGeomImageable>()) {
                    skipped.push_back(path.GetString());
                    continue;
                }
                bool resetsXformStack = false;
                const PXR_NS::GfMatrix4d local = xforms.GetLocalTransformation(prim, &resetsXformStack);
                const PXR_NS::GfMatrix4d parentWorld = resetsXformStack ? PXR_NS::GfMatrix4d(1.) : xforms.GetParentToWorldTransform(prim);
                const GeoMatrix4 world = geoMatrixFromUsd(local * parentWorld);
                const std::array<double, 3> pivot = geoTransformPoint(world, { 0., 0., 0. });
                const GeoOffset point = builder.appendPoints(1);
                builder.setPointPosition(point, pivot[0], pivot[1], pivot[2]);
                const GeoBounds bounds = geoBoundsFromRange(bboxes.ComputeUntransformedBound(prim).ComputeAlignedRange());
                const GeoOffset primitive = geoAppendPackedUSD(builder, point, path.GetString(), geoMatrixFromUsd(parentWorld), geoMatrixFromUsd(local), bounds);
                packedPaths.emplace_back(primitive, path.GetString());
            }
            if (!packedPaths.empty()) {
                GeoAttributeDesc pathDesc;
                pathDesc.name = "path";
                pathDesc.storage = eGeoStorageString;
                GeoAttribute& pathAttribute = builder.addAttribute(eGeoAttribClassPrimitive, pathDesc);
                for (const std::pair<GeoOffset, std::string>& entry : packedPaths) {
                    pathAttribute.setString(std::size_t(entry.first), entry.second);
                }
            }

            return eStatusOK;
        }

        for (const PXR_NS::SdfPath& path : matches) {
            const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(path);
            if (!prim || prim.IsInstanceProxy() || !isReadableUnpacked(prim)) {
                skipped.push_back(path.GetString());
                continue;
            }
            const PXR_NS::GfMatrix4d world = xforms.GetLocalToWorldTransform(prim);
            std::string readError;
            if (!geoUsdReadPrim(prim, time, flatten ? &world : nullptr, builder, &warnings, &readError)) {
                skipped.push_back(path.GetString() + ": " + readError);
            }
        }

        return eStatusOK;
    },
                                                      outputDetail, errorMessage);
    if (status != eStatusOK) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return status;
    }

    if (skipped.empty() && warnings.empty()) {
        clearPersistentMessage(false);

        return eStatusOK;
    }
    QString message;
    if (!skipped.empty()) {
        message = unpack ? tr("Skipped, not a mesh, points or curves prim (or an instance proxy):")
                         : tr("Skipped, not an imageable prim (or an instance proxy):");
        for (const std::string& entry : skipped) {
            message += QString::fromUtf8("\n  ") + QString::fromStdString(entry);
        }
    }
    for (const std::string& warning : warnings) {
        if (!message.isEmpty()) {
            message += QString::fromUtf8("\n");
        }
        message += QString::fromStdString(warning);
    }
    setPersistentMessage(eMessageTypeInfo, message.toStdString());

    return eStatusOK;
} // USDToGeo::renderGeometry

NATRON_NAMESPACE_EXIT
