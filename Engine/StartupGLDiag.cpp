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

#include "StartupGLDiag.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <system_error>
#include <vector>

#include <dlfcn.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <QtCore/QByteArray>
#include <QtCore/QStandardPaths>
#include <QtCore/QString>
#include <QtCore/QtGlobal>

NATRON_NAMESPACE_ENTER

namespace StartupGLDiag {

namespace {

    namespace fs = std::filesystem;

    using GetCurrentContextFn = void* (*)();
    using GetCurrentDrawableFn = unsigned long (*)();
    using GetErrorFn = unsigned int (*)();
    using GetStringFn = const unsigned char* (*)(unsigned int);

    constexpr unsigned int kGLVendor = 0x1F00;
    constexpr unsigned int kGLRenderer = 0x1F01;
    constexpr unsigned int kGLVersion = 0x1F02;

    constexpr size_t kMaxListedFilesPerRoot = 200;

    std::atomic<QtGLStateQuery> g_qtQuery { nullptr };
    std::atomic<bool> g_failureDumped { false };
    std::atomic<bool> g_handlerInstalled { false };
    QtMessageHandler g_previousHandler = nullptr;

    // Only libraries that are already loaded are searched, so the diagnostics
    // never pull a GL or EGL implementation into the process themselves.
    void*
    resolve(const char* symbol,
            std::initializer_list<const char*> libraries)
    {
        if (void* p = dlsym(RTLD_DEFAULT, symbol)) {
            return p;
        }
        for (const char* lib : libraries) {
            void* handle = dlopen(lib, RTLD_LAZY | RTLD_NOLOAD);
            if (!handle) {
                continue;
            }
            void* p = dlsym(handle, symbol);
            dlclose(handle);
            if (p) {
                return p;
            }
        }

        return nullptr;
    }

    template <typename Fn>
    Fn
    glxSymbol(const char* symbol)
    {
        return reinterpret_cast<Fn>(resolve(symbol, { "libGL.so.1", "libGL.so", "libGLX.so.0" }));
    }

    template <typename Fn>
    Fn
    glSymbol(const char* symbol)
    {
        return reinterpret_cast<Fn>(resolve(symbol, { "libGL.so.1", "libGL.so", "libOpenGL.so.0" }));
    }

    const void*
    currentEGLContext()
    {
        auto fn = reinterpret_cast<GetCurrentContextFn>(resolve("eglGetCurrentContext", { "libEGL.so.1", "libEGL.so" }));

        return fn ? fn() : nullptr;
    }

    double
    secondsSinceFirstEvent()
    {
        static const auto start = std::chrono::steady_clock::now();

        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }

    std::string
    hexString(unsigned long long v)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%llx", v);

        return buf;
    }

    std::string
    pointerString(const void* p)
    {
        return hexString(static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p)));
    }

    std::string
    bindingString(const GLXBinding& b)
    {
        return "ctx:" + pointerString(b.context) + ",drw:" + hexString(b.drawable);
    }

    // A symbol name when the caller is exported, otherwise module+offset, which
    // addr2line resolves against the unstripped binary.
    std::string
    describeAddress(const void* addr)
    {
        if (!addr) {
            return "?";
        }
        Dl_info info {};
        if (dladdr(addr, &info) && info.dli_fname) {
            const uintptr_t a = reinterpret_cast<uintptr_t>(addr);
            if (info.dli_sname && info.dli_saddr) {
                return std::string(info.dli_sname) + "+" + hexString(a - reinterpret_cast<uintptr_t>(info.dli_saddr));
            }
            const char* slash = std::strrchr(info.dli_fname, '/');
            const char* base = slash ? slash + 1 : info.dli_fname;

            return std::string(base) + "+" + hexString(a - reinterpret_cast<uintptr_t>(info.dli_fbase));
        }

        return pointerString(addr);
    }

    std::string
    quoted(const std::string& s)
    {
        std::string out = "\"";
        for (char c : s) {
            if (c == '"' || c == '\\') {
                out += '\\';
                out += c;
            } else if (c == '\n' || c == '\r') {
                out += "\\n";
            } else {
                out += c;
            }
        }
        out += '"';

        return out;
    }

    void
    writeLine(const std::string& body)
    {
        const long tid = syscall(SYS_gettid);
        char head[96];
        std::snprintf(head, sizeof(head), "[natron-gl-diag] t=%.3f tid=%ld%s ", secondsSinceFirstEvent(), tid, (tid == getpid()) ? "(main)" : "");
        std::string line = head;
        line += body;
        line += '\n';
        // One write per line keeps lines from different threads whole.
        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fflush(stderr);
    }

    const char*
    kindName(QtNativeKind k)
    {
        switch (k) {
        case QtNativeKind::None:
            return "none";
        case QtNativeKind::GLX:
            return "glx";
        case QtNativeKind::EGL:
            return "egl";
        case QtNativeKind::Unknown:
            break;
        }

        return "unknown";
    }

    bool
    queryQt(QtGLState* state)
    {
        QtGLStateQuery q = g_qtQuery.load();
        if (!q) {
            return false;
        }
        *state = q();

        return true;
    }

    std::string
    qtStateString(bool known,
                  const QtGLState& s)
    {
        if (!known) {
            return "qt=unhooked";
        }

        return "qt=" + pointerString(s.qtContext) + " qt-native=" + kindName(s.nativeKind) + ":" + pointerString(s.nativeContext);
    }

    // Returns an empty string when Qt and the platform agree, otherwise the reason.
    std::string
    mismatchReason(const QtGLState& qt,
                   const GLXBinding& glx,
                   const void* egl)
    {
        if (!qt.qtContext) {
            if (glx.context) {
                return "qt-none-glx-current";
            }
            if (egl) {
                return "qt-none-egl-current";
            }

            return {};
        }
        switch (qt.nativeKind) {
        case QtNativeKind::GLX:
            if (!glx.context) {
                return "qt-current-glx-none";
            }
            if (glx.context != qt.nativeContext) {
                return "qt-glx-differs";
            }

            return {};
        case QtNativeKind::EGL:
            if (!egl) {
                return "qt-current-egl-none";
            }
            if (egl != qt.nativeContext) {
                return "qt-egl-differs";
            }

            return {};
        case QtNativeKind::None:
        case QtNativeKind::Unknown:
            break;
        }

        return "qt-native-unknown";
    }

    std::string
    formatMTime(const fs::file_time_type& t)
    {
        const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
        const std::time_t tt = std::chrono::system_clock::to_time_t(sys);
        std::tm tmv {};
        localtime_r(&tt, &tmv);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv);

        return buf;
    }

    void
    listCacheRoot(const char* label,
                  const fs::path& root)
    {
        std::error_code ec;
        if (root.empty() || !fs::exists(root, ec)) {
            writeLine(std::string("cache-root label=") + label + " path=" + quoted(root.string()) + " exists=0");

            return;
        }
        size_t files = 0;
        unsigned long long bytes = 0;
        fs::file_time_type newest {};
        std::vector<std::string> lines;
        auto addFile = [&](const fs::directory_entry& e) {
            std::error_code fec;
            if (!e.is_regular_file(fec)) {
                return;
            }
            const auto size = e.file_size(fec);
            const auto mtime = e.last_write_time(fec);
            ++files;
            bytes += fec ? 0 : size;
            newest = (files == 1) ? mtime : std::max(newest, mtime);
            if (lines.size() < kMaxListedFilesPerRoot) {
                lines.push_back(std::string("cache-file label=") + label + " path=" + quoted(e.path().string()) + " size=" + std::to_string(size) + " mtime=" + formatMTime(mtime));
            }
        };
        if (fs::is_directory(root, ec)) {
            for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
                addFile(*it);
            }
        } else {
            addFile(fs::directory_entry(root, ec));
        }
        writeLine(std::string("cache-root label=") + label + " path=" + quoted(root.string()) + " exists=1 files=" + std::to_string(files) + " bytes=" + std::to_string(bytes) + " newest=" + (files ? formatMTime(newest) : std::string("-")) + ((files > lines.size()) ? " truncated=1" : ""));
        for (const std::string& l : lines) {
            writeLine(l);
        }
    }

    fs::path
    xdgCacheHome()
    {
        const char* xdg = std::getenv("XDG_CACHE_HOME");
        if (xdg && xdg[0] == '/') {
            return fs::path(xdg);
        }
        const char* home = std::getenv("HOME");
        if (home && *home) {
            return fs::path(home) / ".cache";
        }

        return {};
    }

    void
    listQtShaderCaches()
    {
        const QStandardPaths::StandardLocation locations[] = { QStandardPaths::CacheLocation, QStandardPaths::GenericCacheLocation };
        const char* labels[] = { "qt-shader-cache", "qt-shader-cache-generic" };
        for (int i = 0; i < 2; ++i) {
            const QString dir = QStandardPaths::writableLocation(locations[i]);
            if (dir.isEmpty()) {
                writeLine(std::string("cache-root label=") + labels[i] + " path=\"\" exists=0");
                continue;
            }
            const fs::path root(dir.toStdString());
            std::error_code ec;
            bool any = false;
            if (fs::is_directory(root, ec)) {
                for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
                    if (it->path().filename().string().rfind("qtshadercache", 0) == 0) {
                        any = true;
                        listCacheRoot(labels[i], it->path());
                    }
                }
            }
            if (!any) {
                listCacheRoot(labels[i], root / "qtshadercache-*");
            }
        }
    }

    void
    listDriverAndNatronCaches()
    {
        const fs::path cacheHome = xdgCacheHome();

        const char* nvPath = std::getenv("__GL_SHADER_DISK_CACHE_PATH");
        if (nvPath && *nvPath) {
            listCacheRoot("nvidia-gl-cache", fs::path(nvPath));
        } else {
            listCacheRoot("nvidia-gl-cache", cacheHome.empty() ? fs::path() : cacheHome / "nvidia" / "GLCache");
        }

        const char* mesaPath = std::getenv("MESA_SHADER_CACHE_DIR");
        if (mesaPath && *mesaPath) {
            listCacheRoot("mesa-shader-cache", fs::path(mesaPath));
        } else {
            listCacheRoot("mesa-shader-cache", cacheHome.empty() ? fs::path() : cacheHome / "mesa_shader_cache");
            listCacheRoot("mesa-shader-cache-db", cacheHome.empty() ? fs::path() : cacheHome / "mesa_shader_cache_db");
        }

        // Mirrors GpuPipelineCache's default directory, which lives in an
        // anonymous namespace behind the Vulkan headers.
        listCacheRoot("natron-gpu-pipelines", cacheHome.empty() ? fs::path() : cacheHome / "Natron" / "gpu-pipelines");
    }

    std::string
    glString(GetStringFn getString,
             unsigned int name)
    {
        const unsigned char* s = getString ? getString(name) : nullptr;

        return s ? quoted(reinterpret_cast<const char*>(s)) : std::string("null");
    }

    void
    dumpFailureState()
    {
        checkpoint("shader-failure");

        const GLXBinding glx = currentGLXBinding();
        if (glx.context || currentEGLContext()) {
            auto getError = glSymbol<GetErrorFn>("glGetError");
            auto getString = glSymbol<GetStringFn>("glGetString");
            std::string errors;
            // Bounded because a lost context can report errors forever.
            for (int i = 0; getError && i < 8; ++i) {
                const unsigned int e = getError();
                if (!e) {
                    break;
                }
                errors += (errors.empty() ? "" : ",") + hexString(e);
            }
            writeLine("gl-state error=" + (errors.empty() ? std::string("0x0") : errors) + " vendor=" + glString(getString, kGLVendor) + " renderer=" + glString(getString, kGLRenderer) + " version=" + glString(getString, kGLVersion));
        } else {
            writeLine("gl-state no-current-context");
        }

        listQtShaderCaches();
        listDriverAndNatronCaches();
        writeLine("failure-dump-end");
    }

    void
    messageHandler(QtMsgType type,
                   const QMessageLogContext& context,
                   const QString& msg)
    {
        if (g_previousHandler) {
            g_previousHandler(type, context, msg);
        } else {
            std::fprintf(stderr, "%s\n", msg.toLocal8Bit().constData());
        }

        if (!msg.contains(QLatin1String("Failed to compile shader")) && !msg.contains(QLatin1String("Failed to build graphics pipeline"))) {
            return;
        }
        writeLine("qt-gl-failure msg=" + quoted(msg.toStdString()));
        if (g_failureDumped.exchange(true)) {
            return;
        }
        dumpFailureState();
    }

} // anonymous namespace

bool
enabled()
{
    static const bool on = [] {
        const char* v = std::getenv("NATRON_DIAG_GL");

        return v && std::strcmp(v, "1") == 0;
    }();

    return on;
}

void
setQtGLStateQuery(QtGLStateQuery query)
{
    if (!enabled()) {
        return;
    }
    g_qtQuery.store(query);
    writeLine("qt-hook registered");
}

void
installMessageHandler()
{
    if (!enabled() || g_handlerInstalled.exchange(true)) {
        return;
    }
    g_previousHandler = qInstallMessageHandler(messageHandler);
    writeLine("message-handler installed");
}

GLXBinding
currentGLXBinding()
{
    GLXBinding b;
    if (!enabled()) {
        return b;
    }
    if (auto getContext = glxSymbol<GetCurrentContextFn>("glXGetCurrentContext")) {
        b.context = getContext();
    }
    if (auto getDrawable = glxSymbol<GetCurrentDrawableFn>("glXGetCurrentDrawable")) {
        b.drawable = getDrawable();
    }

    return b;
}

void
logMakeCurrent(const char* tag,
               const void* caller,
               const GLXBinding& target,
               const GLXBinding& before,
               bool ok)
{
    if (!enabled()) {
        return;
    }
    const GLXBinding after = currentGLXBinding();
    QtGLState qt;
    const bool known = queryQt(&qt);
    std::string line = std::string("make-current tag=") + (tag ? tag : "?") + " caller=" + describeAddress(caller) + " target=" + bindingString(target) + " before=" + bindingString(before) + " after=" + bindingString(after) + " ok=" + (ok ? "1" : "0") + " " + qtStateString(known, qt);
    // Qt keeps its own record of the current context and skips makeCurrent
    // when that record matches, so a Natron switch under a Qt-current context
    // leaves Qt compiling against whatever Natron left bound.
    if (known && qt.qtContext && (qt.nativeKind == QtNativeKind::GLX) && (after.context != qt.nativeContext)) {
        line += " QT-STALE";
    }
    writeLine(line);
}

void
checkpoint(const char* name)
{
    if (!enabled()) {
        return;
    }
    const GLXBinding glx = currentGLXBinding();
    const void* egl = currentEGLContext();
    QtGLState qt;
    const bool known = queryQt(&qt);
    std::string line = std::string("checkpoint name=") + (name ? name : "?") + " " + qtStateString(known, qt) + " glx=" + bindingString(glx) + " egl=" + pointerString(egl);
    if (!known) {
        line += " UNCHECKED";
    } else {
        const std::string reason = mismatchReason(qt, glx, egl);
        line += reason.empty() ? std::string(" OK") : " MISMATCH reason=" + reason;
    }
    writeLine(line);
}

namespace {

    struct SwitchInfo {
        Switch id;
        const char* env;
    };

    constexpr SwitchInfo kSwitches[] = {
        { Switch::ForceProbeContext, "NATRON_DIAG_GL_FORCE_PROBE_CONTEXT" },
    };

    constexpr size_t kSwitchCount = sizeof(kSwitches) / sizeof(kSwitches[0]);

    const char*
    switchEnv(Switch s)
    {
        for (const SwitchInfo& info : kSwitches) {
            if (info.id == s) {
                return info.env;
            }
        }

        return "?";
    }

    bool
    readSwitch(const char* env)
    {
        const char* v = std::getenv(env);

        return v && std::strcmp(v, "1") == 0;
    }

    std::atomic<bool> g_switchesLogged { false };

} // anonymous namespace

bool
switchOn(Switch s)
{
    static const auto values = [] {
        std::array<bool, kSwitchCount> v {};
        for (size_t i = 0; i < kSwitchCount; ++i) {
            v[i] = readSwitch(kSwitches[i].env);
        }

        return v;
    }();

    for (size_t i = 0; i < kSwitchCount; ++i) {
        if (kSwitches[i].id == s) {
            return values[i];
        }
    }

    return false;
}

void
logActiveSwitches()
{
    if (g_switchesLogged.exchange(true)) {
        return;
    }
    for (const SwitchInfo& info : kSwitches) {
        if (switchOn(info.id)) {
            writeLine(std::string("switch active ") + info.env + "=1");
        }
    }
}

void
logSwitchAction(Switch s,
                const char* action)
{
    if (!switchOn(s)) {
        return;
    }
    writeLine(std::string("switch ") + switchEnv(s) + ": " + (action ? action : "?"));
}

} // namespace StartupGLDiag

NATRON_NAMESPACE_EXIT
