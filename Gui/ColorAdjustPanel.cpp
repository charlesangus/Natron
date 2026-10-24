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
#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QResizeEvent>
#include <QVBoxLayout>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Gui/ColorEditSlider.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/Label.h"
#include "Gui/SpinBox.h"

NATRON_NAMESPACE_ENTER

namespace {
const double kGrey = kColorPivotGrey;
const int kLineCount = 5;

// Below this distance from white a value has no usable hue of its own, so the remembered hue
// is kept rather than overwritten by the noise angle of a near-neutral point.
const double kNeutralSaturation = 1e-9;

// The Hue track is painted at this fraction of the rim when the value is (near) neutral, so the
// spectrum stays readable instead of collapsing to grey.
const double kHueTrackSaturationFraction = 0.5;

// The shortest slider track the wheel leaves room for before it stops growing.
const double kMinTrackLength = 120.0;

const ColorSliderKindEnum kLineKinds[kLineCount] = {
    eColorSliderKindHue,
    eColorSliderKindSat,
    eColorSliderKindValue,
    eColorSliderKindTemp,
    eColorSliderKindTint
};

QString
lineTitle(ColorSliderKindEnum kind)
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
    }

    return QString();
}

// This text becomes part of an undo command's label verbatim ("value slider", "typed hue"),
// so it reads as a word inside a phrase rather than as a caption.
QString
lineUndoName(ColorSliderKindEnum kind)
{
    switch (kind) {
    case eColorSliderKindHue:
        return QObject::tr("hue");
    case eColorSliderKindSat:
        return QObject::tr("saturation");
    case eColorSliderKindValue:
        return QObject::tr("value");
    case eColorSliderKindTemp:
        return QObject::tr("temperature");
    case eColorSliderKindTint:
        return QObject::tr("tint");
    }

    return QString();
}

// Enough digits that one step of the last one is finer than a one-pixel slider move over the
// line's range, without padding the field with noise for the wide physical ranges.
int
lineDecimals(ColorSliderKindEnum kind,
             ColorKnobRoleEnum role)
{
    bool ratio = isRatioRole(role);

    switch (kind) {
    case eColorSliderKindHue:
        return 2;
    case eColorSliderKindSat:
        return 5;
    case eColorSliderKindValue:
        return ratio ? 3 : 4;
    case eColorSliderKindTemp:
    case eColorSliderKindTint:
        return ratio ? 2 : 4;
    }

    return 3;
}

// One arrow-key or scroll step of a line's spinbox: a degree of hue, and a hundredth of the
// track for the rest, which keeps every line's step a small fraction of its own range.
double
lineIncrement(ColorSliderKindEnum kind,
              const SliderRange& range)
{
    if (kind == eColorSliderKindHue) {
        return 1.0;
    }

    return (range.rangeMax - range.rangeMin) / 100.0;
}

double
roundToDecimals(double v,
                int decimals)
{
    double scale = std::pow(10.0, decimals);
    double r = std::round(v * scale) / scale;

    return std::isfinite(r) ? r : v;
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

    int captionWidth;

    KnobColorValue value;
    int nDims;
    ColorKnobRoleEnum role;
    LumaBasis basis;
    ColorAdjustPanel::DisplayConverter converter;
    TrackballRates rates;
    double rememberedHue;

    QWidget* editArea;
    QGridLayout* linesLayout;
    ColorWheelWidget* wheel;
    Label* captions[kLineCount];
    SpinBox* spinBoxes[kLineCount];
    ColorEditSlider* customSliders[kLineCount];

    SourceEnum source;
    int activeSlider;
    bool started;
    KnobColorValue segmentBase;
    double segmentBaseHue;
    ColorEdit total;
    bool hasTotal;
    bool totalReplaces;
    QString undoLabel;

    explicit ColorAdjustPanelPrivate(ColorAdjustPanel* publicInterface)
        : q(publicInterface)
        , captionWidth(0)
        , value(neutralGainValue())
        , nDims(4)
        , role(eColorKnobRoleGain)
        , basis(kRec709LumaWeights)
        , converter()
        , rates()
        , rememberedHue(0.0)
        , editArea(nullptr)
        , linesLayout(nullptr)
        , wheel(nullptr)
        , source(eSourceNone)
        , activeSlider(-1)
        , started(false)
        , segmentBase(neutralGainValue())
        , segmentBaseHue(0.0)
        , total(makeEdit(eColorAxisTrackball, 0.0, 0.0, false))
        , hasTotal(false)
        , totalReplaces(false)
        , undoLabel()
    {
        for (int i = 0; i < kLineCount; ++i) {
            captions[i] = nullptr;
            spinBoxes[i] = nullptr;
            customSliders[i] = nullptr;
        }
    }

    bool lineVisible(int i) const
    {
        return (kLineKinds[i] == eColorSliderKindValue) || (nDims != 1);
    }

    void buildEditArea();
    void connectEditArea();
    void updateWheelSize();

    int sliderIndex(const QObject* s) const;
    int spinBoxIndex(const QObject* s) const;

    double valueAxisLuma(const KnobColorValue& v) const;
    double lineValue(ColorSliderKindEnum kind) const;
    QGradientStops lineGradient(ColorSliderKindEnum kind) const;
    ColorEdit lineEdit(ColorSliderKindEnum kind, double target, const KnobColorValue& from, bool* replaces) const;
    ColorEdit lineNudge(ColorSliderKindEnum kind, double delta) const;
    void updateRememberedHue();
    void applyRanges();
    void applyDimensions();

    void refreshWheel();
    void refreshGradients();
    void refreshLines(bool skipFocused);
    void refreshAll(bool skipFocused);

    void beginGesture(SourceEnum src);
    void step(const ColorEdit& edit, bool replacesTotal, const QString& stepLabel);
    void endGesture();
    void oneShot(const KnobColorValue& newValue, const QString& stepLabel);
};

void
ColorAdjustPanelPrivate::buildEditArea()
{
    editArea = new QWidget(q);
    for (int i = 0; i < kLineCount; ++i) {
        captions[i] = nullptr;
        spinBoxes[i] = nullptr;
        customSliders[i] = nullptr;
    }

    // Sized from the widest caption, so none of them clip at the app's actual font and DPI.
    QFontMetrics captionMetrics(QApplication::font());

    captionWidth = 0;
    for (int i = 0; i < kLineCount; ++i) {
        captionWidth = std::max(captionWidth, captionMetrics.horizontalAdvance(lineTitle(kLineKinds[i])));
    }
    captionWidth += (int)TO_DPIX(4.);

    QHBoxLayout* areaLayout = new QHBoxLayout(editArea);

    areaLayout->setContentsMargins(0, (int)TO_DPIY(2.), 0, (int)TO_DPIY(2.));
    areaLayout->setSpacing((int)TO_DPIX(8.));

    wheel = new ColorWheelWidget(editArea);
    areaLayout->addWidget(wheel, 0, Qt::AlignTop);

    linesLayout = new QGridLayout();
    linesLayout->setContentsMargins(0, 0, 0, 0);
    linesLayout->setHorizontalSpacing((int)TO_DPIX(4.));
    linesLayout->setVerticalSpacing((int)TO_DPIY(1.));
    linesLayout->setColumnStretch(2, 1);

    for (int i = 0; i < kLineCount; ++i) {
        ColorSliderKindEnum kind = kLineKinds[i];
        QString objectSuffix = lineTitle(kind);

        Label* caption = new Label(lineTitle(kind), editArea);

        caption->setFixedWidth(captionWidth);
        linesLayout->addWidget(caption, i, 0);

        SpinBox* box = new SpinBox(editArea, SpinBox::eSpinBoxTypeDouble);

        box->setObjectName(QString::fromUtf8("ColorAdjustSpinBox") + objectSuffix);
        linesLayout->addWidget(box, i, 1);

        ColorEditSlider* slider = new ColorEditSlider(kind, editArea);

        slider->setObjectName(QString::fromUtf8("ColorAdjustCustomSlider") + objectSuffix);
        linesLayout->addWidget(slider, i, 2);
        customSliders[i] = slider;

        captions[i] = caption;
        spinBoxes[i] = box;
    }
    linesLayout->setRowStretch(kLineCount, 1);
    areaLayout->addLayout(linesLayout, 1);
}

void
ColorAdjustPanelPrivate::connectEditArea()
{
    QObject::connect(wheel, &ColorWheelWidget::gestureStarted, q, &ColorAdjustPanel::onWheelGestureStarted);
    QObject::connect(wheel, &ColorWheelWidget::gestureMoved, q, &ColorAdjustPanel::onWheelGestureMoved);
    QObject::connect(wheel, &ColorWheelWidget::gestureFinished, q, &ColorAdjustPanel::onWheelGestureFinished);
    for (int i = 0; i < kLineCount; ++i) {
        ColorEditSlider* c = customSliders[i];

        QObject::connect(c, &ColorEditSlider::gestureStarted, q, &ColorAdjustPanel::onCustomSliderGestureStarted);
        QObject::connect(c, &ColorEditSlider::gestureMoved, q, &ColorAdjustPanel::onCustomSliderGestureMoved);
        QObject::connect(c, &ColorEditSlider::gestureFinished, q, &ColorAdjustPanel::onCustomSliderGestureFinished);
        QObject::connect(c, &ColorEditSlider::resetRequested, q, &ColorAdjustPanel::onSliderResetRequested);
        QObject::connect(spinBoxes[i], &SpinBox::valueChanged, q, &ColorAdjustPanel::onSpinBoxValueChanged);
    }
}

// The wheel takes what the lines leave once they have a caption, a field and the shortest
// usable track (never below the slider's own minimum, or the layout would squeeze the wheel
// back); the wheel itself keeps that within its floor and cap.
void
ColorAdjustPanelPrivate::updateWheelSize()
{
    QHBoxLayout* areaLayout = qobject_cast<QHBoxLayout*>(editArea->layout());

    int fieldWidth = 0;
    int trackWidth = (int)TO_DPIX(kMinTrackLength);

    for (int i = 0; i < kLineCount; ++i) {
        fieldWidth = std::max(fieldWidth, spinBoxes[i]->minimumWidth());
        trackWidth = std::max(trackWidth, customSliders[i]->minimumSizeHint().width());
    }

    int linesMinimum = captionWidth + fieldWidth + 2 * linesLayout->horizontalSpacing() + trackWidth;
    int available = q->contentsRect().width() - (areaLayout ? areaLayout->spacing() : 0) - linesMinimum;

    wheel->setPreferredDiameter(available);
}

int
ColorAdjustPanelPrivate::sliderIndex(const QObject* s) const
{
    for (int i = 0; i < kLineCount; ++i) {
        if (customSliders[i] == s) {
            return i;
        }
    }

    return -1;
}

int
ColorAdjustPanelPrivate::spinBoxIndex(const QObject* s) const
{
    for (int i = 0; i < kLineCount; ++i) {
        if (spinBoxes[i] == s) {
            return i;
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
ColorAdjustPanelPrivate::lineValue(ColorSliderKindEnum kind) const
{
    double out = 0.0;

    switch (kind) {
    case eColorSliderKindHue: {
        PlanePoint p = toPlane(role, basis, value);

        out = (p.valid && planeSaturation(p) > kNeutralSaturation) ? planeHue(p) : rememberedHue;
        break;
    }
    case eColorSliderKindSat: {
        PlanePoint p = toPlane(role, basis, value);

        out = p.valid ? planeSaturation(p) : 0.0;
        break;
    }
    case eColorSliderKindValue: {
        double Y = valueAxisLuma(value);

        if (!isRatioRole(role)) {
            out = Y;
        } else {
            // A non-positive Y has no stop count, so it reads as the bottom of the range.
            out = Y > 0.0 ? std::log2(Y / valueStopPivot(role)) : sliderMapping(kind, role, rates).rangeMin;
        }
        break;
    }
    case eColorSliderKindTemp: {
        TempTintResult tt = toTempTint(role, basis, value);

        out = tt.valid ? tt.temp : 0.0;
        break;
    }
    case eColorSliderKindTint: {
        TempTintResult tt = toTempTint(role, basis, value);

        out = tt.valid ? tt.tint : 0.0;
        break;
    }
    }

    return std::isfinite(out) ? out : 0.0;
}

QGradientStops
ColorAdjustPanelPrivate::lineGradient(ColorSliderKindEnum kind) const
{
    QGradientStops stops;
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

ColorEdit
ColorAdjustPanelPrivate::lineEdit(ColorSliderKindEnum kind,
                                  double target,
                                  const KnobColorValue& from,
                                  bool* replaces) const
{
    *replaces = false;
    switch (kind) {
    case eColorSliderKindHue:
        return makeEdit(eColorAxisHue, target, 0.0, true);
    case eColorSliderKindSat:
        return makeEdit(eColorAxisSat, target, 0.0, true);
    case eColorSliderKindValue:
        return makeEdit(eColorAxisValue, target, 0.0, true);
    case eColorSliderKindTemp:
    case eColorSliderKindTint: {
        bool isTemp = kind == eColorSliderKindTemp;
        TempTintResult base = toTempTint(role, basis, from);
        double d = target - (base.valid ? (isTemp ? base.temp : base.tint) : 0.0);

        // The target becomes a total nudge from the gesture's base: it reaches the target
        // exactly wherever the locus is defined, keeps the other axis where it was, and still
        // moves along the linear axes where an absolute fromTempTint would refuse.
        *replaces = true;

        return makeEdit(eColorAxisTempTint, isTemp ? d : 0.0, isTemp ? 0.0 : d, false);
    }
    }

    return makeEdit(eColorAxisValue, target, 0.0, true);
}

// A ColorEditSlider's off-marker drag reports increments in the line's unit, which are the
// relative form of the same axis, so they accumulate in step() like the wheel's own moves.
ColorEdit
ColorAdjustPanelPrivate::lineNudge(ColorSliderKindEnum kind,
                                   double delta) const
{
    switch (kind) {
    case eColorSliderKindHue:
        return makeEdit(eColorAxisHue, delta, 0.0, false);
    case eColorSliderKindSat:
        return makeEdit(eColorAxisSat, delta, 0.0, false);
    case eColorSliderKindValue:
        return makeEdit(eColorAxisValue, delta, 0.0, false);
    case eColorSliderKindTemp:
        return makeEdit(eColorAxisTempTint, delta, 0.0, false);
    case eColorSliderKindTint:
        return makeEdit(eColorAxisTempTint, 0.0, delta, false);
    }

    return makeEdit(eColorAxisValue, delta, 0.0, false);
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
ColorAdjustPanelPrivate::applyRanges()
{
    for (int i = 0; i < kLineCount; ++i) {
        ColorSliderKindEnum kind = kLineKinds[i];
        SliderRange range = sliderMapping(kind, role, rates);
        QString tip = QObject::tr("%1, in %2. Drag the marker to set it, drag elsewhere to nudge: Ctrl 10x slower, Shift 4x faster, Ctrl+Shift 100x slower; double-click resets.")
                          .arg(lineTitle(kind), QString::fromStdString(range.unit));

        customSliders[i]->setRole(role);
        customSliders[i]->setRates(rates);
        spinBoxes[i]->decimals(lineDecimals(kind, role));
        spinBoxes[i]->setIncrement(lineIncrement(kind, range));

        // Wide enough for the longest value the line's range shows at its decimals: the grid
        // would otherwise squeeze the field in a narrow properties pane until it is clipped.
        QFontMetrics boxMetrics(spinBoxes[i]->font());
        int textWidth = std::max(boxMetrics.horizontalAdvance(QString::number(range.rangeMin, 'f', lineDecimals(kind, role))),
                                 boxMetrics.horizontalAdvance(QString::number(range.rangeMax, 'f', lineDecimals(kind, role))));

        spinBoxes[i]->setFixedWidth(textWidth + (int)TO_DPIX(10.));
        if (kind == eColorSliderKindSat) {
            spinBoxes[i]->setMinimum(0.0);
        }
        customSliders[i]->setToolTip(tip);
        spinBoxes[i]->setToolTip(tip);
        captions[i]->setToolTip(tip);
    }
}

void
ColorAdjustPanelPrivate::applyDimensions()
{
    bool single = nDims == 1;

    wheel->setVisible(!single);
    for (int i = 0; i < kLineCount; ++i) {
        bool visible = lineVisible(i);

        captions[i]->setVisible(visible);
        spinBoxes[i]->setVisible(visible);
        customSliders[i]->setVisible(visible);
    }
}

void
ColorAdjustPanelPrivate::refreshWheel()
{
    if (source == eSourceWheel) {
        return;
    }
    // Re-anchoring re-renders the whole disc, so a slider drag only moves the puck off the
    // anchor it started on; endGesture re-anchors once the drag is over.
    if (source == eSourceSlider) {
        PlanePoint now = toPlane(role, basis, value);

        if (now.valid) {
            PlanePoint anchor = wheel->getAnchor();
            PlanePoint dq;

            dq.x = now.x - (anchor.valid ? anchor.x : 0.0);
            dq.y = now.y - (anchor.valid ? anchor.y : 0.0);
            dq.valid = true;
            wheel->setDragOffset(dq);
        }

        return;
    }
    wheel->setValue(value);
}

void
ColorAdjustPanelPrivate::refreshGradients()
{
    for (int i = 0; i < kLineCount; ++i) {
        customSliders[i]->setGradientStops(lineGradient(kLineKinds[i]));
    }
}

void
ColorAdjustPanelPrivate::refreshLines(bool skipFocused)
{
    for (int i = 0; i < kLineCount; ++i) {
        ColorSliderKindEnum kind = kLineKinds[i];
        double v = lineValue(kind);

        // The dragged slider keeps its own geared position: re-seeking it to the value derived
        // back from the knob would fight the drag wherever the edit cannot reach its target.
        if (i != activeSlider) {
            customSliders[i]->setValue(v);
        }
        if (!(skipFocused && spinBoxes[i]->hasFocus())) {
            spinBoxes[i]->setValue(roundToDecimals(v, lineDecimals(kind, role)));
        }
    }
    refreshGradients();
}

void
ColorAdjustPanelPrivate::refreshAll(bool skipFocused)
{
    refreshWheel();
    refreshLines(skipFocused);
}

void
ColorAdjustPanelPrivate::beginGesture(SourceEnum src)
{
    if (source != eSourceNone) {
        endGesture();
    }
    source = src;
    activeSlider = -1;
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
    refreshLines(false);
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
    activeSlider = -1;
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
        refreshLines(false);

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

ColorAdjustPanel::ColorAdjustPanel(QWidget* parent)
    : QWidget(parent)
    , _imp(new ColorAdjustPanelPrivate(this))
{
    QVBoxLayout* layout = new QVBoxLayout(this);

    layout->setContentsMargins(0, 0, 0, 0);
    _imp->buildEditArea();
    layout->addWidget(_imp->editArea);
    _imp->connectEditArea();

    _imp->wheel->setBasis(_imp->basis);
    _imp->wheel->setRole(_imp->role);
    _imp->applyRanges();
    _imp->applyDimensions();
    _imp->updateRememberedHue();
    _imp->refreshAll(false);
    _imp->updateWheelSize();
}

ColorAdjustPanel::~ColorAdjustPanel()
{
    // The wheel outlives _imp by a few steps of QWidget teardown, and must not call back in.
    if (_imp->wheel) {
        QObject::disconnect(_imp->wheel, nullptr, this, nullptr);
    }
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
    _imp->applyRanges();
    _imp->updateWheelSize();
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
ColorAdjustPanel::setRole(ColorKnobRoleEnum role)
{
    if (role == _imp->role) {
        return;
    }
    _imp->role = role;
    _imp->wheel->setRole(role);
    _imp->applyRanges();
    _imp->updateWheelSize();
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
    _imp->refreshGradients();
}

void
ColorAdjustPanel::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    _imp->updateWheelSize();
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

QWidget*
ColorAdjustPanel::getEditArea() const
{
    return _imp->editArea;
}

ColorWheelWidget*
ColorAdjustPanel::getWheel() const
{
    return _imp->wheel;
}

ColorEditSlider*
ColorAdjustPanel::getCustomSlider(ColorSliderKindEnum kind) const
{
    for (int i = 0; i < kLineCount; ++i) {
        if (kLineKinds[i] == kind) {
            return _imp->customSliders[i];
        }
    }

    return nullptr;
}

SpinBox*
ColorAdjustPanel::getSpinBox(ColorSliderKindEnum kind) const
{
    for (int i = 0; i < kLineCount; ++i) {
        if (kLineKinds[i] == kind) {
            return _imp->spinBoxes[i];
        }
    }

    return nullptr;
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
ColorAdjustPanel::onCustomSliderGestureStarted()
{
    int index = _imp->sliderIndex(sender());

    if (index < 0) {
        return;
    }
    _imp->beginGesture(ColorAdjustPanelPrivate::eSourceSlider);
    _imp->activeSlider = index;
}

void
ColorAdjustPanel::onCustomSliderGestureMoved(double value,
                                             QString axisLabel)
{
    int index = _imp->sliderIndex(sender());

    if ((_imp->source != ColorAdjustPanelPrivate::eSourceSlider) || (index < 0) || (index != _imp->activeSlider)) {
        return;
    }

    ColorSliderKindEnum kind = kLineKinds[index];

    _imp->step(_imp->lineNudge(kind, value), false, axisLabel);
}

void
ColorAdjustPanel::onCustomSliderGestureFinished(bool /*moved*/)
{
    int index = _imp->sliderIndex(sender());

    if ((_imp->source != ColorAdjustPanelPrivate::eSourceSlider) || (index != _imp->activeSlider)) {
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

    ColorSliderKindEnum kind = kLineKinds[index];
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
        edit = makeEdit(eColorAxisValue, isRatioRole(role) ? std::log2(*target / valueStopPivot(role)) : *target, 0.0, true);
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
    }

    ChromaEditResult r = applyEdit(role, _imp->basis, v, edit, _imp->rememberedHue);

    if (r.valid) {
        _imp->oneShot(r.value, tr("reset %1").arg(lineUndoName(kind)));
    }
}

void
ColorAdjustPanel::onSpinBoxValueChanged(double value)
{
    int index = _imp->spinBoxIndex(sender());

    if ((index < 0) || isEditing()) {
        return;
    }

    ColorSliderKindEnum kind = kLineKinds[index];

    // Return re-commits whatever the field shows, which is the current value rounded for
    // display; applying that would nudge the colour by the rounding, so it is a no-op.
    if (!std::isfinite(value) || (value == roundToDecimals(_imp->lineValue(kind), lineDecimals(kind, _imp->role)))) {
        _imp->refreshLines(false);

        return;
    }

    bool replaces = false;
    ColorEdit edit = _imp->lineEdit(kind, value, _imp->value, &replaces);

    // Through a one-step gesture rather than oneShot, so a hue typed on a neutral value is
    // still remembered for the next saturation edit even though the colour itself is unchanged.
    _imp->beginGesture(ColorAdjustPanelPrivate::eSourceOneShot);
    _imp->step(edit, replaces, tr("typed %1").arg(lineUndoName(kind)));
    _imp->endGesture();
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_ColorAdjustPanel.cpp"
