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

#ifndef Engine_Nodes_Geometry_GeoToUSD_h
#define Engine_Nodes_Geometry_GeoToUSD_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <memory>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/NativeEffectBase.h"

// Free of USD: NativeRegistryAll.inc includes every node header into translation units that keep
// Py_LIMITED_API, which USD's headers cannot be compiled under.

#define PLUGINID_NATRON_GEOTOUSD "GeoToUSD"
#define kGeoToUSDParamGroup "group"
#define kGeoToUSDParamGroupType "groupType"
#define kGeoToUSDParamPathAttributes "pathAttributes"
#define kGeoToUSDParamPathPrefix "pathPrefix"
#define kGeoToUSDParamPrefixAbsolutePaths "prefixAbsolutePaths"
#define kGeoToUSDParamOtherPrimitives "otherPrimitives"
#define kGeoToUSDParamDefineOnlyLeafPrimitives "defineOnlyLeafPrimitives"
#define kGeoToUSDParamPackedUsdPrimitives "packedUsdPrimitives"
#define kGeoToUSDParamPackedPrimitives "packedPrimitives"
#define kGeoToUSDParamParentPrimitiveType "parentPrimitiveType"
#define kGeoToUSDParamKindAuthoring "kindAuthoring"
#define kGeoToUSDParamAdjustTransformsForInputHierarchy "adjustTransformsForInputHierarchy"
#define kGeoToUSDParamLoadAsReference "loadAsReference"
#define kGeoToUSDParamReferenceType "referenceType"
#define kGeoToUSDParamReferencePrimitive "referencePrimitive"
#define kGeoToUSDParamReferencePrimitivePath "referencePrimitivePath"
#define kGeoToUSDParamMakeInstanceable "makeInstanceable"
#define kGeoToUSDParamParentPrimitiveKind "parentPrimitiveKind"
#define kGeoToUSDParamLayerStyle "layerStyle"
#define kGeoToUSDParamAuthorTimeSamples "authorTimeSamples"
#define kGeoToUSDParamDefaultPointWidth "defaultPointWidth"
#define kGeoToUSDInputGeometry 0
#define kGeoToUSDInputScene 1

NATRON_NAMESPACE_ENTER

/**
 * @brief Puts geometry on the scene stack, over the optional Scene input. By default the node's
 * layer sublayers a natrongeo layer (GeoSdfFileFormat) that serves the detail without copying
 * it; a changed detail is applied to that layer in place, before the scene evaluation that needs
 * it takes its read scope (prepareSceneSources()). The layer is keyed by the node, plus the time
 * when anything upstream varies with it.
 *
 * Only the part of the detail the group selects (selectGeometry()) is gathered and converted.
 *
 * Path Attributes and Path Prefix place each partition (GeoUsdConvert); Other Primitives makes
 * them defs or overs of the Scene input's prims. Layer Style Copy Into Node Layer authors the
 * detail into the node's own layer instead, optionally as time samples over the project range.
 * Load As Reference instead puts the served layer on the prefix prim as a reference or payload.
 **/
class GeoToUSD
    : public NativeEffectBase {
public:
    enum PackedPrimitivesEnum {
        ePackedPrimitivesCreateXforms = 0,
        ePackedPrimitivesNativeInstances,
        ePackedPrimitivesPointInstancer,
        ePackedPrimitivesUnpack
    };

    enum LayerStyleEnum {
        eLayerStyleSublayer = 0,
        eLayerStyleCopyIntoNodeLayer
    };

    enum ReferenceTypeEnum {
        eReferenceTypeWeakerPayload = 0,
        eReferenceTypeStrongerPayload,
        eReferenceTypeWeakerReference,
        eReferenceTypeStrongerReference
    };

    enum ReferencePrimitiveEnum {
        eReferencePrimitiveAutomatic = 0,
        eReferencePrimitiveDefault,
        eReferencePrimitiveSpecific
    };

    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Geometry", .kind = eDataKindGeometry },
        InputSpec { .label = "Scene", .kind = eDataKindScene, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GEOTOUSD,
        .label = "GeoToUSD",
        .description = "Put geometry on the scene, over the optional Scene input; Group restricts it to the selected "
                       "points or primitives. The first of the Path Attributes a primitive has names the prim it "
                       "becomes: relative paths go under Path Prefix, and so do absolute ones with Prefix Absolute "
                       "Paths; primitives without one go under Path Prefix as mesh_0, curves_0 and so on. Other "
                       "Primitives defines the prims, or overlays the Scene input's prims at those paths; set it to "
                       "Overlay to edit geometry read with USDToGeo in place. Packed Primitives and Packed USD "
                       "Primitives choose what packed geometry becomes. The geometry is served to the scene as it "
                       "is, without a copy, as a sublayer or as a reference or payload on the Path Prefix prim "
                       "(Load As Reference); Layer Style Copy Into Node Layer copies it into the node's layer "
                       "instead.",
        .grouping = PLUGIN_GROUP_GEOMETRY,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindScene,
        .inputs = kInputs,
        .geoGroupKnob = kGeoToUSDParamGroup,
        .geoGroupClasses = eGeoGroupClassPoints | eGeoGroupClassPrimitives,
        .geoGroupTypeKnob = kGeoToUSDParamGroupType,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new GeoToUSD(node);
    }

    explicit GeoToUSD(NodePtr node);

    virtual ~GeoToUSD();

    /**
     * @brief hook is called with each frame a scene render has gathered the geometry of, on the
     * thread that gathered it, until this is called again with an empty one.
     **/
    static void setFrameGatheredHookForTests(const std::function<void(double frame)>& hook);

    /**
     * @brief The part of @p detail a group pattern converts, in @p selected. A primitive group
     * keeps the selected primitives and the points they use. A point group keeps the primitives
     * whose points are all selected, their points, and the selected points no primitive uses.
     * Element offsets and attribute values of what is kept do not change; a blank pattern, or
     * one that selects everything, gives @p detail itself. @p groupType is points, primitives
     * or guess, which resolves to points or primitives. False with @p error set for a malformed
     * pattern or one that names another class.
     **/
    static bool selectGeometry(const GeoDetailConstPtr& detail,
                               const std::string& group,
                               GeoGroupTypeEnum groupType,
                               GeoDetailConstPtr* selected,
                               std::string* error);

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum renderScene(const SceneRenderActionArgs& args,
                                   ScenePayload* outputScene,
                                   std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual RenderRoIRetCode prepareSceneSources(const EvaluateSceneArgs& args,
                                                 std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void onScriptNameChanged(const std::string& fullyQualifiedName) OVERRIDE FINAL;

    void updateKnobsEnabled();

    KnobStringWPtr _group;
    KnobChoiceWPtr _groupType;
    KnobStringWPtr _pathAttributes;
    KnobStringWPtr _pathPrefix;
    KnobBoolWPtr _prefixAbsolutePaths;
    KnobChoiceWPtr _otherPrimitives;
    KnobBoolWPtr _defineOnlyLeafPrimitives;
    KnobChoiceWPtr _packedUsdPrimitives;
    KnobChoiceWPtr _packedPrimitives;
    KnobChoiceWPtr _parentPrimitiveType;
    KnobChoiceWPtr _kindAuthoring;
    KnobBoolWPtr _adjustTransformsForInputHierarchy;
    KnobBoolWPtr _loadAsReference;
    KnobChoiceWPtr _referenceType;
    KnobChoiceWPtr _referencePrimitive;
    KnobStringWPtr _referencePrimitivePath;
    KnobBoolWPtr _makeInstanceable;
    KnobChoiceWPtr _parentPrimitiveKind;
    KnobChoiceWPtr _layerStyle;
    KnobBoolWPtr _authorTimeSamples;
    KnobDoubleWPtr _defaultPointWidth;

    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoToUSD_h
