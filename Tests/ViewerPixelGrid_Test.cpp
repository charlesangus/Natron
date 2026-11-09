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

#include <cmath>

#include <gtest/gtest.h>

#include "Gui/ViewerPixelGrid.h"
#include "Gui/ZoomContext.h"

NATRON_NAMESPACE_USING

namespace {

const int kOddWidth = 1137;
const int kOddHeight = 319;

struct ViewerSetup {
    double zoomFactor;
    int devicePixelRatio;
};

const ViewerSetup kSetups[] = {
    { 1., 1 },
    { 2., 1 },
    { 1., 2 },
    { 2., 2 },
};

// Same limits as the viewer.
ZoomContext
makeViewerZoom(double rawLeft,
               double rawBottom,
               const ViewerSetup& setup,
               bool snapped = true)
{
    ZoomContext ctx(0.01, 1024.);
    ctx.setScreenSize(kOddWidth, kOddHeight);
    ctx.setZoom(rawLeft, rawBottom, setup.zoomFactor, 1.);
    if (snapped) {
        ctx.setDevicePixelGrid(kOddWidth * setup.devicePixelRatio, kOddHeight * setup.devicePixelRatio);
    }

    return ctx;
}

// Where glOrtho(left, right, bottom, top) followed by the viewport transform puts a zoom
// coordinate, in device pixels from the left and from the top.
double
drawnDeviceX(const ZoomContext& ctx,
             double zoomX)
{
    return (zoomX - ctx.left()) / (ctx.right() - ctx.left()) * ctx.deviceWidth();
}

double
drawnDeviceYFromTop(const ZoomContext& ctx,
                    double zoomY)
{
    return (ctx.top() - zoomY) / (ctx.top() - ctx.bottom()) * ctx.deviceHeight();
}

// The texel a nearest-filtered image at 1 texel per zoom unit shows in a device pixel: the one
// containing the zoom coordinate of the pixel's centre.
int
drawnTexelColumn(const ZoomContext& ctx,
                 int deviceX)
{
    const double unitsPerDevicePixel = (ctx.right() - ctx.left()) / ctx.deviceWidth();

    return (int)std::floor(ctx.left() + (deviceX + 0.5) * unitsPerDevicePixel);
}

int
drawnTexelRow(const ZoomContext& ctx,
              int deviceYFromTop)
{
    const double unitsPerDevicePixel = (ctx.top() - ctx.bottom()) / ctx.deviceHeight();

    return (int)std::floor(ctx.top() - (deviceYFromTop + 0.5) * unitsPerDevicePixel);
}

} // namespace

TEST(ViewerPixelGrid, OddViewerAtFullSizePutsTheImageOnWholePixels)
{
    // A 320x200 image centred in the viewer at 100%.
    const double rawLeft = 160. - kOddWidth / 2.;
    const double rawBottom = 100. - kOddHeight / 2.;
    const ZoomContext ctx = makeViewerZoom(rawLeft, rawBottom, kSetups[0]);

    EXPECT_DOUBLE_EQ(kOddWidth, ctx.right() - ctx.left());
    EXPECT_DOUBLE_EQ(kOddHeight, ctx.top() - ctx.bottom());
    EXPECT_DOUBLE_EQ(ctx.left(), std::floor(ctx.left()));
    EXPECT_DOUBLE_EQ(ctx.bottom(), std::floor(ctx.bottom()));
    EXPECT_LE(std::abs(ctx.left() - rawLeft), 0.5);
    EXPECT_LE(std::abs(ctx.bottom() - rawBottom), 0.5);
    EXPECT_DOUBLE_EQ(rawLeft, ctx.unsnappedLeft());
    EXPECT_DOUBLE_EQ(rawBottom, ctx.unsnappedBottom());
}

TEST(ViewerPixelGrid, SnapsToDevicePixelsAtAnyZoomAndRatio)
{
    for (const ViewerSetup& setup : kSetups) {
        const ZoomContext ctx = makeViewerZoom(-137.3, 12.61, setup);
        const double devicePixelsPerUnit = setup.zoomFactor * setup.devicePixelRatio;
        const double originX = drawnDeviceX(ctx, 0.);
        const double originY = drawnDeviceYFromTop(ctx, 0.);

        EXPECT_NEAR(std::floor(originX + 0.5), originX, 1e-9) << setup.zoomFactor << " " << setup.devicePixelRatio;
        EXPECT_NEAR(std::floor(originY + 0.5), originY, 1e-9) << setup.zoomFactor << " " << setup.devicePixelRatio;
        EXPECT_LE(std::abs(ctx.left() - -137.3) * devicePixelsPerUnit, 0.5 + 1e-9);
        EXPECT_LE(std::abs(ctx.bottom() - 12.61) * devicePixelsPerUnit, 0.5 + 1e-9);
    }
}

TEST(ViewerPixelGrid, AlignedProjectionIsUnchanged)
{
    const ZoomContext ctx = makeViewerZoom(-400., -50., kSetups[0]);

    EXPECT_DOUBLE_EQ(-400., ctx.left());
    EXPECT_DOUBLE_EQ(-50., ctx.bottom());
}

TEST(ViewerPixelGrid, WithoutADeviceGridNothingSnaps)
{
    const ZoomContext ctx = makeViewerZoom(0.25, 0.75, kSetups[0], /*snapped=*/false);

    EXPECT_DOUBLE_EQ(0.25, ctx.left());
    EXPECT_DOUBLE_EQ(0.75, ctx.bottom());
    EXPECT_DOUBLE_EQ(0.25, snapEdgeToPixelGrid(0.25, 0., 10.));
}

TEST(ViewerPixelGrid, WidgetToZoomRoundTripMatchesTheDrawnProjection)
{
    const double widgetPoints[][2] = {
        { 0., 0. },
        { 1., 1. },
        { 10.5, 3.25 },
        { 568., 159. },
        { 568.5, 159.5 },
        { 1136., 318. },
        { 1137., 319. },
    };

    for (const ViewerSetup& setup : kSetups) {
        const ZoomContext ctx = makeViewerZoom(160. - kOddWidth / (2. * setup.zoomFactor), 100.3 - kOddHeight / (2. * setup.zoomFactor), setup);
        for (const auto& p : widgetPoints) {
            const QPointF zoomPos = ctx.toZoomCoordinates(p[0], p[1]);
            const QPointF back = ctx.toWidgetCoordinates(zoomPos.x(), zoomPos.y());

            EXPECT_NEAR(p[0], back.x(), 1e-9);
            EXPECT_NEAR(p[1], back.y(), 1e-9);
            EXPECT_NEAR(p[0] * setup.devicePixelRatio, drawnDeviceX(ctx, zoomPos.x()), 1e-9)
                << setup.zoomFactor << " " << setup.devicePixelRatio;
            EXPECT_NEAR(p[1] * setup.devicePixelRatio, drawnDeviceYFromTop(ctx, zoomPos.y()), 1e-9)
                << setup.zoomFactor << " " << setup.devicePixelRatio;
        }
    }
}

TEST(ViewerPixelGrid, PickingUnderAPixelReturnsTheTexelDrawnThere)
{
    // Pointer positions anywhere inside a device pixel, including its top left corner.
    const double insidePixel[] = { 0., 0.3, 0.99 };

    for (const ViewerSetup& setup : kSetups) {
        const ZoomContext ctx = makeViewerZoom(160. - kOddWidth / (2. * setup.zoomFactor), 100. - kOddHeight / (2. * setup.zoomFactor), setup);
        const double ratio = setup.devicePixelRatio;
        for (int device = 0; device < 64; ++device) {
            for (double inside : insidePixel) {
                const double widget = (device + inside) / ratio;
                const QPointF picked = ctx.toZoomCoordinatesAtPixelCentre(widget, widget);

                EXPECT_EQ(drawnTexelColumn(ctx, device), (int)std::floor(picked.x()))
                    << setup.zoomFactor << " " << ratio << " " << device << " " << inside;
                EXPECT_EQ(drawnTexelRow(ctx, device), (int)std::floor(picked.y()))
                    << setup.zoomFactor << " " << ratio << " " << device << " " << inside;
            }
        }
    }
}

TEST(ViewerPixelGrid, PanningKeepsTheUnsnappedState)
{
    for (const ViewerSetup& setup : kSetups) {
        ZoomContext snapped = makeViewerZoom(-13.37, 4.2, setup);
        ZoomContext reference = makeViewerZoom(-13.37, 4.2, setup, /*snapped=*/false);
        QPointF mouse(500., 150.);
        for (int i = 0; i < 1000; ++i) {
            const QPointF next(mouse.x() + ((i % 3) - 1) * 0.7 + 0.3, mouse.y() + ((i % 5) - 2) * 0.9 + 0.1);
            for (ZoomContext* ctx : { &snapped, &reference }) {
                const QPointF from = ctx->toZoomCoordinates(mouse.x(), mouse.y());
                const QPointF to = ctx->toZoomCoordinates(next.x(), next.y());
                ctx->translate(from.x() - to.x(), from.y() - to.y());
            }
            mouse = next;
        }
        const double devicePixelsPerUnit = setup.zoomFactor * setup.devicePixelRatio;

        EXPECT_NEAR(reference.left(), snapped.unsnappedLeft(), 1e-9);
        EXPECT_NEAR(reference.bottom(), snapped.unsnappedBottom(), 1e-9);
        EXPECT_LE(std::abs(snapped.left() - reference.left()) * devicePixelsPerUnit, 0.5 + 1e-9);
        EXPECT_LE(std::abs(snapped.bottom() - reference.bottom()) * devicePixelsPerUnit, 0.5 + 1e-9);
    }
}

TEST(ViewerPixelGrid, ZoomingAboutTheCursorKeepsTheUnsnappedState)
{
    for (const ViewerSetup& setup : kSetups) {
        ZoomContext snapped = makeViewerZoom(-13.37, 4.2, setup);
        ZoomContext reference = makeViewerZoom(-13.37, 4.2, setup, /*snapped=*/false);
        const QPointF mouse(733.4, 97.);
        for (int i = 0; i < 200; ++i) {
            const double scale = (i % 7 < 4) ? 1.1 : 1. / 1.1;
            for (ZoomContext* ctx : { &snapped, &reference }) {
                const QPointF centre = ctx->toZoomCoordinates(mouse.x(), mouse.y());
                ctx->zoom(centre.x(), centre.y(), scale);
            }
        }

        EXPECT_DOUBLE_EQ(reference.factor(), snapped.factor());
        const double devicePixelsPerUnit = snapped.factor() * setup.devicePixelRatio;
        EXPECT_NEAR(0., (reference.left() - snapped.unsnappedLeft()) * devicePixelsPerUnit, 1e-6);
        EXPECT_NEAR(0., (reference.bottom() - snapped.unsnappedBottom()) * devicePixelsPerUnit, 1e-6);

        // The zoom point stays under the cursor to within the snap.
        const QPointF before = reference.toZoomCoordinates(mouse.x(), mouse.y());
        const QPointF drawnAt = snapped.toWidgetCoordinates(before.x(), before.y());
        EXPECT_LE(std::abs(drawnAt.x() - mouse.x()) * setup.devicePixelRatio, 0.5 + 1e-6);
        EXPECT_LE(std::abs(drawnAt.y() - mouse.y()) * setup.devicePixelRatio, 0.5 + 1e-6);
    }
}
