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

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <QString>

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include <pxr/usd/sdf/listOp.h>
#include <pxr/usd/usdGeom/basisCurves.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdGeom/points.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>

#include "Engine/AppInstance.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/Geo/GeoPacked.h"
#include "Engine/KnobTypes.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Geometry/GeoUsdConvert.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneProjectionCameras.h"
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

std::string
stringOf(const KnobStringWPtr& knob)
{
    const KnobStringPtr locked = knob.lock();

    return locked ? locked->getValue() : std::string();
}

int
choiceOf(const KnobChoiceWPtr& knob)
{
    const KnobChoicePtr locked = knob.lock();

    return locked ? locked->getValue() : 0;
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

bool
attributesMightVary(const PXR_NS::UsdPrim& prim,
                    bool withTransform)
{
    for (const PXR_NS::UsdAttribute& attribute : prim.GetAuthoredAttributes()) {
        if (!withTransform && PXR_NS::UsdGeomXformOp::IsXformOp(attribute)) {
            continue;
        }
        if (attribute.ValueMightBeTimeVarying()) {
            return true;
        }
    }

    return false;
}

// Whether a packed prim's transform or bounds may change over time.
bool
packedMightVary(const PXR_NS::UsdPrim& prim)
{
    if (SceneProjectionCameras::transformMightVary(prim)) {
        return true;
    }
    for (const PXR_NS::UsdPrim& descendant : PXR_NS::UsdPrimRange(prim, PXR_NS::UsdTraverseInstanceProxies())) {
        if (attributesMightVary(descendant, true)) {
            return true;
        }
    }

    return false;
}

// Whether an instancer read one packed primitive per instance may change over time: each
// instance takes its bounds from its prototype, which may lie anywhere on the stage.
bool
pointInstancesMightVary(const PXR_NS::UsdGeomPointInstancer& instancer)
{
    const PXR_NS::UsdPrim prim = instancer.GetPrim();
    if (packedMightVary(prim)) {
        return true;
    }
    PXR_NS::SdfPathVector prototypes;
    instancer.GetPrototypesRel().GetForwardedTargets(&prototypes);
    for (const PXR_NS::SdfPath& path : prototypes) {
        if (path.HasPrefix(prim.GetPath())) {
            continue;
        }
        const PXR_NS::UsdPrim prototype = prim.GetStage()->GetPrimAtPath(path);
        if (prototype && packedMightVary(prototype)) {
            return true;
        }
    }

    return false;
}

// Whether what geoUsdReadPrim() reads from the prim may change over time: its attributes and its
// GeomSubsets', its world transform when that is baked in or recorded, and the primvars it
// inherits when those are read.
bool
unpackedMightVary(const PXR_NS::UsdPrim& prim,
                  bool withTransform,
                  bool inheritedPrimvars)
{
    if (attributesMightVary(prim, false) || (withTransform && SceneProjectionCameras::transformMightVary(prim))) {
        return true;
    }
    for (const PXR_NS::UsdPrim& child : prim.GetChildren()) {
        if (child.IsA<PXR_NS::UsdGeomSubset>() && attributesMightVary(child, true)) {
            return true;
        }
    }
    if (inheritedPrimvars) {
        for (PXR_NS::UsdPrim ancestor = prim.GetParent(); ancestor && !ancestor.IsPseudoRoot(); ancestor = ancestor.GetParent()) {
            for (const PXR_NS::UsdGeomPrimvar& primvar : PXR_NS::UsdGeomPrimvarsAPI(ancestor).GetPrimvarsWithValues()) {
                if (primvar.ValueMightBeTimeVarying()) {
                    return true;
                }
            }
        }
    }

    return false;
}

// Parses the purpose knob: purpose names separated by spaces or commas.
bool
parsePurposes(const std::string& text,
              PXR_NS::TfTokenVector* purposes,
              std::string* errorMessage)
{
    purposes->clear();
    std::string word;
    auto flush = [&]() {
        if (word.empty()) {
            return true;
        }
        PXR_NS::TfToken token;
        if (word == "default") {
            token = PXR_NS::UsdGeomTokens->default_;
        } else if (word == "render") {
            token = PXR_NS::UsdGeomTokens->render;
        } else if (word == "proxy") {
            token = PXR_NS::UsdGeomTokens->proxy;
        } else if (word == "guide") {
            token = PXR_NS::UsdGeomTokens->guide;
        } else {
            *errorMessage = QObject::tr("Purpose: \"%1\" is not a purpose; use default, render, proxy or guide.").arg(QString::fromStdString(word)).toStdString();

            return false;
        }
        if (std::find(purposes->begin(), purposes->end(), token) == purposes->end()) {
            purposes->push_back(token);
        }
        word.clear();

        return true;
    };
    for (char c : text) {
        if (c == ' ' || c == ',' || c == '\t') {
            if (!flush()) {
                return false;
            }
        } else {
            word.push_back(c);
        }
    }

    if (!flush()) {
        return false;
    }
    if (purposes->empty()) {
        *errorMessage = QObject::tr("Purpose: no purpose is listed; list at least one of default, render, proxy or guide.").toStdString();

        return false;
    }

    return true;
}

bool
hasKind(const PXR_NS::UsdPrim& prim,
        const PXR_NS::TfToken& baseKind)
{
    PXR_NS::TfToken kind;

    return PXR_NS::UsdModelAPI(prim).GetKind(&kind) && !kind.IsEmpty() && PXR_NS::KindRegistry::IsA(kind, baseKind);
}

// UsdPrimIsLoaded is false on every inactive prim, so it cannot be part of the predicate that
// reads them; the read stage loads every payload, so dropping it loses nothing.
PXR_NS::Usd_PrimFlagsPredicate
readPredicate(bool includeInactive)
{
    return includeInactive ? PXR_NS::Usd_PrimFlagsPredicate(PXR_NS::UsdPrimIsDefined && !PXR_NS::UsdPrimIsAbstract)
                           : PXR_NS::Usd_PrimFlagsPredicate(PXR_NS::UsdPrimDefaultPredicate);
}

struct TraversalWalk {
    USDToGeoTraversalEnum traversal = eUSDToGeoTraversalNone;
    PXR_NS::TfTokenVector purposes;
    bool includeInactive = false;
    std::vector<PXR_NS::UsdPrim>* out = nullptr;
    std::unordered_set<PXR_NS::SdfPath, PXR_NS::SdfPath::Hash> seen;

    // A prim that is not imageable has no purpose of its own and hands its parent's down.
    static PXR_NS::UsdGeomImageable::PurposeInfo purposeInfoOf(const PXR_NS::UsdPrim& prim,
                                                               const PXR_NS::UsdGeomImageable::PurposeInfo& parent)
    {
        const PXR_NS::UsdGeomImageable imageable(prim);

        return imageable ? imageable.ComputePurposeInfo(parent) : parent;
    }

    static PXR_NS::UsdGeomImageable::PurposeInfo inheritedPurposeInfo(const PXR_NS::UsdPrim& prim)
    {
        for (PXR_NS::UsdPrim ancestor = prim.GetParent(); ancestor && !ancestor.IsPseudoRoot(); ancestor = ancestor.GetParent()) {
            if (const PXR_NS::UsdGeomImageable imageable = PXR_NS::UsdGeomImageable(ancestor)) {
                return imageable.ComputePurposeInfo();
            }
        }

        return PXR_NS::UsdGeomImageable::PurposeInfo();
    }

    bool allowed(const PXR_NS::TfToken& purpose) const
    {
        return std::find(purposes.begin(), purposes.end(), purpose) != purposes.end();
    }

    bool isTarget(const PXR_NS::UsdPrim& prim) const
    {
        switch (traversal) {
        case eUSDToGeoTraversalNone:
            return true;
        case eUSDToGeoTraversalComponents:
            return hasKind(prim, PXR_NS::KindTokens->component);
        case eUSDToGeoTraversalGprims:
            // An instancer is the drawn leaf of its hierarchy: its prototypes are only drawn
            // through it.
            return prim.IsA<PXR_NS::UsdGeomGprim>() || prim.IsA<PXR_NS::UsdGeomPointInstancer>();
        case eUSDToGeoTraversalGroups:
            return hasKind(prim, PXR_NS::KindTokens->group);
        }

        return true;
    }

    // Only the outermost target below a matched prim is taken, so a prim is never read twice
    // through its ancestor.
    void visit(const PXR_NS::UsdPrim& prim)
    {
        visit(prim, inheritedPurposeInfo(prim));
    }

    void visit(const PXR_NS::UsdPrim& prim,
               const PXR_NS::UsdGeomImageable::PurposeInfo& parentPurpose)
    {
        if (!includeInactive && !prim.IsActive()) {
            return;
        }
        // A purpose other than default is inherited by the whole subtree, so a prim with one
        // that is not read hides everything below it; a default-purpose group may still hold
        // render or proxy prims, and is walked into whatever is read.
        const PXR_NS::UsdGeomImageable::PurposeInfo purposeInfo = purposeInfoOf(prim, parentPurpose);
        const PXR_NS::TfToken purpose = PXR_NS::UsdGeomImageable(prim) && purposeInfo ? purposeInfo.purpose : PXR_NS::UsdGeomTokens->default_;
        const bool purposeRead = allowed(purpose);
        if (!purposeRead && purpose != PXR_NS::UsdGeomTokens->default_) {
            return;
        }
        if (isTarget(prim)) {
            if (purposeRead && seen.insert(prim.GetPath()).second) {
                out->push_back(prim);
            }

            return;
        }
        for (const PXR_NS::UsdPrim& child : prim.GetFilteredChildren(readPredicate(includeInactive))) {
            visit(child, purposeInfo);
        }
    }
};

// What a packed primitive names and whether its source is active.
struct PackedRecord {
    GeoOffset primitive = 0;
    GeoOffset point = 0;
    std::string path;
    std::string name;
    bool active = true;
    // Only instances an instancer lists in invisibleIds are hidden; a prim's own visibility
    // stays on the prim it names.
    bool invisible = false;
};

void
writePackedRecords(GeoDetailBuilder& builder,
                   const std::vector<PackedRecord>& records,
                   const std::string& pathAttribute,
                   const std::string& nameAttribute)
{
    if (records.empty()) {
        return;
    }
    const std::pair<const std::string*, bool> strings[] = { { &pathAttribute, true }, { &nameAttribute, false } };
    for (const auto& [name, isPath] : strings) {
        if (name->empty()) {
            continue;
        }
        GeoAttributeDesc desc;
        desc.name = *name;
        desc.storage = eGeoStorageString;
        GeoAttribute& attribute = builder.addAttribute(eGeoAttribClassPrimitive, desc);
        for (const PackedRecord& record : records) {
            attribute.setString(std::size_t(record.primitive), isPath ? record.path : record.name);
        }
    }
    if (std::any_of(records.begin(), records.end(), [](const PackedRecord& r) { return r.invisible; })) {
        GeoAttributeDesc desc;
        desc.name = kGeoUsdVisibilityAttr;
        desc.storage = eGeoStorageString;
        GeoAttribute& attribute = builder.addAttribute(eGeoAttribClassPrimitive, desc);
        for (const PackedRecord& record : records) {
            attribute.setString(std::size_t(record.primitive), record.invisible ? "invisible" : "inherited");
        }
    }
    if (std::any_of(records.begin(), records.end(), [](const PackedRecord& r) { return !r.active; })) {
        GeoAttributeDesc desc;
        desc.name = kGeoUsdActiveAttr;
        desc.storage = eGeoStorageInt32;
        GeoAttribute& attribute = builder.addAttribute(eGeoAttribClassPrimitive, desc);
        for (const PackedRecord& record : records) {
            attribute.set<std::int32_t>(std::size_t(record.primitive), record.active ? 1 : 0);
        }
    }
}

struct PackedContext {
    GeoDetailBuilder& builder;
    PXR_NS::UsdGeomXformCache& xforms;
    PXR_NS::UsdGeomBBoxCache& bboxes;
    PXR_NS::UsdTimeCode time;
    USDToGeoPivotLocationEnum pivotLocation;
    std::vector<PackedRecord>& records;
};

GeoOffset
appendPacked(PackedContext& c,
             const PXR_NS::UsdPrim& source,
             const PXR_NS::GfMatrix4d& parentWorld,
             const PXR_NS::GfMatrix4d& worldUsd,
             bool active)
{
    const GeoMatrix4 world = geoMatrixFromUsd(worldUsd);
    const GeoBounds bounds = geoBoundsFromRange(c.bboxes.ComputeUntransformedBound(source).ComputeAlignedRange());
    std::array<double, 3> local = { 0., 0., 0. };
    if (c.pivotLocation == eUSDToGeoPivotLocationCentroid) {
        for (int i = 0; i < 3; ++i) {
            local[i] = 0.5 * (bounds.min[i] + bounds.max[i]);
        }
    }
    const std::array<double, 3> pivot = geoTransformPoint(world, local);
    const GeoOffset point = c.builder.appendPoints(1);
    c.builder.setPointPosition(point, pivot[0], pivot[1], pivot[2]);
    const GeoOffset primitive = geoAppendPackedUSD(c.builder, point, source.GetPath().GetString(), geoMatrixFromUsd(parentWorld), world, bounds);
    c.records.push_back(PackedRecord { primitive, point, source.GetPath().GetString(), source.GetName().GetString(), active, false });

    return primitive;
}

// One packed primitive per instance, on its prototype, with the instancer's per-instance data on
// the pivot points. Instances the instancer deactivates are dropped unless includeInactive.
void
appendPointInstances(PackedContext& c,
                     const PXR_NS::UsdGeomPointInstancer& instancer,
                     bool includeInactive,
                     const GeoUsdReadOptions& readOptions,
                     std::vector<GeoOffset>* dropped,
                     std::vector<std::string>* warnings)
{
    const PXR_NS::UsdPrim prim = instancer.GetPrim();
    const std::string context = prim.GetPath().GetString();
    PXR_NS::VtArray<PXR_NS::GfMatrix4d> transforms;
    // The mask is applied by hand so that instance i keeps index i in the per-instance arrays.
    if (!instancer.ComputeInstanceTransformsAtTime(&transforms, c.time, c.time, PXR_NS::UsdGeomPointInstancer::IncludeProtoXform, PXR_NS::UsdGeomPointInstancer::IgnoreMask)) {
        warnings->push_back(context + ": the instance transforms could not be computed; no instance is read");

        return;
    }
    PXR_NS::VtIntArray protoIndices;
    instancer.GetProtoIndicesAttr().Get(&protoIndices, c.time);
    PXR_NS::SdfPathVector prototypes;
    instancer.GetPrototypesRel().GetForwardedTargets(&prototypes);
    PXR_NS::VtInt64Array ids;
    instancer.GetIdsAttr().Get(&ids, c.time);
    // ComputeMaskAtTime also masks invisibleIds, which are read as hidden instances rather than
    // dropped, so only inactiveIds decide whether an instance is active.
    PXR_NS::SdfInt64ListOp inactiveIdsOp;
    prim.GetMetadata(PXR_NS::UsdGeomTokens->inactiveIds, &inactiveIdsOp);
    const std::vector<std::int64_t> inactiveIdList = inactiveIdsOp.GetAppliedItems();
    const std::unordered_set<std::int64_t> inactive(inactiveIdList.begin(), inactiveIdList.end());
    PXR_NS::VtInt64Array invisibleIds;
    instancer.GetInvisibleIdsAttr().Get(&invisibleIds, c.time);
    const std::unordered_set<std::int64_t> invisible(invisibleIds.begin(), invisibleIds.end());

    const PXR_NS::GfMatrix4d instancerWorld = c.xforms.GetLocalToWorldTransform(prim);
    const PXR_NS::UsdStagePtr stage = prim.GetStage();
    const std::size_t count = std::min(transforms.size(), protoIndices.size());
    std::vector<std::size_t> pivots;
    for (std::size_t i = 0; i < count; ++i) {
        const int protoIndex = protoIndices[i];
        const PXR_NS::UsdPrim prototype = (protoIndex >= 0 && std::size_t(protoIndex) < prototypes.size()) ? stage->GetPrimAtPath(prototypes[std::size_t(protoIndex)]) : PXR_NS::UsdPrim();
        if (!prototype) {
            warnings->push_back(context + ": instance " + std::to_string(i) + " has no prototype and is skipped");
            continue;
        }
        const std::int64_t id = (i < ids.size()) ? ids[i] : std::int64_t(i);
        const bool active = inactive.count(id) == 0;
        const GeoOffset primitive = appendPacked(c, prototype, instancerWorld, transforms[i] * instancerWorld, active);
        PackedRecord& record = c.records.back();
        pivots.push_back(std::size_t(record.point));
        record.invisible = invisible.count(id) != 0;
        if (!active && !includeInactive) {
            dropped->push_back(primitive);
        }
    }
    if (pivots.empty()) {
        return;
    }
    // Instance k of the kept ones is instance k of the instancer only when none was skipped for
    // a missing prototype; otherwise the per-instance arrays no longer line up and are not read.
    if (pivots.size() != count) {
        warnings->push_back(context + ": instances without a prototype were skipped, so per-instance attributes are not read");

        return;
    }
    const std::pair<PXR_NS::UsdAttribute, const char*> perInstance[] = {
        { instancer.GetIdsAttr(), "id" },
        { instancer.GetVelocitiesAttr(), "v" },
        { instancer.GetAngularVelocitiesAttr(), "w" },
        { instancer.GetAccelerationsAttr(), "accel" },
    };
    for (const auto& [attribute, name] : perInstance) {
        PXR_NS::VtValue value;
        if (attribute.HasAuthoredValue() && attribute.Get(&value, c.time)) {
            geoUsdReadValue(c.builder, eGeoAttribClassPoint, name, attribute.GetTypeName(), 1, value, pivots, false, context, warnings);
        }
    }
    const std::vector<std::size_t> detailOffsets { 0 };
    for (const PXR_NS::UsdGeomPrimvar& primvar : PXR_NS::UsdGeomPrimvarsAPI(prim).GetPrimvarsWithValues()) {
        const std::string usdName = primvar.GetPrimvarName().GetString();
        if (!geoNameMatchesPattern(readOptions.importPrimvars, usdName)) {
            continue;
        }
        PXR_NS::VtValue value;
        if (!primvar.ComputeFlattened(&value, c.time)) {
            continue;
        }
        std::string name = usdName;
        if (name == "st" && readOptions.translateStToUv) {
            name = "uv";
        } else if (name == "displayColor") {
            name = "Cd";
        }
        const PXR_NS::TfToken interpolation = primvar.GetInterpolation();
        const bool constant = interpolation == PXR_NS::UsdGeomTokens->constant;
        geoUsdReadValue(c.builder, constant ? eGeoAttribClassDetail : eGeoAttribClassPoint, name, primvar.GetTypeName(), primvar.GetElementSize(), value,
                        constant ? detailOffsets : pivots, geoNameMatchesPattern(readOptions.nonTransformingPrimvars, usdName), context, warnings);
    }
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

    KnobChoicePtr traversal = createKnob<KnobChoice>(tr("Traversal"));
    traversal->setName(kUSDToGeoParamTraversal);
    traversal->setHintToolTip(tr("Which prims at or below each matched prim become geometry. Only the outermost prim of "
                                 "the chosen sort below a matched prim is taken, never one inside it.")
                                  .toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("none", tr("No Traversal").toStdString(), tr("Each matched prim itself.").toStdString()));
        options.push_back(ChoiceOption("components", tr("Components").toStdString(), tr("Each prim of kind component.").toStdString()));
        options.push_back(ChoiceOption("gprims", tr("Gprims").toStdString(),
                                       tr("Each geometric prim (mesh, points, curves, shapes), and each point instancer.").toStdString()));
        options.push_back(ChoiceOption("groups", tr("Groups").toStdString(), tr("Each prim of kind group or assembly.").toStdString()));
        traversal->populateChoices(options);
    }
    traversal->setDefaultValue((int)eUSDToGeoTraversalNone);
    traversal->setAnimationEnabled(false);
    page->addKnob(traversal);
    _traversal = traversal;

    KnobStringPtr purpose = createKnob<KnobString>(tr("Purpose"));
    purpose->setName(kUSDToGeoParamPurpose);
    purpose->setHintToolTip(tr("The purposes read, separated by spaces: at least one of default, render, proxy and guide. "
                               "A prim whose computed purpose is render, proxy or guide and not listed is not read, nor is "
                               "anything below it. A default-purpose prim is not read unless default is listed, but the "
                               "prims below it are still searched.")
                                .toStdString());
    purpose->setDefaultValue("default render proxy");
    purpose->setAnimationEnabled(false);
    page->addKnob(purpose);
    _purpose = purpose;

    KnobBoolPtr excludeInactive = createKnob<KnobBool>(tr("Exclude Inactive Primitives"));
    excludeInactive->setName(kUSDToGeoParamExcludeInactivePrimitives);
    excludeInactive->setHintToolTip(tr("On, inactive prims are not read. Off, they are read and their primitives get "
                                       "usdactive set to 0 (1 on the others), so the scene they are written back to can "
                                       "reactivate them or keep them inactive.")
                                        .toStdString());
    excludeInactive->setDefaultValue(true);
    excludeInactive->setAnimationEnabled(false);
    page->addKnob(excludeInactive);
    _excludeInactive = excludeInactive;

    KnobBoolPtr pointInstances = createKnob<KnobBool>(tr("Import Point Instances"));
    pointInstances->setName(kUSDToGeoParamImportPointInstances);
    pointInstances->setHintToolTip(tr("When packed, off, a point instancer becomes one packed primitive. On, each of its "
                                      "instances becomes a packed primitive on its prototype, placed by the instance's "
                                      "transform, with the instancer's ids, velocities, angular velocities, accelerations "
                                      "and per-instance primvars on its point as id, v, w, accel and the primvar names. "
                                      "Path Attribute and Name Attribute then hold the prototype's path and name, and "
                                      "instances listed in invisibleIds get usdvisibility set to invisible.")
                                       .toStdString());
    pointInstances->setDefaultValue(false);
    pointInstances->setAnimationEnabled(false);
    page->addKnob(pointInstances);
    _importPointInstances = pointInstances;

    KnobChoicePtr timeSample = createKnob<KnobChoice>(tr("Time Sample"));
    timeSample->setName(kUSDToGeoParamTimeSample);
    timeSample->setHintToolTip(tr("Whether the scene is read at the current frame or at one fixed frame.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("animated", tr("Animated").toStdString(), tr("Read at the frame being rendered.").toStdString()));
        options.push_back(ChoiceOption("static", tr("Static").toStdString(),
                                       tr("Read at Import Frame whatever the frame; the geometry is the same at every frame.").toStdString()));
        timeSample->populateChoices(options);
    }
    timeSample->setDefaultValue((int)eUSDToGeoTimeSampleAnimated);
    timeSample->setAnimationEnabled(false);
    page->addKnob(timeSample);
    _timeSample = timeSample;

    KnobDoublePtr importFrame = createKnob<KnobDouble>(tr("Import Frame"));
    importFrame->setName(kUSDToGeoParamImportFrame);
    importFrame->setHintToolTip(tr("With Time Sample set to Static, the frame the scene is read at.").toStdString());
    importFrame->setDefaultValue(1.);
    importFrame->setAnimationEnabled(false);
    page->addKnob(importFrame);
    _importFrame = importFrame;

    KnobStringPtr pathAttribute = createKnob<KnobString>(tr("Path Attribute"));
    pathAttribute->setName(kUSDToGeoParamPathAttribute);
    pathAttribute->setHintToolTip(tr("The primitive string attribute that holds the path of the prim each primitive came "
                                     "from (a point attribute for a points prim). Blank writes none.")
                                      .toStdString());
    pathAttribute->setDefaultValue("path");
    pathAttribute->setAnimationEnabled(false);
    page->addKnob(pathAttribute);
    _pathAttribute = pathAttribute;

    KnobStringPtr nameAttribute = createKnob<KnobString>(tr("Name Attribute"));
    nameAttribute->setName(kUSDToGeoParamNameAttribute);
    nameAttribute->setHintToolTip(tr("The primitive string attribute that holds the name of the prim each primitive came "
                                     "from, the last element of its path. Blank writes none.")
                                      .toStdString());
    nameAttribute->setDefaultValue("");
    nameAttribute->setAnimationEnabled(false);
    page->addKnob(nameAttribute);
    _nameAttribute = nameAttribute;

    KnobChoicePtr pivot = createKnob<KnobChoice>(tr("Pivot Location"));
    pivot->setName(kUSDToGeoParamPivotLocation);
    pivot->setHintToolTip(tr("When packed, where each packed primitive's point goes. The point does not move the "
                             "contents.")
                              .toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("centroid", tr("Centroid").toStdString(), tr("The centre of the prim's bounds, in world space.").toStdString()));
        options.push_back(ChoiceOption("origin", tr("Origin").toStdString(), tr("The prim's origin, in world space.").toStdString()));
        pivot->populateChoices(options);
    }
    pivot->setDefaultValue((int)eUSDToGeoPivotLocationCentroid);
    pivot->setAnimationEnabled(false);
    page->addKnob(pivot);
    _pivotLocation = pivot;

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

    KnobStringPtr importPrimvars = createKnob<KnobString>(tr("Import Primvars"));
    importPrimvars->setName(kUSDToGeoParamImportPrimvars);
    importPrimvars->setHintToolTip(tr("When unpacked, the primvars read, as space-separated globs over their USD names "
                                      "(displayColor, st); a glob starting with ^ removes what it matches. displayColor "
                                      "becomes Cd.")
                                       .toStdString());
    importPrimvars->setDefaultValue("*");
    importPrimvars->setAnimationEnabled(false);
    page->addKnob(importPrimvars);
    _importPrimvars = importPrimvars;

    KnobBoolPtr inherited = createKnob<KnobBool>(tr("Import Inherited Primvars"));
    inherited->setName(kUSDToGeoParamImportInheritedPrimvars);
    inherited->setHintToolTip(tr("When unpacked, also reads the constant primvars a prim inherits from its ancestors, "
                                 "filtered by Import Primvars. The prim's own primvar wins over an inherited one of the "
                                 "same name.")
                                  .toStdString());
    inherited->setDefaultValue(false);
    inherited->setAnimationEnabled(false);
    page->addKnob(inherited);
    _importInheritedPrimvars = inherited;

    KnobStringPtr importAttributes = createKnob<KnobString>(tr("Import Attributes"));
    importAttributes->setName(kUSDToGeoParamImportAttributes);
    importAttributes->setHintToolTip(tr("When unpacked, the custom attributes read besides the primitives' primvars, as "
                                        "globs over their names. Schema attributes are not matched. A custom attribute "
                                        "has no interpolation, so it becomes a point, vertex or primitive attribute when "
                                        "its array holds one value per point, vertex or primitive, and a detail "
                                        "attribute otherwise.")
                                         .toStdString());
    importAttributes->setDefaultValue("");
    importAttributes->setAnimationEnabled(false);
    page->addKnob(importAttributes);
    _importAttributes = importAttributes;

    KnobStringPtr nonTransforming = createKnob<KnobString>(tr("Non Transforming Primvars"));
    nonTransforming->setName(kUSDToGeoParamNonTransformingPrimvars);
    nonTransforming->setHintToolTip(tr("When unpacked, globs over the primvars read as plain values rather than points, "
                                       "vectors or normals, so transforming the geometry leaves them as they are. rest "
                                       "positions are the usual case.")
                                        .toStdString());
    nonTransforming->setDefaultValue("rest");
    nonTransforming->setAnimationEnabled(false);
    page->addKnob(nonTransforming);
    _nonTransformingPrimvars = nonTransforming;

    KnobBoolPtr translateSt = createKnob<KnobBool>(tr("Translate ST To UV"));
    translateSt->setName(kUSDToGeoParamTranslateStToUv);
    translateSt->setHintToolTip(tr("When unpacked, reads the st primvar as the uv attribute the geometry nodes use. Off, "
                                   "it stays st.")
                                    .toStdString());
    translateSt->setDefaultValue(true);
    translateSt->setAnimationEnabled(false);
    page->addKnob(translateSt);
    _translateStToUv = translateSt;

    KnobBoolPtr writeXform = createKnob<KnobBool>(tr("Write USD Xform"));
    writeXform->setName(kUSDToGeoParamWriteUsdXform);
    writeXform->setHintToolTip(tr("When unpacked, writes each prim's local-to-world matrix on its primitives as the "
                                  "usdxform attribute (on the points of a points prim). It records where the prim was, "
                                  "so transforming the geometry leaves it as it is, and it is not written back to USD.")
                                   .toStdString());
    writeXform->setDefaultValue(true);
    writeXform->setAnimationEnabled(false);
    page->addKnob(writeXform);
    _writeUsdXform = writeXform;

    KnobBoolPtr applyInverse = createKnob<KnobBool>(tr("Apply Inverse USD Transform"));
    applyInverse->setName(kUSDToGeoParamApplyInverseUsdTransform);
    applyInverse->setHintToolTip(tr("When unpacked, applies the inverse of each prim's world transform to points read in "
                                    "world space, so the points are in the prim's own space and writing them back to "
                                    "the same prim does not apply its transform twice. It overrides Flatten Transforms.")
                                     .toStdString());
    applyInverse->setDefaultValue(false);
    applyInverse->setAnimationEnabled(false);
    page->addKnob(applyInverse);
    _applyInverseUsdTransform = applyInverse;

    KnobBoolPtr materialPath = createKnob<KnobBool>(tr("Import Material Path"));
    materialPath->setName(kUSDToGeoParamImportMaterialPath);
    materialPath->setHintToolTip(tr("When unpacked, writes the path of the material bound to each prim on its primitives "
                                    "as the usdmaterialpath attribute. Faces of a GeomSubset with its own binding get "
                                    "the subset's material.")
                                     .toStdString());
    materialPath->setDefaultValue(true);
    materialPath->setAnimationEnabled(false);
    page->addKnob(materialPath);
    _importMaterialPath = materialPath;

    updateEnabled();
}

bool
USDToGeo::isStatic() const
{
    return choiceOf(_timeSample) == (int)eUSDToGeoTimeSampleStatic;
}

bool
USDToGeo::isOutputTimeInvariantWhateverInputs() const
{
    return isStatic();
}

void
USDToGeo::updateEnabled()
{
    const KnobBoolPtr unpackKnob = _unpack.lock();
    if (!unpackKnob) {
        return;
    }
    const bool unpack = unpackKnob->getValue();
    const KnobBoolPtr applyInverse = _applyInverseUsdTransform.lock();
    const bool inverse = applyInverse && applyInverse->getValue();
    const KnobIPtr unpackedOnly[] = {
        _importPrimvars.lock(),
        _importInheritedPrimvars.lock(),
        _importAttributes.lock(),
        _nonTransformingPrimvars.lock(),
        _translateStToUv.lock(),
        _writeUsdXform.lock(),
        _applyInverseUsdTransform.lock(),
        _importMaterialPath.lock(),
    };
    for (const KnobIPtr& knob : unpackedOnly) {
        if (knob) {
            knob->setAllDimensionsEnabled(unpack);
        }
    }
    if (const KnobBoolPtr flatten = _flatten.lock()) {
        flatten->setAllDimensionsEnabled(unpack && !inverse);
    }
    const KnobIPtr packedOnly[] = { _pivotLocation.lock(), _importPointInstances.lock() };
    for (const KnobIPtr& knob : packedOnly) {
        if (knob) {
            knob->setAllDimensionsEnabled(!unpack);
        }
    }
    if (const KnobDoublePtr importFrame = _importFrame.lock()) {
        importFrame->setAllDimensionsEnabled(isStatic());
    }
}

void
USDToGeo::onKnobsLoaded()
{
    updateEnabled();
}

bool
USDToGeo::knobChanged(KnobI* k,
                      ValueChangedReasonEnum /*reason*/,
                      ViewSpec /*view*/,
                      double /*time*/,
                      bool /*originatedFromMainThread*/)
{
    const KnobIPtr drivers[] = { _unpack.lock(), _applyInverseUsdTransform.lock(), _timeSample.lock() };
    for (const KnobIPtr& driver : drivers) {
        if (driver && (k == driver.get())) {
            updateEnabled();

            return true;
        }
    }

    return false;
}

StatusEnum
USDToGeo::getPreferredMetadataAt(double /*time*/,
                                 ViewIdx /*view*/,
                                 NodeMetadata& metadata)
{
    // A static sample reads one frame of the input whatever the time, so the geometry is the same
    // at every frame and the cache need not key it by time.
    if (isStatic()) {
        return eStatusOK;
    }
    // The input scene's payload is the same at every frame while its prims may be animated, so
    // until a render has looked at the prims the geometry must be taken to vary.
    bool varying = true;
    {
        std::lock_guard<std::mutex> lock(_varyingMutex);
        if (_varyingKnown && (_varyingHash == getHash())) {
            varying = _varying;
        }
    }
    if (varying) {
        metadata.setIsFrameVarying(true);
    }

    return eStatusOK;
}

void
USDToGeo::setFrameVarying(U64 hash,
                          bool varying)
{
    {
        std::lock_guard<std::mutex> lock(_varyingMutex);
        const bool before = (_varyingKnown && (_varyingHash == hash)) ? _varying : true;
        _varyingHash = hash;
        _varyingKnown = true;
        _varying = varying;
        if ((before == varying) || (getHash() != hash)) {
            return;
        }
    }
    dropCachedMetadataDownstream();
}

StatusEnum
USDToGeo::renderGeometry(const GeoRenderActionArgs& args,
                         GeoDetailConstPtr* outputDetail,
                         std::string* errorMessage)
{
    const std::string pattern = _primPattern.getExpression();
    const bool unpack = boolAt(_unpack, args.time, args.view);
    const bool applyInverse = unpack && boolAt(_applyInverseUsdTransform, args.time, args.view);
    // Points baked into world space and then brought back by the inverse are the prim's own
    // points, so the inverse is applied by not baking.
    const bool flatten = unpack && !applyInverse && boolAt(_flatten, args.time, args.view);
    const bool staticSample = isStatic();
    const KnobDoublePtr importFrameKnob = _importFrame.lock();
    const double readFrame = (staticSample && importFrameKnob) ? importFrameKnob->getValue() : args.time;
    const USDToGeoTraversalEnum traversal = (USDToGeoTraversalEnum)choiceOf(_traversal);
    const USDToGeoPivotLocationEnum pivotLocation = (USDToGeoPivotLocationEnum)choiceOf(_pivotLocation);
    const bool includeInactive = !boolAt(_excludeInactive, args.time, args.view);
    const bool pointInstances = !unpack && boolAt(_importPointInstances, args.time, args.view);
    const bool writeXform = unpack && boolAt(_writeUsdXform, args.time, args.view);
    const std::string pathAttribute = stringOf(_pathAttribute);
    const std::string nameAttribute = stringOf(_nameAttribute);

    GeoUsdReadOptions readOptions;
    readOptions.pathAttribute = pathAttribute;
    readOptions.nameAttribute = nameAttribute;
    readOptions.importPrimvars = stringOf(_importPrimvars);
    readOptions.importInheritedPrimvars = boolAt(_importInheritedPrimvars, args.time, args.view);
    readOptions.importAttributes = stringOf(_importAttributes);
    readOptions.nonTransformingPrimvars = stringOf(_nonTransformingPrimvars);
    readOptions.translateStToUv = boolAt(_translateStToUv, args.time, args.view);
    readOptions.importMaterialPath = boolAt(_importMaterialPath, args.time, args.view);

    PXR_NS::TfTokenVector purposes;
    if (!PrimSelector::validate(pattern, errorMessage) || !parsePurposes(stringOf(_purpose), &purposes, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }

    ScenePayload scene;
    const EffectInstancePtr input = getInput(0);
    if (input && !PrimSelector::isBlank(pattern)) {
        if (input->evaluateScene(EvaluateSceneArgs(readFrame, args.view), &scene, errorMessage) != eRenderRoIRetCodeOk) {
            if (errorMessage->empty()) {
                *errorMessage = tr("The input scene could not be evaluated.").toStdString();
            }
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
    }
    // Taken after the input's evaluation: a reader's first evaluation walks its file and moves its
    // hash, and so this node's, and the answer must be kept under the hash the metadata will see.
    const U64 hash = getHash();

    std::vector<std::string> skipped;
    std::vector<std::string> warnings;
    bool varying = false;
    const StatusEnum status = renderGeometryGenerator([&](GeoDetailBuilder& builder, std::string* error) {
        if (scene.isEmpty()) {
            return eStatusOK;
        }
        // A geometry render has no viewer load set, and the detail it makes is cached and shared by
        // every consumer, so the pattern is matched against the whole scene, payloads included.
        SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(scene, SceneLoadSet::everything(), error);
        const PXR_NS::UsdStageRefPtr stage = lease.getStage();
        if (!stage) {
            if (error->empty()) {
                *error = tr("USD could not compose the input scene.").toStdString();
            }

            return eStatusFailed;
        }
        std::vector<PXR_NS::SdfPath> matches;
        const bool selected = SceneOps::selectPrimsWithPredicate(stage, pattern, &matches, error, readPredicate(includeInactive));
        if (!selected) {
            return eStatusFailed;
        }
        std::vector<PXR_NS::UsdPrim> prims;
        {
            TraversalWalk walk;
            walk.traversal = traversal;
            walk.purposes = purposes;
            walk.includeInactive = includeInactive;
            walk.out = &prims;
            for (const PXR_NS::SdfPath& path : matches) {
                const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(path);
                if (!prim || prim.IsInstanceProxy()) {
                    skipped.push_back(path.GetString());
                    continue;
                }
                walk.visit(prim);
            }
        }

        const AppInstancePtr app = getApp();
        const double fps = (app && app->getProject()) ? app->getProject()->getProjectFrameRate() : 24.;
        const PXR_NS::UsdTimeCode time(SceneOps::frameToTimeCode(scene, readFrame, fps));
        PXR_NS::UsdGeomXformCache xforms(time);

        if (!unpack) {
            PXR_NS::UsdGeomBBoxCache bboxes(time, purposes, true);
            std::vector<PackedRecord> records;
            std::vector<GeoOffset> dropped;
            PackedContext context { builder, xforms, bboxes, time, pivotLocation, records };
            for (const PXR_NS::UsdPrim& prim : prims) {
                if (!prim.IsA<PXR_NS::UsdGeomImageable>()) {
                    skipped.push_back(prim.GetPath().GetString());
                    continue;
                }
                if (pointInstances && prim.IsA<PXR_NS::UsdGeomPointInstancer>()) {
                    varying = varying || pointInstancesMightVary(PXR_NS::UsdGeomPointInstancer(prim));
                    appendPointInstances(context, PXR_NS::UsdGeomPointInstancer(prim), includeInactive, readOptions, &dropped, &warnings);
                    continue;
                }
                varying = varying || packedMightVary(prim);
                bool resetsXformStack = false;
                const PXR_NS::GfMatrix4d local = xforms.GetLocalTransformation(prim, &resetsXformStack);
                const PXR_NS::GfMatrix4d parentWorld = resetsXformStack ? PXR_NS::GfMatrix4d(1.) : xforms.GetParentToWorldTransform(prim);
                appendPacked(context, prim, parentWorld, local * parentWorld, prim.IsActive());
            }
            writePackedRecords(builder, records, pathAttribute, nameAttribute);
            if (!dropped.empty()) {
                builder.deletePrimitives(std::span<const GeoOffset>(dropped.data(), dropped.size()), true);
            }

            return eStatusOK;
        }

        for (const PXR_NS::UsdPrim& prim : prims) {
            if (!isReadableUnpacked(prim)) {
                skipped.push_back(prim.GetPath().GetString());
                continue;
            }
            varying = varying || unpackedMightVary(prim, flatten || writeXform, readOptions.importInheritedPrimvars);
            const PXR_NS::GfMatrix4d world = xforms.GetLocalToWorldTransform(prim);
            readOptions.usdXform = writeXform ? &world : nullptr;
            std::string readError;
            if (!geoUsdReadPrim(prim, time, flatten ? &world : nullptr, readOptions, builder, &warnings, &readError)) {
                skipped.push_back(prim.GetPath().GetString() + ": " + readError);
            }
        }
        readOptions.usdXform = nullptr;
        if (flatten) {
            GeoAttributeDesc worldSpaceDesc;
            worldSpaceDesc.name = kGeoUsdWorldSpaceAttr;
            worldSpaceDesc.storage = eGeoStorageInt32;
            builder.addAttribute(eGeoAttribClassDetail, worldSpaceDesc).set<std::int32_t>(0, 1);
        }

        return eStatusOK;
    },
                                                      outputDetail, errorMessage);
    if (status != eStatusOK) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return status;
    }
    setFrameVarying(hash, varying && !staticSample);

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
