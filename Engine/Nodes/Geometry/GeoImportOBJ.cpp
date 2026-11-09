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

#include "GeoImportOBJ.h"

#include <cmath>
#include <exception>
#include <string>

#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Geometry/ObjParser.h"

NATRON_NAMESPACE_ENTER

bool
GeoImportOBJ::importFile(const std::string& path,
                         GeoDetailBuilder& builder,
                         std::string* error)
{
    if (path.empty()) {
        return true;
    }

    return ObjParser::parseFile(path, builder, error);
}

bool
GeoImportOBJ::isFrameSequencePattern(const std::string& pattern)
{
    if (pattern.find('#') != std::string::npos) {
        return true;
    }
    for (std::size_t i = pattern.find('%'); i != std::string::npos; i = pattern.find('%', i + 1)) {
        std::size_t j = i + 1;
        while (j < pattern.size() && pattern[j] >= '0' && pattern[j] <= '9') {
            ++j;
        }
        if (j < pattern.size() && pattern[j] == 'd') {
            return true;
        }
    }

    return false;
}

GeoImportOBJ::GeoImportOBJ(NodePtr node)
    : NativeEffectBase(node)
{
}

GeoImportOBJ::~GeoImportOBJ()
{
}

const NodeSpec*
GeoImportOBJ::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
GeoImportOBJ::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    KnobFilePtr file = createKnob<KnobFile>(tr("File").toStdString());
    file->setName(kGeoImportOBJParamFile);
    file->setHintToolTip(tr("The Wavefront OBJ file to read. A file that cannot be read is an error; an empty "
                            "name gives empty geometry.")
                             .toStdString());
    file->setFileExtensions({ "obj" });
    file->setAnimationEnabled(false);
    page->addKnob(file);
    _file = file;
}

StatusEnum
GeoImportOBJ::getPreferredMetadataAt(double /*time*/,
                                     ViewIdx /*view*/,
                                     NodeMetadata& metadata)
{
    // The geometry cache keys on time only for frame-varying nodes; without this every frame of a
    // sequence would be served the first frame's mesh.
    const KnobFilePtr file = _file.lock();
    metadata.setIsFrameVarying(file && isFrameSequencePattern(file->getValue()));

    return eStatusOK;
}

StatusEnum
GeoImportOBJ::renderGeometry(const GeoRenderActionArgs& args,
                             GeoDetailConstPtr* outputDetail,
                             std::string* errorMessage)
{
    std::string path;
    if (const KnobFilePtr file = _file.lock()) {
        path = file->getFileName((int)std::floor(args.time + 0.5), args.view);
    }

    return renderGeometryGenerator([&path](GeoDetailBuilder& detail, std::string* error) {
        try {
            return importFile(path, detail, error) ? eStatusOK : eStatusFailed;
        } catch (const std::exception& e) {
            if (error) {
                *error = e.what();
            }

            return eStatusFailed;
        }
    },
                                   outputDetail, errorMessage);
}

NATRON_NAMESPACE_EXIT
