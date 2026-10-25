// Usage: m19spike <file.usda> <outDir> [--force-version M.m]
// Requests a GL 4.5 compatibility default format, docks a compat widget drawing immediate-mode GL
// beside a Storm (UsdImagingGLEngine, HgiGL) widget in one QMainWindow, renders the stage, then
// writes storm.png, compat.png and window.png to outDir and prints one REPORT block. Exit 0 only
// if both widgets drew and the context is >= 4.5 compatibility.
#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/rect2i.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hdx/tokens.h>
#include <pxr/imaging/hgi/tokens.h>
#include <pxr/pxr.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/bboxCache.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QImage>
#include <QMainWindow>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLFunctions_4_5_Compatibility>
#include <QOpenGLVersionFunctionsFactory>
#include <QOpenGLWidget>
#include <QSurfaceFormat>
#include <QTimer>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {
const QColor kBackground(51, 51, 51);

QString
describe(const QOpenGLContext* context)
{
    if (!context) {
        return QStringLiteral("no context");
    }
    const QSurfaceFormat f = context->format();
    const char* profile = (f.profile() == QSurfaceFormat::CompatibilityProfile) ? "compatibility"
        : (f.profile() == QSurfaceFormat::CoreProfile)                          ? "core"
                                                                                : "none";

    return QStringLiteral("%1.%2 %3").arg(f.majorVersion()).arg(f.minorVersion()).arg(QString::fromUtf8(profile));
}

// Pixels that differ from the clear colour by more than a little.
int
countDrawn(const QImage& image)
{
    int n = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor c = image.pixelColor(x, y);
            if (std::abs(c.red() - kBackground.red()) + std::abs(c.green() - kBackground.green()) + std::abs(c.blue() - kBackground.blue()) > 12) {
                ++n;
            }
        }
    }

    return n;
}

struct GlInfo {
    QString context;
    QString version;
    QString renderer;
    bool hasCompat45Functions = false;
    void read(QOpenGLContext* ctx)
    {
        context = describe(ctx);
        QOpenGLFunctions* f = ctx->functions();
        version = QString::fromUtf8((const char*)f->glGetString(GL_VERSION));
        renderer = QString::fromUtf8((const char*)f->glGetString(GL_RENDERER));
        hasCompat45Functions = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_4_5_Compatibility>(ctx) != nullptr;
    }
};
} // namespace

class CompatWidget : public QOpenGLWidget {
public:
    GlInfo info;
    int paints = 0;

protected:
    void initializeGL() override
    {
        info.read(context());
    }

    void paintGL() override
    {
        ++paints;
        auto* gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_4_5_Compatibility>(context());
        QOpenGLFunctions* f = context()->functions();
        f->glClearColor(kBackground.redF(), kBackground.greenF(), kBackground.blueF(), 1.f);
        f->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (!gl) {
            return;
        }
        // The fixed-function path the viewer overlays and OFX interacts use.
        gl->glMatrixMode(GL_PROJECTION);
        gl->glLoadIdentity();
        gl->glOrtho(-1, 1, -1, 1, -1, 1);
        gl->glMatrixMode(GL_MODELVIEW);
        gl->glLoadIdentity();
        gl->glBegin(GL_TRIANGLES);
        gl->glColor3f(1.f, 0.f, 0.f);
        gl->glVertex2f(-0.8f, -0.8f);
        gl->glColor3f(0.f, 1.f, 0.f);
        gl->glVertex2f(0.8f, -0.8f);
        gl->glColor3f(0.f, 0.f, 1.f);
        gl->glVertex2f(0.f, 0.8f);
        gl->glEnd();
        gl->glLineWidth(2.f);
        gl->glBegin(GL_LINE_LOOP);
        gl->glColor3f(1.f, 1.f, 0.f);
        gl->glVertex2f(-0.9f, -0.9f);
        gl->glVertex2f(0.9f, -0.9f);
        gl->glVertex2f(0.9f, 0.9f);
        gl->glVertex2f(-0.9f, 0.9f);
        gl->glEnd();
    }
};

class StormWidget : public QOpenGLWidget {
public:
    explicit StormWidget(const std::string& path)
        : _path(path)
    {
    }

    ~StormWidget() override
    {
        makeCurrent();
        _engine.reset();
        doneCurrent();
    }

    GlInfo info;
    QString engineError;
    QString rendererName;
    int paints = 0;

protected:
    void initializeGL() override
    {
        info.read(context());
        _stage = UsdStage::Open(_path);
        if (!_stage) {
            engineError = QStringLiteral("could not open the stage");
            return;
        }
        UsdImagingGLEngine::Parameters params;
        _engine = std::make_unique<UsdImagingGLEngine>(params);
        rendererName = QString::fromStdString(UsdImagingGLEngine::GetRendererDisplayName(_engine->GetCurrentRendererId()));
    }

    void paintGL() override
    {
        ++paints;
        QOpenGLFunctions* f = context()->functions();
        f->glClearColor(kBackground.redF(), kBackground.greenF(), kBackground.blueF(), 1.f);
        f->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (!_engine || !_stage) {
            return;
        }
        const int w = int(width() * devicePixelRatio());
        const int h = int(height() * devicePixelRatio());

        UsdGeomBBoxCache bboxes(UsdTimeCode::Default(), { UsdGeomTokens->default_ });
        const GfRange3d range = bboxes.ComputeWorldBound(_stage->GetPseudoRoot()).ComputeAlignedRange();
        const GfVec3d center = range.GetMidpoint();
        const double radius = range.GetSize().GetLength() * 0.5;
        const bool zUp = UsdGeomGetStageUpAxis(_stage) == UsdGeomTokens->z;
        const GfVec3d up = zUp ? GfVec3d(0, 0, 1) : GfVec3d(0, 1, 0);
        const GfVec3d away = zUp ? GfVec3d(0.6, -1, 0.5) : GfVec3d(0.6, 0.5, 1);
        const GfVec3d eye = center + away.GetNormalized() * radius * 3.;
        GfMatrix4d view;
        view.SetLookAt(eye, center, up);
        GfFrustum frustum;
        frustum.SetPerspective(35., double(w) / double(std::max(h, 1)), radius * 0.05, radius * 20.);

        _engine->SetRenderBufferSize(GfVec2i(w, h));
        _engine->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), w, h)));
        _engine->SetCameraState(view, frustum.ComputeProjectionMatrix());
        // Presents into the widget's FBO, as usdview does with its QOpenGLWidget.
        _engine->SetPresentationOutput(HgiTokens->OpenGL, VtValue(uint32_t(defaultFramebufferObject())));

        UsdImagingGLRenderParams params;
        params.frame = UsdTimeCode(1.);
        params.drawMode = UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH;
        params.enableLighting = true;
        params.clearColor = GfVec4f(kBackground.redF(), kBackground.greenF(), kBackground.blueF(), 1.f);
        params.colorCorrectionMode = HdxColorCorrectionTokens->sRGB;
        // Storm may need more than one pass before the result is final.
        for (int i = 0; i < 8; ++i) {
            _engine->Render(_stage->GetPseudoRoot(), params);
            if (_engine->IsConverged()) {
                break;
            }
        }
    }

private:
    std::string _path;
    UsdStageRefPtr _stage;
    std::unique_ptr<UsdImagingGLEngine> _engine;
};

int
main(int argc,
     char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <file.usda> <outDir> [--force-version M.m]\n", argv[0]);
        return 2;
    }
    int major = 4, minor = 5;
    if (argc >= 5 && std::string(argv[3]) == "--force-version") {
        std::sscanf(argv[4], "%d.%d", &major, &minor);
    }
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(major, minor);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    QSurfaceFormat::setDefaultFormat(format);
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);

    const QString outDir = QString::fromLocal8Bit(argv[2]);
    QDir().mkpath(outDir);

    QMainWindow window;
    auto* storm = new StormWidget(argv[1]);
    window.setCentralWidget(storm);
    auto* dock = new QDockWidget(QStringLiteral("Compat"), &window);
    auto* compat = new CompatWidget;
    compat->setMinimumSize(320, 240);
    dock->setWidget(compat);
    window.addDockWidget(Qt::RightDockWidgetArea, dock);
    window.resize(1100, 600);
    window.show();

    int exitCode = 1;
    QTimer::singleShot(2000, [&]() {
        const QImage stormImage = storm->grabFramebuffer();
        const QImage compatImage = compat->grabFramebuffer();
        stormImage.save(outDir + QStringLiteral("/storm.png"));
        compatImage.save(outDir + QStringLiteral("/compat.png"));
        window.grab().toImage().save(outDir + QStringLiteral("/window.png"));
        const int stormDrawn = countDrawn(stormImage);
        const int compatDrawn = countDrawn(compatImage);
        QOpenGLContext* ctx = storm->context();
        const QSurfaceFormat got = ctx ? ctx->format() : QSurfaceFormat();
        const bool is45Compat = (got.majorVersion() > 4 || (got.majorVersion() == 4 && got.minorVersion() >= 5))
            && got.profile() == QSurfaceFormat::CompatibilityProfile;
        std::printf("REPORT\n");
        std::printf("requested: %d.%d compatibility\n", major, minor);
        std::printf("storm context: %s | GL_VERSION %s | GL_RENDERER %s | 4.5 compat functions: %s\n",
                    qPrintable(storm->info.context), qPrintable(storm->info.version), qPrintable(storm->info.renderer),
                    storm->info.hasCompat45Functions ? "yes" : "no");
        std::printf("compat context: %s | GL_VERSION %s | 4.5 compat functions: %s\n",
                    qPrintable(compat->info.context), qPrintable(compat->info.version), compat->info.hasCompat45Functions ? "yes" : "no");
        std::printf("contexts share: %s\n", (storm->context() && compat->context() && QOpenGLContext::areSharing(storm->context(), compat->context())) ? "yes" : "no");
        std::printf("hydra renderer: %s %s\n", qPrintable(storm->rendererName), qPrintable(storm->engineError));
        std::printf("storm drew: %s (%d px, %d paints)\n", stormDrawn > 0 ? "yes" : "no", stormDrawn, storm->paints);
        std::printf("compat drew: %s (%d px, %d paints)\n", compatDrawn > 0 ? "yes" : "no", compatDrawn, compat->paints);
        exitCode = (stormDrawn > 0 && compatDrawn > 0 && is45Compat) ? 0 : 1;
        std::printf("RESULT %s\n", exitCode == 0 ? "PASS" : "FAIL");
        std::fflush(stdout);
        app.quit();
    });
    app.exec();

    return exitCode;
}
