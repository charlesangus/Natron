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

#include "IoColorSpace.h"

#include <exception>

#include <QString>

#include "Global/FloatingPointExceptions.h"

#include "Engine/Project.h"

NATRON_NAMESPACE_ENTER

namespace IoColorSpace {
std::string
nameInConfig(const OCIO_NAMESPACE::ConstConfigRcPtr& config,
             const std::string& name)
{
    if (!config || name.empty()) {
        return std::string();
    }
    try {
        OCIO_NAMESPACE::ConstColorSpaceRcPtr colorSpace = config->getColorSpace(name.c_str());

        return (colorSpace && colorSpace->getName()) ? std::string(colorSpace->getName()) : std::string();
    } catch (const std::exception&) {
        return std::string();
    }
}

FileColorCategoryEnum
categoryOf(const OIIO::ImageSpec& spec)
{
    switch (spec.format.basetype) {
    case OIIO::TypeDesc::HALF:
    case OIIO::TypeDesc::FLOAT:
    case OIIO::TypeDesc::DOUBLE:
        return eFileColorCategoryFloat;
    case OIIO::TypeDesc::UINT16:
    case OIIO::TypeDesc::INT16:
        return eFileColorCategory16Bit;
    default:
        return eFileColorCategory8Bit;
    }
}

std::string
fileRuleColorSpace(const OCIO_NAMESPACE::ConstConfigRcPtr& config,
                   const std::string& path)
{
    const std::size_t slash = path.find_last_of("/\\");
    const std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);

    if (!config || name.empty()) {
        return std::string();
    }
    try {
        // The default rule matches every path, so it stands for "no rule" here and leaves the
        // choice to the file and the project defaults.
        if (config->filepathOnlyMatchesDefaultRule(name.c_str())) {
            return std::string();
        }
        const char* const ruleSpace = config->getColorSpaceFromFilepath(name.c_str());

        return nameInConfig(config, ruleSpace ? std::string(ruleSpace) : std::string());
    } catch (const std::exception&) {
        return std::string();
    }
}

std::string
embeddedColorSpace(const OCIO_NAMESPACE::ConstConfigRcPtr& config,
                   const OIIO::ImageSpec& spec)
{
    const OIIO::ParamValue* value = spec.find_attribute("oiio:ColorSpace", OIIO::TypeDesc::STRING);

    if (!value || !value->data()) {
        return std::string();
    }
    const char* const tag = *(const char* const*)value->data();

    if (!tag || !config) {
        return std::string();
    }
    const std::string named = nameInConfig(config, tag);
    if (!named.empty()) {
        return named;
    }
    // OIIO records a file's colourspace as its Color Interop ID, which a config need not also
    // list among the space's aliases.
    try {
        const int count = config->getNumColorSpaces(OCIO_NAMESPACE::SEARCH_REFERENCE_SPACE_ALL, OCIO_NAMESPACE::COLORSPACE_ALL);
        for (int i = 0; i < count; ++i) {
            const char* const name = config->getColorSpaceNameByIndex(OCIO_NAMESPACE::SEARCH_REFERENCE_SPACE_ALL, OCIO_NAMESPACE::COLORSPACE_ALL, i);
            OCIO_NAMESPACE::ConstColorSpaceRcPtr colorSpace = name ? config->getColorSpace(name) : OCIO_NAMESPACE::ConstColorSpaceRcPtr();
            const char* const interop = colorSpace ? colorSpace->getInteropID() : NULL;
            if (interop && (std::string(interop) == tag)) {
                return std::string(name);
            }
        }
    } catch (const std::exception&) {
    }

    return std::string();
}

std::string
fileTag(const Project& project,
        const std::string& space)
{
    const ProjectColorManagementPtr colorManagement = project.getColorManagement();
    const OCIO_NAMESPACE::ConstConfigRcPtr config = colorManagement ? colorManagement->getConfig() : OCIO_NAMESPACE::ConstConfigRcPtr();

    // OIIO turns the tag into an interop ID through its own config, which knows nothing of a
    // custom config's names and drops a tag it cannot map.
    try {
        OCIO_NAMESPACE::ConstColorSpaceRcPtr colorSpace = (config && !space.empty()) ? config->getColorSpace(space.c_str()) : OCIO_NAMESPACE::ConstColorSpaceRcPtr();
        const char* const interop = colorSpace ? colorSpace->getInteropID() : NULL;
        if (interop && *interop) {
            return std::string(interop);
        }
    } catch (const std::exception&) {
    }

    return space;
}

std::string
defaultInputSpace(const Project& project,
                  const std::string& path,
                  const OIIO::ImageSpec& spec)
{
    const ProjectColorManagementPtr colorManagement = project.getColorManagement();
    const OCIO_NAMESPACE::ConstConfigRcPtr config = colorManagement ? colorManagement->getConfig() : OCIO_NAMESPACE::ConstConfigRcPtr();
    std::string space = fileRuleColorSpace(config, path);

    if (space.empty()) {
        space = embeddedColorSpace(config, spec);
    }
    if (space.empty()) {
        space = project.getFileColorSpace(categoryOf(spec));
    }

    return space;
}

std::string
defaultOutputSpace(const Project& project,
                   const std::string& path,
                   FileColorCategoryEnum category)
{
    const ProjectColorManagementPtr colorManagement = project.getColorManagement();
    const OCIO_NAMESPACE::ConstConfigRcPtr config = colorManagement ? colorManagement->getConfig() : OCIO_NAMESPACE::ConstConfigRcPtr();
    std::string space = fileRuleColorSpace(config, path);

    if (space.empty()) {
        space = project.getFileColorSpace(category);
    }

    return space;
}

static bool
buildProcessor(const Project& project,
               const std::string& from,
               const std::string& to,
               const std::vector<std::pair<std::string, std::string>>& context,
               const QString& failure,
               OCIO_NAMESPACE::ConstCPUProcessorRcPtr* processor,
               std::string* error)
{
    processor->reset();
    if (from.empty() || to.empty() || from == to) {
        return true;
    }
    std::string configError;
    if (project.getOCIOConfigError(&configError)) {
        *error = configError;

        return false;
    }
    const ProjectColorManagementPtr colorManagement = project.getColorManagement();
    std::vector<std::pair<std::string, std::string>> variables;
    for (std::size_t i = 0; i < context.size(); ++i) {
        if (!context[i].first.empty()) {
            variables.push_back(context[i]);
        }
    }
    OCIO_NAMESPACE::ConstCPUProcessorRcPtr cpu = colorManagement ? colorManagement->getConversionProcessor(from, to, variables) : OCIO_NAMESPACE::ConstCPUProcessorRcPtr();
    if (!cpu) {
        *error = failure.arg(QString::fromStdString(from)).arg(QString::fromStdString(to)).toStdString();

        return false;
    }
    if (!cpu->isNoOp()) {
        *processor = cpu;
    }

    return true;
}

bool
processor(const Project& project,
          const std::string& from,
          const std::string& to,
          const std::vector<std::pair<std::string, std::string>>& context,
          OCIO_NAMESPACE::ConstCPUProcessorRcPtr* processor,
          std::string* error)
{
    return buildProcessor(project, from, to, context, Project::tr("Cannot convert from the OpenColorIO colorspace \"%1\" to the colorspace \"%2\"."), processor, error);
}

bool
toWorkingProcessor(const Project& project,
                   const std::string& input,
                   const std::string& working,
                   const std::vector<std::pair<std::string, std::string>>& context,
                   OCIO_NAMESPACE::ConstCPUProcessorRcPtr* processor,
                   std::string* error)
{
    return buildProcessor(project, input, working, context, Project::tr("Cannot convert from the OpenColorIO colorspace \"%1\" to the working space \"%2\"."), processor, error);
}

bool
convertRow(const OCIO_NAMESPACE::CPUProcessor& processor,
           float* pixels,
           int width,
           int nComps,
           std::string* error)
{
    if (width <= 0 || (nComps != 3 && nComps != 4)) {
        return true;
    }
    try {
        OCIO_NAMESPACE::PackedImageDesc desc(pixels, width, 1, nComps);
        const boost_adaptbx::floating_point::FpTrapGuard fpGuard;
        processor.apply(desc);
    } catch (const std::exception& e) {
        *error = e.what();

        return false;
    }

    return true;
}
} // namespace IoColorSpace

NATRON_NAMESPACE_EXIT
