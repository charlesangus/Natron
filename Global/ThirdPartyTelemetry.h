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

#ifndef NATRON_GLOBAL_THIRDPARTYTELEMETRY_H
#define NATRON_GLOBAL_THIRDPARTYTELEMETRY_H

#include "Global/Macros.h"

#include <cstdlib>

NATRON_NAMESPACE_ENTER

/**
 * @brief Opts out of telemetry in the third-party libraries Natron loads. Call it first in main(),
 * before any thread exists, because setenv is not safe against concurrent getenv.
 *
 * ONNX Runtime starts its upload thread when its telemetry object is constructed, and only the
 * ORT_DISABLE_TELEMETRY environment variable, read then, stops it; the API call that disables
 * events comes too late. A value the user already set is kept.
 **/
inline void
disableThirdPartyTelemetry()
{
    setenv("ORT_DISABLE_TELEMETRY", "1", 0);
}

NATRON_NAMESPACE_EXIT

#endif // NATRON_GLOBAL_THIRDPARTYTELEMETRY_H
