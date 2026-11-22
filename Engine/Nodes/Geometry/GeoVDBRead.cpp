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

#include "GeoVDBRead.h"

#include <cmath>
#include <exception>
#include <string>
#include <vector>

#include "Engine/Geo/GeoOpenVDB.h"

#include <SequenceParsing.h>

#include "Engine/AppInstance.h"
#include "Engine/Geo/GeoVDB.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Geometry/GeoImportOBJ.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_ENTER

bool
GeoVDBRead::readFile(const std::string& path,
                     std::string_view grids,
                     GeoDetailBuilder& builder,
                     std::string* error)
{
    if (path.empty()) {
        return true;
    }
    std::vector<GeoVDBGridBaseConstPtr> read;
    if (!geoReadVDBFile(path, grids, &read, error)) {
        return false;
    }
    for (const GeoVDBGridBaseConstPtr& grid : read) {
        geoAppendVDB(builder, grid, grid->getName());
    }

    return true;
}

GeoVDBRead::GeoVDBRead(NodePtr node)
    : NativeEffectBase(node)
{
}

GeoVDBRead::~GeoVDBRead()
{
}

const NodeSpec*
GeoVDBRead::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
GeoVDBRead::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    KnobFilePtr file = createKnob<KnobFile>(tr("File").toStdString());
    file->setName(kGeoVDBReadParamFile);
    file->setHintToolTip(tr("The OpenVDB file to read, or a sequence of them with a # or %04d frame number. "
                            "A file that cannot be read is an error; an empty name gives empty geometry.")
                             .toStdString());
    file->setFileExtensions({ "vdb" });
    file->setAnimationEnabled(false);
    page->addKnob(file);
    _file = file;

    KnobStringPtr grids = createKnob<KnobString>(tr("Grids").toStdString());
    grids->setName(kGeoVDBReadParamGrids);
    grids->setHintToolTip(tr("The grids to read, by name: space-separated globs, ^ in front of one to leave "
                             "out what it matches. * reads every grid.")
                              .toStdString());
    grids->setDefaultValue("*");
    grids->setAnimationEnabled(false);
    page->addKnob(grids);
    _grids = grids;
}

StatusEnum
GeoVDBRead::getPreferredMetadataAt(double /*time*/,
                                   ViewIdx /*view*/,
                                   NodeMetadata& metadata)
{
    // The geometry cache keys on time only for frame-varying nodes.
    const KnobFilePtr file = _file.lock();
    metadata.setIsFrameVarying(file && GeoImportOBJ::isFrameSequencePattern(file->getValue()));

    return eStatusOK;
}

StatusEnum
GeoVDBRead::renderGeometry(const GeoRenderActionArgs& args,
                           GeoDetailConstPtr* outputDetail,
                           std::string* errorMessage)
{
    std::string path;
    if (const KnobFilePtr file = _file.lock()) {
        const int frame = (int)std::floor(args.time + 0.5);
        path = file->getFileName(frame, args.view);
        // getFileName substitutes frame numbers only for image-reader knobs, which this is not.
        if (GeoImportOBJ::isFrameSequencePattern(path)) {
            std::vector<std::string> views;
            if (getApp()) {
                views = getApp()->getProject()->getProjectViewNames();
            }
            path = SequenceParsing::generateFileNameFromPattern(path, views, frame, args.view);
        }
    }
    std::string grids = "*";
    if (const KnobStringPtr knob = _grids.lock()) {
        grids = knob->getValue();
    }

    return renderGeometryGenerator([&path, &grids](GeoDetailBuilder& detail, std::string* error) {
        try {
            return readFile(path, grids, detail, error) ? eStatusOK : eStatusFailed;
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
