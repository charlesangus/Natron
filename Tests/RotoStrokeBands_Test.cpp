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
#include <cstring>
#include <list>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <cairo/cairo.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoContextPrivate.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoPoint.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// The pattern cache size renderStroke() expects, ROTO_PRESSURE_LEVELS in the Engine sources.
const std::size_t kPressureLevels = 512;

struct StrokeLook {
    const char* name;
    bool buildUp;
    double hardness;
    bool pressureAffectsSize;
    bool pressureAffectsOpacity;
};

const StrokeLook kLooks[] = {
    { "soft build-up", true, 0.3, true, true },
    { "hard lighten", false, 1., false, false },
    { "soft lighten", false, 0.7, true, false },
    { "hard build-up", true, 1., false, true },
};

typedef std::list<std::list<std::pair<Point, double>>> StrokePoints;

// The surface's rows, row 0 first, without the stride padding.
struct Pixels {
    int bytesPerRow = 0;
    std::vector<unsigned char> data;
};

int
bytesPerPixel(cairo_format_t format)
{
    return (format == CAIRO_FORMAT_A8) ? 1 : 4;
}

void
appendRows(cairo_surface_t* surface,
           int width,
           int rows,
           cairo_format_t format,
           Pixels* out)
{
    cairo_surface_flush(surface);
    const unsigned char* data = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    out->bytesPerRow = width * bytesPerPixel(format);
    for (int y = 0; y < rows; ++y) {
        const unsigned char* row = data + (std::size_t)y * stride;
        out->data.insert(out->data.end(), row, row + out->bytesPerRow);
    }
}

bool
samePixels(const Pixels& a,
           const Pixels& b)
{
    return !a.data.empty() && (a.bytesPerRow == b.bytesPerRow) && (a.data.size() == b.data.size()) && (std::memcmp(a.data.data(), b.data.data(), a.data.size()) == 0);
}

bool
anyCoverage(const Pixels& p)
{
    for (unsigned char v : p.data) {
        if (v != 0) {
            return true;
        }
    }

    return false;
}

class RotoStrokeBandsTest
    : public BaseTest {
protected:
    void SetUp() override
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        appPTR->clearAllCaches();
        _paint = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTOPAINT));
        ASSERT_TRUE(_paint != nullptr);
        _context = _paint->getRotoContext();
        ASSERT_TRUE(_context != nullptr);
        ASSERT_FALSE(_context->getLayers().empty());

        // A wavy stroke whose pressure varies along it, so dabs straddle every band edge.
        _stroke.reset(new RotoStrokeItem(eRotoStrokeTypeSolid, _context, "Stroke", RotoLayerPtr()));
        _stroke->createNodes();
        _context->addItem(_context->getLayers().front(), 0, _stroke, RotoItem::eSelectionReasonOther);
        const int samples = 96;
        for (int i = 0; i < samples; ++i) {
            const double x = 60. + 6. * i;
            const double y = 220. + 140. * std::sin(0.11 * i);
            const double pressure = 0.25 + 0.75 * std::fabs(std::sin(0.07 * i));
            _stroke->appendPoint(i == 0, RotoPoint(x, y, pressure, 1. + 0.01 * i));
        }
        _stroke->setStrokeFinished();
        // A RotoPaint stroke lives on its lifetime frame alone, which the paint interact sets.
        _stroke->getLifeTimeFrameKnob()->setValue((int)kTime, ViewSpec::all(), 0);
        _stroke->getBrushSizeKnob()->setValue(48., ViewSpec::all(), 0);
        _stroke->getBrushSpacingKnob()->setValue(0.1, ViewSpec::all(), 0);
        _stroke->getOpacityKnob()->setValue(0.8, ViewSpec::all(), 0);
    }

    void TearDown() override
    {
        appPTR->clearAllCaches();
        _stroke.reset();
        _context.reset();
        _paint.reset();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    void applyLook(const StrokeLook& look)
    {
        _stroke->getBuildupKnob()->setValue(look.buildUp, ViewSpec::all(), 0);
        _stroke->getBrushHardnessKnob()->setValue(look.hardness, ViewSpec::all(), 0);
        _stroke->getPressureSizeKnob()->setValue(look.pressureAffectsSize, ViewSpec::all(), 0);
        _stroke->getPressureOpacityKnob()->setValue(look.pressureAffectsOpacity, ViewSpec::all(), 0);
    }

    // The stroke's points and the pixels its mask covers, as renderMaskFromStroke() finds them.
    RectI evaluate(StrokePoints* points)
    {
        RectD bbox;
        _stroke->evaluateStroke(0, kTime, points, &bbox);

        return bbox.toPixelEnclosing(0, 1.);
    }

    cairo_format_t format(const StrokeLook& look) const
    {
        return look.buildUp ? CAIRO_FORMAT_A8 : CAIRO_FORMAT_ARGB32;
    }

    // The stroke drawn by renderStroke() on one surface over roi, as masks were drawn before bands.
    Pixels serial(const StrokeLook& look,
                  const StrokePoints& points,
                  const RectI& roi)
    {
        Pixels out;
        cairo_surface_t* surface = cairo_image_surface_create(format(look), roi.width(), roi.height());
        if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(surface);

            return out;
        }
        cairo_surface_set_device_offset(surface, -roi.x1, -roi.y1);
        cairo_t* ctx = cairo_create(surface);
        cairo_set_fill_rule(ctx, CAIRO_FILL_RULE_WINDING);
        cairo_set_antialias(ctx, CAIRO_ANTIALIAS_NONE);
        std::vector<cairo_pattern_t*> dotPatterns(kPressureLevels, (cairo_pattern_t*)0);
        RotoContextPrivate::renderStroke(ctx, dotPatterns, points, 0, _stroke.get(), look.buildUp, _stroke->getOpacity(kTime), kTime, 0);
        appendRows(surface, roi.width(), roi.height(), format(look), &out);
        cairo_destroy(ctx);
        cairo_surface_destroy(surface);

        return out;
    }

    bool banded(const StrokeLook& look,
                const RotoContextPrivate::StrokeDabs& dabs,
                const RectI& roi,
                int nBands,
                RenderCancellation& cancel,
                Pixels* out,
                int* sinkCalls)
    {
        int nextRow = roi.y1;
        bool ordered = true;
        const bool rendered = RotoContextPrivate::renderStrokeDabsInBands(dabs, roi, format(look), look.buildUp, _stroke->getOpacity(kTime), nBands, cancel, [&](const RectI& band, cairo_surface_t* surface) {
            ++*sinkCalls;
            ordered = ordered && (band.y1 == nextRow) && (band.x1 == roi.x1) && (band.x2 == roi.x2) && (cairo_image_surface_get_height(surface) == band.height());
            nextRow = band.y2;
            appendRows(surface, band.width(), band.height(), format(look), out);
        });
        EXPECT_TRUE(ordered);
        if (rendered) {
            EXPECT_EQ(roi.y2, nextRow);
        }

        return rendered;
    }

    Pixels banded(const StrokeLook& look,
                  const RotoContextPrivate::StrokeDabs& dabs,
                  const RectI& roi,
                  int nBands)
    {
        Pixels out;
        int sinkCalls = 0;
        RenderCancellation cancel([]() { return false; });
        EXPECT_TRUE(banded(look, dabs, roi, nBands, cancel, &out, &sinkCalls)) << nBands << " bands";

        return out;
    }

    RotoContextPrivate::StrokeDabs dabsOf(const StrokePoints& points)
    {
        RotoContextPrivate::StrokeDabs dabs;
        RotoContextPrivate::evaluateStrokeDabs(points, 0, _stroke.get(), _stroke->getOpacity(kTime), kTime, 0, &dabs);

        return dabs;
    }

    NodePtr _paint;
    RotoContextPtr _context;
    RotoStrokeItemPtr _stroke;
};

} // namespace

TEST_F(RotoStrokeBandsTest, AnyBandCountMatchesOneBand)
{
    for (const StrokeLook& look : kLooks) {
        applyLook(look);
        StrokePoints points;
        const RectI roi = evaluate(&points);
        ASSERT_FALSE(roi.isNull()) << look.name;
        const RotoContextPrivate::StrokeDabs dabs = dabsOf(points);
        ASSERT_GT(dabs.dabs.size(), (std::size_t)100) << look.name;

        const Pixels one = banded(look, dabs, roi, 1);
        ASSERT_TRUE(anyCoverage(one)) << look.name;
        for (int nBands : { 2, 7, 16, 61, roi.height() }) {
            EXPECT_TRUE(samePixels(one, banded(look, dabs, roi, nBands))) << look.name << ", " << nBands << " bands";
        }
    }
}

TEST_F(RotoStrokeBandsTest, BandsMatchTheSingleSurfaceStroke)
{
    for (const StrokeLook& look : kLooks) {
        applyLook(look);
        StrokePoints points;
        const RectI roi = evaluate(&points);
        ASSERT_FALSE(roi.isNull()) << look.name;
        const Pixels reference = serial(look, points, roi);
        ASSERT_TRUE(anyCoverage(reference)) << look.name;
        const RotoContextPrivate::StrokeDabs dabs = dabsOf(points);
        for (int nBands : { 1, 16 }) {
            EXPECT_TRUE(samePixels(reference, banded(look, dabs, roi, nBands))) << look.name << ", " << nBands << " bands";
        }
    }
}

TEST_F(RotoStrokeBandsTest, TheMaskMatchesTheSingleSurfaceStroke)
{
    for (const StrokeLook& look : kLooks) {
        applyLook(look);
        appPTR->clearAllCaches();
        StrokePoints points;
        const RectI roi = evaluate(&points);
        ASSERT_FALSE(roi.isNull()) << look.name;
        const Pixels reference = serial(look, points, roi);
        ASSERT_TRUE(anyCoverage(reference)) << look.name;

        const ImagePtr mask = _stroke->renderMaskFromStroke(ImageLayerDesc::getAlphaComponents(), kTime, ViewIdx(0), eImageBitDepthFloat, 0, RectD());
        ASSERT_TRUE(mask != nullptr) << look.name;
        ASSERT_EQ(1u, mask->getComponentsCount()) << look.name;
        ASSERT_TRUE(mask->getBounds().contains(roi)) << look.name;

        const int srcComps = bytesPerPixel(format(look));
        int mismatches = 0;
        Image::ReadAccess access = mask->getReadRights();
        for (int y = 0; y < roi.height(); ++y) {
            const float* row = reinterpret_cast<const float*>(access.pixelAt(roi.x1, roi.y1 + y));
            const unsigned char* src = &reference.data[(std::size_t)y * reference.bytesPerRow];
            for (int x = 0; x < roi.width(); ++x) {
                const float expected = (float)src[(std::size_t)x * srcComps] * (1 / 255.f);
                if (std::memcmp(&expected, &row[x], sizeof(float)) != 0) {
                    ++mismatches;
                }
            }
        }
        EXPECT_EQ(0, mismatches) << look.name;
    }
}

TEST_F(RotoStrokeBandsTest, AnAbortedRenderHandsOverNoBand)
{
    const StrokeLook& look = kLooks[0];
    applyLook(look);
    StrokePoints points;
    const RectI roi = evaluate(&points);
    ASSERT_FALSE(roi.isNull());
    const RotoContextPrivate::StrokeDabs dabs = dabsOf(points);

    {
        Pixels out;
        int sinkCalls = 0;
        RenderCancellation cancel([]() { return true; });
        EXPECT_FALSE(banded(look, dabs, roi, 16, cancel, &out, &sinkCalls));
        EXPECT_EQ(0, sinkCalls);
    }
    // One band is asked about before the render, before the band and before every 32nd dab, so
    // the fourth ask stops it 32 dabs in. With helpers the caller may draw no band, but asks at
    // least once more after they finish.
    const std::pair<int, int> bandsAndAnswered[] = { { 1, 3 }, { 16, 1 } };
    for (const std::pair<int, int>& c : bandsAndAnswered) {
        Pixels out;
        int sinkCalls = 0;
        int asked = 0;
        const int answered = c.second;
        RenderCancellation cancel([&asked, answered]() { return ++asked > answered; });
        EXPECT_FALSE(banded(look, dabs, roi, c.first, cancel, &out, &sinkCalls)) << c.first << " bands";
        EXPECT_EQ(0, sinkCalls) << c.first << " bands";
    }
}
