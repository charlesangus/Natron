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

#include "LensfunRuntime.h"

#include <filesystem>
#include <memory>
#include <system_error>

#include <lensfun.h>

#ifndef NATRON_LENSFUN_VERSION
#error "NATRON_LENSFUN_VERSION must name the lensfun commit Engine/CMakeLists.txt links"
#endif

static_assert(LF_VERSION >= ((0 << 24) | (3 << 16) | (95 << 8) | 0),
              "lensfun.h is older than the pinned lensfun; the image's /usr/local/include/lensfun shadows build/assets/lensfun");

NATRON_NAMESPACE_ENTER

namespace {

struct LensDatabase {
    std::unique_ptr<lfDatabase> database;
    std::string error;
};

const LensDatabase&
lensDatabase()
{
    static const LensDatabase loaded = []() {
        LensDatabase result;
        const std::string path = LensfunRuntime::databasePath();

        if (path.empty()) {
            result.error = "lensfun's lens database (Resources/lensfun) is missing from this installation";

            return result;
        }
        auto database = std::make_unique<lfDatabase>();
        const lfError status = database->Load(path.c_str());
        if (status != LF_NO_ERROR) {
            result.error = "lensfun could not load its lens database from " + path + " (lensfun error " + std::to_string(static_cast<int>(status)) + ")";

            return result;
        }
        result.database = std::move(database);

        return result;
    }();

    return loaded;
}

} // namespace

namespace LensfunRuntime {
std::string
version()
{
    return NATRON_LENSFUN_VERSION;
}

std::string
databasePath()
{
    std::error_code ec;
    const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);

    if (ec) {
        return std::string();
    }
    const std::filesystem::path dir = (exe.parent_path() / ".." / "Resources" / "lensfun").lexically_normal();
    if (!std::filesystem::is_directory(dir, ec)) {
        return std::string();
    }

    return dir.string();
}

const lfDatabase*
database(std::string* error)
{
    const LensDatabase& loaded = lensDatabase();

    if (!loaded.database && error) {
        *error = loaded.error;
    }

    return loaded.database.get();
}

std::mutex&
queryMutex()
{
    static std::mutex mutex;

    return mutex;
}
} // namespace LensfunRuntime

NATRON_NAMESPACE_EXIT
