// Renders a small deep test scene through UsdImagingGLEngine the way Natron's USDRender does
// (gpuEnabled = false, presentation off, SetRendererAovs, Render until converged), once flat
// ({color}) and once deep ({color, cycles:deep}), reads the deep samples the delegate hands out
// through HdRenderBuffer::GetResource(), flattens them, compares the flattening with the colour
// AOV, and prints sample-count statistics and timings.
//
// Usage: deep_probe [size] [samples] [maxDeepSamples] [outDir] [--flat-only]

#include "HydraDeepBuffer.h"

#include <pxr/pxr.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/diagnosticMgr.h>
#include <pxr/base/tf/error.h>
#include <pxr/base/tf/status.h>
#include <pxr/base/tf/warning.h>
#include <pxr/base/vt/array.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/rendererPlugin.h>
#include <pxr/imaging/hd/rendererPluginHandle.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/sphereLight.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

#include <OpenImageIO/imageio.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

const TfToken kDeepAov("cycles:deep");

class Capture : public TfDiagnosticMgr::Delegate {
public:
    void IssueError(TfError const& e) override { add("ERROR", e.GetCommentary()); }
    void IssueFatalError(TfCallContext const&, std::string const& msg) override { add("FATAL", msg); }
    void IssueStatus(TfStatus const& s) override { add("STATUS", s.GetCommentary()); }
    void IssueWarning(TfWarning const& w) override { add("WARNING", w.GetCommentary()); }

    void flush()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (const std::string& line : _lines) {
            if (line.rfind("STATUS", 0) != 0) {
                std::printf("  diag %s\n", line.c_str());
            }
        }
        _lines.clear();
    }

private:
    void add(const char* kind, const std::string& msg)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _lines.push_back(std::string(kind) + ": " + msg);
    }

    std::mutex _mutex;
    std::vector<std::string> _lines;
};

void bind(const UsdStageRefPtr& stage, const UsdPrim& prim, const char* name, GfVec3f colour, float opacity)
{
    const std::string path = std::string("/World/mtl/") + name;
    UsdShadeMaterial mat = UsdShadeMaterial::Define(stage, SdfPath(path));
    UsdShadeShader surf = UsdShadeShader::Define(stage, SdfPath(path + "/surface"));
    surf.CreateIdAttr(VtValue(TfToken("UsdPreviewSurface")));
    surf.CreateInput(TfToken("diffuseColor"), SdfValueTypeNames->Color3f).Set(colour);
    surf.CreateInput(TfToken("roughness"), SdfValueTypeNames->Float).Set(0.6f);
    surf.CreateInput(TfToken("opacity"), SdfValueTypeNames->Float).Set(opacity);
    mat.CreateSurfaceOutput().ConnectToSource(surf.ConnectableAPI(), TfToken("surface"));
    UsdShadeMaterialBindingAPI::Apply(prim).Bind(mat);
}

void addCard(const UsdStageRefPtr& stage, const char* name, GfVec3f centre, float half, GfVec3f colour,
             float opacity)
{
    UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath(std::string("/World/") + name));
    const float x = centre[0], y = centre[1], z = centre[2];
    mesh.CreatePointsAttr(VtValue(VtVec3fArray {
        { x - half, y - half, z }, { x + half, y - half, z }, { x + half, y + half, z }, { x - half, y + half, z } }));
    mesh.CreateFaceVertexCountsAttr(VtValue(VtIntArray { 4 }));
    mesh.CreateFaceVertexIndicesAttr(VtValue(VtIntArray { 0, 1, 2, 3 }));
    mesh.CreateSubdivisionSchemeAttr(VtValue(UsdGeomTokens->none));
    mesh.CreateDoubleSidedAttr(VtValue(true));
    bind(stage, mesh.GetPrim(), name, colour, opacity);
}

void addSphere(const UsdStageRefPtr& stage, GfVec3f centre, float radius)
{
    const int rings = 24, segments = 48;
    VtVec3fArray points;
    VtIntArray counts, indices;
    for (int r = 0; r <= rings; ++r) {
        const double theta = M_PI * r / rings;
        for (int s = 0; s < segments; ++s) {
            const double phi = 2.0 * M_PI * s / segments;
            points.push_back(centre + radius * GfVec3f(float(std::sin(theta) * std::cos(phi)), float(std::cos(theta)),
                                                       float(std::sin(theta) * std::sin(phi))));
        }
    }
    for (int r = 0; r < rings; ++r) {
        for (int s = 0; s < segments; ++s) {
            const int a = r * segments + s, b = r * segments + (s + 1) % segments;
            counts.push_back(4);
            for (int i : { a, b, b + segments, a + segments }) {
                indices.push_back(i);
            }
        }
    }
    UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath("/World/sphere"));
    mesh.CreatePointsAttr(VtValue(points));
    mesh.CreateFaceVertexCountsAttr(VtValue(counts));
    mesh.CreateFaceVertexIndicesAttr(VtValue(indices));
    mesh.CreateSubdivisionSchemeAttr(VtValue(UsdGeomTokens->none));
    bind(stage, mesh.GetPrim(), "sphere", GfVec3f(0.8f, 0.8f, 0.75f), 1.0f);
}

// Three overlapping semi-transparent cards at different depths in front of and behind an opaque
// sphere, with uncovered background (transparent film) around them.
UsdStageRefPtr makeStage()
{
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdGeomSetStageUpAxis(stage, UsdGeomTokens->y);
    addSphere(stage, GfVec3f(0.3f, 0.0f, -1.0f), 1.0f);
    addCard(stage, "cardNear", GfVec3f(-0.7f, 0.4f, 1.5f), 1.0f, GfVec3f(0.9f, 0.15f, 0.1f), 0.5f);
    addCard(stage, "cardMid", GfVec3f(0.5f, -0.4f, 0.6f), 1.1f, GfVec3f(0.1f, 0.3f, 0.9f), 0.4f);
    addCard(stage, "cardFar", GfVec3f(-0.4f, -0.2f, -2.6f), 1.4f, GfVec3f(0.1f, 0.8f, 0.2f), 0.7f);

    UsdLuxDistantLight sun = UsdLuxDistantLight::Define(stage, SdfPath("/World/sun"));
    sun.CreateIntensityAttr(VtValue(3.0f));
    UsdGeomXformCommonAPI(sun.GetPrim()).SetRotate(GfVec3f(-35, 25, 0));
    UsdLuxSphereLight lamp = UsdLuxSphereLight::Define(stage, SdfPath("/World/lamp"));
    lamp.CreateIntensityAttr(VtValue(60.0f));
    lamp.CreateRadiusAttr(VtValue(0.3f));
    UsdGeomXformCommonAPI(lamp.GetPrim()).SetTranslate(GfVec3d(1.5, 2.5, 4));

    UsdGeomCamera cam = UsdGeomCamera::Define(stage, SdfPath("/World/cam"));
    cam.CreateFocalLengthAttr(VtValue(35.0f));
    cam.CreateClippingRangeAttr(VtValue(GfVec2f(0.1f, 100.0f)));
    UsdGeomXformCommonAPI(cam.GetPrim()).SetTranslate(GfVec3d(0, 0, 8));
    return stage;
}

struct Render {
    double seconds = 0;
    bool converged = false;
    int width = 0, height = 0;
    std::vector<float> color;
    std::vector<float> deepPixels;
    VtValue resource;
    std::string peakMemory;
};

std::vector<float> readRgba(HdRenderBuffer* b, int& w, int& h)
{
    std::vector<float> out;
    if (!b || b->GetFormat() != HdFormatFloat32Vec4) {
        std::printf("  buffer missing or not Float32Vec4 (format %d)\n", b ? int(b->GetFormat()) : -1);
        return out;
    }
    b->Resolve();
    w = int(b->GetWidth());
    h = int(b->GetHeight());
    const float* data = static_cast<const float*>(b->Map());
    if (data) {
        out.assign(data, data + size_t(w) * h * 4);
    }
    b->Unmap();
    return out;
}

Render render(const TfToken& plugin, int size, int samples, int maxDeep, bool deep, Capture& capture)
{
    Render r;
    UsdStageRefPtr stage = makeStage();
    UsdImagingGLEngine::Parameters params;
    params.rendererPluginId = plugin;
    params.gpuEnabled = false;
    std::unique_ptr<UsdImagingGLEngine> engine(new UsdImagingGLEngine(params));
    engine->SetEnablePresentation(false);
    engine->SetRendererSetting(TfToken("stageMetersPerUnit"), VtValue(1.0));
    engine->SetRendererSetting(TfToken("cycles:samples"), VtValue(samples));
    if (deep) {
        engine->SetRendererSetting(TfToken("cycles:film:deep_max_samples"), VtValue(maxDeep));
    }
    TfTokenVector aovs = { HdAovTokens->color };
    if (deep) {
        aovs.push_back(kDeepAov);
    }
    if (!engine->SetRendererAovs(aovs)) {
        std::printf("  SetRendererAovs failed\n");
    }
    engine->SetCameraPath(SdfPath("/World/cam"));
    engine->SetRenderBufferSize(GfVec2i(size, size));
    engine->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), size, size)));
    UsdImagingGLRenderParams rp;
    rp.enableLighting = true;
    rp.enableSceneMaterials = true;
    const auto start = std::chrono::steady_clock::now();
    do {
        engine->Render(stage->GetPseudoRoot(), rp);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (!engine->IsConverged() && std::chrono::steady_clock::now() - start < std::chrono::seconds(300));
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    r.converged = engine->IsConverged();
    const VtDictionary stats = engine->GetRenderStats();
    const auto peak = stats.find("peakMemory");
    r.peakMemory = peak != stats.end() ? TfStringify(peak->second) : "?";
    r.color = readRgba(engine->GetAovRenderBuffer(HdAovTokens->color), r.width, r.height);
    if (deep) {
        HdRenderBuffer* b = engine->GetAovRenderBuffer(kDeepAov);
        if (!b) {
            std::printf("  no render buffer for %s\n", kDeepAov.GetText());
        } else {
            r.resource = b->GetResource(false);
            int w = 0, h = 0;
            r.deepPixels = readRgba(b, w, h);
        }
    }
    engine.reset();
    capture.flush();
    return r;
}

void writeImage(const std::string& path, int w, int h, int channels, const std::vector<float>& px, bool flip)
{
    const bool png = path.size() > 4 && path.compare(path.size() - 4, 4, ".png") == 0;
    std::vector<float> rows(px.size());
    for (int y = 0; y < h; ++y) {
        const int src = flip ? h - 1 - y : y;
        std::memcpy(rows.data() + size_t(y) * w * channels, px.data() + size_t(src) * w * channels,
                    sizeof(float) * w * channels);
    }
    auto out = OIIO::ImageOutput::create(path);
    if (!out) {
        std::printf("  cannot write %s\n", path.c_str());
        return;
    }
    if (png) {
        std::vector<unsigned char> bytes(rows.size());
        for (size_t i = 0; i < rows.size(); ++i) {
            const bool alpha = channels == 4 && i % 4 == 3;
            const float v = std::clamp(rows[i], 0.0f, 1.0f);
            bytes[i] = (unsigned char)std::lround(255.0f * (alpha ? v : std::pow(v, 1.0f / 2.2f)));
        }
        out->open(path, OIIO::ImageSpec(w, h, channels, OIIO::TypeDesc::UINT8));
        out->write_image(OIIO::TypeDesc::UINT8, bytes.data());
    } else {
        out->open(path, OIIO::ImageSpec(w, h, channels, OIIO::TypeDesc::FLOAT));
        out->write_image(OIIO::TypeDesc::FLOAT, rows.data());
    }
    out->close();
    std::printf("  wrote %s\n", path.c_str());
}

void compare(const char* label, const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.empty() || a.size() != b.size()) {
        std::printf("  %s: cannot compare (sizes %zu, %zu)\n", label, a.size(), b.size());
        return;
    }
    double sum[4] = {}, worst[4] = {};
    for (size_t i = 0; i < a.size(); ++i) {
        const double d = std::fabs(double(a[i]) - double(b[i]));
        sum[i % 4] += d;
        worst[i % 4] = std::max(worst[i % 4], d);
    }
    const double n = double(a.size() / 4);
    std::printf("  %s: mean |diff| R %.5f G %.5f B %.5f A %.5f, max R %.4f G %.4f B %.4f A %.4f\n", label,
                sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n, worst[0], worst[1], worst[2], worst[3]);
}

void dumpPixel(const HydraDeepBuffer& d, int x, int y)
{
    const size_t p = size_t(y) * d.width + x;
    std::printf("  pixel (%d,%d) buffer row %d: %u samples\n", x, d.height - 1 - y, y, d.counts[p]);
    for (uint32_t s = 0; s < d.counts[p]; ++s) {
        const uint64_t i = d.offsets[p] + s;
        std::printf("    Z %.4f ZBack %.4f  RGBA %.4f %.4f %.4f %.4f\n", d.channels[4][i], d.channels[5][i],
                    d.channels[0][i], d.channels[1][i], d.channels[2][i], d.channels[3][i]);
    }
}

void report(const HydraDeepBuffer& d, int maxDeep, const std::vector<float>& color,
            const std::vector<float>& delegateFlat, const std::string& outDir)
{
    const size_t numPixels = size_t(d.width) * d.height;
    if (d.counts.size() != numPixels || d.offsets.size() != numPixels || d.channels.size() != d.channelNames.size()) {
        std::printf("  malformed deep buffer\n");
        return;
    }
    std::printf("  channels:");
    for (const std::string& n : d.channelNames) {
        std::printf(" %s", n.c_str());
    }
    std::printf("\n");
    for (const char* need : { "R", "G", "B", "A", "Z", "ZBack" }) {
        if (d.channelIndex(need) < 0) {
            std::printf("  missing channel %s\n", need);
            return;
        }
    }
    uint32_t lo = UINT32_MAX, hi = 0;
    uint64_t total = 0, covered = 0, volume = 0;
    uint64_t hist[8] = {};
    for (size_t p = 0; p < numPixels; ++p) {
        const uint32_t c = d.counts[p];
        lo = std::min(lo, c);
        hi = std::max(hi, c);
        total += c;
        covered += c > 0;
        hist[c == 0 ? 0 : c == 1 ? 1 : c == 2 ? 2 : c == 3 ? 3 : c == 4 ? 4 : c <= 8 ? 5 : c <= 16 ? 6 : 7]++;
    }
    float zlo = INFINITY, zhi = -INFINITY;
    for (uint64_t i = 0; i < total; ++i) {
        zlo = std::min(zlo, d.channels[4][i]);
        zhi = std::max(zhi, d.channels[4][i]);
        volume += d.channels[5][i] > d.channels[4][i] + 1e-6f;
    }
    std::printf("  samples per pixel: min %u mean %.3f max %u (mean over covered pixels %.3f), total %llu\n", lo,
                double(total) / numPixels, hi, covered ? double(total) / covered : 0.0, (unsigned long long)total);
    std::printf("  histogram 0:%llu 1:%llu 2:%llu 3:%llu 4:%llu 5-8:%llu 9-16:%llu 17+:%llu\n",
                (unsigned long long)hist[0], (unsigned long long)hist[1], (unsigned long long)hist[2],
                (unsigned long long)hist[3], (unsigned long long)hist[4], (unsigned long long)hist[5],
                (unsigned long long)hist[6], (unsigned long long)hist[7]);
    std::printf("  Z range %.4f .. %.4f, volume samples %llu, pixels at the %d-sample cap %llu\n", zlo, zhi,
                (unsigned long long)volume, maxDeep,
                (unsigned long long)std::count(d.counts.begin(), d.counts.end(), uint32_t(maxDeep)));
    const double handedOut = (double(total) * d.channels.size() * sizeof(float) +
                              numPixels * (sizeof(uint32_t) + sizeof(uint64_t))) / numPixels;
    const double reserved = double(maxDeep) * 32 + sizeof(uint32_t);
    std::printf("  bytes per pixel: handed out %.1f (%zu float channels + count + offset), kernel buffer %.0f "
                "(%d slots x 32 B + count), kernel buffer total %.1f MB\n",
                handedOut, d.channels.size(), reserved, maxDeep, reserved * numPixels / (1024.0 * 1024.0));

    std::vector<float> flat(numPixels * 4, 0.0f), countImage(numPixels);
    bool sorted = true;
    for (size_t p = 0; p < numPixels; ++p) {
        float* out = flat.data() + p * 4;
        for (uint32_t s = 0; s < d.counts[p]; ++s) {
            const uint64_t i = d.offsets[p] + s;
            if (s > 0 && d.channels[4][i] < d.channels[4][i - 1]) {
                sorted = false;
            }
            const float t = 1.0f - out[3];
            for (int c = 0; c < 4; ++c) {
                out[c] += t * d.channels[c][i];
            }
        }
        countImage[p] = hi ? float(d.counts[p]) / float(hi) : 0.0f;
    }
    std::printf("  samples sorted front to back: %s\n", sorted ? "yes" : "NO");
    compare("flatten(deep) vs color AOV", flat, color);
    compare("flatten(deep) vs cycles:deep buffer pixels", flat, delegateFlat);

    const int w = d.width, h = d.height;
    dumpPixel(d, w / 2, h / 2);
    dumpPixel(d, w * 3 / 10, h * 6 / 10);
    dumpPixel(d, w * 6 / 10, h * 4 / 10);
    dumpPixel(d, w / 10, h / 10);

    writeImage(outDir + "/deep_flat.exr", w, h, 4, flat, true);
    writeImage(outDir + "/deep_flat.png", w, h, 4, flat, true);
    writeImage(outDir + "/color.png", w, h, 4, color, true);
    writeImage(outDir + "/deep_count.png", w, h, 1, countImage, true);
}

} // namespace

int main(int argc, char** argv)
{
    const int size = argc > 1 ? std::atoi(argv[1]) : 256;
    const int samples = argc > 2 ? std::atoi(argv[2]) : 16;
    const int maxDeep = argc > 3 ? std::atoi(argv[3]) : 64;
    const std::string outDir = argc > 4 ? argv[4] : "out";
    const bool flatOnly = argc > 5 && std::strcmp(argv[5], "--flat-only") == 0;
    const TfToken plugin("HdCyclesPlugin");

    Capture capture;
    TfDiagnosticMgr::GetInstance().AddDelegate(&capture);

    {
        HdRendererPluginHandle ph = HdRendererPluginRegistry::GetInstance().GetOrCreateRendererPlugin(plugin);
        HdRenderDelegate* d = ph ? ph->CreateRenderDelegate() : nullptr;
        if (!d) {
            std::printf("could not create %s\n", plugin.GetText());
            capture.flush();
            return 1;
        }
        const HdAovDescriptor desc = d->GetDefaultAovDescriptor(kDeepAov);
        std::printf("== delegate: %s descriptor format %d (%s)\n", kDeepAov.GetText(), int(desc.format),
                    desc.format == HdFormatFloat32Vec4 ? "patched delegate" : "stock delegate, no deep");
        ph->DeleteRenderDelegate(d);
        capture.flush();
        if (!flatOnly && desc.format != HdFormatFloat32Vec4) {
            return 1;
        }
    }
    std::printf("== scene: %dx%d, %d samples, deep slots per pixel %d\n", size, size, samples, maxDeep);

    double flatTimes[2] = {}, deepTimes[2] = {};
    std::string flatPeak, deepPeak;
    Render deep;
    for (int round = 0; round < 2; ++round) {
        std::printf("== round %d flat {color}\n", round + 1);
        Render flat = render(plugin, size, samples, maxDeep, false, capture);
        flatTimes[round] = flat.seconds;
        flatPeak = flat.peakMemory;
        std::printf("  %.3f s converged=%d peakMemory=%s\n", flat.seconds, int(flat.converged), flat.peakMemory.c_str());
        if (flatOnly) {
            if (round == 1) {
                writeImage(outDir + "/color.png", flat.width, flat.height, 4, flat.color, true);
            }
            continue;
        }
        std::printf("== round %d deep {color, %s}\n", round + 1, kDeepAov.GetText());
        deep = render(plugin, size, samples, maxDeep, true, capture);
        deepTimes[round] = deep.seconds;
        deepPeak = deep.peakMemory;
        std::printf("  %.3f s converged=%d peakMemory=%s\n", deep.seconds, int(deep.converged), deep.peakMemory.c_str());
        if (round == 0) {
            compare("deep run color vs flat run color", deep.color, flat.color);
        }
    }

    std::printf("== timing (render to converged, wall clock)\n");
    std::printf("  flat: %.3f s, %.3f s   peakMemory %s\n", flatTimes[0], flatTimes[1], flatPeak.c_str());
    if (flatOnly) {
        TfDiagnosticMgr::GetInstance().RemoveDelegate(&capture);
        return 0;
    }
    std::printf("  deep: %.3f s, %.3f s   peakMemory %s\n", deepTimes[0], deepTimes[1], deepPeak.c_str());
    std::printf("  deep / flat (round 2): %.2fx\n", flatTimes[1] > 0 ? deepTimes[1] / flatTimes[1] : 0.0);

    std::printf("== deep buffer (last deep render)\n");
    std::printf("  GetResource type: %s\n", deep.resource.IsEmpty() ? "empty" : deep.resource.GetTypeName().c_str());
    if (!deep.resource.IsHolding<HydraDeepBufferPtr>() || !deep.resource.UncheckedGet<HydraDeepBufferPtr>()) {
        std::printf("  RESULT: no HydraDeepBuffer handed out\n");
        TfDiagnosticMgr::GetInstance().RemoveDelegate(&capture);
        return 1;
    }
    const HydraDeepBuffer& d = *deep.resource.UncheckedGet<HydraDeepBufferPtr>();
    std::printf("  %dx%d\n", d.width, d.height);
    report(d, maxDeep, deep.color, deep.deepPixels, outDir);
    std::printf("  RESULT: deep buffer handed out\n");
    TfDiagnosticMgr::GetInstance().RemoveDelegate(&capture);
    return 0;
}
