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

#include "Global/Macros.h"

#include <QByteArray>
#include <QColor>
#include <QFile>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QString>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "Gui/ThemedIcon.h"

NATRON_NAMESPACE_USING

namespace {

// A glyph in the same shape the vendored Lucide/natron files use: currentColor fill,
// a 2px stroke, on the 24-unit grid. recolor()/renderPixmap() only look at those two
// tokens, so one circle is enough to exercise them without vendoring a real file here.
const char* kGlyphSvg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" "
                        "fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" "
                        "stroke-linejoin=\"round\"><circle cx=\"12\" cy=\"12\" r=\"10\"/></svg>";

// renderPixmap() and icon() read the glyph from disk by path, so the fixture writes it to a
// temp file rather than passing bytes directly.
class ThemedIconTest
    : public ::testing::Test {
protected:
    QString glyphPath;

    void SetUp() override
    {
        glyphPath = tmpDir.filePath(QString::fromUtf8("glyph.svg"));
        QFile file(glyphPath);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(kGlyphSvg);
    }

private:
    QTemporaryDir tmpDir;
};

} // anon namespace

TEST(ThemedIcon, RecolorSubstitutesCurrentColor)
{
    const QByteArray recolored = ThemedIcon::recolor(QByteArray(kGlyphSvg), QColor(255, 0, 0));

    EXPECT_FALSE(recolored.contains("currentColor"));
    EXPECT_TRUE(recolored.contains("#ff0000"));
}

TEST(ThemedIcon, RecolorRewritesStrokeWidth)
{
    const QByteArray recolored = ThemedIcon::recolor(QByteArray(kGlyphSvg), QColor(255, 0, 0));

    EXPECT_FALSE(recolored.contains("stroke-width=\"2\""));
    EXPECT_TRUE(recolored.contains("stroke-width=\"1.5\""));
}

TEST_F(ThemedIconTest, RenderPixmapAtDpr2DoublesDeviceSize)
{
    const QPixmap pix1x = ThemedIcon::renderPixmap(glyphPath, 24, QColor(255, 0, 0), 1.);
    const QPixmap pix2x = ThemedIcon::renderPixmap(glyphPath, 24, QColor(255, 0, 0), 2.);

    EXPECT_EQ(24, pix1x.width());
    EXPECT_EQ(24, pix1x.height());
    EXPECT_EQ(48, pix2x.width());
    EXPECT_EQ(48, pix2x.height());
    EXPECT_DOUBLE_EQ(2., pix2x.devicePixelRatio());
}

TEST_F(ThemedIconTest, IconNormalOnAndOffPixmapsDiffer)
{
    const QIcon icon = ThemedIcon::icon(glyphPath, 24);
    const QImage off = icon.pixmap(24, QIcon::Normal, QIcon::Off).toImage();
    const QImage on = icon.pixmap(24, QIcon::Normal, QIcon::On).toImage();

    ASSERT_FALSE(off.isNull());
    ASSERT_FALSE(on.isNull());
    EXPECT_NE(off, on);
}

TEST(ThemedIcon, PixmapCacheKeyIncludesSize)
{
    const QColor color(255, 0, 0);
    const QString key24 = ThemedIcon::pixmapCacheKey(QString::fromUtf8("glyph.svg"), 24, color, 1.);
    const QString key18 = ThemedIcon::pixmapCacheKey(QString::fromUtf8("glyph.svg"), 18, color, 1.);
    const QString key24Again = ThemedIcon::pixmapCacheKey(QString::fromUtf8("glyph.svg"), 24, color, 1.);

    EXPECT_NE(key24, key18);
    EXPECT_EQ(key24, key24Again);
}

TEST(ThemedIcon, StrokeKeepsItsScreenWeightBelow18Px)
{
    EXPECT_DOUBLE_EQ(ThemedIcon::STROKE_WIDTH, ThemedIcon::strokeWidthFor(24));
    EXPECT_DOUBLE_EQ(ThemedIcon::STROKE_WIDTH, ThemedIcon::strokeWidthFor(18));
    EXPECT_DOUBLE_EQ(ThemedIcon::STROKE_WIDTH * 18. / 12., ThemedIcon::strokeWidthFor(12));
    EXPECT_DOUBLE_EQ(2.5, ThemedIcon::strokeWidthFor(4));

    const QByteArray recolored = ThemedIcon::recolor(QByteArray(kGlyphSvg), QColor(255, 0, 0), 12);
    EXPECT_TRUE(recolored.contains("stroke-width=\"2.25\""));
}

TEST_F(ThemedIconTest, IconRendersAtTheRequestedSize)
{
    const QIcon icon = ThemedIcon::icon(glyphPath, 24);
    const QPixmap small = icon.pixmap(QSize(13, 13), 1., QIcon::Normal, QIcon::Off);
    const QPixmap large = icon.pixmap(QSize(36, 36), 1., QIcon::Normal, QIcon::Off);

    EXPECT_EQ(13, small.width());
    EXPECT_EQ(36, large.width());
}

TEST_F(ThemedIconTest, TwoGlyphIconDrawsOnStateFromItsOwnFile)
{
    const QString onPath = glyphPath + QString::fromUtf8(".on.svg");
    {
        QFile file(onPath);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" "
                   "fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\"><path d=\"M1 1h22v22H1z\"/></svg>");
    }
    const QIcon icon = ThemedIcon::icon(glyphPath, onPath, 24);
    const QImage off = icon.pixmap(QSize(24, 24), 1., QIcon::Normal, QIcon::Off).toImage();
    const QImage on = icon.pixmap(QSize(24, 24), 1., QIcon::Normal, QIcon::On).toImage();

    ASSERT_FALSE(off.isNull());
    ASSERT_FALSE(on.isNull());
    EXPECT_NE(off, on);
    // The corner of the square is drawn only by the On glyph.
    EXPECT_EQ(0, off.pixelColor(1, 1).alpha());
    EXPECT_GT(on.pixelColor(1, 1).alpha(), 0);
}
