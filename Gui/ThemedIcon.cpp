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
#include <QFile>
#include <QPainter>
#include <QSvgRenderer>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

NATRON_NAMESPACE_ENTER

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

NATRON_NAMESPACE_EXIT
