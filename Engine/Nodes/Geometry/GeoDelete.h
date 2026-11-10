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

#ifndef Engine_Nodes_Geometry_GeoDelete_h
#define Engine_Nodes_Geometry_GeoDelete_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_GEODELETE "GeoDelete"

#define kGeoDeleteParamGroup "group"
#define kGeoDeleteParamGroupType "groupType"
#define kGeoDeleteParamDeleteNonSelected "deleteNonSelected"
#define kGeoDeleteParamDeleteUnusedPoints "deleteUnusedPoints"

NATRON_NAMESPACE_ENTER

/** @brief What GeoDelete removes; the defaults are the knobs' defaults. */
struct GeoDeleteParams {
    std::string group;
    GeoGroupTypeEnum groupType = eGeoGroupTypePrimitives;
    bool deleteNonSelected = false;
    bool deleteUnusedPoints = true;
};

/**
 * @brief Deletes the elements a group pattern selects, or every other element.
 *
 * Deleted elements are tombstoned, so element numbers of what remains stay in order and the
 * attribute pages are not rewritten. Deleting points also deletes the primitives using them
 * and their vertices. Deleting primitives deletes their vertices, and the points no remaining
 * vertex uses when deleteUnusedPoints is on. A blank pattern selects every element.
 **/
class GeoDelete
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindGeometry, .optional = false },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GEODELETE,
        .label = "GeoDelete",
        .description = "Delete the points or primitives a group pattern selects, or keep only them.",
        .grouping = PLUGIN_GROUP_3D,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindGeometry,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new GeoDelete(node);
    }

    explicit GeoDelete(NodePtr node);
    virtual ~GeoDelete();

    /** @brief False with @p error set for a malformed pattern. */
    static bool deleteElements(GeoDetailBuilder& builder,
                               const GeoDeleteParams& params,
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
    virtual StatusEnum renderGeometry(const GeoRenderActionArgs& args,
                                      GeoDetailConstPtr* outputDetail,
                                      std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void updateVisibility();

    KnobStringWPtr _group;
    KnobChoiceWPtr _groupType;
    KnobBoolWPtr _deleteNonSelected;
    KnobBoolWPtr _deleteUnusedPoints;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoDelete_h
