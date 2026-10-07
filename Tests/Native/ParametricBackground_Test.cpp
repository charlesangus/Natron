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

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "NativeParity.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/ColorLookup.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

ParametricBackgroundContext
fullContext()
{
    ParametricBackgroundContext context;

    context.xMin = -0.5;
    context.xMax = 1.5;
    context.yMin = -0.5;
    context.yMax = 1.5;
    context.pixelScaleX = 2. / 800.;
    context.pixelScaleY = 2. / 200.;

    return context;
}

} // namespace

class ParametricBackgroundTest
    : public BaseTest {
protected:
    static void setDisplay(KnobChoice* display,
                           ColorLookup::DisplayEnum value)
    {
        display->setValue((int)value, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
    }

    ParityPair makePair()
    {
        ParityPair pair = makeParityPair(getApp(), PLUGINID_NATRON_COLORLOOKUP, 1, PLUGIN_MAJOR_NATRON_COLORLOOKUP);

        EXPECT_TRUE(bool(pair.native));

        return pair;
    }
};

TEST_F(ParametricBackgroundTest, PainterIsReturnedAndRemovable)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    KnobParametric* table = dynamic_cast<KnobParametric*>(pair.native->getKnobByName(kColorLookupParamLookupTable).get());
    ASSERT_TRUE(table != NULL);
    int calls = 0;

    table->setBackgroundPainter([&calls](const ParametricBackgroundContext& context) {
        ++calls;
        ParametricBackground background;
        background.quads.resize(1);
        background.quads[0].v[0] = ParametricBackgroundVertex(context.xMin, context.yMin, 1.f, 0.f, 0.f);

        return background;
    });
    ParametricBackgroundPainter painter = table->getBackgroundPainter();
    ASSERT_TRUE(bool(painter));
    const ParametricBackground result = painter(fullContext());
    EXPECT_EQ(1, calls);
    ASSERT_EQ(1u, result.quads.size());
    EXPECT_EQ(-0.5, result.quads[0].v[0].x);

    table->setBackgroundPainter(ParametricBackgroundPainter());
    EXPECT_FALSE(bool(table->getBackgroundPainter()));
}

TEST_F(ParametricBackgroundTest, ColorLookupDrawsTheRampOverItsRange)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    KnobParametric* table = dynamic_cast<KnobParametric*>(pair.native->getKnobByName(kColorLookupParamLookupTable).get());
    ASSERT_TRUE(table != NULL);
    ParametricBackgroundPainter painter = table->getBackgroundPainter();
    ASSERT_TRUE(bool(painter));

    const ParametricBackground ramp = painter(fullContext());
    EXPECT_TRUE(ramp.additiveQuads.empty());
    ASSERT_GE(ramp.quads.size(), 1u);
    EXPECT_LE(ramp.quads.size(), 1024u);

    // With the default straight curves the ramp is a grey ramp, clipped to the range 0..1 inside
    // the view.
    EXPECT_NEAR(0., ramp.quads.front().v[0].x, 1e-12);
    EXPECT_NEAR(1., ramp.quads.back().v[1].x, 1e-12);
    for (std::size_t i = 0; i < ramp.quads.size(); ++i) {
        const ParametricBackgroundQuad& quad = ramp.quads[i];
        for (int j = 0; j < 4; ++j) {
            EXPECT_NEAR(quad.v[j].x, quad.v[j].r, 1e-4);
            EXPECT_NEAR(quad.v[j].x, quad.v[j].g, 1e-4);
            EXPECT_NEAR(quad.v[j].x, quad.v[j].b, 1e-4);
        }
        EXPECT_EQ(0., quad.v[0].y);
        EXPECT_EQ(1., quad.v[2].y);
    }

    // A raised master curve shows in the ramp.
    ASSERT_EQ(eStatusOK, table->addControlPoint(eValueChangedReasonPluginEdited, ColorLookup::eCurveMaster, 0.5, 0.8, eKeyframeTypeCubic));
    const ParametricBackground raised = painter(fullContext());
    ASSERT_GE(raised.quads.size(), 1u);
    double reddest = 0.;
    for (std::size_t i = 0; i < raised.quads.size(); ++i) {
        reddest = (std::max)(reddest, (double)raised.quads[i].v[0].r - raised.quads[i].v[0].x);
    }
    EXPECT_GT(reddest, 0.1);

    KnobChoice* display = dynamic_cast<KnobChoice*>(pair.native->getKnobByName(kColorLookupParamDisplay).get());
    ASSERT_TRUE(display != NULL);
    setDisplay(display, ColorLookup::eDisplayNone);
    const ParametricBackground none = painter(fullContext());
    EXPECT_TRUE(none.quads.empty());
    EXPECT_TRUE(none.additiveQuads.empty());
}

TEST_F(ParametricBackgroundTest, HistogramCountsEverySourcePixelAndIsFreedWhenHidden)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ColorLookup* effect = dynamic_cast<ColorLookup*>(pair.native->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);
    KnobParametric* table = dynamic_cast<KnobParametric*>(pair.native->getKnobByName(kColorLookupParamLookupTable).get());
    ASSERT_TRUE(table != NULL);
    KnobChoice* display = dynamic_cast<KnobChoice*>(pair.native->getKnobByName(kColorLookupParamDisplay).get());
    ASSERT_TRUE(display != NULL);
    KnobButton* update = dynamic_cast<KnobButton*>(pair.native->getKnobByName(kColorLookupParamUpdateHistogram).get());
    ASSERT_TRUE(update != NULL);
    ParametricBackgroundPainter painter = table->getBackgroundPainter();
    ASSERT_TRUE(bool(painter));

    EXPECT_TRUE(effect->getHistogramCounts().empty());
    setDisplay(display, ColorLookup::eDisplayHistogram);
    EXPECT_TRUE(update->isEnabled(0));

    effect->updateHistogram(kTime, ViewIdx(0));
    const std::vector<unsigned long long> counts = effect->getHistogramCounts();
    ASSERT_EQ(3u * 256u, counts.size());

    const RectI window = paritySourceWindow(pair.source, kTime, 0);
    const unsigned long long pixels = (unsigned long long)window.width() * (unsigned long long)window.height();
    ASSERT_GT(pixels, 0u);
    for (int c = 0; c < 3; ++c) {
        unsigned long long sum = 0;
        for (int i = 0; i < 256; ++i) {
            sum += counts[c * 256 + i];
        }
        EXPECT_EQ(pixels, sum) << "channel " << c;
    }

    const ParametricBackground histogram = painter(fullContext());
    EXPECT_TRUE(histogram.quads.empty());
    EXPECT_FALSE(histogram.additiveQuads.empty());
    EXPECT_LE(histogram.additiveQuads.size(), 3u * 256u);
    for (std::size_t i = 0; i < histogram.additiveQuads.size(); ++i) {
        const ParametricBackgroundQuad& quad = histogram.additiveQuads[i];
        EXPECT_EQ(0., quad.v[0].y);
        EXPECT_GT(quad.v[1].y, 0.);
        EXPECT_LT(quad.v[0].x, quad.v[2].x);
    }

    setDisplay(display, ColorLookup::eDisplayColorRamp);
    EXPECT_TRUE(effect->getHistogramCounts().empty());
    EXPECT_FALSE(update->isEnabled(0));
}
