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
#include <QAbstractScrollArea>
#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QFontComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLayout>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QScrollArea>
#include <QStyle>
#include <QToolTip>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidgetAction>
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
#include "Gui/GuiDensity.h"
#include "Gui/GuiMacros.h"
#include "Gui/KnobUndoCommand.h"
#include "Gui/Label.h"
#include "Gui/NewLayerDialog.h"
#include "Gui/ProjectGui.h"
#include "Gui/ScaleSliderQWidget.h"
#include "Gui/SpinBox.h"
#include "Gui/SpinBoxValidator.h"
#include "Gui/TabGroup.h"
#include "Gui/ThemedIcon.h"

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

// Showing or hiding the adjust panel, or folding or unfolding the dimension lines above it,
// changes the below-row column's preferred size. Qt applies that through LayoutRequests that
// each level posts to the next one up, and a flush of posted events never delivers the ones
// posted during it, so the change climbs one level per event loop pass and a paint in between
// draws the knob page squeezed into the panel's old height. Delivering each widget's pending
// LayoutRequest and activating its layout bottom-up, from the column to the scroll area that
// holds the panel (the properties bin), settles the whole chain in one go, before updates are
// turned back on. Widgets that place their children without a QLayout, like QTabWidget, are
// only reached through that event.
void
activateLayoutsUpToInclusive(QWidget* bottom, QWidget* top)
{
    bool reachedTop = false;

    for (QWidget* w = bottom; w; w = w->parentWidget()) {
        QCoreApplication::sendPostedEvents(w, QEvent::LayoutRequest);
        if (QLayout* l = w->layout()) {
            l->activate();
        }
        if (w == top) {
            reachedTop = true;
        }
        if (w->isWindow() || (reachedTop && qobject_cast<QAbstractScrollArea*>(w))) {
            break;
        }
    }
}

// Holds a widget's updates off for one relayout, then flushes the layout requests and turns
// them back on, unless they were already off when it started: whoever turned them off then is
// still relaying out, and turning them back on here would paint its half-built state.
class UpdatesHeldOff {
public:
    explicit UpdatesHeldOff(QWidget* widget)
        : _widget(widget)
        , _restore(widget && widget->updatesEnabled())
    {
        if (_restore) {
            widget->setUpdatesEnabled(false);
        }
    }

    ~UpdatesHeldOff()
    {
        if (_restore && _widget) {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            _widget->setUpdatesEnabled(true);
        }
    }

    UpdatesHeldOff(const UpdatesHeldOff&) = delete;
    UpdatesHeldOff& operator=(const UpdatesHeldOff&) = delete;

private:
    QPointer<QWidget> _widget;
    bool _restore;
};

QVariant
touchTagFor(KnobGuiColor* knobGui)
{
    return QVariant::fromValue(QPointer<KnobGuiColor>(knobGui));
}

const char*
openBlockPropertyName()
{
    return "_natronColorAdjustOpen";
}

// The open state's icon is the closed state's, flipped, so the pair always matches. Both
// check states get it: a themed glyph's On state is tinted with the selection colour.
QIcon
verticallyMirroredIcon(const QIcon& source,
                       const QSize& size,
                       qreal devicePixelRatio)
{
    QIcon mirrored;
    const QIcon::Mode modes[] = { QIcon::Normal, QIcon::Disabled, QIcon::Active };

    for (QIcon::Mode mode : modes) {
        const QPixmap pix = source.pixmap(size, devicePixelRatio, mode, QIcon::Off);
        QPixmap flipped = QPixmap::fromImage(pix.toImage().mirrored(false, true));

        flipped.setDevicePixelRatio(pix.devicePixelRatio());
        mirrored.addPixmap(flipped, mode, QIcon::Off);
        mirrored.addPixmap(flipped, mode, QIcon::On);
    }

    return mirrored;
}

void
repolishTree(QWidget* root)
{
    QList<QWidget*> widgets = root->findChildren<QWidget*>();

    widgets.prepend(root);
    for (QWidget* w : widgets) {
        w->style()->unpolish(w);
        w->style()->polish(w);
        w->update();
    }
}

QColor
mixColors(const QColor& a,
          const QColor& b,
          double t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t,
                            a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

// Spans the page grid's cells for the knob's row and its panel, under them. The widgets in
// those cells turn their stylesheet background transparent while it shows (mainstyle.qss,
// _natronColorAdjustOpen), so this fill also covers the grid's spacing between them. The
// colours come from the application palette, which Gui::loadStyleSheet() fills from the theme:
// Mid is the panel background, Dark the sunken colour, Light the raised one. The frame stops
// short of the cells' bottom by the bottom gap, which keeps the row gap every other row ends
// with between the block and the knob below it.
class AdjustPanelBackdrop
    : public QWidget {
public:
    explicit AdjustPanelBackdrop(QWidget* parent)
        : QWidget(parent)
        , _highlighted(false)
        , _bottomGap(0)
    {
        setObjectName(QString::fromUtf8("ColorAdjustOpenBackdrop"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    }

    void setHighlighted(bool highlighted)
    {
        if (_highlighted != highlighted) {
            _highlighted = highlighted;
            // Read by the GUI evidence scripts, which cannot reach this class.
            setProperty("colorGlobalModeHighlighted", highlighted);
            update();
        }
    }

    void setBottomGap(int gap)
    {
        if (_bottomGap != gap) {
            _bottomGap = gap;
            update();
        }
    }

protected:
    virtual void paintEvent(QPaintEvent* /*e*/) OVERRIDE
    {
        const QPalette palette = QApplication::palette();
        const QColor base = palette.color(QPalette::Mid);
        const QColor sunken = palette.color(QPalette::Dark);
        QColor shadowEdge = sunken.darker(140);
        QColor lightEdge = palette.color(QPalette::Light);

        if (_highlighted) {
            // Only the outline marks the engaged knob, in the selection colour the stylesheet
            // uses; the fill stays the plain block's so the colours being edited keep their
            // surroundings.
            shadowEdge = ThemedIcon::themeSelectionColor();
            lightEdge = shadowEdge;
        }

        const QRect r = rect().adjusted(0, 0, 0, -_bottomGap);
        QPainter p(this);

        p.fillRect(r, mixColors(base, sunken, 0.5));
        p.setPen(shadowEdge);
        p.drawLine(r.topLeft(), r.topRight());
        p.drawLine(r.topLeft(), r.bottomLeft());
        p.setPen(lightEdge);
        p.drawLine(r.bottomLeft(), r.bottomRight());
        p.drawLine(r.topRight(), r.bottomRight());
    }

private:
    bool _highlighted;
    int _bottomGap;
};

} // namespace

ColorPickerLabel::ColorPickerLabel(KnobGuiColor* knob,
                                   QWidget* parent)
    : Label(parent)
    , _pickingEnabled(false)
    , _knob(knob)
    , _pressed(false)
    , _pressPos()
{
    setMouseTracking(true);
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
    _pressPos = e->position();
}

void
ColorPickerLabel::mouseReleaseEvent(QMouseEvent* e)
{
    if (!_knob || !_pressed) {
        Label::mouseReleaseEvent(e);

        return;
    }

    _pressed = false;
    if ((e->position() - _pressPos).manhattanLength() >= QApplication::startDragDistance()) {
        return;
    }
    _pickingEnabled = !_pickingEnabled;
    Q_EMIT pickingEnabled(_pickingEnabled);
    setColor(_currentColor); //< refresh the icon
}

void
ColorPickerLabel::enterEvent(QEnterEvent*)
{
    QToolTip::showText( QCursor::pos(), toolTip() );
}

void
ColorPickerLabel::leaveEvent(QEvent*)
{
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
    , _openBackdrop()
    , _openBlockWidgets()
    , _paddedLabelCell()
    , _paddedFieldCell()
    , _globalModeHighlighted(false)
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
    if (_openBackdrop) {
        _openBackdrop->deleteLater();
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
    if (!_adjustPanel || !_colorSelectorButton) {
        return;
    }
    _colorSelectorButton->setChecked(expanded);
}

QWidget*
KnobGuiColor::getOpenBackdrop() const
{
    return _openBackdrop.data();
}

ColorKnobEditSession*
KnobGuiColor::getEditSession()
{
    if (!_editSession) {
        _editSession.reset(new ColorKnobEditSession(shared_from_this()));
    }

    return _editSession.get();
}

ColorAdjustPanel::DisplayConverter
KnobGuiColor::getDisplayConverter() const
{
    return makeDisplayConverter(getColorManagement());
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

QWidget*
KnobGuiColor::getGlobalModeColumnWidget() const
{
    QVBoxLayout* columnLayout = getBelowRowColumnLayout();

    return columnLayout ? columnLayout->parentWidget() : 0;
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
    if (_globalModeHighlighted == highlighted) {
        return;
    }
    _globalModeHighlighted = highlighted;
    if (_adjustPanel) {
        refreshOpenBackdrop();
    }
}

bool
KnobGuiColor::isGlobalModeHighlighted() const
{
    return _globalModeHighlighted;
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
    _adjustPanel->setProperty(colorKnobTouchTagPropertyName(), touchTagFor(this));
    _adjustPanel->setKnobValue(ColorKnobEditSession::readKnobValue(knob),
                               knob->getDimension(),
                               knob->getColorRole(),
                               ColorKnobEditSession::basisFor(colorManagement));
    _adjustPanel->setDisplayConverter(makeDisplayConverter(colorManagement));
    QObject::connect(_adjustPanel.data(), &ColorAdjustPanel::valuesEdited, this, &KnobGuiColor::onAdjustPanelValuesEdited);
    refreshWheelFraction();
    if (SettingsPtr settings = appPTR ? appPTR->getCurrentSettings() : SettingsPtr()) {
        QObject::connect(settings.get(), &Settings::settingChanged, this, [this](KnobI*) {
            refreshWheelFraction();
        });
    }

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
    _adjustPanel->setEditable(editable);
}

void
KnobGuiColor::refreshWheelFraction()
{
    SettingsPtr settings = appPTR ? appPTR->getCurrentSettings() : SettingsPtr();

    if (_adjustPanel && settings) {
        _adjustPanel->setWheelFraction(settings->getColorTrackballWheelFraction());
    }
}

void
KnobGuiColor::onAdjustPanelToggled(bool expanded)
{
    if (!_adjustPanel) {
        return;
    }

    QWidget* topWidget = getContainer() ? getContainer()->getContainerWidget() : 0;
    QWidget* columnWidget = _adjustPanel->parentWidget();

    {
        // Everything in here is one update: the panel's visibility, the channel lines' fold
        // state, and the layout settling they both need. See activateLayoutsUpToInclusive().
        UpdatesHeldOff held(topWidget);

        if (expanded) {
            KnobColorPtr knob = _knob.lock();

            if (knob && !_adjustPanel->isEditing()) {
                _adjustPanel->setKnobValue(ColorKnobEditSession::readKnobValue(knob));
            }
            setDimensionsFolded(false);
            _adjustPanel->setVisible(true);
        } else {
            _adjustPanel->setVisible(false);
        }
        refreshOpenBackdrop();

        if (columnWidget) {
            activateLayoutsUpToInclusive(columnWidget, topWidget);
        }
        // Shown at its settled width, the panel sizes its wheel from that width, which can
        // change the panel's height.
        if (expanded) {
            activateLayoutsUpToInclusive(_adjustPanel->getEditArea(), topWidget);
        }
    }
    refreshAdjustPanelToolTip(expanded);
} // KnobGuiColor::onAdjustPanelToggled

void
KnobGuiColor::setOpenBlockTagged(bool tagged)
{
    for (const OpenBlockCell& cell : _openBlockWidgets) {
        if (cell.widget) {
            cell.widget->setProperty(openBlockPropertyName(), tagged);
            cell.widget->setContentsMargins(tagged ? cell.blockMargins : cell.restMargins);
            // Qt re-evaluates property selectors, the descendant ones included, only on polish.
            repolishTree(cell.widget.data());
        }
    }
    if (!tagged) {
        _openBlockWidgets.clear();
    }
}

void
KnobGuiColor::refreshOpenBackdrop()
{
    QWidget* fullWidthRow = getFullWidthRowWidget();
    QWidget* page = fullWidthRow ? fullWidthRow->parentWidget() : 0;
    QGridLayout* grid = page ? qobject_cast<QGridLayout*>(page->layout()) : 0;
    QWidget* field = _rowContainer.data();

    while (field && page && (field->parentWidget() != page)) {
        field = field->parentWidget();
    }

    const int fieldIndex = (grid && field) ? grid->indexOf(field) : -1;
    const int fullIndex = grid ? grid->indexOf(fullWidthRow) : -1;
    const bool open = isAdjustPanelExpanded() && !_adjustPanel->isHidden() && fullWidthRow && !fullWidthRow->isHidden() && (fieldIndex >= 0) && (fullIndex >= 0);
    // Engaged with the panel closed, the block still marks the knob, around its row alone.
    const bool rowOnly = !open && _globalModeHighlighted && (fieldIndex >= 0) && !field->isHidden() && _colorSelectorButton && !_colorSelectorButton->isHidden();

    setOpenBlockTagged(false);
    if (_openBackdrop) {
        if (QLayout* l = _openBackdrop->parentWidget() ? _openBackdrop->parentWidget()->layout() : 0) {
            l->removeWidget(_openBackdrop.data());
        }
        _openBackdrop->hide();
    }
    if (!open && !rowOnly) {
        return;
    }

    int firstRow, lastRow, rowSpan, column, columnSpan;
    grid->getItemPosition(fieldIndex, &firstRow, &column, &rowSpan, &columnSpan);
    lastRow = firstRow + rowSpan - 1;
    if (open) {
        grid->getItemPosition(fullIndex, &lastRow, &column, &rowSpan, &columnSpan);
        lastRow += rowSpan - 1;
    }

    // The cells along the block's edges are padded so their contents sit inside the frame by the
    // margin a settings panel keeps inside its own edge. The row above already ends with the row
    // gap; the block ends with one too, outside its frame, taken from the cells' own bottom gap
    // when they have one. The label and field cells keep their side padding at all times
    // (reserveOpenBlockSidePadding()), so only the full-width row gains any here.
    const int padX = TO_DPIX(NATRON_DENSITY_PANEL_MARGIN);
    const int padY = TO_DPIY(NATRON_DENSITY_PANEL_MARGIN);
    const int rowGap = TO_DPIY(NATRON_DENSITY_ROW_GAP);
    const int lastColumn = std::max(1, grid->columnCount()) - 1;

    // Rows are found afresh each time: knobs created later can shift the page's grid.
    for (int i = 0; i < grid->count(); ++i) {
        int row, col, rs, cs;
        grid->getItemPosition(i, &row, &col, &rs, &cs);
        QWidget* w = grid->itemAt(i)->widget();
        if (!w || (row < firstRow) || (row + rs - 1 > lastRow)) {
            continue;
        }
        OpenBlockCell cell;
        cell.widget = w;
        cell.restMargins = w->contentsMargins();
        QMargins pad;
        if (row == firstRow) {
            pad.setTop(padY);
        }
        if (row + rs - 1 == lastRow) {
            const int ownGap = w->layout() ? w->layout()->contentsMargins().bottom() : 0;
            pad.setBottom(padY + std::max(0, rowGap - ownGap));
        }
        if ((col == 0) && (w != _paddedLabelCell.data())) {
            pad.setLeft(padX);
        }
        if ((col + cs - 1 >= lastColumn) && (w != _paddedFieldCell.data())) {
            pad.setRight(padX);
        }
        cell.blockMargins = cell.restMargins + pad;
        _openBlockWidgets.push_back(cell);
    }

    if (!_openBackdrop || (_openBackdrop->parentWidget() != page)) {
        if (_openBackdrop) {
            _openBackdrop->deleteLater();
        }
        _openBackdrop = new AdjustPanelBackdrop(page);
        QObject::connect(fullWidthRow, &QObject::destroyed, _openBackdrop.data(), &QObject::deleteLater);
    }
    AdjustPanelBackdrop* backdrop = static_cast<AdjustPanelBackdrop*>(_openBackdrop.data());
    backdrop->setHighlighted(_globalModeHighlighted);
    backdrop->setBottomGap(rowGap);
    grid->addWidget(_openBackdrop.data(), firstRow, 0, lastRow - firstRow + 1, std::max(1, grid->columnCount()));
    _openBackdrop->lower();
    _openBackdrop->show();
    setOpenBlockTagged(true);
} // KnobGuiColor::refreshOpenBackdrop

// The block's side padding is kept by the label's left and the field's right whether or not the
// block shows: padding them only while it shows would narrow the field and move the row's
// buttons, the open toggle among them, out from under the cursor that clicked it.
void
KnobGuiColor::reserveOpenBlockSidePadding()
{
    const int padX = TO_DPIX(NATRON_DENSITY_PANEL_MARGIN);
    QWidget* label = getLabelContainer();
    QWidget* field = getFieldContainer();

    if (label && (label != _paddedLabelCell.data())) {
        label->setContentsMargins(label->contentsMargins() + QMargins(padX, 0, 0, 0));
        _paddedLabelCell = label;
    }
    if (field && (field != _paddedFieldCell.data())) {
        field->setContentsMargins(field->contentsMargins() + QMargins(0, 0, padX, 0));
        _paddedFieldCell = field;
    }
}

void
KnobGuiColor::refreshAdjustPanelToolTip(bool expanded)
{
    QIcon icon;
    appPTR->getIcon(NATRON_PIXMAP_COLOR_ADJUST_DROPDOWN, NATRON_MEDIUM_BUTTON_ICON_SIZE, &icon);
    if (expanded) {
        icon = verticallyMirroredIcon(icon, _colorSelectorButton->iconSize(), _colorSelectorButton->devicePixelRatio());
    }
    _colorSelectorButton->setIcon(icon);
    _colorSelectorButton->setToolTip(NATRON_NAMESPACE::convertFromPlainText(expanded ? tr("Hide the colour adjustment panel.") : tr("Show the colour adjustment panel: a trackball wheel with hue, saturation, value, temperature and tint sliders."),
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
    // Claims the row's extra width ahead of the swatch and buttons below, so they stay
    // right-aligned whether the slider is visible (its own stretch then wins) or hidden
    // for the broken-out spinboxes (this spacer is then the only stretchable item).
    containerLayout->addStretch(0);
    containerLayout->addSpacing( TO_DPIX(10) );
    KnobColorPtr knob = _knob.lock();
    if (_adjustPanel) {
        // Tags the row so ColorKnobTouchTracker's app-wide filter can find this knob from a
        // press, wheel or focus-in on any of its spinboxes, not only from a gesture session.
        QWidget* row = containerLayout->parentWidget();
        if (row) {
            row->setProperty(colorKnobTouchTagPropertyName(), touchTagFor(this));
        }
        _rowContainer = row;
        QWidget* dimensionLines = getDimensionLinesWidget();
        if (dimensionLines) {
            dimensionLines->setProperty(colorKnobTouchTagPropertyName(), touchTagFor(this));
        }
    }
    _colorLabel = new ColorPickerLabel( _useSimplifiedUI ? NULL : this, containerLayout->widget() );
    if (!_useSimplifiedUI) {
        _colorLabel->setToolTip(NATRON_NAMESPACE::convertFromPlainText(tr("To pick a color on a viewer, click this and then press control + left click on any viewer.\n"
                                                                          "You can also pick the average color of a given rectangle by holding control + shift + left click.\n"
                                                                          "To deselect the picker left click anywhere.\n"
                                                                          "Picked values are the image's scene-referred values, before the viewer's "
                                                                          "gain, gamma and display transform."),
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
        QIcon dropdownIcon;
        appPTR->getIcon(NATRON_PIXMAP_COLOR_ADJUST_DROPDOWN, NATRON_MEDIUM_BUTTON_ICON_SIZE, &dropdownIcon);
        _colorSelectorButton->setIcon(dropdownIcon);
        _colorSelectorButton->setIconSize(medIconSize);
        _colorSelectorButton->setArrowType(Qt::NoArrow);
        _colorSelectorButton->setAutoRaise(false);
        _colorSelectorButton->setCheckable(true);
        _colorSelectorButton->setChecked(false);
        _colorSelectorButton->setFocusPolicy(Qt::NoFocus);
        QObject::connect(_colorSelectorButton, &QToolButton::toggled, this, &KnobGuiColor::onAdjustPanelToggled);
        // The panel starts collapsed with nothing to undo, so the full toggle and its layout
        // flush are not needed here, once per colour knob, while the panel is being built.
        _adjustPanel->setVisible(false);
        refreshAdjustPanelToolTip(false);
        containerLayout->addWidget(_colorSelectorButton);

        _globalModeButton = new QToolButton(containerLayout->widget());
        _globalModeButton->setObjectName(QString::fromUtf8("ColorGlobalModeButton"));
        _globalModeButton->setFixedSize(medSize);
        QIcon globalModeIcon;
        appPTR->getIcon(NATRON_PIXMAP_COLOR_GLOBAL_TRACKBALL, NATRON_MEDIUM_BUTTON_ICON_SIZE, &globalModeIcon);
        _globalModeButton->setIcon(globalModeIcon);
        _globalModeButton->setIconSize(medIconSize);
        _globalModeButton->setCheckable(true);
        _globalModeButton->setChecked(false);
        _globalModeButton->setAutoRaise(false);
        _globalModeButton->setFocusPolicy(Qt::NoFocus);
        _globalModeButton->setToolTip(NATRON_NAMESPACE::convertFromPlainText(tr("Engage the global trackball mode (Ctrl+E) on this knob: press C (hue+saturation trackball), S (hue on x, saturation on y), T (temperature/tint) or V (value) to pick what a click-drag anywhere adjusts, until Esc or Enter accepts, or the shortcut is pressed again."), NATRON_NAMESPACE::WhiteSpaceNormal));
        QObject::connect(_globalModeButton, &QToolButton::clicked, this, &KnobGuiColor::engageGlobalMode);
        containerLayout->addWidget(_globalModeButton);

        // The panel has a row of its own, from the label column to the row's last button.
        setBelowRowWidgetFullWidth(true);
        reserveOpenBlockSidePadding();

        return;
    }

    QIcon colorSelectorPix;
    appPTR->getIcon(NATRON_PIXMAP_COLORWHEEL, TO_DPIX(NATRON_MEDIUM_BUTTON_ICON_SIZE), &colorSelectorPix);
    _colorSelectorButton->setIcon(colorSelectorPix);
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
    setDimensionsFolded(false);
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
    if (_globalModeButton) {
        _globalModeButton->hide();
    }
    if (_adjustPanel) {
        _adjustPanel->hide();
        refreshOpenBackdrop();
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
    if (_globalModeButton) {
        _globalModeButton->show();
    }
    if (isAdjustPanelExpanded()) {
        _adjustPanel->show();
        refreshOpenBackdrop();
    } else if (_globalModeHighlighted && _adjustPanel) {
        refreshOpenBackdrop();
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
        ScaleSliderQWidget* slider = getDimensionSlider(i);
        if (slider) {
            slider->setUseLineColor(false, Qt::red);
        }
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
        ScaleSliderQWidget* slider = getDimensionSlider(i);
        if (slider) {
            slider->setUseLineColor(true, colors[i]);
        }
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

bool
KnobGuiColor::wantsFixedDimensionLabelSlot() const
{
    // Only rows with the dropdown's below-row panel break out into channel lines, so only
    // they need the label slot reserved across the fold.
    return wantsPerDimensionSliders();
}

bool
KnobGuiColor::wantsPerDimensionSliders() const
{
    KnobColorPtr knob = _knob.lock();

    return knob && _adjustPanel && (knob->getDimension() > 1);
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_KnobGuiColor.cpp"
