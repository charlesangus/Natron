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
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QString>
#include <QToolButton>
#include <QUndoCommand>
#include <QUndoStack>
#include <QWidget>

#include <gtest/gtest.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ColorWheelMath.h"
#include "Engine/KnobTypes.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

#include "Gui/ColorAdjustPanel.h"
#include "Gui/ColorPanelLayoutDev.h"
#include "Gui/ColorTrackballBinPanel.h"
#include "Gui/KnobGuiColor.h"
#include "Gui/KnobGuiContainerI.h"

NATRON_NAMESPACE_USING

namespace {

// A container with a widget and a real QUndoStack and no Gui: the knob finds the bin-top panel
// constructed for no Gui, and the panel's gestures land on this stack.
class BinTestContainer
    : public KnobGuiContainerI {
public:
    explicit BinTestContainer(QWidget* widget)
        : KnobGuiContainerI(widget)
        , undoStack()
    {
    }

    virtual Gui* getGui() const OVERRIDE FINAL
    {
        return 0;
    }

    virtual const QUndoCommand* getLastUndoCommand() const OVERRIDE FINAL
    {
        return undoStack.index() > 0 ? undoStack.command(undoStack.index() - 1) : 0;
    }

    virtual void pushUndoCommand(QUndoCommand* cmd) OVERRIDE FINAL
    {
        undoStack.push(cmd);
    }

    virtual KnobGuiPtr getKnobGui(const KnobIPtr& /*knob*/) const OVERRIDE FINAL
    {
        return KnobGuiPtr();
    }

    virtual int getItemsSpacingOnSameLine() const OVERRIDE FINAL
    {
        return 0;
    }

    mutable QUndoStack undoStack;
};

void
flushEvents()
{
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::processEvents();
    }
}

KnobColorValue
makeValue(double r,
          double g,
          double b,
          double a)
{
    KnobColorValue v;

    v.rgb[0] = r;
    v.rgb[1] = g;
    v.rgb[2] = b;
    v.a = a;
    v.hasAlpha = true;

    return v;
}

// One colour knob with its full row built, as a node panel builds it: the field, the ▾ and the
// adjust panel in the column below.
struct BuiltColorKnob {
    std::unique_ptr<QWidget> page;
    std::unique_ptr<BinTestContainer> container;
    KnobColorPtr knob;
    std::shared_ptr<KnobGuiColor> gui;

    ~BuiltColorKnob()
    {
        // The widgets go first so nothing can reach them through the GUI object after.
        page.reset();
        gui.reset();
        container.reset();
    }
};

std::unique_ptr<BuiltColorKnob>
buildColorKnob(const std::string& name)
{
    AppInstancePtr app = appPTR->getTopLevelInstance();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();

    if (!project) {
        return std::unique_ptr<BuiltColorKnob>();
    }

    std::unique_ptr<BuiltColorKnob> built(new BuiltColorKnob);

    built->knob = AppManager::createKnob<KnobColor>(project.get(), name, 4, false);
    if (!built->knob) {
        return std::unique_ptr<BuiltColorKnob>();
    }
    for (int i = 0; i < 4; ++i) {
        built->knob->setValue(0.5, ViewSpec::all(), i, true);
    }
    built->page.reset(new QWidget());
    built->container.reset(new BinTestContainer(built->page.get()));
    built->gui = std::make_shared<KnobGuiColor>(built->knob, built->container.get());
    built->gui->initialize();

    QWidget* field = new QWidget(built->page.get());
    QHBoxLayout* layout = new QHBoxLayout(field);

    built->gui->createGUI(field, 0, 0, 0, layout, true, 0, std::vector<KnobIPtr>());
    flushEvents();

    return built;
}

// Puts the switch's default layout back however the test ends.
class PlacementScope {
public:
    explicit PlacementScope(ColorPanelLayout::Placement placement)
    {
        ColorPanelLayoutDev::reloadForTests();
        ColorPanelLayoutDev::instance()->setPlacement(placement);
    }

    ~PlacementScope()
    {
        ColorPanelLayoutDev::reloadForTests();
    }
};

} // namespace

TEST(ColorTrackballBinPanel, TitleIsNodeDotKnobOrTheKnobAlone)
{
    EXPECT_EQ(QString::fromUtf8("Grade1.gain"), colorTrackballBinPanelTitle(QString::fromUtf8("Grade1"), QString::fromUtf8("gain")));
    EXPECT_EQ(QString::fromUtf8("gain"), colorTrackballBinPanelTitle(QString(), QString::fromUtf8("gain")));
}

TEST(ColorTrackballBinPanel, StartsHiddenAndUnbound)
{
    QWidget bin;
    ColorTrackballBinPanel panel(0, &bin);

    EXPECT_TRUE(panel.isHidden());
    EXPECT_FALSE(panel.isBound());
    EXPECT_EQ(&panel, ColorTrackballBinPanel::instanceFor(0));
}

TEST(ColorTrackballBinPanel, ExpandingInBinTopBindsThePanelAndKeepsTheInlinePanelHidden)
{
    PlacementScope placement(ColorPanelLayout::Placement::BinTop);
    QWidget bin;
    ColorTrackballBinPanel panel(0, &bin);
    std::unique_ptr<BuiltColorKnob> white = buildColorKnob("White");

    ASSERT_TRUE(bool(white));
    ASSERT_TRUE(white->gui->getAdjustPanel() != 0);

    white->gui->setAdjustPanelExpanded(true);
    flushEvents();

    EXPECT_EQ(white->gui.get(), panel.getBoundKnob());
    EXPECT_FALSE(panel.isHidden());
    EXPECT_EQ(QString::fromUtf8("White"), panel.getTitle());
    EXPECT_TRUE(white->gui->isAdjustPanelExpanded());
    EXPECT_TRUE(white->gui->getAllDimensionsVisible());
    EXPECT_TRUE(white->gui->getAdjustPanel()->isHidden());
    EXPECT_EQ(4, panel.getAdjustPanel()->getDimensions());

    white->gui->setAdjustPanelExpanded(false);

    EXPECT_FALSE(panel.isBound());
    EXPECT_TRUE(panel.isHidden());
}

TEST(ColorTrackballBinPanel, InKnobPlacementLeavesThePanelAlone)
{
    PlacementScope placement(ColorPanelLayout::Placement::InKnob);
    QWidget bin;
    ColorTrackballBinPanel panel(0, &bin);
    std::unique_ptr<BuiltColorKnob> white = buildColorKnob("White");

    ASSERT_TRUE(bool(white));
    white->gui->setAdjustPanelExpanded(true);
    flushEvents();

    EXPECT_FALSE(panel.isBound());
    EXPECT_TRUE(panel.isHidden());
    EXPECT_FALSE(white->gui->getAdjustPanel()->isHidden());
}

TEST(ColorTrackballBinPanel, AGestureInThePanelIsOneUndoStepOnTheBoundKnob)
{
    PlacementScope placement(ColorPanelLayout::Placement::BinTop);
    QWidget bin;
    ColorTrackballBinPanel panel(0, &bin);
    std::unique_ptr<BuiltColorKnob> white = buildColorKnob("White");

    ASSERT_TRUE(bool(white));
    ASSERT_TRUE(panel.bindTo(white->gui.get()));

    const QString label = QString::fromUtf8("Trackball");
    ColorAdjustPanel* adjust = panel.getAdjustPanel();

    Q_EMIT adjust->valuesEdited(makeValue(0.5, 0.5, 0.5, 0.5), ColorAdjustPanel::eGesturePhaseStart, label);
    Q_EMIT adjust->valuesEdited(makeValue(0.6, 0.5, 0.5, 0.5), ColorAdjustPanel::eGesturePhaseMove, label);
    Q_EMIT adjust->valuesEdited(makeValue(0.7, 0.5, 0.5, 0.5), ColorAdjustPanel::eGesturePhaseMove, label);
    Q_EMIT adjust->valuesEdited(makeValue(0.7, 0.5, 0.5, 0.5), ColorAdjustPanel::eGesturePhaseEnd, label);

    EXPECT_EQ(1, white->container->undoStack.count());
    EXPECT_DOUBLE_EQ(0.7, white->knob->getValue(0));
    EXPECT_DOUBLE_EQ(0.7, adjust->getValue().rgb[0]);

    white->container->undoStack.undo();
    flushEvents();

    EXPECT_DOUBLE_EQ(0.5, white->knob->getValue(0));
    EXPECT_DOUBLE_EQ(0.5, adjust->getValue().rgb[0]);
}

TEST(ColorTrackballBinPanel, DeletingTheBoundKnobsWidgetsUnbindsWithoutTouchingThem)
{
    PlacementScope placement(ColorPanelLayout::Placement::BinTop);
    QWidget bin;
    ColorTrackballBinPanel panel(0, &bin);
    std::unique_ptr<BuiltColorKnob> white = buildColorKnob("White");

    ASSERT_TRUE(bool(white));
    ASSERT_TRUE(panel.bindTo(white->gui.get()));

    // The row's ▾ is deleted before the adjust panel below it, whose destroyed() still finds
    // the knob GUI alive and its pointer to the ▾ dangling.
    white->page.reset();

    EXPECT_FALSE(panel.isBound());
    EXPECT_TRUE(panel.isHidden());
}

TEST(ColorTrackballBinPanel, OnlyOneKnobIsBoundAtATime)
{
    PlacementScope placement(ColorPanelLayout::Placement::BinTop);
    QWidget bin;
    ColorTrackballBinPanel panel(0, &bin);
    std::unique_ptr<BuiltColorKnob> white = buildColorKnob("White");
    std::unique_ptr<BuiltColorKnob> black = buildColorKnob("Black");

    ASSERT_TRUE(bool(white));
    ASSERT_TRUE(bool(black));

    white->gui->setAdjustPanelExpanded(true);
    black->gui->setAdjustPanelExpanded(true);
    flushEvents();

    EXPECT_EQ(black->gui.get(), panel.getBoundKnob());
    EXPECT_EQ(QString::fromUtf8("Black"), panel.getTitle());
    EXPECT_FALSE(white->gui->isAdjustPanelExpanded());
    EXPECT_TRUE(black->gui->isAdjustPanelExpanded());
}

TEST(ColorTrackballBinPanel, FollowingTheTopNodeRebindsOrHides)
{
    PlacementScope placement(ColorPanelLayout::Placement::BinTop);
    QWidget bin;
    ColorTrackballBinPanel panel(0, &bin);
    std::unique_ptr<BuiltColorKnob> white = buildColorKnob("White");
    std::unique_ptr<BuiltColorKnob> black = buildColorKnob("Black");

    ASSERT_TRUE(bool(white));
    ASSERT_TRUE(bool(black));

    // Unbound, a top-node change does nothing.
    panel.followTopNode(white->gui.get());
    EXPECT_FALSE(panel.isBound());

    ASSERT_TRUE(panel.bindTo(white->gui.get()));
    panel.followTopNode(black->gui.get());

    EXPECT_EQ(black->gui.get(), panel.getBoundKnob());
    EXPECT_FALSE(white->gui->isAdjustPanelExpanded());
    EXPECT_TRUE(black->gui->isAdjustPanelExpanded());

    // A top node with no colour knob to edit.
    panel.followTopNode(0);

    EXPECT_FALSE(panel.isBound());
    EXPECT_TRUE(panel.isHidden());
    EXPECT_FALSE(black->gui->isAdjustPanelExpanded());
}

TEST(ColorTrackballBinPanel, TheCloseButtonUnbindsAndUnchecksTheKnob)
{
    PlacementScope placement(ColorPanelLayout::Placement::BinTop);
    QWidget bin;
    ColorTrackballBinPanel panel(0, &bin);
    std::unique_ptr<BuiltColorKnob> white = buildColorKnob("White");

    ASSERT_TRUE(bool(white));
    white->gui->setAdjustPanelExpanded(true);
    ASSERT_TRUE(panel.isBound());

    panel.getCloseButton()->click();

    EXPECT_FALSE(panel.isBound());
    EXPECT_TRUE(panel.isHidden());
    EXPECT_FALSE(white->gui->isAdjustPanelExpanded());
}

TEST(ColorTrackballBinPanel, DestroyingTheKnobsWidgetsUnbinds)
{
    PlacementScope placement(ColorPanelLayout::Placement::BinTop);
    QWidget bin;
    ColorTrackballBinPanel panel(0, &bin);
    std::unique_ptr<BuiltColorKnob> white = buildColorKnob("White");

    ASSERT_TRUE(bool(white));
    ASSERT_TRUE(panel.bindTo(white->gui.get()));

    white->page.reset();
    flushEvents();

    EXPECT_FALSE(panel.isBound());
    EXPECT_TRUE(panel.isHidden());

    white.reset();
    flushEvents();

    EXPECT_EQ(0, panel.getBoundKnob());
}

TEST(ColorTrackballBinPanel, SwitchingBackToInKnobHandsTheEditingBackToTheKnob)
{
    PlacementScope placement(ColorPanelLayout::Placement::BinTop);
    QWidget bin;
    ColorTrackballBinPanel panel(0, &bin);
    std::unique_ptr<BuiltColorKnob> white = buildColorKnob("White");

    ASSERT_TRUE(bool(white));
    white->gui->setAdjustPanelExpanded(true);
    ASSERT_TRUE(panel.isBound());
    ASSERT_TRUE(white->gui->getAdjustPanel()->isHidden());

    ColorPanelLayoutDev::instance()->setPlacement(ColorPanelLayout::Placement::InKnob);
    flushEvents();

    EXPECT_FALSE(panel.isBound());
    EXPECT_TRUE(panel.isHidden());
    EXPECT_TRUE(white->gui->isAdjustPanelExpanded());
    EXPECT_FALSE(white->gui->getAdjustPanel()->isHidden());

    ColorPanelLayoutDev::instance()->setPlacement(ColorPanelLayout::Placement::BinTop);
    flushEvents();

    EXPECT_EQ(white->gui.get(), panel.getBoundKnob());
    EXPECT_TRUE(white->gui->getAdjustPanel()->isHidden());
}
