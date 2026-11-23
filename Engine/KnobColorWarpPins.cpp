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

#include "KnobColorWarpPins.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QMutexLocker>
#include <QtCore/QString>

#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/UndoCommand.h"

NATRON_NAMESPACE_ENTER

namespace {

bool
sameTime(double a,
         double b)
{
    return std::abs(a - b) < 1e-6;
}

QJsonValue
channelToJson(const ColorWarpPinChannel& channel)
{
    QJsonObject object;

    object.insert(QStringLiteral("v"), channel.value);
    if (channel.isAnimated()) {
        QJsonArray keys;
        for (const std::pair<double, double>& key : channel.keys) {
            QJsonArray pair;
            pair.append(key.first);
            pair.append(key.second);
            keys.append(pair);
        }
        object.insert(QStringLiteral("k"), keys);
    }

    return object;
}

ColorWarpPinChannel
channelFromJson(const QJsonValue& json,
                double fallback)
{
    ColorWarpPinChannel channel(fallback);

    if (json.isDouble()) {
        channel.value = json.toDouble();

        return channel;
    }
    const QJsonObject object = json.toObject();
    channel.value = object.value(QStringLiteral("v")).toDouble(fallback);
    const QJsonArray keys = object.value(QStringLiteral("k")).toArray();
    for (const QJsonValue& key : keys) {
        const QJsonArray pair = key.toArray();
        if (pair.size() == 2) {
            channel.setKey(pair.at(0).toDouble(), pair.at(1).toDouble());
        }
    }

    return channel;
}

QJsonArray
tripleToJson(const double v[],
             int count)
{
    QJsonArray array;

    for (int i = 0; i < count; ++i) {
        array.append(v[i]);
    }

    return array;
}

void
tripleFromJson(const QJsonValue& json,
               double v[],
               int count)
{
    const QJsonArray array = json.toArray();

    for (int i = 0; (i < count) && (i < array.size()); ++i) {
        v[i] = array.at(i).toDouble();
    }
}

class ColorWarpPinsUndoCommand
    : public UndoCommand {
public:
    ColorWarpPinsUndoCommand(const KnobColorWarpPinsPtr& knob,
                             const ColorWarpPinSet& before,
                             const ColorWarpPinSet& after,
                             const std::string& text)
        : UndoCommand(text)
        , _knob(knob)
        , _before(before)
        , _after(after)
    {
    }

    virtual void redo() OVERRIDE FINAL
    {
        if (KnobColorWarpPinsPtr knob = _knob.lock()) {
            knob->setPinSet(_after);
        }
    }

    virtual void undo() OVERRIDE FINAL
    {
        if (KnobColorWarpPinsPtr knob = _knob.lock()) {
            knob->setPinSet(_before);
        }
    }

private:
    KnobColorWarpPinsWPtr _knob;
    ColorWarpPinSet _before;
    ColorWarpPinSet _after;
};

} // anonymous namespace

double
ColorWarpPinChannel::valueAt(double time) const
{
    if (keys.empty()) {
        return value;
    }
    if (keys.size() == 1) {
        return keys.front().second;
    }
    Curve curve;
    for (const std::pair<double, double>& key : keys) {
        curve.addKeyFrame(KeyFrame(key.first, key.second));
    }

    return curve.getValueAt(time, false);
}

void
ColorWarpPinChannel::set(double time,
                         double v)
{
    if (isAnimated()) {
        setKey(time, v);
    } else {
        value = v;
    }
}

void
ColorWarpPinChannel::setKey(double time,
                            double v)
{
    for (std::pair<double, double>& key : keys) {
        if (sameTime(key.first, time)) {
            key.second = v;
            value = v;

            return;
        }
    }
    keys.emplace_back(time, v);
    std::sort(keys.begin(), keys.end(), [](const std::pair<double, double>& a, const std::pair<double, double>& b) { return a.first < b.first; });
    value = v;
}

bool
ColorWarpPinChannel::removeKey(double time)
{
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (sameTime(keys[i].first, time)) {
            value = keys[i].second;
            keys.erase(keys.begin() + i);

            return true;
        }
    }

    return false;
}

bool
ColorWarpPinChannel::hasKeyAt(double time) const
{
    for (const std::pair<double, double>& key : keys) {
        if (sameTime(key.first, time)) {
            return true;
        }
    }

    return false;
}

void
ColorWarpPinChannel::offset(double delta)
{
    value += delta;
    for (std::pair<double, double>& key : keys) {
        key.second += delta;
    }
}

ColorWarpPin
ColorWarpPinData::evaluate(double time) const
{
    ColorWarpPin pin;

    for (int c = 0; c < 3; ++c) {
        pin.source[c] = source[c];
        pin.target[c] = target[c].valueAt(time);
    }
    pin.strength = strength.valueAt(time);
    pin.falloff = falloff.valueAt(time);
    pin.enabled = enabled;

    return pin;
}

bool
ColorWarpPinData::operator==(const ColorWarpPinData& other) const
{
    return (id == other.id) && (name == other.name) && (enabled == other.enabled) && std::equal(source, source + 3, other.source) && std::equal(position, position + 2, other.position) && (frame == other.frame) && std::equal(target, target + 3, other.target) && (strength == other.strength) && (falloff == other.falloff);
}

int
ColorWarpPinSet::indexOf(int id) const
{
    for (std::size_t i = 0; i < pins.size(); ++i) {
        if (pins[i].id == id) {
            return (int)i;
        }
    }

    return -1;
}

int
ColorWarpPinSet::addPin(const double source[3],
                        double x,
                        double y,
                        double frame)
{
    ColorWarpPinData pin;

    pin.id = nextId++;
    pin.name = "pin" + std::to_string(pin.id);
    for (int c = 0; c < 3; ++c) {
        pin.source[c] = source[c];
        pin.target[c] = ColorWarpPinChannel(source[c]);
    }
    pin.position[0] = x;
    pin.position[1] = y;
    pin.frame = frame;
    pins.push_back(pin);

    return (int)pins.size() - 1;
}

const std::string KnobColorWarpPins::_typeNameStr("ColorWarpPins");

const std::string&
KnobColorWarpPins::typeNameStatic()
{
    return _typeNameStr;
}

KnobColorWarpPins::KnobColorWarpPins(KnobHolder* holder,
                                     const std::string& label,
                                     int dimension,
                                     bool declaredByPlugin)
    : QObject()
    , KnobStringBase(holder, label, dimension, declaredByPlugin)
    , _cacheMutex()
    , _cacheValid(false)
    , _cachedRaw()
    , _cachedPins()
    , _selectedId(0)
{
}

KnobColorWarpPins::~KnobColorWarpPins()
{
}

std::string
KnobColorWarpPins::encode(const ColorWarpPinSet& set)
{
    QJsonObject root;

    root.insert(QStringLiteral("nextId"), set.nextId);
    QJsonArray pins;
    for (const ColorWarpPinData& pin : set.pins) {
        QJsonObject object;
        object.insert(QStringLiteral("id"), pin.id);
        object.insert(QStringLiteral("name"), QString::fromStdString(pin.name));
        object.insert(QStringLiteral("enabled"), pin.enabled);
        object.insert(QStringLiteral("source"), tripleToJson(pin.source, 3));
        object.insert(QStringLiteral("position"), tripleToJson(pin.position, 2));
        object.insert(QStringLiteral("frame"), pin.frame);
        QJsonArray target;
        for (int c = 0; c < 3; ++c) {
            target.append(channelToJson(pin.target[c]));
        }
        object.insert(QStringLiteral("target"), target);
        object.insert(QStringLiteral("strength"), channelToJson(pin.strength));
        object.insert(QStringLiteral("falloff"), channelToJson(pin.falloff));
        pins.append(object);
    }
    root.insert(QStringLiteral("pins"), pins);

    return QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
}

ColorWarpPinSet
KnobColorWarpPins::decode(const std::string& raw)
{
    ColorWarpPinSet set;

    if (raw.empty()) {
        return set;
    }
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(raw));
    if (!document.isObject()) {
        return set;
    }
    const QJsonObject root = document.object();
    int maxId = 0;
    for (const QJsonValue& value : root.value(QStringLiteral("pins")).toArray()) {
        const QJsonObject object = value.toObject();
        ColorWarpPinData pin;
        pin.id = object.value(QStringLiteral("id")).toInt();
        pin.name = object.value(QStringLiteral("name")).toString().toStdString();
        pin.enabled = object.value(QStringLiteral("enabled")).toBool(true);
        tripleFromJson(object.value(QStringLiteral("source")), pin.source, 3);
        tripleFromJson(object.value(QStringLiteral("position")), pin.position, 2);
        pin.frame = object.value(QStringLiteral("frame")).toDouble();
        const QJsonArray target = object.value(QStringLiteral("target")).toArray();
        for (int c = 0; c < 3; ++c) {
            pin.target[c] = channelFromJson(target.at(c), pin.source[c]);
        }
        pin.strength = channelFromJson(object.value(QStringLiteral("strength")), 1.);
        pin.falloff = channelFromJson(object.value(QStringLiteral("falloff")), 1.);
        maxId = (std::max)(maxId, pin.id);
        set.pins.push_back(pin);
    }
    set.nextId = (std::max)(root.value(QStringLiteral("nextId")).toInt(1), maxId + 1);

    return set;
}

ColorWarpPinSet
KnobColorWarpPins::getPinSet() const
{
    KnobColorWarpPins* self = const_cast<KnobColorWarpPins*>(this);
    const std::string raw = self->getValue();
    QMutexLocker locker(&_cacheMutex);

    if (!_cacheValid || (raw != _cachedRaw)) {
        _cachedPins = decode(raw);
        _cachedRaw = raw;
        _cacheValid = true;
    }

    return _cachedPins;
}

std::vector<ColorWarpPin>
KnobColorWarpPins::evaluate(double time) const
{
    const ColorWarpPinSet set = getPinSet();
    std::vector<ColorWarpPin> pins;

    pins.reserve(set.pins.size());
    for (const ColorWarpPinData& pin : set.pins) {
        if (pin.enabled) {
            pins.push_back(pin.evaluate(time));
        }
    }

    return pins;
}

void
KnobColorWarpPins::setPinSet(const ColorWarpPinSet& pins)
{
    const std::string raw = encode(pins);

    if (raw == getValue()) {
        return;
    }
    setValue(raw, ViewSpec::all(), 0, eValueChangedReasonNatronInternalEdited, 0);
}

void
KnobColorWarpPins::editPinSet(const ColorWarpPinSet& before,
                              const ColorWarpPinSet& after,
                              const std::string& text)
{
    if (before == after) {
        setPinSet(after);

        return;
    }
    EffectInstance* effect = dynamic_cast<EffectInstance*>(getHolder());
    KnobColorWarpPinsPtr self = std::dynamic_pointer_cast<KnobColorWarpPins>(shared_from_this());
    if (!effect || !self || !effect->getNode()) {
        setPinSet(after);

        return;
    }
    effect->pushUndoCommand(new ColorWarpPinsUndoCommand(self, before, after, text));
}

void
KnobColorWarpPins::editPinSet(const ColorWarpPinSet& after,
                              const std::string& text)
{
    editPinSet(getPinSet(), after, text);
}

int
KnobColorWarpPins::getSelectedPinId() const
{
    const int id = _selectedId;

    return (getPinSet().indexOf(id) >= 0) ? id : 0;
}

void
KnobColorWarpPins::setSelectedPinId(int id)
{
    if (id == _selectedId) {
        return;
    }
    _selectedId = id;
    Q_EMIT selectedPinChanged(id);
    if (EffectInstance* effect = dynamic_cast<EffectInstance*>(getHolder())) {
        effect->redrawOverlayInteract();
    }
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_KnobColorWarpPins.cpp"
