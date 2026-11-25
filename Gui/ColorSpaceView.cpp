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

#include "ColorSpaceView.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "Global/GLIncludes.h"

#include <QColor>
#include <QCoreApplication>
#include <QFont>
#include <QKeyEvent>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QPointer>
#include <QTimer>
#include <QVector3D>
#include <QVector4D>
#include <QWheelEvent>
#include <QWidget>

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Knob.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobColorWarpPins.h"
#include "Engine/Node.h"
#include "Engine/NodeColorSampler.h"
#include "Engine/Nodes/Color/ColorWarp/ColorWarp.h"
#include "Engine/Nodes/Color/ColorWarp/ColorWarpSpace.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/RectI.h"
#include "Engine/Settings.h"
#include "Engine/TimeLine.h"

#include "Gui/ActionShortcuts.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/TextRenderer.h"

NATRON_NAMESPACE_ENTER

namespace {

// The cloud is drawn from a render whose longest side is at most this, and holds at most
// kPointBudget pixels of it.
constexpr int kProxyLongestSide = 512;
constexpr int kPointBudget = 20000;
constexpr double kFieldOfViewDegrees = 35.;
constexpr double kPickRadius = 8.;
constexpr double kDefaultYaw = 0.7;
constexpr double kDefaultPitch = 0.35;
constexpr double kMaxPitch = 1.55;
constexpr double kHandleShare = 0.18;
// How often edits upstream of the cloud are looked for; a burst of them within it costs one fetch.
constexpr int kEditPollMs = 150;

GuiApplicationManager*
guiManager()
{
    return dynamic_cast<GuiApplicationManager*>(AppManager::instance());
}

bool
matchesMouseBinding(const char* actionID,
                    Qt::KeyboardModifiers modifiers,
                    Qt::MouseButton button)
{
    if (GuiApplicationManager* manager = guiManager()) {
        return manager->matchesMouseShortcut(kShortcutGroupViewer3D, actionID, modifiers, button);
    }
    MouseAction action;
    if (std::string(actionID) == kShortcutIDMouseViewer3DDollyWheel) {
        action.gesture = eMouseGestureWheel;
    }
    std::vector<MouseBinding> bindings;
    getViewer3DNavigationPresetBindings((int)Settings::eViewer3DNavigationNuke, QString::fromUtf8(actionID), &bindings);
    action.setBindings(bindings);

    return action.matches(modifiers, button);
}

// A fixed hash, so the pixel each stratum contributes is the same on every fetch.
std::uint32_t
mixHash(std::uint32_t a,
        std::uint32_t b)
{
    std::uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u + (a << 6) + (a >> 2));

    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;

    return h;
}

template <typename PIX, int maxValue>
void
readPixels(const Image& image,
           const std::vector<std::pair<int, int>>& coords,
           const int index[4],
           std::vector<float>* rgba)
{
    Image::ReadAccess access(&image);

    rgba->assign(coords.size() * 4, 0.f);
    for (std::size_t i = 0; i < coords.size(); ++i) {
        const PIX* pix = reinterpret_cast<const PIX*>(access.pixelAt(coords[i].first, coords[i].second));
        float* out = &(*rgba)[i * 4];
        out[3] = 1.f;
        if (!pix) {
            continue;
        }
        for (int c = 0; c < 4; ++c) {
            if (index[c] >= 0) {
                out[c] = (float)(pix[index[c]] * (1. / maxValue));
            }
        }
    }
}

bool
readImagePixels(const Image& image,
                const std::vector<std::pair<int, int>>& coords,
                const int index[4],
                std::vector<float>* rgba)
{
    switch (image.getBitDepth()) {
    case eImageBitDepthByte:
        readPixels<unsigned char, 255>(image, coords, index, rgba);

        return true;
    case eImageBitDepthShort:
        readPixels<unsigned short, 65535>(image, coords, index, rgba);

        return true;
    case eImageBitDepthFloat:
        readPixels<float, 1>(image, coords, index, rgba);

        return true;
    default:

        return false;
    }
}

// The index in image's pixels of the channel called name, else of fallback when image has that
// many channels, else -1.
int
channelIndexIn(const Image& image,
               const std::string& name,
               int fallback)
{
    const std::vector<std::string>& present = image.getComponents().getChannels();
    const std::size_t nComps = std::min<std::size_t>(present.size(), image.getComponentsCount());

    for (std::size_t i = 0; i < nComps; ++i) {
        if (present[i] == name) {
            return (int)i;
        }
    }

    return ((fallback >= 0) && ((std::size_t)fallback < image.getComponentsCount())) ? fallback : -1;
}

void
axisNames(ColorWarpSpaceEnum space,
          const char* names[3])
{
    switch (space) {
    case eColorWarpSpaceOklabLog:
    case eColorWarpSpaceOklab:
        names[0] = "L";
        names[1] = "a";
        names[2] = "b";
        break;
    case eColorWarpSpaceXyz:
        names[0] = "X";
        names[1] = "Y";
        names[2] = "Z";
        break;
    case eColorWarpSpaceLogRgb:
        names[0] = "log R";
        names[1] = "log G";
        names[2] = "log B";
        break;
    case eColorWarpSpaceLinearRgb:
        names[0] = "R";
        names[1] = "G";
        names[2] = "B";
        break;
    }
}

QVector3D
toVec(const double v[3])
{
    return QVector3D((float)v[0], (float)v[1], (float)v[2]);
}

double
segmentDistance(const QPointF& p,
                const QPointF& a,
                const QPointF& b)
{
    const QPointF ab = b - a;
    const double len2 = ab.x() * ab.x() + ab.y() * ab.y();
    double t = 0.;

    if (len2 > 0.) {
        t = std::clamp(((p.x() - a.x()) * ab.x() + (p.y() - a.y()) * ab.y()) / len2, 0., 1.);
    }
    const QPointF d = p - (a + ab * t);

    return std::sqrt(d.x() * d.x() + d.y() * d.y());
}

double
pointDistance(const QPointF& a,
              const QPointF& b)
{
    const QPointF d = a - b;

    return std::sqrt(d.x() * d.x() + d.y() * d.y());
}

struct CloudRequest {
    std::uint64_t serial = 0;
    NodePtr node;
    double time = 0.;
    ViewIdx view;
    // The channel colours are divided by, divisorChannel of divisorLayer, or none when -1.
    ImageLayerDesc divisorLayer;
    int divisorChannel = -1;
    ColorSpaceView::DisplayConverter display;
};

struct CloudResult {
    std::uint64_t serial = 0;
    bool ok = false;
    // Unpremultiplied working-space RGB and display RGB, three floats per point.
    std::vector<float> rgb;
    std::vector<float> display;
};

bool
fetchCloud(const CloudRequest& r,
           const AbortableRenderInfoPtr& abortInfo,
           CloudResult* out)
{
    RectI format, window;

    if (!getNodeImageWindow(r.node, r.time, r.view, &format, &window) || window.isNull()) {
        return false;
    }
    unsigned int level = 0;
    while ((level < 16) && ((std::max(window.width(), window.height()) >> level) > kProxyLongestSide)) {
        ++level;
    }
    const RectI pixelRect = window.downscalePowerOfTwoSmallestEnclosing(level);
    RectI rendered;
    const ImagePtr image = renderNodeImage(r.node, r.time, r.view, level, pixelRect, ImageLayerDesc::getRGBAComponents(), &rendered, abortInfo);
    if (!image || abortInfo->isAborted()) {
        return false;
    }
    const RectI rect = rendered.intersect(image->getBounds());
    if (rect.isNull()) {
        out->ok = true;

        return true;
    }

    const std::vector<std::string>& present = image->getComponents().getChannels();
    const std::size_t nComps = std::min<std::size_t>(present.size(), image->getComponentsCount());
    static const char* const wanted[4] = { "R", "G", "B", "A" };
    int index[4] = { -1, -1, -1, -1 };
    for (int c = 0; c < 4; ++c) {
        for (std::size_t i = 0; i < nComps; ++i) {
            if (present[i] == wanted[c]) {
                index[c] = (int)i;
                break;
            }
        }
    }

    // A jittered pixel per cell of a grid with about kPointBudget cells over the image covers it
    // evenly, and the fixed hash keeps the pick stable between fetches.
    const int w = rect.width();
    const int h = rect.height();
    std::vector<std::pair<int, int>> coords;
    if ((std::int64_t)w * h <= kPointBudget) {
        coords.reserve((std::size_t)w * h);
        for (int y = rect.y1; y < rect.y2; ++y) {
            for (int x = rect.x1; x < rect.x2; ++x) {
                coords.emplace_back(x, y);
            }
        }
    } else {
        const double cell = std::sqrt((double)w * h / kPointBudget);
        const int nx = std::max(1, (int)std::ceil(w / cell));
        const int ny = std::max(1, (int)std::ceil(h / cell));
        coords.reserve((std::size_t)nx * ny);
        for (int cy = 0; cy < ny; ++cy) {
            const int y0 = rect.y1 + (int)(cy * cell);
            const int y1 = std::min(rect.y2, rect.y1 + (int)((cy + 1) * cell));
            for (int cx = 0; cx < nx; ++cx) {
                const int x0 = rect.x1 + (int)(cx * cell);
                const int x1 = std::min(rect.x2, rect.x1 + (int)((cx + 1) * cell));
                if ((x1 <= x0) || (y1 <= y0)) {
                    continue;
                }
                const std::uint32_t hash = mixHash((std::uint32_t)cx, (std::uint32_t)cy);
                coords.emplace_back(x0 + (int)(hash % (std::uint32_t)(x1 - x0)), y0 + (int)((hash >> 16) % (std::uint32_t)(y1 - y0)));
            }
        }
    }

    std::vector<float> rgba;
    if (!readImagePixels(*image, coords, index, &rgba) || abortInfo->isAborted()) {
        return false;
    }

    const std::size_t n = coords.size();
    // A divisor in a layer other than RGBA is rendered on its own, over the same pixels.
    std::vector<float> divisors;
    const std::vector<std::string>& divisorChannels = r.divisorLayer.getChannels();
    if ((r.divisorChannel >= 0) && (r.divisorChannel < (int)divisorChannels.size())) {
        ImagePtr divisorImage = image;
        if (!(r.divisorLayer == ImageLayerDesc::getRGBAComponents())) {
            RectI divisorRendered;
            divisorImage = renderNodeImage(r.node, r.time, r.view, level, pixelRect, r.divisorLayer, &divisorRendered, abortInfo);
            if (abortInfo->isAborted()) {
                return false;
            }
        }
        const int channel = divisorImage ? channelIndexIn(*divisorImage, divisorChannels[r.divisorChannel], r.divisorChannel) : -1;
        if (channel >= 0) {
            const int divisorIndex[4] = { channel, -1, -1, -1 };
            std::vector<float> values;
            if (readImagePixels(*divisorImage, coords, divisorIndex, &values)) {
                divisors.resize(n);
                for (std::size_t i = 0; i < n; ++i) {
                    divisors[i] = values[i * 4];
                }
            }
        }
        if (abortInfo->isAborted()) {
            return false;
        }
    }

    out->rgb.resize(n * 3);
    for (std::size_t i = 0; i < n; ++i) {
        float* px = &rgba[i * 4];
        if (!divisors.empty() && (divisors[i] > 0.f)) {
            for (int c = 0; c < 3; ++c) {
                px[c] = Image::unPremultiplyValue(px[c], divisors[i]);
            }
        }
        for (int c = 0; c < 3; ++c) {
            out->rgb[i * 3 + c] = px[c];
        }
        px[3] = 1.f;
    }

    bool converted = false;
    if (r.display) {
        try {
            r.display(rgba.data(), (int)n);
            converted = true;
        } catch (...) {
            converted = false;
        }
    }
    out->display.resize(n * 3);
    for (std::size_t i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
            const float v = converted ? rgba[i * 4 + c] : ColorMath::toSRGB(out->rgb[i * 3 + c]);
            out->display[i * 3 + c] = std::isfinite(v) ? std::clamp(v, 0.f, 1.f) : 0.f;
        }
    }
    out->ok = true;

    return true;
} // fetchCloud

// Fetches run on one worker thread, latest request first; a newer request aborts the render in
// flight. The worker holds the fetcher, so shutting down never waits for it.
struct CloudFetcher {
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<CloudRequest> pending;
    AbortableRenderInfoPtr running;
    bool quit = false;
    std::thread worker;
    std::function<void(const CloudResult&)> deliver;

    void request(const CloudRequest& r,
                 const std::weak_ptr<CloudFetcher>& token)
    {
        std::lock_guard<std::mutex> locker(mutex);
        pending = r;
        if (running) {
            running->setAborted();
        }
        if (!worker.joinable()) {
            if (const std::shared_ptr<CloudFetcher> self = token.lock()) {
                worker = std::thread([self, token]() {
                    self->run(token);
                });
            }
        }
        wake.notify_one();
    }

    void abort()
    {
        std::lock_guard<std::mutex> locker(mutex);
        pending.reset();
        if (running) {
            running->setAborted();
        }
    }

    void run(const std::weak_ptr<CloudFetcher>& token)
    {
        std::unique_lock<std::mutex> locker(mutex);
        for (;;) {
            wake.wait(locker, [this]() {
                return quit || pending.has_value();
            });
            if (quit) {
                return;
            }
            const CloudRequest r = *pending;
            pending.reset();
            running = AbortableRenderInfo::create(true, 0);
            const AbortableRenderInfoPtr abortInfo = running;
            locker.unlock();
            CloudResult result;
            result.serial = r.serial;
            if (!fetchCloud(r, abortInfo, &result) || abortInfo->isAborted()) {
                result = CloudResult();
                result.serial = r.serial;
            }
            locker.lock();
            running.reset();
            if (quit) {
                return;
            }
            if (QCoreApplication::instance()) {
                QMetaObject::invokeMethod(QCoreApplication::instance(), [token, result]() {
                    if (std::shared_ptr<CloudFetcher> self = token.lock()) {
                        if (self->deliver) {
                            self->deliver(result);
                        }
                    } }, Qt::QueuedConnection);
            }
        }
    }

    void shutdown()
    {
        std::lock_guard<std::mutex> locker(mutex);
        quit = true;
        pending.reset();
        if (running) {
            running->setAborted();
        }
        wake.notify_one();
        if (worker.joinable()) {
            worker.detach();
        }
    }
};
} // anon namespace

struct ColorSpaceView::Implementation {
    enum DragEnum {
        eDragNone,
        eDragTumble,
        eDragPan,
        eDragDolly,
        eDragTarget
    };

    struct Pin {
        int id = 0;
        bool hold = false;
        double source[3] = { 0., 0., 0. };
        double target[3] = { 0., 0., 0. };
        double falloff = 1.;
    };

    QPointer<QWidget> widget;
    std::function<DisplayConverter()> makeDisplayConverter;
    std::function<ViewIdx()> currentView;

    NodeWPtr node;
    bool active = false;
    bool showOutput = false;
    std::unique_ptr<QObject> connections;
    QTimer* editPoll = nullptr;

    std::shared_ptr<CloudFetcher> fetcher;
    std::uint64_t requestSerial = 0;
    bool fetching = false;
    bool fetchQueued = false;
    bool frameOnNextCloud = false;
    // What the last fetch was asked of, to tell when the cloud no longer shows it.
    NodeWPtr fetchedSource;
    U64 fetchedMarks = 0;
    U64 fetchedHash = 0;
    bool fetchedHashKnown = false;

    // The fetched colours, working-space and display RGB, three floats per pixel.
    std::vector<float> cloudRgb;
    std::vector<float> cloudDisplay;
    // The pixels whose warp-space position is finite: their positions and display colours.
    std::vector<float> cloudWarp;
    std::vector<float> cloudWarpDisplay;
    ColorWarpSpaceEnum cloudSpace = eColorWarpSpaceOklabLog;
    bool vboDirty = true;
    unsigned int vbo = 0;
    const QOpenGLContext* vboContext = nullptr;
    int vboCount = 0;

    QVector3D boundsMin = QVector3D(0.f, 0.f, 0.f);
    QVector3D boundsMax = QVector3D(1.f, 1.f, 1.f);

    QVector3D target = QVector3D(0.5f, 0.5f, 0.5f);
    double yaw = kDefaultYaw;
    double pitch = kDefaultPitch;
    double distance = 3.;

    DragEnum drag = eDragNone;
    bool spaceHeld = false;
    QPointF lastPos;
    int dragPinId = 0;
    int dragAxis = -1;
    QPointF grabOffset;
    double grabDepth = 0.;
    QVector3D grabStart;
    std::optional<ColorWarpPinSet> dragBefore;

    Implementation(QWidget* widget,
                   const std::function<DisplayConverter()>& makeDisplayConverter,
                   const std::function<ViewIdx()>& currentView)
        : widget(widget)
        , makeDisplayConverter(makeDisplayConverter)
        , currentView(currentView)
        , fetcher(std::make_shared<CloudFetcher>())
    {
    }

    ColorWarp* colorWarp() const
    {
        const NodePtr n = node.lock();

        return n ? dynamic_cast<ColorWarp*>(n->getEffectInstance().get()) : nullptr;
    }

    KnobColorWarpPinsPtr pinsKnob() const
    {
        ColorWarp* cw = colorWarp();

        return cw ? cw->getPinsKnob() : KnobColorWarpPinsPtr();
    }

    double currentTime() const
    {
        const KnobColorWarpPinsPtr knob = pinsKnob();

        return knob ? knob->getCurrentTime() : 0.;
    }

    ViewIdx view() const
    {
        return currentView ? currentView() : ViewIdx(0);
    }

    void redraw()
    {
        if (widget && active) {
            widget->update();
        }
    }

    ColorWarpSpaceEnum space() const
    {
        ColorWarp* cw = colorWarp();

        return cw ? cw->getWarpSpace() : eColorWarpSpaceOklabLog;
    }

    ColorWarpSpaceBasis basis() const
    {
        ColorWarp* cw = colorWarp();
        if (cw) {
            return cw->getWarpSpaceBasis();
        }
        double m[9];
        ColorMath::workingToXYZ(nullptr, m);

        return ColorWarpSpaceBasis(m);
    }

    void lightnessFrame(QVector3D* up,
                        QVector3D* side,
                        QVector3D* front) const
    {
        double axis[3] = { 0., 1., 0. };

        colorWarpLightnessAxis(space(), basis(), axis);
        QVector3D u = toVec(axis).normalized();
        if (u.isNull()) {
            u = QVector3D(0.f, 1.f, 0.f);
        }
        int least = 0;
        for (int k = 1; k < 3; ++k) {
            if (std::fabs(u[k]) < std::fabs(u[least])) {
                least = k;
            }
        }
        QVector3D e(0.f, 0.f, 0.f);
        e[least] = 1.f;
        QVector3D s = (e - QVector3D::dotProduct(e, u) * u).normalized();
        *up = u;
        *side = s;
        *front = QVector3D::crossProduct(u, s);
    }

    QVector3D eye() const
    {
        QVector3D u, s, f;

        lightnessFrame(&u, &s, &f);
        const QVector3D dir = (float)std::cos(pitch) * ((float)std::cos(yaw) * f + (float)std::sin(yaw) * s) + (float)std::sin(pitch) * u;

        return target + (float)distance * dir;
    }

    QSizeF logicalSize() const
    {
        return widget ? QSizeF(std::max(1, widget->width()), std::max(1, widget->height())) : QSizeF(1., 1.);
    }

    QMatrix4x4 projection() const
    {
        const QSizeF size = logicalSize();
        QMatrix4x4 m;

        m.perspective((float)kFieldOfViewDegrees, (float)(size.width() / size.height()), (float)(distance * 0.01), (float)(distance * 100.));

        return m;
    }

    QMatrix4x4 modelView() const
    {
        QVector3D u, s, f;

        lightnessFrame(&u, &s, &f);
        QMatrix4x4 m;
        m.lookAt(eye(), target, u);

        return m;
    }

    QMatrix4x4 mvp() const
    {
        return projection() * modelView();
    }

    // Widget position in logical pixels, top-left origin, and the NDC depth in z.
    QVector3D toScreen(const QMatrix4x4& m,
                       const QVector3D& p) const
    {
        const QVector4D c = m * QVector4D(p, 1.f);
        const float w = (std::fabs(c.w()) > 1e-12f) ? c.w() : 1e-12f;
        const QSizeF size = logicalSize();

        return QVector3D((float)((c.x() / w + 1.) * 0.5 * size.width()), (float)((1. - c.y() / w) * 0.5 * size.height()), c.z() / w);
    }

    QPointF screenOf(const QMatrix4x4& m,
                     const QVector3D& p) const
    {
        const QVector3D s = toScreen(m, p);

        return QPointF(s.x(), s.y());
    }

    bool inFront(const QMatrix4x4& m,
                 const QVector3D& p) const
    {
        return (m * QVector4D(p, 1.f)).w() > 0.f;
    }

    QVector3D unproject(const QMatrix4x4& m,
                        const QPointF& pos,
                        double ndcDepth) const
    {
        const QSizeF size = logicalSize();
        const QVector4D ndc((float)(pos.x() / size.width() * 2. - 1.), (float)(1. - pos.y() / size.height() * 2.), (float)ndcDepth, 1.f);
        QVector4D p = m.inverted() * ndc;
        if (std::fabs(p.w()) > 1e-12f) {
            p /= p.w();
        }

        return p.toVector3D();
    }

    std::vector<Pin> pins() const
    {
        std::vector<Pin> out;
        const KnobColorWarpPinsPtr knob = pinsKnob();
        if (!knob) {
            return out;
        }
        const ColorWarpSpaceEnum sp = space();
        const ColorWarpSpaceBasis b = basis();
        const double time = currentTime();
        const ColorWarpPinSet set = knob->getPinSet();
        for (const ColorWarpPinData& data : set.pins) {
            if (!data.enabled) {
                continue;
            }
            const ColorWarpPin pin = data.evaluate(time);
            Pin p;
            p.id = data.id;
            p.falloff = pin.falloff;
            p.hold = (pin.strength <= 0.) || ((pin.source[0] == pin.target[0]) && (pin.source[1] == pin.target[1]) && (pin.source[2] == pin.target[2]));
            toWarpSpace(sp, b, pin.source, p.source);
            toWarpSpace(sp, b, pin.target, p.target);
            out.push_back(p);
        }

        return out;
    }

    void rebuildWarp()
    {
        const ColorWarpSpaceEnum sp = space();
        const ColorWarpSpaceBasis b = basis();
        const std::size_t n = std::min(cloudRgb.size(), cloudDisplay.size()) / 3;

        cloudSpace = sp;
        cloudWarp.clear();
        cloudWarpDisplay.clear();
        cloudWarp.reserve(n * 3);
        cloudWarpDisplay.reserve(n * 3);
        for (std::size_t i = 0; i < n; ++i) {
            const double rgb[3] = { cloudRgb[i * 3], cloudRgb[i * 3 + 1], cloudRgb[i * 3 + 2] };
            double w[3];
            toWarpSpace(sp, b, rgb, w);
            if (!std::isfinite(w[0]) || !std::isfinite(w[1]) || !std::isfinite(w[2])) {
                continue;
            }
            for (int c = 0; c < 3; ++c) {
                cloudWarp.push_back((float)w[c]);
                cloudWarpDisplay.push_back(cloudDisplay[i * 3 + c]);
            }
        }
        vboDirty = true;
        rebuildBounds();
    }

    void rebuildBounds()
    {
        const float inf = std::numeric_limits<float>::infinity();
        QVector3D lo(inf, inf, inf), hi(-inf, -inf, -inf);
        auto grow = [&](const QVector3D& p) {
            for (int c = 0; c < 3; ++c) {
                if (std::isfinite(p[c])) {
                    lo[c] = std::min(lo[c], p[c]);
                    hi[c] = std::max(hi[c], p[c]);
                }
            }
        };
        for (std::size_t i = 0; i + 2 < cloudWarp.size(); i += 3) {
            grow(QVector3D(cloudWarp[i], cloudWarp[i + 1], cloudWarp[i + 2]));
        }
        for (const Pin& p : pins()) {
            grow(toVec(p.source));
            grow(toVec(p.target));
        }
        if (!(lo[0] <= hi[0])) {
            const ColorWarpSpaceEnum sp = space();
            const ColorWarpSpaceBasis b = basis();
            const double black[3] = { 0., 0., 0. };
            const double white[3] = { 1., 1., 1. };
            double w0[3], w1[3];
            toWarpSpace(sp, b, black, w0);
            toWarpSpace(sp, b, white, w1);
            lo = hi = toVec(w0);
            grow(toVec(w1));
        }
        float extent = 0.f;
        for (int c = 0; c < 3; ++c) {
            extent = std::max(extent, hi[c] - lo[c]);
        }
        const float pad = (extent > 0.f) ? extent * 0.05f : 0.05f;
        for (int c = 0; c < 3; ++c) {
            if (hi[c] - lo[c] < pad) {
                lo[c] -= pad;
                hi[c] += pad;
            }
        }
        boundsMin = lo;
        boundsMax = hi;
    }

    void frame()
    {
        target = (boundsMin + boundsMax) * 0.5f;
        const double radius = std::max(1e-6, (double)(boundsMax - boundsMin).length() * 0.5);
        distance = radius / std::sin(kFieldOfViewDegrees * M_PI / 360.) * 1.1;
    }

    void scheduleFetch()
    {
        if (!active || fetchQueued) {
            return;
        }
        fetchQueued = true;
        QTimer::singleShot(0, connections.get(), [this]() {
            fetchQueued = false;
            fetch();
        });
    }

    // The node whose output the cloud shows: the ColorWarp, or the node its input passes on.
    NodePtr sourceNode() const
    {
        ColorWarp* cw = colorWarp();
        const NodePtr n = node.lock();
        if (!cw || !n) {
            return NodePtr();
        }
        if (showOutput) {
            return n;
        }
        EffectInstancePtr input = cw->getInput(0);
        input = input ? input->getNearestNonDisabled(currentTime()) : EffectInstancePtr();

        return input ? input->getNode() : NodePtr();
    }

    // Re-fetches when the source is another node, or when an edit of it or of anything upstream
    // may have changed its hash: its hash mark count moves with every such edit.
    void pollEdits()
    {
        if (!active || fetchQueued) {
            return;
        }
        const NodePtr source = sourceNode();
        if (source != fetchedSource.lock()) {
            scheduleFetch();

            return;
        }
        if (!source) {
            return;
        }
        const U64 marks = source->getHashMarkCount();
        if (marks == fetchedMarks) {
            return;
        }
        fetchedMarks = marks;
        U64 hash = 0;
        if (!fetchedHashKnown || !source->getHashValueIfUpToDate(&hash) || (hash != fetchedHash)) {
            scheduleFetch();
        }
    }

    void fetch()
    {
        ColorWarp* cw = colorWarp();
        const NodePtr n = node.lock();
        if (!active || !cw || !n) {
            return;
        }
        const double time = currentTime();
        const ViewIdx v = view();
        const NodePtr source = sourceNode();
        ++requestSerial;
        fetchedSource = source;
        fetchedMarks = source ? source->getHashMarkCount() : 0;
        fetchedHashKnown = source && source->getHashValueIfUpToDate(&fetchedHash);
        if (!source) {
            fetcher->abort();
            fetching = false;
            cloudRgb.clear();
            cloudDisplay.clear();
            rebuildWarp();
            redraw();

            return;
        }
        CloudRequest r;
        r.serial = requestSerial;
        r.node = source;
        r.time = time;
        r.view = v;
        if (!cw->getUnPremultChannel(time, v, &r.divisorLayer, &r.divisorChannel)) {
            r.divisorChannel = -1;
        }
        r.display = makeDisplayConverter ? makeDisplayConverter() : DisplayConverter();
        fetching = true;
        fetcher->request(r, fetcher);
        redraw();
    }

    void onFetched(const CloudResult& result)
    {
        if (result.serial != requestSerial) {
            return;
        }
        fetching = false;
        if (!result.ok) {
            cloudRgb.clear();
            cloudDisplay.clear();
            rebuildWarp();
            redraw();

            return;
        }
        cloudRgb = result.rgb;
        cloudDisplay = result.display;
        rebuildWarp();
        if (frameOnNextCloud) {
            frameOnNextCloud = false;
            frame();
        }
        redraw();
    }

    void connectNode()
    {
        // A fetch queued on the old context is dropped with it, and so is its poll.
        connections.reset(new QObject);
        editPoll = nullptr;
        fetchQueued = false;
        ColorWarp* cw = colorWarp();
        if (!cw) {
            return;
        }
        QObject* ctx = connections.get();
        editPoll = new QTimer(ctx);
        editPoll->setInterval(kEditPollMs);
        QObject::connect(editPoll, &QTimer::timeout, ctx, [this]() {
            pollEdits();
        });
        if (active) {
            editPoll->start();
        }
        if (const KnobColorWarpPinsPtr knob = cw->getPinsKnob()) {
            QObject::connect(knob.get(), &KnobColorWarpPins::selectedPinChanged, ctx, [this](int) {
                redraw();
            });
            if (const std::shared_ptr<KnobSignalSlotHandler> handler = knob->getSignalSlotHandler()) {
                QObject::connect(handler.get(), &KnobSignalSlotHandler::valueChanged, ctx, [this](ViewSpec, int, int) {
                    if (showOutput) {
                        scheduleFetch();
                    }
                    rebuildBounds();
                    redraw();
                });
            }
        }
        for (const KnobIPtr& knob : cw->getFieldKnobs()) {
            if (const std::shared_ptr<KnobSignalSlotHandler> handler = knob->getSignalSlotHandler()) {
                QObject::connect(handler.get(), &KnobSignalSlotHandler::valueChanged, ctx, [this](ViewSpec, int, int) {
                    if (space() != cloudSpace) {
                        rebuildWarp();
                        frame();
                    }
                    if (showOutput) {
                        scheduleFetch();
                    }
                    redraw();
                });
            }
        }
        const NodePtr n = node.lock();
        // The divisor changes the input cloud without moving the input's hash.
        if (const KnobChannelSelectPtr unPremultBy = n ? n->getUnPremultBySelector() : KnobChannelSelectPtr()) {
            if (const std::shared_ptr<KnobSignalSlotHandler> handler = unPremultBy->getSignalSlotHandler()) {
                QObject::connect(handler.get(), &KnobSignalSlotHandler::valueChanged, ctx, [this](ViewSpec, int, int) {
                    scheduleFetch();
                });
            }
        }
        const AppInstancePtr app = n ? n->getApp() : AppInstancePtr();
        if (const TimeLinePtr timeline = app ? app->getTimeLine() : TimeLinePtr()) {
            QObject::connect(timeline.get(), &TimeLine::frameChanged, ctx, [this](SequenceTime, int) {
                scheduleFetch();
                redraw();
            });
        }
    }

    void uploadCloud()
    {
        const QOpenGLContext* context = QOpenGLContext::currentContext();
        if (vbo && (vboContext != context)) {
            // The buffer belonged to a context that is gone; its name means nothing here.
            vbo = 0;
            vboDirty = true;
        }
        if (!vboDirty) {
            return;
        }
        vboDirty = false;
        const std::size_t n = std::min(cloudWarp.size(), cloudWarpDisplay.size()) / 3;
        std::vector<float> interleaved(n * 6);
        for (std::size_t i = 0; i < n; ++i) {
            for (int c = 0; c < 3; ++c) {
                interleaved[i * 6 + c] = cloudWarp[i * 3 + c];
                interleaved[i * 6 + 3 + c] = cloudWarpDisplay[i * 3 + c];
            }
        }
        if (!vbo) {
            glGenBuffers(1, &vbo);
            vboContext = context;
        }
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(interleaved.size() * sizeof(float)), interleaved.empty() ? nullptr : interleaved.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        vboCount = (int)n;
    }

    void handleAxes(const QVector3D& at,
                    QVector3D ends[3]) const
    {
        const float len = (boundsMax - boundsMin).length() * (float)kHandleShare;

        for (int k = 0; k < 3; ++k) {
            QVector3D e(0.f, 0.f, 0.f);
            e[k] = len;
            ends[k] = at + e;
        }
    }

    // The pin and part under pos: part 0 its target, 1 its source, 2 + k the target's axis handle k.
    bool pick(const QPointF& pos,
              int* pinId,
              int* part) const
    {
        const QMatrix4x4 m = mvp();
        const std::vector<Pin> all = pins();
        const KnobColorWarpPinsPtr knob = pinsKnob();
        const int selected = knob ? knob->getSelectedPinId() : 0;
        double best = kPickRadius;
        bool found = false;

        for (const Pin& p : all) {
            const QVector3D t = toVec(p.target);
            if (inFront(m, t)) {
                const double d = pointDistance(pos, screenOf(m, t));
                if (d <= best) {
                    best = d;
                    *pinId = p.id;
                    *part = 0;
                    found = true;
                }
            }
        }
        if (found) {
            return true;
        }
        for (const Pin& p : all) {
            if (p.id != selected) {
                continue;
            }
            const QVector3D t = toVec(p.target);
            QVector3D ends[3];
            handleAxes(t, ends);
            for (int k = 0; k < 3; ++k) {
                if (!inFront(m, t) || !inFront(m, ends[k])) {
                    continue;
                }
                const double d = segmentDistance(pos, screenOf(m, t), screenOf(m, ends[k]));
                if (d <= best) {
                    best = d;
                    *pinId = p.id;
                    *part = 2 + k;
                    found = true;
                }
            }
        }
        if (found) {
            return true;
        }
        for (const Pin& p : all) {
            const QVector3D s = toVec(p.source);
            if (inFront(m, s)) {
                const double d = pointDistance(pos, screenOf(m, s));
                if (d <= best) {
                    best = d;
                    *pinId = p.id;
                    *part = 1;
                    found = true;
                }
            }
        }

        return found;
    } // pick

    void dragTargetTo(const QPointF& pos)
    {
        const KnobColorWarpPinsPtr knob = pinsKnob();
        if (!knob || !dragBefore) {
            return;
        }
        const QMatrix4x4 m = mvp();
        const QPointF aim = pos - grabOffset;
        QVector3D warp;
        if (dragAxis < 0) {
            warp = unproject(m, aim, grabDepth);
        } else {
            const QVector3D nearP = unproject(m, aim, -1.);
            const QVector3D farP = unproject(m, aim, 1.);
            const QVector3D d = farP - nearP;
            QVector3D a(0.f, 0.f, 0.f);
            a[dragAxis] = 1.f;
            const QVector3D w0 = grabStart - nearP;
            const double b = QVector3D::dotProduct(a, d);
            const double c = QVector3D::dotProduct(d, d);
            const double denom = c - b * b;
            if (std::fabs(denom) < 1e-12) {
                return;
            }
            const double s = (b * QVector3D::dotProduct(d, w0) - c * QVector3D::dotProduct(a, w0)) / denom;
            warp = grabStart + (float)s * a;
        }
        const double w[3] = { warp.x(), warp.y(), warp.z() };
        double rgb[3];
        fromWarpSpace(space(), basis(), w, rgb);
        if (!std::isfinite(rgb[0]) || !std::isfinite(rgb[1]) || !std::isfinite(rgb[2])) {
            return;
        }
        ColorWarpPinSet set = *dragBefore;
        const int index = set.indexOf(dragPinId);
        if (index < 0) {
            return;
        }
        const double time = currentTime();
        for (int c = 0; c < 3; ++c) {
            set.pins[index].target[c].set(time, rgb[c]);
        }
        knob->setPinSet(set);
    }

    void endDrag()
    {
        if ((drag == eDragTarget) && dragBefore) {
            const ColorWarpPinSet before = *dragBefore;
            dragBefore.reset();
            if (const KnobColorWarpPinsPtr knob = pinsKnob()) {
                const ColorWarpPinSet after = knob->getPinSet();
                if (after != before) {
                    knob->editPinSet(before, after, QCoreApplication::translate("ColorSpaceView", "Move Pin Target").toStdString());
                }
            }
        }
        dragBefore.reset();
        drag = eDragNone;
        dragAxis = -1;
        dragPinId = 0;
    }

    void drawText(const TextRenderer& text,
                  const QFont& font,
                  double dpr,
                  const QPointF& logical,
                  const QString& label,
                  const QColor& color) const
    {
        const QSizeF size = logicalSize();

        text.renderText((float)(logical.x() * dpr), (float)((size.height() - logical.y()) * dpr), 1.f, 1.f, label, color, font);
    }

    void drawEllipsoid(const QVector3D& centre,
                       double radius,
                       double lightnessStretch) const
    {
        QVector3D u, s, f;

        lightnessFrame(&u, &s, &f);
        const int rings = 12;
        const int segments = 24;
        auto pointAt = [&](int i, int j) {
            const double theta = M_PI * i / rings - M_PI_2;
            const double phi = 2. * M_PI * j / segments;
            const double c = std::cos(theta);

            return centre + (float)(radius * c * std::cos(phi)) * s + (float)(radius * c * std::sin(phi)) * f + (float)(radius * lightnessStretch * std::sin(theta)) * u;
        };
        glColor4f(1.f, 0.75f, 0.3f, 0.12f);
        for (int i = 0; i < rings; ++i) {
            glBegin(GL_QUAD_STRIP);
            for (int j = 0; j <= segments; ++j) {
                const QVector3D a = pointAt(i, j);
                const QVector3D b = pointAt(i + 1, j);
                glVertex3f(a.x(), a.y(), a.z());
                glVertex3f(b.x(), b.y(), b.z());
            }
            glEnd();
        }
        glColor4f(1.f, 0.75f, 0.3f, 0.45f);
        glBegin(GL_LINE_LOOP);
        for (int j = 0; j < segments; ++j) {
            const QVector3D a = pointAt(rings / 2, j);
            glVertex3f(a.x(), a.y(), a.z());
        }
        glEnd();
    }
};

ColorSpaceView::ColorSpaceView(QWidget* widget,
                               const std::function<DisplayConverter()>& makeDisplayConverter,
                               const std::function<ViewIdx()>& currentView)
    : _imp(new Implementation(widget, makeDisplayConverter, currentView))
{
    _imp->connections.reset(new QObject);
    Implementation* imp = _imp.get();
    _imp->fetcher->deliver = [imp](const CloudResult& result) {
        imp->onFetched(result);
    };
}

ColorSpaceView::~ColorSpaceView()
{
    _imp->fetcher->deliver = nullptr;
    _imp->fetcher->shutdown();
    _imp->fetcher.reset();
    _imp->editPoll = nullptr;
    _imp->connections.reset();
}

void
ColorSpaceView::setNode(const NodePtr& node)
{
    if (node == _imp->node.lock()) {
        return;
    }
    if (_imp->drag != Implementation::eDragNone) {
        _imp->endDrag();
    }
    _imp->node = node;
    _imp->fetcher->abort();
    ++_imp->requestSerial;
    _imp->fetching = false;
    _imp->cloudRgb.clear();
    _imp->cloudDisplay.clear();
    _imp->connectNode();
    _imp->rebuildWarp();
    _imp->yaw = kDefaultYaw;
    _imp->pitch = kDefaultPitch;
    _imp->frame();
    _imp->frameOnNextCloud = true;
    _imp->scheduleFetch();
    _imp->redraw();
}

NodePtr
ColorSpaceView::getNode() const
{
    return _imp->node.lock();
}

void
ColorSpaceView::setActive(bool active)
{
    if (active == _imp->active) {
        return;
    }
    _imp->active = active;
    _imp->spaceHeld = false;
    if (_imp->editPoll) {
        if (active) {
            _imp->editPoll->start();
        } else {
            _imp->editPoll->stop();
        }
    }
    if (active) {
        _imp->scheduleFetch();
    } else {
        if (_imp->drag != Implementation::eDragNone) {
            _imp->endDrag();
        }
        _imp->fetcher->abort();
        ++_imp->requestSerial;
        _imp->fetching = false;
    }
}

bool
ColorSpaceView::isActive() const
{
    return _imp->active;
}

void
ColorSpaceView::setShowOutput(bool output)
{
    if (output == _imp->showOutput) {
        return;
    }
    _imp->showOutput = output;
    _imp->scheduleFetch();
    _imp->redraw();
}

bool
ColorSpaceView::isShowingOutput() const
{
    return _imp->showOutput;
}

void
ColorSpaceView::frameAll()
{
    _imp->rebuildBounds();
    _imp->frame();
    _imp->redraw();
}

void
ColorSpaceView::paint(const QColor& background,
                      const QFont& font,
                      const TextRenderer& text)
{
    GLProtectAttrib attribs(GL_ALL_ATTRIB_BITS);
    GLint viewport[4] = { 0, 0, 0, 0 };

    glGetIntegerv(GL_VIEWPORT, viewport);
    glClearColor(background.redF(), background.greenF(), background.blueF(), 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!_imp->colorWarp() || (viewport[2] <= 0) || (viewport[3] <= 0)) {
        return;
    }
    const double dpr = _imp->widget ? _imp->widget->devicePixelRatioF() : 1.;
    const ColorWarpSpaceEnum space = _imp->space();
    const ColorWarpSpaceBasis basis = _imp->basis();
    if (space != _imp->cloudSpace) {
        _imp->rebuildWarp();
    }
    _imp->uploadCloud();
    const std::vector<Implementation::Pin> pins = _imp->pins();
    const KnobColorWarpPinsPtr knob = _imp->pinsKnob();
    const int selected = knob ? knob->getSelectedPinId() : 0;
    ColorWarp* cw = _imp->colorWarp();
    const double time = _imp->currentTime();

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    {
        GLProtectMatrix projection(GL_PROJECTION);
        GLProtectMatrix modelView(GL_MODELVIEW);
        glMatrixMode(GL_PROJECTION);
        glLoadMatrixf(_imp->projection().constData());
        glMatrixMode(GL_MODELVIEW);
        glLoadMatrixf(_imp->modelView().constData());

        const QVector3D lo = _imp->boundsMin;
        const QVector3D hi = _imp->boundsMax;
        glLineWidth((float)dpr);
        glColor4f(0.45f, 0.45f, 0.45f, 0.6f);
        glBegin(GL_LINES);
        for (int k = 0; k < 3; ++k) {
            const int a = (k + 1) % 3;
            const int b = (k + 2) % 3;
            for (int corner = 0; corner < 4; ++corner) {
                QVector3D p0 = lo, p1;
                p0[a] = (corner & 1) ? hi[a] : lo[a];
                p0[b] = (corner & 2) ? hi[b] : lo[b];
                p1 = p0;
                p1[k] = hi[k];
                glVertex3f(p0.x(), p0.y(), p0.z());
                glVertex3f(p1.x(), p1.y(), p1.z());
            }
        }
        glEnd();

        glLineWidth((float)(2. * dpr));
        glBegin(GL_LINES);
        for (int k = 0; k < 3; ++k) {
            QVector3D end = lo;
            end[k] = hi[k];
            glColor4f(k == 0 ? 0.9f : 0.3f, k == 1 ? 0.9f : 0.3f, k == 2 ? 0.9f : 0.3f, 0.9f);
            glVertex3f(lo.x(), lo.y(), lo.z());
            glVertex3f(end.x(), end.y(), end.z());
        }
        glEnd();

        // Neutrals lie on a straight line in every warp space: through mid grey, along the
        // lightness axis, clipped to the box.
        {
            double axis[3];
            colorWarpLightnessAxis(space, basis, axis);
            const double grey[3] = { 0.18, 0.18, 0.18 };
            double g[3];
            toWarpSpace(space, basis, grey, g);
            const QVector3D origin = toVec(g);
            const QVector3D dir = toVec(axis).normalized();
            double tMin = -std::numeric_limits<double>::infinity();
            double tMax = std::numeric_limits<double>::infinity();
            for (int c = 0; c < 3; ++c) {
                if (std::fabs(dir[c]) < 1e-9f) {
                    continue;
                }
                double t0 = (lo[c] - origin[c]) / dir[c];
                double t1 = (hi[c] - origin[c]) / dir[c];
                if (t0 > t1) {
                    std::swap(t0, t1);
                }
                tMin = std::max(tMin, t0);
                tMax = std::min(tMax, t1);
            }
            if (std::isfinite(tMin) && std::isfinite(tMax) && (tMin < tMax)) {
                const QVector3D a = origin + (float)tMin * dir;
                const QVector3D b = origin + (float)tMax * dir;
                glLineWidth((float)dpr);
                glColor4f(0.85f, 0.85f, 0.85f, 0.7f);
                glBegin(GL_LINES);
                glVertex3f(a.x(), a.y(), a.z());
                glVertex3f(b.x(), b.y(), b.z());
                glEnd();
            }
        }

        if (_imp->vbo && (_imp->vboCount > 0)) {
            glPointSize((float)std::max(1., 2. * dpr));
            glBindBuffer(GL_ARRAY_BUFFER, _imp->vbo);
            glEnableClientState(GL_VERTEX_ARRAY);
            glEnableClientState(GL_COLOR_ARRAY);
            glVertexPointer(3, GL_FLOAT, 6 * sizeof(float), (const void*)0);
            glColorPointer(3, GL_FLOAT, 6 * sizeof(float), (const void*)(3 * sizeof(float)));
            glDrawArrays(GL_POINTS, 0, _imp->vboCount);
            glDisableClientState(GL_COLOR_ARRAY);
            glDisableClientState(GL_VERTEX_ARRAY);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }

        if (cw && (cw->getWarpKernel() == eColorWarpKernelGaussian)) {
            const double stretch = cw->getLightnessFalloff(time);
            for (const Implementation::Pin& p : pins) {
                if (p.id == selected) {
                    _imp->drawEllipsoid(toVec(p.source), p.falloff * colorWarpFalloffScale(space), (std::isfinite(stretch) && (stretch > 0.)) ? stretch : 1.);
                }
            }
        }

        glLineWidth((float)(2. * dpr));
        glBegin(GL_LINES);
        for (const Implementation::Pin& p : pins) {
            if (p.hold) {
                continue;
            }
            if (p.id == selected) {
                glColor4f(1.f, 0.7f, 0.2f, 1.f);
            } else {
                glColor4f(0.95f, 0.95f, 0.95f, 0.9f);
            }
            glVertex3d(p.source[0], p.source[1], p.source[2]);
            glVertex3d(p.target[0], p.target[1], p.target[2]);
        }
        for (const Implementation::Pin& p : pins) {
            if (p.id != selected) {
                continue;
            }
            QVector3D ends[3];
            const QVector3D t = toVec(p.target);
            _imp->handleAxes(t, ends);
            for (int k = 0; k < 3; ++k) {
                glColor4f(k == 0 ? 1.f : 0.25f, k == 1 ? 1.f : 0.25f, k == 2 ? 1.f : 0.25f, 1.f);
                glVertex3f(t.x(), t.y(), t.z());
                glVertex3f(ends[k].x(), ends[k].y(), ends[k].z());
            }
        }
        glEnd();
    }

    // Markers, arrow heads and labels are drawn in device pixels over the 3D pass.
    const QMatrix4x4 m = _imp->mvp();
    const QSizeF size = _imp->logicalSize();
    GLProtectMatrix projection(GL_PROJECTION);
    GLProtectMatrix modelView(GL_MODELVIEW);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0., viewport[2], 0., viewport[3], -1., 1.);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    auto device = [&](const QPointF& logical) {
        return QPointF(logical.x() * dpr, (size.height() - logical.y()) * dpr);
    };
    for (const Implementation::Pin& p : pins) {
        const QVector3D s = toVec(p.source);
        const QVector3D t = toVec(p.target);
        if (!_imp->inFront(m, t) || !_imp->inFront(m, s)) {
            continue;
        }
        const QPointF ts = device(_imp->screenOf(m, t));
        const QPointF ss = device(_imp->screenOf(m, s));
        const bool isSelected = (p.id == selected);
        const float r = isSelected ? 1.f : 0.95f;
        const float g = isSelected ? 0.7f : 0.95f;
        const float b = isSelected ? 0.2f : 0.95f;
        const double size = 5. * dpr;
        if (p.hold) {
            glColor4f(r, g, b, 1.f);
            glLineWidth((float)(2. * dpr));
            glBegin(GL_LINE_LOOP);
            glVertex2d(ts.x() - size, ts.y());
            glVertex2d(ts.x(), ts.y() + size);
            glVertex2d(ts.x() + size, ts.y());
            glVertex2d(ts.x(), ts.y() - size);
            glEnd();
            continue;
        }
        const QPointF dir = ts - ss;
        const double len = std::sqrt(dir.x() * dir.x() + dir.y() * dir.y());
        if (len > 1e-3) {
            const QPointF unit = dir / len;
            const QPointF normal(-unit.y(), unit.x());
            const double head = std::min(3. * size, len * 0.5);
            const QPointF base = ts - unit * head;
            glColor4f(r, g, b, 1.f);
            glBegin(GL_TRIANGLES);
            glVertex2d(ts.x(), ts.y());
            glVertex2d(base.x() + normal.x() * head * 0.45, base.y() + normal.y() * head * 0.45);
            glVertex2d(base.x() - normal.x() * head * 0.45, base.y() - normal.y() * head * 0.45);
            glEnd();
        }
        glPointSize((float)(4. * dpr));
        glBegin(GL_POINTS);
        glColor4f(0.f, 0.f, 0.f, 1.f);
        glVertex2d(ss.x(), ss.y());
        glEnd();
        glPointSize((float)(2.5 * dpr));
        glBegin(GL_POINTS);
        glColor4f(r, g, b, 1.f);
        glVertex2d(ss.x(), ss.y());
        glEnd();
    }

    const char* names[3];
    axisNames(space, names);
    const QVector3D lo = _imp->boundsMin;
    const QVector3D hi = _imp->boundsMax;
    for (int k = 0; k < 3; ++k) {
        QVector3D end = lo;
        end[k] = hi[k];
        if (!_imp->inFront(m, end)) {
            continue;
        }
        const QString label = QString::fromUtf8("%1 %2..%3").arg(QString::fromUtf8(names[k])).arg(lo[k], 0, 'g', 3).arg(hi[k], 0, 'g', 3);
        _imp->drawText(text, font, dpr, _imp->screenOf(m, end) + QPointF(6., -6.), label,
                       QColor::fromRgbF(k == 0 ? 0.95f : 0.5f, k == 1 ? 0.95f : 0.5f, k == 2 ? 0.95f : 0.5f));
    }
    QString title = QString::fromUtf8(colorWarpSpaceName(space)) + QString::fromUtf8(" - ") + (_imp->showOutput ? QCoreApplication::translate("ColorSpaceView", "Output") : QCoreApplication::translate("ColorSpaceView", "Input"));
    if (_imp->fetching) {
        title += QString::fromUtf8(" ...");
    }
    _imp->drawText(text, font, dpr, QPointF(10., 20.), title, QColor::fromRgbF(0.85f, 0.85f, 0.85f));
} // ColorSpaceView::paint

void
ColorSpaceView::releaseGL()
{
    if (_imp->vbo && (_imp->vboContext == QOpenGLContext::currentContext())) {
        glDeleteBuffers(1, &_imp->vbo);
    }
    _imp->vbo = 0;
    _imp->vboContext = nullptr;
    _imp->vboDirty = true;
}

bool
ColorSpaceView::mousePress(QMouseEvent* e)
{
    if (!_imp->colorWarp()) {
        return false;
    }
    const QPointF pos = e->position();
    _imp->lastPos = pos;
    const Qt::KeyboardModifiers modifiers = mouseBindingModifiers(e->modifiers(), _imp->spaceHeld);
    if (matchesMouseBinding(kShortcutIDMouseViewer3DTumble, modifiers, e->button())) {
        _imp->drag = Implementation::eDragTumble;
    } else if (matchesMouseBinding(kShortcutIDMouseViewer3DPan, modifiers, e->button())) {
        _imp->drag = Implementation::eDragPan;
    } else if (matchesMouseBinding(kShortcutIDMouseViewer3DDolly, modifiers, e->button())) {
        _imp->drag = Implementation::eDragDolly;
    } else if (matchesMouseBinding(kShortcutIDMouseViewer3DSelect, modifiers, e->button())) {
        int pinId = 0;
        int part = 0;
        if (_imp->pick(pos, &pinId, &part)) {
            if (const KnobColorWarpPinsPtr knob = _imp->pinsKnob()) {
                knob->setSelectedPinId(pinId);
                if (part != 1) {
                    for (const Implementation::Pin& p : _imp->pins()) {
                        if (p.id == pinId) {
                            const QMatrix4x4 m = _imp->mvp();
                            const QVector3D t = toVec(p.target);
                            const QVector3D screen = _imp->toScreen(m, t);
                            _imp->drag = Implementation::eDragTarget;
                            _imp->dragPinId = pinId;
                            _imp->dragAxis = (part >= 2) ? (part - 2) : -1;
                            _imp->grabStart = t;
                            _imp->grabDepth = screen.z();
                            _imp->grabOffset = (part == 0) ? (pos - QPointF(screen.x(), screen.y())) : QPointF(0., 0.);
                            if (part >= 2) {
                                // A handle grabbed away from the target keeps its offset along the axis.
                                const QVector3D nearP = _imp->unproject(m, pos, -1.);
                                const QVector3D farP = _imp->unproject(m, pos, 1.);
                                const QVector3D d = farP - nearP;
                                QVector3D a(0.f, 0.f, 0.f);
                                a[_imp->dragAxis] = 1.f;
                                const QVector3D w0 = t - nearP;
                                const double b = QVector3D::dotProduct(a, d);
                                const double c = QVector3D::dotProduct(d, d);
                                const double denom = c - b * b;
                                if (std::fabs(denom) > 1e-12) {
                                    const double s = (b * QVector3D::dotProduct(d, w0) - c * QVector3D::dotProduct(a, w0)) / denom;
                                    _imp->grabStart = t - (float)s * a;
                                }
                            }
                            _imp->dragBefore = knob->getPinSet();
                            break;
                        }
                    }
                }
            }
            _imp->redraw();
        }
    }
    e->accept();

    return true;
} // ColorSpaceView::mousePress

bool
ColorSpaceView::mouseMove(QMouseEvent* e)
{
    const QPointF pos = e->position();
    const QPointF delta = pos - _imp->lastPos;

    _imp->lastPos = pos;
    switch (_imp->drag) {
    case Implementation::eDragNone:

        return false;
    case Implementation::eDragTumble:
        _imp->yaw -= delta.x() * 0.01;
        _imp->pitch = std::clamp(_imp->pitch + delta.y() * 0.01, -kMaxPitch, kMaxPitch);
        break;
    case Implementation::eDragPan: {
        QVector3D u, s, f;
        _imp->lightnessFrame(&u, &s, &f);
        const QVector3D forward = (_imp->target - _imp->eye()).normalized();
        const QVector3D right = QVector3D::crossProduct(forward, u).normalized();
        const QVector3D up = QVector3D::crossProduct(right, forward).normalized();
        const double perPixel = 2. * _imp->distance * std::tan(kFieldOfViewDegrees * M_PI / 360.) / _imp->logicalSize().height();
        _imp->target += (float)(-delta.x() * perPixel) * right + (float)(delta.y() * perPixel) * up;
        break;
    }
    case Implementation::eDragDolly:
        _imp->distance = std::max(1e-6, _imp->distance * std::exp((delta.y() - delta.x()) * 0.01));
        break;
    case Implementation::eDragTarget:
        _imp->dragTargetTo(pos);
        break;
    }
    _imp->redraw();
    e->accept();

    return true;
}

bool
ColorSpaceView::mouseRelease(QMouseEvent* e)
{
    if (_imp->drag == Implementation::eDragNone) {
        return false;
    }
    _imp->endDrag();
    _imp->redraw();
    e->accept();

    return true;
}

bool
ColorSpaceView::wheel(QWheelEvent* e)
{
    const QPoint angle = e->angleDelta();
    const double steps = ((angle.y() != 0) ? angle.y() : angle.x()) / 120.;

    if ((steps == 0.) || !matchesMouseBinding(kShortcutIDMouseViewer3DDollyWheel, mouseBindingModifiers(e->modifiers(), _imp->spaceHeld), Qt::NoButton)) {
        return false;
    }
    _imp->distance = std::max(1e-6, _imp->distance * std::pow(0.85, steps));
    _imp->redraw();
    e->accept();

    return true;
}

bool
ColorSpaceView::event(QEvent* e)
{
    switch (e->type()) {
    case QEvent::ShortcutOverride:
    case QEvent::KeyPress:
    case QEvent::KeyRelease: {
        QKeyEvent* k = static_cast<QKeyEvent*>(e);
        if (!_imp->active || (k->key() != Qt::Key_Space) || !viewer3DMouseBindingsUseSpace()) {
            return false;
        }
        if (e->type() == QEvent::KeyPress) {
            _imp->spaceHeld = true;
        } else if ((e->type() == QEvent::KeyRelease) && !k->isAutoRepeat()) {
            _imp->spaceHeld = false;
        }
        e->accept();

        return true;
    }
    case QEvent::FocusOut:
        _imp->spaceHeld = false;

        return false;
    default:

        return false;
    }
}

QVariantMap
ColorSpaceView::state() const
{
    QVariantMap map;
    const NodePtr node = _imp->node.lock();
    const std::vector<Implementation::Pin> pins = _imp->pins();
    const KnobColorWarpPinsPtr knob = _imp->pinsKnob();
    const int selected = knob ? knob->getSelectedPinId() : 0;
    const QMatrix4x4 m = _imp->mvp();
    const ColorWarpSpaceEnum space = _imp->space();

    map[QString::fromUtf8("node")] = node ? QString::fromUtf8(node->getScriptName_mt_safe().c_str()) : QString();
    map[QString::fromUtf8("active")] = _imp->active;
    map[QString::fromUtf8("output")] = _imp->showOutput;
    map[QString::fromUtf8("fetching")] = _imp->fetching;
    map[QString::fromUtf8("pointCount")] = (int)(_imp->cloudWarp.size() / 3);
    map[QString::fromUtf8("space")] = QString::fromUtf8(colorWarpSpaceName(space));
    map[QString::fromUtf8("selectedPin")] = selected;
    map[QString::fromUtf8("camera")] = QString::fromUtf8("%1 %2 %3 %4 %5 %6")
                                           .arg(_imp->yaw, 0, 'f', 4)
                                           .arg(_imp->pitch, 0, 'f', 4)
                                           .arg(_imp->distance, 0, 'f', 4)
                                           .arg(_imp->target.x(), 0, 'f', 4)
                                           .arg(_imp->target.y(), 0, 'f', 4)
                                           .arg(_imp->target.z(), 0, 'f', 4);
    const char* names[3];
    axisNames(space, names);
    map[QString::fromUtf8("axes")] = QVariantList { QString::fromUtf8(names[0]), QString::fromUtf8(names[1]), QString::fromUtf8(names[2]) };
    map[QString::fromUtf8("boundsMin")] = QVariantList { _imp->boundsMin.x(), _imp->boundsMin.y(), _imp->boundsMin.z() };
    map[QString::fromUtf8("boundsMax")] = QVariantList { _imp->boundsMax.x(), _imp->boundsMax.y(), _imp->boundsMax.z() };

    int arrows = 0;
    int holds = 0;
    QVariantList pinList;
    auto point = [](const QPointF& p) {
        return QVariantList { p.x(), p.y() };
    };
    for (const Implementation::Pin& p : pins) {
        if (p.hold) {
            ++holds;
        } else {
            ++arrows;
        }
        QVariantMap entry;
        entry[QString::fromUtf8("id")] = p.id;
        entry[QString::fromUtf8("hold")] = p.hold;
        entry[QString::fromUtf8("source")] = point(_imp->screenOf(m, toVec(p.source)));
        entry[QString::fromUtf8("target")] = point(_imp->screenOf(m, toVec(p.target)));
        entry[QString::fromUtf8("targetWarp")] = QVariantList { p.target[0], p.target[1], p.target[2] };
        if (p.id == selected) {
            QVector3D ends[3];
            _imp->handleAxes(toVec(p.target), ends);
            entry[QString::fromUtf8("handles")] = QVariantList { point(_imp->screenOf(m, ends[0])), point(_imp->screenOf(m, ends[1])), point(_imp->screenOf(m, ends[2])) };
        }
        pinList.push_back(entry);
    }
    map[QString::fromUtf8("arrowCount")] = arrows;
    map[QString::fromUtf8("holdCount")] = holds;
    map[QString::fromUtf8("pins")] = pinList;

    return map;
} // ColorSpaceView::state

QVariantMap
ColorSpaceView::probe(double r,
                      double g,
                      double b) const
{
    QVariantMap map;
    const double rgb[3] = { r, g, b };
    double w[3];

    toWarpSpace(_imp->space(), _imp->basis(), rgb, w);
    const QVector3D p = toVec(w);
    double best = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i + 2 < _imp->cloudWarp.size(); i += 3) {
        const QVector3D q(_imp->cloudWarp[i], _imp->cloudWarp[i + 1], _imp->cloudWarp[i + 2]);
        best = std::min(best, (double)(q - p).length());
    }
    const QPointF screen = _imp->screenOf(_imp->mvp(), p);
    map[QString::fromUtf8("warp")] = QVariantList { w[0], w[1], w[2] };
    map[QString::fromUtf8("screen")] = QVariantList { screen.x(), screen.y() };
    map[QString::fromUtf8("nearest")] = std::isfinite(best) ? best : -1.;

    return map;
}

NATRON_NAMESPACE_EXIT
