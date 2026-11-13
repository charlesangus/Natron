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

#include <cstdint>
#include <vector>

#include <QFont>
#include <QFontMetrics>

#include <gtest/gtest.h>

#include "Gui/EdgeLabelMetrics.h"
#include "Gui/NodeGraphStacking.h"

NATRON_NAMESPACE_USING

namespace {

typedef std::vector<std::uint64_t> Serials;

} // anon namespace

TEST(NodeGraphStacking, OffsetsStayBelowTheNextDepthAndKeepOrder)
{
    EXPECT_EQ(0., NodeGraphStacking::offsetOf(0));
    EXPECT_LT(NodeGraphStacking::offsetOf(NodeGraphStacking::kSerialLimit), 1.);
    // Node depths are whole numbers around 20, where a double still tells every serial apart.
    for (std::uint64_t s = 1; s <= NodeGraphStacking::kSerialLimit; s += 4093) {
        EXPECT_LT(20. + NodeGraphStacking::offsetOf(s - 1), 20. + NodeGraphStacking::offsetOf(s));
    }
    EXPECT_LT(20. + NodeGraphStacking::offsetOf(NodeGraphStacking::kSerialLimit - 1),
              20. + NodeGraphStacking::offsetOf(NodeGraphStacking::kSerialLimit));
    EXPECT_LT(20. + NodeGraphStacking::offsetOf(NodeGraphStacking::kSerialLimit), 21.);
}

TEST(NodeGraphStacking, TopmostOnlyWhenTheLastRaisedInOrder)
{
    EXPECT_TRUE(NodeGraphStacking::areTopmost(Serials(), 7));
    EXPECT_TRUE(NodeGraphStacking::areTopmost(Serials { 7 }, 7));
    EXPECT_TRUE(NodeGraphStacking::areTopmost(Serials { 5, 6, 7 }, 7));
    EXPECT_FALSE(NodeGraphStacking::areTopmost(Serials { 6 }, 7));
    EXPECT_FALSE(NodeGraphStacking::areTopmost(Serials { 6, 5, 7 }, 7));
    EXPECT_FALSE(NodeGraphStacking::areTopmost(Serials { 5, 7 }, 7));
    EXPECT_FALSE(NodeGraphStacking::areTopmost(Serials { 0 }, 0));
    EXPECT_FALSE(NodeGraphStacking::areTopmost(Serials { 0, 1 }, 1));
}

TEST(NodeGraphStacking, CompactingKeepsTheOrder)
{
    const Serials before { 900, 0, 12, 500000, 0, 13, 7 };
    const Serials after = NodeGraphStacking::compacted(before);
    ASSERT_EQ(before.size(), after.size());
    EXPECT_EQ((Serials { 4, 0, 2, 5, 0, 3, 1 }), after);
    for (std::size_t i = 0; i < before.size(); ++i) {
        for (std::size_t j = 0; j < before.size(); ++j) {
            EXPECT_EQ(before[i] < before[j], after[i] < after[j]) << i << " " << j;
        }
    }
    EXPECT_TRUE(NodeGraphStacking::compacted(Serials()).empty());
    EXPECT_EQ((Serials { 0, 0 }), NodeGraphStacking::compacted(Serials { 0, 0 }));
}

TEST(EdgeLabelMetrics, MeasuresAgainOnlyWhenTextOrFontChanges)
{
    QFont font;
    font.setPointSize(11);
    EdgeLabelMetrics metrics;
    EXPECT_TRUE(metrics.update(QString::fromUtf8("Source"), font));
    const QFontMetrics fm(font);
    EXPECT_EQ(fm.horizontalAdvance(QString::fromUtf8("Source")), metrics.width());
    EXPECT_EQ(fm.height(), metrics.height());
    EXPECT_FALSE(metrics.update(QString::fromUtf8("Source"), font));

    EXPECT_TRUE(metrics.update(QString::fromUtf8("Mask"), font));
    EXPECT_EQ(fm.horizontalAdvance(QString::fromUtf8("Mask")), metrics.width());

    QFont bigger = font;
    bigger.setPointSize(30);
    EXPECT_TRUE(metrics.update(QString::fromUtf8("Mask"), bigger));
    EXPECT_EQ(QFontMetrics(bigger).horizontalAdvance(QString::fromUtf8("Mask")), metrics.width());
    EXPECT_EQ(QFontMetrics(bigger).height(), metrics.height());
    EXPECT_FALSE(metrics.update(QString::fromUtf8("Mask"), bigger));
}
