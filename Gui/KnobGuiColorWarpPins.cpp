/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-3.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "KnobGuiColorWarpPins.h"

#include <algorithm>
#include <cmath>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QColor>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/KnobColorWarpPins.h"
#include "Engine/Project.h"
#include "Engine/TimeLine.h"

#include "Gui/ColorKnobEditSession.h"
#include "Gui/ColorTrackballGlobalMode.h"
#include "Gui/ColorTrackballTarget.h"
#include "Gui/DockablePanel.h"
#include "Gui/SpinBox.h"

NATRON_NAMESPACE_ENTER

namespace {

enum PinColumnEnum {
    ePinColumnEnabled = 0,
    ePinColumnName,
    ePinColumnSource,
    ePinColumnTarget,
    ePinColumnCount
};

enum PinEditorEnum {
    ePinEditorTargetR = 0,
    ePinEditorTargetG,
    ePinEditorTargetB,
    ePinEditorStrength,
    ePinEditorFalloff
};

enum PinKeyEnum {
    ePinKeyTarget = 0,
    ePinKeyStrength,
    ePinKeyFalloff
};

const int kPinIdRole = Qt::UserRole + 1;

ProjectColorManagementPtr
colorManagementOf(const KnobIPtr& knob)
{
    KnobHolder* holder = knob ? knob->getHolder() : 0;
    AppInstancePtr app = holder ? holder->getApp() : AppInstancePtr();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();

    return project ? project->getColorManagement() : ProjectColorManagementPtr();
}

QIcon
swatchIcon(const double rgb[3],
           const ColorGlobalModeHook::DisplayConverter& converter)
{
    float display[3] = { (float)rgb[0], (float)rgb[1], (float)rgb[2] };

    if (converter) {
        converter(display, 1);
    }

    QPixmap pix(TO_DPIX(18), TO_DPIY(12));
    pix.fill(QColor::fromRgbF(std::clamp(display[0], 0.f, 1.f), std::clamp(display[1], 0.f, 1.f), std::clamp(display[2], 0.f, 1.f)));

    return QIcon(pix);
}

/**
 * @brief The global mode's view of one pin's target. It addresses the pin by id, so it keeps
 * driving the same pin when others are added or removed, and reports no knob once that pin is
 * gone. Strokes write the pin set live and leave one undo step at their end.
 **/
class PinTargetGlobalModeHook
    : public ColorGlobalModeHook {
public:
    PinTargetGlobalModeHook(KnobGuiColorWarpPins* owner,
                            const KnobColorWarpPinsPtr& knob,
                            int pinId)
        : _owner(owner)
        , _ownerRef(std::dynamic_pointer_cast<KnobGui>(owner->shared_from_this()))
        , _knob(knob)
        , _pinId(pinId)
        , _editing(false)
        , _editTime(0.)
        , _before()
    {
    }

    virtual KnobGui* getKnobGui() const OVERRIDE FINAL
    {
        KnobColorWarpPinsPtr knob = _knob.lock();

        if (!_owner || _ownerRef.expired() || !knob || (knob->getPinSet().indexOf(_pinId) < 0)) {
            return 0;
        }

        return _owner.data();
    }

    virtual QString getLabel() const OVERRIDE FINAL
    {
        ColorWarpPinData pin;

        return readPin(&pin) ? QObject::tr("%1 Target").arg(QString::fromStdString(pin.name)) : QString();
    }

    virtual bool isColorEditable() const OVERRIDE FINAL
    {
        KnobColorWarpPinsPtr knob = _knob.lock();

        return getKnobGui() && knob->isEnabled(0);
    }

    virtual ColorKnobRoleEnum getColorRole() const OVERRIDE FINAL
    {
        return eColorKnobRoleColour;
    }

    virtual LumaBasis getEditBasis() const OVERRIDE FINAL
    {
        return ColorKnobEditSession::basisFor(colorManagementOf(_knob.lock()));
    }

    virtual DisplayConverter getDisplayConverter() const OVERRIDE FINAL
    {
        return makeColorPickingDisplayConverter(colorManagementOf(_knob.lock()));
    }

    virtual KnobColorValue getCurrentColorValue() const OVERRIDE FINAL
    {
        KnobColorValue value;
        ColorWarpPinData pin;
        const bool ok = readPin(&pin);
        const double time = _editing ? _editTime : currentTime();

        for (int c = 0; c < 3; ++c) {
            value.rgb[c] = ok ? pin.target[c].valueAt(time) : 0.;
        }
        value.a = 1.;
        value.hasAlpha = false;

        return value;
    }

    virtual void attach() OVERRIDE FINAL
    {
        if (!getKnobGui()) {
            return;
        }
        _owner->setGlobalModeHighlighted(true);
        touch();
        revealKnobInPanel(dynamic_cast<DockablePanel*>(_owner->getContainer()), _owner->getKnob(), _owner->getPinList());
    }

    virtual void detach() OVERRIDE FINAL
    {
        if (_owner && !_ownerRef.expired()) {
            _owner->setGlobalModeHighlighted(false);
        }
    }

    virtual void touch() OVERRIDE FINAL
    {
        if (KnobGui* knobGui = getKnobGui()) {
            touchGlobalModeKnob(knobGui);
        }
    }

    virtual void beginEdit(const QString& /*label*/) OVERRIDE FINAL
    {
        KnobColorWarpPinsPtr knob = _knob.lock();

        if (!knob) {
            return;
        }
        _before = knob->getPinSet();
        _editTime = currentTime();
        _editing = true;
    }

    virtual void moveEdit(const KnobColorValue& value,
                          const QString& /*label*/) OVERRIDE FINAL
    {
        ColorWarpPinSet pins;

        if (applyTarget(value, &pins)) {
            _knob.lock()->setPinSet(pins);
        }
    }

    virtual void endEdit(const KnobColorValue& value,
                         const QString& label) OVERRIDE FINAL
    {
        ColorWarpPinSet pins;

        if (applyTarget(value, &pins)) {
            _knob.lock()->editPinSet(_before, pins, label.toStdString());
        }
        _editing = false;
    }

    virtual void abandonEdit() OVERRIDE FINAL
    {
        KnobColorWarpPinsPtr knob = _knob.lock();

        if (_editing && knob) {
            knob->setPinSet(_before);
        }
        _editing = false;
    }

private:
    double currentTime() const
    {
        return (_owner && !_ownerRef.expired()) ? _owner->getEditTime() : 0.;
    }

    bool readPin(ColorWarpPinData* pin) const
    {
        KnobColorWarpPinsPtr knob = _knob.lock();

        if (!knob) {
            return false;
        }

        const ColorWarpPinSet pins = knob->getPinSet();
        const int index = pins.indexOf(_pinId);

        if (index < 0) {
            return false;
        }
        *pin = pins.pins[index];

        return true;
    }

    bool applyTarget(const KnobColorValue& value,
                     ColorWarpPinSet* pins) const
    {
        KnobColorWarpPinsPtr knob = _knob.lock();

        if (!_editing || !knob) {
            return false;
        }
        *pins = _before;

        const int index = pins->indexOf(_pinId);

        if (index < 0) {
            return false;
        }
        for (int c = 0; c < 3; ++c) {
            pins->pins[index].target[c].set(_editTime, value.rgb[c]);
        }

        return true;
    }

    QPointer<KnobGuiColorWarpPins> _owner;
    std::weak_ptr<KnobGui> _ownerRef;
    KnobColorWarpPinsWPtr _knob;
    int _pinId;
    bool _editing;
    double _editTime;
    ColorWarpPinSet _before;
};

} // namespace

struct KnobGuiColorWarpPinsPrivate {
    KnobColorWarpPinsWPtr knob;
    QPointer<QWidget> container;
    QPointer<QTreeWidget> list;
    QPointer<QPushButton> addButton;
    QPointer<QPushButton> deleteButton;
    QPointer<QWidget> editors;
    SpinBox* target[3];
    SpinBox* strength;
    SpinBox* falloff;
    QPushButton* keys[3];
    bool refreshing;
    bool highlighted;

    KnobGuiColorWarpPinsPrivate()
        : knob()
        , container()
        , list()
        , addButton()
        , deleteButton()
        , editors()
        , strength(0)
        , falloff(0)
        , refreshing(false)
        , highlighted(false)
    {
        for (int c = 0; c < 3; ++c) {
            target[c] = 0;
            keys[c] = 0;
        }
    }
};

KnobGuiColorWarpPins::KnobGuiColorWarpPins(KnobIPtr knob,
                                           KnobGuiContainerI* container)
    : KnobGui(knob, container)
    , _imp(new KnobGuiColorWarpPinsPrivate())
{
    _imp->knob = std::dynamic_pointer_cast<KnobColorWarpPins>(knob);
}

KnobGuiColorWarpPins::~KnobGuiColorWarpPins()
{
}

KnobIPtr
KnobGuiColorWarpPins::getKnob() const
{
    return _imp->knob.lock();
}

QTreeWidget*
KnobGuiColorWarpPins::getPinList() const
{
    return _imp->list.data();
}

double
KnobGuiColorWarpPins::getEditTime() const
{
    KnobColorWarpPinsPtr knob = _imp->knob.lock();

    return knob ? knob->getCurrentTime() : 0.;
}

void
KnobGuiColorWarpPins::setGlobalModeHighlighted(bool highlighted)
{
    _imp->highlighted = highlighted;
    if (_imp->list) {
        _imp->list->setStyleSheet(highlighted ? QString::fromUtf8("QTreeWidget { border: 2px solid rgb(240, 160, 60); }") : QString());
    }
}

std::shared_ptr<ColorGlobalModeHook>
KnobGuiColorWarpPins::makeColorGlobalModeHook()
{
    KnobColorWarpPinsPtr knob = _imp->knob.lock();
    const int id = knob ? knob->getSelectedPinId() : 0;

    if (!knob || (id == 0) || weak_from_this().expired()) {
        return std::shared_ptr<ColorGlobalModeHook>();
    }

    return std::make_shared<PinTargetGlobalModeHook>(this, knob, id);
}

void
KnobGuiColorWarpPins::createWidget(QHBoxLayout* layout)
{
    QWidget* parent = layout->parentWidget();
    KnobColorWarpPinsPtr knob = _imp->knob.lock();

    _imp->container = new QWidget(parent);
    _imp->container->setProperty(globalModeKnobTouchTagPropertyName(), QVariant::fromValue(QPointer<KnobGui>(this)));
    QVBoxLayout* column = new QVBoxLayout(_imp->container);
    column->setContentsMargins(0, 0, 0, 0);

    _imp->list = new QTreeWidget(_imp->container);
    _imp->list->setColumnCount(ePinColumnCount);
    _imp->list->setHeaderLabels(QStringList() << tr("Enabled") << tr("Name") << tr("Source") << tr("Target"));
    _imp->list->setRootIsDecorated(false);
    _imp->list->setSelectionMode(QAbstractItemView::SingleSelection);
    _imp->list->setMinimumHeight(TO_DPIY(120));
    _imp->list->header()->setSectionResizeMode(ePinColumnName, QHeaderView::Stretch);
    column->addWidget(_imp->list);

    QHBoxLayout* buttons = new QHBoxLayout();
    _imp->addButton = new QPushButton(tr("Add Pin"), _imp->container);
    _imp->addButton->setToolTip(tr("Adds a pin on mid grey. Clicking in the viewer adds a pin on the colour under the pointer instead."));
    _imp->deleteButton = new QPushButton(tr("Delete Pin"), _imp->container);
    _imp->deleteButton->setToolTip(tr("Deletes the selected pin."));
    buttons->addWidget(_imp->addButton);
    buttons->addWidget(_imp->deleteButton);
    buttons->addStretch();
    column->addLayout(buttons);

    _imp->editors = new QWidget(_imp->container);
    QGridLayout* grid = new QGridLayout(_imp->editors);
    grid->setContentsMargins(0, 0, 0, 0);

    grid->addWidget(new QLabel(tr("Target"), _imp->editors), 0, 0);
    for (int c = 0; c < 3; ++c) {
        _imp->target[c] = new SpinBox(_imp->editors, SpinBox::eSpinBoxTypeDouble);
        _imp->target[c]->decimals(4);
        _imp->target[c]->setIncrement(0.01);
        grid->addWidget(_imp->target[c], 0, 1 + c);
    }
    _imp->target[0]->setToolTip(tr("Red of the colour the pin's source maps to."));
    _imp->target[1]->setToolTip(tr("Green of the colour the pin's source maps to."));
    _imp->target[2]->setToolTip(tr("Blue of the colour the pin's source maps to."));

    grid->addWidget(new QLabel(tr("Strength"), _imp->editors), 1, 0);
    _imp->strength = new SpinBox(_imp->editors, SpinBox::eSpinBoxTypeDouble);
    _imp->strength->decimals(3);
    _imp->strength->setMinimum(0.);
    _imp->strength->setMaximum(1.);
    _imp->strength->setIncrement(0.05);
    _imp->strength->setToolTip(tr("How much of the shift from source to target the pin applies. 0 makes a hold pin, which keeps its colour where it is."));
    grid->addWidget(_imp->strength, 1, 1);

    grid->addWidget(new QLabel(tr("Falloff"), _imp->editors), 2, 0);
    _imp->falloff = new SpinBox(_imp->editors, SpinBox::eSpinBoxTypeDouble);
    _imp->falloff->decimals(3);
    _imp->falloff->setMinimum(kColorWarpMinPinFalloff);
    _imp->falloff->setIncrement(0.05);
    _imp->falloff->setToolTip(tr("How far from its source colour the pin reaches."));
    grid->addWidget(_imp->falloff, 2, 1);

    for (int k = 0; k < 3; ++k) {
        _imp->keys[k] = new QPushButton(tr("Key"), _imp->editors);
        _imp->keys[k]->setCheckable(true);
        _imp->keys[k]->setToolTip(tr("Sets a key at the current frame, or removes the one there."));
        grid->addWidget(_imp->keys[k], k, 4);
        connect(_imp->keys[k], &QPushButton::clicked, this, [this, k]() {
            onKeyClicked(k);
        });
    }
    grid->setColumnStretch(5, 1);
    column->addWidget(_imp->editors);

    layout->addWidget(_imp->container);

    connect(_imp->list.data(), &QTreeWidget::itemSelectionChanged, this, [this]() {
        onListSelectionChanged();
    });
    connect(_imp->list.data(), &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int col) {
        onListItemChanged(_imp->list->indexOfTopLevelItem(item), col);
    });
    connect(_imp->addButton.data(), &QPushButton::clicked, this, [this]() {
        onAddClicked();
    });
    connect(_imp->deleteButton.data(), &QPushButton::clicked, this, [this]() {
        onDeleteClicked();
    });
    for (int c = 0; c < 3; ++c) {
        SpinBox* box = _imp->target[c];
        connect(box, &SpinBox::valueChanged, this, [this, box, c](double v) {
            onEditorValueChanged(ePinEditorTargetR + c, v, box->getGestureKey());
        });
    }
    connect(_imp->strength, &SpinBox::valueChanged, this, [this](double v) {
        onEditorValueChanged(ePinEditorStrength, v, _imp->strength->getGestureKey());
    });
    connect(_imp->falloff, &SpinBox::valueChanged, this, [this](double v) {
        onEditorValueChanged(ePinEditorFalloff, v, _imp->falloff->getGestureKey());
    });

    if (knob) {
        connect(knob.get(), &KnobColorWarpPins::selectedPinChanged, this, [this](int id) {
            onSelectionChangedInKnob(id);
        });

        KnobHolder* holder = knob->getHolder();
        AppInstancePtr app = holder ? holder->getApp() : AppInstancePtr();
        TimeLinePtr timeline = app ? app->getTimeLine() : TimeLinePtr();

        if (timeline) {
            connect(timeline.get(), &TimeLine::frameChanged, this, [this](SequenceTime, int) {
                rebuildList();
            });
        }
    }

    rebuildList();
} // KnobGuiColorWarpPins::createWidget

void
KnobGuiColorWarpPins::removeSpecificGui()
{
    if (_imp->container) {
        _imp->container->deleteLater();
    }
}

void
KnobGuiColorWarpPins::_hide()
{
    if (_imp->container) {
        _imp->container->hide();
    }
}

void
KnobGuiColorWarpPins::_show()
{
    if (_imp->container) {
        _imp->container->show();
    }
}

void
KnobGuiColorWarpPins::setEnabled()
{
    KnobColorWarpPinsPtr knob = _imp->knob.lock();

    if (_imp->container) {
        _imp->container->setEnabled(knob && knob->isEnabled(0));
    }
}

void
KnobGuiColorWarpPins::setReadOnly(bool readOnly,
                                  int /*dimension*/)
{
    if (_imp->container) {
        _imp->container->setEnabled(!readOnly);
    }
}

void
KnobGuiColorWarpPins::setDirty(bool /*dirty*/)
{
}

void
KnobGuiColorWarpPins::updateGUI(int /*dimension*/)
{
    rebuildList();
}

void
KnobGuiColorWarpPins::rebuildList()
{
    KnobColorWarpPinsPtr knob = _imp->knob.lock();

    if (!_imp->list || !knob) {
        return;
    }

    const ColorWarpPinSet pins = knob->getPinSet();
    const int selected = knob->getSelectedPinId();
    const double time = getEditTime();
    const ColorGlobalModeHook::DisplayConverter converter = makeColorPickingDisplayConverter(colorManagementOf(knob));

    _imp->refreshing = true;
    {
        QSignalBlocker blocker(_imp->list.data());

        while (_imp->list->topLevelItemCount() > (int)pins.pins.size()) {
            delete _imp->list->takeTopLevelItem(_imp->list->topLevelItemCount() - 1);
        }
        for (std::size_t i = 0; i < pins.pins.size(); ++i) {
            const ColorWarpPinData& pin = pins.pins[i];
            QTreeWidgetItem* item = _imp->list->topLevelItem((int)i);

            if (!item) {
                item = new QTreeWidgetItem(_imp->list.data());
            }
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable | Qt::ItemIsEditable);
            item->setData(ePinColumnEnabled, kPinIdRole, pin.id);
            item->setCheckState(ePinColumnEnabled, pin.enabled ? Qt::Checked : Qt::Unchecked);
            item->setText(ePinColumnName, QString::fromStdString(pin.name));

            const ColorWarpPin evaluated = pin.evaluate(time);
            item->setIcon(ePinColumnSource, swatchIcon(pin.source, converter));
            item->setIcon(ePinColumnTarget, swatchIcon(evaluated.target, converter));
            item->setSelected(pin.id == selected);
        }
    }
    _imp->refreshing = false;
    refreshEditors();
} // KnobGuiColorWarpPins::rebuildList

void
KnobGuiColorWarpPins::refreshEditors()
{
    KnobColorWarpPinsPtr knob = _imp->knob.lock();

    if (!_imp->editors || !knob) {
        return;
    }

    const ColorWarpPinSet pins = knob->getPinSet();
    const int index = pins.indexOf(knob->getSelectedPinId());

    _imp->editors->setEnabled(index >= 0);
    if (_imp->deleteButton) {
        _imp->deleteButton->setEnabled(index >= 0);
    }
    if (index < 0) {
        return;
    }

    const ColorWarpPinData& pin = pins.pins[index];
    const double time = getEditTime();

    _imp->refreshing = true;
    for (int c = 0; c < 3; ++c) {
        _imp->target[c]->setValue(pin.target[c].valueAt(time));
        _imp->target[c]->setAnimation(pin.target[c].isAnimated() ? (pin.target[c].hasKeyAt(time) ? 2 : 1) : 0);
    }
    _imp->strength->setValue(pin.strength.valueAt(time));
    _imp->strength->setAnimation(pin.strength.isAnimated() ? (pin.strength.hasKeyAt(time) ? 2 : 1) : 0);
    _imp->falloff->setValue(pin.falloff.valueAt(time));
    _imp->falloff->setAnimation(pin.falloff.isAnimated() ? (pin.falloff.hasKeyAt(time) ? 2 : 1) : 0);
    _imp->keys[ePinKeyTarget]->setChecked(pin.target[0].hasKeyAt(time) || pin.target[1].hasKeyAt(time) || pin.target[2].hasKeyAt(time));
    _imp->keys[ePinKeyStrength]->setChecked(pin.strength.hasKeyAt(time));
    _imp->keys[ePinKeyFalloff]->setChecked(pin.falloff.hasKeyAt(time));
    _imp->refreshing = false;
}

void
KnobGuiColorWarpPins::onSelectionChangedInKnob(int /*id*/)
{
    rebuildList();
    touchGlobalModeKnob(this);

    Gui* gui = getGui();
    ColorTrackballGlobalMode* mode = gui ? ColorTrackballGlobalMode::instanceFor(gui) : 0;

    if (!mode || !mode->isEngaged()) {
        return;
    }

    GlobalModeTarget* current = mode->getGlobalModeTarget();

    if (current && (current->getKnobGui() != this) && (current->getKnobGui() != 0)) {
        return;
    }

    std::shared_ptr<GlobalModeTarget> target = makeGlobalModeTarget(this);

    if (target) {
        mode->engage(target);
    } else {
        mode->exit();
    }
}

void
KnobGuiColorWarpPins::onListSelectionChanged()
{
    KnobColorWarpPinsPtr knob = _imp->knob.lock();

    if (_imp->refreshing || !knob || !_imp->list) {
        return;
    }

    const QList<QTreeWidgetItem*> items = _imp->list->selectedItems();
    const int id = items.isEmpty() ? 0 : items.front()->data(ePinColumnEnabled, kPinIdRole).toInt();

    knob->setSelectedPinId(id);
}

void
KnobGuiColorWarpPins::onListItemChanged(int row,
                                        int column)
{
    KnobColorWarpPinsPtr knob = _imp->knob.lock();

    if (_imp->refreshing || !knob || !_imp->list || (row < 0)) {
        return;
    }

    ColorWarpPinSet pins = knob->getPinSet();

    if (row >= (int)pins.pins.size()) {
        return;
    }

    QTreeWidgetItem* item = _imp->list->topLevelItem(row);
    ColorWarpPinData& pin = pins.pins[row];

    if (column == ePinColumnEnabled) {
        const bool enabled = item->checkState(ePinColumnEnabled) == Qt::Checked;

        if (enabled == pin.enabled) {
            return;
        }
        pin.enabled = enabled;
        knob->editPinSet(pins, enabled ? tr("Enable Pin").toStdString() : tr("Disable Pin").toStdString());
    } else if (column == ePinColumnName) {
        const std::string name = item->text(ePinColumnName).toStdString();

        if (name.empty() || (name == pin.name)) {
            rebuildList();

            return;
        }
        pin.name = name;
        knob->editPinSet(pins, tr("Rename Pin").toStdString());
    }
}

void
KnobGuiColorWarpPins::onAddClicked()
{
    KnobColorWarpPinsPtr knob = _imp->knob.lock();

    if (!knob) {
        return;
    }

    ColorWarpPinSet pins = knob->getPinSet();
    const double grey[3] = { 0.18, 0.18, 0.18 };
    const int index = pins.addUnpickedPin(grey);
    const int id = pins.pins[index].id;

    knob->editPinSet(pins, tr("Add Pin").toStdString());
    knob->setSelectedPinId(id);
}

void
KnobGuiColorWarpPins::onDeleteClicked()
{
    KnobColorWarpPinsPtr knob = _imp->knob.lock();

    if (!knob) {
        return;
    }

    ColorWarpPinSet pins = knob->getPinSet();
    const int index = pins.indexOf(knob->getSelectedPinId());

    if (index < 0) {
        return;
    }
    pins.pins.erase(pins.pins.begin() + index);
    knob->editPinSet(pins, tr("Delete Pin").toStdString());
    knob->setSelectedPinId(0);
}

void
KnobGuiColorWarpPins::onEditorValueChanged(int editor,
                                           double value,
                                           quint64 gestureKey)
{
    KnobColorWarpPinsPtr knob = _imp->knob.lock();

    if (_imp->refreshing || !knob) {
        return;
    }

    ColorWarpPinSet pins = knob->getPinSet();
    const int index = pins.indexOf(knob->getSelectedPinId());

    if (index < 0) {
        return;
    }

    ColorWarpPinData& pin = pins.pins[index];
    const double time = getEditTime();
    const ColorWarpPinSet before = pins;

    switch (editor) {
    case ePinEditorStrength:
        pin.strength.set(time, colorWarpClampPinStrength(value));
        break;
    case ePinEditorFalloff:
        pin.falloff.set(time, colorWarpClampPinFalloff(value));
        break;
    default:
        if ((editor < ePinEditorTargetR) || (editor > ePinEditorTargetB)) {
            return;
        }
        pin.target[editor - ePinEditorTargetR].set(time, value);
        break;
    }
    if (pins == before) {
        return;
    }
    knob->editPinSet(before, pins, tr("Edit Pin").toStdString(), gestureKey);
}

void
KnobGuiColorWarpPins::onKeyClicked(int which)
{
    KnobColorWarpPinsPtr knob = _imp->knob.lock();

    if (!knob) {
        return;
    }

    ColorWarpPinSet pins = knob->getPinSet();
    const int index = pins.indexOf(knob->getSelectedPinId());

    if (index < 0) {
        refreshEditors();

        return;
    }

    ColorWarpPinData& pin = pins.pins[index];
    const double time = getEditTime();
    std::vector<ColorWarpPinChannel*> channels;

    switch (which) {
    case ePinKeyTarget:
        channels.push_back(&pin.target[0]);
        channels.push_back(&pin.target[1]);
        channels.push_back(&pin.target[2]);
        break;
    case ePinKeyStrength:
        channels.push_back(&pin.strength);
        break;
    default:
        channels.push_back(&pin.falloff);
        break;
    }

    bool anyKey = false;

    for (std::size_t i = 0; i < channels.size(); ++i) {
        anyKey = anyKey || channels[i]->hasKeyAt(time);
    }
    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (anyKey) {
            (void)channels[i]->removeKey(time);
        } else {
            channels[i]->setKey(time, channels[i]->valueAt(time));
        }
    }
    knob->editPinSet(pins, anyKey ? tr("Remove Pin Key").toStdString() : tr("Set Pin Key").toStdString());
} // KnobGuiColorWarpPins::onKeyClicked

NATRON_NAMESPACE_EXIT
