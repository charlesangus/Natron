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

#include "KnobGuiValue.h"

#include <algorithm> // min, max
#include <functional>
#include <limits>
#include <stdexcept>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QApplication>
#include <QColorDialog>
#include <QEvent>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPointer>
#include <QScrollArea>
#include <QStyle>
#include <QToolTip>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
GCC_DIAG_UNUSED_PRIVATE_FIELD_OFF
// /opt/local/include/QtGui/qmime.h:119:10: warning: private field 'type' is not used [-Wunused-private-field]
#include <QKeyEvent>
GCC_DIAG_UNUSED_PRIVATE_FIELD_ON
#include <QDebug>
#include <QFontComboBox>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Lut.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/Settings.h"
#include "Engine/TimeLine.h"
#include "Engine/Utils.h" // convertFromPlainText
#include "Engine/ViewIdx.h"

#include "Gui/Button.h"
#include "Gui/ClickableLabel.h"
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
// Runs a callback whenever the watched widget is shown or hidden on its own account, which is
// all the full-width holder needs to follow it.
class ExplicitVisibilityWatcher
    : public QObject {
public:
    ExplicitVisibilityWatcher(QObject* parent,
                              const std::function<void()>& onChange)
        : QObject(parent)
        , _onChange(onChange)
    {
    }

    virtual bool eventFilter(QObject* watched,
                             QEvent* e) OVERRIDE FINAL
    {
        if ((e->type() == QEvent::ShowToParent) || (e->type() == QEvent::HideToParent)) {
            _onChange();
        }

        return QObject::eventFilter(watched, e);
    }

private:
    std::function<void()> _onChange;
};
} // namespace

struct KnobGuiValuePrivate
{
    KnobIWPtr knob;
    KnobIntBaseWPtr intKnob;
    KnobDoubleBaseWPtr doubleKnob;
    std::vector<std::pair<SpinBox *, ClickableLabel *> > spinBoxes;
    QWidget *container;
    QWidget* belowRowColumn; // non-null only when createBelowRowWidget() supplied a widget; owns "container" as a child
    QPointer<QWidget> belowRowWidget;
    QPointer<QObject> belowRowWidgetWatcher; // reaches back into this struct, so it goes first
    QWidget* fullWidthRow; // the below-row widget's holder when it has a row of its own; null with no below-row widget
    bool belowRowWidgetFullWidth;
    bool knobHidden;
    ScaleSliderQWidget *slider;
    Button* rectangleFormatButton;
    Button *dimensionSwitchButton;
    bool rectangleFormatIsWidthHeight;
    QWidget* dimensionLines;
    QGridLayout* dimensionLinesLayout;
    std::vector<ScaleSliderQWidget*> dimensionSliders; // indexed by dimension; entry 0 stays null, the row's slider drives it
    bool foldStateHeld;
    bool dimensionsFolded;

    KnobGuiValuePrivate(KnobIPtr knob)
        : knob(knob)
        , intKnob(std::dynamic_pointer_cast<KnobIntBase>(knob))
        , doubleKnob(std::dynamic_pointer_cast<KnobDoubleBase>(knob))
        , spinBoxes()
        , container(0)
        , belowRowColumn(0)
        , belowRowWidget()
        , belowRowWidgetWatcher()
        , fullWidthRow(0)
        , belowRowWidgetFullWidth(false)
        , knobHidden(false)
        , slider(0)
        , rectangleFormatButton(0)
        , dimensionSwitchButton(0)
        , rectangleFormatIsWidthHeight(true)
        , dimensionLines(0)
        , dimensionLinesLayout(0)
        , dimensionSliders()
        , foldStateHeld(false)
        , dimensionsFolded(false)
    {
    }

    KnobIntBasePtr getKnobAsInt() const
    {
        return intKnob.lock();
    }

    void refreshFullWidthRowVisibility()
    {
        if (!fullWidthRow) {
            return;
        }
        fullWidthRow->setVisible(belowRowWidgetFullWidth && !knobHidden && belowRowWidget && !belowRowWidget->isHidden());
    }

    KnobDoubleBasePtr getKnobAsDouble() const
    {
        return doubleKnob.lock();
    }

    KnobIPtr getKnob() const
    {
        return knob.lock();
    }

    double getKnobValue(int dimension) const
    {
        double value;
        KnobDoubleBasePtr k = getKnobAsDouble();
        KnobIntBasePtr i = getKnobAsInt();

        if (k) {
            value = k->getValue(dimension);
        } else if (i) {
            value = (double)i->getValue(dimension);
        } else {
            value = 0;
        }

        return value;
    }
};


/*
   FROM THE OFX SPEC:

 * kOfxParamCoordinatesNormalised is OFX > 1.2 and is used ONLY for setting defaults

   These new parameter types can set their defaults in one of two coordinate systems, the property kOfxParamPropDefaultCoordinateSystem. Specifies the coordinate system the default value is being specified in.

 * kOfxParamDoubleTypeNormalized* is OFX < 1.2 and is used ONLY for displaying the value: get/set should always return the normalized value:

   To flag to the host that a parameter as normalised, we use the kOfxParamPropDoubleType property. Parameters that are so flagged have values set and retrieved by an effect in normalized coordinates. However a host can choose to represent them to the user in whatever space it chooses.

   Both properties can be easily supported:
   - the first one is used ONLY when setting the *DEFAULT* value
   - the second is used ONLY by the GUI
 */
double
KnobGuiValue::valueAccordingToType(const bool doNormalize,
                                   const int dimension,
                                   const double value)
{
    if ( (dimension != 0) && (dimension != 1) ) {
        return value;
    }

    ValueIsNormalizedEnum state = getNormalizationPolicy(dimension);

    if (state != eValueIsNormalizedNone) {
        KnobIPtr knob = _imp->getKnob();
        if (knob) {
            SequenceTime time = knob->getCurrentTime();
            if (doNormalize) {
                return normalize(dimension, time, value);
            } else {
                return denormalize(dimension, time, value);
            }
        }
    }

    return value;
}

bool
KnobGuiValue::shouldAddStretch() const
{
    return isSliderDisabled();
}

KnobGuiValue::KnobGuiValue(KnobIPtr knob,
                           KnobGuiContainerI *container)
    : KnobGui(knob, container)
    , _imp( new KnobGuiValuePrivate(knob) )
{
    KnobSignalSlotHandlerPtr handler = knob->getSignalSlotHandler();

    if (handler) {
#ifdef SPINBOX_TAKE_PLUGIN_RANGE_INTO_ACCOUNT
        QObject::connect( handler.get(), SIGNAL(minMaxChanged(double,double,int)), this, SLOT(onMinMaxChanged(double,double,int)) );
#endif
        QObject::connect( handler.get(), SIGNAL(displayMinMaxChanged(double,double,int)), this, SLOT(onDisplayMinMaxChanged(double,double,int)) );
    }
}

KnobGuiValue::~KnobGuiValue()
{
    delete _imp->belowRowWidgetWatcher.data();
}

void
KnobGuiValue::removeSpecificGui()
{
    // The column, when there is one, owns "container" as a child, so deleting it is enough.
    if (_imp->belowRowColumn) {
        _imp->belowRowColumn->deleteLater();
    } else {
        _imp->container->deleteLater();
    }
    delete _imp->belowRowWidgetWatcher.data();
    if (_imp->fullWidthRow) {
        _imp->fullWidthRow->deleteLater();
        _imp->fullWidthRow = 0;
    }
    _imp->belowRowWidgetFullWidth = false;
    _imp->spinBoxes.clear();
    _imp->dimensionLines = 0;
    _imp->dimensionLinesLayout = 0;
    _imp->dimensionSliders.clear();
}

void
KnobGuiValue::createWidget(QHBoxLayout* layout)
{
    connectKnobSignalSlots();

    _imp->container = new QWidget( layout->parentWidget() );
    QHBoxLayout* containerLayout = new QHBoxLayout(_imp->container);

    containerLayout->setContentsMargins(0, 0, 0, 0);
    containerLayout->setSpacing(3);

    if (getKnobsCountOnSameLine() > 1) {
        disableSlider();
    }

    // Rows that share a line with other knobs have no single field column to grow,
    // so they never get a below-row widget.
    QWidget* belowRowWidget = (getKnobsCountOnSameLine() > 1) ? 0 : createBelowRowWidget(layout->parentWidget());

    if (belowRowWidget) {
        _imp->belowRowColumn = new QWidget(layout->parentWidget());
        QVBoxLayout* columnLayout = new QVBoxLayout(_imp->belowRowColumn);
        columnLayout->setContentsMargins(0, 0, 0, 0);
        columnLayout->setSpacing(0);
        columnLayout->addWidget(_imp->container);
        belowRowWidget->setParent(_imp->belowRowColumn);
        belowRowWidget->hide();
        columnLayout->addWidget(belowRowWidget);
        layout->addWidget(_imp->belowRowColumn);
        _imp->belowRowWidget = belowRowWidget;

        // The page re-parents the holder into its grid; until the widget moves in it stays empty
        // and hidden, so its row takes no space.
        _imp->fullWidthRow = new QWidget(layout->parentWidget());
        QVBoxLayout* holderLayout = new QVBoxLayout(_imp->fullWidthRow);
        holderLayout->setContentsMargins(0, 0, 0, 0);
        holderLayout->setSpacing(0);
        _imp->fullWidthRow->hide();

        KnobGuiValuePrivate* imp = _imp.get();
        _imp->belowRowWidgetWatcher = new ExplicitVisibilityWatcher(belowRowWidget, [imp]() {
            imp->refreshFullWidthRowVisibility();
        });
        belowRowWidget->installEventFilter(_imp->belowRowWidgetWatcher.data());
    } else {
        layout->addWidget(_imp->container);
    }

    if ( !isSliderDisabled() ) {
        // A row with a below-row widget must be able to grow when that widget is shown,
        // so its fixed height only applies to rows that cannot have one.
        if (belowRowWidget) {
            layout->parentWidget()->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        } else {
            layout->parentWidget()->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        }
    }

    KnobIPtr knob = _imp->getKnob();
    const int nDims = knob->getDimension();
    std::vector<double> increments, displayMins, displayMaxs, mins, maxs;
    std::vector<int> decimals;
    KnobDoubleBasePtr doubleKnob = _imp->getKnobAsDouble();
    KnobIntBasePtr intKnob = _imp->getKnobAsInt();
    if (doubleKnob) {
        displayMins = doubleKnob->getDisplayMinimums();
        displayMaxs = doubleKnob->getDisplayMaximums();
        mins = doubleKnob->getMinimums();
        maxs = doubleKnob->getMaximums();
    } else {
        const std::vector<int>& intDmins = intKnob->getDisplayMinimums();
        const std::vector<int>& intDMaxs = intKnob->getDisplayMaximums();
        const std::vector<int>& intMins = intKnob->getMinimums();
        const std::vector<int>& intMaxs = intKnob->getMaximums();
        assert( intDMaxs.size() == intDmins.size() && intDmins.size() == intMins.size() && intMins.size() == intMaxs.size() );
        displayMins.resize( intDmins.size() );
        displayMaxs.resize( intDmins.size() );
        mins.resize( intDmins.size() );
        maxs.resize( intDmins.size() );

        for (std::size_t i = 0; i < intMins.size(); ++i) {
            displayMins[i] = intDmins[i];
            displayMaxs[i] = intDMaxs[i];
            mins[i] = intMins[i];
            maxs[i] = intMaxs[i];
        }
    }

    getIncrements(&increments);
    getDecimals(&decimals);

    std::vector<std::string> dimensionLabels(nDims);
    bool isRectangleParam = isRectangleType();
    // This is a rectangle parameter
    if (isRectangleParam) {
        dimensionLabels[0] = "x";
        dimensionLabels[1] = "y";
        dimensionLabels[2] = "w";
        dimensionLabels[3] = "h";
    } else {
        for (int i = 0; i < nDims; ++i) {
            dimensionLabels[i] = knob->getDimensionName(i);
        }
    }

    KnobGuiPtr thisShared = shared_from_this();
    SpinBox::SpinBoxTypeEnum type;
    if (doubleKnob) {
        type = SpinBox::eSpinBoxTypeDouble;
    } else {
        type = SpinBox::eSpinBoxTypeInt;
    }


    int nItemsPerRow = nDims;
    if (std::floor(nDims / 3. + 0.5) == nDims / 3.) {
        nItemsPerRow = 3;
    }
    if (std::floor(nDims / 4. + 0.5) == nDims / 4.) {
        nItemsPerRow = 4;
    }

    int nbRows = nDims / nItemsPerRow;
    assert(nbRows >= 1);
    QWidget* allSpinBoxesContainer = 0;
    QGridLayout* spinBoxesGrid = 0;
    if (nbRows == 1) {
        allSpinBoxesContainer = _imp->container;
    } else {
        allSpinBoxesContainer = new QWidget(_imp->container);
        spinBoxesGrid = new QGridLayout(allSpinBoxesContainer);
        spinBoxesGrid->setContentsMargins(0, 0, 0, 0);
        spinBoxesGrid->setVerticalSpacing( TO_DPIY(1) );
        spinBoxesGrid->setHorizontalSpacing( TO_DPIX(1) );
    }

    _imp->spinBoxes.resize(nDims);

    int dimensionLabelSlotWidth = 0;
    if (wantsFixedDimensionLabelSlot() || wantsPerDimensionSliders()) {
        QFontMetrics fm(QApplication::font());
        for (int i = 0; i < nDims; ++i) {
            dimensionLabelSlotWidth = std::max(dimensionLabelSlotWidth, fm.horizontalAdvance(QString::fromUtf8(dimensionLabels[i].c_str())));
        }
    }

    std::vector<QWidget*> boxContainers(nDims, (QWidget*)0);
    int rowIndex = 0;
    int columnIndex = 0;
    for (std::size_t i = 0; i < _imp->spinBoxes.size(); ++i) {
        QWidget *boxContainer = new QWidget(allSpinBoxesContainer);
        boxContainers[i] = boxContainer;
        boxContainer->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

        QHBoxLayout *boxContainerLayout = 0;
        boxContainerLayout = new QHBoxLayout(boxContainer);
        boxContainerLayout->setContentsMargins(0, 0, 0, 0);
        boxContainerLayout->setSpacing(3);


        ClickableLabel *subDesc = 0;
        if ( (nDims != 1) && (nbRows == 1) ) {
            subDesc = new ClickableLabel(QString::fromUtf8( dimensionLabels[i].c_str() ), boxContainer);
            if ((i == 0) && wantsFixedDimensionLabelSlot()) {
                subDesc->setFixedWidth(dimensionLabelSlotWidth);
            }
            boxContainerLayout->addWidget(subDesc);
        }

        SpinBox *box = new KnobSpinBox(layout->parentWidget(), type, thisShared, i);
        NumericKnobValidator* validator = new NumericKnobValidator(box, thisShared);
        box->setValidator(validator);
        QObject::connect( box, SIGNAL(valueChanged(double)), this, SLOT(onSpinBoxValueChanged()) );

        // Set the copy/link actions in the right click menu of the SpinBox
        enableRightClickMenu(box, i);

#ifdef SPINBOX_TAKE_PLUGIN_RANGE_INTO_ACCOUNT
        double min = valueAccordingToType(false, i, mins[i]);
        double max = valueAccordingToType(false, i, maxs[i]);
        box->setMaximum(max);
        box->setMinimum(min);
#endif


        if (type == SpinBox::eSpinBoxTypeDouble) {
            // Set the number of digits after the decimal point
            if ( i < decimals.size() ) {
                box->decimals(decimals[i]);
            }
        }

        if ( i < increments.size() ) {
            double incr = 1;
            incr = valueAccordingToType(false, i, increments[i]);
            box->setIncrement(incr);
        }
        boxContainerLayout->addWidget(box);
        if (!spinBoxesGrid) {
            containerLayout->addWidget(boxContainer);
        } else {
            spinBoxesGrid->addWidget(boxContainer, rowIndex, columnIndex);
        }
        _imp->spinBoxes[i] = std::make_pair(box, subDesc);

        ++columnIndex;
        if (columnIndex >= nItemsPerRow) {
            columnIndex = 0;
            ++rowIndex;
        }
    }

    if (spinBoxesGrid) {
        containerLayout->addWidget(allSpinBoxesContainer);
    }

    bool sliderVisible = false;
    if (!isSliderDisabled() && !isRectangleParam) {
        double dispmin = displayMins[0];
        double dispmax = displayMaxs[0];
        if (dispmin == std::numeric_limits<double>::lowest()) {
            dispmin = mins[0];
        }
        if (dispmax == std::numeric_limits<double>::max()) {
            dispmax = maxs[0];
        }

        // denormalize if necessary
        double dispminGui = valueAccordingToType(false, 0, dispmin);
        double dispmaxGui = valueAccordingToType(false, 0, dispmax);
        bool spatial = isSpatialType();
        Format f;
        if (spatial) {
            getKnob()->getHolder()->getApp()->getProject()->getProjectDefaultFormat(&f);
        }
        if (dispminGui < -SLIDER_MAX_RANGE) {
            if (spatial) {
                dispminGui = -f.width();
            } else {
                dispminGui = -SLIDER_MAX_RANGE;
            }
        }
        if (dispmaxGui > SLIDER_MAX_RANGE) {
            if (spatial) {
                dispmaxGui = f.width();
            } else {
                dispmaxGui = SLIDER_MAX_RANGE;
            }
        }

        double value0 = _imp->getKnobValue(0);
        ScaleSliderQWidget::DataTypeEnum sliderType;
        if (doubleKnob) {
            sliderType = ScaleSliderQWidget::eDataTypeDouble;
        } else {
            sliderType = ScaleSliderQWidget::eDataTypeInt;
        }


        _imp->slider = new ScaleSliderQWidget( dispminGui, dispmaxGui, value0, knob->getEvaluateOnChange(),
                                               sliderType, getGui(), eScaleTypeLinear, layout->parentWidget() );

        _imp->slider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        if ( hasToolTip() ) {
            _imp->slider->setToolTip( toolTip() );
        }
        QObject::connect( _imp->slider, SIGNAL(resetToDefaultRequested()), this, SLOT(onResetToDefaultRequested()) );
        QObject::connect( _imp->slider, SIGNAL(positionChanged(double)), this, SLOT(onSliderValueChanged(double)) );
        QObject::connect( _imp->slider, SIGNAL(editingFinished(bool)), this, SLOT(onSliderEditingFinished(bool)) );
        if (spinBoxesGrid) {
            spinBoxesGrid->addWidget(_imp->slider, rowIndex, columnIndex);
            // Lets the slider's column claim extra width over a trailing stretch item added
            // by addExtraWidgets(), instead of splitting it with that item.
            spinBoxesGrid->setColumnStretch(columnIndex, 1);
        } else {
            containerLayout->addWidget(_imp->slider, 1);
        }

        sliderVisible = shouldSliderBeVisible(dispminGui, dispmaxGui);

        // onDisplayMinMaxChanged takes original (maybe normalized) values
        onDisplayMinMaxChanged(dispmin, dispmax);
    }

    QSize medSize( TO_DPIX(NATRON_MEDIUM_BUTTON_SIZE), TO_DPIY(NATRON_MEDIUM_BUTTON_SIZE) );
    QSize medIconSize( TO_DPIX(NATRON_MEDIUM_BUTTON_ICON_SIZE), TO_DPIY(NATRON_MEDIUM_BUTTON_ICON_SIZE) );

    if (isRectangleParam) {
        _imp->rectangleFormatButton = new Button(QIcon(), QString::fromUtf8("wh"), _imp->container);
        _imp->rectangleFormatButton->setToolTip( NATRON_NAMESPACE::convertFromPlainText(tr("Switch between width/height and right/top notation"), NATRON_NAMESPACE::WhiteSpaceNormal) );
        _imp->rectangleFormatButton->setFixedSize(medSize);
        _imp->rectangleFormatButton->setIconSize(medIconSize);
        _imp->rectangleFormatButton->setFocusPolicy(Qt::NoFocus);
        _imp->rectangleFormatButton->setCheckable(true);
        _imp->rectangleFormatButton->setChecked(_imp->rectangleFormatIsWidthHeight);
        _imp->rectangleFormatButton->setDown(_imp->rectangleFormatIsWidthHeight);
        QObject::connect( _imp->rectangleFormatButton, SIGNAL(clicked(bool)), this, SLOT(onRectangleFormatButtonClicked()) );
        containerLayout->addWidget(_imp->rectangleFormatButton);
    }

    if ( (nDims > 1) && !isSliderDisabled() && sliderVisible ) {
        _imp->dimensionSwitchButton = new Button(QIcon(), QString::number(nDims), _imp->container);
        _imp->dimensionSwitchButton->setToolTip( NATRON_NAMESPACE::convertFromPlainText(tr("Switch between a single value for all dimensions and multiple values."), NATRON_NAMESPACE::WhiteSpaceNormal) );
        _imp->dimensionSwitchButton->setFixedSize(medSize);
        _imp->dimensionSwitchButton->setIconSize(medIconSize);
        _imp->dimensionSwitchButton->setFocusPolicy(Qt::NoFocus);
        _imp->dimensionSwitchButton->setCheckable(true);
        containerLayout->addWidget(_imp->dimensionSwitchButton);

        if (wantsPerDimensionSliders() && _imp->belowRowColumn && !spinBoxesGrid) {
            createDimensionLines(containerLayout, boxContainers, dimensionLabelSlotWidth, displayMins, displayMaxs, mins, maxs);
        }
        if (!_imp->dimensionLines) {
            // Expanded, the slider hides and the fixed-size boxes would spread across the row;
            // this keeps them packed and the switch button at the row's end, where it sits
            // while folded. A folded slider's stretch factor outweighs this one's.
            containerLayout->insertStretch(containerLayout->indexOf(_imp->dimensionSwitchButton), 0);
        }

        bool showSlider = true;
        double firstDimensionValue = 0.;
        SequenceTime time = 0;
        if ( knob->getHolder() && knob->getHolder()->getApp() ) {
            time = knob->getHolder()->getApp()->getTimeLine()->currentFrame();
        }
        for (int i = 0; i < nDims; ++i) {
            double v = _imp->getKnobValue(i);
            if (getNormalizationPolicy(i) != eValueIsNormalizedNone) {
                v = denormalize(i, time, v);
            }
            if (i == 0) {
                firstDimensionValue = v;
            } else if (v != firstDimensionValue) {
                showSlider = false;
                break;
            }
        }
        bool unfolded;
        if (knob->hasDimensionsFoldState()) {
            // A fold state set by hand or by a loaded project is kept, unless the dimensions
            // have come to differ since, which a folded knob would hide.
            unfolded = knob->getDimensionsUnfoldedInGui() || !areAllDimensionsEqual();
        } else {
            unfolded = !showSlider || !isAutoFoldDimensionsEnabled() || !areAllDimensionCurvesEqual();
        }
        knob->setDimensionsUnfoldedInGui(unfolded);
        if (unfolded) {
            if (_imp->dimensionLines) {
                expandAllDimensions();
            } else {
                _imp->slider->hide();
                _imp->dimensionSwitchButton->setChecked(true);
            }
        } else {
            foldAllDimensions();
        }

        QObject::connect( _imp->dimensionSwitchButton, SIGNAL(clicked(bool)), this, SLOT(onDimensionSwitchClicked(bool)) );
        KnobSignalSlotHandlerPtr handler = knob->getSignalSlotHandler();
        if (handler) {
            QObject::connect(handler.get(), SIGNAL(dimensionsFoldStateRestored()), this, SLOT(onDimensionsFoldStateRestored()));
        }
    } else {
        if (_imp->slider) {
            _imp->slider->hide();
        }
    }

    addExtraWidgets(containerLayout);

    if (_imp->dimensionLines) {
        setDimensionLinesRightInset(containerLayout);
    }
} // createWidget

void
KnobGuiValue::createDimensionLines(QHBoxLayout* containerLayout,
                                   const std::vector<QWidget*>& boxContainers,
                                   int labelSlotWidth,
                                   const std::vector<double>& displayMins,
                                   const std::vector<double>& displayMaxs,
                                   const std::vector<double>& mins,
                                   const std::vector<double>& maxs)
{
    QVBoxLayout* columnLayout = qobject_cast<QVBoxLayout*>(_imp->belowRowColumn->layout());
    if (!columnLayout) {
        return;
    }

    KnobIPtr knob = _imp->getKnob();
    const int nDims = (int)_imp->spinBoxes.size();
    const ScaleSliderQWidget::DataTypeEnum sliderType = _imp->getKnobAsDouble() ? ScaleSliderQWidget::eDataTypeDouble : ScaleSliderQWidget::eDataTypeInt;

    _imp->dimensionLines = new QWidget(_imp->belowRowColumn);
    _imp->dimensionLinesLayout = new QGridLayout(_imp->dimensionLines);
    _imp->dimensionLinesLayout->setContentsMargins(0, 0, 0, 0);
    // Matches the row's own spacing so each line's spinbox and slider start at the same x as
    // spinbox 0 and the row's slider.
    _imp->dimensionLinesLayout->setHorizontalSpacing(containerLayout->spacing());
    _imp->dimensionLinesLayout->setVerticalSpacing(TO_DPIY(1));
    _imp->dimensionLinesLayout->setColumnStretch(2, 1);
    _imp->dimensionSliders.assign(nDims, (ScaleSliderQWidget*)0);

    for (int i = 1; i < nDims; ++i) {
        const int line = i - 1;
        SpinBox* box = _imp->spinBoxes[i].first;
        ClickableLabel* label = _imp->spinBoxes[i].second;

        if (label) {
            label->setFixedWidth(labelSlotWidth);
            _imp->dimensionLinesLayout->addWidget(label, line, 0);
        }
        _imp->dimensionLinesLayout->addWidget(box, line, 1);

        double dispmin = displayMins[i];
        double dispmax = displayMaxs[i];
        if (dispmin == std::numeric_limits<double>::lowest()) {
            dispmin = mins[i];
        }
        if (dispmax == std::numeric_limits<double>::max()) {
            dispmax = maxs[i];
        }

        ScaleSliderQWidget* slider = new ScaleSliderQWidget(0., 1., _imp->getKnobValue(i), knob->getEvaluateOnChange(),
                                                            sliderType, getGui(), eScaleTypeLinear, _imp->dimensionLines);
        slider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        if (hasToolTip()) {
            slider->setToolTip(toolTip());
        }
        QObject::connect(slider, &ScaleSliderQWidget::positionChanged, this, [this, i](double v) {
            onDimensionSliderValueChanged(i, v);
        });
        QObject::connect(slider, &ScaleSliderQWidget::editingFinished, this, [this, i](bool hasMovedOnce) {
            onDimensionSliderEditingFinished(i, hasMovedOnce);
        });
        QObject::connect(slider, &ScaleSliderQWidget::resetToDefaultRequested, this, [this, i]() {
            onDimensionSliderResetRequested(i);
        });
        _imp->dimensionLinesLayout->addWidget(slider, line, 2);
        _imp->dimensionSliders[i] = slider;

        onDisplayMinMaxChanged(dispmin, dispmax, i);
        slider->seekScalePosition(valueAccordingToType(false, i, _imp->getKnobValue(i)));

        // The box's label and spinbox now live on the line, so its slot in the row is dropped.
        if (boxContainers[i]) {
            containerLayout->removeWidget(boxContainers[i]);
            delete boxContainers[i];
        }
    }

    _imp->dimensionLines->hide();
    // After the row and before the subclass's below-row widget.
    columnLayout->insertWidget(1, _imp->dimensionLines);
} // KnobGuiValue::createDimensionLines

void
KnobGuiValue::setDimensionLinesRightInset(QHBoxLayout* containerLayout)
{
    const int sliderIndex = containerLayout->indexOf(_imp->slider);
    if ((sliderIndex < 0) || !_imp->dimensionLinesLayout) {
        return;
    }

    // Everything right of the row's slider has a fixed width, so insetting the lines by the
    // same amount ends every channel slider at the row slider's right edge. QBoxLayout puts
    // its spacing only between widget items; spacer items contribute just their own width.
    int inset = 0;
    for (int i = sliderIndex + 1; i < containerLayout->count(); ++i) {
        QLayoutItem* item = containerLayout->itemAt(i);
        if (!item) {
            continue;
        }
        if (item->widget()) {
            inset += containerLayout->spacing();
        }
        inset += item->sizeHint().width();
    }
    _imp->dimensionLinesLayout->setContentsMargins(0, 0, inset, 0);
}

ScaleSliderQWidget*
KnobGuiValue::getDimensionSlider(int dimension) const
{
    if (!_imp->dimensionLines || (dimension < 0) || (dimension >= (int)_imp->dimensionSliders.size())) {
        return 0;
    }
    if (dimension == 0) {
        return _imp->slider;
    }

    return _imp->dimensionSliders[dimension];
}

QWidget*
KnobGuiValue::getDimensionLinesWidget() const
{
    return _imp->dimensionLines;
}

QVBoxLayout*
KnobGuiValue::getBelowRowColumnLayout() const
{
    return _imp->belowRowColumn ? qobject_cast<QVBoxLayout*>(_imp->belowRowColumn->layout()) : 0;
}

QWidget*
KnobGuiValue::getFullWidthRowWidget() const
{
    return _imp->fullWidthRow;
}

bool
KnobGuiValue::isBelowRowWidgetFullWidth() const
{
    return _imp->belowRowWidgetFullWidth;
}

void
KnobGuiValue::setBelowRowWidgetFullWidth(bool fullWidth)
{
    QWidget* widget = _imp->belowRowWidget.data();
    QVBoxLayout* columnLayout = getBelowRowColumnLayout();
    QVBoxLayout* holderLayout = _imp->fullWidthRow ? qobject_cast<QVBoxLayout*>(_imp->fullWidthRow->layout()) : 0;

    if (!widget || !columnLayout || !holderLayout || (fullWidth == _imp->belowRowWidgetFullWidth)) {
        return;
    }

    QVBoxLayout* from = fullWidth ? columnLayout : holderLayout;
    QVBoxLayout* to = fullWidth ? holderLayout : columnLayout;

    // setParent() hides a widget; its own shown state is what the caller left it in.
    const bool wasHidden = widget->isHidden();

    _imp->belowRowWidgetFullWidth = fullWidth;
    from->removeWidget(widget);
    widget->setParent(to->parentWidget());
    to->addWidget(widget);
    widget->setVisible(!wasHidden);
    _imp->refreshFullWidthRowVisibility();
}

void
KnobGuiValue::setFoldStateHeld(bool held)
{
    _imp->foldStateHeld = held;
}

void
KnobGuiValue::onDimensionsFoldStateRestored()
{
    KnobIPtr knob = _imp->getKnob();
    if (!knob) {
        return;
    }
    setDimensionsFolded(!knob->getDimensionsUnfoldedInGui() && areAllDimensionsEqual());
}

bool
KnobGuiValue::areAllDimensionsEqual() const
{
    KnobIPtr knob = _imp->getKnob();
    const int nDims = (int)_imp->spinBoxes.size();
    if (!knob || (nDims < 2)) {
        return true;
    }

    const double ref = _imp->getKnobValue(0);
    const std::string refExpression = knob->getExpression(0);
    for (int i = 1; i < nDims; ++i) {
        if ((_imp->getKnobValue(i) != ref) || (knob->getExpression(i) != refExpression)) {
            return false;
        }
    }

    return areAllDimensionCurvesEqual();
}

bool
KnobGuiValue::areAllDimensionCurvesEqual() const
{
    KnobIPtr knob = _imp->getKnob();
    if (!knob || (knob->getDimension() < 2)) {
        return true;
    }

    CurvePtr refCurve = knob->getCurve(ViewIdx(0), 0, true);
    const KeyFrameSet refKeys = refCurve ? refCurve->getKeyFrames_mt_safe() : KeyFrameSet();
    for (int i = 1; i < knob->getDimension(); ++i) {
        CurvePtr curve = knob->getCurve(ViewIdx(0), i, true);
        const KeyFrameSet keys = curve ? curve->getKeyFrames_mt_safe() : KeyFrameSet();
        if (keys != refKeys) {
            return false;
        }
    }

    return true;
}

void
KnobGuiValue::setDimensionsFolded(bool folded)
{
    if (!_imp->dimensionSwitchButton) {
        return;
    }
    KnobIPtr knob = _imp->getKnob();
    if (knob) {
        knob->setDimensionsUnfoldedInGui(!folded);
    }
    if (folded) {
        foldAllDimensions();
    } else {
        expandAllDimensions();
    }
}

namespace {

class FoldStateHoldGuard {
public:
    explicit FoldStateHoldGuard(bool& held)
        : _held(held)
        , _wasHeld(held)
    {
        _held = true;
    }

    ~FoldStateHoldGuard()
    {
        _held = _wasHeld;
    }

private:
    FoldStateHoldGuard(const FoldStateHoldGuard&) = delete;
    FoldStateHoldGuard& operator=(const FoldStateHoldGuard&) = delete;

    bool& _held;
    const bool _wasHeld;
};

} // namespace

void
KnobGuiValue::copyDimensionZeroValueToOthers()
{
    KnobIPtr knob = _imp->getKnob();
    KnobDoubleBasePtr doubleKnob = _imp->getKnobAsDouble();
    KnobIntBasePtr intKnob = _imp->getKnobAsInt();
    if (!knob || _imp->spinBoxes.empty()) {
        return;
    }
    const int nDims = knob->getDimension();
    if (nDims <= 1) {
        return;
    }
    SequenceTime time = knob->getCurrentTime();
    double firstDimensionValue = _imp->spinBoxes[0].first->value();
    if (getNormalizationPolicy(0) != eValueIsNormalizedNone) {
        firstDimensionValue = denormalize(0, time, firstDimensionValue);
    }
    // Each dimension's value change refreshes the GUI at once, while the dimensions after it still
    // differ, which would otherwise record the knob as unfolded.
    FoldStateHoldGuard holdFoldState(_imp->foldStateHeld);
    knob->beginChanges();
    for (int i = 1; i < nDims; ++i) {
        double v = firstDimensionValue;
        if (getNormalizationPolicy(i) != eValueIsNormalizedNone) {
            v = normalize(i, time, v);
        }
        if (doubleKnob) {
            doubleKnob->setValue(v, ViewSpec::all(), i);
        } else {
            assert(intKnob);
            intKnob->setValue(v, ViewSpec::all(), i);
        }
    }
    knob->endChanges();
}

void
KnobGuiValue::seekDimensionSlidersToSpinBoxes()
{
    for (std::size_t i = 1; i < _imp->dimensionSliders.size(); ++i) {
        if (_imp->dimensionSliders[i]) {
            _imp->dimensionSliders[i]->seekScalePosition(_imp->spinBoxes[i].first->value());
        }
    }
}

void
KnobGuiValue::onDimensionSliderValueChanged(int dimension,
                                            double d)
{
    if (appPTR->getCurrentSettings()->getRenderOnEditingFinishedOnly()) {
        return;
    }
    dimensionSliderEditingEnd(dimension, d);
}

void
KnobGuiValue::onDimensionSliderEditingFinished(int dimension,
                                               bool hasMovedOnce)
{
    ScaleSliderQWidget* slider = getDimensionSlider(dimension);
    if (!slider) {
        return;
    }
    SettingsPtr settings = appPTR->getCurrentSettings();
    if (settings->getRenderOnEditingFinishedOnly()) {
        dimensionSliderEditingEnd(dimension, slider->getPosition());
    } else if (settings->isAutoProxyEnabled() && hasMovedOnce && getGui()) {
        getGui()->renderAllViewers(true);
    }
}

void
KnobGuiValue::onDimensionSliderResetRequested(int dimension)
{
    std::list<KnobIPtr> knobsList;

    knobsList.push_back(_imp->knob.lock());
    pushUndoCommand(new RestoreDefaultsCommand(false, knobsList, dimension));
}

void
KnobGuiValue::dimensionSliderEditingEnd(int dimension,
                                        double d)
{
    ScaleSliderQWidget* slider = getDimensionSlider(dimension);
    if (!slider || (dimension >= (int)_imp->spinBoxes.size())) {
        return;
    }

    KnobDoubleBasePtr doubleKnob = _imp->getKnobAsDouble();
    KnobIntBasePtr intKnob = _imp->getKnobAsInt();

    if (doubleKnob) {
        int digits = std::max(0, (int)-std::floor(std::log10(slider->increment())));
        QString str;
        str.setNum(d, 'f', digits);
        d = str.toDouble();
    }
    _imp->spinBoxes[dimension].first->setValue(d);
    d = valueAccordingToType(true, dimension, d);
    if (doubleKnob) {
        pushUndoCommand(new KnobUndoCommand<double>(shared_from_this(), doubleKnob->getValue(dimension), d, dimension, false));
    } else {
        assert(intKnob);
        pushUndoCommand(new KnobUndoCommand<int>(shared_from_this(), intKnob->getValue(dimension), (int)d, dimension, false));
    }

    std::vector<double> values(_imp->spinBoxes.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = ((int)i == dimension) ? d : _imp->getKnobValue(i);
    }
    updateExtraGui(values);
}

void
KnobGuiValue::onResetToDefaultRequested()
{
    KnobIPtr knob = _imp->knob.lock();
    std::list<KnobIPtr> knobsList;

    knobsList.push_back(knob);
    const bool rowSliderDrivesDimensionZero = _imp->dimensionLines && _imp->dimensionSwitchButton && _imp->dimensionSwitchButton->isChecked();
    pushUndoCommand(new RestoreDefaultsCommand(false, knobsList, rowSliderDrivesDimensionZero ? 0 : -1));
}

void
KnobGuiValue::getSpinBox(int dim,
                         SpinBox** spinbox,
                         Label** label) const
{
    assert( dim >= 0 && dim < (int)_imp->spinBoxes.size() );
    if (spinbox) {
        *spinbox = _imp->spinBoxes[dim].first;
    }
    if (label) {
        *label = _imp->spinBoxes[dim].second;
    }
}

bool
KnobGuiValue::getAllDimensionsVisible() const
{
    return !_imp->dimensionSwitchButton || _imp->dimensionSwitchButton->isChecked();
}

int
KnobGuiValue::getDimensionForSpinBox(const SpinBox* spinbox) const
{
    for (std::size_t i = 0; i < _imp->spinBoxes.size(); ++i) {
        if (_imp->spinBoxes[i].first == spinbox) {
            return (int)i;
        }
    }

    return -1;
}

void
KnobGuiValue::onRectangleFormatButtonClicked()
{
    if (!_imp->rectangleFormatButton) {
        return;
    }
    _imp->rectangleFormatIsWidthHeight = !_imp->rectangleFormatIsWidthHeight;
    _imp->rectangleFormatButton->setDown(_imp->rectangleFormatIsWidthHeight);
    if (_imp->rectangleFormatIsWidthHeight) {
        if (_imp->spinBoxes[2].second) {
            _imp->spinBoxes[2].second->setText( QString::fromUtf8("w") );
        }
        if (_imp->spinBoxes[3].second) {
            _imp->spinBoxes[3].second->setText( QString::fromUtf8("h") );
        }
    } else {
        if (_imp->spinBoxes[2].second) {
            _imp->spinBoxes[2].second->setText( QString::fromUtf8("r") );
        }
        if (_imp->spinBoxes[3].second) {
            _imp->spinBoxes[3].second->setText( QString::fromUtf8("t") );
        }
    }
    updateGUI(2);
    updateGUI(3);
}

namespace {

class DimensionsFoldCommand
    : public QUndoCommand {
    Q_DECLARE_TR_FUNCTIONS(DimensionsFoldCommand)

public:
    DimensionsFoldCommand(const KnobGuiPtr& knobGui,
                          bool fold)
        : QUndoCommand()
        , _knobGui(knobGui)
        , _fold(fold)
        , _knobBeforeFold()
    {
        KnobIPtr knob = knobGui->getKnob();
        // Folding writes dimension 0's value into the others, which undo has to put back.
        if (_fold && knob) {
            _knobBeforeFold = MultipleKnobEditsUndoCommand::createCopyForKnob(knob);
        }
        const QString label = knob ? QString::fromUtf8(knob->getLabel().c_str()) : QString();
        setText(_fold ? tr("Fold %1").arg(label) : tr("Expand %1").arg(label));
    }

    virtual ~DimensionsFoldCommand() OVERRIDE
    {
    }

    virtual void undo() OVERRIDE FINAL
    {
        KnobGuiPtr knobGui = _knobGui.lock();
        KnobGuiValue* isValue = dynamic_cast<KnobGuiValue*>(knobGui.get());
        if (!isValue) {
            return;
        }
        KnobIPtr knob = knobGui->getKnob();
        if (knob && _knobBeforeFold) {
            knob->cloneAndUpdateGui(_knobBeforeFold.get());
        }
        isValue->setDimensionsFolded(!_fold);
    }

    virtual void redo() OVERRIDE FINAL
    {
        KnobGuiPtr knobGui = _knobGui.lock();
        KnobGuiValue* isValue = dynamic_cast<KnobGuiValue*>(knobGui.get());
        if (!isValue) {
            return;
        }
        if (_fold) {
            isValue->copyDimensionZeroValueToOthers();
        }
        isValue->setDimensionsFolded(_fold);
    }

private:
    KnobGuiWPtr _knobGui;
    bool _fold;
    KnobIPtr _knobBeforeFold;
};

} // namespace

void
KnobGuiValue::onDimensionSwitchClicked(bool clicked)
{
    if (!_imp->dimensionSwitchButton) {
        if (_imp->slider) {
            _imp->slider->hide();
        }

        return;
    }
    pushUndoCommand(new DimensionsFoldCommand(shared_from_this(), !clicked));
}

void
KnobGuiValue::expandAllDimensions()
{
    if (!_imp->dimensionSwitchButton) {
        return;
    }

    _imp->dimensionSwitchButton->setChecked(true);
    _imp->dimensionSwitchButton->setDown(true);
    if (_imp->dimensionLines) {
        _imp->slider->setVisible(shouldSliderBeVisible(_imp->slider->minimum(), _imp->slider->maximum()));
    } else {
        _imp->slider->hide();
    }
    for (std::size_t i = 0; i < _imp->spinBoxes.size(); ++i) {
        if (i > 0) {
            _imp->spinBoxes[i].first->show();
        }
        setDimensionLabelVisible(i, true);
    }
    if (_imp->dimensionLines) {
        seekDimensionSlidersToSpinBoxes();
        _imp->dimensionLines->show();
    }
    onDimensionsExpanded();
    if (_imp->dimensionsFolded) {
        _imp->dimensionsFolded = false;
        Q_EMIT dimensionsFoldedChanged(false);
    }
}

void
KnobGuiValue::foldAllDimensions()
{
    if (!_imp->dimensionSwitchButton) {
        return;
    }

    _imp->dimensionSwitchButton->setChecked(false);
    _imp->dimensionSwitchButton->setDown(false);
    _imp->slider->show();
    for (std::size_t i = 0; i < _imp->spinBoxes.size(); ++i) {
        if (i > 0) {
            _imp->spinBoxes[i].first->hide();
        }
        setDimensionLabelVisible(i, false);
    }
    if (_imp->dimensionLines) {
        _imp->dimensionLines->hide();
    }
    onDimensionsFolded();
    if (!_imp->dimensionsFolded) {
        _imp->dimensionsFolded = true;
        Q_EMIT dimensionsFoldedChanged(true);
    }
}

void
KnobGuiValue::setDimensionLabelVisible(std::size_t index,
                                       bool visible)
{
    if (index >= _imp->spinBoxes.size()) {
        return;
    }
    ClickableLabel* label = _imp->spinBoxes[index].second;
    if (!label) {
        return;
    }
    if ((index == 0) && wantsFixedDimensionLabelSlot()) {
        // The slot has a fixed width, so blanking its text rather than hiding it keeps
        // spinbox 0 and the slider at the same x whether the knob is folded or not.
        if (visible) {
            KnobIPtr knob = _imp->getKnob();
            label->setText(QString::fromUtf8(knob->getDimensionName((int)index).c_str()));
        } else {
            label->setText(QString());
        }
        label->show();
    } else {
        label->setVisible(visible);
    }
}

void
KnobGuiValue::onMinMaxChanged(const double mini,
                              const double maxi,
                              const int index)
{
#ifdef SPINBOX_TAKE_PLUGIN_RANGE_INTO_ACCOUNT
    assert( _imp->spinBoxes.size() > std::size_t(index) );
    _imp->spinBoxes[index].first->setMinimum( valueAccordingToType(false, index, mini) );
    _imp->spinBoxes[index].first->setMaximum( valueAccordingToType(false, index, maxi) );
#else
    (void)mini;
    (void)maxi;
    (void)index;
#endif
}

void
KnobGuiValue::onDisplayMinMaxChanged(const double mini,
                                     const double maxi,
                                     int index)
{
    ScaleSliderQWidget* targetSlider = _imp->slider;
    if ((index > 0) && (index < (int)_imp->dimensionSliders.size()) && _imp->dimensionSliders[index]) {
        targetSlider = _imp->dimensionSliders[index];
    }
    if (targetSlider) {
        double sliderMin = valueAccordingToType(false, index, mini);
        double sliderMax = valueAccordingToType(false, index, maxi);
        if ( (sliderMax - sliderMin) >= SLIDER_MAX_RANGE ) {
            // Use min max for slider if dispmin/dispmax was not set
            KnobDoubleBasePtr doubleKnob = _imp->getKnobAsDouble();
            KnobIntBasePtr intKnob = _imp->getKnobAsInt();
            double min, max;
            if (doubleKnob) {
                max = valueAccordingToType( false, index, doubleKnob->getMaximum(index) );
                min = valueAccordingToType( false, index, doubleKnob->getMinimum(index) );
            } else {
                assert(intKnob);
                max = valueAccordingToType( false, index, intKnob->getMaximum(index) );
                min = valueAccordingToType( false, index, intKnob->getMinimum(index) );
            }
            if ( (max - min) < SLIDER_MAX_RANGE ) {
                sliderMin = min;
                sliderMax = max;
            }
        }

        if (sliderMin < -SLIDER_MAX_RANGE) {
            if ( isSpatialType() ) {
                Format f;
                getKnob()->getHolder()->getApp()->getProject()->getProjectDefaultFormat(&f);
                sliderMin = -f.width();
            } else {
                sliderMin = -SLIDER_MAX_RANGE;
            }
        }
        if (sliderMax > SLIDER_MAX_RANGE) {
            if ( isSpatialType() ) {
                Format f;
                getKnob()->getHolder()->getApp()->getProject()->getProjectDefaultFormat(&f);
                sliderMax = f.width();
            } else {
                sliderMax = SLIDER_MAX_RANGE;
            }
        }

        if (targetSlider == _imp->slider) {
            if (shouldSliderBeVisible(sliderMin, sliderMax)) {
                _imp->slider->setVisible(true);
            } else {
                _imp->slider->setVisible(false);
            }
        }
        if (sliderMax > sliderMin) {
            targetSlider->setMinimumAndMaximum(sliderMin, sliderMax);
        }
    }
} // KnobGuiValue::onDisplayMinMaxChanged

void
KnobGuiValue::onIncrementChanged(const double incr,
                                 const int index)
{
    assert(_imp->spinBoxes.size() > (std::size_t)index);
    _imp->spinBoxes[index].first->setIncrement( valueAccordingToType(false, index, incr) );
}

void
KnobGuiValue::onDecimalsChanged(const int deci,
                                const int index)
{
    assert(_imp->spinBoxes.size() > (std::size_t)index);
    _imp->spinBoxes[index].first->decimals(deci);
}

void
KnobGuiValue::updateGUI(const int dimension)
{
    KnobIPtr knob = _imp->getKnob();
    const int knobDim = (int)_imp->spinBoxes.size();

    if ( (knobDim < 1) || (dimension >= knobDim) ) {
        return;
    }
    assert(1 <= knobDim);
    assert( dimension == -1 || (dimension >= 0 && dimension < knobDim) );
    std::vector<double> values(knobDim);
    double refValue = 0.;
    std::vector<std::string> expressions(knobDim);
    std::string refExpresion;
    SequenceTime time = 0;
    if ( knob->getHolder() && knob->getHolder()->getApp() ) {
        time = knob->getCurrentTime();
    }

    for (int i = 0; i < knobDim; ++i) {
        double v = _imp->getKnobValue(i);
        if (getNormalizationPolicy(i) != eValueIsNormalizedNone) {
            v = denormalize(i, time, v);
        }

        // For rectangle in top/right mode, convert values
        if ( (knobDim == 4) && !_imp->rectangleFormatIsWidthHeight ) {
            if (i == 2) {
                v = values[0] + v;
            } else if (i == 3) {
                v = values[1] + v;
            }
        }
        values[i] = v;
        expressions[i] = knob->getExpression(i);
    }

    if (dimension == -1) {
        refValue = values[0];
        refExpresion = expressions[0];
    } else {
        refValue = values[dimension];
        refExpresion = expressions[dimension];
    }
    bool allValuesNotEqual = false;
    for (int i = 0; i < knobDim; ++i) {
        if ( (values[i] != refValue) || (expressions[i] != refExpresion) ) {
            allValuesNotEqual = true;
        }
    }
    if (!_imp->foldStateHeld && _imp->dimensionSwitchButton) {
        if (allValuesNotEqual) {
            knob->setDimensionsUnfoldedInGui(true);
        }
        if (knob->getDimensionsUnfoldedInGui() && !_imp->dimensionSwitchButton->isChecked()) {
            expandAllDimensions();
        }
    }

    if (dimension != -1) {
        switch (dimension) {
        case 0:
            assert(knobDim >= 1);
            if (_imp->slider) {
                _imp->slider->seekScalePosition(values[0]);
            }
            _imp->spinBoxes[dimension].first->setValue(values[dimension]);
            if ( _imp->dimensionSwitchButton && !_imp->dimensionSwitchButton->isChecked() ) {
                for (int i = 1; i < knobDim; ++i) {
                    _imp->spinBoxes[i].first->setValue(values[dimension]);
                }
            }
            break;
        default:
            _imp->spinBoxes[dimension].first->setValue(values[dimension]);
            if ((dimension < (int)_imp->dimensionSliders.size()) && _imp->dimensionSliders[dimension]) {
                _imp->dimensionSliders[dimension]->seekScalePosition(values[dimension]);
            }
            break;
        }
    } else {
        if (_imp->slider) {
            _imp->slider->seekScalePosition(values[0]);
        }
        if ( _imp->dimensionSwitchButton && !_imp->dimensionSwitchButton->isChecked() ) {
            for (int i = 0; i < knobDim; ++i) {
                _imp->spinBoxes[i].first->setValue(values[0]);
            }
        } else {
            for (int i = 0; i < knobDim; ++i) {
                _imp->spinBoxes[i].first->setValue(values[i]);
            }
        }
        for (int i = 1; i < (int)_imp->dimensionSliders.size(); ++i) {
            if (_imp->dimensionSliders[i]) {
                _imp->dimensionSliders[i]->seekScalePosition(values[i]);
            }
        }
    }

    updateExtraGui(values);
} // KnobGuiValue::updateGUI

void
KnobGuiValue::reflectAnimationLevel(int dimension,
                                    AnimationLevelEnum level)
{
    int value;

    switch (level) {
    case eAnimationLevelNone:
        value = 0;
        break;
    case eAnimationLevelInterpolatedValue:
        value = 1;
        break;
    case eAnimationLevelOnKeyframe:
        value = 2;
        break;
    default:
        value = 0;
        break;
    }
    if ( value != _imp->spinBoxes[dimension].first->getAnimation() ) {
        _imp->spinBoxes[dimension].first->setAnimation(value);
    }
}

void
KnobGuiValue::onSliderValueChanged(const double d)
{
    assert( _imp->knob.lock()->isEnabled(0) );
    bool penUpOnly = appPTR->getCurrentSettings()->getRenderOnEditingFinishedOnly();

    if (penUpOnly) {
        return;
    }
    sliderEditingEnd(d);
}

void
KnobGuiValue::onSliderEditingFinished(bool hasMovedOnce)
{
    assert( _imp->knob.lock()->isEnabled(0) );
    SettingsPtr settings = appPTR->getCurrentSettings();
    bool onEditingFinishedOnly = settings->getRenderOnEditingFinishedOnly();
    bool autoProxyEnabled = settings->isAutoProxyEnabled();
    if (onEditingFinishedOnly) {
        double v = _imp->slider->getPosition();
        sliderEditingEnd(v);
    } else if (autoProxyEnabled && hasMovedOnce && getGui()) {
        getGui()->renderAllViewers(true);
    }
}

void
KnobGuiValue::sliderEditingEnd(double d)
{
    if (_imp->dimensionLines && _imp->dimensionSwitchButton && _imp->dimensionSwitchButton->isChecked()) {
        dimensionSliderEditingEnd(0, d);

        return;
    }

    KnobDoubleBasePtr doubleKnob = _imp->getKnobAsDouble();
    KnobIntBasePtr intKnob = _imp->getKnobAsInt();

    if (doubleKnob) {
        int digits = std::max( 0, (int)-std::floor( std::log10( _imp->slider->increment() ) ) );
        QString str;
        str.setNum(d, 'f', digits);
        d = str.toDouble();
    }
    std::vector<double> newValuesVec;
    if (_imp->dimensionSwitchButton) {
        // The folded slider stands for every dimension, but one driven by an expression or a
        // link keeps its driver's value. Driven trailing dimensions are left out of the undo
        // command altogether, so it never writes them; one between two free ones is rewritten
        // with the value it already shows.
        KnobIPtr knob = _imp->getKnob();
        std::vector<bool> driven(_imp->spinBoxes.size(), false);
        int written = 1;

        for (std::size_t i = 0; i < _imp->spinBoxes.size(); ++i) {
            driven[i] = knob && (!knob->getExpression((int)i).empty() || knob->isSlave((int)i));
            if (!driven[i]) {
                _imp->spinBoxes[i].first->setValue(d);
                written = (int)i + 1;
            }
        }
        d = valueAccordingToType(true, 0, d);

        if (doubleKnob) {
            std::list<double> oldValues, newValues;
            for (int i = 0; i < written; ++i) {
                const double old = doubleKnob->getValue(i);

                oldValues.push_back(old);
                newValues.push_back(driven[i] ? old : d);
                newValuesVec.push_back(driven[i] ? old : d);
            }
            pushUndoCommand( new KnobUndoCommand<double>(shared_from_this(), oldValues, newValues, false) );
        } else {
            assert(intKnob);
            std::list<int> oldValues, newValues;
            for (int i = 0; i < written; ++i) {
                const int old = intKnob->getValue(i);

                oldValues.push_back(old);
                newValues.push_back(driven[i] ? old : (int)d);
                newValuesVec.push_back(driven[i] ? old : d);
            }
            pushUndoCommand( new KnobUndoCommand<int>(shared_from_this(), oldValues, newValues, false) );
        }
        for (int i = written; i < (int)_imp->spinBoxes.size(); ++i) {
            newValuesVec.push_back(doubleKnob ? doubleKnob->getValue(i) : (double)intKnob->getValue(i));
        }
    } else {
        _imp->spinBoxes[0].first->setValue(d);
        d = valueAccordingToType(true, 0, d);
        if (doubleKnob) {
            pushUndoCommand( new KnobUndoCommand<double>(shared_from_this(), doubleKnob->getValue(0), d, 0, false) );
        } else {
            assert(intKnob);
            pushUndoCommand( new KnobUndoCommand<int>(shared_from_this(), intKnob->getValue(0), (int)d, 0, false) );
        }
        newValuesVec.push_back(d);
    }
    updateExtraGui(newValuesVec);
}

void
KnobGuiValue::onSpinBoxValueChanged()
{
    SpinBox* changedBox = qobject_cast<SpinBox*>( sender() );
    KnobDoubleBasePtr doubleKnob = _imp->getKnobAsDouble();
    KnobIntBasePtr intKnob = _imp->getKnobAsInt();
    int spinBoxDim = -1;
    std::vector<double> oldValue( _imp->spinBoxes.size() );
    std::vector<double> newValue( _imp->spinBoxes.size() );

    for (std::size_t i = 0; i < _imp->spinBoxes.size(); ++i) {
        newValue[i] = valueAccordingToType( true, i, _imp->spinBoxes[i].first->value() );
        if ( (_imp->spinBoxes.size() == 4) && !_imp->rectangleFormatIsWidthHeight ) {
            if (i == 2) {
                newValue[i] = _imp->spinBoxes[0].first->value();
            } else if (i == 3) {
                newValue[i] = _imp->spinBoxes[1].first->value();
            }
        }
        oldValue[i] = _imp->getKnobValue(i);
        if ( (_imp->spinBoxes[i].first == changedBox) && changedBox ) {
            spinBoxDim = i;
        }
    }

    if ( _imp->dimensionSwitchButton && !_imp->dimensionSwitchButton->isChecked() ) {
        // use the value of the first dimension only, and set all spinboxes
        for (std::size_t i = 1; i < _imp->spinBoxes.size(); ++i) {
            if (_imp->spinBoxes[i].first != changedBox) {
                _imp->spinBoxes[i].first->setValue(newValue[0]);
            }
        }

        for (std::size_t i = 1; i < _imp->spinBoxes.size(); ++i) {
            newValue[i] = newValue[0];
        }
        spinBoxDim = -1;
    }

    if (spinBoxDim != -1) {
        if (doubleKnob) {
            pushUndoCommand( new KnobUndoCommand<double>(shared_from_this(), oldValue[spinBoxDim], newValue[spinBoxDim], spinBoxDim, false) );
        } else {
            pushUndoCommand( new KnobUndoCommand<int>(shared_from_this(), (int)oldValue[spinBoxDim], (int)newValue[spinBoxDim], spinBoxDim, false) );
        }
    } else {
        if (doubleKnob) {
            std::list<double> oldValues, newValues;
            for (std::size_t i = 0; i < _imp->spinBoxes.size(); ++i) {
                newValues.push_back(newValue[i]);
                oldValues.push_back(oldValue[i]);
            }
            pushUndoCommand( new KnobUndoCommand<double>(shared_from_this(), oldValues, newValues, false) );
        } else {
            std::list<int> oldValues, newValues;
            for (std::size_t i = 0; i < _imp->spinBoxes.size(); ++i) {
                newValues.push_back( (int)newValue[i] );
                oldValues.push_back( (int)oldValue[i] );
            }
            pushUndoCommand( new KnobUndoCommand<int>(shared_from_this(), oldValues, newValues, false) );
        }
    }

    updateExtraGui(newValue);

    if (_imp->slider) {
        _imp->slider->seekScalePosition(newValue[0]);
    }
    seekDimensionSlidersToSpinBoxes();
} // KnobGuiValue::onSpinBoxValueChanged

void
KnobGuiValue::_hide()
{
    _imp->knobHidden = true;
    _imp->refreshFullWidthRowVisibility();
    for (std::size_t i = 0; i < _imp->spinBoxes.size(); ++i) {
        _imp->spinBoxes[i].first->hide();
        if (_imp->spinBoxes[i].second) {
            _imp->spinBoxes[i].second->hide();
        }
    }
    if (_imp->slider) {
        _imp->slider->hide();
    }
    if (_imp->dimensionSwitchButton) {
        _imp->dimensionSwitchButton->hide();
    }
    if (_imp->rectangleFormatButton) {
        _imp->rectangleFormatButton->hide();
    }
    if (_imp->dimensionLines) {
        _imp->dimensionLines->hide();
    }
}

void
KnobGuiValue::_show()
{
    _imp->knobHidden = false;
    _imp->refreshFullWidthRowVisibility();
    for (std::size_t i = 0; i < _imp->spinBoxes.size(); ++i) {
        if ( !_imp->dimensionSwitchButton || ( (i > 0) && _imp->dimensionSwitchButton->isChecked() ) || (i == 0) ) {
            _imp->spinBoxes[i].first->show();
        }
        if ( !_imp->dimensionSwitchButton || _imp->dimensionSwitchButton->isChecked() ) {
            if (_imp->spinBoxes[i].second) {
                _imp->spinBoxes[i].second->show();
            }
        } else if ((i == 0) && wantsFixedDimensionLabelSlot()) {
            setDimensionLabelVisible(0, false);
        }
    }
    if (_imp->slider && ((!_imp->dimensionSwitchButton && (_imp->spinBoxes.size() == 1)) || (_imp->dimensionSwitchButton && !_imp->dimensionSwitchButton->isChecked()) || _imp->dimensionLines)) {
        double sliderMax = _imp->slider->maximum();
        double sliderMin = _imp->slider->minimum();
        if ( (sliderMax > sliderMin) && ( (sliderMax - sliderMin) < SLIDER_MAX_RANGE ) && (sliderMax < std::numeric_limits<int>::max()) && (sliderMin > std::numeric_limits<int>::min()) ) {
            _imp->slider->show();
        }
    }
    if (_imp->dimensionSwitchButton) {
        _imp->dimensionSwitchButton->show();
    }
    if (_imp->rectangleFormatButton) {
        _imp->rectangleFormatButton->show();
    }
    if (_imp->dimensionLines && _imp->dimensionSwitchButton && _imp->dimensionSwitchButton->isChecked()) {
        _imp->dimensionLines->show();
    }
}

void
KnobGuiValue::setEnabled()
{
    KnobIPtr knob = _imp->getKnob();
    bool enabled0 = knob->isEnabled(0)  && !knob->isSlave(0) && knob->getExpression(0).empty();

    for (std::size_t i = 0; i < _imp->spinBoxes.size(); ++i) {
        bool b = knob->isEnabled(i) && !knob->isSlave(i);
        _imp->spinBoxes[i].first->setReadOnly_NoFocusRect(!b);
        if (_imp->spinBoxes[i].second) {
            _imp->spinBoxes[i].second->setReadOnly(!b);
        }
    }
    if (_imp->slider) {
        _imp->slider->setReadOnly( !enabled0 );
    }
    for (std::size_t i = 1; i < _imp->dimensionSliders.size(); ++i) {
        if (_imp->dimensionSliders[i]) {
            bool enabled = knob->isEnabled(i) && !knob->isSlave(i) && knob->getExpression(i).empty();
            _imp->dimensionSliders[i]->setReadOnly(!enabled);
        }
    }

    setEnabledExtraGui(enabled0);
}

void
KnobGuiValue::setReadOnly(bool readOnly,
                          int dimension)
{
    assert( dimension < (int)_imp->spinBoxes.size() );
    _imp->spinBoxes[dimension].first->setReadOnly_NoFocusRect(readOnly);
    if ( _imp->slider && (dimension == 0) ) {
        _imp->slider->setReadOnly(readOnly);
    }
    if ((dimension > 0) && (dimension < (int)_imp->dimensionSliders.size()) && _imp->dimensionSliders[dimension]) {
        _imp->dimensionSliders[dimension]->setReadOnly(readOnly);
    }
}

void
KnobGuiValue::setDirty(bool dirty)
{
    for (U32 i = 0; i < _imp->spinBoxes.size(); ++i) {
        _imp->spinBoxes[i].first->setDirty(dirty);
    }
}

KnobIPtr
KnobGuiValue::getKnob() const
{
    return _imp->getKnob();
}

void
KnobGuiValue::reflectExpressionState(int dimension,
                                     bool hasExpr)
{
    KnobIPtr knob = _imp->getKnob();

    if (hasExpr) {
        _imp->spinBoxes[dimension].first->setAnimation(3);
        //_spinBoxes[dimension].first->setReadOnly_NoFocusRect(true);
        // With per-dimension lines the row's slider only ever drives dimension 0 once another
        // dimension differs, so an expression elsewhere leaves it editable.
        if (_imp->slider && ((dimension == 0) || !_imp->dimensionLines)) {
            _imp->slider->setReadOnly(true);
        }
        if ((dimension > 0) && (dimension < (int)_imp->dimensionSliders.size()) && _imp->dimensionSliders[dimension]) {
            _imp->dimensionSliders[dimension]->setReadOnly(true);
        }
    } else {
        AnimationLevelEnum lvl = knob->getAnimationLevel(dimension);
        _imp->spinBoxes[dimension].first->setAnimation( (int)lvl );
        bool isEnabled = knob->isEnabled(dimension);
        _imp->spinBoxes[dimension].first->setReadOnly_NoFocusRect(!isEnabled);
        if (_imp->slider) {
            bool isEnabled0 = knob->isEnabled(0);
            _imp->slider->setReadOnly(!isEnabled0);
        }
        if ((dimension > 0) && (dimension < (int)_imp->dimensionSliders.size()) && _imp->dimensionSliders[dimension]) {
            _imp->dimensionSliders[dimension]->setReadOnly(!isEnabled || knob->isSlave(dimension));
        }
    }
}

void
KnobGuiValue::updateToolTip()
{
    if ( hasToolTip() ) {
        QString tt = toolTip();
        for (std::size_t i = 0; i < _imp->spinBoxes.size(); ++i) {
            _imp->spinBoxes[i].first->setToolTip( tt );
        }
        if (_imp->slider) {
            _imp->slider->setToolTip(tt);
        }
        for (std::size_t i = 1; i < _imp->dimensionSliders.size(); ++i) {
            if (_imp->dimensionSliders[i]) {
                _imp->dimensionSliders[i]->setToolTip(tt);
            }
        }
    }
}

void
KnobGuiValue::reflectModificationsState()
{
    bool hasModif = _imp->getKnob()->hasModifications();

    for (std::size_t i = 0; i < _imp->spinBoxes.size(); ++i) {
        _imp->spinBoxes[i].first->setAltered(!hasModif);
        if (_imp->spinBoxes[i].second) {
            _imp->spinBoxes[i].second->setAltered(!hasModif);
        }
    }
    if (_imp->slider) {
        _imp->slider->setAltered(!hasModif);
    }
    for (std::size_t i = 1; i < _imp->dimensionSliders.size(); ++i) {
        if (_imp->dimensionSliders[i]) {
            _imp->dimensionSliders[i]->setAltered(!hasModif);
        }
    }
}

void
KnobGuiValue::refreshDimensionName(int dim)
{
    if ( (dim < 0) || ( dim >= (int)_imp->spinBoxes.size() ) ) {
        return;
    }
    KnobIPtr knob = _imp->getKnob();
    if (_imp->spinBoxes[dim].second) {
        _imp->spinBoxes[dim].second->setText( QString::fromUtf8( knob->getDimensionName(dim).c_str() ) );;
    }
}

KnobGuiDouble::KnobGuiDouble(KnobIPtr knob,
                             KnobGuiContainerI *container)
    : KnobGuiValue(knob, container)
    , _knob( std::dynamic_pointer_cast<KnobDouble>(knob) )
{
}

bool
KnobGuiDouble::isSliderDisabled() const
{
    KnobDoublePtr knob = _knob.lock();
    if (!knob) {
        return false;
    }
    return knob->isSliderDisabled();
}

bool
KnobGuiDouble::isAutoFoldDimensionsEnabled() const
{
    KnobDoublePtr knob = _knob.lock();
    if (!knob) {
        return false;
    }
    return knob->isAutoFoldDimensionsEnabled();
}

bool
KnobGuiDouble::isRectangleType() const
{
    KnobDoublePtr knob = _knob.lock();
    if (!knob) {
        return false;
    }
    return knob->isRectangle();
}

bool
KnobGuiDouble::isSpatialType() const
{
    KnobDoublePtr knob = _knob.lock();
    if (!knob) {
        return false;
    }
    return knob->getIsSpatial();
}

ValueIsNormalizedEnum
KnobGuiDouble::getNormalizationPolicy(int dimension) const
{
    KnobDoublePtr knob = _knob.lock();
    if (!knob) {
        return eValueIsNormalizedNone;
    }
    return knob->getValueIsNormalized(dimension);
}

double
KnobGuiDouble::denormalize(int dimension,
                           double time,
                           double value) const
{
    KnobDoublePtr knob = _knob.lock();
    if (!knob) {
        return value;
    }
    return knob->denormalize(dimension, time, value);
}

double
KnobGuiDouble::normalize(int dimension,
                         double time,
                         double value) const
{
    KnobDoublePtr knob = _knob.lock();
    if (!knob) {
        return value;
    }
    return knob->normalize(dimension, time, value);
}

void
KnobGuiDouble::connectKnobSignalSlots()
{
    KnobDoublePtr knob = _knob.lock();
    QObject::connect( knob.get(), SIGNAL(incrementChanged(double,int)), this, SLOT(onIncrementChanged(double,int)) );
    QObject::connect( knob.get(), SIGNAL(decimalsChanged(int,int)), this, SLOT(onDecimalsChanged(int,int)) );
}

void
KnobGuiDouble::disableSlider()
{
    KnobDoublePtr knob = _knob.lock();
    if (!knob) {
        return;
    }
    return knob->disableSlider();
}

void
KnobGuiDouble::getIncrements(std::vector<double>* increments) const
{
    KnobDoublePtr knob = _knob.lock();
    if (!knob) {
        return;
    }
    *increments = knob->getIncrements();
}

void
KnobGuiDouble::getDecimals(std::vector<int>* decimals) const
{
    KnobDoublePtr knob = _knob.lock();
    if (!knob) {
        return;
    }
    *decimals = knob->getDecimals();
}

KnobGuiInt::KnobGuiInt(KnobIPtr knob,
                       KnobGuiContainerI *container)
    : KnobGuiValue(knob, container)
    , _knob( std::dynamic_pointer_cast<KnobInt>(knob) )
{
}

bool
KnobGuiInt::isSliderDisabled() const
{
    KnobIntPtr knob = _knob.lock();
    if (!knob) {
        return false;
    }
    return knob->isSliderDisabled();
}

bool
KnobGuiInt::isAutoFoldDimensionsEnabled() const
{
    KnobIntPtr knob = _knob.lock();
    if (!knob) {
        return false;
    }
    return knob->isAutoFoldDimensionsEnabled();
}

bool
KnobGuiInt::isRectangleType() const
{
    KnobIntPtr knob = _knob.lock();
    if (!knob) {
        return false;
    }
    return knob->isRectangle();
}

void
KnobGuiInt::connectKnobSignalSlots()
{
    KnobIntPtr knob = _knob.lock();
    QObject::connect( knob.get(), SIGNAL(incrementChanged(double,int)), this, SLOT(onIncrementChanged(double,int)) );
}

void
KnobGuiInt::disableSlider()
{
    KnobIntPtr knob = _knob.lock();
    if (!knob) {
        return;
    }
    return knob->disableSlider();
}

void
KnobGuiInt::getIncrements(std::vector<double>* increments) const
{
    KnobIntPtr knob = _knob.lock();
    if (!knob) {
        return;
    }
    const std::vector<int>& incr = knob->getIncrements();

    increments->resize( incr.size() );
    for (std::size_t i = 0; i < incr.size(); ++i) {
        (*increments)[i] = (double)incr[i];
    }
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_KnobGuiValue.cpp"
