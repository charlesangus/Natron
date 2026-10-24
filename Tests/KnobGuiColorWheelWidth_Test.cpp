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
#include <memory>
#include <string>
#include <vector>

#include <QApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPoint>
#include <QRect>
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
#include "Engine/Settings.h"
#include "Engine/ViewIdx.h"

#include "Gui/ColorAdjustPanel.h"
#include "Gui/ColorEditSlider.h"
#include "Gui/ColorWheelWidget.h"
#include "Gui/KnobGuiColor.h"
#include "Gui/KnobGuiContainerI.h"
#include "Gui/SpinBox.h"

NATRON_NAMESPACE_USING

namespace {

class WheelWidthTestContainer
    : public KnobGuiContainerI {
public:
    explicit WheelWidthTestContainer(QWidget* widget)
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

const ColorSliderKindEnum kKinds[] = {
    eColorSliderKindHue,
    eColorSliderKindSat,
    eColorSliderKindValue,
    eColorSliderKindTemp,
    eColorSliderKindTint
};

void
flushEvents()
{
    for (int i = 0; i < 3; ++i) {
        QApplication::processEvents();
    }
}

QRect
rectIn(const QWidget* w,
       const QWidget* top)
{
    return QRect(w->mapTo(top, QPoint(0, 0)), w->size());
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

QImage
argb(const QPixmap& pix)
{
    return pix.toImage().convertToFormat(QImage::Format_ARGB32);
}

} // namespace

// A 4-dim colour knob on the project, its GUI built the way a properties page builds it: the
// row in the field column beside a label, and the full-width row (which holds the adjust
// panel) spanning both columns below, all in a page of a fixed width.
class KnobGuiColorWheelWidth
    : public ::testing::Test {
protected:
    virtual void SetUp() OVERRIDE
    {
        SettingsPtr settings = appPTR->getCurrentSettings();
        ASSERT_TRUE(bool(settings));
        _widthSetting = settings->getKnobByNameAndType<KnobChoice>("colorTrackballWheelWidth");
        ASSERT_TRUE(bool(_widthSetting));
        _savedWidth = _widthSetting->getValue();
        _widthSetting->setValue((int)Settings::eColorTrackballWheelWidthThird);
    }

    virtual void TearDown() OVERRIDE
    {
        _page.reset();
        _gui.reset();
        _container.reset();
        _knob.reset();
        if (_widthSetting) {
            _widthSetting->setValue(_savedWidth);
        }
    }

    void setWheelWidth(Settings::ColorTrackballWheelWidthEnum width)
    {
        _widthSetting->setValue((int)width);
        settle();
    }

    void build(int width)
    {
        AppInstancePtr app = appPTR->getTopLevelInstance();
        ASSERT_TRUE(bool(app));
        ProjectPtr project = app->getProject();
        ASSERT_TRUE(bool(project));

        _knob = AppManager::createKnob<KnobColor>(project.get(), std::string("WheelWidthGain"), 4, false);
        ASSERT_TRUE(bool(_knob));
        for (int i = 0; i < 4; ++i) {
            _knob->setDisplayMinimum(0., i);
            _knob->setDisplayMaximum(1., i);
            _knob->setValue(0.5, ViewSpec::all(), i, true);
        }

        _page.reset(new QWidget());
        _page->setFixedWidth(width);
        QGridLayout* grid = new QGridLayout(_page.get());
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(0);

        _container.reset(new WheelWidthTestContainer(_page.get()));
        _gui.reset(new KnobGuiColor(_knob, _container.get()));
        _gui->initialize();

        _label = new QLabel(QString::fromUtf8("Gain"), _page.get());
        _label->setFixedWidth(80);
        grid->addWidget(_label, 0, 0, Qt::AlignTop);

        _field = new QWidget(_page.get());
        QHBoxLayout* fieldLayout = new QHBoxLayout(_field);
        fieldLayout->setContentsMargins(0, 0, 0, 0);
        _gui->createGUI(_field, 0, 0, 0, fieldLayout, true, 0, std::vector<KnobIPtr>());
        grid->addWidget(_field, 0, 1);

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

    ColorWheelWidget* wheel() const
    {
        return panel()->getWheel();
    }

    QToolButton* toggle() const
    {
        return _page->findChild<QToolButton*>(QString::fromUtf8("ColorSelectorButton"));
    }

    // The column of lines beside the wheel, from the top of its first line to the bottom of
    // its last, in the panel's coordinates.
    QRect sliderColumn() const
    {
        QRect column;

        for (ColorSliderKindEnum kind : kKinds) {
            ColorEditSlider* slider = panel()->getCustomSlider(kind);
            SpinBox* box = panel()->getSpinBox(kind);
            if (slider && slider->isVisible()) {
                column = column.united(rectIn(slider, panel()));
            }
            if (box && box->isVisible()) {
                column = column.united(rectIn(box, panel()));
            }
        }

        return column;
    }

    KnobChoicePtr _widthSetting;
    int _savedWidth = 0;
    KnobColorPtr _knob;
    std::unique_ptr<QWidget> _page;
    std::unique_ptr<WheelWidthTestContainer> _container;
    std::shared_ptr<KnobGuiColor> _gui;
    QLabel* _label = 0;
    QWidget* _field = 0;
};

TEST_F(KnobGuiColorWheelWidth, TheWheelTakesItsFractionFlushLeftAndTheSlidersFillTheRest)
{
    build(900);
    setExpanded(true);
    ASSERT_TRUE(panel()->isVisible());

    const Settings::ColorTrackballWheelWidthEnum widths[] = {
        Settings::eColorTrackballWheelWidthHalf,
        Settings::eColorTrackballWheelWidthThird,
        Settings::eColorTrackballWheelWidthQuarter,
        Settings::eColorTrackballWheelWidthFifth
    };
    const double fractions[] = { 1. / 2., 1. / 3., 1. / 4., 1. / 5. };

    for (int i = 0; i < 4; ++i) {
        // Applied live, with the panel open.
        setWheelWidth(widths[i]);
        const QRect row = panel()->contentsRect();
        const int expected = (int)std::floor(row.width() * fractions[i] + 0.5);

        ASSERT_GE(expected, sliderColumn().height()) << "the row must be wide enough for the fraction to win";
        EXPECT_NEAR(expected, wheel()->width(), 2) << "fraction 1/" << (i + 2);
        EXPECT_EQ(wheel()->width(), wheel()->height());
        EXPECT_EQ(row.left(), rectIn(wheel(), panel()).left());
        for (ColorSliderKindEnum kind : kKinds) {
            ColorEditSlider* slider = panel()->getCustomSlider(kind);
            ASSERT_TRUE(slider);
            EXPECT_NEAR(row.right(), rectIn(slider, panel()).right(), 2) << "fraction 1/" << (i + 2);
            EXPECT_GT(rectIn(slider, panel()).left(), rectIn(wheel(), panel()).right());
        }
        EXPECT_NEAR(_page->width() - 1, rectIn(panel(), _page.get()).right(), 2);
    }
}

TEST_F(KnobGuiColorWheelWidth, AtANarrowWidthTheWheelIsAsTallAsTheSliderColumn)
{
    build(320);
    setWheelWidth(Settings::eColorTrackballWheelWidthFifth);
    setExpanded(true);
    ASSERT_TRUE(panel()->isVisible());

    const QRect column = sliderColumn();

    ASSERT_GT(column.height(), (int)std::floor(panel()->contentsRect().width() / 5. + 0.5));
    EXPECT_NEAR(column.height(), wheel()->height(), 2);
    EXPECT_EQ(wheel()->width(), wheel()->height());
    EXPECT_EQ(panel()->contentsRect().left(), rectIn(wheel(), panel()).left());
}

TEST_F(KnobGuiColorWheelWidth, TheOpenToggleIsTheClosedIconFlippedAndNeverASwatch)
{
    build(600);
    QToolButton* button = toggle();
    ASSERT_TRUE(button);
    ASSERT_FALSE(_gui->isAdjustPanelExpanded());

    const QSize iconSize = button->iconSize();
    const QSize size = button->size();
    const QPoint position = button->pos();
    const QString closedTip = button->toolTip();
    const QImage closed = argb(button->icon().pixmap(iconSize, QIcon::Normal, QIcon::Off));
    const QImage flipped = closed.mirrored(false, true);

    ASSERT_FALSE(closed.isNull());
    ASSERT_NE(closed, flipped) << "the dropdown glyph must not be symmetric top to bottom";

    auto expectFlipped = [&](const char* when) {
        EXPECT_EQ(flipped, argb(button->icon().pixmap(iconSize, QIcon::Normal, QIcon::On))) << when;
        EXPECT_EQ(flipped, argb(button->icon().pixmap(iconSize, QIcon::Normal, QIcon::Off))) << when;
    };

    setExpanded(true);
    ASSERT_TRUE(button->isChecked());
    expectFlipped("after opening");
    EXPECT_TRUE(button->toolTip().contains(QString::fromUtf8("Hide")));
    EXPECT_EQ(size, button->size());
    EXPECT_EQ(position, button->pos());

    for (int i = 0; i < 3; ++i) {
        _knob->setValue(0.25 + 0.1 * i, ViewSpec::all(), i, true);
    }
    settle();
    expectFlipped("after a value change");

    ColorEditSlider* slider = panel()->getCustomSlider(eColorSliderKindValue);
    ASSERT_TRUE(slider);
    const double before = _knob->getValue(0);
    const int y = slider->height() / 2;
    const int x0 = slider->width() / 4;
    sendMouse(slider, QEvent::MouseButtonPress, QPoint(x0, y), Qt::LeftButton, Qt::LeftButton);
    for (int step = 1; step <= 4; ++step) {
        sendMouse(slider, QEvent::MouseMove, QPoint(x0 + 8 * step, y), Qt::NoButton, Qt::LeftButton);
    }
    sendMouse(slider, QEvent::MouseButtonRelease, QPoint(x0 + 32, y), Qt::LeftButton, Qt::NoButton);
    settle();
    EXPECT_NE(before, _knob->getValue(0)) << "the drag should have edited the knob";
    expectFlipped("after a slider drag");

    button->click();
    settle();
    EXPECT_FALSE(_gui->isAdjustPanelExpanded());
    EXPECT_EQ(closed, argb(button->icon().pixmap(iconSize, QIcon::Normal, QIcon::Off)));
    EXPECT_EQ(closedTip, button->toolTip());
}

TEST_F(KnobGuiColorWheelWidth, AnOpenPanelAndItsRowSitInOneSunkenBlock)
{
    build(600);
    EXPECT_TRUE(!_gui->getOpenBackdrop() || !_gui->getOpenBackdrop()->isVisible());

    setExpanded(true);
    QWidget* backdrop = _gui->getOpenBackdrop();
    ASSERT_TRUE(backdrop);
    EXPECT_TRUE(backdrop->isVisible());
    EXPECT_EQ(_page.get(), backdrop->parentWidget());

    const QRect block = backdrop->geometry();
    EXPECT_TRUE(block.contains(rectIn(_label, _page.get()))) << "the label";
    EXPECT_TRUE(block.contains(rectIn(_field, _page.get()))) << "the knob's row";
    EXPECT_TRUE(block.contains(rectIn(panel(), _page.get()))) << "the panel";

    // Drawn under the row and the panel, which let it show through, and never in the way of a click.
    EXPECT_EQ(backdrop, _page->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly).value(0));
    EXPECT_TRUE(backdrop->testAttribute(Qt::WA_TransparentForMouseEvents));
    EXPECT_TRUE(_label->property("_natronColorAdjustOpen").toBool());
    EXPECT_TRUE(_field->property("_natronColorAdjustOpen").toBool());
    EXPECT_TRUE(_gui->getFullWidthRowWidget()->property("_natronColorAdjustOpen").toBool());

    setExpanded(false);
    EXPECT_TRUE(!_gui->getOpenBackdrop() || !_gui->getOpenBackdrop()->isVisible());
    EXPECT_FALSE(_label->property("_natronColorAdjustOpen").toBool());
    EXPECT_FALSE(_field->property("_natronColorAdjustOpen").toBool());
}
