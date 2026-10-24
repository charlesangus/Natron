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

/*
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7): the colour matrix
 * tables of src/common/colormatrices.c and their lookup in src/iop/colorin.c
 * (update_profile_list()) and src/common/colorspaces.c (dt_colorspaces_create_darktable_profile(),
 * dt_colorspaces_create_vendor_profile() and dt_colorspaces_create_alternate_profile()).
 * darktable is distributed under the GNU GPL, version 3 or (at your option) any later version,
 * and so is this port: GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2010-2021 darktable developers.
 *
 *     darktable is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     darktable is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef Engine_Nodes_Raw_RawColorMatrices_h
#define Engine_Nodes_Raw_RawColorMatrices_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <optional>
#include <string>

#include "Engine/Nodes/Raw/RawColorMath.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief darktable's own camera colour matrices: the enhanced ("darktable profiled"), vendor and
 * alternate tables, each entry the XYZ of a profile's red, green and blue primaries and of its
 * white, scaled by 1e6, keyed by darktable's camera_makermodel.
 **/
namespace RawColorMatrices {
struct Entry {
    const char* makermodel;
    int rXYZ[3], gXYZ[3], bXYZ[3], white[3];
};

enum class Table {
    Enhanced,
    Vendor,
    Alternate
};

/**
 * @brief darktable's camera_makermodel: the canonical make and model joined by a space.
 **/
std::string makerModel(const std::string& make,
                       const std::string& model);

/**
 * @brief The first entry of table for makermodel, nullptr when it has none. The enhanced table
 * matches regardless of case, the vendor and alternate ones exactly, as darktable does.
 **/
const Entry* find(Table table,
                  const std::string& makermodel);

/**
 * @brief The camera RGB to XYZ (D65) matrix of entry's profile
 * (RawColorMath::profileCameraToXyz()), nullopt when its primaries are degenerate.
 **/
std::optional<RawColorMath::Mat3> cameraToXyz(const Entry& entry);
} // namespace RawColorMatrices

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Raw_RawColorMatrices_h
