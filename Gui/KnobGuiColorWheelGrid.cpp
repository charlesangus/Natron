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

#include "KnobGuiColorWheelGrid.h"

#include <algorithm>
#include <list>
#include <vector>

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QUndoCommand>
#include <QVBoxLayout>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Knob.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/Settings.h"

#include "Gui/ColorWheelWidget.h"
#include "Gui/Gui.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/GuiDefines.h"
#include "Gui/KnobUndoCommand.h"
#include "Gui/ScaleSliderQWidget.h"

NATRON_NAMESPACE_ENTER

namespace {

ColorWheelWidget::DisplayConverter
makeDisplayConverter(const KnobIPtr& knob)
{
    KnobHolder* holder = knob ? knob->getHolder() : 0;
    AppInstancePtr app = holder ? holder->getApp() : AppInstancePtr();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    ProjectColorManagementPtr colorManagement = project ? project->getColorManagement() : ProjectColorManagementPtr();

    if (!colorManagement) {
        return ColorWheelWidget::DisplayConverter();
    }
    std::weak_ptr<ProjectColorManagement> weak = colorManagement;

    return [weak](float* rgb, int count) {
        ProjectColorManagementPtr cm = weak.lock();

        if (!cm) {
            return;
        }
        for (int i = 0; i < count; ++i) {
            cm->workingToColorPicking(&rgb[3 * i], &rgb[3 * i + 1], &rgb[3 * i + 2]);
        }
    };
}

// Whether a gesture may write the knob: one driven by an expression or a link is left alone.
bool
isWritable(const KnobDoublePtr& knob)
{
    return knob && knob->getExpression(0).empty() && !knob->isSlave(0);
}

/**
 * @brief One wheel drag's edits of the hue and chroma knobs, a KnobUndoCommand per knob, undone
 * in reverse. Later steps of the same gesture fold into it, each knob's into its own command;
 * one that will not fold (it added a keyframe) is kept after it.
 **/
class ColorWheelEditCommand
    : public QUndoCommand,
      public UndoGestureMember {
public:
    ColorWheelEditCommand(quint64 gestureKey,
                          const QString& text)
        : QUndoCommand()
        , _steps()
    {
        setGestureKey(gestureKey);
        setText(text);
    }

    virtual ~ColorWheelEditCommand() OVERRIDE
    {
        for (const Step& s : _steps) {
            delete s.command;
        }
    }

    // slot tells the knobs apart: 0 hue, 1 chroma.
    void addStep(int slot,
                 QUndoCommand* command)
    {
        command->setText(text());
        _steps.push_back(Step { slot, command });
    }

    bool isEmpty() const
    {
        return _steps.empty();
    }

    virtual void redo() OVERRIDE FINAL
    {
        for (const Step& s : _steps) {
            s.command->redo();
        }
    }

    virtual void undo() OVERRIDE FINAL
    {
        for (std::vector<Step>::reverse_iterator it = _steps.rbegin(); it != _steps.rend(); ++it) {
            it->command->undo();
        }
    }

    virtual int id() const OVERRIDE FINAL
    {
        return kColorWheelEditCommandCompressionID;
    }

    virtual bool mergeWith(const QUndoCommand* command) OVERRIDE FINAL
    {
        const ColorWheelEditCommand* other = dynamic_cast<const ColorWheelEditCommand*>(command);

        if (!other || !inSameGestureAs(*other)) {
            return false;
        }
        // The stack deletes other once it has folded in, so the steps that do not fold are
        // taken from it rather than copied.
        ColorWheelEditCommand* source = const_cast<ColorWheelEditCommand*>(other);
        for (Step& incoming : source->_steps) {
            QUndoCommand* last = 0;
            for (std::vector<Step>::reverse_iterator it = _steps.rbegin(); it != _steps.rend(); ++it) {
                if (it->slot == incoming.slot) {
                    last = it->command;
                    break;
                }
            }
            if (last && (last->id() == incoming.command->id()) && last->mergeWith(incoming.command)) {
                delete incoming.command;
                incoming.command = 0;
                continue;
            }
            _steps.push_back(incoming);
            incoming.command = 0;
        }
        source->_steps.clear();

        return true;
    }

private:
    struct Step {
        int slot;
        QUndoCommand* command;
    };

    std::vector<Step> _steps;
};
} // anonymous namespace

KnobGuiColorWheelGrid::KnobGuiColorWheelGrid(KnobIPtr knob,
                                             KnobGuiContainerI* container)
    : KnobGui(knob, container)
    , _knob(std::dynamic_pointer_cast<KnobSeparator>(knob))
    , _line()
    , _grid()
    , _cells()
    , _activeCell(-1)
    , _gesture()
    , _value()
    , _moved(false)
    , _draftGui()
{
}

KnobGuiColorWheelGrid::~KnobGuiColorWheelGrid()
{
    if (_draftGui) {
        _draftGui->setDraftRenderEnabled(false);
    }
}

bool
KnobGuiColorWheelGrid::isLabelOnSameColumn() const
{
    return true;
}

bool
KnobGuiColorWheelGrid::isLabelBold() const
{
    return true;
}

KnobIPtr
KnobGuiColorWheelGrid::getKnob() const
{
    return _knob.lock();
}

QWidget*
KnobGuiColorWheelGrid::getFullWidthRowWidget() const
{
    return _grid.data();
}

void
KnobGuiColorWheelGrid::createWidget(QHBoxLayout* layout)
{
    QWidget* parent = layout->parentWidget();

    parent->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    QFrame* line = new QFrame(parent);
    line->setFixedHeight(TO_DPIY(2));
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    line->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(line);
    _line = line;

    KnobSeparatorPtr knob = _knob.lock();
    const std::vector<KnobSeparator::ColorWheelSpec> specs = knob ? knob->getColorWheelGrid() : std::vector<KnobSeparator::ColorWheelSpec>();
    QWidget* grid = new QWidget(parent);
    grid->setObjectName(QString::fromUtf8("ColorWheelGrid_") + QString::fromUtf8(knob ? knob->getName().c_str() : ""));
    QGridLayout* gridLayout = new QGridLayout(grid);
    gridLayout->setContentsMargins(0, TO_DPIY(4), 0, TO_DPIY(4));
    gridLayout->setHorizontalSpacing(TO_DPIX(12));
    gridLayout->setVerticalSpacing(TO_DPIY(8));
    _cells.clear();
    for (std::size_t i = 0; i < specs.size(); ++i) {
        Cell cell;
        cell.spec = specs[i];
        _cells.push_back(cell);
    }
    for (std::size_t i = 0; i < _cells.size(); ++i) {
        gridLayout->addWidget(createCell((int)i, grid), (int)i / 2, (int)i % 2, Qt::AlignHCenter | Qt::AlignTop);
    }
    _grid = grid;
    for (std::size_t i = 0; i < _cells.size(); ++i) {
        refreshCell((int)i);
    }
} // KnobGuiColorWheelGrid::createWidget

QWidget*
KnobGuiColorWheelGrid::createCell(int index,
                                  QWidget* parent)
{
    const KnobSeparator::ColorWheelSpec& spec = _cells[index].spec;
    KnobDoublePtr hue = spec.hue.lock();
    KnobDoublePtr chroma = spec.chroma.lock();
    KnobDoublePtr luminance = spec.luminance.lock();
    const QString name = QString::fromUtf8(chroma ? chroma->getName().c_str() : "");
    const int diameter = (int)TO_DPIX(kWheelDiameter);

    QWidget* cell = new QWidget(parent);
    cell->setObjectName(QString::fromUtf8("ColorWheelCell_") + name);
    QVBoxLayout* column = new QVBoxLayout(cell);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(TO_DPIY(2));

    QLabel* title = new QLabel(QString::fromUtf8(spec.label.c_str()), cell);
    title->setObjectName(QString::fromUtf8("ColorWheelTitle_") + name);
    title->setAlignment(Qt::AlignHCenter);
    column->addWidget(title);

    ColorWheelWidget* wheel = new ColorWheelWidget(cell);
    wheel->setObjectName(QString::fromUtf8("ColorWheel_") + name);
    wheel->setToolTip(tr("Drag to set %1 (the angle) and %2 (the distance from the centre). "
                         "Drag the ring to set the hue alone, double-click to reset the chroma, and scroll to zoom.")
                          .arg(QString::fromUtf8(hue ? hue->getLabel().c_str() : ""))
                          .arg(QString::fromUtf8(chroma ? chroma->getLabel().c_str() : "")));
    wheel->setDisplayConverter(makeDisplayConverter(chroma));
    wheel->setPolarMode(spec.colour);
    if (chroma) {
        wheel->setRim(chroma->getDisplayMaximum());
    }
    wheel->setMinimumDiameter(diameter);
    wheel->setMaximumDiameter(diameter);
    wheel->setPreferredDiameter(diameter);
    wheel->setFixedSize(diameter, diameter);
    QObject::connect(wheel, &ColorWheelWidget::gestureStarted, this, [this, index]() {
        onGestureStarted(index);
    });
    QObject::connect(wheel, &ColorWheelWidget::gestureMoved, this, [this, index](ColorEdit edit, QString) {
        onGestureMoved(index, edit);
    });
    QObject::connect(wheel, &ColorWheelWidget::gestureFinished, this, [this](bool) {
        onGestureFinished();
    });
    column->addWidget(wheel, 0, Qt::AlignHCenter);

    const double low = luminance ? luminance->getDisplayMinimum() : -1.;
    const double high = luminance ? luminance->getDisplayMaximum() : 1.;
    ScaleSliderQWidget* slider = new ScaleSliderQWidget(low, high, luminance ? luminance->getValue() : 0.,
                                                        luminance ? luminance->getEvaluateOnChange() : true,
                                                        ScaleSliderQWidget::eDataTypeDouble, getGui(), eScaleTypeLinear, cell);
    slider->setObjectName(QString::fromUtf8("ColorWheelLuminance_") + name);
    slider->setFixedWidth(diameter);
    slider->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    slider->setToolTip(QString::fromUtf8(luminance ? luminance->getLabel().c_str() : ""));
    QObject::connect(slider, &ScaleSliderQWidget::positionChanged, this, [this, index](double v) {
        if (!appPTR->getCurrentSettings()->getRenderOnEditingFinishedOnly()) {
            onLuminanceSlid(index, v);
        }
    });
    QObject::connect(slider, &ScaleSliderQWidget::editingFinished, this, [this, index](bool) {
        ScaleSliderQWidget* s = _cells[index].slider.data();
        if (s && appPTR->getCurrentSettings()->getRenderOnEditingFinishedOnly()) {
            onLuminanceSlid(index, s->getPosition());
        }
    });
    QObject::connect(slider, &ScaleSliderQWidget::resetToDefaultRequested, this, [this, index]() {
        onLuminanceReset(index);
    });
    column->addWidget(slider, 0, Qt::AlignHCenter);

    QLabel* readout = new QLabel(cell);
    readout->setObjectName(QString::fromUtf8("ColorWheelReadout_") + name);
    readout->setAlignment(Qt::AlignHCenter);
    column->addWidget(readout);

    _cells[index].widget = cell;
    _cells[index].wheel = wheel;
    _cells[index].slider = slider;
    _cells[index].readout = readout;

    const KnobDoublePtr knobs[3] = { hue, chroma, luminance };
    for (const KnobDoublePtr& k : knobs) {
        KnobSignalSlotHandlerPtr handler = k ? k->getSignalSlotHandler() : KnobSignalSlotHandlerPtr();
        if (handler) {
            QObject::connect(handler.get(), &KnobSignalSlotHandler::valueChanged, this, [this, index](ViewSpec, int, int) {
                refreshCell(index);
            });
        }
    }

    return cell;
} // KnobGuiColorWheelGrid::createCell

PolarValue
KnobGuiColorWheelGrid::polarValue(int index) const
{
    PolarValue v;
    KnobDoublePtr hue = _cells[index].spec.hue.lock();
    KnobDoublePtr chroma = _cells[index].spec.chroma.lock();

    v.hueDegrees = hue ? hue->getValue() : 0.;
    v.radius = chroma ? chroma->getValue() : 0.;

    return v;
}

void
KnobGuiColorWheelGrid::refreshCell(int index)
{
    if ((index < 0) || (index >= (int)_cells.size())) {
        return;
    }
    Cell& cell = _cells[index];
    KnobDoublePtr luminance = cell.spec.luminance.lock();
    const bool dragging = _gesture.isOpen() && (index == _activeCell);
    const PolarValue v = dragging ? _value : polarValue(index);
    const double y = luminance ? luminance->getValue() : 0.;

    if (cell.wheel && !dragging) {
        cell.wheel->setPolarValue(v.hueDegrees, v.radius);
    }
    if (cell.slider && !cell.slider->getGestureKey()) {
        cell.slider->seekScalePosition(y);
    }
    if (cell.readout) {
        // Compact so that two cells fit side by side; the tooltip spells the values out.
        cell.readout->setText(tr("H %1\u00B0  C %2  Y %3")
                                  .arg(v.hueDegrees, 0, 'f', 1)
                                  .arg(v.radius, 0, 'f', 4)
                                  .arg(y, 0, 'f', 3));
        cell.readout->setToolTip(tr("Hue %1\u00B0, Chroma %2, Luminance %3")
                                     .arg(v.hueDegrees, 0, 'f', 2)
                                     .arg(v.radius, 0, 'f', 5)
                                     .arg(y, 0, 'f', 4));
    }
}

void
KnobGuiColorWheelGrid::onGestureStarted(int index)
{
    if (_gesture.isOpen()) {
        onGestureFinished();
    }
    _activeCell = index;
    _gesture.begin();
    _value = polarValue(index);
    _moved = false;
    _draftGui = getGui();
    if (_draftGui) {
        _draftGui->setDraftRenderEnabled(true);
    }
}

void
KnobGuiColorWheelGrid::onGestureMoved(int index,
                                      const ColorEdit& edit)
{
    if (!_gesture.isOpen() || (index != _activeCell)) {
        return;
    }
    KnobDoublePtr chroma = _cells[index].spec.chroma.lock();
    if (!chroma) {
        return;
    }
    _value = applyPolarEdit(_value, edit, chroma->getMaximum());
    _moved = true;
    if (_cells[index].wheel) {
        _cells[index].wheel->setPolarValue(_value.hueDegrees, _value.radius);
    }
    if (!appPTR->getCurrentSettings()->getRenderOnEditingFinishedOnly()) {
        pushWheel();
    }
    refreshCell(index);
}

void
KnobGuiColorWheelGrid::onGestureFinished()
{
    if (!_gesture.isOpen()) {
        return;
    }
    Gui* gui = _draftGui.data();

    _draftGui.clear();
    // Draft goes off before the final push so that the committed value renders at full quality.
    if (gui) {
        gui->setDraftRenderEnabled(false);
    }
    if (_moved) {
        pushWheel();
    }
    _gesture.end();
    const int index = _activeCell;
    _activeCell = -1;
    refreshCell(index);
}

void
KnobGuiColorWheelGrid::pushWheel()
{
    if ((_activeCell < 0) || (_activeCell >= (int)_cells.size())) {
        return;
    }
    const KnobSeparator::ColorWheelSpec& spec = _cells[_activeCell].spec;
    KnobDoublePtr hue = spec.hue.lock();
    KnobDoublePtr chroma = spec.chroma.lock();
    KnobGuiPtr hueGui = hue ? KnobGui::getLiveGui(hue) : KnobGuiPtr();
    KnobGuiPtr chromaGui = chroma ? KnobGui::getLiveGui(chroma) : KnobGuiPtr();

    if (!chromaGui) {
        return;
    }
    const QString text = QString::fromUtf8(spec.label.c_str()) + QString::fromUtf8(": ") + tr("wheel");
    ColorWheelEditCommand* command = new ColorWheelEditCommand(_gesture.key(), text);
    if (hueGui && isWritable(hue) && (hue->getValue() != _value.hueDegrees)) {
        command->addStep(0, new KnobUndoCommand<double>(hueGui, std::list<double>(1, hue->getValue()), std::list<double>(1, _value.hueDegrees), true, _gesture.key()));
    }
    if (isWritable(chroma) && (chroma->getValue() != _value.radius)) {
        command->addStep(1, new KnobUndoCommand<double>(chromaGui, std::list<double>(1, chroma->getValue()), std::list<double>(1, _value.radius), true, _gesture.key()));
    }
    if (command->isEmpty()) {
        delete command;

        return;
    }
    chromaGui->pushUndoCommand(command);
}

void
KnobGuiColorWheelGrid::onLuminanceSlid(int index,
                                       double value)
{
    Cell& cell = _cells[index];
    KnobDoublePtr luminance = cell.spec.luminance.lock();
    KnobGuiPtr luminanceGui = luminance ? KnobGui::getLiveGui(luminance) : KnobGuiPtr();

    if (!luminanceGui || !isWritable(luminance) || (luminance->getValue() == value)) {
        return;
    }
    const quint64 key = cell.slider ? cell.slider->getGestureKey() : 0;
    KnobUndoCommand<double>* command = new KnobUndoCommand<double>(luminanceGui, std::list<double>(1, luminance->getValue()), std::list<double>(1, value), true, key);
    command->setText(QString::fromUtf8(cell.spec.label.c_str()) + QString::fromUtf8(": ") + tr("luminance"));
    luminanceGui->pushUndoCommand(command);
}

void
KnobGuiColorWheelGrid::onLuminanceReset(int index)
{
    KnobDoublePtr luminance = _cells[index].spec.luminance.lock();
    KnobGuiPtr luminanceGui = luminance ? KnobGui::getLiveGui(luminance) : KnobGuiPtr();

    if (!luminanceGui || !isWritable(luminance)) {
        return;
    }
    luminanceGui->pushUndoCommand(new RestoreDefaultsCommand(false, std::list<KnobIPtr>(1, luminance), -1));
}

void
KnobGuiColorWheelGrid::setEnabled()
{
    KnobSeparatorPtr knob = _knob.lock();
    const bool enabled = knob ? knob->isEnabled(0) : true;

    if (_grid) {
        _grid->setEnabled(enabled);
    }
}

void
KnobGuiColorWheelGrid::removeSpecificGui()
{
    if (_line) {
        _line->deleteLater();
    }
    if (_grid) {
        _grid->deleteLater();
    }
    _cells.clear();
}

void
KnobGuiColorWheelGrid::_hide()
{
    if (_line) {
        _line->hide();
    }
    if (_grid) {
        _grid->hide();
    }
}

void
KnobGuiColorWheelGrid::_show()
{
    if (_line) {
        _line->show();
    }
    if (_grid) {
        _grid->show();
    }
}

NATRON_NAMESPACE_EXIT
