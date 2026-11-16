// Probes whether AOVs declared as UsdRender prims (RenderSettings -> RenderProduct -> RenderVar)
// reach a Hydra delegate through UsdImagingGLEngine, the path Natron's USDRender takes
// (gpuEnabled = false, presentation off), and what a host-side mapping of those prims onto
// SetRendererAovs gets back.
//
// Usage: rs_probe [pluginId] [size] [samples]
//
// Runs, each on a fresh engine:
//   A  prims only: stage metadata names the settings prim; no SetRendererAovs, no SetCameraPath.
//   B  as A, plus SetActiveRenderSettingsPrimPath.
//   C  reference: SetCameraPath to the settings camera, color only, no settings prim at all.
//   D  host mapping: UsdRenderComputeSpec flattens the prims; each RenderVar becomes an AOV token
//      (raw -> sourceName, primvar -> "primvars:" + sourceName, lpe -> "lpe:" + sourceName) and
//      goes to SetRendererAovs with the settings camera and product resolution.

#include <pxr/pxr.h>
#include <pxr/base/gf/half.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/diagnosticMgr.h>
#include <pxr/base/tf/error.h>
#include <pxr/base/tf/status.h>
#include <pxr/base/tf/warning.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/rendererPlugin.h>
#include <pxr/imaging/hd/rendererPluginHandle.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
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
#include <pxr/usd/usdRender/product.h>
#include <pxr/usd/usdRender/settings.h>
#include <pxr/usd/usdRender/spec.h>
#include <pxr/usd/usdRender/tokens.h>
#include <pxr/usd/usdRender/var.h>
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

    void flush()
    {
        std::vector<std::string> lines;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            lines.swap(_lines);
        }
        for (const std::string& l : lines) {
            if (l.rfind("STATUS", 0) != 0) {
                std::printf("  diag %s\n", l.c_str());
            }
        }
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

const SdfPath kSettings("/Render/settings");
const SdfPath kProduct("/Render/product");
const SdfPath kCamera("/World/cam");
const GfVec2i kResolution(96, 48);

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

void addVar(const UsdStageRefPtr& stage, const char* name, const TfToken& dataType, const std::string& sourceName,
            const TfToken& sourceType, std::vector<SdfPath>* vars)
{
    const SdfPath path = kProduct.AppendChild(TfToken(name));
    UsdRenderVar var = UsdRenderVar::Define(stage, path);
    var.CreateDataTypeAttr().Set(dataType);
    var.CreateSourceNameAttr().Set(sourceName);
    var.CreateSourceTypeAttr().Set(sourceType);
    // Per-var metadata the way Karma and hdPrman carry it, plus a host-owned namespace: both must
    // survive UsdRenderComputeSpec for a manifest or a layer name to ride on a RenderVar.
    var.GetPrim()
        .CreateAttribute(TfToken("driver:parameters:aov:name"), SdfValueTypeNames->String, true)
        .Set(std::string(name));
    var.GetPrim()
        .CreateAttribute(TfToken("natron:layer"), SdfValueTypeNames->String, true)
        .Set(std::string(name));
    vars->push_back(path);
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
    mat.CreateSurfaceOutput().ConnectToSource(surf.ConnectableAPI(), TfToken("surface"));
    UsdShadeMaterialBindingAPI::Apply(stage->GetPrimAtPath(SdfPath("/World/boxB"))).Bind(mat);

    UsdLuxDistantLight sun = UsdLuxDistantLight::Define(stage, SdfPath("/World/sun"));
    sun.CreateIntensityAttr(VtValue(3.0f));
    UsdGeomXformCommonAPI(sun.GetPrim()).SetRotate(GfVec3f(-50, 30, 0));
    UsdLuxSphereLight lamp = UsdLuxSphereLight::Define(stage, SdfPath("/World/lamp"));
    lamp.CreateIntensityAttr(VtValue(40.0f));
    lamp.CreateRadiusAttr(VtValue(0.3f));
    UsdGeomXformCommonAPI(lamp.GetPrim()).SetTranslate(GfVec3d(0, 2.5, 2));

    UsdGeomCamera cam = UsdGeomCamera::Define(stage, kCamera);
    cam.CreateFocalLengthAttr(VtValue(24.0f));
    UsdGeomXformCommonAPI(cam.GetPrim()).SetTranslate(GfVec3d(0.9, 1.5, 7));
    UsdGeomXformCommonAPI(cam.GetPrim()).SetRotate(GfVec3f(-18, 0, 0));

    std::vector<SdfPath> vars;
    addVar(stage, "color", TfToken("color4f"), "color", UsdRenderTokens->raw, &vars);
    addVar(stage, "depth", TfToken("float"), "depth", UsdRenderTokens->raw, &vars);
    addVar(stage, "normal", TfToken("normal3f"), "normal", UsdRenderTokens->raw, &vars);
    addVar(stage, "probeScalar", TfToken("float"), "probeScalar", UsdRenderTokens->primvar, &vars);
    addVar(stage, "diffuseLpe", TfToken("color3f"), "C<RD>L", UsdRenderTokens->lpe, &vars);
    addVar(stage, "cyclesDiffuseDirect", TfToken("color3f"), "cycles:diffuse_direct", UsdRenderTokens->raw, &vars);
    addVar(stage, "cyclesBare", TfToken("color3f"), "diffuse_direct", UsdRenderTokens->raw, &vars);

    UsdRenderProduct product = UsdRenderProduct::Define(stage, kProduct);
    product.CreateProductNameAttr().Set(TfToken("out/product.exr"));
    product.CreateOrderedVarsRel().SetTargets(vars);

    UsdRenderSettings settings = UsdRenderSettings::Define(stage, kSettings);
    settings.CreateCameraRel().SetTargets({ kCamera });
    settings.CreateResolutionAttr().Set(kResolution);
    settings.CreateProductsRel().SetTargets({ kProduct });
    settings.GetPrim().CreateAttribute(TfToken("cycles:samples"), SdfValueTypeNames->Int, true).Set(8);

    stage->GetRootLayer()->SetField(SdfPath::AbsoluteRootPath(), UsdRenderTokens->renderSettingsPrimPath,
                                    kSettings.GetString());
    return stage;
}

double component(const unsigned char* px, HdFormat comp, int c)
{
    switch (comp) {
    case HdFormatFloat32: return reinterpret_cast<const float*>(px)[c];
    case HdFormatFloat16: return float(reinterpret_cast<const GfHalf*>(px)[c]);
    case HdFormatInt32: return reinterpret_cast<const int32_t*>(px)[c];
    case HdFormatUNorm8: return px[c] / 255.0;
    default: return std::numeric_limits<double>::quiet_NaN();
    }
}

void report(UsdImagingGLEngine& engine, const TfToken& name)
{
    HdRenderBuffer* b = engine.GetAovRenderBuffer(name);
    if (!b) {
        std::printf("  [%s] null buffer\n", name.GetText());
        return;
    }
    b->Resolve();
    const HdFormat f = b->GetFormat();
    const HdFormat comp = HdGetComponentFormat(f);
    const int n = int(HdGetComponentCount(f));
    const size_t stride = HdDataSizeOfFormat(f);
    const int w = int(b->GetWidth()), h = int(b->GetHeight());
    const unsigned char* data = static_cast<const unsigned char*>(b->Map());
    std::printf("  [%s] %dx%d format=%d comps=%d", name.GetText(), w, h, int(f), n);
    if (data && f != HdFormatInvalid && n > 0) {
        for (int c = 0; c < std::min(n, 3); ++c) {
            double sum = 0, lo = std::numeric_limits<double>::infinity(), hi = -lo;
            for (int i = 0; i < w * h; ++i) {
                const double v = component(data + size_t(i) * stride, comp, c);
                if (std::isfinite(v)) {
                    sum += v;
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
            }
            std::printf(" ch%d[mean=%.4g min=%.4g max=%.4g]", c, sum / std::max(1, w * h), lo, hi);
        }
    }
    std::printf("\n");
    b->Unmap();
}

TfToken aovForVar(const UsdRenderSpec::RenderVar& v)
{
    if (v.sourceType == UsdRenderTokens->primvar) {
        return HdAovTokensMakePrimvar(TfToken(v.sourceName));
    }
    if (v.sourceType == UsdRenderTokens->lpe) {
        return HdAovTokensMakeLpe(TfToken(v.sourceName));
    }
    return TfToken(v.sourceName);
}

enum class Run { PrimsOnly, PrimsActive, Reference, HostMapping };

void render(const TfToken& plugin, int size, int samples, Run run, Capture& capture)
{
    UsdStageRefPtr stage = makeStage();
    UsdImagingGLEngine::Parameters params;
    params.rendererPluginId = plugin;
    params.gpuEnabled = false;
    UsdImagingGLEngine engine(params);
    engine.SetEnablePresentation(false);
    engine.SetRendererSetting(TfToken("stageMetersPerUnit"), VtValue(1.0));
    engine.SetRendererSetting(TfToken("cycles:samples"), VtValue(samples));

    if (run == Run::Reference) {
        stage->GetRootLayer()->EraseField(SdfPath::AbsoluteRootPath(), UsdRenderTokens->renderSettingsPrimPath);
        stage->RemovePrim(SdfPath("/Render"));
    }
    if (run == Run::PrimsActive) {
        engine.SetActiveRenderSettingsPrimPath(kSettings);
    }

    TfTokenVector probeNames = { HdAovTokens->color, HdAovTokens->depth, HdAovTokens->normal,
                                 HdAovTokens->primId,
                                 HdAovTokensMakePrimvar(TfToken("probeScalar")),
                                 HdAovTokensMakeLpe(TfToken("C<RD>L")),
                                 TfToken("cycles:diffuse_direct"), TfToken("diffuse_direct"),
                                 TfToken("probeScalar"), TfToken("diffuseLpe"),
                                 TfToken(kProduct.AppendChild(TfToken("depth")).GetString()) };

    GfVec2i res(size, size);
    if (run == Run::HostMapping) {
        const UsdRenderSpec spec =
            UsdRenderComputeSpec(UsdRenderSettings(stage->GetPrimAtPath(kSettings)),
                                 { TfToken("cycles"), TfToken("driver"), TfToken("natron") });
        std::printf("  spec: %zu products, %zu vars, settings namespaced keys:", spec.products.size(),
                    spec.renderVars.size());
        for (const auto& kv : spec.namespacedSettings) {
            std::printf(" %s", kv.first.c_str());
        }
        std::printf("\n");
        TfTokenVector aovs;
        for (const UsdRenderSpec::Product& p : spec.products) {
            std::printf("  product %s name=%s camera=%s res=%dx%d vars=%zu\n", p.renderProductPath.GetText(),
                        p.name.GetText(), p.cameraPath.GetText(), p.resolution[0], p.resolution[1],
                        p.renderVarIndices.size());
            res = p.resolution;
            engine.SetCameraPath(p.cameraPath);
            for (size_t i : p.renderVarIndices) {
                const UsdRenderSpec::RenderVar& v = spec.renderVars[i];
                const TfToken aov = aovForVar(v);
                std::printf("    var %s dataType=%s sourceType=%s sourceName=%s -> aov %s; namespaced:",
                            v.renderVarPath.GetName().c_str(), v.dataType.GetText(), v.sourceType.GetText(),
                            v.sourceName.c_str(), aov.GetText());
                for (const auto& kv : v.namespacedSettings) {
                    std::printf(" %s=%s", kv.first.c_str(), TfStringify(kv.second).c_str());
                }
                std::printf("\n");
                aovs.push_back(aov);
            }
        }
        std::printf("  SetRendererAovs -> %d\n", int(engine.SetRendererAovs(aovs)));
        probeNames = aovs;
    } else if (run == Run::Reference) {
        engine.SetCameraPath(kCamera);
        engine.SetRendererAovs({ HdAovTokens->color });
    }

    engine.SetRenderBufferSize(res);
    engine.SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), res[0], res[1])));
    UsdImagingGLRenderParams rp;
    rp.enableLighting = true;
    rp.enableSceneMaterials = true;
    const auto start = std::chrono::steady_clock::now();
    do {
        engine.Render(stage->GetPseudoRoot(), rp);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (!engine.IsConverged() && std::chrono::steady_clock::now() - start < std::chrono::seconds(30));
    std::printf("  converged=%d GetActiveRenderSettingsPrimPath=<%s> GetActiveRenderPassPrimPath=<%s>\n",
                int(engine.IsConverged()), engine.GetActiveRenderSettingsPrimPath().GetText(),
                engine.GetActiveRenderPassPrimPath().GetText());
    for (const TfToken& n : probeNames) {
        report(engine, n);
    }
    capture.flush();
}

} // namespace

int main(int argc, char** argv)
{
    const TfToken plugin(argc > 1 ? argv[1] : "HdCyclesPlugin");
    const int size = argc > 2 ? std::atoi(argv[2]) : 64;
    const int samples = argc > 3 ? std::atoi(argv[3]) : 16;

    Capture capture;
    TfDiagnosticMgr::GetInstance().AddDelegate(&capture);

    makeStage()->Export("out/stage.usda");

    std::printf("== delegate prim types (%s)\n", plugin.GetText());
    {
        HdRendererPluginHandle ph = HdRendererPluginRegistry::GetInstance().GetOrCreateRendererPlugin(plugin);
        HdRenderDelegate* d = ph ? ph->CreateRenderDelegate() : nullptr;
        if (!d) {
            std::printf("could not create delegate %s\n", plugin.GetText());
            return 1;
        }
        std::printf("  Bprims:");
        for (const TfToken& t : d->GetSupportedBprimTypes()) {
            std::printf(" %s", t.GetText());
        }
        std::printf("\n  supports renderSettings Bprim: %d\n",
                    int(std::find(d->GetSupportedBprimTypes().begin(), d->GetSupportedBprimTypes().end(),
                                  HdPrimTypeTokens->renderSettings) != d->GetSupportedBprimTypes().end()));
        ph->DeleteRenderDelegate(d);
    }
    capture.flush();

    std::printf("== GetAvailableRenderSettingsPrimPaths:");
    {
        UsdStageRefPtr stage = makeStage();
        for (const SdfPath& p : UsdImagingGLEngine::GetAvailableRenderSettingsPrimPaths(stage->GetPseudoRoot())) {
            std::printf(" %s", p.GetText());
        }
        std::printf("\n");
    }

    std::printf("== A: prims only (stage metadata), no SetRendererAovs, no SetCameraPath\n");
    render(plugin, size, samples, Run::PrimsOnly, capture);
    std::printf("== B: prims + SetActiveRenderSettingsPrimPath, no SetRendererAovs, no SetCameraPath\n");
    render(plugin, size, samples, Run::PrimsActive, capture);
    std::printf("== C: reference, no settings prim, SetCameraPath + color\n");
    render(plugin, size, samples, Run::Reference, capture);
    std::printf("== D: host mapping of RenderVars onto SetRendererAovs\n");
    render(plugin, size, samples, Run::HostMapping, capture);

    TfDiagnosticMgr::GetInstance().RemoveDelegate(&capture);
    return 0;
}
