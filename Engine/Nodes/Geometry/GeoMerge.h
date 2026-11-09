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

#ifndef Engine_Nodes_Geometry_GeoMerge_h
#define Engine_Nodes_Geometry_GeoMerge_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <span>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_GEOMERGE "GeoMerge"

#define kGeoMergeInputCount 64

NATRON_NAMESPACE_ENTER

/**
 * @brief Combines geometry from any number of inputs.
 **/
class GeoMerge
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "", .kind = eDataKindGeometry, .role = InputRole::Variadic, .optional = true, .count = kGeoMergeInputCount, .firstNumber = 0 },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GEOMERGE,
        .label = "GeoMerge",
        .description = "Combine the geometry of every connected input into one, in input order: "
                       "points, vertices and primitives keep their order within each input and "
                       "follow those of the inputs before. An attribute some inputs lack takes "
                       "its default on their elements. Attributes of one name but different "
                       "types are widened to a type holding both (integer to float, a longer "
                       "tuple); string against number keeps the first input's and warns. Groups "
                       "of one name are combined.",
        .grouping = PLUGIN_GROUP_3D,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindGeometry,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new GeoMerge(node);
    }

    explicit GeoMerge(NodePtr node);
    virtual ~GeoMerge();

    /**
     * @brief The inputs appended in order; null entries are skipped. The result shares the first
     * input's pages wherever its attributes need no widening. Attributes that cannot be widened
     * to one type keep the first description met and add a line to @p warnings.
     **/
    static GeoDetail merge(std::span<const GeoDetailConstPtr> inputs,
                           std::vector<std::string>* warnings);

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual StatusEnum renderGeometry(const GeoRenderActionArgs& args,
                                      GeoDetailConstPtr* outputDetail,
                                      std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoMerge_h
