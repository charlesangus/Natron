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

#ifndef Engine_Nodes_Geometry_GeoVDBRead_h
#define Engine_Nodes_Geometry_GeoVDBRead_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <string_view>

#include "Engine/EngineFwd.h"
#include "Engine/Geo/GeoVDB.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_GEOVDBREAD "GeoVDBRead"

#define kGeoVDBReadParamFile "file"
#define kGeoVDBReadParamGrids "grids"

NATRON_NAMESPACE_ENTER

/**
 * @brief Reads the grids of an OpenVDB file into VDB primitives, one per grid, named after the
 * grid. A file that cannot be read is a node error; an empty file name gives empty geometry.
 **/
class GeoVDBRead
    : public NativeEffectBase {
public:
    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GEOVDBREAD,
        .label = "GeoVDBRead",
        .description = "Read volumes from an OpenVDB (.vdb) file or a sequence of them: each selected grid "
                       "becomes one VDB primitive on one point at the grid's origin, its name in the primitive "
                       "attribute name.",
        .grouping = PLUGIN_GROUP_GEOMETRY,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindGeometry,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new GeoVDBRead(node);
    }

    explicit GeoVDBRead(NodePtr node);
    virtual ~GeoVDBRead();

    /**
     * @brief Appends the grids of @p path whose names match @p grids (space-separated globs, ^ to
     * exclude) to @p builder. Empty geometry for an empty @p path; false with @p error set when the
     * file cannot be read, or once @p aborted says so, which is asked before each grid is read.
     **/
    static bool readFile(const std::string& path,
                         std::string_view grids,
                         GeoDetailBuilder& builder,
                         std::string* error,
                         const GeoVDBAbortFunc& aborted = GeoVDBAbortFunc());

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual StatusEnum getPreferredMetadataAt(double time,
                                              ViewIdx view,
                                              NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum renderGeometry(const GeoRenderActionArgs& args,
                                      GeoDetailConstPtr* outputDetail,
                                      std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    KnobFileWPtr _file;
    KnobStringWPtr _grids;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoVDBRead_h
