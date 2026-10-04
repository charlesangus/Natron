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

#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kCheckerBoardPluginID = "net.sf.openfx.CheckerBoardPlugin";
const char* const kGradePluginID = "net.sf.openfx.GradePlugin";

} // namespace

class RenderBothWaysSelf
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

    // CheckerBoard -> Grade x3 -> Write on a 512x512 project format; each Grade scales by a
    // different factor so the chain is not an identity. Returns the last Grade in `lastGrade`.
    void buildChain(NodePtr* lastGrade,
                    NodePtr* writer)
    {
        Format format(0, 0, 512, 512, "renderBothWaysFormat", 1.);
        getApp()->getProject()->setOrAddProjectFormat(format);

        NodePtr upstream = createNode(QString::fromUtf8(kCheckerBoardPluginID));
        ASSERT_TRUE(bool(upstream));
        const double factors[3] = { 0.5, 1.25, 0.9 };
        for (int i = 0; i < 3; ++i) {
            NodePtr grade = createNode(QString::fromUtf8(kGradePluginID));
            ASSERT_TRUE(bool(grade));
            KnobColor* multiply = dynamic_cast<KnobColor*>(grade->getKnobByName("multiply").get());
            ASSERT_TRUE(multiply != NULL);
            multiply->setValues(factors[i], factors[i], factors[i], 1., ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
            connectNodes(upstream, grade, 0, true);
            upstream = grade;
        }
        *lastGrade = upstream;

        *writer = createNode(_writeOIIOPluginID);
        ASSERT_TRUE(bool(*writer));
        connectNodes(upstream, *writer, 0, true);
    }
};

TEST_F(RenderBothWaysSelf, WriterSequenceMatches)
{
    NodePtr lastGrade;
    NodePtr writer;
    buildChain(&lastGrade, &writer);
    if (HasFatalFailure()) {
        return;
    }

    const RenderMismatch m = renderBothWays(writer, 1, 2, std::vector<int> { 1, 4 });
    EXPECT_FALSE(m.any) << describe(m);
}

TEST_F(RenderBothWaysSelf, DirectRenderMatchesAtMipmapZeroAndOne)
{
    NodePtr lastGrade;
    NodePtr writer;
    buildChain(&lastGrade, &writer);
    if (HasFatalFailure()) {
        return;
    }

    const RectI roi(0, 0, 256, 256);
    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        const RenderMismatch m = renderBothWaysDirect(lastGrade, 1., ViewIdx(0), mipmapLevel, roi, std::vector<int> { 1, 4 });
        EXPECT_FALSE(m.any) << "mipmap " << mipmapLevel << ": " << describe(m);
    }
}
