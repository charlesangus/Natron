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

#include <cmath>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QApplication>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppManager.h"
#include "Engine/Lut.h" // Color::floatToInt
#include "Engine/Settings.h"

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
} // anon namespace

const double ThemedIcon::STROKE_WIDTH = 1.5;

bool
ThemedIcon::isSvgPath(const QString& path)
{
    return path.endsWith(QString::fromUtf8(".svg"), Qt::CaseInsensitive);
}

QByteArray
ThemedIcon::recolor(const QByteArray& svgData,
                    const QColor& color)
{
    QByteArray result = svgData;

    result.replace("currentColor", color.name().toLatin1());

    const QByteArray thinStroke = "stroke-width=\"" + QByteArray::number(STROKE_WIDTH) + "\"";
    result.replace("stroke-width=\"2\"", thinStroke);

    return result;
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

    const QByteArray svgData = recolor(file.readAll(), color);
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

QColor
ThemedIcon::themeTextColor()
{
    double r, g, b;

    appPTR->getCurrentSettings()->getTextColor(&r, &g, &b);

    return QColor(Color::floatToInt<256>(r), Color::floatToInt<256>(g), Color::floatToInt<256>(b));
}

QColor
ThemedIcon::themeSelectionColor()
{
    double r, g, b;

    appPTR->getCurrentSettings()->getSelectionColor(&r, &g, &b);

    return QColor(Color::floatToInt<256>(r), Color::floatToInt<256>(g), Color::floatToInt<256>(b));
}

QString
ThemedIcon::pixmapCacheKey(const QString& resourcePath,
                           int logicalSize,
                           const QColor& color,
                           double dpr)
{
    return resourcePath + QLatin1Char('@') + QString::number(logicalSize) + QLatin1Char('@') + color.name() + QLatin1Char('@') + QString::number(dpr);
}

namespace {
template <typename RenderFn>
QIcon
buildStateIcon(RenderFn render)
{
    const QColor text = ThemedIcon::themeTextColor();
    const QColor selection = ThemedIcon::themeSelectionColor();

    double br, bg, bb;
    appPTR->getCurrentSettings()->getBaseColor(&br, &bg, &bb);
    const QColor base(Color::floatToInt<256>(br), Color::floatToInt<256>(bg), Color::floatToInt<256>(bb));
    const QColor disabled = blend40Percent(base, text);

    QIcon result;

    result.addPixmap(render(text), QIcon::Normal, QIcon::Off);
    result.addPixmap(render(selection), QIcon::Normal, QIcon::On);
    result.addPixmap(render(disabled), QIcon::Disabled, QIcon::Off);
    result.addPixmap(render(text), QIcon::Active, QIcon::Off);

    return result;
}
} // anon namespace

QIcon
ThemedIcon::icon(const QString& resourcePath,
                 int logicalSize)
{
    const double dpr = qApp ? qApp->devicePixelRatio() : 1.;

    return buildStateIcon([&](const QColor& color) {
        return renderPixmap(resourcePath, logicalSize, color, dpr);
    });
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

QIcon
ThemedIcon::tintedIcon(const QString& rasterPath,
                       int logicalSize)
{
    const double dpr = qApp ? qApp->devicePixelRatio() : 1.;
    const QImage source(rasterPath);

    return buildStateIcon([&](const QColor& color) {
        return tintPixmap(source, logicalSize, color, dpr);
    });
}

NATRON_NAMESPACE_EXIT
