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

#include "ColorAdjustPanel.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QColor>
#include <QGradient>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QSettings>
#include <QStringList>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QVariant>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/ProjectColorManagement.h"

#include "Gui/Button.h"
#include "Gui/ColorEditSlider.h"
#include "Gui/ColorSelectorWidget.h"
#include "Gui/Label.h"
#include "Gui/LineEdit.h"

NATRON_NAMESPACE_ENTER

namespace {
const double kRec709Weights[3] = { 0.2126729, 0.7151522, 0.0721750 };
const double kGrey = 0.18;
const int kSliderCount = 6;
const int kFieldCount = 8;
const int kNumericFieldCount = 7;

// Below this distance from white a value has no usable hue of its own, so the remembered hue
// is kept rather than overwritten by the noise angle of a near-neutral point.
const double kNeutralSaturation = 1e-9;

// The Hue track is painted at this fraction of the rim when the value is (near) neutral, so the
// spectrum stays readable instead of collapsing to grey.
const double kHueTrackSaturationFraction = 0.5;

const ColorSliderKindEnum kSliderKinds[kSliderCount] = {
    eColorSliderKindHue,
    eColorSliderKindSat,
    eColorSliderKindValue,
    eColorSliderKindTemp,
    eColorSliderKindTint,
    eColorSliderKindAlpha
};

KnobColorValue
neutralGain()
{
    KnobColorValue v;

    v.rgb[0] = v.rgb[1] = v.rgb[2] = 1.0;
    v.a = 1.0;
    v.hasAlpha = true;

    return v;
}

QString
sliderTitle(ColorSliderKindEnum kind)
{
    switch (kind) {
    case eColorSliderKindHue:
        return QObject::tr("Hue");
    case eColorSliderKindSat:
        return QObject::tr("Sat");
    case eColorSliderKindValue:
        return QObject::tr("Value");
    case eColorSliderKindTemp:
        return QObject::tr("Temp");
    case eColorSliderKindTint:
        return QObject::tr("Tint");
    case eColorSliderKindAlpha:
        return QObject::tr("Alpha");
    }

    return QString();
}

QString
fieldTitle(int field)
{
    switch (field) {
    case ColorAdjustPanel::eValueFieldR:
        return QString::fromUtf8("R");
    case ColorAdjustPanel::eValueFieldG:
        return QString::fromUtf8("G");
    case ColorAdjustPanel::eValueFieldB:
        return QString::fromUtf8("B");
    case ColorAdjustPanel::eValueFieldA:
        return QString::fromUtf8("A");
    case ColorAdjustPanel::eValueFieldH:
        return QString::fromUtf8("H");
    case ColorAdjustPanel::eValueFieldS:
        return QString::fromUtf8("S");
    case ColorAdjustPanel::eValueFieldV:
        return QString::fromUtf8("V");
    case ColorAdjustPanel::eValueFieldHex:
        return QObject::tr("Hex (display, clamped)");
    }

    return QString();
}

QString
fieldUndoLabel(int field)
{
    switch (field) {
    case ColorAdjustPanel::eValueFieldR:
        return QObject::tr("typed red");
    case ColorAdjustPanel::eValueFieldG:
        return QObject::tr("typed green");
    case ColorAdjustPanel::eValueFieldB:
        return QObject::tr("typed blue");
    case ColorAdjustPanel::eValueFieldA:
        return QObject::tr("typed alpha");
    case ColorAdjustPanel::eValueFieldH:
        return QObject::tr("typed HSV hue");
    case ColorAdjustPanel::eValueFieldS:
        return QObject::tr("typed HSV saturation");
    case ColorAdjustPanel::eValueFieldV:
        return QObject::tr("typed HSV value");
    case ColorAdjustPanel::eValueFieldHex:
        return QObject::tr("typed hex");
    }

    return QString();
}

QString
formatField(double v)
{
    return QString::number(v, 'g', 6);
}

double
wrapDegrees(double d)
{
    double w = std::fmod(d, 360.0);

    return w < 0.0 ? w + 360.0 : w;
}

bool
sameValue(const KnobColorValue& a,
          const KnobColorValue& b)
{
    return a.rgb[0] == b.rgb[0] && a.rgb[1] == b.rgb[1] && a.rgb[2] == b.rgb[2] && a.hasAlpha == b.hasAlpha && (!a.hasAlpha || (a.a == b.a));
}

// What the knob does to 18% grey, the same level the wheel's disc is drawn at, so the slider
// tracks and the wheel read as one picture for every role.
void
previewRgb(ColorKnobRoleEnum role,
           const KnobColorValue& v,
           float out[3])
{
    for (int i = 0; i < 3; ++i) {
        double c = kGrey;

        switch (role) {
        case eColorKnobRoleColour:
            c = v.rgb[i];
            break;
        case eColorKnobRoleOffset:
            c = kGrey + v.rgb[i];
            break;
        case eColorKnobRolePower: {
            PowerEffectResult e = powerToEffect(v.rgb[i]);
            c = e.valid ? e.effect : kGrey;
            break;
        }
        case eColorKnobRoleGain:
        case eColorKnobRoleAuto:
            c = kGrey * v.rgb[i];
            break;
        }
        out[i] = std::isfinite(c) ? (float)c : (float)kGrey;
    }
}

QColor
clampedColor(const float rgb[3])
{
    QColor c;

    c.setRgbF(std::min(std::max(rgb[0], 0.f), 1.f),
              std::min(std::max(rgb[1], 0.f), 1.f),
              std::min(std::max(rgb[2], 0.f), 1.f));

    return c;
}

ColorEdit
makeEdit(ColorAxisEnum axis,
         double d1,
         double d2,
         bool absolute)
{
    ColorEdit e;

    e.axis = axis;
    e.d1 = d1;
    e.d2 = d2;
    e.absolute = absolute;

    return e;
}
} // namespace

struct ColorAdjustPanelPrivate {
    enum SourceEnum {
        eSourceNone,
        eSourceWheel,
        eSourceSlider,
        eSourceOneShot
    };

    ColorAdjustPanel* q;

    KnobColorValue value;
    int nDims;
    ColorKnobRoleEnum role;
    LumaBasis basis;
    ColorAdjustPanel::DisplayConverter converter;
    ProjectColorManagementPtr colorManagement;
    TrackballRates rates;
    double rememberedHue;

    QTabWidget* tabs;
    QWidget* wheelTab;
    QWidget* valuesTab;
    ColorWheelWidget* wheel;
    ColorEditSlider* sliders[kSliderCount];
    QWidget* sliderRows[kSliderCount];
    LineEdit* fields[kFieldCount];
    Label* fieldLabels[kFieldCount];
    QWidget* paletteWidget;
    Button* paletteAddButton;
    Button* paletteClearButton;
    std::vector<ColorSelectorPaletteButton*> palette;

    SourceEnum source;
    bool started;
    KnobColorValue segmentBase;
    double segmentBaseHue;
    ColorEdit total;
    bool hasTotal;
    bool totalReplaces;
    QString undoLabel;

    explicit ColorAdjustPanelPrivate(ColorAdjustPanel* publicInterface)
        : q(publicInterface)
        , value(neutralGain())
        , nDims(4)
        , role(eColorKnobRoleGain)
        , basis(kRec709Weights)
        , converter()
        , colorManagement()
        , rates()
        , rememberedHue(0.0)
        , tabs(nullptr)
        , wheelTab(nullptr)
        , valuesTab(nullptr)
        , wheel(nullptr)
        , paletteWidget(nullptr)
        , paletteAddButton(nullptr)
        , paletteClearButton(nullptr)
        , palette()
        , source(eSourceNone)
        , started(false)
        , segmentBase(neutralGain())
        , segmentBaseHue(0.0)
        , total(makeEdit(eColorAxisTrackball, 0.0, 0.0, false))
        , hasTotal(false)
        , totalReplaces(false)
        , undoLabel()
    {
        for (int i = 0; i < kSliderCount; ++i) {
            sliders[i] = nullptr;
            sliderRows[i] = nullptr;
        }
        for (int f = 0; f < kFieldCount; ++f) {
            fields[f] = nullptr;
            fieldLabels[f] = nullptr;
        }
    }

    void buildWheelTab();
    void buildValuesTab();

    int sliderIndex(const QObject* s) const;
    int fieldIndex(const QObject* e) const;

    double valueAxisLuma(const KnobColorValue& v) const;
    double sliderValue(ColorSliderKindEnum kind) const;
    QGradientStops sliderGradient(ColorSliderKindEnum kind) const;
    void updateRememberedHue();
    void applySliderRates();
    void applyDimensions();

    void refreshWheel();
    void refreshSliders();
    void refreshFields(bool skipFocused);
    void refreshAll(bool skipFocusedFields);

    void beginGesture(SourceEnum src);
    void step(const ColorEdit& edit, bool replacesTotal, const QString& stepLabel);
    void endGesture();
    void oneShot(const KnobColorValue& newValue, const QString& stepLabel);
    void savePalette();
};

void
ColorAdjustPanelPrivate::buildWheelTab()
{
    wheelTab = new QWidget(tabs);

    QHBoxLayout* layout = new QHBoxLayout(wheelTab);

    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(8);

    wheel = new ColorWheelWidget(wheelTab);
    layout->addWidget(wheel, 0, Qt::AlignTop);

    QWidget* column = new QWidget(wheelTab);
    QVBoxLayout* columnLayout = new QVBoxLayout(column);

    columnLayout->setContentsMargins(0, 0, 0, 0);
    columnLayout->setSpacing(2);

    for (int i = 0; i < kSliderCount; ++i) {
        QWidget* row = new QWidget(column);
        QHBoxLayout* rowLayout = new QHBoxLayout(row);

        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(4);

        Label* title = new Label(sliderTitle(kSliderKinds[i]), row);

        title->setFixedWidth(36);
        rowLayout->addWidget(title);

        ColorEditSlider* slider = new ColorEditSlider(kSliderKinds[i], row);

        rowLayout->addWidget(slider, 1);
        columnLayout->addWidget(row);

        sliders[i] = slider;
        sliderRows[i] = row;
    }
    columnLayout->addStretch(1);
    layout->addWidget(column, 1);
}

void
ColorAdjustPanelPrivate::buildValuesTab()
{
    valuesTab = new QWidget(tabs);

    QVBoxLayout* layout = new QVBoxLayout(valuesTab);

    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    QGridLayout* grid = new QGridLayout();

    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(4);
    grid->setVerticalSpacing(2);

    for (int f = 0; f < kFieldCount; ++f) {
        fieldLabels[f] = new Label(fieldTitle(f), valuesTab);
        fields[f] = new LineEdit(valuesTab);
    }

    const int rgbaFields[4] = {
        ColorAdjustPanel::eValueFieldR, ColorAdjustPanel::eValueFieldG,
        ColorAdjustPanel::eValueFieldB, ColorAdjustPanel::eValueFieldA
    };
    const int hsvFields[3] = {
        ColorAdjustPanel::eValueFieldH, ColorAdjustPanel::eValueFieldS, ColorAdjustPanel::eValueFieldV
    };

    for (int i = 0; i < 4; ++i) {
        grid->addWidget(fieldLabels[rgbaFields[i]], 0, 2 * i);
        grid->addWidget(fields[rgbaFields[i]], 0, 2 * i + 1);
    }
    for (int i = 0; i < 3; ++i) {
        grid->addWidget(fieldLabels[hsvFields[i]], 1, 2 * i);
        grid->addWidget(fields[hsvFields[i]], 1, 2 * i + 1);
    }
    grid->addWidget(fieldLabels[ColorAdjustPanel::eValueFieldHex], 2, 0, 1, 4);
    grid->addWidget(fields[ColorAdjustPanel::eValueFieldHex], 2, 4, 1, 4);
    fields[ColorAdjustPanel::eValueFieldHex]->setToolTip(QObject::tr("The colour as #RRGGBB in the colour-picking (display) space. It is clamped to 0-1 and 8 bits, so it cannot hold HDR values: use the R, G and B fields for those."));
    layout->addLayout(grid);

    paletteWidget = new QWidget(valuesTab);

    QHBoxLayout* paletteLayout = new QHBoxLayout(paletteWidget);

    paletteLayout->setContentsMargins(0, 4, 0, 0);
    paletteLayout->setSpacing(4);

    QWidget* options = new QWidget(paletteWidget);
    QVBoxLayout* optionsLayout = new QVBoxLayout(options);

    optionsLayout->setContentsMargins(0, 0, 0, 0);
    optionsLayout->setSpacing(2);

    paletteAddButton = new Button(QObject::tr("Add"), options);
    paletteClearButton = new Button(QObject::tr("Clear"), options);
    paletteAddButton->setToolTip(QObject::tr("Add the current colour to the palette."));
    paletteClearButton->setToolTip(QObject::tr("Clear every colour in the palette."));
    optionsLayout->addWidget(paletteAddButton);
    optionsLayout->addStretch(1);
    optionsLayout->addWidget(paletteClearButton);
    paletteLayout->addWidget(options);

    QWidget* swatches = new QWidget(paletteWidget);
    QVBoxLayout* swatchesLayout = new QVBoxLayout(swatches);

    swatchesLayout->setContentsMargins(0, 0, 0, 0);
    swatchesLayout->setSpacing(0);

    for (int r = 0; r < COLOR_SELECTOR_PALETTE_ROWS; ++r) {
        QWidget* rowWidget = new QWidget(swatches);
        QHBoxLayout* rowLayout = new QHBoxLayout(rowWidget);

        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(0);
        for (int c = 0; c < COLOR_SELECTOR_PALETTE_COLS; ++c) {
            ColorSelectorPaletteButton* button = new ColorSelectorPaletteButton(rowWidget);

            rowLayout->addWidget(button);
            palette.push_back(button);
        }
        rowLayout->addStretch(1);
        swatchesLayout->addWidget(rowWidget);
    }
    paletteLayout->addWidget(swatches, 1);
    layout->addWidget(paletteWidget);
    layout->addStretch(1);
}

int
ColorAdjustPanelPrivate::sliderIndex(const QObject* s) const
{
    for (int i = 0; i < kSliderCount; ++i) {
        if (sliders[i] == s) {
            return i;
        }
    }

    return -1;
}

int
ColorAdjustPanelPrivate::fieldIndex(const QObject* e) const
{
    for (int f = 0; f < kFieldCount; ++f) {
        if (fields[f] == e) {
            return f;
        }
    }

    return -1;
}

// Power's value edits run on its 18% grey effect vector, so its stops are counted there rather
// than on the gamma numbers themselves.
double
ColorAdjustPanelPrivate::valueAxisLuma(const KnobColorValue& v) const
{
    if (role != eColorKnobRolePower) {
        return luma(basis, v.rgb);
    }

    double effect[3];

    for (int i = 0; i < 3; ++i) {
        PowerEffectResult e = powerToEffect(v.rgb[i]);

        if (!e.valid) {
            return 0.0;
        }
        effect[i] = e.effect;
    }

    return luma(basis, effect);
}

double
ColorAdjustPanelPrivate::sliderValue(ColorSliderKindEnum kind) const
{
    switch (kind) {
    case eColorSliderKindHue: {
        PlanePoint p = toPlane(role, basis, value);

        return (p.valid && planeSaturation(p) > kNeutralSaturation) ? planeHue(p) : rememberedHue;
    }
    case eColorSliderKindSat: {
        PlanePoint p = toPlane(role, basis, value);

        return p.valid ? planeSaturation(p) : 0.0;
    }
    case eColorSliderKindValue: {
        double Y = valueAxisLuma(value);

        if (!isRatioRole(role)) {
            return Y;
        }

        // A non-positive Y has no stop count; reading it as below the range shows the low
        // chevron instead of a marker that pretends to sit somewhere on the track.
        return Y > 0.0 ? std::log2(Y) : sliderMapping(kind, role, rates).rangeMin - 1.0;
    }
    case eColorSliderKindTemp: {
        TempTintResult tt = toTempTint(role, basis, value);

        return tt.valid ? tt.temp : 0.0;
    }
    case eColorSliderKindTint: {
        TempTintResult tt = toTempTint(role, basis, value);

        return tt.valid ? tt.tint : 0.0;
    }
    case eColorSliderKindAlpha:
        return value.hasAlpha ? value.a : 0.0;
    }

    return 0.0;
}

QGradientStops
ColorAdjustPanelPrivate::sliderGradient(ColorSliderKindEnum kind) const
{
    QGradientStops stops;

    if (kind == eColorSliderKindAlpha) {
        stops.append(QGradientStop(0.0, QColor(0, 0, 0)));
        stops.append(QGradientStop(1.0, QColor(255, 255, 255)));

        return stops;
    }

    const int count = (kind == eColorSliderKindHue) ? 13 : 9;
    SliderRange range = sliderMapping(kind, role, rates);
    TempTintResult current = toTempTint(role, basis, value);
    PlanePoint currentPlane = toPlane(role, basis, value);
    double currentSat = currentPlane.valid ? planeSaturation(currentPlane) : 0.0;
    std::vector<float> rgb(3 * count);

    for (int k = 0; k < count; ++k) {
        double t = (double)k / (count - 1);
        ChromaEditResult r;

        r.value = value;
        r.valid = false;
        switch (kind) {
        case eColorSliderKindHue: {
            double hue = 360.0 * t;
            double s = std::max(currentSat, rates.wheelRim * kHueTrackSaturationFraction);

            r = applyEdit(role, basis, value, makeEdit(eColorAxisSat, s, 0.0, true), hue);
            if (r.valid) {
                r = applyEdit(role, basis, r.value, makeEdit(eColorAxisHue, hue, 0.0, true), hue);
            }
            break;
        }
        case eColorSliderKindSat:
            r = applyEdit(role, basis, value, makeEdit(eColorAxisSat, markerToValue(range, t), 0.0, true), rememberedHue);
            break;
        case eColorSliderKindValue:
            r = applyEdit(role, basis, value, makeEdit(eColorAxisValue, markerToValue(range, t), 0.0, true), rememberedHue);
            break;
        case eColorSliderKindTemp: {
            double d = markerToValue(range, t) - (current.valid ? current.temp : 0.0);

            r = applyEdit(role, basis, value, makeEdit(eColorAxisTempTint, d, 0.0, false), rememberedHue);
            break;
        }
        case eColorSliderKindTint: {
            double d = markerToValue(range, t) - (current.valid ? current.tint : 0.0);

            r = applyEdit(role, basis, value, makeEdit(eColorAxisTempTint, 0.0, d, false), rememberedHue);
            break;
        }
        case eColorSliderKindAlpha:
            break;
        }
        previewRgb(role, r.valid ? r.value : value, &rgb[3 * k]);
    }

    if (converter) {
        converter(rgb.data(), count);
    }
    for (int k = 0; k < count; ++k) {
        stops.append(QGradientStop((double)k / (count - 1), clampedColor(&rgb[3 * k])));
    }

    return stops;
}

void
ColorAdjustPanelPrivate::updateRememberedHue()
{
    PlanePoint p = toPlane(role, basis, value);

    if (p.valid && planeSaturation(p) > kNeutralSaturation) {
        rememberedHue = planeHue(p);
    }
}

void
ColorAdjustPanelPrivate::applySliderRates()
{
    for (int i = 0; i < kSliderCount; ++i) {
        double perPixel = 0.0;

        switch (kSliderKinds[i]) {
        case eColorSliderKindHue:
            perPixel = rates.hueDegreesPerPixel;
            break;
        case eColorSliderKindSat:
            perPixel = rates.satPlanePerPixel;
            break;
        case eColorSliderKindValue:
            perPixel = valueRatePerPixel(rates, role);
            break;
        case eColorSliderKindTemp:
            perPixel = tempRatePerPixel(rates, role);
            break;
        case eColorSliderKindTint:
            perPixel = tintRatePerPixel(rates, role);
            break;
        case eColorSliderKindAlpha:
            break;
        }
        sliders[i]->setRates(rates);
        sliders[i]->setRole(role);
        sliders[i]->setRelativeRatePerPixel(perPixel);
    }
}

void
ColorAdjustPanelPrivate::applyDimensions()
{
    bool single = nDims == 1;
    bool alpha = nDims >= 4;

    wheel->setVisible(!single);
    for (int i = 0; i < kSliderCount; ++i) {
        bool visible;

        switch (kSliderKinds[i]) {
        case eColorSliderKindValue:
            visible = true;
            break;
        case eColorSliderKindAlpha:
            visible = alpha;
            break;
        default:
            visible = !single;
            break;
        }
        sliderRows[i]->setVisible(visible);
    }

    for (int f = 0; f < kFieldCount; ++f) {
        bool visible;

        switch (f) {
        case ColorAdjustPanel::eValueFieldR:
            visible = true;
            break;
        case ColorAdjustPanel::eValueFieldA:
            visible = alpha;
            break;
        default:
            visible = !single;
            break;
        }
        fields[f]->setVisible(visible);
        fieldLabels[f]->setVisible(visible);
    }
    fieldLabels[ColorAdjustPanel::eValueFieldR]->setText(single ? QObject::tr("Value") : fieldTitle(ColorAdjustPanel::eValueFieldR));
    paletteWidget->setVisible(!single);
}

void
ColorAdjustPanelPrivate::refreshWheel()
{
    if (source != eSourceWheel) {
        wheel->setValue(value);
    }
}

void
ColorAdjustPanelPrivate::refreshSliders()
{
    for (int i = 0; i < kSliderCount; ++i) {
        sliders[i]->setValue(sliderValue(kSliderKinds[i]));
        sliders[i]->setGradientStops(sliderGradient(kSliderKinds[i]));
    }
}

void
ColorAdjustPanelPrivate::refreshFields(bool skipFocused)
{
    double hsv[3];

    ColorAdjustPanel::rgbToHsv(value.rgb, hsv);

    const double numbers[kNumericFieldCount] = {
        value.rgb[0], value.rgb[1], value.rgb[2], value.hasAlpha ? value.a : 1.0, hsv[0], hsv[1], hsv[2]
    };

    for (int f = 0; f < kNumericFieldCount; ++f) {
        if (skipFocused && fields[f]->hasFocus()) {
            continue;
        }
        fields[f]->setText(formatField(numbers[f]));
    }

    LineEdit* hex = fields[ColorAdjustPanel::eValueFieldHex];

    if (!(skipFocused && hex->hasFocus())) {
        float display[3] = { (float)value.rgb[0], (float)value.rgb[1], (float)value.rgb[2] };

        if (colorManagement) {
            colorManagement->workingToColorPicking(&display[0], &display[1], &display[2]);
        }
        hex->setText(clampedColor(display).name());
    }
}

void
ColorAdjustPanelPrivate::refreshAll(bool skipFocusedFields)
{
    refreshWheel();
    refreshSliders();
    refreshFields(skipFocusedFields);
}

void
ColorAdjustPanelPrivate::beginGesture(SourceEnum src)
{
    if (source != eSourceNone) {
        endGesture();
    }
    source = src;
    started = false;
    segmentBase = value;
    segmentBaseHue = rememberedHue;
    hasTotal = false;
    totalReplaces = false;
    undoLabel.clear();
}

void
ColorAdjustPanelPrivate::step(const ColorEdit& edit,
                              bool replacesTotal,
                              const QString& stepLabel)
{
    if (source == eSourceNone) {
        return;
    }
    if (!started) {
        started = true;
        Q_EMIT q->valuesEdited(value, ColorAdjustPanel::eGesturePhaseStart, stepLabel);
    }

    bool replaces = replacesTotal || edit.absolute;

    // A held lock pressed mid-drag switches the axis: the steps so far are folded into a new
    // base so the new axis accumulates from where the old one left the value.
    if (hasTotal && ((edit.axis != total.axis) || (replaces != totalReplaces))) {
        segmentBase = value;
        segmentBaseHue = rememberedHue;
        hasTotal = false;
    }
    if (replaces || !hasTotal) {
        total = edit;
    } else {
        total.d1 += edit.d1;
        total.d2 += edit.d2;
    }
    hasTotal = true;
    totalReplaces = replaces;

    ChromaEditResult r = applyEdit(role, basis, segmentBase, total, segmentBaseHue);

    if (r.valid) {
        value = r.value;
    }

    PlanePoint p = toPlane(role, basis, value);

    if (p.valid && planeSaturation(p) > kNeutralSaturation) {
        rememberedHue = planeHue(p);
    } else if (total.axis == eColorAxisHue) {
        rememberedHue = wrapDegrees(total.absolute ? total.d1 : segmentBaseHue + total.d1);
    }

    undoLabel = stepLabel;
    refreshWheel();
    refreshSliders();
    refreshFields(false);
    Q_EMIT q->valuesEdited(value, ColorAdjustPanel::eGesturePhaseMove, undoLabel);
}

void
ColorAdjustPanelPrivate::endGesture()
{
    if (source == eSourceNone) {
        return;
    }

    bool wasStarted = started;

    source = eSourceNone;
    started = false;
    hasTotal = false;
    totalReplaces = false;
    refreshAll(false);
    if (wasStarted) {
        Q_EMIT q->valuesEdited(value, ColorAdjustPanel::eGesturePhaseEnd, undoLabel);
    }
}

void
ColorAdjustPanelPrivate::oneShot(const KnobColorValue& newValue,
                                 const QString& stepLabel)
{
    if (sameValue(newValue, value)) {
        refreshFields(false);

        return;
    }
    beginGesture(eSourceOneShot);
    started = true;
    Q_EMIT q->valuesEdited(value, ColorAdjustPanel::eGesturePhaseStart, stepLabel);
    value = newValue;
    updateRememberedHue();
    undoLabel = stepLabel;
    Q_EMIT q->valuesEdited(value, ColorAdjustPanel::eGesturePhaseMove, undoLabel);
    endGesture();
}

void
ColorAdjustPanelPrivate::savePalette()
{
    QSettings settings(QString::fromUtf8(NATRON_ORGANIZATION_NAME), QString::fromUtf8(NATRON_APPLICATION_NAME));

    settings.beginGroup(QString::fromUtf8(COLOR_SELECTOR_PALETTE_SETTINGS));
    for (std::size_t i = 0; i < palette.size(); ++i) {
        float r, g, b, a;

        palette[i]->getColor(&r, &g, &b, &a);

        QStringList values;

        values << QString::number(r) << QString::number(g) << QString::number(b) << QString::number(a);
        settings.setValue(QString::number((int)i), values);
    }
    settings.endGroup();
}

ColorAdjustPanel::ColorAdjustPanel(QWidget* parent)
    : QWidget(parent)
    , _imp(new ColorAdjustPanelPrivate(this))
{
    QVBoxLayout* layout = new QVBoxLayout(this);

    layout->setContentsMargins(0, 0, 0, 0);

    _imp->tabs = new QTabWidget(this);
    _imp->buildWheelTab();
    _imp->buildValuesTab();
    _imp->tabs->addTab(_imp->wheelTab, tr("Wheel"));
    _imp->tabs->addTab(_imp->valuesTab, tr("Values"));
    layout->addWidget(_imp->tabs);

    QObject::connect(_imp->wheel, &ColorWheelWidget::gestureStarted, this, &ColorAdjustPanel::onWheelGestureStarted);
    QObject::connect(_imp->wheel, &ColorWheelWidget::gestureMoved, this, &ColorAdjustPanel::onWheelGestureMoved);
    QObject::connect(_imp->wheel, &ColorWheelWidget::gestureFinished, this, &ColorAdjustPanel::onWheelGestureFinished);
    for (int i = 0; i < kSliderCount; ++i) {
        ColorEditSlider* s = _imp->sliders[i];

        QObject::connect(s, &ColorEditSlider::gestureStarted, this, &ColorAdjustPanel::onSliderGestureStarted);
        QObject::connect(s, &ColorEditSlider::gestureMoved, this, &ColorAdjustPanel::onSliderGestureMoved);
        QObject::connect(s, &ColorEditSlider::gestureFinished, this, &ColorAdjustPanel::onSliderGestureFinished);
        QObject::connect(s, &ColorEditSlider::resetRequested, this, &ColorAdjustPanel::onSliderResetRequested);
    }
    for (int f = 0; f < kFieldCount; ++f) {
        if (f == eValueFieldHex) {
            QObject::connect(_imp->fields[f], &LineEdit::returnPressed, this, &ColorAdjustPanel::onHexCommitted);
        } else {
            QObject::connect(_imp->fields[f], &LineEdit::returnPressed, this, &ColorAdjustPanel::onValueFieldCommitted);
        }
    }
    for (std::size_t i = 0; i < _imp->palette.size(); ++i) {
        QObject::connect(_imp->palette[i], &ColorSelectorPaletteButton::colorPicked, this, &ColorAdjustPanel::onPaletteColorPicked);
    }
    QObject::connect(_imp->paletteAddButton, &Button::clicked, this, &ColorAdjustPanel::addCurrentColorToPalette);
    QObject::connect(_imp->paletteClearButton, &Button::clicked, this, &ColorAdjustPanel::clearPalette);
    QObject::connect(_imp->tabs, &QTabWidget::currentChanged, this, &ColorAdjustPanel::onTabChanged);

    _imp->wheel->setBasis(_imp->basis);
    _imp->wheel->setRole(_imp->role);
    _imp->applySliderRates();
    _imp->applyDimensions();
    _imp->updateRememberedHue();
    _imp->refreshAll(false);
    reloadPalette();
}

ColorAdjustPanel::~ColorAdjustPanel()
{
}

void
ColorAdjustPanel::setKnobValue(const KnobColorValue& value,
                               int nDims,
                               ColorKnobRoleEnum role,
                               const LumaBasis& basis)
{
    if (isEditing()) {
        return;
    }
    _imp->basis = basis;
    _imp->wheel->setBasis(basis);
    _imp->nDims = nDims;
    _imp->applyDimensions();
    _imp->role = role;
    _imp->wheel->setRole(role);
    _imp->applySliderRates();
    setKnobValue(value);
}

void
ColorAdjustPanel::setKnobValue(const KnobColorValue& value)
{
    if (isEditing()) {
        return;
    }
    _imp->value = value;
    _imp->updateRememberedHue();
    _imp->refreshAll(true);
}

void
ColorAdjustPanel::setDimensions(int nDims)
{
    _imp->nDims = nDims;
    _imp->applyDimensions();
}

void
ColorAdjustPanel::setRole(ColorKnobRoleEnum role)
{
    if (role == _imp->role) {
        return;
    }
    _imp->role = role;
    _imp->wheel->setRole(role);
    _imp->applySliderRates();
    if (!isEditing()) {
        _imp->updateRememberedHue();
        _imp->refreshAll(true);
    }
}

void
ColorAdjustPanel::setBasis(const LumaBasis& basis)
{
    _imp->basis = basis;
    _imp->wheel->setBasis(basis);
    if (!isEditing()) {
        _imp->updateRememberedHue();
        _imp->refreshAll(true);
    }
}

void
ColorAdjustPanel::setDisplayConverter(const DisplayConverter& converter)
{
    _imp->converter = converter;
    _imp->wheel->setDisplayConverter(converter);
    _imp->refreshSliders();
}

void
ColorAdjustPanel::setColorManagement(const ProjectColorManagementPtr& colorManagement)
{
    _imp->colorManagement = colorManagement;
    for (std::size_t i = 0; i < _imp->palette.size(); ++i) {
        _imp->palette[i]->setColorManagement(colorManagement);
    }
    _imp->refreshFields(true);
}

const KnobColorValue&
ColorAdjustPanel::getValue() const
{
    return _imp->value;
}

int
ColorAdjustPanel::getDimensions() const
{
    return _imp->nDims;
}

ColorKnobRoleEnum
ColorAdjustPanel::getRole() const
{
    return _imp->role;
}

const LumaBasis&
ColorAdjustPanel::getBasis() const
{
    return _imp->basis;
}

double
ColorAdjustPanel::getRememberedHue() const
{
    return _imp->rememberedHue;
}

bool
ColorAdjustPanel::isEditing() const
{
    return _imp->source != ColorAdjustPanelPrivate::eSourceNone;
}

QTabWidget*
ColorAdjustPanel::getTabWidget() const
{
    return _imp->tabs;
}

QWidget*
ColorAdjustPanel::getWheelTab() const
{
    return _imp->wheelTab;
}

QWidget*
ColorAdjustPanel::getValuesTab() const
{
    return _imp->valuesTab;
}

ColorWheelWidget*
ColorAdjustPanel::getWheel() const
{
    return _imp->wheel;
}

ColorEditSlider*
ColorAdjustPanel::getSlider(ColorSliderKindEnum kind) const
{
    for (int i = 0; i < kSliderCount; ++i) {
        if (kSliderKinds[i] == kind) {
            return _imp->sliders[i];
        }
    }

    return nullptr;
}

LineEdit*
ColorAdjustPanel::getValueField(ValueFieldEnum field) const
{
    int f = (int)field;

    return (f >= 0 && f < kFieldCount) ? _imp->fields[f] : nullptr;
}

int
ColorAdjustPanel::getPaletteButtonCount() const
{
    return (int)_imp->palette.size();
}

ColorSelectorPaletteButton*
ColorAdjustPanel::getPaletteButton(int index) const
{
    return (index >= 0 && index < (int)_imp->palette.size()) ? _imp->palette[index] : nullptr;
}

void
ColorAdjustPanel::rgbToHsv(const double rgb[3],
                           double hsv[3])
{
    double mx = std::max(rgb[0], std::max(rgb[1], rgb[2]));
    double mn = std::min(rgb[0], std::min(rgb[1], rgb[2]));
    double d = mx - mn;
    double h = 0.0;

    if (d > 0.0) {
        if (mx == rgb[0]) {
            h = (rgb[1] - rgb[2]) / d;
        } else if (mx == rgb[1]) {
            h = 2.0 + (rgb[2] - rgb[0]) / d;
        } else {
            h = 4.0 + (rgb[0] - rgb[1]) / d;
        }
        h = wrapDegrees(60.0 * h);
    }
    hsv[0] = h;
    hsv[1] = (mx != 0.0) ? d / mx : 0.0;
    hsv[2] = mx;
}

void
ColorAdjustPanel::hsvToRgb(const double hsv[3],
                           double rgb[3])
{
    double h = wrapDegrees(hsv[0]) / 60.0;
    double c = hsv[2] * hsv[1];
    double m = hsv[2] - c;
    double x = c * (1.0 - std::fabs(std::fmod(h, 2.0) - 1.0));
    int sector = std::min(5, std::max(0, (int)std::floor(h)));
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;

    switch (sector) {
    case 0:
        r = c;
        g = x;
        break;
    case 1:
        r = x;
        g = c;
        break;
    case 2:
        g = c;
        b = x;
        break;
    case 3:
        g = x;
        b = c;
        break;
    case 4:
        r = x;
        b = c;
        break;
    default:
        r = c;
        b = x;
        break;
    }
    rgb[0] = r + m;
    rgb[1] = g + m;
    rgb[2] = b + m;
}

void
ColorAdjustPanel::addCurrentColorToPalette()
{
    std::vector<ColorSelectorPaletteButton*>& palette = _imp->palette;

    if (palette.empty()) {
        return;
    }

    // With every swatch in use the oldest one is overwritten, so adding never silently fails.
    std::size_t target = 0;
    bool full = true;

    for (std::size_t i = 0; i < palette.size(); ++i) {
        if (!palette[i]->isModified()) {
            target = i;
            full = false;
            break;
        }
    }
    if (full) {
        for (std::size_t i = 0; i < palette.size(); ++i) {
            palette[i]->clearModified();
        }
        target = 0;
    }

    const KnobColorValue& v = _imp->value;

    palette[target]->setColor((float)v.rgb[0], (float)v.rgb[1], (float)v.rgb[2], v.hasAlpha ? (float)v.a : 1.f);
    _imp->savePalette();
}

void
ColorAdjustPanel::clearPalette()
{
    for (std::size_t i = 0; i < _imp->palette.size(); ++i) {
        _imp->palette[i]->clearColor();
    }
    _imp->savePalette();
}

void
ColorAdjustPanel::reloadPalette()
{
    QSettings settings(QString::fromUtf8(NATRON_ORGANIZATION_NAME), QString::fromUtf8(NATRON_APPLICATION_NAME));

    settings.beginGroup(QString::fromUtf8(COLOR_SELECTOR_PALETTE_SETTINGS));
    for (std::size_t i = 0; i < _imp->palette.size(); ++i) {
        QVariant stored = settings.value(QString::number((int)i));

        if (!stored.isValid()) {
            continue;
        }

        QStringList values = stored.toStringList();

        if (values.size() > 3) {
            _imp->palette[i]->setColor(values.at(0).toFloat(), values.at(1).toFloat(), values.at(2).toFloat(), values.at(3).toFloat());
        }
    }
    settings.endGroup();
}

void
ColorAdjustPanel::onWheelGestureStarted()
{
    _imp->beginGesture(ColorAdjustPanelPrivate::eSourceWheel);
}

void
ColorAdjustPanel::onWheelGestureMoved(ColorEdit edit,
                                      QString axisLabel)
{
    if (_imp->source != ColorAdjustPanelPrivate::eSourceWheel) {
        return;
    }
    _imp->step(edit, false, axisLabel);
}

void
ColorAdjustPanel::onWheelGestureFinished(bool /*moved*/)
{
    if (_imp->source != ColorAdjustPanelPrivate::eSourceWheel) {
        return;
    }
    _imp->endGesture();
}

void
ColorAdjustPanel::onSliderGestureStarted()
{
    _imp->beginGesture(ColorAdjustPanelPrivate::eSourceSlider);
}

void
ColorAdjustPanel::onSliderGestureMoved(double value,
                                       bool absolute,
                                       QString axisLabel)
{
    int index = _imp->sliderIndex(sender());

    if ((index < 0) || (_imp->source != ColorAdjustPanelPrivate::eSourceSlider)) {
        return;
    }

    ColorSliderKindEnum kind = kSliderKinds[index];
    ColorEdit edit = makeEdit(eColorAxisValue, value, 0.0, absolute);
    bool replaces = false;

    switch (kind) {
    case eColorSliderKindHue:
        edit.axis = eColorAxisHue;
        break;
    case eColorSliderKindSat:
        edit.axis = eColorAxisSat;
        break;
    case eColorSliderKindValue:
        edit.axis = eColorAxisValue;
        break;
    case eColorSliderKindAlpha:
        edit.axis = eColorAxisAlpha;
        break;
    case eColorSliderKindTemp:
    case eColorSliderKindTint: {
        bool isTemp = kind == eColorSliderKindTemp;
        double d = value;

        // An absolute target becomes a total nudge from the gesture's base: it reaches the
        // target exactly wherever the locus is defined, keeps the other axis where it was, and
        // still moves along the linear axes where an absolute fromTempTint would refuse.
        if (absolute) {
            TempTintResult base = toTempTint(_imp->role, _imp->basis, _imp->segmentBase);
            double from = base.valid ? (isTemp ? base.temp : base.tint) : 0.0;

            d = value - from;
            replaces = true;
        }
        edit = makeEdit(eColorAxisTempTint, isTemp ? d : 0.0, isTemp ? 0.0 : d, false);
        break;
    }
    }

    _imp->step(edit, replaces, axisLabel);
}

void
ColorAdjustPanel::onSliderGestureFinished(bool /*moved*/)
{
    if (_imp->source != ColorAdjustPanelPrivate::eSourceSlider) {
        return;
    }
    _imp->endGesture();
}

void
ColorAdjustPanel::onSliderResetRequested()
{
    int index = _imp->sliderIndex(sender());

    if ((index < 0) || isEditing()) {
        return;
    }

    ColorSliderKindEnum kind = kSliderKinds[index];
    const KnobColorValue& v = _imp->value;
    ColorKnobRoleEnum role = _imp->role;
    ColorEdit edit = makeEdit(eColorAxisSat, 0.0, 0.0, true);

    switch (kind) {
    case eColorSliderKindHue:
        return;
    case eColorSliderKindSat:
        break;
    case eColorSliderKindValue: {
        std::optional<double> target = resetValue(role);

        if (!target) {
            return;
        }
        edit = makeEdit(eColorAxisValue, isRatioRole(role) ? std::log2(*target) : *target, 0.0, true);
        break;
    }
    case eColorSliderKindTemp:
    case eColorSliderKindTint: {
        TempTintResult tt = toTempTint(role, _imp->basis, v);

        if (!tt.valid) {
            return;
        }
        edit = makeEdit(eColorAxisTempTint,
                        (kind == eColorSliderKindTemp) ? -tt.temp : 0.0,
                        (kind == eColorSliderKindTint) ? -tt.tint : 0.0,
                        false);
        break;
    }
    case eColorSliderKindAlpha:
        if (!v.hasAlpha) {
            return;
        }
        edit = makeEdit(eColorAxisAlpha, (role == eColorKnobRoleOffset) ? 0.0 : 1.0, 0.0, true);
        break;
    }

    ChromaEditResult r = applyEdit(role, _imp->basis, v, edit, _imp->rememberedHue);

    if (r.valid) {
        _imp->oneShot(r.value, tr("reset %1").arg(sliderTitle(kind).toLower()));
    }
}

void
ColorAdjustPanel::onValueFieldCommitted()
{
    int f = _imp->fieldIndex(sender());

    if ((f < 0) || (f == eValueFieldHex) || isEditing()) {
        return;
    }

    bool ok = false;
    double typed = _imp->fields[f]->text().toDouble(&ok);

    if (!ok || !std::isfinite(typed)) {
        _imp->refreshFields(false);

        return;
    }

    const KnobColorValue& v = _imp->value;
    KnobColorValue out = v;
    QString label = fieldUndoLabel(f);

    switch (f) {
    case eValueFieldR:
    case eValueFieldG:
    case eValueFieldB:
        if (_imp->nDims == 1) {
            out.rgb[0] = out.rgb[1] = out.rgb[2] = typed;
            label = tr("typed value");
        } else {
            out.rgb[f - eValueFieldR] = typed;
        }
        break;
    case eValueFieldA:
        if (!v.hasAlpha) {
            _imp->refreshFields(false);

            return;
        }
        out.a = typed;
        break;
    case eValueFieldH:
    case eValueFieldS: {
        double hsv[3];

        rgbToHsv(v.rgb, hsv);
        hsv[(f == eValueFieldH) ? 0 : 1] = typed;
        hsvToRgb(hsv, out.rgb);
        break;
    }
    case eValueFieldV: {
        double hsv[3];

        rgbToHsv(v.rgb, hsv);

        double v0 = hsv[2];
        double y0 = luma(_imp->basis, v.rgb);

        if (v0 != 0.0) {
            for (int i = 0; i < 3; ++i) {
                out.rgb[i] = v.rgb[i] * (typed / v0);
            }
        } else {
            out.rgb[0] = out.rgb[1] = out.rgb[2] = typed;
        }

        // A V-only commit is a value edit, so alpha tracks luma the way a Value slider drag
        // makes it: scaled with a ratio role's rgb, shifted by the luma change otherwise.
        if (out.hasAlpha) {
            if ((v0 != 0.0) && isRatioRole(_imp->role)) {
                out.a = v.a * (typed / v0);
            } else {
                out.a = v.a + (luma(_imp->basis, out.rgb) - y0);
            }
        }
        break;
    }
    default:
        return;
    }

    _imp->oneShot(out, label);
}

void
ColorAdjustPanel::onHexCommitted()
{
    if (isEditing()) {
        return;
    }

    QString text = _imp->fields[eValueFieldHex]->text().trimmed();

    if (!text.startsWith(QLatin1Char('#'))) {
        text.prepend(QLatin1Char('#'));
    }

    QColor color = QColor::fromString(text);

    if (!color.isValid()) {
        _imp->refreshFields(false);

        return;
    }

    float rgb[3] = { (float)color.redF(), (float)color.greenF(), (float)color.blueF() };

    if (_imp->colorManagement) {
        _imp->colorManagement->colorPickingToWorking(&rgb[0], &rgb[1], &rgb[2]);
    }

    KnobColorValue out = _imp->value;

    for (int i = 0; i < 3; ++i) {
        out.rgb[i] = rgb[i];
    }
    _imp->oneShot(out, fieldUndoLabel(eValueFieldHex));
}

void
ColorAdjustPanel::onPaletteColorPicked(float r,
                                       float g,
                                       float b,
                                       float a)
{
    if (isEditing()) {
        return;
    }

    KnobColorValue out = _imp->value;

    out.rgb[0] = r;
    out.rgb[1] = g;
    out.rgb[2] = b;
    if (out.hasAlpha) {
        out.a = a;
    }
    _imp->oneShot(out, tr("palette colour"));
}

void
ColorAdjustPanel::onTabChanged(int index)
{
    if (_imp->tabs->widget(index) == _imp->valuesTab) {
        reloadPalette();
        _imp->refreshFields(true);
    }
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_ColorAdjustPanel.cpp"
