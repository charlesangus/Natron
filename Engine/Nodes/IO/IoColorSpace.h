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

#ifndef Engine_Nodes_IO_ReadColorSpace_h
#define Engine_Nodes_IO_ReadColorSpace_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <utility>
#include <vector>

#include <OpenColorIO/OpenColorIO.h>
#include <OpenImageIO/imageio.h>

#include "Engine/EngineFwd.h"
#include "Engine/ProjectColorManagement.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The input colourspace of the native Read: the default it takes for a file, and the
 * conversion from it to the project's working space.
 **/
namespace ReadColorSpace {
/**
 * @brief The project file default a file of this spec takes: float for float, half and double
 * pixels, 16-bit for 16-bit integer pixels (which is how OIIO reads a 10- or 12-bit DPX), 8-bit
 * otherwise.
 **/
FileColorCategoryEnum categoryOf(const OIIO::ImageSpec& spec);

/**
 * @brief The name in \p config of the colourspace \p name designates, as a colourspace, role or
 * alias. Empty when it designates none.
 **/
std::string nameInConfig(const OCIO_NAMESPACE::ConstConfigRcPtr& config, const std::string& name);

/**
 * @brief The colourspace the file rules of \p config give the file name of \p path, by its name
 * in the config. Empty when only the default rule matches, which leaves the choice to the file
 * and the project defaults.
 **/
std::string fileRuleColorSpace(const OCIO_NAMESPACE::ConstConfigRcPtr& config, const std::string& path);

/**
 * @brief The colourspace the file's own oiio:ColorSpace tag names, by its name in \p config.
 * Empty when there is no tag or \p config has no colourspace, role or alias of that name.
 **/
std::string embeddedColorSpace(const OCIO_NAMESPACE::ConstConfigRcPtr& config, const OIIO::ImageSpec& spec);

/**
 * @brief The input colourspace a Read takes for \p path: the config's file rules, then the file's
 * own tag, then the project's file default for categoryOf(spec).
 **/
std::string defaultInputSpace(const Project& project, const std::string& path, const OIIO::ImageSpec& spec);

/**
 * @brief The processor converting \p input to \p working through the project's config, left
 * null when no conversion is needed: an empty space, equal spaces or a processor that is a no-op.
 * \p context holds the OCIO context variables to set; a pair with an empty key is ignored.
 * Returns false, filling \p error, when a conversion is needed and cannot be built.
 **/
bool toWorkingProcessor(const Project& project,
                        const std::string& input,
                        const std::string& working,
                        const std::vector<std::pair<std::string, std::string>>& context,
                        OCIO_NAMESPACE::ConstCPUProcessorRcPtr* processor,
                        std::string* error);

/**
 * @brief Applies \p processor in place to the RGB of \p width packed pixels of \p nComps (3 or 4)
 * floats each; a fourth component is left untouched. Returns false, filling \p error, on failure.
 **/
bool convertRow(const OCIO_NAMESPACE::CPUProcessor& processor, float* pixels, int width, int nComps, std::string* error);
} // namespace ReadColorSpace

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_IO_ReadColorSpace_h
