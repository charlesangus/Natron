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

#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Merge/MergeOps.h"
#include "Gpu/MergeOpsGolden.h"

NATRON_NAMESPACE_USING

using namespace MergeOps;

namespace {

Operator
op(const char* name)
{
    Operator o = eOperatorCount;
    EXPECT_TRUE(operatorFromName(name, &o)) << name;

    return o;
}

AlphaSpan
broadcast(const float* a)
{
    return AlphaSpan { a, 1, 0 };
}

} // namespace

TEST(MergeOps, OperatorNamesAreNukesAndRoundTrip)
{
    EXPECT_EQ(int(eOperatorCount), 39);
    std::set<std::string> names;
    for (int i = 0; i < eOperatorCount; ++i) {
        const std::string name = operatorName(Operator(i));
        EXPECT_FALSE(name.empty());
        EXPECT_TRUE(names.insert(name).second) << name;
        Operator back = eOperatorCount;
        ASSERT_TRUE(operatorFromName(name, &back)) << name;
        EXPECT_EQ(int(back), i);
        EXPECT_NE(std::string(operatorHint(Operator(i))), "");
    }
    for (const char* n : { "atop", "average", "color-burn", "color-dodge", "conjoint-over", "copy", "difference",
                           "disjoint-over", "divide", "exclusion", "from", "geometric", "hard-light", "hypot", "in",
                           "mask", "matte", "max", "min", "minus", "multiply", "out", "over", "overlay", "plus",
                           "screen", "soft-light", "stencil", "under", "xor", "hue", "saturation", "color",
                           "luminosity", "freeze", "grain-extract", "grain-merge", "pin-light", "reflect" }) {
        EXPECT_EQ(names.count(n), 1u) << n;
    }
    Operator o = eOver;
    EXPECT_FALSE(operatorFromName("pinlight", &o));
    EXPECT_FALSE(operatorFromName("Over", &o));
    EXPECT_EQ(o, eOver);

    for (Operator ns : { eHue, eSaturation, eColor, eLuminosity }) {
        EXPECT_FALSE(isSeparable(ns));
    }
    EXPECT_TRUE(isSeparable(eOver));
    EXPECT_EQ(rowFunction(eOver, 0), nullptr);
    EXPECT_EQ(rowFunction(eOver, 5), nullptr);
}

TEST(MergeOps, SeparableGoldens)
{
    for (const mergegolden::SeparableGolden& g : mergegolden::kSeparableGoldens) {
        const Operator o = op(g.op);
        ASSERT_TRUE(isSeparable(o)) << g.op;
        for (int nComps : { 1, 4 }) {
            std::vector<float> A(nComps, g.A), B(nComps, g.B), out(nComps, -123.f);
            mergeRow(o, nComps, A.data(), broadcast(&g.a), B.data(), broadcast(&g.b), 1, out.data());
            for (int c = 0; c < nComps; ++c) {
                EXPECT_TRUE(mergegolden::goldenAccepts(g.expected, out[c]))
                    << g.op << " A=" << g.A << " a=" << g.a << " B=" << g.B << " b=" << g.b << " nComps=" << nComps
                    << " channel " << c << ": got " << out[c] << ", want " << g.expected;
            }
        }
    }
}

TEST(MergeOps, NonSeparableGoldens)
{
    for (const mergegolden::NonSeparableGolden& g : mergegolden::kNonSeparableGoldens) {
        const Operator o = op(g.op);
        ASSERT_FALSE(isSeparable(o)) << g.op;
        float out[4] = { -123.f, -123.f, -123.f, -123.f };
        mergeRow(o, 4, g.A, broadcast(&g.a), g.B, broadcast(&g.b), 1, out);
        for (int c = 0; c < 4; ++c) {
            EXPECT_TRUE(mergegolden::goldenAccepts(g.expected[c], out[c]))
                << g.op << " a=" << g.a << " b=" << g.b << " channel " << c << ": got " << out[c] << ", want "
                << g.expected[c];
        }
    }
}

// Two RGB pixels over B with a per-channel alpha layer, against hand-computed A + B(1 - a_c).
TEST(MergeOps, PerChannelAlphaLayer)
{
    const float A[6] = { 0.5f, 0.25f, 1.f, 2.f, 0.f, -1.f };
    const float B[6] = { 1.f, 1.f, 1.f, 0.5f, 0.5f, 0.5f };
    const float alphaA[6] = { 0.5f, 0.25f, 1.f, 0.f, 0.75f, 1.5f };
    const float one[1] = { 1.f };
    float out[6];
    mergeRow(eOver, 3, A, AlphaSpan { alphaA, 3, 1 }, B, AlphaSpan { one, 0, 0 }, 2, out);
    const float want[6] = { 1.f, 1.f, 1.f, 2.5f, 0.125f, -1.25f };
    for (int i = 0; i < 6; ++i) {
        EXPECT_FLOAT_EQ(out[i], want[i]) << i;
    }
}

// A universal alpha read in place from each side's own rgba row, as rgba.A.
TEST(MergeOps, BroadcastAlphaFromInterleavedChannel)
{
    const float A[8] = { 0.25f, 0.5f, 0.125f, 0.5f, 2.f, 1.f, 0.f, 1.f };
    const float B[8] = { 1.f, 0.5f, 0.25f, 1.f, 0.5f, 0.5f, 0.5f, 0.25f };
    float out[8];
    mergeRow(eATop, 4, A, AlphaSpan { A + 3, 4, 0 }, B, AlphaSpan { B + 3, 4, 0 }, 2, out);
    for (int x = 0; x < 2; ++x) {
        const float a = A[x * 4 + 3];
        const float b = B[x * 4 + 3];
        for (int c = 0; c < 4; ++c) {
            EXPECT_FLOAT_EQ(out[x * 4 + c], A[x * 4 + c] * b + B[x * 4 + c] * (1.f - a)) << x << ' ' << c;
        }
    }
}

// The fold writes the result over the result so far, its alpha read from that same row.
TEST(MergeOps, OutputMayAliasB)
{
    for (Operator o : { eOver, eScreen, eHue, eDivide }) {
        const float A[8] = { 0.25f, 0.5f, 0.125f, 0.5f, 2.f, 1.f, 0.f, 1.f };
        float B[8] = { 1.f, 0.5f, 0.25f, 1.f, 0.5f, 0.75f, 0.5f, 0.25f };
        float separate[8];
        mergeRow(o, 4, A, AlphaSpan { A + 3, 4, 0 }, B, AlphaSpan { B + 3, 4, 0 }, 2, separate);
        mergeRow(o, 4, A, AlphaSpan { A + 3, 4, 0 }, B, AlphaSpan { B + 3, 4, 0 }, 2, B);
        for (int i = 0; i < 8; ++i) {
            EXPECT_EQ(B[i], separate[i]) << operatorName(o) << ' ' << i;
        }
    }
}

// Channels are in channel-bit order: one channel is alpha, so a non-separable operator merges it
// with over, and a two-channel pixel blends as colour whose third channel reads zero.
TEST(MergeOps, NonSeparableOnFewerThanThreeChannels)
{
    const float a = 0.5f, b = 0.75f;
    {
        const float A[1] = { 0.5f };
        const float B[1] = { 0.75f };
        float out[1];
        mergeRow(eHue, 1, A, broadcast(&a), B, broadcast(&b), 1, out);
        EXPECT_FLOAT_EQ(out[0], 0.5f + 0.75f * (1.f - a));
    }
    {
        const float A2[2] = { 0.4f, 0.2f };
        const float B2[2] = { 0.1f, 0.3f };
        const float A3[3] = { 0.4f, 0.2f, 0.f };
        const float B3[3] = { 0.1f, 0.3f, 0.f };
        for (Operator o : { eHue, eSaturation, eColor, eLuminosity }) {
            float out2[2], out3[3];
            mergeRow(o, 2, A2, broadcast(&a), B2, broadcast(&b), 1, out2);
            mergeRow(o, 3, A3, broadcast(&a), B3, broadcast(&b), 1, out3);
            EXPECT_EQ(out2[0], out3[0]) << operatorName(o);
            EXPECT_EQ(out2[1], out3[1]) << operatorName(o);
        }
    }
}
