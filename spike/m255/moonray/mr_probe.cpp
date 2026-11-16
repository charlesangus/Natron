// Renders scene.usda through hdMoonray (Arras by default) with a deep RenderOutput riding on the
// "beauty" AOV next to flat AOVs, and reports what reached Hydra.
//
// UsdImagingGLEngine cannot pass aovSettings, so this drives Hydra directly through
// HdxTaskController::SetRenderOutputSettings. The stage goes in through the scene-index path because
// UsdImagingDelegate drops shader nodes Sdr does not know, such as DwaBaseMaterial.
#include <pxr/pxr.h>
#include <pxr/base/gf/rect2i.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/tf/diagnosticMgr.h>
#include <pxr/base/tf/error.h>
#include <pxr/base/tf/status.h>
#include <pxr/base/tf/warning.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/driver.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/pluginRenderDelegateUniqueHandle.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/imaging/hdx/taskController.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <OpenImageIO/imageio.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

class Capture : public TfDiagnosticMgr::Delegate {
public:
    void IssueError(const TfError& e) override { print("TF_ERROR", e.GetCommentary()); }
    void IssueFatalError(const TfCallContext&, const std::string& m) override { print("TF_FATAL", m); }
    void IssueStatus(const TfStatus& s) override { print("TF_STATUS", s.GetCommentary()); }
    void IssueWarning(const TfWarning& w) override { print("TF_WARN", w.GetCommentary()); }

private:
    static void print(const char* kind, const std::string& m) { std::printf("  %s: %s\n", kind, m.c_str()); }
};

struct Options {
    std::string scene;
    std::string outDir;
    std::string tag = "progressive";
    std::string plugin = "HdMoonrayRendererPlugin";
    std::string shimDone;
    int size = 256;
    int pixelSamples = 8;
    int deepFormat = 0;
    int timeoutSeconds = 900;
    bool deep = true;
};

struct Plane {
    int width = 0, height = 0, channels = 0;
    std::vector<float> data;
};

Plane readBuffer(HdRenderBuffer* b)
{
    Plane p;
    if (!b) {
        return p;
    }
    b->Resolve();
    const HdFormat format = b->GetFormat();
    if (HdGetComponentFormat(format) != HdFormatFloat32) {
        std::printf("  unsupported buffer format %d\n", int(format));
        return p;
    }
    p.width = int(b->GetWidth());
    p.height = int(b->GetHeight());
    p.channels = int(HdGetComponentCount(format));
    const float* src = static_cast<const float*>(b->Map());
    if (src) {
        p.data.assign(src, src + size_t(p.width) * p.height * p.channels);
    }
    b->Unmap();
    return p;
}

void writeExr(const std::string& path, const Plane& p)
{
    std::unique_ptr<OIIO::ImageOutput> out = OIIO::ImageOutput::create(path);
    if (!out) {
        return;
    }
    OIIO::ImageSpec spec(p.width, p.height, p.channels, OIIO::TypeDesc::FLOAT);
    out->open(path, spec);
    out->write_image(OIIO::TypeDesc::FLOAT, p.data.data());
    out->close();
}

void report(const char* name, const Plane& p)
{
    if (p.data.empty()) {
        std::printf("  aov %-8s MISSING\n", name);
        return;
    }
    std::vector<double> sum(p.channels, 0.0), maxv(p.channels, 0.0);
    size_t nonZero = 0, nonFinite = 0;
    const size_t pixels = size_t(p.width) * p.height;
    for (size_t i = 0; i < pixels; ++i) {
        bool any = false;
        for (int c = 0; c < p.channels; ++c) {
            const float v = p.data[i * p.channels + c];
            if (!std::isfinite(v)) {
                ++nonFinite;
                continue;
            }
            sum[c] += v;
            maxv[c] = std::max(maxv[c], double(v));
            any = any || v != 0.0f;
        }
        nonZero += any;
    }
    std::printf("  aov %-8s %dx%dx%d nonzero=%zu/%zu nonfinite=%zu mean=(", name, p.width, p.height, p.channels,
                nonZero, pixels, nonFinite);
    for (int c = 0; c < p.channels; ++c) {
        std::printf("%s%.4f", c ? "," : "", sum[c] / double(pixels));
    }
    std::printf(") max=(");
    for (int c = 0; c < p.channels; ++c) {
        std::printf("%s%.4f", c ? "," : "", maxv[c]);
    }
    std::printf(")\n");
}

double rgbRmse(const Plane& a, const Plane& b)
{
    if (a.data.empty() || b.data.empty() || a.width != b.width || a.height != b.height) {
        return -1.0;
    }
    double s = 0.0;
    const size_t pixels = size_t(a.width) * a.height;
    for (size_t i = 0; i < pixels; ++i) {
        for (int c = 0; c < 3; ++c) {
            const double d = a.data[i * a.channels + c] - b.data[i * b.channels + c];
            s += d * d;
        }
    }
    return std::sqrt(s / double(pixels * 3));
}

HdAovSettingsMap outputSettings(const Options& o, const char* aov, bool deep)
{
    HdAovSettingsMap s;
    s[TfToken("parameters:moonray:file_name")] = VtValue(o.outDir + "/" + o.tag + "_" + aov + ".exr");
    if (deep) {
        s[TfToken("parameters:moonray:output_type")] = VtValue(std::string("deep"));
    }
    return s;
}

int run(const Options& o)
{
    Capture capture;
    TfDiagnosticMgr::GetInstance().AddDelegate(&capture);

    UsdStageRefPtr stage = UsdStage::Open(o.scene);
    if (!stage) {
        std::printf("cannot open %s\n", o.scene.c_str());
        return 2;
    }
    HdRenderSettingsMap settings;
    settings[TfToken("moonray:sceneVariable:pixel_samples")] = VtValue(o.pixelSamples);
    settings[TfToken("moonray:sceneVariable:deep_format")] = VtValue(o.deepFormat);
    settings[TfToken("rdlOutput")] = VtValue(o.outDir + "/" + o.tag + "_scene.rdla");
    HdPluginRenderDelegateUniqueHandle delegate =
        HdRendererPluginRegistry::GetInstance().CreateRenderDelegate(TfToken(o.plugin), settings);
    if (!delegate) {
        std::printf("render delegate %s unavailable\n", o.plugin.c_str());
        return 2;
    }
    for (const auto& kv : settings) {
        delegate->SetRenderSetting(kv.first, kv.second);
    }
    std::unique_ptr<HdRenderIndex> index(HdRenderIndex::New(delegate.Get(), HdDriverVector()));
    UsdImagingCreateSceneIndicesInfo info;
    info.stage = stage;
    UsdImagingSceneIndices sceneIndices = UsdImagingCreateSceneIndices(info);
    sceneIndices.stageSceneIndex->SetTime(UsdTimeCode::Default());
    index->InsertSceneIndex(sceneIndices.finalSceneIndex, SdfPath::AbsoluteRootPath());

    HdxTaskController tc(index.get(), SdfPath("/m255TaskController"), false);
    const TfToken color = HdAovTokens->color, beauty("beauty"), alpha("alpha");
    tc.SetRenderOutputs({ color, beauty, alpha });
    for (const TfToken& aov : { beauty, alpha }) {
        HdAovDescriptor d = tc.GetRenderOutputSettings(aov);
        d.aovSettings = outputSettings(o, aov.GetText(), o.deep && aov == beauty);
        tc.SetRenderOutputSettings(aov, d);
    }
    tc.SetCameraPath(SdfPath("/World/cam"));
    tc.SetRenderBufferSize(GfVec2i(o.size, o.size));
    tc.SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), o.size, o.size)));
    tc.SetEnableSelection(false);

    HdEngine engine;
    const auto start = std::chrono::steady_clock::now();
    bool converged = false;
    int iterations = 0;
    double seconds = 0.0;
    for (;;) {
        sceneIndices.stageSceneIndex->ApplyPendingUpdates();
        HdTaskSharedPtrVector tasks = tc.GetRenderingTasks();
        engine.Execute(index.get(), &tasks);
        ++iterations;
        seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (tc.IsConverged()) {
            converged = true;
            break;
        }
        if (seconds > o.timeoutSeconds) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::printf("render %s: converged=%d seconds=%.1f iterations=%d deep=%d\n", o.tag.c_str(), int(converged), seconds,
                iterations, int(o.deep));

    if (converged && !o.shimDone.empty()) {
        bool done = false;
        for (int i = 0; i < 1200 && !done; ++i) {
            done = std::ifstream(o.shimDone).good();
            if (!done) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        std::printf("  mcrt frame-complete dump %s\n", done ? "seen" : "NOT SEEN within 120 s");
    }

    std::map<std::string, Plane> planes;
    for (const TfToken& aov : { color, beauty, alpha }) {
        Plane p = readBuffer(tc.GetRenderOutput(aov));
        report(aov.GetText(), p);
        if (!p.data.empty()) {
            writeExr(o.outDir + "/" + o.tag + "_hydra_" + aov.GetString() + ".exr", p);
        }
        planes[aov.GetString()] = std::move(p);
    }
    std::printf("  rmse(color.rgb, beauty.rgb)=%.6f\n", rgbRmse(planes["color"], planes["beauty"]));

    // Tearing down the Arras-backed delegate crashes; the process exits without it.
    std::fflush(stdout);
    std::_Exit(converged ? 0 : 1);
}

} // namespace

int main(int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--scene") {
            o.scene = next();
        } else if (a == "--out") {
            o.outDir = next();
        } else if (a == "--tag") {
            o.tag = next();
        } else if (a == "--plugin") {
            o.plugin = next();
        } else if (a == "--size") {
            o.size = std::atoi(next().c_str());
        } else if (a == "--pixel-samples") {
            o.pixelSamples = std::atoi(next().c_str());
        } else if (a == "--deep-format") {
            o.deepFormat = std::atoi(next().c_str());
        } else if (a == "--timeout") {
            o.timeoutSeconds = std::atoi(next().c_str());
        } else if (a == "--shim-done") {
            o.shimDone = next();
        } else if (a == "--no-deep") {
            o.deep = false;
        } else {
            std::printf("unknown argument %s\n", a.c_str());
            return 2;
        }
    }
    if (o.scene.empty() || o.outDir.empty()) {
        std::printf("usage: mr_probe --scene s.usda --out dir [--tag t] [--plugin id] [--size n] "
                    "[--pixel-samples n] [--deep-format 0|1] [--timeout s] [--shim-done path] [--no-deep]\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    const int rc = run(o);
    std::fflush(stdout);
    std::_Exit(rc);
}
