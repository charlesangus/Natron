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

#ifndef NATRON_ENGINE_KNOBCOLORWARPPINS_H
#define NATRON_ENGINE_KNOBCOLORWARPPINS_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <utility>
#include <vector>

#include <QtCore/QMutex>
#include <QtCore/QObject>

#include "Engine/EngineFwd.h"
#include "Engine/Knob.h"
#include "Engine/Nodes/Color/ColorWarp/ColorWarpField.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief One keyable value of a pin: a static value, or keys (time, value) sorted by time that
 * interpolate smoothly and hold their end values outside the keyed range.
 **/
struct ColorWarpPinChannel {
    double value = 0.;
    std::vector<std::pair<double, double>> keys;

    ColorWarpPinChannel() = default;
    explicit ColorWarpPinChannel(double v)
        : value(v)
    {
    }

    bool isAnimated() const
    {
        return !keys.empty();
    }

    double valueAt(double time) const WARN_UNUSED_RETURN;

    /**
     * @brief Sets the value at time: a key there when the channel is animated, else the static value.
     **/
    void set(double time, double v);

    /**
     * @brief Adds or replaces the key at time, animating the channel.
     **/
    void setKey(double time, double v);

    /**
     * @brief Removes the key at time; the last key's removal leaves its value as the static value.
     **/
    bool removeKey(double time);

    bool hasKeyAt(double time) const WARN_UNUSED_RETURN;

    /**
     * @brief Adds delta to the static value and to every key.
     **/
    void offset(double delta);

    bool operator==(const ColorWarpPinChannel& other) const
    {
        return (value == other.value) && (keys == other.keys);
    }

    bool operator!=(const ColorWarpPinChannel& other) const
    {
        return !(*this == other);
    }
};

/**
 * @brief A pin maps its source colour to its target colour with a strength. Its pick position
 * (canonical coordinates) and frame say where the source was sampled, for display.
 **/
struct ColorWarpPinData {
    int id = 0;
    std::string name;
    bool enabled = true;
    double source[3] = { 0., 0., 0. };
    double position[2] = { 0., 0. };
    double frame = 0.;
    ColorWarpPinChannel target[3];
    ColorWarpPinChannel strength = ColorWarpPinChannel(1.);
    ColorWarpPinChannel falloff = ColorWarpPinChannel(1.);

    ColorWarpPin evaluate(double time) const WARN_UNUSED_RETURN;

    bool operator==(const ColorWarpPinData& other) const;
    bool operator!=(const ColorWarpPinData& other) const
    {
        return !(*this == other);
    }
};

struct ColorWarpPinSet {
    int nextId = 1;
    std::vector<ColorWarpPinData> pins;

    /**
     * @brief The index of the pin with id, or -1.
     **/
    int indexOf(int id) const WARN_UNUSED_RETURN;

    /**
     * @brief Appends a pin whose target is its source, with a fresh id and name; returns its index.
     **/
    int addPin(const double source[3], double x, double y, double frame);

    bool operator==(const ColorWarpPinSet& other) const
    {
        return (nextId == other.nextId) && (pins == other.pins);
    }

    bool operator!=(const ColorWarpPinSet& other) const
    {
        return !(*this == other);
    }
};

/**
 * @brief The pins of a ColorWarp node, held as one string value so that saving, loading,
 * copying and the node hash cover every pin and key. The selection is not saved.
 **/
class KnobColorWarpPins
    : public QObject,
      public KnobStringBase {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    static KnobHelper* BuildKnob(KnobHolder* holder,
                                 const std::string& label,
                                 int dimension,
                                 bool declaredByPlugin = true)
    {
        return new KnobColorWarpPins(holder, label, dimension, declaredByPlugin);
    }

    KnobColorWarpPins(KnobHolder* holder,
                      const std::string& label,
                      int dimension,
                      bool declaredByPlugin);

    virtual ~KnobColorWarpPins();

    static const std::string& typeNameStatic() WARN_UNUSED_RETURN;

    static std::string encode(const ColorWarpPinSet& pins) WARN_UNUSED_RETURN;

    /**
     * @brief Parses raw; an empty or malformed string reads as no pins.
     **/
    static ColorWarpPinSet decode(const std::string& raw) WARN_UNUSED_RETURN;

    ColorWarpPinSet getPinSet() const WARN_UNUSED_RETURN;

    /**
     * @brief The enabled pins evaluated at time.
     **/
    std::vector<ColorWarpPin> evaluate(double time) const WARN_UNUSED_RETURN;

    /**
     * @brief Replaces the pins with one value change and no undo step, as a drag in progress does.
     **/
    void setPinSet(const ColorWarpPinSet& pins);

    /**
     * @brief Replaces the pins as one undo step named text, from before to after. With no undo
     * stack (no interface) the change is just made.
     **/
    void editPinSet(const ColorWarpPinSet& before,
                    const ColorWarpPinSet& after,
                    const std::string& text);
    void editPinSet(const ColorWarpPinSet& after,
                    const std::string& text);

    /**
     * @brief The id of the selected pin, or 0 when none is selected or it no longer exists.
     **/
    int getSelectedPinId() const WARN_UNUSED_RETURN;
    void setSelectedPinId(int id);

Q_SIGNALS:

    void selectedPinChanged(int id);

private:
    virtual bool canAnimate() const OVERRIDE FINAL
    {
        return false;
    }

    virtual bool supportsExpressions() const OVERRIDE FINAL
    {
        return false;
    }

    virtual const std::string& typeName() const OVERRIDE FINAL
    {
        return typeNameStatic();
    }

    static const std::string _typeNameStr;
    mutable QMutex _cacheMutex;
    mutable bool _cacheValid;
    mutable std::string _cachedRaw;
    mutable ColorWarpPinSet _cachedPins;
    int _selectedId;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_KNOBCOLORWARPPINS_H
