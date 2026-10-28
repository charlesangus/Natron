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

#include <memory>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/PyRoto.h"
#include "Engine/RotoCurveContext.h"

NATRON_NAMESPACE_USING

class PyRotoCreateTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }
};

// A script that creates a finished shape must see it rendered, as after setCurveFinished(true).
TEST_F(PyRotoCreateTest, FinishedShapesAreEvaluated)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    ASSERT_TRUE(bool(node));
    RotoCurveContextPtr ctx = node->getCurveContext();
    ASSERT_TRUE(bool(ctx));
    NATRON_NAMESPACE::NATRON_PYTHON_NAMESPACE::Roto roto(ctx);

    U64 age = ctx->getAge();
    std::unique_ptr<NATRON_NAMESPACE::NATRON_PYTHON_NAMESPACE::BezierCurve> square(roto.createRectangle(760., 340., 400., 1.));
    ASSERT_TRUE(square != nullptr);
    EXPECT_GT(ctx->getAge(), age);

    age = ctx->getAge();
    std::unique_ptr<NATRON_NAMESPACE::NATRON_PYTHON_NAMESPACE::BezierCurve> ellipse(roto.createEllipse(960., 540., 200., true, 1.));
    ASSERT_TRUE(ellipse != nullptr);
    EXPECT_GT(ctx->getAge(), age);
}
