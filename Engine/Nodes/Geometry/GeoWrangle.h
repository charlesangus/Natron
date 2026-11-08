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

#ifndef Engine_Nodes_Geometry_GeoWrangle_h
#define Engine_Nodes_Geometry_GeoWrangle_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Geo/GeoKernelBinding.h"
#include "Engine/Kernel/KernelLexer.h"
#include "Engine/Kernel/KernelTypes.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_GEOWRANGLE "GeoWrangle"

#define kGeoWrangleParamSnippet "snippet"
#define kGeoWrangleParamRunOver "runOver"
#define kGeoWrangleParamGroup "group"
#define kGeoWrangleParamCreateParameters "createParameters"

NATRON_NAMESPACE_ENTER

/** @brief A parameter a snippet reads with a ch function, in the order the snippet first names it. */
struct GeoWrangleParameterCall {
    std::string name;
    // The function that first names it: "ch", "chf", "chi", "chv", "chs" or "chramp".
    std::string function;
};

/** @brief The parameters source reads through ch functions with a literal name. */
std::vector<GeoWrangleParameterCall> geoWrangleParameterCalls(const std::string& source);

inline constexpr InputSpec kGeoWrangleInputs[] = {
    InputSpec { .label = "Source", .kind = eDataKindGeometry, .role = InputRole::Source, .optional = true },
    InputSpec { .label = "Second", .kind = eDataKindGeometry, .role = InputRole::Aux, .optional = true },
};

class GeoWrangle
    : public NativeEffectBase {
public:
    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GEOWRANGLE,
        .label = "GeoWrangle",
        .description = "Run a snippet of the geometry kernel language over every point, primitive "
                       "or vertex of the input geometry, or once over the whole geometry, to read "
                       "and write its attributes and groups and edit its topology. Second reads "
                       "a second geometry. The ch functions read the node's user parameters; "
                       "Create Parameters adds the ones the snippet names.",
        .grouping = PLUGIN_GROUP_3D,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindGeometry,
        .inputs = kGeoWrangleInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new GeoWrangle(node);
    }

    explicit GeoWrangle(NodePtr node);

    virtual ~GeoWrangle();

    /**
     * @brief Adds a user parameter for each name the snippet reads with a ch function that the
     * node lacks, as one undo step. Returns the names added. Main thread only.
     **/
    std::vector<std::string> createParameters();

private:
    struct Compiled {
        std::shared_ptr<const KernelProgram> program;
        KernelCompileError error;
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual Route route(double time,
                        ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum getPreferredMetadataAt(double time,
                                              ViewIdx view,
                                              NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum renderGeometry(const GeoRenderActionArgs& args,
                                      GeoDetailConstPtr* outputDetail,
                                      std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    GeoAttribClassEnum runOver() const;
    GeoKernelParameters readParameters(double time,
                                       ViewIdx view) const;
    std::shared_ptr<const Compiled> compile(const std::string& snippet,
                                            GeoAttribClassEnum runOver,
                                            const GeoKernelParameters& parameters) const;

    KnobStringWPtr _snippet;
    KnobChoiceWPtr _runOver;
    KnobStringWPtr _group;
    KnobButtonWPtr _createParameters;

    mutable std::mutex _compiledLock;
    mutable std::string _compiledKey;
    mutable std::shared_ptr<const Compiled> _compiled;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoWrangle_h
