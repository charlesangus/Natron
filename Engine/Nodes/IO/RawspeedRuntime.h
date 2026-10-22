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

#ifndef Engine_Nodes_IO_RawspeedRuntime_h
#define Engine_Nodes_IO_RawspeedRuntime_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

namespace rawspeed {
class CameraMetaData;
}

NATRON_NAMESPACE_ENTER

/**
 * @brief What the raw decoder needs from the installation: which rawspeed it is, and its camera
 * database, rawspeed's cameras.xml.
 **/
namespace RawspeedRuntime {
/**
 * @brief The rawspeed commit this build links, for error messages naming the decoder version.
 **/
std::string version();

/**
 * @brief <directory of the running executable>/../Resources/rawspeed/cameras.xml, where the build
 * tree and the bundle both put it, or an empty string when no file is there.
 **/
std::string camerasXmlPath();

/**
 * @brief The camera database, parsed once per process on first use and shared by every thread.
 * Null when it could not be found or parsed, with the reason in *error when error is not null.
 **/
const rawspeed::CameraMetaData* cameraMetaData(std::string* error = nullptr);
} // namespace RawspeedRuntime

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_IO_RawspeedRuntime_h
