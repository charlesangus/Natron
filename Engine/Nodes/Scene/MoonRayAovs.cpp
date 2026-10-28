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

#include "MoonRayAovs.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <mutex>
#include <set>
#include <thread>

#include <QtCore/QLoggingCategory>
#include <QtCore/QString>

#include <pxr/base/tf/stl.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/pluginRenderDelegateUniqueHandle.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/usdLux/lightFilter.h>
#include <pxr/usdImaging/usdImaging/adapterRegistry.h>
#include <pxr/usdImaging/usdImaging/apiSchemaAdapter.h>
#include <pxr/usdImaging/usdImaging/primAdapter.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>

#ifdef NATRON_HAVE_LOG4CPLUS
#include <log4cplus/appender.h>
#include <log4cplus/logger.h>
#include <log4cplus/loglevel.h>
#include <log4cplus/spi/loggingevent.h>
#include <log4cplus/tstring.h>
#endif

NATRON_NAMESPACE_ENTER

Q_LOGGING_CATEGORY(moonRayLog, "natron.render.moonray", QtInfoMsg)

PXR_NAMESPACE_USING_DIRECTIVE

namespace MoonRayAovs {
bool
isMoonRay(const std::string& rendererId)
{
    return rendererId == kMoonRayRendererId;
}

const TfToken&
depthAov()
{
    // hdMoonray's depth is the camera distance remapped to a perspective window depth with its
    // own clipping range; cameraDepth is the distance itself.
    static const TfToken token("cameraDepth");

    return token;
}

const TfToken&
normalAov()
{
    return HdAovTokens->normal;
}

const TfTokenVector&
aovs()
{
    static const TfTokenVector list = { HdAovTokens->color, depthAov(), HdAovTokens->primId, HdAovTokens->instanceId, normalAov() };

    return list;
}

float
cameraDepth(float value)
{
    if (!std::isfinite(value) || (value <= 0.f)) {
        return std::numeric_limits<float>::infinity();
    }

    return value;
}

int
reservedCores(int threadBudget,
              int hardwareThreads)
{
    if ((threadBudget <= 0) || (hardwareThreads <= 1)) {
        return 1;
    }

    return std::max(1, hardwareThreads - threadBudget);
}

std::vector<std::pair<TfToken, VtValue>>
renderSettings(int threadBudget)
{
    static const TfToken localReservedCores("localReservedCores");
    static const TfToken maxConnectRetries("maxConnectRetries");
    // The hardware count Arras subtracts the reserve from, as it reads it.
    const int hardwareThreads = (int)std::thread::hardware_concurrency();

    // A render that cannot reach its session fails at once rather than after retries.
    return { { localReservedCores, VtValue(reservedCores(threadBudget, hardwareThreads)) }, { maxConnectRetries, VtValue(0) } };
}

void
configureEngine(UsdImagingGLEngine* engine,
                int threadBudget)
{
    for (const std::pair<TfToken, VtValue>& setting : renderSettings(threadBudget)) {
        engine->SetRendererSetting(setting.first, setting.second);
    }
}

struct LogCapture::Imp {
    mutable std::mutex mutex;
    std::vector<Notice> notices;

    void add(const Notice& notice)
    {
        std::lock_guard<std::mutex> lock(mutex);

        notices.push_back(notice);
    }
};

namespace {
    class LogRouter {
    public:
        static LogRouter& instance()
        {
            static LogRouter router;

            return router;
        }

        void attach(const std::shared_ptr<LogCapture::Imp>& capture);
        void detach(const std::shared_ptr<LogCapture::Imp>& capture);

        void route(const Notice& notice)
        {
            const QString text = QString::fromStdString(notice.text);

            qCDebug(moonRayLog).noquote() << text;
            std::lock_guard<std::mutex> lock(_mutex);
            for (const std::shared_ptr<LogCapture::Imp>& capture : _captures) {
                capture->add(notice);
            }
        }

    private:
        void install();

        std::mutex _installMutex;
        std::mutex _mutex;
        std::vector<std::shared_ptr<LogCapture::Imp>> _captures;
        bool _installed = false;
    };

#ifdef NATRON_HAVE_LOG4CPLUS
    class RouterAppender
        : public log4cplus::Appender {
    public:
        RouterAppender()
        {
            setName(LOG4CPLUS_TEXT("natron"));
        }

        ~RouterAppender() override
        {
            destructorImpl();
        }

        void close() override
        {
            closed = true;
        }

    protected:
        void append(const log4cplus::spi::InternalLoggingEvent& event) override
        {
            Notice notice;
            const log4cplus::LogLevel level = event.getLogLevel();

            if (level >= log4cplus::ERROR_LOG_LEVEL) {
                notice.level = Notice::eLevelError;
            } else if (level >= log4cplus::WARN_LOG_LEVEL) {
                notice.level = Notice::eLevelWarning;
            } else if (level >= log4cplus::INFO_LOG_LEVEL) {
                notice.level = Notice::eLevelInfo;
            }
            notice.text = LOG4CPLUS_TSTRING_TO_STRING(event.getMessage());
            LogRouter::instance().route(notice);
        }
    };
#endif

    void
    LogRouter::install()
    {
#ifdef NATRON_HAVE_LOG4CPLUS
        log4cplus::Logger root = log4cplus::Logger::getRoot();

        // scene_rdl2 attaches these console appenders to the root logger when MoonRay's libraries
        // load; they would print every warning a scene produces.
        for (const char* name : { "DEBUG", "INFO", "WARN", "ERROR", "FATAL" }) {
            root.removeAppender(LOG4CPLUS_C_STR_TO_TSTRING(name));
        }
        if (!_installed) {
            root.addAppender(log4cplus::SharedAppenderPtr(new RouterAppender));
            _installed = true;
        }
#endif
    }

    void
    LogRouter::attach(const std::shared_ptr<LogCapture::Imp>& capture)
    {
        {
            // Not under _mutex: log4cplus holds its own lock while route() takes _mutex.
            std::lock_guard<std::mutex> lock(_installMutex);
            install();
        }
        std::lock_guard<std::mutex> lock(_mutex);
        _captures.push_back(capture);
    }

    void
    LogRouter::detach(const std::shared_ptr<LogCapture::Imp>& capture)
    {
        std::lock_guard<std::mutex> lock(_mutex);

        _captures.erase(std::remove(_captures.begin(), _captures.end(), capture), _captures.end());
    }

    const std::string&
    kindLabel(int kind)
    {
        static const std::string labels[] = { "lights", "materials", "prims" };

        return labels[kind];
    }

    enum KindEnum {
        eKindLight = 0,
        eKindMaterial,
        eKindPrim,
        eKindCount
    };

    // The path a notice starts with ("/World/Cube.material: ..." names /World/Cube), and the rest.
    bool
    leadingPath(const std::string& text,
                SdfPath* path,
                std::string* rest)
    {
        if (text.empty() || (text[0] != '/')) {
            return false;
        }
        size_t end = text.find_first_of(" :", 0);
        if (end == std::string::npos) {
            end = text.size();
        }
        const SdfPath parsed(text.substr(0, end));
        if (parsed.IsEmpty() || !parsed.IsAbsolutePath()) {
            return false;
        }
        *path = parsed.GetPrimPath();
        *rest = text.substr(end);

        return !path->IsEmpty();
    }

    // The nearest prim at or above path on stage.
    UsdPrim
    nearestPrim(const UsdStageRefPtr& stage,
                SdfPath path)
    {
        while (!path.IsEmpty() && !path.IsAbsoluteRootPath()) {
            const UsdPrim prim = stage->GetPrimAtPath(path);
            if (prim) {
                return prim;
            }
            path = path.GetParentPath();
        }

        return UsdPrim();
    }

    bool
    isMaterialPrim(UsdPrim prim)
    {
        for (; prim && !prim.IsPseudoRoot(); prim = prim.GetParent()) {
            if (prim.IsA<UsdShadeMaterial>() || prim.IsA<UsdShadeShader>()) {
                return true;
            }
        }

        return false;
    }

    KindEnum
    kindOf(const UsdPrim& prim,
           const std::string& rest)
    {
        if (rest.find("material") != std::string::npos) {
            return eKindMaterial;
        }
        if (prim.HasAPI<UsdLuxLightAPI>() || prim.IsA<UsdLuxLightFilter>()) {
            return eKindLight;
        }
        if (isMaterialPrim(prim)) {
            return eKindMaterial;
        }

        return eKindPrim;
    }

    // The Hydra prim types prim renders as, through the adapters UsdImaging has for its type and its
    // applied API schemas.
    std::set<TfToken>
    hydraTypes(const UsdPrim& prim)
    {
        std::set<TfToken> types;
        UsdImagingAdapterRegistry& registry = UsdImagingAdapterRegistry::GetInstance();
        const TfToken typeName = prim.GetTypeName();

        if (!typeName.IsEmpty() && registry.HasAdapter(typeName)) {
            const UsdImagingPrimAdapterSharedPtr adapter = registry.ConstructAdapter(typeName);
            if (adapter) {
                for (const TfToken& subprim : adapter->GetImagingSubprims(prim)) {
                    const TfToken type = adapter->GetImagingSubprimType(prim, subprim);
                    if (!type.IsEmpty()) {
                        types.insert(type);
                    }
                }
            }
        }
        for (const TfToken& applied : prim.GetAppliedSchemas()) {
            const std::string& full = applied.GetString();
            const size_t colon = full.find(':');
            const TfToken schema(full.substr(0, colon));
            const TfToken instance(colon == std::string::npos ? std::string() : full.substr(colon + 1));
            if (!registry.HasAPISchemaAdapter(schema)) {
                continue;
            }
            const UsdImagingAPISchemaAdapterSharedPtr adapter = registry.ConstructAPISchemaAdapter(schema);
            if (!adapter) {
                continue;
            }
            for (const TfToken& subprim : adapter->GetImagingSubprims(prim, instance)) {
                const TfToken type = adapter->GetImagingSubprimType(prim, subprim, instance);
                if (!type.IsEmpty()) {
                    types.insert(type);
                }
            }
        }

        return types;
    }

    bool
    startsWith(const std::string& text,
               const std::string& prefix)
    {
        return text.compare(0, prefix.size(), prefix) == 0;
    }

    void
    appendName(std::vector<std::string>* names,
               const std::string& name)
    {
        if (std::find(names->begin(), names->end(), name) == names->end()) {
            names->push_back(name);
        }
    }

    std::string
    joinNames(const std::vector<std::string>& names)
    {
        const size_t shown = 8;
        std::string text;

        for (size_t i = 0; (i < names.size()) && (i < shown); ++i) {
            if (i) {
                text += ", ";
            }
            text += names[i];
        }
        if (names.size() > shown) {
            text += " and " + std::to_string(names.size() - shown) + " more";
        }

        return text;
    }
} // anon namespace

LogCapture::LogCapture()
    : _imp(std::make_shared<Imp>())
{
    LogRouter::instance().attach(_imp);
}

LogCapture::~LogCapture()
{
    LogRouter::instance().detach(_imp);
}

std::vector<Notice>
LogCapture::notices() const
{
    std::lock_guard<std::mutex> lock(_imp->mutex);

    return _imp->notices;
}

bool
LogCapture::available()
{
#ifdef NATRON_HAVE_LOG4CPLUS
    return true;
#else
    return false;
#endif
}

std::string
sessionFailure(const VtDictionary& renderStats,
               const std::vector<Notice>& notices)
{
    for (const Notice& notice : notices) {
        if ((notice.level == Notice::eLevelError) && (startsWith(notice.text, "Failed to create an Arras session") || startsWith(notice.text, "Arras session stopped"))) {
            return notice.text;
        }
    }
    const VtValue* annotation = TfMapLookupPtr(renderStats, "renderProgressAnnotation");
    if (annotation && annotation->IsHolding<std::string>() && (annotation->UncheckedGet<std::string>() == "Disconnected")) {
        return "MoonRay's render session ended before the frame was complete.";
    }

    return std::string();
}

std::vector<Light>
unsupportedLights(const UsdStageRefPtr& stage,
                  const TfTokenVector& supportedPrimTypes)
{
    std::vector<Light> lights;

    if (!stage) {
        return lights;
    }
    for (const UsdPrim& prim : stage->Traverse()) {
        if (!prim.HasAPI<UsdLuxLightAPI>()) {
            continue;
        }
        std::set<TfToken> types = hydraTypes(prim);
        if (types.empty() && !prim.GetTypeName().IsEmpty()) {
            // An adapter without scene-index support names no type; UsdLux's schemas and Hydra's
            // light types differ only in the first letter (SphereLight, sphereLight).
            std::string type = prim.GetTypeName().GetString();
            type[0] = (char)std::tolower((unsigned char)type[0]);
            types.insert(TfToken(type));
        }
        for (const TfToken& type : types) {
            if (std::find(supportedPrimTypes.begin(), supportedPrimTypes.end(), type) == supportedPrimTypes.end()) {
                lights.push_back({ prim.GetPath(), prim.GetTypeName().GetString() });
                break;
            }
        }
    }

    return lights;
}

const TfTokenVector&
supportedPrimTypes()
{
    static const TfTokenVector types = [] {
        TfTokenVector list;
        HdRendererPluginRegistry& registry = HdRendererPluginRegistry::GetInstance();
        // A delegate on hdMoonray's NullRenderer, which starts no Arras session.
        HdRenderSettingsMap settings;
        settings[TfToken("disableRender")] = VtValue(true);
        HdPluginRenderDelegateUniqueHandle delegate = registry.CreateRenderDelegate(TfToken(kMoonRayRendererId), settings);
        if (delegate) {
            for (const TfTokenVector* types : { &delegate->GetSupportedSprimTypes(), &delegate->GetSupportedRprimTypes(), &delegate->GetSupportedBprimTypes() }) {
                list.insert(list.end(), types->begin(), types->end());
            }
        }

        return list;
    }();

    return types;
}

std::string
warningText(const UsdStageRefPtr& stage,
            const std::vector<Light>& unsupported,
            const std::vector<Notice>& notices)
{
    std::vector<std::string> names[eKindCount];

    for (const Light& light : unsupported) {
        appendName(&names[eKindLight], light.path.GetString() + " (" + light.type + ")");
    }
    if (stage) {
        for (const Notice& notice : notices) {
            if (notice.level < Notice::eLevelWarning) {
                continue;
            }
            SdfPath path;
            std::string rest;
            if (!leadingPath(notice.text, &path, &rest)) {
                continue;
            }
            const UsdPrim prim = nearestPrim(stage, path);
            if (!prim) {
                continue;
            }
            const KindEnum kind = kindOf(prim, rest);
            std::string name = prim.GetPath().GetString();
            if (kind == eKindLight) {
                // The scan above already names the light with its type.
                bool named = false;
                for (const Light& light : unsupported) {
                    named = named || (light.path == prim.GetPath());
                }
                if (named) {
                    continue;
                }
            }
            appendName(&names[kind], name);
        }
    }
    std::string text;
    for (int kind = 0; kind < eKindCount; ++kind) {
        if (names[kind].empty()) {
            continue;
        }
        if (!text.empty()) {
            text += '\n';
        }
        text += "MoonRay could not render these " + kindLabel(kind) + " as authored: " + joinNames(names[kind]);
    }

    return text;
}

std::string
errorText(const UsdStageRefPtr& stage,
          const std::vector<Notice>& notices)
{
    std::vector<std::string> lines;

    for (const Notice& notice : notices) {
        if (notice.level != Notice::eLevelError) {
            continue;
        }
        SdfPath path;
        std::string rest;
        if (stage && leadingPath(notice.text, &path, &rest) && nearestPrim(stage, path)) {
            continue;
        }
        appendName(&lines, notice.text);
    }
    std::string text;
    for (const std::string& line : lines) {
        if (!text.empty()) {
            text += '\n';
        }
        text += line;
    }

    return text;
}
} // namespace MoonRayAovs

NATRON_NAMESPACE_EXIT
