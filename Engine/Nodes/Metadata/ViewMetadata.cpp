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

#include "ViewMetadata.h"

#include <charconv>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include <QMetaObject>
#include <QString>

#include "Engine/AppInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

namespace {
std::string
escapeHtml(const std::string& text)
{
    QString escaped = QString::fromStdString(text).toHtmlEscaped();

    escaped.replace(QLatin1Char('\n'), QString::fromUtf8("<br />"));

    return escaped.toStdString();
}

std::string
formatDouble(double value)
{
    char buffer[64];
    const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value);

    return std::string(buffer, result.ptr);
}

template <typename T, typename Format>
std::string
joinValues(const std::vector<T>& values,
           Format format)
{
    std::string joined;

    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            joined += ", ";
        }
        joined += format(values[i]);
    }

    return joined;
}

// Updating the display must not look like an edit: a plain setValue() would still run knobChanged,
// the Python param-changed callback and a viewer redraw. Blocking the knob's value-change handlers
// and discarding the evaluation when the bracket closes leaves only the new text.
class PassiveKnobUpdate {
public:
    PassiveKnobUpdate(KnobHolder* holder,
                      const KnobIPtr& knob)
        : _holder(holder)
        , _knob(knob)
    {
        _knob->blockValueChanges();
        _holder->beginChanges();
    }

    ~PassiveKnobUpdate()
    {
        try {
            _holder->endChanges(true);
        } catch (...) {
        }
        _knob->unblockValueChanges();
    }

    PassiveKnobUpdate(const PassiveKnobUpdate&) = delete;
    PassiveKnobUpdate& operator=(const PassiveKnobUpdate&) = delete;

private:
    KnobHolder* _holder;
    KnobIPtr _knob;
};

std::string
formatValue(const ImageMetadata::Value& value)
{
    switch (value.index()) {
    case 0:

        return std::to_string(std::get<int>(value));
    case 1:

        return formatDouble(std::get<double>(value));
    case 2:

        return std::get<std::string>(value);
    case 3:

        return joinValues(std::get<std::vector<int>>(value), [](int v) { return std::to_string(v); });
    default:

        return joinValues(std::get<std::vector<double>>(value), [](double v) { return formatDouble(v); });
    }
}
} // namespace

ViewMetadata::ViewMetadata(NodePtr node)
    : NativeEffectBase(node)
    , _display()
    , _refreshButton()
    , _refreshPending(false)
{
}

void
ViewMetadata::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Metadata"));

    KnobStringPtr display = createKnob<KnobString>(std::string());
    display->setName("metadataDisplay");
    display->setAnimationEnabled(false);
    display->setIsPersistent(false);
    display->setAsMultiLine();
    display->setAsCustomHTMLText(true);
    display->setPreferredRows(20);
    display->setEvaluateOnChange(false);
    display->setHintToolTip(tr("The metadata of the input at the current frame. It follows the timeline "
                               "and upstream changes, or press Refresh to update it.")
                                .toStdString());
    page->addKnob(display);
    _display = display;

    KnobButtonPtr refresh = createKnob<KnobButton>(tr("Refresh"));
    refresh->setName("refresh");
    refresh->setEvaluateOnChange(false);
    refresh->setHintToolTip(tr("Redraws the table with the current metadata.").toStdString());
    page->addKnob(refresh);
    _refreshButton = refresh;
}

std::string
ViewMetadata::getMetadataDisplayText() const
{
    KnobStringPtr display = _display.lock();

    return display ? display->getValue() : std::string();
}

void
ViewMetadata::refreshMetadataDisplay(double time)
{
    KnobStringPtr display = _display.lock();

    if (!display) {
        return;
    }

    const ImageMetadata metadata = getOutputMetadata(time, ViewIdx(0));
    std::ostringstream text;

    if (metadata.empty()) {
        text << "<i>" << escapeHtml(tr("No metadata").toStdString()) << "</i>";
    } else {
        text << "<table cellspacing=\"4\">";
        for (ImageMetadata::const_iterator it = metadata.begin(); it != metadata.end(); ++it) {
            text << "<tr><td style=\"white-space:nowrap\"><b>" << escapeHtml(it->first) << "</b></td><td><font color=#c8c8c8>"
                 << escapeHtml(formatValue(it->second)) << "</font></td></tr>";
        }
        text << "</table>";
    }

    const std::string html = text.str();

    if (html != display->getValue()) {
        PassiveKnobUpdate passive(this, display);
        display->setValue(html);
    }
}

void
ViewMetadata::scheduleRefresh()
{
    if (_refreshPending.exchange(true)) {
        return;
    }

    // The hooks that call this fire mid-recompute of a hash or the clip preferences,
    // before the upstream metadata has settled.
    QMetaObject::invokeMethod(this, [this]() {
        _refreshPending = false;
        AppInstancePtr app = getApp();
        if (app) {
            refreshMetadataDisplay(app->getTimeLine()->currentFrame());
        } }, Qt::QueuedConnection);
}

bool
ViewMetadata::knobChanged(KnobI* k,
                          ValueChangedReasonEnum /*reason*/,
                          ViewSpec /*view*/,
                          double time,
                          bool /*originatedFromMainThread*/)
{
    KnobButtonPtr refresh = _refreshButton.lock();

    if (refresh && (k == refresh.get())) {
        refreshMetadataDisplay(time);

        return true;
    }

    return false;
}

Route
ViewMetadata::route(double time,
                    ViewIdx /*view*/)
{
    return Route::input(0, time);
}

void
ViewMetadata::refreshExtraStateAfterTimeChanged(bool isPlayback,
                                                double time)
{
    NativeEffectBase::refreshExtraStateAfterTimeChanged(isPlayback, time);
    refreshMetadataDisplay(time);
}

void
ViewMetadata::onNodeHashChanged(U64 hash)
{
    NativeEffectBase::onNodeHashChanged(hash);
    scheduleRefresh();
}

void
ViewMetadata::onCurrentFrameMetadataChanged(const NodeMetadata& metadata)
{
    NativeEffectBase::onCurrentFrameMetadataChanged(metadata);
    scheduleRefresh();
}

NATRON_NAMESPACE_EXIT
