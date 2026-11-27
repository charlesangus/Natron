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

#ifndef Engine_Nodes_Geometry_GeoEdit_h
#define Engine_Nodes_Geometry_GeoEdit_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <map>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Geo/GeoPacked.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_GEOEDIT "GeoEdit"

#define kGeoEditParamEdits "edits"
#define kGeoEditParamClearEdits "clearEdits"

NATRON_NAMESPACE_ENTER

struct GeoComponentSelection;

/**
 * @brief The edits a GeoEdit holds: offsets added to P of points and transforms multiplied into
 * packed primitives' transforms, keyed by point and primitive number in the input, with the
 * input's point and primitive counts when they were made. Numbers only name the same elements
 * while the counts are the same, so edits made on other counts are not applied.
 **/
struct GeoEditData {
    GeoIndex pointCount = 0;
    GeoIndex primitiveCount = 0;
    std::map<GeoIndex, std::array<double, 3>> pointOffsets;
    // Left-multiplied into the packed primitive's transform.
    std::map<GeoIndex, GeoMatrix4> primitiveTransforms;

    bool empty() const
    {
        return pointOffsets.empty() && primitiveTransforms.empty();
    }

    // Whether the edits apply to @p detail: none are held, or its counts are the recorded ones.
    bool matches(const GeoDetail& detail) const;

    /**
     * @brief One line of ';'-separated records: the format version "1", the counts
     * "<points> <primitives>", then "p <point> <dx> <dy> <dz>" per point and
     * "x <primitive> <16 values, row-major>" per packed primitive, in ascending number. Values
     * are written in the shortest form that reads back exactly. No edits encode as "".
     **/
    std::string encode() const;

    // False with @p error set for text encode() could not have written.
    static bool decode(const std::string& text,
                       GeoEditData* data,
                       std::string* error);

    bool operator==(const GeoEditData&) const = default;
};

/** @brief What a component selection drags: point numbers, and packed primitive numbers. */
struct GeoEditTargets {
    std::vector<GeoIndex> points;
    std::vector<GeoIndex> packedPrimitives;

    bool empty() const
    {
        return points.empty() && packedPrimitives.empty();
    }
};

/**
 * @brief Offsets points and transforms packed primitives of its input by edits made in a 3D
 * view. Dragging points, edges or primitives picked on the geometry this node shows stores an
 * offset per point dragged (the ends of an edge, the points of a polygon or curve) and a
 * transform per packed primitive. A second drag adds to the first.
 *
 * Edits are keyed by element number, so they are only applied while the input has the point and
 * primitive counts it had when they were made; otherwise the input passes through and the node
 * warns. Pages no edit touches stay shared with the input. N, when the input has it on points,
 * vertices or primitives, is computed again from the edited polygons.
 **/
class GeoEdit
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindGeometry, .optional = false },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GEOEDIT,
        .label = "GeoEdit",
        .description = "Move points, edges and primitives by dragging them in the 3D view. "
                       "View this node, pick points, edges or primitives, and drag the handles. "
                       "Edits follow element numbers, so they are skipped with a warning when "
                       "the input's point or primitive count changes.",
        .grouping = PLUGIN_GROUP_GEOMETRY,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindGeometry,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new GeoEdit(node);
    }

    explicit GeoEdit(NodePtr node);
    virtual ~GeoEdit();

    /**
     * @brief Applies @p data to the builder's detail. False, leaving the detail alone, with
     * @p warning naming both counts, when the edits were made on other counts.
     **/
    static bool apply(GeoDetailBuilder& builder,
                      const GeoEditData& data,
                      std::string* warning);

    /**
     * @brief The points and packed primitives @p selection drags in @p detail: the points
     * picked, the ends of the edges picked, and for primitives picked, the packed ones whole and
     * the points of the others. Numbers not in the detail are dropped.
     **/
    static GeoEditTargets targets(const GeoDetail& detail,
                                  const GeoComponentSelection& selection);

    // The mean of the targets' points and packed primitives' bounds centres; false for none.
    static bool centre(const GeoDetail& detail,
                       const GeoEditTargets& targets,
                       std::array<double, 3>* centre);

    /**
     * @brief Adds a translation by @p delta of the targets to @p data, recording @p detail's
     * counts. @p detail must be geometry the edits apply to (see GeoEditData::matches()).
     **/
    static void translate(GeoEditData* data,
                          const GeoDetail& detail,
                          const GeoEditTargets& targets,
                          const std::array<double, 3>& delta);

    // The knob holding GeoEditData::encode()'s text.
    KnobStringPtr getEditsKnob() const;

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum renderGeometry(const GeoRenderActionArgs& args,
                                      GeoDetailConstPtr* outputDetail,
                                      std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    KnobStringWPtr _edits;
    KnobButtonWPtr _clearEdits;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoEdit_h
