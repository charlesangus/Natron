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

#include "KnobGuiColor.h"

#include <cfloat>
#include <algorithm> // min, max
#include <stdexcept>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QApplication>
#include <QFocusEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMetaObject>
#include <QMouseEvent>
#include <QScrollArea>
#include <QStyle>
#include <QToolTip>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVariant>
#include <QWidgetAction>
GCC_DIAG_UNUSED_PRIVATE_FIELD_OFF
// /opt/local/include/QtGui/qmime.h:119:10: warning: private field 'type' is not used [-Wunused-private-field]
#include <QKeyEvent>
GCC_DIAG_UNUSED_PRIVATE_FIELD_ON
#include <QDebug>
#include <QFontComboBox>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppInstance.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/Settings.h"
#include "Engine/TimeLine.h"
#include "Engine/Utils.h" // convertFromPlainText
#include "Engine/ViewIdx.h"

#include "Gui/Button.h"
#include "Gui/ClickableLabel.h"
#include "Gui/ColorKnobEditSession.h"
#include "Gui/ColorTrackballGlobalMode.h"
#include "Gui/ComboBox.h"
#include "Gui/CurveGui.h"
#include "Gui/DockablePanel.h"
#include "Gui/GroupBoxLabel.h"
#include "Gui/Gui.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/GuiDefines.h"
#include "Gui/GuiMacros.h"
#include "Gui/KnobUndoCommand.h"
#include "Gui/Label.h"
#include "Gui/NewLayerDialog.h"
#include "Gui/ProjectGui.h"
#include "Gui/ScaleSliderQWidget.h"
#include "Gui/SpinBox.h"
#include "Gui/SpinBoxValidator.h"
#include "Gui/TabGroup.h"

#include <ofxNatron.h>


NATRON_NAMESPACE_ENTER

namespace {
ColorAdjustPanel::DisplayConverter
makeDisplayConverter(const ProjectColorManagementPtr& colorManagement)
{
    if (!colorManagement) {
        return ColorAdjustPanel::DisplayConverter();
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

KnobColorValue
colorValueFromVector(const std::vector<double>& values)
{
    KnobColorValue v;

    v.rgb[0] = values.empty() ? 0.0 : values[0];
    v.rgb[1] = (values.size() >= 3) ? values[1] : v.rgb[0];
    v.rgb[2] = (values.size() >= 3) ? values[2] : v.rgb[0];
    v.hasAlpha = values.size() >= 4;
    v.a = v.hasAlpha ? values[3] : 1.0;

    return v;
}

// Fixed rather than derived from the swatch's own size (22px, NATRON_MEDIUM_BUTTON_SIZE), so
// the drag's feel does not change if that size ever does.
const double kSwatchTrackballPixelsPerRim = 64.0;
} // namespace

ColorPickerLabel::ColorPickerLabel(KnobGuiColor* knob,
                                   QWidget* parent)
    : Label(parent)
    , _pickingEnabled(false)
    , _knob(knob)
    , _pressed(false)
    , _dragging(false)
    , _pressPos()
    , _lastDragPos()
    , _segmentBase()
    , _dragValue()
    , _dragTotal()
    , _hasDragTotal(false)
    , _heldLocks()
    , _hovered(false)
    , _keyboardGrabbed(false)
{
    setMouseTracking(true);
    if (_knob) {
        // So a V/T lock key held before the drag starts still reaches keyPressEvent below.
        setFocusPolicy(Qt::StrongFocus);
    }
}

bool
ColorPickerLabel::isLockKey(int key) const
{
    return (key == Qt::Key_V) || (key == Qt::Key_T);
}

bool
ColorPickerLabel::wantsShortcutOverride(const QKeyEvent* e) const
{
    if (!isLockKey(e->key()) || !(_dragging || _hovered)) {
        return false;
    }

    // Ctrl is only claimed mid-drag, where it is the gear: over an idle swatch Ctrl+V and
    // Ctrl+T must still reach their application shortcuts.
    Qt::KeyboardModifiers allowed = Qt::ShiftModifier | Qt::KeypadModifier;

    if (_dragging) {
        allowed |= Qt::ControlModifier;
    }

    return !(e->modifiers() & ~allowed);
}

ColorAxisEnum
ColorPickerLabel::activeLock() const
{
    if (_heldLocks.empty()) {
        return eColorAxisTrackball;
    }

    return (_heldLocks.back() == Qt::Key_V) ? eColorAxisValue : eColorAxisTempTint;
}

QString
ColorPickerLabel::axisLabel(ColorAxisEnum axis) const
{
    switch (axis) {
    case eColorAxisValue:
        return tr("Value");
    case eColorAxisTempTint:
        return tr("Temp/Tint");
    default:
        return tr("Trackball");
    }
}

bool
ColorPickerLabel::beginTrackballDrag()
{
    if (!_knob->isColorEditable()) {
        return false;
    }
    _segmentBase = _knob->getCurrentColorValue();
    _dragValue = _segmentBase;
    _hasDragTotal = false;
    _knob->touchColorKnob();
    _knob->getEditSession()->begin(tr("Trackball"));
    grabKeyboard();
    _keyboardGrabbed = true;

    return true;
}

void
ColorPickerLabel::stepTrackballDrag(const QPointF& pos,
                                    Qt::KeyboardModifiers modifiers)
{
    double dx = pos.x() - _lastDragPos.x();
    double dy = pos.y() - _lastDragPos.y();

    _lastDragPos = pos;
    if ((dx == 0.0) && (dy == 0.0)) {
        return;
    }

    ColorKnobRoleEnum role = _knob->getColorRole();
    TrackballRates rates;
    double g = gear(modifiers.testFlag(Qt::ControlModifier), modifiers.testFlag(Qt::ShiftModifier));
    ColorEdit edit;

    edit.axis = activeLock();
    switch (edit.axis) {
    case eColorAxisValue:
        edit.d1 = -dy * valueRatePerPixel(rates, role) * g;
        break;

    case eColorAxisTempTint:
        edit.d1 = dx * tempRatePerPixel(rates, role) * g;
        edit.d2 = -dy * tintRatePerPixel(rates, role) * g;
        break;

    default: {
        double scale = rates.wheelRim / kSwatchTrackballPixelsPerRim * g;

        edit.axis = eColorAxisTrackball;
        edit.d1 = dx * scale;
        edit.d2 = -dy * scale;
        break;
    }
    }

    // Applied from a fixed base plus the accumulated total, like ColorAdjustPanel, so repeated
    // rounding across many small moves never drifts the result away from a single big one.
    if (_hasDragTotal && (edit.axis != _dragTotal.axis)) {
        _segmentBase = _dragValue;
        _hasDragTotal = false;
    }
    if (!_hasDragTotal) {
        _dragTotal = edit;
    } else {
        _dragTotal.d1 += edit.d1;
        _dragTotal.d2 += edit.d2;
    }
    _hasDragTotal = true;

    ChromaEditResult r = applyEdit(role, _knob->getEditBasis(), _segmentBase, _dragTotal, 0.0);

    if (r.valid) {
        _dragValue = r.value;
    }

    _knob->getEditSession()->move(_dragValue, axisLabel(edit.axis));
}

void
ColorPickerLabel::endTrackballDrag()
{
    if (_keyboardGrabbed) {
        releaseKeyboard();
        _keyboardGrabbed = false;
    }
    _knob->getEditSession()->end(_dragValue, axisLabel(_hasDragTotal ? _dragTotal.axis : eColorAxisTrackball));
    _hasDragTotal = false;
}

void
ColorPickerLabel::mousePressEvent(QMouseEvent* e)
{
    if (!_knob) {
        return;
    }
    if (_pressed) {
        Label::mousePressEvent(e);

        return;
    }
    _pressed = true;
    _dragging = false;
    _pressPos = e->position();
    _lastDragPos = _pressPos;
    setFocus(Qt::MouseFocusReason);
}

void
ColorPickerLabel::mouseMoveEvent(QMouseEvent* e)
{
    if (!_knob || !_pressed) {
        Label::mouseMoveEvent(e);

        return;
    }

    QPointF pos = e->position();

    if (!_dragging) {
        if ((pos - _pressPos).manhattanLength() < QApplication::startDragDistance()) {
            return;
        }
        if (!beginTrackballDrag()) {
            // Not editable right now (an expression, a slaved dimension, or disabled): the
            // gesture never starts, so an eventual release still reads as a plain click.
            return;
        }
        _dragging = true;
    }
    stepTrackballDrag(pos, e->modifiers());
}

void
ColorPickerLabel::mouseReleaseEvent(QMouseEvent* e)
{
    if (!_knob || !_pressed) {
        Label::mouseReleaseEvent(e);

        return;
    }

    bool wasDragging = _dragging;

    if (_dragging) {
        stepTrackballDrag(e->position(), e->modifiers());
        endTrackballDrag();
    }
    _pressed = false;
    _dragging = false;
    if (!wasDragging) {
        _pickingEnabled = !_pickingEnabled;
        Q_EMIT pickingEnabled(_pickingEnabled);
        setColor(_currentColor); //< refresh the icon
    }
}

void
ColorPickerLabel::keyPressEvent(QKeyEvent* e)
{
    int key = e->key();

    if (!_knob || !isLockKey(key) || !(_dragging || _hovered)) {
        Label::keyPressEvent(e);

        return;
    }
    e->accept();
    if (e->isAutoRepeat()) {
        return;
    }
    _heldLocks.erase(std::remove(_heldLocks.begin(), _heldLocks.end(), key), _heldLocks.end());
    _heldLocks.push_back(key);
}

void
ColorPickerLabel::keyReleaseEvent(QKeyEvent* e)
{
    int key = e->key();

    if (!_knob || !isLockKey(key)) {
        Label::keyReleaseEvent(e);

        return;
    }
    e->accept();
    if (e->isAutoRepeat()) {
        return;
    }
    _heldLocks.erase(std::remove(_heldLocks.begin(), _heldLocks.end(), key), _heldLocks.end());
}

void
ColorPickerLabel::focusOutEvent(QFocusEvent* e)
{
    // The releases of keys held when focus leaves go elsewhere, so they would otherwise stay
    // latched; mid-drag the keyboard grab still delivers them here.
    if (!_dragging) {
        _heldLocks.clear();
    }
    Label::focusOutEvent(e);
}

bool
ColorPickerLabel::event(QEvent* e)
{
    if (_knob && (e->type() == QEvent::ShortcutOverride) && wantsShortcutOverride(static_cast<QKeyEvent*>(e))) {
        e->accept();

        return true;
    }

    return Label::event(e);
}

void
ColorPickerLabel::enterEvent(QEnterEvent*)
{
    _hovered = true;
    if (_knob) {
        // Taking focus on hover is what lets a lock key pressed before the mouse button reach
        // the swatch at all; with click-to-focus it would only work from the second drag on.
        setFocus(Qt::MouseFocusReason);
    }
    QToolTip::showText( QCursor::pos(), toolTip() );
}

void
ColorPickerLabel::leaveEvent(QEvent*)
{
    _hovered = false;
    QToolTip::hideText();
}

void
ColorPickerLabel::setPickingEnabled(bool enabled)
{
    if ( !isEnabled() ) {
        return;
    }
    _pickingEnabled = enabled;
    setColor(_currentColor);
}

void
ColorPickerLabel::setEnabledMode(bool enabled)
{
    setEnabled(enabled);
    if (!enabled && _pickingEnabled) {
        _pickingEnabled = false;
        setColor(_currentColor);
        if (_knob) {
            _knob->getGui()->removeColorPicker( std::dynamic_pointer_cast<KnobColor>( _knob->getKnob() ) );
        }
    }
}

void
ColorPickerLabel::setColor(const QColor & color)
{
    _currentColor = color;

    if (_pickingEnabled && _knob) {
        //draw the picker on top of the label
        QPixmap pickerIcon;
        appPTR->getIcon(NATRON_PIXMAP_COLOR_PICKER, &pickerIcon);
        QImage pickerImg = pickerIcon.toImage();
        QImage img(pickerIcon.width(), pickerIcon.height(), QImage::Format_ARGB32);
        img.fill( color.rgb() );


        int h = pickerIcon.height();
        int w = pickerIcon.width();
        for (int i = 0; i < h; ++i) {
            const QRgb* data = (QRgb*)pickerImg.scanLine(i);
            if ( (i == 0) || (i == h - 1) ) {
                for (int j = 0; j < w; ++j) {
                    img.setPixel( j, i, qRgb(0, 0, 0) );
                }
            } else {
                for (int j = 0; j < w; ++j) {
                    if ( (j == 0) || (j == w - 1) ) {
                        img.setPixel( j, i, qRgb(0, 0, 0) );
                    } else {
                        int alpha = qAlpha(data[j]);
                        if (alpha > 0) {
                            img.setPixel(j, i, data[j]);
                        }
                    }
                }
            }
        }
        QPixmap pix = QPixmap::fromImage(img); //.scaled(20, 20);
        setPixmap(pix);
    } else {
        QImage img(NATRON_MEDIUM_BUTTON_SIZE, NATRON_MEDIUM_BUTTON_SIZE, QImage::Format_ARGB32);
        int h = img.height();
        int w = img.width();
        QRgb c = color.rgb();
        for (int i = 0; i < h; ++i) {
            if ( (i == 0) || (i == h - 1) ) {
                for (int j = 0; j < w; ++j) {
                    img.setPixel( j, i, qRgb(0, 0, 0) );
                }
            } else {
                for (int j = 0; j < w; ++j) {
                    if ( (j == 0) || (j == w - 1) ) {
                        img.setPixel( j, i, qRgb(0, 0, 0) );
                    } else {
                        img.setPixel(j, i, c);
                    }
                }
            }
        }
        QPixmap pix = QPixmap::fromImage(img);
        setPixmap(pix);
    }
} // ColorPickerLabel::setColor

KnobGuiColor::KnobGuiColor(KnobIPtr knob,
                           KnobGuiContainerI* container)
    : KnobGuiValue(knob, container)
    , _knob(std::dynamic_pointer_cast<KnobColor>(knob))
    , _colorLabel(0)
    , _colorSelector(0)
    , _colorSelectorButton(0)
    , _globalModeButton(0)
    , _lastColor()
    , _useSimplifiedUI(true)
    , _adjustPanel()
    , _rowContainer()
    , _editSession()
    , _configCallbackOwner()
    , _configCallbackId(-1)
{
    KnobColorPtr k = _knob.lock();
    assert(k);
    if (!k) {
        throw std::logic_error(__func__);
    }
    int nDims = k->getDimension();

    assert(nDims == 1 || nDims == 3 || nDims == 4);
    if (nDims != 1 && nDims != 3 && nDims != 4) {
        throw std::logic_error("A color Knob can only have dimension 1, 3 or 4");
    }

    _lastColor.resize(nDims);
    _useSimplifiedUI = isViewerUIKnob() || ( k && k->isSimplified() );
}

KnobGuiColor::~KnobGuiColor()
{
    ProjectColorManagementPtr colorManagement = _configCallbackOwner.lock();

    if (colorManagement && (_configCallbackId >= 0)) {
        colorManagement->removeConfigChangedCallback(_configCallbackId);
    }
}

bool
KnobGuiColor::wantsTopAlignedLabel() const
{
    return !_adjustPanel.isNull();
}

ColorAdjustPanel*
KnobGuiColor::getAdjustPanel() const
{
    return _adjustPanel.data();
}

bool
KnobGuiColor::isAdjustPanelExpanded() const
{
    return _adjustPanel && _colorSelectorButton && _colorSelectorButton->isChecked();
}

void
KnobGuiColor::setAdjustPanelExpanded(bool expanded)
{
    if (_adjustPanel && _colorSelectorButton) {
        _colorSelectorButton->setChecked(expanded);
    }
}

ColorKnobEditSession*
KnobGuiColor::getEditSession()
{
    if (!_editSession) {
        _editSession.reset(new ColorKnobEditSession(shared_from_this()));
    }

    return _editSession.get();
}

void
KnobGuiColor::touchColorKnob()
{
    Gui* gui = getGui();

    if (!gui) {
        return;
    }

    ColorKnobTouchTracker* tracker = ColorKnobTouchTracker::instanceFor(gui);

    if (tracker) {
        tracker->touch(this);
    }
}

ColorKnobRoleEnum
KnobGuiColor::getColorRole() const
{
    KnobColorPtr knob = _knob.lock();

    return knob ? knob->getColorRole() : eColorKnobRoleAuto;
}

LumaBasis
KnobGuiColor::getEditBasis() const
{
    return ColorKnobEditSession::basisFor(getColorManagement());
}

KnobColorValue
KnobGuiColor::getCurrentColorValue() const
{
    return ColorKnobEditSession::readKnobValue(_knob.lock());
}

bool
KnobGuiColor::isColorEditable() const
{
    return isAdjustPanelEditable();
}

QWidget*
KnobGuiColor::getGlobalModeRowWidget() const
{
    return _rowContainer.data();
}

void
KnobGuiColor::engageGlobalMode()
{
    Gui* gui = getGui();
    ColorTrackballGlobalMode* mode = gui ? ColorTrackballGlobalMode::instanceFor(gui) : 0;

    if (!mode || !_adjustPanel) {
        return;
    }
    mode->engage(std::dynamic_pointer_cast<KnobGuiColor>(shared_from_this()));
}

void
KnobGuiColor::setGlobalModeHighlighted(bool highlighted)
{
    if (_globalModeButton) {
        _globalModeButton->setChecked(highlighted);
    }
    if (!_rowContainer) {
        return;
    }
    _rowContainer->setProperty(colorTrackballTargetPropertyName(), highlighted);
    // Qt only re-evaluates property selectors on polish.
    _rowContainer->style()->unpolish(_rowContainer.data());
    _rowContainer->style()->polish(_rowContainer.data());
    _rowContainer->update();
}

ProjectColorManagementPtr
KnobGuiColor::getColorManagement() const
{
    KnobColorPtr knob = _knob.lock();

    if (!knob || !knob->getHolder() || !knob->getHolder()->getApp()) {
        return ProjectColorManagementPtr();
    }

    ProjectPtr project = knob->getHolder()->getApp()->getProject();

    return project ? project->getColorManagement() : ProjectColorManagementPtr();
}

QWidget*
KnobGuiColor::createBelowRowWidget(QWidget* parent)
{
    KnobColorPtr knob = _knob.lock();

    if (_useSimplifiedUI || !knob) {
        return 0;
    }

    ProjectColorManagementPtr colorManagement = getColorManagement();

    _adjustPanel = new ColorAdjustPanel(parent);
    _adjustPanel->setObjectName(QString::fromUtf8("ColorAdjustPanel"));
    _adjustPanel->setProperty(colorKnobTouchTagPropertyName(), QVariant::fromValue<void*>(this));
    _adjustPanel->setKnobValue(ColorKnobEditSession::readKnobValue(knob),
                               knob->getDimension(),
                               knob->getColorRole(),
                               ColorKnobEditSession::basisFor(colorManagement));
    _adjustPanel->setDisplayConverter(makeDisplayConverter(colorManagement));
    _adjustPanel->setColorManagement(colorManagement);
    QObject::connect(_adjustPanel.data(), &ColorAdjustPanel::valuesEdited, this, &KnobGuiColor::onAdjustPanelValuesEdited);

    if (colorManagement) {
        QPointer<KnobGuiColor> self(this);
        _configCallbackId = colorManagement->addConfigChangedCallback([self]() {
            if (self) {
                QMetaObject::invokeMethod(self.data(), "onProjectColorConfigChanged", Qt::AutoConnection);
            }
        });
        _configCallbackOwner = colorManagement;
    }

    return _adjustPanel.data();
}

void
KnobGuiColor::refreshAdjustPanelColorManagement()
{
    KnobColorPtr knob = _knob.lock();

    if (!_adjustPanel || !knob) {
        return;
    }

    ProjectColorManagementPtr colorManagement = getColorManagement();

    _adjustPanel->setColorManagement(colorManagement);
    _adjustPanel->setDisplayConverter(makeDisplayConverter(colorManagement));
    _adjustPanel->setRole(knob->getColorRole());
    _adjustPanel->setBasis(ColorKnobEditSession::basisFor(colorManagement));
}

void
KnobGuiColor::onProjectColorConfigChanged()
{
    refreshAdjustPanelColorManagement();
}

bool
KnobGuiColor::isAdjustPanelEditable() const
{
    KnobColorPtr knob = _knob.lock();

    if (!knob || !knob->isEnabled(0)) {
        return false;
    }

    const int rgbDims = std::min(knob->getDimension(), 3);

    for (int i = 0; i < rgbDims; ++i) {
        if (!knob->getExpression(i).empty() || knob->isSlave(i)) {
            return false;
        }
    }

    return true;
}

void
KnobGuiColor::refreshAdjustPanelState()
{
    KnobColorPtr knob = _knob.lock();

    if (!_adjustPanel || !knob) {
        return;
    }

    bool editable = isAdjustPanelEditable();

    _adjustPanel->setEnabled(knob->isEnabled(0));
    _adjustPanel->getWheelTab()->setEnabled(editable);
    _adjustPanel->getValuesTab()->setEnabled(editable);
}

void
KnobGuiColor::onAdjustPanelToggled(bool expanded)
{
    if (!_adjustPanel) {
        return;
    }
    if (expanded) {
        KnobColorPtr knob = _knob.lock();

        if (knob && !_adjustPanel->isEditing()) {
            _adjustPanel->setKnobValue(ColorKnobEditSession::readKnobValue(knob));
        }
    }
    _adjustPanel->setVisible(expanded);
    _colorSelectorButton->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    _colorSelectorButton->setToolTip(NATRON_NAMESPACE::convertFromPlainText(expanded ? tr("Hide the colour adjustment panel.") : tr("Show the colour adjustment panel: a trackball wheel with hue, saturation, value, temperature, tint and alpha sliders, and a Values tab with unclamped RGBA and HSV fields and the colour palette."),
                                                                            NATRON_NAMESPACE::WhiteSpaceNormal));
}

void
KnobGuiColor::onAdjustPanelValuesEdited(KnobColorValue value,
                                        ColorAdjustPanel::GesturePhaseEnum phase,
                                        QString undoLabel)
{
    ColorKnobEditSession* session = getEditSession();

    switch (phase) {
    case ColorAdjustPanel::eGesturePhaseStart:
        if (isAdjustPanelEditable()) {
            touchColorKnob();
            session->begin(undoLabel);
        }
        break;
    case ColorAdjustPanel::eGesturePhaseMove:
        session->move(value, undoLabel);
        break;
    case ColorAdjustPanel::eGesturePhaseEnd: {
        bool applied = session->isActive();

        session->end(value, undoLabel);

        // A refused gesture still moved the panel's own copy of the value, so it is put back.
        KnobColorPtr knob = _knob.lock();

        if (!applied && knob && _adjustPanel) {
            _adjustPanel->setKnobValue(ColorKnobEditSession::readKnobValue(knob));
        }
        break;
    }
    }
}

void
KnobGuiColor::connectKnobSignalSlots()
{
    KnobColorPtr knob = _knob.lock();
    QObject::connect( this, SIGNAL(dimensionSwitchToggled(bool)), knob.get(), SLOT(onDimensionSwitchToggled(bool)) );
    QObject::connect( knob.get(), SIGNAL(mustActivateAllDimensions()), this, SLOT(onMustShowAllDimension()) );
    QObject::connect( knob.get(), SIGNAL(pickingEnabled(bool)), this, SLOT(setPickingEnabled(bool)) );

    KnobSignalSlotHandlerPtr handler = knob->getSignalSlotHandler();
    if (handler) {
        QObject::connect(handler.get(), SIGNAL(expressionChanged(int)), this, SLOT(refreshAdjustPanelState()));
        QObject::connect(handler.get(), SIGNAL(knobSlaved(int, bool)), this, SLOT(refreshAdjustPanelState()));
    }
}

void
KnobGuiColor::getIncrements(std::vector<double>* increments) const
{
    KnobColorPtr knob = _knob.lock();
    if (!knob) {
        return;
    }
    int nDims = knob->getDimension();

    assert(nDims == 1 || nDims == 3 || nDims == 4);
    if (nDims != 1 && nDims != 3 && nDims != 4) {
        throw std::logic_error("A color Knob can only have dimension 1, 3 or 4");
    }

    increments->resize(nDims);
    for (int i = 0; i < nDims; ++i) {
        (*increments)[i] = 0.001;
    }
}

void
KnobGuiColor::getDecimals(std::vector<int>* decimals) const
{
    KnobColorPtr knob = _knob.lock();
    if (!knob) {
        return;
    }
    int nDims = knob->getDimension();

    assert(nDims == 1 || nDims == 3 || nDims == 4);
    if (nDims != 1 && nDims != 3 && nDims != 4) {
        throw std::logic_error("A color Knob can only have dimension 1, 3 or 4");
    }

    decimals->resize(nDims);
    for (int i = 0; i < nDims; ++i) {
        (*decimals)[i] = 6;
    }
}

void
KnobGuiColor::addExtraWidgets(QHBoxLayout* containerLayout)
{
    containerLayout->addSpacing( TO_DPIX(10) );
    KnobColorPtr knob = _knob.lock();
    if (_adjustPanel) {
        // Tags the row so ColorKnobTouchTracker's app-wide filter can find this knob from a
        // press, wheel or focus-in on any of its spinboxes, not only from a gesture session.
        containerLayout->widget()->setProperty(colorKnobTouchTagPropertyName(), QVariant::fromValue<void*>(this));
        _rowContainer = containerLayout->widget();
    }
    _colorLabel = new ColorPickerLabel( _useSimplifiedUI ? NULL : this, containerLayout->widget() );
    if (!_useSimplifiedUI) {
        _colorLabel->setToolTip(NATRON_NAMESPACE::convertFromPlainText(tr("Click and drag to adjust this colour's hue and saturation like a trackball. "
                                                                          "Hold V while dragging to change only its value, or T for its temperature and tint. "
                                                                          "Hold Ctrl to move ten times slower, Shift to move four times faster, or both together for the slowest, finest control.\n\n"
                                                                          "A click without dragging toggles picking a color on a viewer instead: once enabled, press control + left click on any viewer.\n"
                                                                          "You can also pick the average color of a given rectangle by holding control + shift + left click\n. "
                                                                          "To deselect the picker left click anywhere."
                                                                          "Note that by default %1 converts to linear the color picked\n"
                                                                          "because all the processing pipeline is linear, but you can turn this off in the\n"
                                                                          "preferences panel.")
                                                                           .arg(QString::fromUtf8(NATRON_APPLICATION_NAME)),
                                                                       NATRON_NAMESPACE::WhiteSpaceNormal));
    }

    QSize medSize( TO_DPIX(NATRON_MEDIUM_BUTTON_SIZE), TO_DPIY(NATRON_MEDIUM_BUTTON_SIZE) );
    QSize medIconSize( TO_DPIX(NATRON_MEDIUM_BUTTON_ICON_SIZE), TO_DPIY(NATRON_MEDIUM_BUTTON_ICON_SIZE) );

    _colorLabel->setFixedSize(medSize);
    QObject::connect( _colorLabel, SIGNAL(pickingEnabled(bool)), this, SLOT(onPickingEnabled(bool)) );
    containerLayout->addWidget(_colorLabel);

    if (_useSimplifiedUI) {
        containerLayout->addSpacing( TO_DPIX(5) );
    }

    _colorSelectorButton = new QToolButton( containerLayout->widget() );
    _colorSelectorButton->setObjectName(QString::fromUtf8("ColorSelectorButton"));
    _colorSelectorButton->setFixedSize(medSize);

    if (_adjustPanel) {
        _colorSelectorButton->setAutoRaise(false);
        _colorSelectorButton->setCheckable(true);
        _colorSelectorButton->setChecked(false);
        _colorSelectorButton->setFocusPolicy(Qt::NoFocus);
        QObject::connect(_colorSelectorButton, &QToolButton::toggled, this, &KnobGuiColor::onAdjustPanelToggled);
        onAdjustPanelToggled(false);
        containerLayout->addWidget(_colorSelectorButton);

        _globalModeButton = new QToolButton(containerLayout->widget());
        _globalModeButton->setObjectName(QString::fromUtf8("ColorGlobalModeButton"));
        _globalModeButton->setFixedSize(medSize);
        QPixmap globalModePix;
        appPTR->getIcon(NATRON_PIXMAP_VIEWER_CENTER, NATRON_MEDIUM_BUTTON_ICON_SIZE, &globalModePix);
        _globalModeButton->setIcon(QIcon(globalModePix));
        _globalModeButton->setIconSize(medIconSize);
        _globalModeButton->setCheckable(true);
        _globalModeButton->setChecked(false);
        _globalModeButton->setAutoRaise(false);
        _globalModeButton->setFocusPolicy(Qt::NoFocus);
        _globalModeButton->setToolTip(NATRON_NAMESPACE::convertFromPlainText(tr("Engage the global trackball mode (Ctrl+E) on this knob: a click-drag anywhere then adjusts it until Esc or Enter accepts, or the shortcut is pressed again."), NATRON_NAMESPACE::WhiteSpaceNormal));
        QObject::connect(_globalModeButton, &QToolButton::clicked, this, &KnobGuiColor::engageGlobalMode);
        containerLayout->addWidget(_globalModeButton);

        return;
    }

    QPixmap colorSelectorPix;
    appPTR->getIcon(NATRON_PIXMAP_COLORWHEEL, NATRON_MEDIUM_BUTTON_ICON_SIZE, &colorSelectorPix);
    _colorSelectorButton->setIcon(QIcon(colorSelectorPix));
    _colorSelectorButton->setIconSize(medIconSize);
    _colorSelectorButton->setPopupMode(QToolButton::InstantPopup);
    _colorSelectorButton->setArrowType(Qt::NoArrow);
    _colorSelectorButton->setAutoRaise(false);
    _colorSelectorButton->setCheckable(false);
    _colorSelectorButton->setToolTip( NATRON_NAMESPACE::convertFromPlainText(tr("Open Color Selector"), NATRON_NAMESPACE::WhiteSpaceNormal) );
    _colorSelectorButton->setFocusPolicy(Qt::NoFocus);

    ProjectColorManagementPtr colorManagement;
    if (!_useSimplifiedUI && knob->getHolder() && knob->getHolder()->getApp()) {
        colorManagement = knob->getHolder()->getApp()->getProject()->getColorManagement();
    }
    _colorSelector = new ColorSelectorWidget(knob->getDimension() == 4, colorManagement, containerLayout->widget());
    QObject::connect( _colorSelector, SIGNAL( colorChanged(float, float, float, float) ),
                      this, SLOT( onColorSelectorChanged(float, float, float, float) ) );
    QObject::connect( _colorSelector, SIGNAL( updateColor() ),
                      this, SLOT( updateColorSelector() ) );

    QWidgetAction *colorPopupAction = new QWidgetAction( containerLayout->widget() );
    colorPopupAction->setDefaultWidget(_colorSelector);
    _colorSelectorButton->addAction(colorPopupAction);
    containerLayout->addWidget(_colorSelectorButton);

    if (_useSimplifiedUI) {
        KnobGuiValue::_hide();
        enableRightClickMenu(_colorLabel, -1);
    }
}

void
KnobGuiColor::onMustShowAllDimension()
{
    onDimensionSwitchClicked(true);
}

void
KnobGuiColor::onColorSelectorChanged(float r,
                                     float g,
                                     float b,
                                     float a)
{
    KnobColorPtr knob = _knob.lock();
    int nDims = knob->getDimension();

    assert(nDims == 1 || nDims == 3 || nDims == 4);
    if (nDims != 1 && nDims != 3 && nDims != 4) {
        throw std::logic_error("A color Knob can only have dimension 1, 3 or 4");
    }

    if (nDims == 1) {
        knob->setValue(r, ViewSpec::all(), 0);
    } else if (nDims == 3) {
        knob->setValues(r, g, b,
                        ViewSpec::all(),
                        eValueChangedReasonNatronInternalEdited);
    } else if (nDims == 4) {
        knob->setValues(r, g, b, a,
                        ViewSpec::all(),
                        eValueChangedReasonNatronInternalEdited);
    }
    if ( getGui() ) {
        getGui()->setDraftRenderEnabled(true);
    }
}

void
KnobGuiColor::updateLabel(double r,
                          double g,
                          double b,
                          double a)
{
    QColor color;
    KnobColorPtr knob = _knob.lock();
    float fr = r, fg = g, fb = b;

    if (!_useSimplifiedUI && knob && knob->getHolder() && knob->getHolder()->getApp()) {
        ProjectColorManagementPtr colorManagement = knob->getHolder()->getApp()->getProject()->getColorManagement();
        if (colorManagement) {
            colorManagement->workingToColorPicking(&fr, &fg, &fb);
        }
    }

    color.setRgbF(Image::clamp<qreal>(fr, 0., 1.),
                  Image::clamp<qreal>(fg, 0., 1.),
                  Image::clamp<qreal>(fb, 0., 1.),
                  Image::clamp<qreal>(a, 0., 1.));
    _colorLabel->setColor(color);
}

void
KnobGuiColor::setPickingEnabled(bool enabled)
{
    if (!_colorLabel) {
        return;
    }
    if (_colorLabel->isPickingEnabled() == enabled) {
        return;
    }
    _colorLabel->setPickingEnabled(enabled);
    onPickingEnabled(enabled);
}

void
KnobGuiColor::onPickingEnabled(bool enabled)
{
    if ( getKnob()->getHolder()->getApp() ) {
        if (enabled) {
            getGui()->registerNewColorPicker( _knob.lock() );
        } else {
            getGui()->removeColorPicker( _knob.lock() );
        }
    }
}

void
KnobGuiColor::_hide()
{
    if (!_useSimplifiedUI) {
        KnobGuiValue::_hide();
    }
    _colorLabel->hide();
    _colorSelectorButton->hide();
    if (_adjustPanel) {
        _adjustPanel->hide();
    }
}

void
KnobGuiColor::_show()
{
    if (!_useSimplifiedUI) {
        KnobGuiValue::_show();
    }
    _colorLabel->show();
    _colorSelectorButton->show();
    if (isAdjustPanelExpanded()) {
        _adjustPanel->show();
    }
}

void
KnobGuiColor::updateExtraGui(const std::vector<double>& values)
{
    assert(values.size() > 0);
    double r = values[0];
    double g = r;
    double b = r;
    double a = r;
    if (values.size() > 1) {
        g = values[1];
    }
    if (values.size() > 2) {
        b = values[2];
    }
    if (values.size() > 3) {
        a = values[3];
    }
    updateLabel(r, g, b, a);

    // A collapsed panel is brought up to date when it is expanded, so it is not redrawn here.
    if (_adjustPanel && !_adjustPanel->isHidden()) {
        _adjustPanel->setKnobValue(colorValueFromVector(values));
    }
}

void
KnobGuiColor::onDimensionsFolded()
{
    KnobColorPtr knob = _knob.lock();
    int nDims = knob->getDimension();

    assert(nDims == 1 || nDims == 3 || nDims == 4);
    if (nDims != 1 && nDims != 3 && nDims != 4) {
        throw std::logic_error("A color Knob can only have dimension 1, 3 or 4");
    }

    for (int i = 0; i < nDims; ++i) {
        SpinBox* sb = 0;
        getSpinBox(i, &sb);
        assert(sb);
        sb->setUseLineColor(false, Qt::red);
    }
    Q_EMIT dimensionSwitchToggled(false);
}

void
KnobGuiColor::onDimensionsExpanded()
{
    QColor colors[4];

    colors[0].setRgbF(0.851643, 0.196936, 0.196936);
    colors[1].setRgbF(0, 0.654707, 0);
    colors[2].setRgbF(0.345293, 0.345293, 1);
    colors[3].setRgbF(0.398979, 0.398979, 0.398979);

    KnobColorPtr knob = _knob.lock();
    int nDims = knob->getDimension();

    assert(nDims == 1 || nDims == 3 || nDims == 4);
    if (nDims != 1 && nDims != 3 && nDims != 4) {
        throw std::logic_error("A color Knob can only have dimension 1, 3 or 4");
    }

    for (int i = 0; i < nDims; ++i) {
        SpinBox* sb = 0;
        Label* label = 0;
        getSpinBox(i, &sb, &label);
        assert(sb);
        sb->setUseLineColor(true, colors[i]);
    }
    Q_EMIT dimensionSwitchToggled(true);
}

void
KnobGuiColor::setEnabledExtraGui(bool enabled)
{
    _colorLabel->setEnabledMode(enabled);
    if (!_adjustPanel) {
        _colorSelectorButton->setEnabled(enabled);
    }
    refreshAdjustPanelState();
}

void
KnobGuiColor::updateColorSelector()
{
    KnobColorPtr knob = _knob.lock();
    if (!_colorSelector || !knob) {
        return;
    }
    const int nDims = knob->getDimension();
    double curR = knob->getValue(0);

    assert(nDims == 1 || nDims == 3 || nDims == 4);
    if (nDims != 1 && nDims != 3 && nDims != 4) {
        throw std::logic_error("A color Knob can only have dimension 1, 3 or 4");
    }

    double curG = curR;
    double curB = curR;
    double curA = 1.;
    if (nDims > 1) {
        curG = knob->getValue(1);
        curB = knob->getValue(2);
    }
    if (nDims > 3) {
        curA = knob->getValue(3);
    }

    _colorSelector->setColor( Image::clamp<qreal>(curR, 0., 1.),
                              Image::clamp<qreal>(curG, 0., 1.),
                              Image::clamp<qreal>(curB, 0., 1.),
                              Image::clamp<qreal>(curA, 0., 1.) );
}

bool
KnobGuiColor::isAutoFoldDimensionsEnabled() const
{
    // [FD] I think all color parameters should be folded automatically:
    // Grade and ColorCorrect are some of the most used nodes,
    // and they have "A" channel uncked by default, so we don't care if colors
    // are folded.
    // The only case where this could matter are the "Draw" nodes (Rectangle, Constant, etc.),
    // but these are rarely used compared to "Color" nodes.
    return true;
    //KnobColorPtr knob = _knob.lock();
    //if (!knob) {
    //    return false;
    //}
    //return knob->getDimension() == 3;
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_KnobGuiColor.cpp"
