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

#include "RawspeedRuntime.h"

#include <exception>
#include <filesystem>
#include <memory>
#include <system_error>

#include "metadata/CameraMetaData.h"

#ifndef NATRON_RAWSPEED_VERSION
#error "NATRON_RAWSPEED_VERSION must name the rawspeed commit Engine/CMakeLists.txt links"
#endif

NATRON_NAMESPACE_ENTER

namespace {

struct CameraDatabase {
    std::unique_ptr<const rawspeed::CameraMetaData> metadata;
    std::string error;
};

const CameraDatabase&
cameraDatabase()
{
    static const CameraDatabase database = []() {
        CameraDatabase loaded;
        const std::string path = RawspeedRuntime::camerasXmlPath();

        if (path.empty()) {
            loaded.error = "rawspeed's camera database (Resources/rawspeed/cameras.xml) is missing from this installation";

            return loaded;
        }
        try {
            loaded.metadata = std::make_unique<const rawspeed::CameraMetaData>(path.c_str());
        } catch (const std::exception& e) {
            loaded.error = std::string("rawspeed could not read its camera database ") + path + ": " + e.what();
        }

        return loaded;
    }();

    return database;
}

} // namespace

namespace RawspeedRuntime {
std::string
version()
{
    return NATRON_RAWSPEED_VERSION;
}

std::string
camerasXmlPath()
{
    std::error_code ec;
    const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);

    if (ec) {
        return std::string();
    }
    const std::filesystem::path xml = (exe.parent_path() / ".." / "Resources" / "rawspeed" / "cameras.xml").lexically_normal();
    if (!std::filesystem::is_regular_file(xml, ec)) {
        return std::string();
    }

    return xml.string();
}

const rawspeed::CameraMetaData*
cameraMetaData(std::string* error)
{
    const CameraDatabase& database = cameraDatabase();

    if (!database.metadata && error) {
        *error = database.error;
    }

    return database.metadata.get();
}
} // namespace RawspeedRuntime

NATRON_NAMESPACE_EXIT
