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

#include "ColorTrackballBinPanel.h"

#include <map>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Utils.h" // convertFromPlainText

#include "Gui/ColorKnobEditSession.h"
#include "Gui/ColorPanelLayoutDev.h"
#include "Gui/ColorTrackballGlobalMode.h"
#include "Gui/ColorTrackballTarget.h"
#include "Gui/DockablePanel.h"
#include "Gui/Gui.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/GuiDefines.h"
#include "Gui/GuiMacros.h"
#include "Gui/KnobGuiColor.h"
#include "Gui/NodeGui.h"
#include "Gui/NodeSettingsPanel.h"
#include "Gui/PropertiesBinWrapper.h"

NATRON_NAMESPACE_ENTER

namespace {

std::map<Gui*, QPointer<ColorTrackballBinPanel>>&
binPanels()
{
    static std::map<Gui*, QPointer<ColorTrackballBinPanel>> panels;

    return panels;
}

bool
binTopPlacement()
{
    return ColorPanelLayoutDev::instance()->getLayout().placement == ColorPanelLayout::Placement::BinTop;
}
} // namespace

QString
colorTrackballBinPanelTitle(const QString& nodeLabel,
                            const QString& knobName)
{
    if (nodeLabel.isEmpty()) {
        return knobName;
    }

    return nodeLabel + QLatin1Char('.') + knobName;
}

ColorTrackballBinPanel*
ColorTrackballBinPanel::instanceFor(Gui* gui)
{
    std::map<Gui*, QPointer<ColorTrackballBinPanel>>& panels = binPanels();
    std::map<Gui*, QPointer<ColorTrackballBinPanel>>::iterator it = panels.find(gui);

    if ((it != panels.end()) && it->second) {
        return it->second.data();
    }

    PropertiesBinWrapper* bin = gui ? gui->getPropertiesBin() : 0;

    return bin ? bin->getColorTrackballPanel() : 0;
}

ColorTrackballBinPanel::ColorTrackballBinPanel(Gui* gui,
                                               QWidget* parent)
    : QWidget(parent)
    , _gui(gui)
    , _header(0)
    , _title(0)
    , _closeButton(0)
    , _layout(0)
    , _panel(0)
    , _knob()
    , _knobObject()
    , _knobConnections()
    , _topPanel()
    , _watchedContainer()
    , _topCheckQueued(false)
    , _topCheckPending(false)
{
    setObjectName(QString::fromUtf8("ColorTrackballBinPanel"));
    binPanels()[gui] = this;

    _layout = new QVBoxLayout(this);
    _layout->setContentsMargins(TO_DPIX(4), TO_DPIY(4), TO_DPIX(4), TO_DPIY(4));
    _layout->setSpacing(TO_DPIY(2));

    _header = new QWidget(this);
    QHBoxLayout* headerLayout = new QHBoxLayout(_header);

    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(TO_DPIX(4));
    _title = new QLabel(_header);
    _title->setObjectName(QString::fromUtf8("ColorTrackballBinPanelTitle"));
    headerLayout->addWidget(_title);
    headerLayout->addStretch();

    const QSize medSize(TO_DPIX(NATRON_MEDIUM_BUTTON_SIZE), TO_DPIY(NATRON_MEDIUM_BUTTON_SIZE));
    const QSize medIconSize(TO_DPIX(NATRON_MEDIUM_BUTTON_ICON_SIZE), TO_DPIY(NATRON_MEDIUM_BUTTON_ICON_SIZE));
    QIcon closeIcon;

    appPTR->getIcon(NATRON_PIXMAP_CLOSE_WIDGET, TO_DPIX(NATRON_MEDIUM_BUTTON_ICON_SIZE), &closeIcon);
    _closeButton = new QToolButton(_header);
    _closeButton->setObjectName(QString::fromUtf8("ColorTrackballBinPanelCloseButton"));
    _closeButton->setIcon(closeIcon);
    _closeButton->setFixedSize(medSize);
    _closeButton->setIconSize(medIconSize);
    _closeButton->setAutoRaise(false);
    _closeButton->setFocusPolicy(Qt::NoFocus);
    _closeButton->setToolTip(NATRON_NAMESPACE::convertFromPlainText(tr("Close the trackball panel."), NATRON_NAMESPACE::WhiteSpaceNormal));
    QObject::connect(_closeButton, &QToolButton::clicked, this, &ColorTrackballBinPanel::onCloseClicked);
    headerLayout->addWidget(_closeButton);
    _layout->addWidget(_header);

    _panel = new ColorAdjustPanel(this);
    _panel->setObjectName(QString::fromUtf8("ColorTrackballBinAdjustPanel"));
    // The bin's full width is the panel's whatever the edge axes say: there is no row of
    // buttons beside it to leave room for.
    _panel->setRightInset(0);
    _layout->addWidget(_panel);
    QObject::connect(_panel, &ColorAdjustPanel::valuesEdited, this, &ColorTrackballBinPanel::onPanelValuesEdited);
    QObject::connect(_panel, &ColorAdjustPanel::layoutApplied, this, [this]() {
        _panel->setRightInset(0);
    });

    // Leaving the BinTop placement hands the bound knob back to its own panel, its ▾ still
    // checked; see unbind().
    QObject::connect(ColorPanelLayoutDev::instance(), &ColorPanelLayoutDev::layoutChanged, this, [this]() {
        if (!binTopPlacement() && isBound()) {
            unbind();
        }
    });

    hide();
}

ColorTrackballBinPanel::~ColorTrackballBinPanel()
{
    for (std::size_t i = 0; i < _knobConnections.size(); ++i) {
        QObject::disconnect(_knobConnections[i]);
    }
    if (_watchedContainer) {
        _watchedContainer->removeEventFilter(this);
    }

    std::map<Gui*, QPointer<ColorTrackballBinPanel>>& panels = binPanels();
    std::map<Gui*, QPointer<ColorTrackballBinPanel>>::iterator it = panels.find(_gui);

    if ((it != panels.end()) && ((it->second.data() == this) || !it->second)) {
        panels.erase(it);
    }
}

KnobGuiColor*
ColorTrackballBinPanel::getBoundKnob() const
{
    KnobGuiColorPtr knob = _knob.lock();

    return (knob && _knobObject) ? knob.get() : 0;
}

QString
ColorTrackballBinPanel::getTitle() const
{
    return _title->text();
}

void
ColorTrackballBinPanel::releaseBinding()
{
    for (std::size_t i = 0; i < _knobConnections.size(); ++i) {
        QObject::disconnect(_knobConnections[i]);
    }
    _knobConnections.clear();
    _knob.reset();
    _knobObject = 0;
}

bool
ColorTrackballBinPanel::bindTo(KnobGuiColor* knobGui)
{
    if (!knobGui || !knobGui->getAdjustPanel()) {
        return false;
    }

    KnobGuiColor* previous = getBoundKnob();

    if (previous == knobGui) {
        show();

        return true;
    }
    if (previous && _panel->isEditing()) {
        previous->getEditSession()->abandon();
    }
    releaseBinding();

    KnobGuiColorPtr shared = std::dynamic_pointer_cast<KnobGuiColor>(knobGui->shared_from_this());

    if (!shared) {
        hide();

        return false;
    }
    _knob = shared;
    _knobObject = knobGui;

    _knobConnections.push_back(QObject::connect(knobGui, &QObject::destroyed, this, [this]() {
        dropDyingBinding();
    }));
    _knobConnections.push_back(QObject::connect(knobGui->getAdjustPanel(), &QObject::destroyed, this, [this]() {
        dropDyingBinding();
    }));

    DockablePanel* nodePanel = dynamic_cast<DockablePanel*>(knobGui->getContainer());

    if (nodePanel) {
        _knobConnections.push_back(QObject::connect(nodePanel, &QObject::destroyed, this, [this]() {
            dropDyingBinding();
        }));
        _knobConnections.push_back(QObject::connect(nodePanel, &DockablePanel::closeChanged, this, [this](bool closed) {
            if (closed) {
                unbind();
            }
        }));
    }

    KnobIPtr knob = knobGui->getKnob();
    KnobSignalSlotHandlerPtr handler = knob ? knob->getSignalSlotHandler() : KnobSignalSlotHandlerPtr();

    if (handler) {
        _knobConnections.push_back(QObject::connect(handler.get(), &KnobSignalSlotHandler::valueChanged, this, [this](ViewSpec, int, int) {
            KnobGuiColor* bound = getBoundKnob();

            if (bound && !_panel->isEditing()) {
                _panel->setKnobValue(bound->getCurrentColorValue());
            }
        }));
        _knobConnections.push_back(QObject::connect(handler.get(), &KnobSignalSlotHandler::expressionChanged, this, [this](int) {
            refreshEditable();
        }));
        _knobConnections.push_back(QObject::connect(handler.get(), &KnobSignalSlotHandler::knobSlaved, this, [this](int, bool) {
            refreshEditable();
        }));
        _knobConnections.push_back(QObject::connect(handler.get(), &KnobSignalSlotHandler::enabledChanged, this, [this]() {
            refreshEditable();
        }));
    }

    refreshFromKnob();
    refreshTitle();
    _topPanel = _gui ? static_cast<QObject*>(topmostNodePanel(_gui)) : 0;
    watchTopNode();
    show();

    if (previous && binTopPlacement() && previous->isAdjustPanelExpanded()) {
        previous->setAdjustPanelExpanded(false);
    }
    if (!knobGui->isAdjustPanelExpanded()) {
        knobGui->setAdjustPanelExpanded(true);
    }
    Q_EMIT boundKnobChanged();

    return true;
} // ColorTrackballBinPanel::bindTo

void
ColorTrackballBinPanel::unbind()
{
    KnobGuiColor* previous = getBoundKnob();
    const bool wasBound = !_knobConnections.empty() || previous;

    if (previous && _panel->isEditing()) {
        previous->getEditSession()->abandon();
    }

    ColorTrackballGlobalMode* mode = (previous && _gui && binTopPlacement()) ? ColorTrackballGlobalMode::instanceFor(_gui) : 0;

    // Global mode on the bound knob keeps its HUD here and drives this panel; with the panel
    // gone it has nothing left to show its state in.
    if (mode && mode->isEngaged() && (mode->getTarget() == previous)) {
        mode->exit();
    }
    releaseBinding();
    _topCheckPending = false;
    hide();
    if (previous && binTopPlacement() && previous->isAdjustPanelExpanded()) {
        previous->setAdjustPanelExpanded(false);
    }
    if (wasBound) {
        Q_EMIT boundKnobChanged();
    }
}

void
ColorTrackballBinPanel::dropDyingBinding()
{
    const bool wasBound = !_knobConnections.empty() || getBoundKnob();

    releaseBinding();
    _topCheckPending = false;
    hide();
    if (wasBound) {
        Q_EMIT boundKnobChanged();
    }
}

void
ColorTrackballBinPanel::followTopNode(KnobGuiColor* target)
{
    KnobGuiColor* bound = getBoundKnob();

    if (!bound) {
        return;
    }

    if (!target || !target->getAdjustPanel()) {
        unbind();

        return;
    }
    if (target == bound) {
        return;
    }

    ColorTrackballGlobalMode* mode = _gui ? ColorTrackballGlobalMode::instanceFor(_gui) : 0;

    if (mode && mode->isEngaged() && (mode->getTarget() == bound)) {
        // Moving the mode binds the panel through the target's ▾; bindTo() below is then a
        // no-op, and the mode keeps its current mode across the move.
        mode->engage(std::dynamic_pointer_cast<KnobGuiColor>(target->shared_from_this()));
    }
    bindTo(target);
}

void
ColorTrackballBinPanel::attachHud(QWidget* hud)
{
    attachColorGlobalModeHud(_layout, _header, hud);
}

void
ColorTrackballBinPanel::onCloseClicked()
{
    unbind();
}

void
ColorTrackballBinPanel::onPanelValuesEdited(KnobColorValue value,
                                            ColorAdjustPanel::GesturePhaseEnum phase,
                                            QString undoLabel)
{
    KnobGuiColor* bound = getBoundKnob();

    if (!bound) {
        return;
    }
    // The knob's own handler, so a gesture here is begun, merged and committed exactly as one
    // on the knob's in-row panel.
    bound->onAdjustPanelValuesEdited(value, phase, undoLabel);
    if (phase != ColorAdjustPanel::eGesturePhaseEnd) {
        return;
    }
    // A refused gesture still moved this panel's copy of the value; an applied one leaves the
    // knob holding the same value, so reading it back is right either way.
    if (KnobGuiColor* stillBound = getBoundKnob()) {
        _panel->setKnobValue(stillBound->getCurrentColorValue());
    }
    if (_topCheckPending) {
        _topCheckPending = false;
        queueTopNodeCheck();
    }
}

void
ColorTrackballBinPanel::refreshFromKnob()
{
    KnobGuiColor* bound = getBoundKnob();

    if (!bound) {
        return;
    }

    KnobIPtr knob = bound->getKnob();
    const int nDims = knob ? knob->getDimension() : 3;

    _panel->setKnobValue(bound->getCurrentColorValue(), nDims, bound->getColorRole(), bound->getEditBasis());
    _panel->setDisplayConverter(bound->getDisplayConverter());
    _panel->setRightInset(0);
    refreshEditable();
}

void
ColorTrackballBinPanel::refreshEditable()
{
    KnobGuiColor* bound = getBoundKnob();
    KnobIPtr knob = bound ? bound->getKnob() : KnobIPtr();

    if (!knob) {
        return;
    }
    _panel->setEnabled(knob->isEnabled(0));
    _panel->getEditArea()->setEnabled(bound->isColorEditable());
}

void
ColorTrackballBinPanel::refreshTitle()
{
    KnobGuiColor* bound = getBoundKnob();
    KnobIPtr knob = bound ? bound->getKnob() : KnobIPtr();
    NodeSettingsPanel* nodePanel = bound ? dynamic_cast<NodeSettingsPanel*>(bound->getContainer()) : 0;
    NodeGuiPtr nodeGui = nodePanel ? nodePanel->getNode() : NodeGuiPtr();
    NodePtr node = nodeGui ? nodeGui->getNode() : NodePtr();
    const QString nodeLabel = node ? QString::fromUtf8(node->getLabel().c_str()) : QString();
    const QString knobName = knob ? QString::fromUtf8(knob->getName().c_str()) : QString();

    _title->setText(colorTrackballBinPanelTitle(nodeLabel, knobName));
}

// The properties container's layout is where node panels are inserted, raised and removed;
// every one of those posts a LayoutRequest or a child event to it. Gui keeps no signal for its
// top panel, so the check runs once those events settle, when Gui's own list is current too.
void
ColorTrackballBinPanel::watchTopNode()
{
    if (_watchedContainer || !_gui) {
        return;
    }

    QVBoxLayout* propertiesLayout = _gui->getPropertiesLayout();
    QWidget* container = propertiesLayout ? propertiesLayout->parentWidget() : 0;

    if (!container) {
        return;
    }
    _watchedContainer = container;
    container->installEventFilter(this);
}

bool
ColorTrackballBinPanel::eventFilter(QObject* watched,
                                    QEvent* event)
{
    if ((watched == _watchedContainer.data()) && isBound()) {
        switch (event->type()) {
        case QEvent::LayoutRequest:
        case QEvent::ChildAdded:
        case QEvent::ChildRemoved:
            queueTopNodeCheck();
            break;
        default:
            break;
        }
    }

    return QWidget::eventFilter(watched, event);
}

void
ColorTrackballBinPanel::queueTopNodeCheck()
{
    if (_topCheckQueued) {
        return;
    }
    _topCheckQueued = true;
    QTimer::singleShot(0, this, [this]() {
        checkTopNode();
    });
}

void
ColorTrackballBinPanel::checkTopNode()
{
    _topCheckQueued = false;
    if (!_gui || !isBound()) {
        return;
    }

    QObject* top = topmostNodePanel(_gui);

    if (top == _topPanel.data()) {
        return;
    }
    // A gesture is never moved onto another knob halfway; the check runs again at its End.
    if (_panel->isEditing()) {
        _topCheckPending = true;

        return;
    }
    _topPanel = top;
    followTopNode(resolveColorTrackballTarget(_gui));
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_ColorTrackballBinPanel.cpp"
