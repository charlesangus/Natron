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
#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/DeepPixelOps.h"
#include "Engine/Nodes/Deep/DeepExpressionEvaluator.h"

NATRON_NAMESPACE_USING

namespace {

// A three-sample pixel with R, G, B, A and a dotted AOV, plus the Z and ZBack the view carries
// separately; sample s of channel c holds kChannelValues[c][s].
const float kChannelValues[5][3] = {
    { 0.1f, 0.2f, 0.3f },
    { 0.4f, 0.5f, 0.6f },
    { 0.7f, 0.8f, 0.9f },
    { 0.25f, 0.5f, 1.f },
    { 10.f, 20.f, 30.f },
};
const float kZ[3] = { 1.f, 2.f, 3.f };
const float kZBack[3] = { 1.5f, 2.f, 4.f };

std::vector<std::string>
channelNames()
{
    std::vector<std::string> names;

    names.push_back("R");
    names.push_back("G");
    names.push_back("B");
    names.push_back("A");
    names.push_back("diffuse.R");

    return names;
}

DeepPixelView
pixelView(bool withZBack)
{
    static const float* const channels[5] = { kChannelValues[0], kChannelValues[1], kChannelValues[2], kChannelValues[3], kChannelValues[4] };
    DeepPixelView view;

    view.z = kZ;
    view.zback = withZBack ? kZBack : nullptr;
    view.channels = channels;
    view.numChannels = 5;
    view.alphaChannelIndex = 3;
    view.numSamples = 3;

    return view;
}

// The value of source on sample sampleIndex of the fixture pixel, at (x, y) on frame.
float
eval(const std::string& source,
     int sampleIndex = 0,
     int x = 0,
     int y = 0,
     float frame = 1.f,
     bool withZBack = true)
{
    DeepExpressionEvaluator evaluator;
    DeepExpressionEvaluator::CompileError error;

    EXPECT_TRUE(evaluator.compile(source, channelNames(), &error)) << source << ": " << error.message << " (column " << error.column << ")";
    if (!evaluator.isCompiled()) {
        return NAN;
    }

    return evaluator.evaluate(pixelView(withZBack), sampleIndex, x, y, frame);
}

DeepExpressionEvaluator::CompileError
compileError(const std::string& source)
{
    DeepExpressionEvaluator evaluator;
    DeepExpressionEvaluator::CompileError error;

    EXPECT_FALSE(evaluator.compile(source, channelNames(), &error)) << source;
    EXPECT_FALSE(evaluator.isCompiled());

    return error;
}

} // namespace

TEST(DeepExpressionEvaluatorTest, LiteralsAndArithmeticFollowCPrecedence)
{
    EXPECT_FLOAT_EQ(7.f, eval("1+2*3"));
    EXPECT_FLOAT_EQ(9.f, eval("(1+2)*3"));
    EXPECT_FLOAT_EQ(2.5f, eval("10/4"));
    EXPECT_FLOAT_EQ(1.f, eval("7%3"));
    EXPECT_FLOAT_EQ(5.f, eval("2+7%3*3"));
    EXPECT_FLOAT_EQ(0.25f, eval(".25"));
    EXPECT_FLOAT_EQ(1500.f, eval("1.5e3"));
    EXPECT_FLOAT_EQ(0.001f, eval("1E-3"));
    EXPECT_FLOAT_EQ(6.f, eval("  1 +\t2\n+ 3 "));
}

TEST(DeepExpressionEvaluatorTest, PowerIsRightAssociativeAndBindsTighterThanUnaryMinus)
{
    EXPECT_FLOAT_EQ(512.f, eval("2^3^2"));
    EXPECT_FLOAT_EQ(64.f, eval("(2^3)^2"));
    EXPECT_FLOAT_EQ(-4.f, eval("-2^2"));
    EXPECT_FLOAT_EQ(4.f, eval("(-2)^2"));
    EXPECT_FLOAT_EQ(0.5f, eval("2^-1"));
    EXPECT_FLOAT_EQ(-3.f, eval("--3*-1"));
    EXPECT_FLOAT_EQ(1.f, eval("!0"));
    EXPECT_FLOAT_EQ(0.f, eval("!2.5"));
    EXPECT_FLOAT_EQ(1.f, eval("!!7"));
}

TEST(DeepExpressionEvaluatorTest, ComparisonsAndLogicYieldZeroOrOne)
{
    EXPECT_FLOAT_EQ(1.f, eval("1<2"));
    EXPECT_FLOAT_EQ(0.f, eval("2<1"));
    EXPECT_FLOAT_EQ(1.f, eval("2<=2"));
    EXPECT_FLOAT_EQ(0.f, eval("3<=2"));
    EXPECT_FLOAT_EQ(1.f, eval("3>2"));
    EXPECT_FLOAT_EQ(0.f, eval("2>2"));
    EXPECT_FLOAT_EQ(1.f, eval("2>=2"));
    EXPECT_FLOAT_EQ(0.f, eval("1>=2"));
    EXPECT_FLOAT_EQ(1.f, eval("2==2"));
    EXPECT_FLOAT_EQ(0.f, eval("2==3"));
    EXPECT_FLOAT_EQ(1.f, eval("2!=3"));
    EXPECT_FLOAT_EQ(0.f, eval("2!=2"));
    EXPECT_FLOAT_EQ(1.f, eval("1&&2"));
    EXPECT_FLOAT_EQ(0.f, eval("1&&0"));
    EXPECT_FLOAT_EQ(1.f, eval("0||0.5"));
    EXPECT_FLOAT_EQ(0.f, eval("0||0"));
    EXPECT_FLOAT_EQ(1.f, eval("1+1==2&&3>2||0"));
    EXPECT_FLOAT_EQ(1.f, eval("0&&1||1"));
    EXPECT_FLOAT_EQ(1.f, eval("1<2==1"));
}

TEST(DeepExpressionEvaluatorTest, TernarySelectsAndNestsRightward)
{
    EXPECT_FLOAT_EQ(10.f, eval("1?10:20"));
    EXPECT_FLOAT_EQ(20.f, eval("0?10:20"));
    EXPECT_FLOAT_EQ(3.f, eval("0?1:0?2:3"));
    EXPECT_FLOAT_EQ(2.f, eval("0?1:1?2:3"));
    EXPECT_FLOAT_EQ(5.f, eval("1?0?4:5:6"));
    EXPECT_FLOAT_EQ(30.f, eval("2>1 ? 10+20 : 40"));
    EXPECT_FLOAT_EQ(1.f, eval("1 ? 1 : 1/0"));
}

TEST(DeepExpressionEvaluatorTest, DivisionByZeroPassesThroughAsIEEE)
{
    EXPECT_TRUE(std::isinf(eval("1/0")));
    EXPECT_TRUE(std::isnan(eval("0/0")));
    EXPECT_TRUE(std::isinf(eval("-1/0")));
    EXPECT_FLOAT_EQ(0.f, eval("1/(1/0)"));
}

TEST(DeepExpressionEvaluatorTest, EveryFunctionGivesItsKnownValue)
{
    EXPECT_FLOAT_EQ(2.5f, eval("abs(-2.5)"));
    EXPECT_FLOAT_EQ(2.f, eval("floor(2.7)"));
    EXPECT_FLOAT_EQ(-3.f, eval("floor(-2.2)"));
    EXPECT_FLOAT_EQ(3.f, eval("ceil(2.2)"));
    EXPECT_FLOAT_EQ(3.f, eval("round(2.5)"));
    EXPECT_FLOAT_EQ(2.f, eval("round(2.4)"));
    EXPECT_FLOAT_EQ(-3.f, eval("round(-2.5)"));
    EXPECT_FLOAT_EQ(4.f, eval("sqrt(16)"));
    EXPECT_FLOAT_EQ(1.f, eval("exp(0)"));
    EXPECT_NEAR((float)M_E, eval("exp(1)"), 1e-6f);
    EXPECT_FLOAT_EQ(0.f, eval("log(1)"));
    EXPECT_NEAR(1.f, eval("log(exp(1))"), 1e-6f);
    EXPECT_FLOAT_EQ(8.f, eval("pow(2, 3)"));
    EXPECT_FLOAT_EQ(-1.f, eval("min(3, -1)"));
    EXPECT_FLOAT_EQ(3.f, eval("max(3, -1)"));
    EXPECT_FLOAT_EQ(1.f, eval("clamp(5, 0, 1)"));
    EXPECT_FLOAT_EQ(0.f, eval("clamp(-5, 0, 1)"));
    EXPECT_FLOAT_EQ(0.5f, eval("clamp(0.5, 0, 1)"));
    EXPECT_FLOAT_EQ(15.f, eval("lerp(10, 20, 0.5)"));
    EXPECT_FLOAT_EQ(10.f, eval("lerp(10, 20, 0)"));
    EXPECT_FLOAT_EQ(20.f, eval("lerp(10, 20, 1)"));
    EXPECT_FLOAT_EQ(0.f, eval("step(0.5, 0.25)"));
    EXPECT_FLOAT_EQ(1.f, eval("step(0.5, 0.5)"));
    EXPECT_FLOAT_EQ(1.f, eval("step(0.5, 0.75)"));
    EXPECT_FLOAT_EQ(0.f, eval("smoothstep(0, 1, -1)"));
    EXPECT_FLOAT_EQ(0.5f, eval("smoothstep(0, 1, 0.5)"));
    EXPECT_FLOAT_EQ(1.f, eval("smoothstep(0, 1, 2)"));
    EXPECT_FLOAT_EQ(0.15625f, eval("smoothstep(0, 1, 0.25)"));
    EXPECT_FLOAT_EQ(0.f, eval("sin(0)"));
    EXPECT_NEAR(1.f, eval("sin(pi/2)"), 1e-6f);
    EXPECT_FLOAT_EQ(1.f, eval("cos(0)"));
    EXPECT_NEAR(-1.f, eval("cos(pi)"), 1e-6f);
    EXPECT_FLOAT_EQ(0.f, eval("tan(0)"));
    EXPECT_NEAR(1.f, eval("tan(pi/4)"), 1e-6f);
    EXPECT_NEAR((float)(M_PI / 4.), eval("atan2(1, 1)"), 1e-6f);
    EXPECT_NEAR((float)(M_PI / 2.), eval("atan2(1, 0)"), 1e-6f);
    EXPECT_FLOAT_EQ(6.f, eval("max(min(10, 6), abs(-2))"));
}

TEST(DeepExpressionEvaluatorTest, BuiltinsAreBoundToTheEvaluationContext)
{
    EXPECT_FLOAT_EQ(7.f, eval("x", 0, 7, 11));
    EXPECT_FLOAT_EQ(11.f, eval("y", 0, 7, 11));
    EXPECT_FLOAT_EQ(2.f, eval("sampleIndex", 2));
    EXPECT_FLOAT_EQ(3.f, eval("sampleCount", 1));
    EXPECT_FLOAT_EQ(42.f, eval("frame", 0, 0, 0, 42.f));
    EXPECT_NEAR((float)M_PI, eval("pi"), 1e-6f);
    EXPECT_FLOAT_EQ(1.f, eval("sampleIndex==sampleCount-1", 2));
    EXPECT_FLOAT_EQ(0.f, eval("sampleIndex==sampleCount-1", 1));
}

TEST(DeepExpressionEvaluatorTest, ChannelsAndDepthsReadTheSampleByName)
{
    EXPECT_FLOAT_EQ(0.1f, eval("R", 0));
    EXPECT_FLOAT_EQ(0.2f, eval("R", 1));
    EXPECT_FLOAT_EQ(0.6f, eval("G", 2));
    EXPECT_FLOAT_EQ(0.8f, eval("B", 1));
    EXPECT_FLOAT_EQ(0.5f, eval("A", 1));
    EXPECT_FLOAT_EQ(20.f, eval("diffuse.R", 1));
    EXPECT_FLOAT_EQ(0.25f, eval("A*0.5", 1));
    EXPECT_FLOAT_EQ(0.2f + 0.5f + 20.f, eval("R+A+diffuse.R", 1));
    EXPECT_FLOAT_EQ(2.f, eval("Z", 1));
    EXPECT_FLOAT_EQ(4.f, eval("ZBack", 2));
    EXPECT_FLOAT_EQ(1.f, eval("ZBack-Z", 2));
    EXPECT_FLOAT_EQ(3.f, eval("ZBack", 2, 0, 0, 1.f, false));
    EXPECT_FLOAT_EQ(0.f, eval("ZBack>Z", 1));
    EXPECT_FLOAT_EQ(1.f, eval("ZBack>Z", 0));
}

TEST(DeepExpressionEvaluatorTest, CompileErrorsCarryAColumnAndAMessage)
{
    DeepExpressionEvaluator::CompileError error = compileError("R + nope * 2");
    EXPECT_EQ(5, error.column);
    EXPECT_NE(std::string::npos, error.message.find("unknown identifier")) << error.message;
    EXPECT_NE(std::string::npos, error.message.find("nope")) << error.message;

    error = compileError("1 + frobnicate(2)");
    EXPECT_EQ(5, error.column);
    EXPECT_NE(std::string::npos, error.message.find("unknown function")) << error.message;
    EXPECT_NE(std::string::npos, error.message.find("frobnicate")) << error.message;

    error = compileError("2 * min(1)");
    EXPECT_EQ(5, error.column);
    EXPECT_NE(std::string::npos, error.message.find("min")) << error.message;
    EXPECT_NE(std::string::npos, error.message.find("2 argument")) << error.message;

    error = compileError("clamp(1, 2, 3, 4)");
    EXPECT_EQ(1, error.column);
    EXPECT_NE(std::string::npos, error.message.find("clamp")) << error.message;

    error = compileError("sqrt()");
    EXPECT_EQ(1, error.column);
    EXPECT_NE(std::string::npos, error.message.find("sqrt")) << error.message;

    error = compileError("1 + * 2");
    EXPECT_EQ(5, error.column);
    EXPECT_NE(std::string::npos, error.message.find("unexpected")) << error.message;

    error = compileError("1 +");
    EXPECT_EQ(4, error.column);
    EXPECT_NE(std::string::npos, error.message.find("end of expression")) << error.message;

    error = compileError("(1 + 2");
    EXPECT_EQ(7, error.column);
    EXPECT_NE(std::string::npos, error.message.find("')'")) << error.message;

    error = compileError("1 + 2)");
    EXPECT_EQ(6, error.column);
    EXPECT_NE(std::string::npos, error.message.find("unexpected ')'")) << error.message;

    error = compileError("1 ? 2");
    EXPECT_EQ(6, error.column);
    EXPECT_NE(std::string::npos, error.message.find("':'")) << error.message;

    error = compileError("1 2");
    EXPECT_EQ(3, error.column);

    error = compileError("R $ 2");
    EXPECT_EQ(3, error.column);
    EXPECT_NE(std::string::npos, error.message.find("unexpected character")) << error.message;

    error = compileError("");
    EXPECT_EQ(1, error.column);

    error = compileError("   ");
    EXPECT_EQ(4, error.column);
}

TEST(DeepExpressionEvaluatorTest, ExpressionsBeyondTheStackAreRejectedAtCompileTime)
{
    // "1+(1+(1+(...(1)...)))" holds one operand per open parenthesis, so depth levels need
    // depth + 1 stack slots.
    std::string fits;
    std::string overflows;
    for (int i = 0; i < DeepExpressionEvaluator::kMaxStackDepth - 1; ++i) {
        fits += "1+(";
    }
    fits += "1" + std::string(DeepExpressionEvaluator::kMaxStackDepth - 1, ')');
    for (int i = 0; i < DeepExpressionEvaluator::kMaxStackDepth; ++i) {
        overflows += "1+(";
    }
    overflows += "1" + std::string(DeepExpressionEvaluator::kMaxStackDepth, ')');

    DeepExpressionEvaluator evaluator;
    DeepExpressionEvaluator::CompileError error;
    ASSERT_TRUE(evaluator.compile(fits, channelNames(), &error)) << error.message;
    EXPECT_EQ(DeepExpressionEvaluator::kMaxStackDepth, evaluator.getStackDepth());
    EXPECT_FLOAT_EQ((float)DeepExpressionEvaluator::kMaxStackDepth, evaluator.evaluate(pixelView(true), 0, 0, 0, 1.f));

    error = compileError(overflows);
    EXPECT_GT(error.column, 0);
    EXPECT_NE(std::string::npos, error.message.find("too deep")) << error.message;

    // Nesting that pushes nothing per level is bounded all the same.
    const std::string parens = std::string(1000, '(') + "1" + std::string(1000, ')');
    error = compileError(parens);
    EXPECT_GT(error.column, 0);
    EXPECT_NE(std::string::npos, error.message.find("nested too deeply")) << error.message;

    DeepExpressionEvaluator simple;
    ASSERT_TRUE(simple.compile("1+2*3", channelNames(), &error));
    EXPECT_EQ(3, simple.getStackDepth());
}
