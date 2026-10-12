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

#ifndef Gui_ThemedIcon_h
#define Gui_ThemedIcon_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <QByteArray>
#include <QColor>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QString>

NATRON_NAMESPACE_ENTER

/**
 * @brief Renders the vendored monochrome SVG glyphs (Gui/Resources/Icons/lucide and
 * Gui/Resources/Icons/natron) recoloured from the current theme, in place of a
 * pre-rasterised PNG.
 **/
class ThemedIcon {
public:
    // Lucide's vendored files keep their native stroke-width="2"; 1.5 reads better
    // at the sizes the UI actually draws these at, so every render thins it to this.
    static const double STROKE_WIDTH;

    static bool isSvgPath(const QString& path);

    /**
     * @brief Returns svgData with every "currentColor" token replaced by color's hex
     * value and every stroke-width="2" attribute replaced by STROKE_WIDTH. Exposed
     * standalone so it can be unit-tested without a QApplication.
     **/
    static QByteArray recolor(const QByteArray& svgData, const QColor& color);

    /**
     * @brief Loads the SVG at resourcePath, recolours it for color, and rasterizes it
     * into a logicalSize x logicalSize pixmap at the given device pixel ratio. The
     * returned pixmap's devicePixelRatio() is set to dpr, so callers can hand it
     * straight to QIcon/QLabel without further scaling.
     **/
    static QPixmap renderPixmap(const QString& resourcePath, int logicalSize, const QColor& color, double dpr);

    /**
     * @brief Returns the current theme's text colour (Settings' "text" knob), as an
     * opaque 8-bit QColor. Shared by GuiApplicationManager::getIcon and icon() so both
     * agree on what "the theme colour" means.
     **/
    static QColor themeTextColor();

    /**
     * @brief Returns the current theme's selection colour (Settings' "selection" knob),
     * the same way themeTextColor() reads the text colour.
     **/
    static QColor themeSelectionColor();

    /**
     * @brief Returns the key a renderPixmap() result for resourcePath/logicalSize/color/dpr
     * is cached under. Every argument has to contribute: callers ask for the same path at
     * several sizes (e.g. 24 and 18 px) and colours (Normal vs. Disabled) at once, and a key
     * that dropped one of those would return another state's or size's pixmap.
     **/
    static QString pixmapCacheKey(const QString& resourcePath, int logicalSize, const QColor& color, double dpr);

    /**
     * @brief Builds the full QIcon for resourcePath at logicalSize: Normal/Off and Active
     * in the theme text colour, Normal/On in the theme selection colour, and Disabled in
     * the text colour blended 40% into the base colour. The blend happens before the SVG
     * is recoloured, so Disabled is an ordinary opaque tone -- not a partially transparent
     * pixmap that would look different depending on what it's drawn over.
     **/
    static QIcon icon(const QString& resourcePath, int logicalSize);

    /**
     * @brief Scales source to fit logicalSize x logicalSize at dpr and paints every pixel in
     * color, keeping only the source's alpha: a raster icon's silhouette in a single tone.
     **/
    static QPixmap tintPixmap(const QImage& source, int logicalSize, const QColor& color, double dpr);

    /**
     * @brief icon()'s states for a raster (PNG) icon drawn through tintPixmap(), for a plugin
     * bundle's own grouping icon that would otherwise be the one coloured icon in the toolbox.
     **/
    static QIcon tintedIcon(const QString& rasterPath, int logicalSize);
};

NATRON_NAMESPACE_EXIT

#endif // Gui_ThemedIcon_h
