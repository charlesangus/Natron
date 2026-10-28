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

#include "CompareMetadata.h"

#include <string>

#include <QMetaObject>

#include "Engine/AppInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Metadata/MetadataText.h"
#include "Engine/Nodes/Metadata/PassiveKnobUpdate.h"
#include "Engine/RectD.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

namespace {
void
appendLine(std::string& text,
           const std::string& line)
{
    if (!text.empty()) {
        text += '\n';
    }
    text += line;
}
} // namespace

CompareMetadata::CompareMetadata(NodePtr node)
    : NativeEffectBase(node)
    , _display()
    , _refreshButton()
    , _refreshPending(false)
{
}

std::string
CompareMetadata::differences(const ImageMetadata& a,
                             const ImageMetadata& b)
{
    std::string text;
    ImageMetadata::const_iterator ai = a.begin();
    ImageMetadata::const_iterator bi = b.begin();

    while ((ai != a.end()) || (bi != b.end())) {
        if ((bi == b.end()) || ((ai != a.end()) && (ai->first < bi->first))) {
            appendLine(text, "A only: " + ai->first + "=" + MetadataText::formatValuePlain(ai->second));
            ++ai;
        } else if ((ai == a.end()) || (bi->first < ai->first)) {
            appendLine(text, "B only: " + bi->first + "=" + MetadataText::formatValuePlain(bi->second));
            ++bi;
        } else {
            // Compared as text, as displayed: a key both hold with the same text reads as equal.
            const std::string aValue = MetadataText::formatValuePlain(ai->second);
            const std::string bValue = MetadataText::formatValuePlain(bi->second);
            if (aValue != bValue) {
                appendLine(text, "differs: " + ai->first + ": A=" + aValue + " B=" + bValue);
            }
            ++ai;
            ++bi;
        }
    }

    return text;
}

void
CompareMetadata::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobStringPtr display = createKnob<KnobString>(tr("Differences"));
    display->setName("differences");
    display->setAnimationEnabled(false);
    display->setIsPersistent(false);
    display->setAsMultiLine();
    display->setPreferredRows(20);
    display->setEvaluateOnChange(false);
    display->setAllDimensionsEnabled(false);
    display->setHintToolTip(tr("One line per key that A and B disagree on at the current frame: a key held "
                               "by only one of them, or held by both with different values. A key both "
                               "hold with the same value is not shown. It follows the timeline and "
                               "upstream changes, or press Refresh to update it.")
                                .toStdString());
    page->addKnob(display);
    _display = display;

    KnobButtonPtr refresh = createKnob<KnobButton>(tr("Refresh"));
    refresh->setName("refresh");
    refresh->setEvaluateOnChange(false);
    refresh->setHintToolTip(tr("Recomputes the differences with the current metadata.").toStdString());
    page->addKnob(refresh);
    _refreshButton = refresh;
}

std::string
CompareMetadata::getDifferencesDisplayText() const
{
    KnobStringPtr display = _display.lock();

    return display ? display->getValue() : std::string();
}

void
CompareMetadata::refreshDifferencesDisplay(double time)
{
    KnobStringPtr display = _display.lock();

    if (!display) {
        return;
    }

    const std::string text = differences(getInputMetadata(0, time, ViewIdx(0)), getInputMetadata(1, time, ViewIdx(0)));

    if (text != display->getValue()) {
        PassiveKnobUpdate passive(this, display);
        display->setValue(text);
    }
}

void
CompareMetadata::scheduleRefresh()
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
            refreshDifferencesDisplay(app->getTimeLine()->currentFrame());
        } }, Qt::QueuedConnection);
}

bool
CompareMetadata::knobChanged(KnobI* k,
                             ValueChangedReasonEnum /*reason*/,
                             ViewSpec /*view*/,
                             double time,
                             bool /*originatedFromMainThread*/)
{
    KnobButtonPtr refresh = _refreshButton.lock();

    if (refresh && (k == refresh.get())) {
        refreshDifferencesDisplay(time);

        return true;
    }

    return false;
}

Route
CompareMetadata::route(double time,
                       ViewIdx /*view*/)
{
    return Route::input(0, time);
}

StatusEnum
CompareMetadata::getRegionOfDefinition(U64 /*hash*/,
                                       double time,
                                       const RenderScale& scale,
                                       ViewIdx view,
                                       RectD* rod)
{
    const EffectInstancePtr input = getInput(0);
    if (!input) {
        return eStatusReplyDefault;
    }
    bool isProjectFormat = false;

    return input->getRegionOfDefinition_public(input->getRenderHash(), time, scale, view, rod, &isProjectFormat);
}

ImageMetadata
CompareMetadata::deriveOutputMetadata(double time,
                                      ViewIdx view)
{
    return getInputMetadata(0, time, view);
}

void
CompareMetadata::refreshExtraStateAfterTimeChanged(bool isPlayback,
                                                   double time)
{
    NativeEffectBase::refreshExtraStateAfterTimeChanged(isPlayback, time);
    refreshDifferencesDisplay(time);
}

void
CompareMetadata::onNodeHashChanged(U64 hash)
{
    NativeEffectBase::onNodeHashChanged(hash);
    scheduleRefresh();
}

void
CompareMetadata::onCurrentFrameMetadataChanged(const NodeMetadata& metadata)
{
    NativeEffectBase::onCurrentFrameMetadataChanged(metadata);
    scheduleRefresh();
}

NATRON_NAMESPACE_EXIT
