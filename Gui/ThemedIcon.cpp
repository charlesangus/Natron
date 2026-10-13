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

#include "ThemedIcon.h"

#include <algorithm>
#include <cmath>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QApplication>
#include <QFile>
#include <QIconEngine>
#include <QImage>
#include <QPainter>
#include <QPixmapCache>
#include <QSvgRenderer>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/Lut.h" // Color::floatToInt
#include "Engine/Settings.h"
#include "Gui/GuiApplicationManager.h"

NATRON_NAMESPACE_ENTER

namespace {
QColor
blend40Percent(const QColor& base,
               const QColor& over)
{
    const double t = 0.4;
    const auto mix = [t](int baseChannel, int overChannel) {
        return (int)std::lround(baseChannel * (1. - t) + overChannel * t);
    };

    return QColor(mix(base.red(), over.red()), mix(base.green(), over.green()), mix(base.blue(), over.blue()));
}

QColor
toQColor(double r,
         double g,
         double b)
{
    return QColor(Color::floatToInt<256>(r), Color::floatToInt<256>(g), Color::floatToInt<256>(b));
}

const int kStrokeHoldSize = 18;
const double kMaxStrokeWidth = 2.5;
} // anon namespace

const double ThemedIcon::STROKE_WIDTH = 1.5;

bool
ThemedIcon::isSvgPath(const QString& path)
{
    return path.endsWith(QString::fromUtf8(".svg"), Qt::CaseInsensitive);
}

double
ThemedIcon::strokeWidthFor(int logicalSize)
{
    if ((logicalSize <= 0) || (logicalSize >= kStrokeHoldSize)) {
        return STROKE_WIDTH;
    }

    return std::min(kMaxStrokeWidth, STROKE_WIDTH * kStrokeHoldSize / logicalSize);
}

namespace {
QByteArray
recolorWithStroke(const QByteArray& svgData,
                  const QColor& color,
                  double strokeWidth)
{
    QByteArray result = svgData;

    result.replace("currentColor", color.name().toLatin1());

    const QByteArray stroke = "stroke-width=\"" + QByteArray::number(strokeWidth) + "\"";
    result.replace("stroke-width=\"2\"", stroke);

    return result;
}
} // anon namespace

QByteArray
ThemedIcon::recolor(const QByteArray& svgData,
                    const QColor& color)
{
    return recolorWithStroke(svgData, color, STROKE_WIDTH);
}

QByteArray
ThemedIcon::recolor(const QByteArray& svgData,
                    const QColor& color,
                    int logicalSize)
{
    return recolorWithStroke(svgData, color, strokeWidthFor(logicalSize));
}

QPixmap
ThemedIcon::renderPixmap(const QString& resourcePath,
                         int logicalSize,
                         const QColor& color,
                         double dpr)
{
    QFile file(resourcePath);

    if (!file.open(QIODevice::ReadOnly)) {
        return QPixmap();
    }

    const QByteArray svgData = recolor(file.readAll(), color, logicalSize);
    QSvgRenderer renderer(svgData);
    const int deviceSize = (int)std::lround(logicalSize * dpr);
    QPixmap pix(deviceSize, deviceSize);

    pix.fill(Qt::transparent);
    {
        QPainter painter(&pix);
        renderer.render(&painter, QRectF(0, 0, deviceSize, deviceSize));
    }
    pix.setDevicePixelRatio(dpr);

    return pix;
}

QPixmap
ThemedIcon::cachedPixmap(const QString& resourcePath,
                         int logicalSize,
                         const QColor& color,
                         double dpr)
{
    const QString key = pixmapCacheKey(resourcePath, logicalSize, color, dpr);
    QPixmap pix;

    if (!QPixmapCache::find(key, &pix)) {
        pix = renderPixmap(resourcePath, logicalSize, color, dpr);
        QPixmapCache::insert(key, pix);
    }

    return pix;
}

QColor
ThemedIcon::themeTextColor()
{
    double r, g, b;

    appPTR->getCurrentSettings()->getTextColor(&r, &g, &b);

    return toQColor(r, g, b);
}

QColor
ThemedIcon::themeSelectionColor()
{
    double r, g, b;

    appPTR->getCurrentSettings()->getSelectionColor(&r, &g, &b);

    return toQColor(r, g, b);
}

QColor
ThemedIcon::themeDisabledColor()
{
    double r, g, b;

    appPTR->getCurrentSettings()->getBaseColor(&r, &g, &b);

    return blend40Percent(toQColor(r, g, b), themeTextColor());
}

QString
ThemedIcon::pixmapCacheKey(const QString& resourcePath,
                           int logicalSize,
                           const QColor& color,
                           double dpr)
{
    return resourcePath + QLatin1Char('@') + QString::number(logicalSize) + QLatin1Char('@') + color.name() + QLatin1Char('@') + QString::number(dpr);
}

QPixmap
ThemedIcon::tintPixmap(const QImage& source,
                       int logicalSize,
                       const QColor& color,
                       double dpr)
{
    if (source.isNull()) {
        return QPixmap();
    }
    const int deviceSize = (int)std::lround(logicalSize * dpr);
    QImage tinted = source.scaled(deviceSize, deviceSize, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                        .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    {
        QPainter painter(&tinted);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(tinted.rect(), color);
    }
    QPixmap pix = QPixmap::fromImage(tinted);
    pix.setDevicePixelRatio(dpr);

    return pix;
}

namespace {
/*
 * Renders on demand instead of holding pixmaps, so one icon serves every size a style
 * asks for, and the colours are read at paint time, so a theme change shows on the next
 * repaint without the owner setting its icon again. Every mode is answered here, which
 * also keeps the platform style from generating its own (often lightened) variants.
 */
class ThemedIconEngine
    : public QIconEngine {
public:
    ThemedIconEngine(const QString& offPath,
                     const QString& onPath,
                     int logicalSize,
                     bool raster,
                     bool selectionColored = false)
        : _offPath(offPath)
        , _onPath(onPath)
        , _logicalSize(logicalSize)
        , _raster(raster)
        , _selectionColored(selectionColored)
    {
    }

    QIconEngine* clone() const override
    {
        return new ThemedIconEngine(*this);
    }

    QString key() const override
    {
        return QString::fromUtf8("NatronThemedIcon");
    }

    bool isNull() override
    {
        return _offPath.isEmpty();
    }

    QSize actualSize(const QSize& size,
                     QIcon::Mode /*mode*/,
                     QIcon::State /*state*/) override
    {
        const int side = std::min(size.width(), size.height());

        return QSize(side, side);
    }

    QList<QSize> availableSizes(QIcon::Mode /*mode*/,
                                QIcon::State /*state*/) override
    {
        return QList<QSize>() << QSize(_logicalSize, _logicalSize);
    }

    QPixmap pixmap(const QSize& size,
                   QIcon::Mode mode,
                   QIcon::State state) override
    {
        return render(size, mode, state, 1.);
    }

    // Qt hands this either the logical or the device size depending on the release; a
    // pixmap that comes out larger is given a matching devicePixelRatio by QIcon::pixmap(),
    // so it still draws at the right logical size either way.
    QPixmap scaledPixmap(const QSize& size,
                         QIcon::Mode mode,
                         QIcon::State state,
                         qreal scale) override
    {
        return render(size, mode, state, scale);
    }

    void paint(QPainter* painter,
               const QRect& rect,
               QIcon::Mode mode,
               QIcon::State state) override
    {
        const qreal dpr = painter->device() ? painter->device()->devicePixelRatio() : 1.;
        const QPixmap pix = render(rect.size(), mode, state, dpr);
        const QSizeF logical = pix.deviceIndependentSize();
        const QPointF topLeft(rect.x() + (rect.width() - logical.width()) / 2.,
                              rect.y() + (rect.height() - logical.height()) / 2.);

        painter->drawPixmap(topLeft, pix);
    }

private:
    QPixmap render(const QSize& size,
                   QIcon::Mode mode,
                   QIcon::State state,
                   qreal scale) const
    {
        const int logicalSize = std::max(1, std::min(size.width(), size.height()));
        const bool hasOnGlyph = !_onPath.isEmpty();
        const QString& path = (state == QIcon::On && hasOnGlyph) ? _onPath : _offPath;
        QColor color;

        if (mode == QIcon::Disabled) {
            color = ThemedIcon::themeDisabledColor();
        } else if (_selectionColored) {
            color = ThemedIcon::themeSelectionColor();
        } else if ((state == QIcon::On) && !hasOnGlyph && (mode != QIcon::Selected)) {
            color = ThemedIcon::themeSelectionColor();
        } else {
            color = ThemedIcon::themeTextColor();
        }

        if (!_raster) {
            return ThemedIcon::cachedPixmap(path, logicalSize, color, scale);
        }

        const QString key = QString::fromUtf8("tint:") + ThemedIcon::pixmapCacheKey(path, logicalSize, color, scale);
        QPixmap pix;
        if (!QPixmapCache::find(key, &pix)) {
            pix = ThemedIcon::tintPixmap(QImage(path), logicalSize, color, scale);
            QPixmapCache::insert(key, pix);
        }

        return pix;
    }

    QString _offPath;
    QString _onPath;
    int _logicalSize;
    bool _raster;
    bool _selectionColored;
};
} // anon namespace

QIcon
ThemedIcon::icon(const QString& resourcePath,
                 int logicalSize)
{
    return QIcon(new ThemedIconEngine(resourcePath, QString(), logicalSize, false));
}

QIcon
ThemedIcon::icon(const QString& offPath,
                 const QString& onPath,
                 int logicalSize)
{
    return QIcon(new ThemedIconEngine(offPath, onPath == offPath ? QString() : onPath, logicalSize, false));
}

QIcon
ThemedIcon::tintedIcon(const QString& rasterPath,
                       int logicalSize)
{
    return QIcon(new ThemedIconEngine(rasterPath, QString(), logicalSize, true));
}

QIcon
ThemedIcon::selectionIcon(const QString& resourcePath,
                          int logicalSize)
{
    return QIcon(new ThemedIconEngine(resourcePath, QString(), logicalSize, false, true));
}

QIcon
ThemedIcon::messageIcon(MessageGlyphEnum kind,
                        int logicalSize)
{
    const char* path = NATRON_ICONS_PATH "lucide/info.svg";

    switch (kind) {
    case eMessageGlyphInformation:
        break;
    case eMessageGlyphWarning:
        path = NATRON_ICONS_PATH "lucide/triangle-alert.svg";
        break;
    case eMessageGlyphError:
        path = NATRON_ICONS_PATH "lucide/circle-x.svg";
        break;
    case eMessageGlyphQuestion:
        path = NATRON_ICONS_PATH "lucide/circle-question-mark.svg";
        break;
    }

    return icon(QString::fromUtf8(path), logicalSize);
}

namespace {
const char* kLabelIconProperty = "_natronThemedIcon";
const char* kLabelIconSizeProperty = "_natronThemedIconSize";
const char* kLabelIconBoundProperty = "_natronThemedIconBound";

void
applyLabelIcon(QLabel* label)
{
    const QIcon icon = label->property(kLabelIconProperty).value<QIcon>();
    const int size = label->property(kLabelIconSizeProperty).toInt();

    label->setPixmap(icon.pixmap(QSize(size, size), label->devicePixelRatio()));
}
} // anon namespace

void
ThemedIcon::setLabelIcon(QLabel* label,
                         const QIcon& icon,
                         int logicalSize)
{
    if (!label) {
        return;
    }
    label->setProperty(kLabelIconProperty, QVariant::fromValue(icon));
    label->setProperty(kLabelIconSizeProperty, logicalSize);
    applyLabelIcon(label);
    if (!label->property(kLabelIconBoundProperty).toBool()) {
        label->setProperty(kLabelIconBoundProperty, true);
        connectThemeChanged(label, [label]() {
            applyLabelIcon(label);
        });
    }
}

void
ThemedIcon::connectThemeChanged(QObject* context,
                                const std::function<void()>& fn)
{
    GuiApplicationManager* app = qobject_cast<GuiApplicationManager*>(AppManager::instance());

    if (!app || !context) {
        return;
    }
    QObject::connect(app, &GuiApplicationManager::themeChanged, context, fn);
}

NATRON_NAMESPACE_EXIT
