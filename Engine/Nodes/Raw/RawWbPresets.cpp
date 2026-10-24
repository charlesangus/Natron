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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "RawWbPresets.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QString>

NATRON_NAMESPACE_ENTER

namespace RawWbPresets {
namespace {
    // DT_WB_PRESETS_VERSION: the newest file layout this reader understands.
    const int kPresetsVersion = 1;

    // darktable lists at most this many settings for a camera.
    const int kMaxCameraPresets = 50;

    bool
    sameCamera(const Entry& entry,
               const std::string& make,
               const std::string& model)
    {
        return (entry.make == make) && (entry.model == model);
    }

    std::array<double, 3>
    firstThree(const std::array<double, 4>& channels)
    {
        return { channels[0], channels[1], channels[2] };
    }

    bool
    fail(std::string* error,
         const std::string& reason)
    {
        if (error) {
            *error = reason;
        }

        return false;
    }
} // namespace

std::string
presetsJsonPath()
{
    std::error_code ec;
    const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);

    if (ec) {
        return std::string();
    }
    const std::filesystem::path json = (exe.parent_path() / ".." / "Resources" / "darktable" / "wb_presets.json").lexically_normal();
    if (!std::filesystem::is_regular_file(json, ec)) {
        return std::string();
    }

    return json.string();
}

bool
load(const std::string& path,
     Database* database,
     std::string* error)
{
    database->clear();
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(error, "cannot open " + path);
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (document.isNull()) {
        return fail(error, "parsing json from " + path + " failed: " + parseError.errorString().toStdString());
    }
    const QJsonObject root = document.object();
    if (!root.contains(QStringLiteral("version"))) {
        return fail(error, "can't find file version");
    }
    if (root.value(QStringLiteral("version")).toInt() > kPresetsVersion) {
        return fail(error, "file version is not what this code understands");
    }
    const QJsonValue makers = root.value(QStringLiteral("wb_presets"));
    if (!makers.isArray()) {
        return fail(error, "`wb_presets' is supposed to be an array");
    }

    Database rows;
    for (const QJsonValue& makerValue : makers.toArray()) {
        const QJsonObject maker = makerValue.toObject();
        if (!maker.contains(QStringLiteral("maker"))) {
            return fail(error, "missing `maker`");
        }
        if (!maker.contains(QStringLiteral("models"))) {
            return fail(error, "missing `models`");
        }
        const std::string make = maker.value(QStringLiteral("maker")).toString().toStdString();
        for (const QJsonValue& modelValue : maker.value(QStringLiteral("models")).toArray()) {
            const QJsonObject model = modelValue.toObject();
            if (!model.contains(QStringLiteral("model"))) {
                return fail(error, "missing `model`");
            }
            if (!model.contains(QStringLiteral("presets"))) {
                return fail(error, "missing `presets`");
            }
            const std::string modelName = model.value(QStringLiteral("model")).toString().toStdString();
            for (const QJsonValue& presetValue : model.value(QStringLiteral("presets")).toArray()) {
                const QJsonObject preset = presetValue.toObject();
                Entry entry;
                entry.make = make;
                entry.model = modelName;
                // darktable compares and shows the names lower-cased (g_utf8_strdown()).
                entry.name = preset.value(QStringLiteral("name")).toString().toLower().toStdString();
                entry.tuning = preset.value(QStringLiteral("tuning")).toInt(0);
                const QJsonArray channels = preset.value(QStringLiteral("channels")).toArray();
                for (int c = 0; c < 4; ++c) {
                    entry.channels[c] = (c < channels.size()) ? channels.at(c).toDouble(0.) : 0.;
                }
                rows.push_back(std::move(entry));
            }
        }
    }
    *database = std::move(rows);

    return true;
} // load

const Database&
database()
{
    static const Database presets = []() {
        Database loaded;
        const std::string path = presetsJsonPath();
        std::string error;

        if (!path.empty()) {
            (void)load(path, &loaded, &error);
        }

        return loaded;
    }();

    return presets;
}

std::vector<CameraPreset>
cameraPresets(const Database& database,
              const std::string& make,
              const std::string& model)
{
    std::vector<CameraPreset> found;
    const std::string* wbName = nullptr;
    const int count = (int)database.size();

    for (int i = 0; i < count; ++i) {
        if ((int)found.size() >= kMaxCameraPresets) {
            break;
        }
        const Entry& wbp = database[i];
        if (!sameCamera(wbp, make, model)) {
            continue;
        }
        if (wbName && (*wbName == wbp.name)) {
            continue;
        }
        wbName = &wbp.name;
        CameraPreset preset;
        preset.name = wbp.name;
        preset.noTuningPos = i;
        preset.minTuningPos = i;
        preset.maxTuningPos = i;
        if (wbp.tuning != 0) {
            // The lowest tuning comes first. darktable's scan stops at the first other name
            // only; this one also stops at the camera's end and the database's.
            int ftPos = i + 1;
            int lastFt = wbp.tuning;
            while ((ftPos < count) && sameCamera(database[ftPos], make, model) && (database[ftPos].name == *wbName)) {
                if (database[ftPos].tuning == 0) {
                    preset.noTuningPos = ftPos;
                }
                if (database[ftPos].tuning > lastFt) {
                    preset.maxTuningPos = ftPos;
                    lastFt = database[ftPos].tuning;
                }
                ++ftPos;
            }
        }
        found.push_back(std::move(preset));
    }

    return found;
} // cameraPresets

std::array<double, 3>
interpolate(const Entry& p1,
            const Entry& p2,
            int tuning)
{
    const double t = std::clamp((double)(tuning - p1.tuning) / (double)(p2.tuning - p1.tuning), 0., 1.);
    std::array<double, 3> out;

    for (int k = 0; k < 3; ++k) {
        out[k] = 1. / (((1. - t) / p1.channels[k]) + (t / p2.channels[k]));
    }

    return out;
}

std::optional<std::array<double, 3>>
presetMultipliers(const Database& database,
                  const std::string& make,
                  const std::string& model,
                  const std::string& name,
                  int tune)
{
    for (const CameraPreset& preset : cameraPresets(database, make, model)) {
        if (preset.name != name) {
            continue;
        }
        // darktable's slider cannot leave the setting's tunings; a saved value can.
        tune = std::clamp(tune, database[preset.minTuningPos].tuning, database[preset.maxTuningPos].tuning);
        const auto sameSetting = [&](int i) {
            return sameCamera(database[i], make, model) && (database[i].name == name);
        };
        for (int i = preset.minTuningPos; (i <= preset.maxTuningPos) && sameSetting(i); ++i) {
            if (database[i].tuning == tune) {
                return firstThree(database[i].channels);
            }
        }
        // The two closest tunings around tune.
        for (int i = preset.minTuningPos + 1; (i <= preset.maxTuningPos) && sameSetting(i); ++i) {
            if ((database[i - 1].tuning < tune) && (database[i].tuning > tune)) {
                return interpolate(database[i - 1], database[i], tune);
            }
        }

        // Where darktable keeps the multipliers it had, the setting without tuning.
        return firstThree(database[preset.noTuningPos].channels);
    }

    return std::nullopt;
} // presetMultipliers

std::optional<std::array<double, 3>>
firstPreset(const Database& database,
            const std::string& make,
            const std::string& model)
{
    for (const Entry& wbp : database) {
        if (sameCamera(wbp, make, model)) {
            return firstThree(wbp.channels);
        }
    }

    return std::nullopt;
}

std::optional<std::array<double, 3>>
daylightPreset(const Database& database,
               const std::string& make,
               const std::string& model)
{
    for (const Entry& wbp : database) {
        // darktable compares with "Daylight" and "DirectSunlight", which the lower-cased names
        // never equal; these are the names it means.
        if (sameCamera(wbp, make, model) && ((wbp.name == "daylight") || (wbp.name == "direct sunlight")) && (wbp.tuning == 0)) {
            return firstThree(wbp.channels);
        }
    }

    return std::nullopt;
}
} // namespace RawWbPresets

NATRON_NAMESPACE_EXIT
