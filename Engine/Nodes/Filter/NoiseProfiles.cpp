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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "NoiseProfiles.h"

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

namespace NoiseProfiles {
namespace {
    // DT_NOISE_PROFILE_VERSION
    const int kProfilesVersion = 0;

    bool
    fail(std::string* error,
         const std::string& what)
    {
        if (error) {
            *error = what;
        }

        return false;
    }

    std::array<double, 3>
    triple(const QJsonValue& value)
    {
        const QJsonArray array = value.toArray();

        return { array.at(0).toDouble(), array.at(1).toDouble(), array.at(2).toDouble() };
    }
} // namespace

std::string
profilesJsonPath()
{
    std::error_code ec;
    const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);

    if (ec) {
        return std::string();
    }
    const std::filesystem::path json = (exe.parent_path() / ".." / "Resources" / "darktable" / "noiseprofiles.json").lexically_normal();
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
        return fail(error, "can't find file version.");
    }
    if (root.value(QStringLiteral("version")).toInt(-1) != kProfilesVersion) {
        return fail(error, "file version is not what this code understands");
    }
    if (!root.contains(QStringLiteral("noiseprofiles"))) {
        return fail(error, "can't find `noiseprofiles' entry.");
    }
    const QJsonValue makers = root.value(QStringLiteral("noiseprofiles"));
    if (!makers.isArray()) {
        return fail(error, "`noiseprofiles' is supposed to be an array");
    }

    Database loaded;
    for (const QJsonValue& makerValue : makers.toArray()) {
        const QJsonObject makerObject = makerValue.toObject();
        if (!makerObject.contains(QStringLiteral("maker"))) {
            return fail(error, "missing `maker`");
        }
        if (!makerObject.contains(QStringLiteral("models"))) {
            return fail(error, "missing `models`");
        }
        Maker maker;
        maker.maker = makerObject.value(QStringLiteral("maker")).toString().toStdString();
        for (const QJsonValue& modelValue : makerObject.value(QStringLiteral("models")).toArray()) {
            const QJsonObject modelObject = modelValue.toObject();
            if (!modelObject.contains(QStringLiteral("model"))) {
                return fail(error, "missing `model`");
            }
            if (!modelObject.contains(QStringLiteral("profiles"))) {
                return fail(error, "missing `profiles`");
            }
            Model model;
            model.model = modelObject.value(QStringLiteral("model")).toString().toStdString();
            for (const QJsonValue& profileValue : modelObject.value(QStringLiteral("profiles")).toArray()) {
                const QJsonObject profile = profileValue.toObject();
                if (!profile.contains(QStringLiteral("name"))) {
                    return fail(error, "missing `name`");
                }
                if (!profile.contains(QStringLiteral("iso"))) {
                    return fail(error, "missing `iso`");
                }
                if (!profile.contains(QStringLiteral("a"))) {
                    return fail(error, "missing `a`");
                }
                if (profile.value(QStringLiteral("a")).toArray().size() != 3) {
                    return fail(error, "`a` with size != 3");
                }
                if (!profile.contains(QStringLiteral("b"))) {
                    return fail(error, "missing `b`");
                }
                if (profile.value(QStringLiteral("b")).toArray().size() != 3) {
                    return fail(error, "`b` with size != 3");
                }
                Entry entry;
                entry.name = profile.value(QStringLiteral("name")).toString().toStdString();
                entry.iso = profile.value(QStringLiteral("iso")).toDouble();
                entry.a = triple(profile.value(QStringLiteral("a")));
                entry.b = triple(profile.value(QStringLiteral("b")));
                entry.skip = profile.value(QStringLiteral("skip")).toBool(false);
                model.profiles.push_back(std::move(entry));
            }
            maker.models.push_back(std::move(model));
        }
        loaded.push_back(std::move(maker));
    }
    *database = std::move(loaded);

    return true;
} // load

const Database&
database()
{
    static const Database profiles = []() {
        Database loaded;
        const std::string path = profilesJsonPath();
        std::string error;

        if (!path.empty()) {
            (void)load(path, &loaded, &error);
        }

        return loaded;
    }();

    return profiles;
}

Profile
generic()
{
    Profile profile;

    profile.name = "generic poissonian";
    profile.iso = 0;
    profile.a = { 0.0001f, 0.0001f, 0.0001f };
    profile.b = { 0.f, 0.f, 0.f };

    return profile;
}

std::vector<Profile>
matching(const Database& database,
         const std::string& make,
         const std::string& model)
{
    std::vector<Profile> result;

    for (const Maker& maker : database) {
        if (make.find(maker.maker) == std::string::npos) {
            continue;
        }
        for (const Model& entryModel : maker.models) {
            if (entryModel.model != model) {
                continue;
            }
            for (const Entry& entry : entryModel.profiles) {
                if (entry.skip) {
                    continue;
                }
                Profile profile;
                profile.name = entry.name;
                profile.iso = (int)entry.iso;
                for (int c = 0; c < 3; ++c) {
                    profile.a[c] = (float)entry.a[c];
                    profile.b[c] = (float)entry.b[c];
                }
                result.push_back(std::move(profile));
            }
            // darktable prepends each profile, then sorts stably by ISO.
            std::reverse(result.begin(), result.end());
            std::stable_sort(result.begin(), result.end(), [](const Profile& lhs, const Profile& rhs) {
                return lhs.iso < rhs.iso;
            });

            return result;
        }
    }

    return result;
}

Profile
interpolate(const Profile& p1,
            const Profile& p2,
            int iso)
{
    Profile out = generic();

    out.iso = iso;
    const float t = std::clamp((float)(iso - p1.iso) / (float)(p2.iso - p1.iso), 0.f, 1.f);
    for (int k = 0; k < 3; ++k) {
        out.a[k] = (1.f - t) * p1.a[k] + t * p2.a[k];
        out.b[k] = (1.f - t) * p1.b[k] + t * p2.b[k];
    }

    return out;
}

AutoProfile
autoProfile(const std::vector<Profile>& profiles,
            int iso)
{
    AutoProfile result;

    result.profile = generic();
    result.name = result.profile.name;
    const Profile* last = NULL;
    for (const Profile& current : profiles) {
        if (current.iso == iso) {
            result.profile = current;
            result.detected = true;
            result.name = "found ISO " + std::to_string(iso);
            break;
        }
        if (last && (last->iso < iso) && (current.iso > iso)) {
            result.profile = interpolate(*last, current, iso);
            result.detected = true;
            result.name = "interpolated ISO " + std::to_string(iso);
            break;
        }
        last = &current;
    }

    return result;
}
} // namespace NoiseProfiles

NATRON_NAMESPACE_EXIT
