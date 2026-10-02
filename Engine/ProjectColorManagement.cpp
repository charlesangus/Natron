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

#include "ProjectColorManagement.h"

#include <exception>
#include <utility>

#include <QDir>
#include <QFileInfo>
#include <QString>

#include <OpenColorIO/OpenColorAppHelpers.h>

#include "Engine/Hash64.h"

NATRON_NAMESPACE_ENTER

namespace OCIO = OCIO_NAMESPACE;

namespace {
const char* const kURIPrefix = "ocio://";
const char* const kCustomConfigName = "Custom config";
const char* const kColorPickingRole = "color_picking";

bool
isURI(const std::string& source)
{
    return source.compare(0, std::char_traits<char>::length(kURIPrefix), kURIPrefix) == 0;
}

std::string
fromCString(const char* s)
{
    return s ? std::string(s) : std::string();
}
}

ProjectColorManagement::ProjectColorManagement()
    : _mutex()
    , _config()
    , _source()
    , _directory()
    , _workingSpace()
    , _workingToPicking()
    , _pickingToWorking()
    , _displayCache()
    , _conversionCache()
    , _callbacksMutex()
    , _callbacks()
    , _nextCallbackId(1)
{
}

ProjectColorManagement::~ProjectColorManagement()
{
}

ProjectColorManagement::LoadErrorEnum
ProjectColorManagement::load(const std::string& source,
                             const std::string& baseDir,
                             std::string* error)
{
    std::string resolved = source;
    std::string directory;
    if (!isURI(source)) {
        QFileInfo info(QString::fromStdString(source));
        if (info.isRelative() && !baseDir.empty()) {
            info.setFile(QDir(QString::fromStdString(baseDir)), QString::fromStdString(source));
        }
        if (!info.isFile()) {
            if (error) {
                *error = "OpenColorIO config file not found: " + info.absoluteFilePath().toStdString();
            }

            return eLoadErrorNoSuchFile;
        }
        resolved = info.absoluteFilePath().toStdString();
        directory = info.absolutePath().toStdString();
    }

    OCIO::ConstConfigRcPtr config;
    try {
        config = OCIO::Config::CreateFromFile(resolved.c_str());
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }

        return eLoadErrorOCIO;
    }
    if (!config) {
        if (error) {
            *error = "OpenColorIO could not load the config " + resolved;
        }

        return eLoadErrorOCIO;
    }

    {
        QMutexLocker k(&_mutex);
        _config = config;
        _source = resolved;
        _directory = directory;
        _displayCache.clear();
        _conversionCache.clear();
    }
    rebuildPickingProcessors();

    return eLoadErrorNone;
}

std::string
ProjectColorManagement::getConfigSource() const
{
    QMutexLocker k(&_mutex);

    return _source;
}

std::string
ProjectColorManagement::getConfigDirectory() const
{
    QMutexLocker k(&_mutex);

    return _directory;
}

OCIO::ConstConfigRcPtr
ProjectColorManagement::getConfig() const
{
    QMutexLocker k(&_mutex);

    return _config;
}

std::vector<std::string>
ProjectColorManagement::getColorSpaces() const
{
    std::vector<std::string> ret;
    OCIO::ConstConfigRcPtr config = getConfig();
    if (!config) {
        return ret;
    }
    const int n = config->getNumColorSpaces(OCIO::SEARCH_REFERENCE_SPACE_ALL, OCIO::COLORSPACE_ACTIVE);
    for (int i = 0; i < n; ++i) {
        ret.push_back(fromCString(config->getColorSpaceNameByIndex(OCIO::SEARCH_REFERENCE_SPACE_ALL, OCIO::COLORSPACE_ACTIVE, i)));
    }

    return ret;
}

std::vector<std::string>
ProjectColorManagement::getRoles() const
{
    std::vector<std::string> ret;
    OCIO::ConstConfigRcPtr config = getConfig();
    if (!config) {
        return ret;
    }
    const int n = config->getNumRoles();
    for (int i = 0; i < n; ++i) {
        ret.push_back(fromCString(config->getRoleName(i)));
    }

    return ret;
}

std::vector<std::string>
ProjectColorManagement::getDisplays() const
{
    std::vector<std::string> ret;
    OCIO::ConstConfigRcPtr config = getConfig();
    if (!config) {
        return ret;
    }
    const int n = config->getNumDisplays();
    for (int i = 0; i < n; ++i) {
        ret.push_back(fromCString(config->getDisplay(i)));
    }

    return ret;
}

std::vector<std::string>
ProjectColorManagement::getViews(const std::string& display) const
{
    std::vector<std::string> ret;
    OCIO::ConstConfigRcPtr config = getConfig();
    if (!config) {
        return ret;
    }
    const int n = config->getNumViews(display.c_str());
    for (int i = 0; i < n; ++i) {
        ret.push_back(fromCString(config->getView(display.c_str(), i)));
    }

    return ret;
}

std::vector<std::string>
ProjectColorManagement::getLooks() const
{
    std::vector<std::string> ret;
    OCIO::ConstConfigRcPtr config = getConfig();
    if (!config) {
        return ret;
    }
    const int n = config->getNumLooks();
    for (int i = 0; i < n; ++i) {
        ret.push_back(fromCString(config->getLookNameByIndex(i)));
    }

    return ret;
}

std::string
ProjectColorManagement::getDefaultDisplay() const
{
    OCIO::ConstConfigRcPtr config = getConfig();

    return config ? fromCString(config->getDefaultDisplay()) : std::string();
}

std::string
ProjectColorManagement::getDefaultView(const std::string& display) const
{
    OCIO::ConstConfigRcPtr config = getConfig();

    return config ? fromCString(config->getDefaultView(display.c_str())) : std::string();
}

std::string
ProjectColorManagement::resolveRoleOrName(const std::string& roleOrName) const
{
    OCIO::ConstConfigRcPtr config = getConfig();
    if (!config || roleOrName.empty()) {
        return std::string();
    }
    OCIO::ConstColorSpaceRcPtr cs = config->getColorSpace(roleOrName.c_str());

    return cs ? fromCString(cs->getName()) : std::string();
}

ProjectColorManagement::DisplayProcessorPtr
ProjectColorManagement::getDisplayProcessor(const std::string& src,
                                            const std::string& display,
                                            const std::string& view,
                                            const std::string& look,
                                            std::string* error) const
{
    const std::string key = src + "|" + display + "|" + view + "|" + look;
    OCIO::ConstConfigRcPtr config;
    {
        QMutexLocker k(&_mutex);
        std::map<std::string, DisplayProcessorPtr>::const_iterator it = _displayCache.find(key);
        if (it != _displayCache.end()) {
            return it->second;
        }
        config = _config;
    }
    if (!config) {
        if (error) {
            *error = "No OpenColorIO config is loaded";
        }

        return DisplayProcessorPtr();
    }

    std::shared_ptr<DisplayProcessor> result = std::make_shared<DisplayProcessor>();
    try {
        OCIO::DisplayViewTransformRcPtr transform = OCIO::DisplayViewTransform::Create();
        transform->setSrc(src.c_str());
        transform->setDisplay(display.c_str());
        transform->setView(view.c_str());

        OCIO::LegacyViewingPipelineRcPtr pipeline = OCIO::LegacyViewingPipeline::Create();
        pipeline->setDisplayViewTransform(transform);
        if (!look.empty()) {
            pipeline->setLooksOverrideEnabled(true);
            pipeline->setLooksOverride(look.c_str());
        }

        result->processor = pipeline->getProcessor(config);
        result->cpu = result->processor->getOptimizedCPUProcessor(OCIO::BIT_DEPTH_F32, OCIO::BIT_DEPTH_F32, OCIO::OPTIMIZATION_DEFAULT);
        result->cacheID = result->processor->getCacheID();
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }

        return DisplayProcessorPtr();
    }

    Hash64 hash;
    Hash64_appendQString(&hash, QString::fromStdString(result->cacheID));
    hash.computeHash();
    result->cacheHash = hash.value();

    DisplayProcessorPtr shared = result;
    QMutexLocker k(&_mutex);
    if (_config != config) {
        return shared;
    }
    std::pair<std::map<std::string, DisplayProcessorPtr>::iterator, bool> inserted = _displayCache.insert(std::make_pair(key, shared));

    return inserted.first->second;
}

OCIO::ConstCPUProcessorRcPtr
ProjectColorManagement::getConversionProcessor(const std::string& src,
                                               const std::string& dst) const
{
    const std::string key = src + "|" + dst;
    OCIO::ConstConfigRcPtr config;
    {
        QMutexLocker k(&_mutex);
        std::map<std::string, OCIO::ConstCPUProcessorRcPtr>::const_iterator it = _conversionCache.find(key);
        if (it != _conversionCache.end()) {
            return it->second;
        }
        config = _config;
    }
    if (!config) {
        return OCIO::ConstCPUProcessorRcPtr();
    }

    OCIO::ConstCPUProcessorRcPtr cpu;
    try {
        OCIO::ConstProcessorRcPtr processor = config->getProcessor(src.c_str(), dst.c_str());
        cpu = processor->getOptimizedCPUProcessor(OCIO::BIT_DEPTH_F32, OCIO::BIT_DEPTH_F32, OCIO::OPTIMIZATION_DEFAULT);
    } catch (const std::exception&) {
        return OCIO::ConstCPUProcessorRcPtr();
    }

    QMutexLocker k(&_mutex);
    if (_config == config) {
        _conversionCache[key] = cpu;
    }

    return cpu;
}

void
ProjectColorManagement::setWorkingSpace(const std::string& name)
{
    {
        QMutexLocker k(&_mutex);
        _workingSpace = name;
    }
    rebuildPickingProcessors();
}

void
ProjectColorManagement::rebuildPickingProcessors()
{
    OCIO::ConstConfigRcPtr config;
    std::string working;
    {
        QMutexLocker k(&_mutex);
        config = _config;
        working = _workingSpace;
    }

    OCIO::ConstCPUProcessorRcPtr forward, backward;
    if (config && !working.empty()) {
        try {
            forward = config->getProcessor(working.c_str(), kColorPickingRole)->getDefaultCPUProcessor();
            backward = config->getProcessor(kColorPickingRole, working.c_str())->getDefaultCPUProcessor();
        } catch (const std::exception&) {
            forward.reset();
            backward.reset();
        }
    }

    QMutexLocker k(&_mutex);
    if ((_config == config) && (_workingSpace == working)) {
        _workingToPicking = forward;
        _pickingToWorking = backward;
    }
}

void
ProjectColorManagement::workingToColorPicking(float* r,
                                              float* g,
                                              float* b) const
{
    OCIO::ConstCPUProcessorRcPtr cpu;
    {
        QMutexLocker k(&_mutex);
        cpu = _workingToPicking;
    }
    if (!cpu) {
        return;
    }
    float rgb[3] = { *r, *g, *b };
    cpu->applyRGB(rgb);
    *r = rgb[0];
    *g = rgb[1];
    *b = rgb[2];
}

void
ProjectColorManagement::colorPickingToWorking(float* r,
                                              float* g,
                                              float* b) const
{
    OCIO::ConstCPUProcessorRcPtr cpu;
    {
        QMutexLocker k(&_mutex);
        cpu = _pickingToWorking;
    }
    if (!cpu) {
        return;
    }
    float rgb[3] = { *r, *g, *b };
    cpu->applyRGB(rgb);
    *r = rgb[0];
    *g = rgb[1];
    *b = rgb[2];
}

std::vector<ChoiceOption>
ProjectColorManagement::builtinConfigOptions()
{
    std::vector<ChoiceOption> ret;
    const OCIO::BuiltinConfigRegistry& registry = OCIO::BuiltinConfigRegistry::Get();
    const size_t n = registry.getNumBuiltinConfigs();
    for (size_t i = 0; i < n; ++i) {
        std::string label = fromCString(registry.getBuiltinConfigUIName(i));
        if (registry.isBuiltinConfigRecommended(i)) {
            label += " (recommended)";
        }
        ret.push_back(ChoiceOption(std::string(kURIPrefix) + fromCString(registry.getBuiltinConfigName(i)), label, std::string()));
    }
    ret.push_back(ChoiceOption(kCustomConfigName));

    return ret;
}

int
ProjectColorManagement::addConfigChangedCallback(const ConfigChangedCallback& cb)
{
    QMutexLocker k(&_callbacksMutex);
    const int id = _nextCallbackId++;
    _callbacks[id] = cb;

    return id;
}

void
ProjectColorManagement::removeConfigChangedCallback(int id)
{
    QMutexLocker k(&_callbacksMutex);
    _callbacks.erase(id);
}

void
ProjectColorManagement::notifyConfigChanged()
{
    std::vector<ConfigChangedCallback> callbacks;
    {
        QMutexLocker k(&_callbacksMutex);
        for (std::map<int, ConfigChangedCallback>::const_iterator it = _callbacks.begin(); it != _callbacks.end(); ++it) {
            callbacks.push_back(it->second);
        }
    }
    for (std::size_t i = 0; i < callbacks.size(); ++i) {
        if (callbacks[i]) {
            callbacks[i]();
        }
    }
}

NATRON_NAMESPACE_EXIT
