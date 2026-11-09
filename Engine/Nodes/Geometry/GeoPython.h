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

#ifndef Engine_Nodes_Geometry_GeoPython_h
#define Engine_Nodes_Geometry_GeoPython_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_GEOPYTHON "GeoPython"

#define kGeoPythonParamScript "script"

NATRON_NAMESPACE_ENTER

enum GeoPythonRunResultEnum {
    eGeoPythonRunOK = 0,
    eGeoPythonRunFailed,
    eGeoPythonRunAborted,
};

/**
 * @brief Edits geometry with a Python script.
 *
 * The script runs in the embedded interpreter on the render thread, holding the GIL for its
 * whole run, with `geo` (GeoPySession) and `frame` defined. Its edits reach the output only if
 * it finishes: an exception, a value that does not fit its attribute or an aborted render
 * leaves nothing behind, and the first two report the traceback as the node's error.
 **/
class GeoPython
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindGeometry, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GEOPYTHON,
        .label = "GeoPython",
        .description = "Edit geometry with a Python script. The script reads and writes attributes through geo "
                       "(geo.points[\"P\"], geo.prims[...], geo.vertices[...], geo.detail[...]), adds and removes "
                       "points and primitives, and can read the current frame as frame.",
        .grouping = PLUGIN_GROUP_3D,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindGeometry,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new GeoPython(node);
    }

    explicit GeoPython(NodePtr node);
    virtual ~GeoPython();

    /**
     * @brief Runs @p script over @p builder. Takes the GIL itself, so the caller must not hold
     * it on another thread. @p aborted is polled while the script runs and stops it with
     * eGeoPythonRunAborted. On eGeoPythonRunFailed @p error holds the traceback, with line
     * numbers in @p sourceName; on anything but eGeoPythonRunOK the builder must be discarded.
     **/
    static GeoPythonRunResultEnum runScript(GeoDetailBuilder& builder,
                                            const std::string& script,
                                            const std::string& sourceName,
                                            double frame,
                                            const std::function<bool()>& aborted,
                                            std::string* error);

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual StatusEnum getPreferredMetadataAt(double time,
                                              ViewIdx view,
                                              NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum renderGeometry(const GeoRenderActionArgs& args,
                                      GeoDetailConstPtr* outputDetail,
                                      std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    KnobStringWPtr _script;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoPython_h
