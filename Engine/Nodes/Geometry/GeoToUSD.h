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

#include <memory>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"

// Free of USD: NativeRegistryAll.inc includes every node header into translation units that keep
// Py_LIMITED_API, which USD's headers cannot be compiled under.

#define PLUGINID_NATRON_GEOTOUSD "GeoToUSD"
#define kGeoToUSDParamRootPath "rootPath"
#define kGeoToUSDParamAuthorTimeSamples "authorTimeSamples"
#define kGeoToUSDParamPackedPrimitives "packedPrimitives"
#define kGeoToUSDParamCopyIntoLayer "copyIntoLayer"
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
 * Primitives read from USD (kGeoUsdSourceAttr) whose path names a prim of the Scene input become
 * overrides of that prim, packed transforms written relative to its parent; other primitives
 * only define new prims. Copy Into Layer authors the detail into the node's
 * own layer instead, optionally as time samples over the project range.
 **/
class GeoToUSD
    : public NativeEffectBase {
public:
    enum PackedPrimitivesEnum {
        ePackedPrimitivesPointInstancer = 0,
        ePackedPrimitivesNativeInstances,
        ePackedPrimitivesUnpack
    };

    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Geometry", .kind = eDataKindGeometry },
        InputSpec { .label = "Scene", .kind = eDataKindScene, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GEOTOUSD,
        .label = "GeoToUSD",
        .description = "Put geometry on the scene, over the optional Scene input. Each primitive's path attribute "
                       "names the prim it becomes; primitives without one go under Root Path. Geometry read with "
                       "USDToGeo overrides the prim it was read from in place when the Scene input still holds it, "
                       "so once edited it comes back where it was read from; if that prim is gone it goes under "
                       "Root Path. Other geometry only defines new prims: a path outside Root Path that the Scene "
                       "input already holds goes under Root Path, with a warning. The geometry is served to the scene as it is, "
                       "without a copy; Copy Into Layer copies it into the node's layer instead.",
        .grouping = PLUGIN_GROUP_3D,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindScene,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new GeoToUSD(node);
    }

    explicit GeoToUSD(NodePtr node);

    virtual ~GeoToUSD();

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

    void updateTimeSamplesEnabled();

    KnobStringWPtr _rootPath;
    KnobBoolWPtr _authorTimeSamples;
    KnobChoiceWPtr _packedPrimitives;
    KnobBoolWPtr _copyIntoLayer;

    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoToUSD_h
