// Probes which AOVs the Cycles Hydra delegate (or any delegate named on the command line) renders
// through UsdImagingGLEngine, the path Natron's USDRender takes: gpuEnabled = false, presentation
// off, SetRendererAovs, Render until converged, GetAovRenderBuffer, Resolve, Map.
//
// Usage: aov_probe [pluginId] [size] [samples] [aovName ...]
// With no AOV names it runs its built-in candidate list. Every request is {color, candidate}, on a
// fresh engine, so one candidate's passes cannot leak into the next; a last run asks for every
// candidate that came back with data, together.

#include <pxr/pxr.h>
#include <pxr/base/gf/half.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/tf/diagnosticMgr.h>
#include <pxr/base/tf/error.h>
#include <pxr/base/tf/status.h>
#include <pxr/base/tf/warning.h>
#include <pxr/base/vt/array.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/rendererPlugin.h>
#include <pxr/imaging/hd/rendererPluginHandle.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/sphereLight.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

class Capture : public TfDiagnosticMgr::Delegate {
public:
    void IssueError(TfError const& e) override { add("ERROR", e.GetCommentary()); }
    void IssueFatalError(TfCallContext const&, std::string const& msg) override { add("FATAL", msg); }
    void IssueStatus(TfStatus const& s) override { add("STATUS", s.GetCommentary()); }
    void IssueWarning(TfWarning const& w) override { add("WARNING", w.GetCommentary()); }

    std::vector<std::string> take()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::vector<std::string> out;
        out.swap(_lines);
        return out;
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

void addBox(const UsdStageRefPtr& stage, const char* path, GfVec3f lo, GfVec3f hi, GfVec3f colour)
{
    UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath(path));
    VtVec3fArray p = {
        { lo[0], lo[1], lo[2] }, { hi[0], lo[1], lo[2] }, { hi[0], hi[1], lo[2] }, { lo[0], hi[1], lo[2] },
        { lo[0], lo[1], hi[2] }, { hi[0], lo[1], hi[2] }, { hi[0], hi[1], hi[2] }, { lo[0], hi[1], hi[2] },
    };
    VtIntArray counts(6, 4);
    VtIntArray idx = { 0, 3, 2, 1, 4, 5, 6, 7, 0, 1, 5, 4, 2, 3, 7, 6, 1, 2, 6, 5, 0, 4, 7, 3 };
    mesh.CreatePointsAttr(VtValue(p));
    mesh.CreateFaceVertexCountsAttr(VtValue(counts));
    mesh.CreateFaceVertexIndicesAttr(VtValue(idx));
    mesh.CreateSubdivisionSchemeAttr(VtValue(UsdGeomTokens->none));
    mesh.CreateDisplayColorAttr(VtValue(VtVec3fArray { colour }));
    UsdGeomPrimvarsAPI(mesh.GetPrim())
        .CreatePrimvar(TfToken("probeScalar"), SdfValueTypeNames->Float, UsdGeomTokens->constant)
        .Set(0.75f);
}

UsdStageRefPtr makeStage()
{
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdGeomSetStageUpAxis(stage, UsdGeomTokens->y);
    addBox(stage, "/World/ground", { -4, -1.1f, -4 }, { 4, -1, 4 }, { 0.5f, 0.5f, 0.5f });
    addBox(stage, "/World/boxA", { -2, -1, -0.5f }, { -0.5f, 0.5f, 1 }, { 0.8f, 0.2f, 0.2f });
    addBox(stage, "/World/boxB", { 0.5f, -1, -1 }, { 2, 1, 0.5f }, { 0.2f, 0.3f, 0.9f });

    UsdShadeMaterial mat = UsdShadeMaterial::Define(stage, SdfPath("/World/mtl/glossy"));
    UsdShadeShader surf = UsdShadeShader::Define(stage, SdfPath("/World/mtl/glossy/surface"));
    surf.CreateIdAttr(VtValue(TfToken("UsdPreviewSurface")));
    surf.CreateInput(TfToken("diffuseColor"), SdfValueTypeNames->Color3f).Set(GfVec3f(0.2f, 0.8f, 0.2f));
    surf.CreateInput(TfToken("roughness"), SdfValueTypeNames->Float).Set(0.2f);
    surf.CreateInput(TfToken("emissiveColor"), SdfValueTypeNames->Color3f).Set(GfVec3f(0.05f, 0, 0));
    mat.CreateSurfaceOutput().ConnectToSource(surf.ConnectableAPI(), TfToken("surface"));
    UsdShadeMaterialBindingAPI::Apply(stage->GetPrimAtPath(SdfPath("/World/boxB"))).Bind(mat);

    UsdLuxDistantLight sun = UsdLuxDistantLight::Define(stage, SdfPath("/World/sun"));
    sun.CreateIntensityAttr(VtValue(3.0f));
    UsdGeomXformCommonAPI(sun.GetPrim()).SetRotate(GfVec3f(-50, 30, 0));
    UsdLuxSphereLight lamp = UsdLuxSphereLight::Define(stage, SdfPath("/World/lamp"));
    lamp.CreateIntensityAttr(VtValue(40.0f));
    lamp.CreateRadiusAttr(VtValue(0.3f));
    UsdGeomXformCommonAPI(lamp.GetPrim()).SetTranslate(GfVec3d(0, 2.5, 2));

    UsdGeomCamera cam = UsdGeomCamera::Define(stage, SdfPath("/World/cam"));
    cam.CreateFocalLengthAttr(VtValue(24.0f));
    UsdGeomXformCommonAPI(cam.GetPrim()).SetTranslate(GfVec3d(0.9, 1.5, 7));
    UsdGeomXformCommonAPI(cam.GetPrim()).SetRotate(GfVec3f(-18, 0, 0));
    return stage;
}

const char* formatName(HdFormat f)
{
    switch (f) {
    case HdFormatInvalid: return "Invalid";
    case HdFormatUNorm8: return "UNorm8";
    case HdFormatUNorm8Vec2: return "UNorm8Vec2";
    case HdFormatUNorm8Vec3: return "UNorm8Vec3";
    case HdFormatUNorm8Vec4: return "UNorm8Vec4";
    case HdFormatSNorm8: return "SNorm8";
    case HdFormatSNorm8Vec2: return "SNorm8Vec2";
    case HdFormatSNorm8Vec3: return "SNorm8Vec3";
    case HdFormatSNorm8Vec4: return "SNorm8Vec4";
    case HdFormatFloat16: return "Float16";
    case HdFormatFloat16Vec2: return "Float16Vec2";
    case HdFormatFloat16Vec3: return "Float16Vec3";
    case HdFormatFloat16Vec4: return "Float16Vec4";
    case HdFormatFloat32: return "Float32";
    case HdFormatFloat32Vec2: return "Float32Vec2";
    case HdFormatFloat32Vec3: return "Float32Vec3";
    case HdFormatFloat32Vec4: return "Float32Vec4";
    case HdFormatInt16: return "Int16";
    case HdFormatInt32: return "Int32";
    case HdFormatInt32Vec2: return "Int32Vec2";
    case HdFormatInt32Vec3: return "Int32Vec3";
    case HdFormatInt32Vec4: return "Int32Vec4";
    case HdFormatFloat32UInt8: return "Float32UInt8";
    default: return "Other";
    }
}

double component(const unsigned char* px, HdFormat comp, int c)
{
    switch (comp) {
    case HdFormatFloat32: return reinterpret_cast<const float*>(px)[c];
    case HdFormatFloat16: return float(reinterpret_cast<const GfHalf*>(px)[c]);
    case HdFormatInt32: return reinterpret_cast<const int32_t*>(px)[c];
    case HdFormatUNorm8: return px[c] / 255.0;
    case HdFormatSNorm8: return reinterpret_cast<const int8_t*>(px)[c] / 127.0;
    default: return std::numeric_limits<double>::quiet_NaN();
    }
}

struct Result {
    std::string name;
    bool setOk = false;
    bool buffer = false;
    bool data = false;
};

Result probe(const TfToken& plugin, int size, int samples, const TfTokenVector& aovs, Capture& capture,
             bool verbose)
{
    Result r;
    r.name = aovs.back().GetString();
    UsdStageRefPtr stage = makeStage();
    UsdImagingGLEngine::Parameters params;
    params.rendererPluginId = plugin;
    params.gpuEnabled = false;
    {
        std::unique_ptr<UsdImagingGLEngine> engine(new UsdImagingGLEngine(params));
        engine->SetEnablePresentation(false);
        engine->SetRendererSetting(TfToken("stageMetersPerUnit"), VtValue(1.0));
        engine->SetRendererSetting(TfToken("cycles:samples"), VtValue(samples));
        r.setOk = engine->SetRendererAovs(aovs);
        std::printf("  before first Render, GetAovRenderBuffer(%s) -> %s\n", aovs.back().GetText(),
                    engine->GetAovRenderBuffer(aovs.back()) ? "buffer" : "null");
        engine->SetCameraPath(SdfPath("/World/cam"));
        engine->SetRenderBufferSize(GfVec2i(size, size));
        engine->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), size, size)));
        UsdImagingGLRenderParams rp;
        rp.enableLighting = true;
        rp.enableSceneMaterials = true;
        const auto start = std::chrono::steady_clock::now();
        do {
            engine->Render(stage->GetPseudoRoot(), rp);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } while (!engine->IsConverged() &&
                 std::chrono::steady_clock::now() - start < std::chrono::seconds(30));
        const bool converged = engine->IsConverged();
        if (verbose) {
            std::printf("  GetRenderStats keys:");
            for (const auto& kv : engine->GetRenderStats()) {
                std::printf(" %s", kv.first.c_str());
            }
            std::printf("\n");
        }
        std::printf("  request:");
        for (const TfToken& a : aovs) {
            std::printf(" %s", a.GetText());
        }
        std::printf("\n  SetRendererAovs -> %s, converged=%d\n", r.setOk ? "true" : "false", int(converged));
        for (const TfToken& a : aovs) {
            HdRenderBuffer* b = engine->GetAovRenderBuffer(a);
            if (!b) {
                std::printf("  [%s] no render buffer\n", a.GetText());
                continue;
            }
            b->Resolve();
            const HdFormat f = b->GetFormat();
            const HdFormat comp = HdGetComponentFormat(f);
            const int n = int(HdGetComponentCount(f));
            const size_t stride = HdDataSizeOfFormat(f);
            const int w = int(b->GetWidth()), h = int(b->GetHeight());
            const unsigned char* data = static_cast<const unsigned char*>(b->Map());
            std::printf("  [%s] buffer %dx%d format=%s comps=%d converged=%d", a.GetText(), w, h,
                        formatName(f), n, int(b->IsConverged()));
            if (!data || f == HdFormatInvalid || n == 0) {
                std::printf(" mapped=%s\n", data ? "yes" : "null");
                b->Unmap();
                continue;
            }
            std::printf("\n");
            bool any = false;
            for (int c = 0; c < n; ++c) {
                double lo = std::numeric_limits<double>::infinity(), hi = -lo;
                size_t nonzero = 0, nonfinite = 0;
                std::vector<double> distinct;
                for (int i = 0; i < w * h; ++i) {
                    const double v = component(data + size_t(i) * stride, comp, c);
                    if (!std::isfinite(v)) {
                        ++nonfinite;
                        continue;
                    }
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                    nonzero += v != 0.0;
                    if (distinct.size() < 9 &&
                        std::find(distinct.begin(), distinct.end(), v) == distinct.end()) {
                        distinct.push_back(v);
                    }
                }
                any = any || lo != hi || nonzero > 0;
                const unsigned char* centre = data + size_t((h / 2) * w + w / 2) * stride;
                std::printf("    ch%d min=%g max=%g nonzero=%zu nonfinite=%zu centre=%g distinct%s=", c, lo, hi,
                            nonzero, nonfinite, component(centre, comp, c), distinct.size() >= 9 ? "(>=9)" : "");
                for (double d : distinct) {
                    std::printf("%g,", d);
                }
                std::printf("\n");
            }
            b->Unmap();
            if (a == aovs.back()) {
                r.buffer = true;
                r.data = any;
            }
        }
    }
    for (const std::string& line : capture.take()) {
        if (verbose || line.rfind("STATUS", 0) != 0) {
            std::printf("  diag %s\n", line.c_str());
        }
    }
    return r;
}

} // namespace

int main(int argc, char** argv)
{
    const TfToken plugin(argc > 1 ? argv[1] : "HdCyclesPlugin");
    const int size = argc > 2 ? std::atoi(argv[2]) : 64;
    const int samples = argc > 3 ? std::atoi(argv[3]) : 16;

    Capture capture;
    TfDiagnosticMgr::GetInstance().AddDelegate(&capture);

    TfTokenVector candidates;
    for (int i = 4; i < argc; ++i) {
        candidates.emplace_back(argv[i]);
    }
    if (candidates.empty()) {
        candidates = {
            HdAovTokens->color, HdAovTokens->depth, HdAovTokens->depthStencil, HdAovTokens->cameraDepth,
            HdAovTokens->primId, HdAovTokens->instanceId, HdAovTokens->elementId, HdAovTokens->edgeId,
            HdAovTokens->pointId, HdAovTokens->Peye, HdAovTokens->Neye, HdAovTokens->patchCoord,
            HdAovTokens->primitiveParam, HdAovTokens->normal,
            HdAovTokensMakePrimvar(TfToken("st")), HdAovTokensMakePrimvar(TfToken("displayColor")),
            HdAovTokensMakePrimvar(TfToken("probeScalar")),
            HdAovTokensMakeLpe(TfToken("C.*")), HdAovTokensMakeLpe(TfToken("C<RD>L")),
            HdAovTokensMakeLpe(TfToken("CD.*L")), HdAovTokensMakeLpe(TfToken("C<RS>.*L")),
            HdAovTokensMakeShader(TfToken("probeShader")),
            TfToken("diffuse_direct"), TfToken("glossy_direct"), TfToken("emission"), TfToken("ao"),
            TfToken("position"), TfToken("uv"), TfToken("mist"), TfToken("object_id"),
            TfToken("cryptomatte"), TfToken("cycles:diffuse_direct"), TfToken("Combined"),
            TfToken("myCustomAov"),
        };
    }

    std::printf("== delegate descriptors (%s)\n", plugin.GetText());
    {
        HdRendererPluginHandle ph = HdRendererPluginRegistry::GetInstance().GetOrCreateRendererPlugin(plugin);
        HdRenderDelegate* d = ph ? ph->CreateRenderDelegate() : nullptr;
        if (!d) {
            std::printf("could not create delegate %s\n", plugin.GetText());
            return 1;
        }
        for (const TfToken& c : candidates) {
            const HdAovDescriptor desc = d->GetDefaultAovDescriptor(c);
            std::printf("  %-28s format=%-12s clear=%s\n", c.GetText(), formatName(desc.format),
                        TfStringify(desc.clearValue).c_str());
        }
        ph->DeleteRenderDelegate(d);
    }
    for (const std::string& line : capture.take()) {
        std::printf("  diag %s\n", line.c_str());
    }

    {
        UsdImagingGLEngine::Parameters params;
        params.rendererPluginId = plugin;
        params.gpuEnabled = false;
        UsdImagingGLEngine engine(params);
        std::printf("== GetRendererAovs:");
        for (const TfToken& t : engine.GetRendererAovs()) {
            std::printf(" %s", t.GetText());
        }
        std::printf("\n");
    }
    capture.take();

    std::vector<Result> results;
    for (const TfToken& c : candidates) {
        std::printf("== probe %s\n", c.GetText());
        TfTokenVector aovs = { HdAovTokens->color };
        if (c != HdAovTokens->color) {
            aovs.push_back(c);
        }
        results.push_back(probe(plugin, size, samples, aovs, capture, results.empty()));
    }

    TfTokenVector all;
    for (const Result& r : results) {
        if (r.data) {
            all.emplace_back(r.name);
        }
    }
    if (all.size() > 1) {
        std::printf("== probe all-with-data together\n");
        probe(plugin, size, samples, all, capture, false);
    }

    std::printf("== summary (name setOk buffer data)\n");
    for (const Result& r : results) {
        std::printf("  %-28s %d %d %d\n", r.name.c_str(), int(r.setOk), int(r.buffer), int(r.data));
    }
    TfDiagnosticMgr::GetInstance().RemoveDelegate(&capture);
    return 0;
}
