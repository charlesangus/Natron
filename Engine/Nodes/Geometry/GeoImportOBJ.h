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

#ifndef Engine_Nodes_Geometry_GeoImportOBJ_h
#define Engine_Nodes_Geometry_GeoImportOBJ_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_GEOIMPORTOBJ "GeoImportOBJ"

#define kGeoImportOBJParamFile "file"

NATRON_NAMESPACE_ENTER

/**
 * @brief Reads a Wavefront OBJ file into geometry; see ObjParser for what is read. A file that
 * cannot be opened or parsed is a node error; an empty file name gives empty geometry.
 **/
class GeoImportOBJ
    : public NativeEffectBase {
public:
    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GEOIMPORTOBJ,
        .label = "GeoImportOBJ",
        .description = "Read geometry from a Wavefront OBJ file: its points, polygons and polylines, "
                       "with texture coordinates (uv) and normals (N) on the vertices, its groups "
                       "as primitive groups and its object names in the primitive attribute name.",
        .grouping = PLUGIN_GROUP_3D,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindGeometry,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new GeoImportOBJ(node);
    }

    explicit GeoImportOBJ(NodePtr node);
    virtual ~GeoImportOBJ();

    /** @brief Empty geometry for an empty @p path; false with @p error set when the file cannot be read. */
    static bool importFile(const std::string& path,
                           GeoDetailBuilder& builder,
                           std::string* error);

    /** @brief True when @p pattern names one file per frame (a # or %0Nd frame number). */
    static bool isFrameSequencePattern(const std::string& pattern);

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
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoImportOBJ_h
