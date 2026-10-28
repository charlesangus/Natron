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

#include "ToneEqualizerInteract.h"

#include <cmath>
#include <cstdio>

#include "Engine/Nodes/Color/ToneEqualizerKernels.h"
#include "Engine/OverlaySupport.h"

NATRON_NAMESPACE_ENTER

namespace {
const char* const kBandNames[ToneEqualizerKernels::kBands] = {
    "Blacks",
    "Deep Shadows",
    "Shadows",
    "Light Shadows",
    "Midtones",
    "Dark Highlights",
    "Highlights",
    "Whites",
    "Speculars",
};

// darktable's cursor sits 16 screen pixels wide with its label beyond that and some padding.
const double kTextOffsetPixels = 24.;
} // namespace

std::string
ToneEqualizerInteract::exposureText(const Reading& reading)
{
    if (!reading.valid) {
        return "? EV";
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%+.1f EV", reading.exposure);

    return buffer;
}

std::string
ToneEqualizerInteract::detailText(const Reading& reading)
{
    if (!reading.valid) {
        return std::string();
    }
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%s, correction %+.2f EV", bandLabel(reading.band).c_str(), reading.correction);

    return buffer;
}

std::string
ToneEqualizerInteract::summaryText(const Reading& reading)
{
    if (!reading.valid) {
        return exposureText(reading);
    }

    return exposureText(reading) + "  " + detailText(reading);
}

std::string
ToneEqualizerInteract::bandLabel(int band)
{
    if ((band < 0) || (band >= ToneEqualizerKernels::kBands)) {
        return std::string();
    }
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%s (%+d EV)", kBandNames[band], band + (int)ToneEqualizerKernels::kMinEv);

    return buffer;
}

float
ToneEqualizerInteract::wheelStepEv(KeyboardModifiers modifiers)
{
    const bool shift = modifiers.testFlag(eKeyboardModifierShift);
    const bool control = modifiers.testFlag(eKeyboardModifierControl);
    const bool other = modifiers.testFlag(eKeyboardModifierAlt) || modifiers.testFlag(eKeyboardModifierMeta);

    if (shift && !control && !other) {
        return 1.0f;
    }
    if (control && !shift && !other) {
        return 0.1f;
    }

    return 0.25f;
}

int
ToneEqualizerInteract::takeWheelSteps(double notches,
                                      bool* editedBurst)
{
    // Turns further apart than this start a new undo step.
    const std::chrono::milliseconds burstGap(500);
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_hasWheel || (now - _lastWheel > burstGap)) {
        _wheelRemainder = 0.;
        _wheelEdited = false;
    } else if (notches * _wheelRemainder < 0.) {
        _wheelRemainder = 0.;
    }
    *editedBurst = _wheelEdited;
    _hasWheel = true;
    _lastWheel = now;
    _wheelRemainder += notches;
    const int steps = (int)std::trunc(_wheelRemainder);
    _wheelRemainder -= steps;

    return steps;
}

void
ToneEqualizerInteract::markWheelEdit()
{
    std::lock_guard<std::mutex> lock(_mutex);

    _wheelEdited = true;
}

void
ToneEqualizerInteract::setCursor(const QPointF& canonical)
{
    std::lock_guard<std::mutex> lock(_mutex);

    _hasCursor = true;
    _cursor = canonical;
}

void
ToneEqualizerInteract::clearCursor()
{
    std::lock_guard<std::mutex> lock(_mutex);

    _hasCursor = false;
}

bool
ToneEqualizerInteract::cursor(QPointF* canonical) const
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (_hasCursor) {
        *canonical = _cursor;
    }

    return _hasCursor;
}

void
ToneEqualizerInteract::draw(OverlaySupport* overlay,
                            const Reading& reading) const
{
    QPointF pos;

    if (!overlay || !cursor(&pos)) {
        return;
    }
    double pixelScaleX = 1.;
    double pixelScaleY = 1.;
    overlay->getPixelScale(pixelScaleX, pixelScaleY);
    const double x = pos.x() + kTextOffsetPixels * pixelScaleX;
    const double lineHeight = (overlay->getWidgetFontHeight() + 2) * pixelScaleY;
    const bool drawn = overlay->renderText(x, pos.y(), exposureText(reading), 1., 1., 1.);
    Q_UNUSED(drawn);
    const std::string detail = detailText(reading);
    if (!detail.empty()) {
        const bool detailDrawn = overlay->renderText(x, pos.y() - lineHeight, detail, 1., 1., 1.);
        Q_UNUSED(detailDrawn);
    }
}

NATRON_NAMESPACE_EXIT
