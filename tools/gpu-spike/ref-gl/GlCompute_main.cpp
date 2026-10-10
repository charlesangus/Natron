#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "blur_gl.h"
#include "grade_gl.h"
#include "ref/BlurRef.h"
#include "ref/GradeRef.h"

namespace {

constexpr int kW = 3840;
constexpr int kH = 2160;
constexpr int kC = 4;
constexpr int kWarmup = 5;
constexpr int kIters = 21;

#define GL_FN(T, n) T n = nullptr
GL_FN(PFNGLCREATESHADERPROC, glCreateShader);
GL_FN(PFNGLSHADERSOURCEPROC, glShaderSource);
GL_FN(PFNGLCOMPILESHADERPROC, glCompileShader);
GL_FN(PFNGLGETSHADERIVPROC, glGetShaderiv);
GL_FN(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog);
GL_FN(PFNGLCREATEPROGRAMPROC, glCreateProgram);
GL_FN(PFNGLATTACHSHADERPROC, glAttachShader);
GL_FN(PFNGLLINKPROGRAMPROC, glLinkProgram);
GL_FN(PFNGLGETPROGRAMIVPROC, glGetProgramiv);
GL_FN(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog);
GL_FN(PFNGLUSEPROGRAMPROC, glUseProgram);
GL_FN(PFNGLGENBUFFERSPROC, glGenBuffers);
GL_FN(PFNGLBINDBUFFERPROC, glBindBuffer);
GL_FN(PFNGLBUFFERDATAPROC, glBufferData);
GL_FN(PFNGLBUFFERSUBDATAPROC, glBufferSubData);
GL_FN(PFNGLGETBUFFERSUBDATAPROC, glGetBufferSubData);
GL_FN(PFNGLBINDBUFFERBASEPROC, glBindBufferBase);
GL_FN(PFNGLDISPATCHCOMPUTEPROC, glDispatchCompute);
GL_FN(PFNGLMEMORYBARRIERPROC, glMemoryBarrier);
GL_FN(PFNGLGENQUERIESPROC, glGenQueries);
GL_FN(PFNGLBEGINQUERYPROC, glBeginQuery);
GL_FN(PFNGLENDQUERYPROC, glEndQuery);
GL_FN(PFNGLGETQUERYOBJECTUI64VPROC, glGetQueryObjectui64v);
#undef GL_FN

template <class T>
void
load(T& fn, const char* name)
{
    fn = reinterpret_cast<T>(eglGetProcAddress(name));
    if (!fn) {
        std::fprintf(stderr, "missing GL entry point %s\n", name);
        std::exit(1);
    }
}

void
loadAll()
{
#define L(n) load(n, #n)
    L(glCreateShader);
    L(glShaderSource);
    L(glCompileShader);
    L(glGetShaderiv);
    L(glGetShaderInfoLog);
    L(glCreateProgram);
    L(glAttachShader);
    L(glLinkProgram);
    L(glGetProgramiv);
    L(glGetProgramInfoLog);
    L(glUseProgram);
    L(glGenBuffers);
    L(glBindBuffer);
    L(glBufferData);
    L(glBufferSubData);
    L(glGetBufferSubData);
    L(glBindBufferBase);
    L(glDispatchCompute);
    L(glMemoryBarrier);
    L(glGenQueries);
    L(glBeginQuery);
    L(glEndQuery);
    L(glGetQueryObjectui64v);
#undef L
}

bool
makeContext()
{
    auto queryDevices = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(eglGetProcAddress("eglQueryDevicesEXT"));
    auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (!queryDevices || !getPlatformDisplay)
        return false;
    EGLDeviceEXT devs[8];
    EGLint nd = 0;
    queryDevices(8, devs, &nd);
    EGLDisplay dpy = EGL_NO_DISPLAY;
    for (int i = 0; i < nd && dpy == EGL_NO_DISPLAY; ++i) {
        EGLDisplay d = getPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, devs[i], nullptr);
        EGLint maj, min;
        if (d != EGL_NO_DISPLAY && eglInitialize(d, &maj, &min))
            dpy = d;
    }
    if (dpy == EGL_NO_DISPLAY)
        return false;
    eglBindAPI(EGL_OPENGL_API);
    EGLint cfgAttr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE };
    EGLConfig cfg;
    EGLint nc = 0;
    if (!eglChooseConfig(dpy, cfgAttr, &cfg, 1, &nc) || nc == 0)
        return false;
    EGLint ctxAttr[] = { EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 3,
                         EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttr);
    if (ctx == EGL_NO_CONTEXT)
        return false;
    return eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx) == EGL_TRUE;
}

GLuint
buildProgram(const char* src)
{
    GLuint sh = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    char log[4096];
    if (!ok) {
        glGetShaderInfoLog(sh, sizeof log, nullptr, log);
        std::fprintf(stderr, "compile failed:\n%s\n", log);
        std::exit(1);
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, sh);
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        glGetProgramInfoLog(prog, sizeof log, nullptr, log);
        std::fprintf(stderr, "link failed:\n%s\n", log);
        std::exit(1);
    }
    return prog;
}

GLuint
makeBuffer(GLenum target, size_t bytes, const void* data)
{
    GLuint b;
    glGenBuffers(1, &b);
    glBindBuffer(target, b);
    glBufferData(target, static_cast<GLsizeiptr>(bytes), data, GL_DYNAMIC_DRAW);
    return b;
}

std::vector<float>
readBack(GLuint buf, size_t floats)
{
    std::vector<float> v(floats);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(floats * sizeof(float)), v.data());
    return v;
}

double
median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

template <class F>
double
timeKernel(F&& dispatch)
{
    GLuint q;
    glGenQueries(1, &q);
    std::vector<double> ms;
    for (int i = 0; i < kWarmup + kIters; ++i) {
        glBeginQuery(GL_TIME_ELAPSED, q);
        dispatch();
        glEndQuery(GL_TIME_ELAPSED);
        GLuint64 ns = 0;
        glGetQueryObjectui64v(q, GL_QUERY_RESULT, &ns);
        if (i >= kWarmup)
            ms.push_back(double(ns) * 1e-6);
    }
    return median(ms);
}

int
ulpDistance(float a, float b)
{
    if (std::isnan(a) || std::isnan(b))
        return (std::isnan(a) && std::isnan(b)) ? 0 : INT32_MAX;
    if (a == b)
        return 0;
    auto key = [](float f) {
        int32_t i;
        std::memcpy(&i, &f, 4);
        return i < 0 ? INT32_MIN - i : i;
    };
    int64_t d = int64_t(key(a)) - int64_t(key(b));
    return int(std::min<int64_t>(std::llabs(d), INT32_MAX));
}

struct GradeParams {
    float a[4], b[4], gamma[4], invGamma[4];
    uint32_t width, height, nComps, channelMask, flags;
    uint32_t pad[3];
};

struct BlurParams {
    uint32_t width, height, channels, radius, vertical, neumann;
    uint32_t pad[2];
};

struct GradeCase {
    const char* name;
    float a, b, gamma;
    bool reverse, clampBlack, clampWhite;
};

bool
runGrade(const std::vector<float>& input, size_t floats)
{
    GLuint prog = buildProgram(grade_gl_glsl);
    glUseProgram(prog);
    GLuint src = makeBuffer(GL_SHADER_STORAGE_BUFFER, floats * 4, input.data());
    GLuint dst = makeBuffer(GL_SHADER_STORAGE_BUFFER, floats * 4, nullptr);
    GLuint ubo = makeBuffer(GL_UNIFORM_BUFFER, sizeof(GradeParams), nullptr);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, src);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, dst);
    glBindBufferBase(GL_UNIFORM_BUFFER, 2, ubo);

    const GradeCase cases[] = {
        { "gamma=1", 1.3f, 0.05f, 1.0f, false, false, false },
        { "gamma=2.2", 1.1f, 0.02f, 2.2f, false, true, true },
        { "reverse g=2.2", 1.1f, 0.02f, 2.2f, true, false, false },
    };
    bool pass = true;
    for (const GradeCase& c : cases) {
        GradeParams p {};
        for (int i = 0; i < 4; ++i) {
            p.a[i] = c.a;
            p.b[i] = c.b;
            p.gamma[i] = c.gamma;
            p.invGamma[i] = float(1.0 / double(c.gamma));
        }
        p.width = kW;
        p.height = kH;
        p.nComps = kC;
        p.channelMask = 0xF;
        p.flags = (c.reverse ? 1u : 0u) | (c.clampBlack ? 2u : 0u) | (c.clampWhite ? 4u : 0u);
        glBindBuffer(GL_UNIFORM_BUFFER, ubo);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof p, &p);

        auto dispatch = [&] {
            glDispatchCompute((kW + 7) / 8, (kH + 7) / 8, 1);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        };
        const double ms = timeKernel(dispatch);
        const std::vector<float> out = readBack(dst, floats);

        graderef::Channel ch { c.a, c.b, c.gamma };
        int maxUlp = 0;
        double maxAbs = 0;
        size_t violations = 0;
        for (size_t i = 0; i < floats; ++i) {
            const float ref = float(graderef::apply(input[i], ch, c.reverse, c.clampBlack, c.clampWhite));
            const double ad = std::fabs(double(out[i]) - double(ref));
            const int ud = ulpDistance(out[i], ref);
            maxAbs = std::max(maxAbs, ad);
            if (ud != INT32_MAX)
                maxUlp = std::max(maxUlp, ud);
            if (ud > 4 && ad > 1e-6)
                ++violations;
        }
        std::printf("grade %-14s %8.3f ms  maxULP %d  maxAbs %.3g  violations %zu\n", c.name, ms, maxUlp,
                    maxAbs, violations);
        pass &= violations == 0;
    }
    return pass;
}

bool
runBlur(const std::vector<float>& input, size_t floats)
{
    GLuint prog = buildProgram(blur_gl_glsl);
    glUseProgram(prog);
    GLuint bufIn = makeBuffer(GL_SHADER_STORAGE_BUFFER, floats * 4, input.data());
    GLuint bufTmp = makeBuffer(GL_SHADER_STORAGE_BUFFER, floats * 4, nullptr);
    GLuint bufOut = makeBuffer(GL_SHADER_STORAGE_BUFFER, floats * 4, nullptr);
    GLuint ubo = makeBuffer(GL_UNIFORM_BUFFER, sizeof(BlurParams), nullptr);
    glBindBufferBase(GL_UNIFORM_BUFFER, 3, ubo);

    bool pass = true;
    for (double sigma : { 3.0, 25.0 }) {
        const std::vector<double> w = blurref::makeWeights(sigma);
        std::vector<float> wf(w.begin(), w.end());
        GLuint bufW = makeBuffer(GL_SHADER_STORAGE_BUFFER, wf.size() * 4, wf.data());
        const uint32_t radius = uint32_t(blurref::radiusForSigma(sigma));

        for (int neumann = 1; neumann >= 0; --neumann) {
            double passMs[2];
            for (int vertical = 0; vertical < 2; ++vertical) {
                BlurParams p { kW, kH, kC, radius, uint32_t(vertical), uint32_t(neumann), {} };
                glBindBuffer(GL_UNIFORM_BUFFER, ubo);
                glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof p, &p);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, vertical ? bufTmp : bufIn);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, vertical ? bufOut : bufTmp);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, bufW);
                const GLuint gx = ((vertical ? kH : kW) + 127) / 128;
                const GLuint gy = vertical ? kW : kH;
                passMs[vertical] = timeKernel([&] {
                    glDispatchCompute(gx, gy, 1);
                    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
                });
            }
            const std::vector<float> tmp = readBack(bufTmp, floats);
            const std::vector<float> out = readBack(bufOut, floats);

            auto sample = [&](const std::vector<float>& s, bool vertical, int line, int p, int c) {
                const int len = vertical ? kH : kW;
                const int R = int(radius);
                double acc = 0;
                for (int k = -R; k <= R; ++k) {
                    int q = p + k;
                    if (q < 0 || q >= len) {
                        if (!neumann)
                            continue;
                        q = std::clamp(q, 0, len - 1);
                    }
                    const size_t px = vertical ? size_t(q) * kW + line : size_t(line) * kW + q;
                    acc += w[k + R] * double(s[px * kC + c]);
                }
                return acc;
            };

            // Horizontal pass checked on whole rows against the input, vertical pass on
            // whole columns against the GPU's own intermediate, so each is judged alone.
            double maxErr = 0;
            const int rows[] = { 0, 1, 77, kH / 2, kH - 2, kH - 1 };
            for (int row : rows)
                for (int x = 0; x < kW; ++x)
                    for (int c = 0; c < kC; ++c) {
                        const double ref = sample(input, false, row, x, c);
                        maxErr = std::max(maxErr, std::fabs(ref - double(tmp[(size_t(row) * kW + x) * kC + c])));
                    }
            const int cols[] = { 0, 1, 129, kW / 2, kW - 2, kW - 1 };
            for (int col : cols)
                for (int y = 0; y < kH; ++y)
                    for (int c = 0; c < kC; ++c) {
                        const double ref = sample(tmp, true, col, y, c);
                        maxErr = std::max(maxErr, std::fabs(ref - double(out[(size_t(y) * kW + col) * kC + c])));
                    }
            const bool ok = maxErr <= 2e-6;
            pass &= ok;
            std::printf("blur sigma=%-4g %-9s H %8.3f ms  V %8.3f ms  total %8.3f ms  maxAbs %.3g %s\n", sigma,
                        neumann ? "neumann" : "dirichlet", passMs[0], passMs[1], passMs[0] + passMs[1], maxErr,
                        ok ? "ok" : "FAIL");
        }
    }
    return pass;
}

} // namespace

int
main()
{
    if (!makeContext()) {
        std::fprintf(stderr, "no EGL device context\n");
        return 1;
    }
    loadAll();
    std::printf("GL_RENDERER %s\nGL_VERSION %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    const size_t floats = size_t(kW) * kH * kC;
    std::vector<float> input(floats);
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> dist(0.f, 1.f);
    for (float& f : input)
        f = dist(rng);

    bool ok = runGrade(input, floats);
    ok &= runBlur(input, floats);
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
