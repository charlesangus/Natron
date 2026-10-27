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

#ifndef Engine_Nodes_Color_ToneEqualizerInteract_h
#define Engine_Nodes_Color_ToneEqualizerInteract_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <mutex>
#include <string>

#include <QPointF>

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The ToneEqualizer's viewer readout: darktable's hover cursor, which shows the mask
 * exposure under the pointer, the band it falls in and the correction the curve applies there.
 **/
class ToneEqualizerInteract {
public:
    struct Reading {
        bool valid = false;
        // The mask exposure under the cursor, in EV.
        float exposure = 0.f;
        // The nearest band, 0 for -8 EV.
        int band = 0;
        // The correction there, in EV.
        float correction = 0.f;
    };

    /**
     * @brief darktable's cursor label: "%+.1f EV", or "? EV" without a reading.
     **/
    static std::string exposureText(const Reading& reading) WARN_UNUSED_RETURN;

    /**
     * @brief The band and correction line, empty without a reading.
     **/
    static std::string detailText(const Reading& reading) WARN_UNUSED_RETURN;

    /**
     * @brief Both lines on one line, as the readout knob shows them.
     **/
    static std::string summaryText(const Reading& reading) WARN_UNUSED_RETURN;

    /**
     * @brief The label of band (0 for -8 EV), with its exposure.
     **/
    static std::string bandLabel(int band) WARN_UNUSED_RETURN;

    void setCursor(const QPointF& canonical);
    void clearCursor();
    bool cursor(QPointF* canonical) const WARN_UNUSED_RETURN;

    /**
     * @brief Draws the reading beside the cursor through the viewer's text renderer.
     **/
    void draw(OverlaySupport* overlay,
              const Reading& reading) const;

private:
    mutable std::mutex _mutex;
    bool _hasCursor = false;
    QPointF _cursor;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Color_ToneEqualizerInteract_h
