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

#include "ColorPanelLayoutDev.h"

#include <functional>
#include <utility>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QString>
#include <QWidget>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppManager.h"

NATRON_NAMESPACE_ENTER

namespace {
const char kDevEnvVar[] = "NATRON_COLOR_PANEL_DEV";

bool
readDevEnv()
{
    QByteArray v = qgetenv(kDevEnvVar).trimmed();

    return !v.isEmpty() && (v != "0");
}

// -1 until the first read.
int s_devEnabled = -1;

QCheckBox*
addToggle(QHBoxLayout* layout,
          const char* objectName,
          const QString& text,
          const QString& toolTip,
          const std::function<bool(const ColorPanelLayout&)>& get,
          const std::function<void(bool)>& set)
{
    QCheckBox* box = new QCheckBox(text, layout->parentWidget());

    box->setObjectName(QString::fromUtf8(objectName));
    box->setToolTip(toolTip);
    box->setFocusPolicy(Qt::NoFocus);
    box->setChecked(get(ColorPanelLayoutDev::instance()->getLayout()));
    QObject::connect(box, &QCheckBox::toggled, box, [set](bool checked) {
        set(checked);
    });
    QObject::connect(ColorPanelLayoutDev::instance(), &ColorPanelLayoutDev::layoutChanged, box, [box, get]() {
        QSignalBlocker blocker(box);

        box->setChecked(get(ColorPanelLayoutDev::instance()->getLayout()));
    });
    layout->addWidget(box);

    return box;
}

// Each item's data is the enum value as an int, so the combo's order is free.
QComboBox*
addChoice(QHBoxLayout* layout,
          const char* objectName,
          const QString& toolTip,
          const std::vector<std::pair<QString, int>>& items,
          const std::function<int(const ColorPanelLayout&)>& get,
          const std::function<void(int)>& set)
{
    QComboBox* combo = new QComboBox(layout->parentWidget());

    combo->setObjectName(QString::fromUtf8(objectName));
    combo->setToolTip(toolTip);
    combo->setFocusPolicy(Qt::NoFocus);
    for (const std::pair<QString, int>& item : items) {
        combo->addItem(item.first, item.second);
    }
    combo->setCurrentIndex(combo->findData(get(ColorPanelLayoutDev::instance()->getLayout())));
    QObject::connect(combo, &QComboBox::currentIndexChanged, combo, [combo, set](int index) {
        if (index >= 0) {
            set(combo->itemData(index).toInt());
        }
    });
    QObject::connect(ColorPanelLayoutDev::instance(), &ColorPanelLayoutDev::layoutChanged, combo, [combo, get]() {
        QSignalBlocker blocker(combo);

        combo->setCurrentIndex(combo->findData(get(ColorPanelLayoutDev::instance()->getLayout())));
    });
    layout->addWidget(combo);

    return combo;
}
} // namespace

bool
ColorPanelLayout::operator==(const ColorPanelLayout& other) const
{
    return (extendLeft == other.extendLeft) && (extendRight == other.extendRight) && (style == other.style);
}

bool
ColorPanelLayout::operator!=(const ColorPanelLayout& other) const
{
    return !(*this == other);
}

ColorPanelLayoutDev::ColorPanelLayoutDev()
    : QObject()
    , _layout()
{
}

ColorPanelLayoutDev*
ColorPanelLayoutDev::instance()
{
    // Never deleted: panels connect to it until the very end of the process.
    static ColorPanelLayoutDev* s_instance = new ColorPanelLayoutDev();

    return s_instance;
}

bool
ColorPanelLayoutDev::isDevEnabled()
{
    if (s_devEnabled < 0) {
        s_devEnabled = readDevEnv() ? 1 : 0;
    }

    return s_devEnabled == 1;
}

void
ColorPanelLayoutDev::reloadForTests()
{
    s_devEnabled = readDevEnv() ? 1 : 0;
    instance()->_layout = ColorPanelLayout();
}

QWidget*
ColorPanelLayoutDev::createDevBar(QWidget* parent)
{
    if (!isDevEnabled()) {
        return nullptr;
    }

    QWidget* bar = new QWidget(parent);

    bar->setObjectName(QString::fromUtf8("ColorPanelDevBar"));

    QHBoxLayout* layout = new QHBoxLayout(bar);

    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing((int)TO_DPIX(6.));

    QLabel* title = new QLabel(tr("Layout:"), bar);

    title->setToolTip(tr("Development switch (NATRON_COLOR_PANEL_DEV): applies to every colour panel at once."));
    layout->addWidget(title);

    addToggle(layout, "ColorPanelDevExtendLeft", tr("Left"), tr("Start the panel in the label column."), [](const ColorPanelLayout& l) { return l.extendLeft; }, [](bool v) { ColorPanelLayoutDev::instance()->setExtendLeft(v); });
    addToggle(layout, "ColorPanelDevExtendRight", tr("Right"), tr("Run the panel's sliders on under the row's buttons."), [](const ColorPanelLayout& l) { return l.extendRight; }, [](bool v) { ColorPanelLayoutDev::instance()->setExtendRight(v); });
    addChoice(layout, "ColorPanelDevStyle", tr("Slider style"), { { tr("Natron sliders"), (int)ColorPanelLayout::SliderStyle::Natron }, { tr("Custom sliders"), (int)ColorPanelLayout::SliderStyle::Custom }, { tr("Vertical sliders"), (int)ColorPanelLayout::SliderStyle::Vertical } }, [](const ColorPanelLayout& l) { return (int)l.style; }, [](int v) { ColorPanelLayoutDev::instance()->setSliderStyle((ColorPanelLayout::SliderStyle)v); });
    layout->addStretch(1);

    return bar;
}

const ColorPanelLayout&
ColorPanelLayoutDev::getLayout() const
{
    return _layout;
}

void
ColorPanelLayoutDev::setLayout(const ColorPanelLayout& layout)
{
    if (layout == _layout) {
        return;
    }
    _layout = layout;
    Q_EMIT layoutChanged();
}

void
ColorPanelLayoutDev::setExtendLeft(bool extend)
{
    ColorPanelLayout l = _layout;

    l.extendLeft = extend;
    setLayout(l);
}

void
ColorPanelLayoutDev::setExtendRight(bool extend)
{
    ColorPanelLayout l = _layout;

    l.extendRight = extend;
    setLayout(l);
}

void
ColorPanelLayoutDev::setSliderStyle(ColorPanelLayout::SliderStyle style)
{
    ColorPanelLayout l = _layout;

    l.style = style;
    setLayout(l);
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_ColorPanelLayoutDev.cpp"
