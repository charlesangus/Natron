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
 * src/common/wb_presets.c, and the preset handling of src/iop/temperature.c
 * (_generate_preset_combo(), _preset_tune_callback(), _find_coeffs() and reload_defaults()).
 * darktable is distributed under the GNU GPL, version 3 or (at your option) any later version,
 * and so is this port: GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2013-2024 darktable developers.
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
 *
 *     UFRaw - Unidentified Flying Raw converter for digital camera images
 *     wb_presets.c - White balance preset values for various cameras
 *     Copyright 2004-2013 by Udi Fuchs
 *
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */

#ifndef Engine_Nodes_Raw_RawWbPresets_h
#define Engine_Nodes_Raw_RawWbPresets_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

namespace RawWbPresets {
/**
 * @brief darktable's dt_wb_data: one row of the preset database. name is lower-cased on load,
 * as darktable does; tuning is 0 when the file gives none.
 **/
struct Entry {
    std::string make;
    std::string model;
    std::string name;
    int tuning = 0;
    std::array<double, 4> channels = { 0., 0., 0., 0. };
};

/**
 * @brief The rows in file order, which keeps each preset's tunings together, lowest first.
 **/
using Database = std::vector<Entry>;

/**
 * @brief One white balance setting of a camera, with the positions in the database of its row
 * without tuning and of its lowest and highest tunings: darktable's
 * dt_iop_temperature_preset_data_t. The three are equal for a preset without tunings.
 **/
struct CameraPreset {
    std::string name;
    int noTuningPos = 0;
    int minTuningPos = 0;
    int maxTuningPos = 0;
};

/**
 * @brief <directory of the running executable>/../Resources/darktable/wb_presets.json, where the
 * build tree and the bundle both put it, or an empty string when no file is there.
 **/
std::string presetsJsonPath();

/**
 * @brief dt_wb_presets_init(): reads the darktable presets file at path into *database. False,
 * with the reason in *error, when the file cannot be read, is not JSON, or is a newer version.
 **/
bool load(const std::string& path,
          Database* database,
          std::string* error);

/**
 * @brief The database at presetsJsonPath(), read once per process on first use and shared by
 * every thread; empty when it could not be read.
 **/
const Database& database();

/**
 * @brief _generate_preset_combo(): the camera's settings, in file order, at most 50. Matching is
 * on darktable's exact spelling of make and model, rawspeed's canonical names (raw/make,
 * raw/model).
 **/
std::vector<CameraPreset> cameraPresets(const Database& database,
                                        const std::string& make,
                                        const std::string& model);

/**
 * @brief dt_wb_preset_interpolate(): the channels at tuning between p1 (the lower tuning) and p2,
 * interpolated linearly in the reciprocals of the multipliers, which is linear in mired.
 **/
std::array<double, 3> interpolate(const Entry& p1,
                                  const Entry& p2,
                                  int tuning);

/**
 * @brief _preset_tune_callback(): the multipliers of the camera's setting called name at tune,
 * the row with that tuning or the interpolation of the two rows around it. tune is first clamped
 * to the setting's tunings. Nothing when the camera has no such setting.
 **/
std::optional<std::array<double, 3>> presetMultipliers(const Database& database,
                                                       const std::string& make,
                                                       const std::string& model,
                                                       const std::string& name,
                                                       int tune);

/**
 * @brief The first row the database has for the camera, which darktable's _find_coeffs() falls
 * back on when the file has no white balance and no colour matrix.
 **/
std::optional<std::array<double, 3>> firstPreset(const Database& database,
                                                 const std::string& make,
                                                 const std::string& model);

/**
 * @brief The camera's "daylight" (or "direct sunlight") row without tuning, which darktable's
 * reload_defaults() takes for the D65 multipliers when there is no colour matrix.
 **/
std::optional<std::array<double, 3>> daylightPreset(const Database& database,
                                                    const std::string& make,
                                                    const std::string& model);
} // namespace RawWbPresets

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Raw_RawWbPresets_h
