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

#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Serialization/Merge/MergeHandler.h"

NATRON_NAMESPACE_USING

namespace {

Serialization::KnobSerialization
opacityKnob(double value)
{
    Serialization::KnobSerialization knob;
    knob.name = "opacity";
    knob.typeName = "Double";
    knob.dimension = 1;
    knob.dimensions.resize(1);
    knob.dimensions[0].defaultValue = Serialization::KnobValueSerialization(1.);
    knob.dimensions[0].value = Serialization::KnobValueSerialization(value);

    return knob;
}

Serialization::RotoLayerChildSerialization
shape(const std::string& name,
      const std::vector<std::pair<double, double>>& points,
      double opacity = 1.)
{
    Serialization::RotoLayerChildSerialization child;
    child.type = Serialization::eRotoLayerChildTypeBezier;
    child.bezier.name = name;
    child.bezier.label = name;
    child.bezier.activated = true;
    child.bezier.closed = true;
    for (const std::pair<double, double>& point : points) {
        Serialization::BezierCPSerialization cp;
        cp.x = point.first;
        cp.y = point.second;
        child.bezier.controlPoints.push_back(cp);
        child.bezier.featherPoints.push_back(cp);
    }
    child.bezier.knobs.push_back(opacityKnob(opacity));

    return child;
}

Serialization::RotoLayerChildSerialization
layer(const std::string& name,
      const std::vector<Serialization::RotoLayerChildSerialization>& children)
{
    Serialization::RotoLayerChildSerialization child;
    child.type = Serialization::eRotoLayerChildTypeLayer;
    child.layer.name = name;
    child.layer.children.assign(children.begin(), children.end());

    return child;
}

Serialization::RotoContextSerialization
context(const std::vector<Serialization::RotoLayerChildSerialization>& children)
{
    Serialization::RotoContextSerialization result;
    result.baseLayer.name = "Layer1";
    result.baseLayer.children.assign(children.begin(), children.end());

    return result;
}

const std::vector<std::pair<double, double>> kSquare = { { 0., 0. }, { 10., 0. }, { 10., 10. }, { 0., 10. } };

std::vector<std::pair<double, double>>
moved(std::size_t index,
      double x)
{
    std::vector<std::pair<double, double>> points = kSquare;
    points[index].first = x;

    return points;
}

std::vector<std::string>
namesOf(const Serialization::RotoLayerSerialization& layer)
{
    std::vector<std::string> result;
    for (const Serialization::RotoLayerChildSerialization& child : layer.children) {
        result.push_back((child.type == Serialization::eRotoLayerChildTypeLayer) ? child.layer.name : (child.type == Serialization::eRotoLayerChildTypeBezier) ? child.bezier.name
                                                                                                                                                               : child.stroke.name);
    }

    return result;
}

const Serialization::RotoLayerChildSerialization&
childNamed(const Serialization::RotoLayerSerialization& layer,
           const std::string& name)
{
    for (const Serialization::RotoLayerChildSerialization& child : layer.children) {
        if (((child.type == Serialization::eRotoLayerChildTypeLayer) ? child.layer.name : child.bezier.name) == name) {
            return child;
        }
    }
    static const Serialization::RotoLayerChildSerialization none;
    ADD_FAILURE() << "no item " << name;

    return none;
}

double
opacityOf(const Serialization::RotoLayerChildSerialization& child)
{
    for (const Serialization::KnobSerialization& knob : child.bezier.knobs) {
        if (knob.name == "opacity") {
            return std::get<double>(*knob.dimensions[0].value);
        }
    }

    return 1.;
}

Serialization::RotoContextSerialization
merge(const Serialization::RotoContextSerialization& base,
      const Serialization::RotoContextSerialization& mine,
      const Serialization::RotoContextSerialization& theirs,
      std::vector<Serialization::MergeConflict>* conflicts)
{
    return Serialization::mergeRotoContexts(&base, mine, theirs, Serialization::MergeHandlerRegistry::defaults(), conflicts);
}

} // namespace

TEST(RotoMerge, ShapesMergeOneByOneAndPointsBothSidesMovedConflictOnce)
{
    const Serialization::RotoContextSerialization base = context({ shape("Garbage", kSquare), shape("Core", kSquare) });
    const Serialization::RotoContextSerialization mine = context({ shape("Garbage", moved(0, -1.)), shape("ArtistFix", kSquare), shape("Core", kSquare) });
    const Serialization::RotoContextSerialization theirs = context({ shape("Garbage", moved(2, 12.)), shape("Core", kSquare, 0.5), shape("Edge", kSquare) });
    std::vector<Serialization::MergeConflict> conflicts;
    const Serialization::RotoContextSerialization merged = merge(base, mine, theirs, &conflicts);

    EXPECT_EQ(namesOf(merged.baseLayer), (std::vector<std::string> { "Garbage", "ArtistFix", "Core", "Edge" }));
    EXPECT_EQ(childNamed(merged.baseLayer, "Garbage").bezier.controlPoints.front().x, -1.);
    EXPECT_EQ(opacityOf(childNamed(merged.baseLayer, "Core")), 0.5);
    ASSERT_EQ(conflicts.size(), 1u);
    EXPECT_EQ(conflicts[0].kind, Serialization::MergeConflictKind::eRotoShape);
    EXPECT_EQ(conflicts[0].item, "Garbage");
}

TEST(RotoMerge, AShapeOneSideMovedTakesThatSide)
{
    const Serialization::RotoContextSerialization base = context({ shape("Garbage", kSquare) });
    const Serialization::RotoContextSerialization mine = context({ shape("Garbage", kSquare, 0.25) });
    const Serialization::RotoContextSerialization theirs = context({ shape("Garbage", moved(1, 20.)) });
    std::vector<Serialization::MergeConflict> conflicts;
    const Serialization::RotoContextSerialization merged = merge(base, mine, theirs, &conflicts);

    const Serialization::RotoLayerChildSerialization& garbage = childNamed(merged.baseLayer, "Garbage");
    EXPECT_EQ(std::next(garbage.bezier.controlPoints.begin())->x, 20.);
    EXPECT_EQ(opacityOf(garbage), 0.25);
    EXPECT_TRUE(conflicts.empty());
}

TEST(RotoMerge, BothSidesSettingOneItemKnobConflictsOnThatItem)
{
    const Serialization::RotoContextSerialization base = context({ shape("Core", kSquare) });
    const Serialization::RotoContextSerialization mine = context({ shape("Core", kSquare, 0.25) });
    const Serialization::RotoContextSerialization theirs = context({ shape("Core", kSquare, 0.75) });
    std::vector<Serialization::MergeConflict> conflicts;
    const Serialization::RotoContextSerialization merged = merge(base, mine, theirs, &conflicts);

    EXPECT_EQ(opacityOf(childNamed(merged.baseLayer, "Core")), 0.25);
    ASSERT_EQ(conflicts.size(), 1u);
    EXPECT_EQ(conflicts[0].kind, Serialization::MergeConflictKind::eValue);
    EXPECT_EQ(conflicts[0].item, "Core");
    EXPECT_EQ(conflicts[0].knob, "opacity");
}

TEST(RotoMerge, DeletingAShapeTheOtherSideChangedKeepsIt)
{
    const Serialization::RotoContextSerialization base = context({ shape("Garbage", kSquare), shape("Core", kSquare) });
    const Serialization::RotoContextSerialization mine = context({ shape("Garbage", kSquare) });
    const Serialization::RotoContextSerialization theirs = context({ shape("Core", kSquare, 0.5) });
    std::vector<Serialization::MergeConflict> conflicts;
    const Serialization::RotoContextSerialization merged = merge(base, mine, theirs, &conflicts);

    EXPECT_EQ(namesOf(merged.baseLayer), (std::vector<std::string> { "Core" }));
    ASSERT_EQ(conflicts.size(), 1u);
    EXPECT_EQ(conflicts[0].kind, Serialization::MergeConflictKind::eDeleteModify);
    EXPECT_EQ(conflicts[0].item, "Core");
    EXPECT_EQ(conflicts[0].side, Serialization::MergeSide::eMine);
}

TEST(RotoMerge, ShapesInsideLayersMergeByTheirLayer)
{
    const Serialization::RotoContextSerialization base = context({ layer("Hair", { shape("Strand", kSquare) }) });
    const Serialization::RotoContextSerialization mine = context({ layer("Hair", { shape("Strand", moved(0, 3.)) }) });
    const Serialization::RotoContextSerialization theirs = context({ layer("Hair", { shape("Strand", kSquare), shape("Flyaway", kSquare) }) });
    std::vector<Serialization::MergeConflict> conflicts;
    const Serialization::RotoContextSerialization merged = merge(base, mine, theirs, &conflicts);

    const Serialization::RotoLayerChildSerialization& hair = childNamed(merged.baseLayer, "Hair");
    EXPECT_EQ(namesOf(hair.layer), (std::vector<std::string> { "Strand", "Flyaway" }));
    EXPECT_EQ(childNamed(hair.layer, "Strand").bezier.controlPoints.front().x, 3.);
    EXPECT_TRUE(conflicts.empty());
}

TEST(RotoMerge, NodesWithRotoItemsChangedCountAsChanged)
{
    Serialization::NodeSerialization a;
    a.hasRotoContext = true;
    a.rotoContext = context({ shape("Core", kSquare) });
    Serialization::NodeSerialization b = a;
    EXPECT_TRUE(Serialization::sameNodeContent(a, b));
    b.rotoContext = context({ shape("Core", moved(0, 1.)) });
    EXPECT_FALSE(Serialization::sameNodeContent(a, b));
    b.rotoContext = a.rotoContext;
    b.rotoContext.selectedItems.push_back("Core");
    EXPECT_TRUE(Serialization::sameNodeContent(a, b));
}
