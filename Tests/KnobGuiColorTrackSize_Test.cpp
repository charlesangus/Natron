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

#include <QApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPoint>
#include <QToolButton>
#include <QUndoCommand>
#include <QUndoStack>
#include <QWidget>

#include <gtest/gtest.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"
#include "Engine/Project.h"
#include "Engine/Settings.h"
#include "Engine/ViewIdx.h"

#include "Gui/ColorAdjustPanel.h"
#include "Gui/ColorWheelWidget.h"
#include "Gui/KnobGuiColor.h"
#include "Gui/KnobGuiContainerI.h"
#include "Gui/ScaleSliderQWidget.h"

NATRON_NAMESPACE_USING

namespace {

class TrackSizeTestContainer
    : public KnobGuiContainerI {
public:
    explicit TrackSizeTestContainer(QWidget* widget)
        : KnobGuiContainerI(widget)
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

    QUndoStack undoStack;
};

class ExposedKnobGuiColor
    : public KnobGuiColor {
public:
    ExposedKnobGuiColor(KnobIPtr knob,
                        KnobGuiContainerI* container)
        : KnobGuiColor(knob, container)
    {
    }

    using KnobGuiValue::getDimensionSlider;
};

void
flushEvents()
{
    for (int i = 0; i < 3; ++i) {
        QApplication::processEvents();
    }
}

int
leftIn(const QWidget* w,
       const QWidget* top)
{
    return w->mapTo(top, QPoint(0, 0)).x();
}

QPoint
originIn(const QWidget* w,
         const QWidget* top)
{
    return w->mapTo(top, QPoint(0, 0));
}

int
rightIn(const QWidget* w,
        const QWidget* top)
{
    return leftIn(w, top) + w->width();
}

void
sendMouse(QWidget* w,
          QEvent::Type type,
          const QPoint& pos,
          Qt::MouseButton button,
          Qt::MouseButtons buttons)
{
    QMouseEvent e(type, QPointF(pos), QPointF(w->mapToGlobal(pos)), button, buttons, Qt::NoModifier);

    QApplication::sendEvent(w, &e);
}

} // namespace

// A 4-dim colour knob on the project, its GUI built the way a properties page builds it: the
// row in the field column beside a label, and the full-width row (which holds the adjust
// panel) spanning both columns below, all in a page of a fixed width.
class KnobGuiColorTrackSize
    : public ::testing::Test {
protected:
    virtual void SetUp() OVERRIDE
    {
        SettingsPtr settings = appPTR->getCurrentSettings();
        ASSERT_TRUE(bool(settings));
        _sizeSetting = settings->getKnobByNameAndType<KnobChoice>("colorTrackballSize");
        ASSERT_TRUE(bool(_sizeSetting));
        _savedSize = _sizeSetting->getValue();
        setLarge(false);
    }

    virtual void TearDown() OVERRIDE
    {
        _page.reset();
        _gui.reset();
        _container.reset();
        _knob.reset();
        if (_sizeSetting) {
            _sizeSetting->setValue(_savedSize);
        }
    }

    void setLarge(bool large)
    {
        _sizeSetting->setValue(large ? (int)Settings::eColorTrackballSizeLarge : (int)Settings::eColorTrackballSizeRegular);
        settle();
    }

    void build(int width)
    {
        AppInstancePtr app = appPTR->getTopLevelInstance();
        ASSERT_TRUE(bool(app));
        ProjectPtr project = app->getProject();
        ASSERT_TRUE(bool(project));

        _knob = AppManager::createKnob<KnobColor>(project.get(), std::string("TrackSizeGain"), 4, false);
        ASSERT_TRUE(bool(_knob));
        for (int i = 0; i < 4; ++i) {
            _knob->setValue(0.5, ViewSpec::all(), i, true);
        }

        _page.reset(new QWidget());
        _page->setFixedWidth(width);
        QGridLayout* grid = new QGridLayout(_page.get());
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(0);

        _container.reset(new TrackSizeTestContainer(_page.get()));
        _gui.reset(new ExposedKnobGuiColor(_knob, _container.get()));
        _gui->initialize();

        QLabel* label = new QLabel(QString::fromUtf8("Gain"), _page.get());
        label->setFixedWidth(80);
        grid->addWidget(label, 0, 0, Qt::AlignTop);

        QWidget* field = new QWidget(_page.get());
        QHBoxLayout* fieldLayout = new QHBoxLayout(field);
        fieldLayout->setContentsMargins(0, 0, 0, 0);
        _gui->createGUI(field, 0, 0, 0, fieldLayout, true, 0, std::vector<KnobIPtr>());
        grid->addWidget(field, 0, 1);

        QWidget* fullWidthRow = _gui->getFullWidthRowWidget();
        ASSERT_TRUE(fullWidthRow);
        fullWidthRow->setParent(_page.get());
        grid->addWidget(fullWidthRow, 1, 0, 1, 2);
        grid->setRowStretch(2, 1);

        _page->show();
        settle();
        ASSERT_TRUE(_gui->getAdjustPanel());
    }

    void setExpanded(bool expanded)
    {
        _gui->setAdjustPanelExpanded(expanded);
        settle();
    }

    void settle()
    {
        flushEvents();
        if (_page && _page->layout()) {
            _page->layout()->activate();
        }
        flushEvents();
    }

    ColorAdjustPanel* panel() const
    {
        return _gui->getAdjustPanel();
    }

    ScaleSliderQWidget* gSlider() const
    {
        return _gui->getDimensionSlider(1);
    }

    ColorWheelWidget* wheel() const
    {
        return panel()->getWheel();
    }

    KnobChoicePtr _sizeSetting;
    int _savedSize = 0;
    KnobColorPtr _knob;
    std::unique_ptr<QWidget> _page;
    std::unique_ptr<TrackSizeTestContainer> _container;
    std::shared_ptr<ExposedKnobGuiColor> _gui;
};

TEST_F(KnobGuiColorTrackSize, LargePutsTheChannelLinesRightOfABiggerWheelAndFoldingPutsThemBack)
{
    build(500);
    setExpanded(true);
    ASSERT_TRUE(gSlider());
    ASSERT_FALSE(panel()->isLargeLayout());
    ASSERT_TRUE(gSlider()->isVisible());

    const QPoint regularG = originIn(gSlider(), _page.get());
    const int regularGWidth = gSlider()->width();
    const int regularWheel = wheel()->width();
    EXPECT_LT(regularG.y(), originIn(panel(), _page.get()).y());

    setLarge(true);
    ASSERT_TRUE(panel()->isLargeLayout());
    EXPECT_TRUE(panel()->isAncestorOf(gSlider()));
    EXPECT_TRUE(gSlider()->isVisible());
    EXPECT_GE(leftIn(gSlider(), _page.get()), rightIn(wheel(), _page.get()));
    EXPECT_GT(wheel()->width(), regularWheel);
    EXPECT_LE(wheel()->width(), ColorAdjustPanel::largeMaximumDiameter());
    EXPECT_EQ(wheel()->width(), wheel()->height());
    EXPECT_GE(gSlider()->width(), ColorAdjustPanel::largeTrackMinimum());

    setExpanded(false);
    EXPECT_FALSE(panel()->isLargeLayout());
    EXPECT_FALSE(panel()->isAncestorOf(gSlider()));
    EXPECT_TRUE(gSlider()->isVisible());
    EXPECT_EQ(regularG, originIn(gSlider(), _page.get()));
    EXPECT_EQ(regularGWidth, gSlider()->width());

    // Opening again in Large goes straight to the Large layout.
    setExpanded(true);
    EXPECT_TRUE(panel()->isLargeLayout());
    EXPECT_GE(leftIn(gSlider(), _page.get()), rightIn(wheel(), _page.get()));

    // And switching the preference back live restores the Regular layout with the panel open.
    setLarge(false);
    EXPECT_FALSE(panel()->isLargeLayout());
    EXPECT_EQ(regularG, originIn(gSlider(), _page.get()));
    EXPECT_EQ(regularWheel, wheel()->width());
}

TEST_F(KnobGuiColorTrackSize, ADragOnTheGreenLineInLargeIsOneUndoStep)
{
    setLarge(true);
    build(500);
    setExpanded(true);
    ASSERT_TRUE(panel()->isLargeLayout());
    ScaleSliderQWidget* slider = gSlider();
    ASSERT_TRUE(slider);
    ASSERT_EQ(0, _container->undoStack.count());

    const double before = _knob->getValue(1);
    const int y = slider->height() / 2;
    const int x0 = slider->width() / 4;

    sendMouse(slider, QEvent::MouseButtonPress, QPoint(x0, y), Qt::LeftButton, Qt::LeftButton);
    for (int step = 1; step <= 4; ++step) {
        sendMouse(slider, QEvent::MouseMove, QPoint(x0 + 6 * step, y), Qt::NoButton, Qt::LeftButton);
    }
    sendMouse(slider, QEvent::MouseButtonRelease, QPoint(x0 + 24, y), Qt::LeftButton, Qt::NoButton);
    settle();

    EXPECT_NE(before, _knob->getValue(1));
    EXPECT_EQ(1, _container->undoStack.count());
    EXPECT_TRUE(panel()->isLargeLayout());

    _container->undoStack.undo();
    EXPECT_DOUBLE_EQ(before, _knob->getValue(1));
}

TEST_F(KnobGuiColorTrackSize, ANarrowBinFallsBackToRegular)
{
    setLarge(true);
    build(300);
    setExpanded(true);
    ASSERT_TRUE(gSlider());
    EXPECT_FALSE(panel()->isLargeLayout());
    EXPECT_FALSE(panel()->isAncestorOf(gSlider()));
    EXPECT_LE(wheel()->width(), ColorWheelWidget::maximumDiameter());
}

TEST_F(KnobGuiColorTrackSize, ThePanelToggleShowsAnUpChevronWhileOpenAndFoldsOnClick)
{
    build(500);
    QToolButton* toggle = _page->findChild<QToolButton*>(QString::fromUtf8("ColorSelectorButton"));
    ASSERT_TRUE(toggle);
    ASSERT_FALSE(_gui->isAdjustPanelExpanded());

    const QSize iconSize = toggle->iconSize();
    const QSize size = toggle->size();
    const QPoint position = toggle->pos();
    ASSERT_FALSE(toggle->icon().isNull());
    const QImage folded = toggle->icon().pixmap(iconSize).toImage();
    const QString foldedTip = toggle->toolTip();

    setExpanded(true);
    ASSERT_FALSE(toggle->icon().isNull());
    const QImage open = toggle->icon().pixmap(iconSize).toImage();
    EXPECT_NE(folded, open);
    // The up chevron is the down chevron mirrored top to bottom.
    EXPECT_EQ(folded.mirrored(false, true), open);
    EXPECT_NE(foldedTip, toggle->toolTip());
    EXPECT_TRUE(toggle->toolTip().contains(QString::fromUtf8("Hide")));
    EXPECT_EQ(size, toggle->size());
    EXPECT_EQ(position, toggle->pos());

    toggle->click();
    settle();
    EXPECT_FALSE(_gui->isAdjustPanelExpanded());
    EXPECT_FALSE(panel()->isVisible());
    EXPECT_EQ(folded, toggle->icon().pixmap(iconSize).toImage());
    EXPECT_EQ(foldedTip, toggle->toolTip());
}
