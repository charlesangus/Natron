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
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7),
 * src/common/noiseprofiles.c and dt_iop_denoiseprofile_get_auto_profile() of
 * src/iop/denoiseprofile.c. darktable is distributed under the GNU GPL, version 3 or (at your
 * option) any later version, and so is this port: GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2012-2026 darktable developers.
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

#ifndef Engine_Nodes_Filter_NoiseProfiles_h
#define Engine_Nodes_Filter_NoiseProfiles_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

namespace NoiseProfiles {
/**
 * @brief darktable's dt_noiseprofile_t: the Poissonian (a) and Gaussian (b) noise fit of one
 * camera at one ISO, per colour channel, in darktable's single precision.
 **/
struct Profile {
    std::string name;
    int iso = 0;
    std::array<float, 3> a = { 0.f, 0.f, 0.f };
    std::array<float, 3> b = { 0.f, 0.f, 0.f };
};

/**
 * @brief One profile as the file has it. skip marks the entries darktable leaves out.
 **/
struct Entry {
    std::string name;
    double iso = 0.;
    std::array<double, 3> a = { 0., 0., 0. };
    std::array<double, 3> b = { 0., 0., 0. };
    bool skip = false;
};

struct Model {
    std::string model;
    std::vector<Entry> profiles;
};

struct Maker {
    std::string maker;
    std::vector<Model> models;
};

/**
 * @brief The makers in file order.
 **/
using Database = std::vector<Maker>;

/**
 * @brief <directory of the running executable>/../Resources/darktable/noiseprofiles.json, where
 * the build tree and the bundle both put it, or an empty string when no file is there.
 **/
std::string profilesJsonPath();

/**
 * @brief dt_noiseprofile_init(): reads and verifies the darktable noise profile file at path into
 * *database. False, with the reason in *error, when the file cannot be read, is not JSON, has
 * another version, or lacks a member darktable requires.
 **/
bool load(const std::string& path,
          Database* database,
          std::string* error);

/**
 * @brief The database at profilesJsonPath(), read once per process on first use and shared by
 * every thread; empty when it could not be read.
 **/
const Database& database();

/**
 * @brief dt_noiseprofile_generic: "generic poissonian", a = 1e-4, b = 0.
 **/
Profile generic();

/**
 * @brief dt_noiseprofile_get_matching(): the profiles of the first model called exactly model
 * under a maker whose name make contains, skipped entries left out, sorted by ISO. Entries of
 * equal ISO come in the reverse of file order, as darktable's prepend-then-stable-sort leaves
 * them.
 **/
std::vector<Profile> matching(const Database& database,
                              const std::string& make,
                              const std::string& model);

/**
 * @brief dt_noiseprofile_interpolate(): p1 (the smaller ISO) and p2 blended linearly in ISO,
 * the weight clamped to [0, 1]. The result is named after neither.
 **/
Profile interpolate(const Profile& p1,
                    const Profile& p2,
                    int iso);

struct AutoProfile {
    Profile profile;
    bool detected = false;
    // "found ISO n", "interpolated ISO n", or the generic profile's name.
    std::string name;
};

/**
 * @brief dt_iop_denoiseprofile_get_auto_profile() without the highlight preservation shift: the
 * profile at exactly iso, else the interpolation of the two profiles around it, else the generic
 * profile. An ISO below the lowest or above the highest profile gets the generic profile, as in
 * darktable.
 **/
AutoProfile autoProfile(const std::vector<Profile>& profiles,
                        int iso);
} // namespace NoiseProfiles

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_NoiseProfiles_h
