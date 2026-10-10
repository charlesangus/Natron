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

#ifndef NATRON_ENGINE_PROJECTCOLORMANAGEMENT_H
#define NATRON_ENGINE_PROJECTCOLORMANAGEMENT_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

CLANG_DIAG_OFF(deprecated)
#include <QMutex>
CLANG_DIAG_ON(deprecated)

#include <OpenColorIO/OpenColorIO.h>

#include "Engine/ChoiceOption.h"
#include "Engine/EngineFwd.h"
#include "Global/GlobalDefines.h"

NATRON_NAMESPACE_ENTER

enum FileColorCategoryEnum {
    eFileColorCategory8Bit,
    eFileColorCategory16Bit,
    eFileColorCategoryLog,
    eFileColorCategoryFloat
};

class EightBitDisplayLut;

class ProjectColorManagement {
public:
    enum LoadErrorEnum {
        eLoadErrorNone,
        eLoadErrorNoSuchFile,
        eLoadErrorOCIO
    };

    struct DisplayProcessor {
        OCIO_NAMESPACE::ConstProcessorRcPtr processor;
        OCIO_NAMESPACE::ConstCPUProcessorRcPtr cpu;
        std::string cacheID;
        U64 cacheHash;
        // Only the built-in configs have been checked against the LUT's sampling; a custom
        // config may hide detail between the samples, so it always runs exact.
        bool eightBitLutAllowed;

        DisplayProcessor()
            : cacheHash(0)
            , eightBitLutAllowed(false)
        {
        }

        /**
         * @brief Applies cpu in place to \p width packed RGBA pixels whose result is quantised to
         * 8 bits. When eightBitLutAllowed, a processor too slow for the 8-bit viewer is approximated
         * by a shaper + 3D LUT, built on the first call, within a code value of cpu; pixels outside
         * the LUT's domain or in a cell where the LUT strays go through cpu. Otherwise cpu is applied
         * exactly. Alpha is untouched. May throw what cpu throws.
         **/
        void applyForEightBitOutput(float* rgba, int width) const;

    private:
        mutable std::once_flag _eightBitLutOnce;
        mutable std::shared_ptr<const EightBitDisplayLut> _eightBitLut;
    };

    typedef std::shared_ptr<const DisplayProcessor> DisplayProcessorPtr;
    typedef std::function<void()> ConfigChangedCallback;

    ProjectColorManagement();

    ~ProjectColorManagement();

    /**
     * @brief Loads the config named by \p source: an "ocio://" URI, or a file path that is resolved
     * against \p baseDir when relative. Never throws. On failure the previous config is kept.
     **/
    LoadErrorEnum load(const std::string& source, const std::string& baseDir, std::string* error);

    std::string getConfigSource() const;
    std::string getConfigDirectory() const;
    OCIO_NAMESPACE::ConstConfigRcPtr getConfig() const;

    std::vector<std::string> getColorSpaces() const;
    std::vector<std::string> getRoles() const;
    std::vector<std::string> getDisplays() const;
    std::vector<std::string> getViews(const std::string& display) const;
    std::vector<std::string> getLooks() const;
    std::string getDefaultDisplay() const;
    std::string getDefaultView(const std::string& display) const;

    /**
     * @brief Returns the colourspace name that \p roleOrName designates, or an empty string.
     **/
    std::string resolveRoleOrName(const std::string& roleOrName) const;

    /**
     * @brief An empty \p look applies no look override. Returns null and fills \p error on failure.
     * Gain, offset and gamma are not part of the processor.
     **/
    DisplayProcessorPtr getDisplayProcessor(const std::string& src,
                                            const std::string& display,
                                            const std::string& view,
                                            const std::string& look,
                                            std::string* error = 0) const;

    /**
     * @brief The processor converting \p src to \p dst. \p context lists the variables set on the
     * config's current context before the processor is built, in order; a processor is cached per
     * set of them.
     **/
    OCIO_NAMESPACE::ConstCPUProcessorRcPtr getConversionProcessor(const std::string& src,
                                                                  const std::string& dst,
                                                                  const std::vector<std::pair<std::string, std::string>>& context = std::vector<std::pair<std::string, std::string>>()) const;

    void setWorkingSpace(const std::string& name);

    /**
     * @brief Converts between the working space and the "color_picking" role. Both are the
     * identity when either space is unresolved.
     **/
    void workingToColorPicking(float* r, float* g, float* b) const;
    void colorPickingToWorking(float* r, float* g, float* b) const;

    /**
     * @brief The built-in config registry entries (ids are "ocio://" URIs) followed by "Custom config".
     **/
    static std::vector<ChoiceOption> builtinConfigOptions();

    int addConfigChangedCallback(const ConfigChangedCallback& cb);
    void removeConfigChangedCallback(int id);
    void notifyConfigChanged();

private:
    void rebuildPickingProcessors();

    mutable QMutex _mutex;
    OCIO_NAMESPACE::ConstConfigRcPtr _config;
    std::string _source;
    std::string _directory;
    std::string _workingSpace;
    OCIO_NAMESPACE::ConstCPUProcessorRcPtr _workingToPicking;
    OCIO_NAMESPACE::ConstCPUProcessorRcPtr _pickingToWorking;
    mutable std::map<std::string, DisplayProcessorPtr> _displayCache;
    mutable std::map<std::string, OCIO_NAMESPACE::ConstCPUProcessorRcPtr> _conversionCache;

    mutable QMutex _callbacksMutex;
    std::map<int, ConfigChangedCallback> _callbacks;
    int _nextCallbackId;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_PROJECTCOLORMANAGEMENT_H
