// LD_PRELOADed into execComp (mcrt). When the progressive frame first reports complete, writes every
// RenderOutput exactly as batch moonray does, so the deep buffer an Arras DeepFrame message would
// serialise can be inspected without patching MoonRay.
#include <moonray/rendering/rndr/RenderContext.h>
#include <moonray/rendering/rndr/RenderOutputDriver.h>
#include <scene_rdl2/common/fb_util/FbTypes.h>
#include <scene_rdl2/common/fb_util/VariablePixelBuffer.h>

#include <dlfcn.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace moonray {
namespace pbr {
// Not installed; only the exported out-of-line member is used.
class DeepBuffer {
public:
    size_t getMemoryUsage() const;
};
} // namespace pbr
} // namespace moonray

namespace {

using IsFrameComplete = bool (*)(const moonray::rndr::RenderContext*);

thread_local bool tInside = false;
std::mutex gMutex;
const moonray::rndr::RenderContext* gLastComplete = nullptr;
int gDumps = 0;

void logLine(const std::string& line)
{
    const char* path = std::getenv("M255_SHIM_LOG");
    FILE* f = path ? std::fopen(path, "a") : stderr;
    if (!f) {
        return;
    }
    std::fprintf(f, "%s\n", line.c_str());
    if (f != stderr) {
        std::fclose(f);
    }
}

void dump(const moonray::rndr::RenderContext* constCtx)
{
    using namespace moonray;
    auto* ctx = const_cast<rndr::RenderContext*>(constCtx);
    const auto start = std::chrono::steady_clock::now();
    const pbr::DeepBuffer* deep = ctx->getDeepBuffer();
    const rndr::RenderOutputDriver* rod = ctx->getRenderOutputDriver();
    logLine("frame complete: dump " + std::to_string(++gDumps) + " deepBuffer=" + (deep ? "yes" : "null") +
            " deepBytes=" + std::to_string(deep ? deep->getMemoryUsage() : 0) +
            " requiresDeep=" + std::to_string(rod ? int(rod->requiresDeepBuffer()) : -1) +
            " outputs=" + std::to_string(rod ? int(rod->getNumberOfRenderOutputs()) : -1));
    if (!rod) {
        return;
    }
    for (unsigned i = 0; i < rod->getNumberOfRenderOutputs(); ++i) {
        const scene_rdl2::rdl2::RenderOutput* ro = rod->getRenderOutput(i);
        logLine("  output " + ro->getName() + " type=" + ro->getOutputType() + " file=" + ro->getFileName());
    }
    pbr::CryptomatteBuffer* crypto = ctx->getCryptomatteBuffer();
    scene_rdl2::fb_util::HeatMapBuffer heatMap;
    ctx->snapshotHeatMapBuffer(&heatMap, true, true);
    scene_rdl2::fb_util::FloatBuffer weight;
    ctx->snapshotWeightBuffer(&weight, true, true);
    std::vector<scene_rdl2::fb_util::VariablePixelBuffer> aovs;
    ctx->snapshotAovBuffers(aovs, true, true);
    scene_rdl2::fb_util::RenderBuffer odd;
    ctx->snapshotRenderBufferOdd(&odd, true, true);
    std::vector<scene_rdl2::fb_util::VariablePixelBuffer> displayFilters;
    ctx->snapshotDisplayFilterBuffers(displayFilters, true, true);
    rod->writeFinal(deep, crypto, &heatMap, &weight, &odd, aovs, displayFilters, nullptr);
    const bool ok = rod->loggingErrorAndInfo(nullptr);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    logLine("  writeFinal " + std::string(ok ? "ok" : "FAILED") + " in " + std::to_string(ms) + " ms");
    if (const char* done = std::getenv("M255_SHIM_DONE")) {
        if (FILE* f = std::fopen(done, "w")) {
            std::fprintf(f, "%d\n", gDumps);
            std::fclose(f);
        }
    }
}

} // namespace

bool moonray::rndr::RenderContext::isFrameComplete() const
{
    static IsFrameComplete real =
        reinterpret_cast<IsFrameComplete>(dlsym(RTLD_NEXT, "_ZNK7moonray4rndr13RenderContext15isFrameCompleteEv"));
    const bool complete = real(this);
    if (tInside) {
        return complete;
    }
    std::lock_guard<std::mutex> lock(gMutex);
    if (!complete) {
        if (gLastComplete == this) {
            gLastComplete = nullptr;
        }
        return complete;
    }
    if (gLastComplete != this) {
        gLastComplete = this;
        tInside = true;
        dump(this);
        tInside = false;
    }
    return complete;
}
