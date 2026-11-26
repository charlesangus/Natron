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

#include "ReadTimeDomainKnobs.h"

#include <limits>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

namespace {
const char* const kKnobOriginalFrameRange = kReaderParamNameOriginalFrameRange;
const char* const kKnobFirstFrame = "firstFrame";
const char* const kKnobLastFrame = "lastFrame";
const char* const kKnobBefore = "beforeFirstFrame";
const char* const kKnobAfter = "afterLastFrame";
const char* const kKnobOnMissingFrame = "onMissingFrame";
const char* const kKnobFrameMode = "frameOffsetMode";
const char* const kKnobStartingTime = "startAt";
const char* const kKnobTimeOffset = "frameOffset";
const char* const kKnobTimeDomainUserEdited = "timeDomainUserEdited";

bool
timeDomainAtDefaults(const KnobIntPtr& firstFrame,
                     const KnobIntPtr& lastFrame,
                     const KnobIntPtr& startingTime,
                     const KnobIntPtr& timeOffset)
{
    return firstFrame->getValue() == firstFrame->getDefaultValue(0)
        && lastFrame->getValue() == lastFrame->getDefaultValue(0)
        && startingTime->getValue() == startingTime->getDefaultValue(0)
        && timeOffset->getValue() == timeOffset->getDefaultValue(0);
}
} // anonymous namespace

void
ReadTimeDomainKnobs::create(KnobHolder* holder,
                            const KnobPagePtr& page)
{
    const std::vector<ChoiceOption> beforeAfterChoices = {
        ChoiceOption("hold", tr("Hold").toStdString(), tr("While outside the sequence, load the nearest end frame.").toStdString()),
        ChoiceOption("loop", tr("Loop").toStdString(), tr("Repeat the sequence outside its range.").toStdString()),
        ChoiceOption("bounce", tr("Bounce").toStdString(), tr("Repeat the sequence in reverse outside its range.").toStdString()),
        ChoiceOption("black", tr("Black").toStdString(), tr("Render a black image.").toStdString()),
        ChoiceOption("error", tr("Error").toStdString(), tr("Report an error.").toStdString()),
    };

    KnobIntPtr firstFrame = AppManager::createKnob<KnobInt>(holder, tr("First Frame"));
    firstFrame->setName(kKnobFirstFrame);
    firstFrame->setHintToolTip(tr("The first frame number to read from this image sequence. It cannot be less than the first "
                                  "frame of the sequence or greater than its last. With Frame Offset at 0, or Start At equal to it, "
                                  "this is also the first output frame."));
    firstFrame->setDefaultValue(0);
    firstFrame->setAnimationEnabled(false);
    firstFrame->disableSlider();
    firstFrame->setAddNewLine(false);
    page->addKnob(firstFrame);
    _firstFrame = firstFrame;

    KnobChoicePtr before = AppManager::createKnob<KnobChoice>(holder, tr("Before First Frame"));
    before->setName(kKnobBefore);
    before->setHintToolTip(tr("What to do before the first frame of the sequence."));
    before->populateChoices(beforeAfterChoices);
    before->setDefaultValue((int)ReadTimeDomain::eBeforeAfterHold);
    page->addKnob(before);
    _before = before;

    KnobIntPtr lastFrame = AppManager::createKnob<KnobInt>(holder, tr("Last Frame"));
    lastFrame->setName(kKnobLastFrame);
    lastFrame->setHintToolTip(tr("The last frame number to read from this image sequence. It cannot be less than the first "
                                 "frame of the sequence or greater than its last. With Frame Offset at 0, or Start At equal to First Frame, "
                                 "this is also the last output frame."));
    lastFrame->setDefaultValue(0);
    lastFrame->setAnimationEnabled(false);
    lastFrame->disableSlider();
    lastFrame->setAddNewLine(false);
    page->addKnob(lastFrame);
    _lastFrame = lastFrame;

    KnobChoicePtr after = AppManager::createKnob<KnobChoice>(holder, tr("After Last Frame"));
    after->setName(kKnobAfter);
    after->setHintToolTip(tr("What to do after the last frame of the sequence."));
    after->populateChoices(beforeAfterChoices);
    after->setDefaultValue((int)ReadTimeDomain::eBeforeAfterHold);
    page->addKnob(after);
    _after = after;

    KnobChoicePtr onMissing = AppManager::createKnob<KnobChoice>(holder, tr("On Missing Frame"));
    onMissing->setName(kKnobOnMissingFrame);
    onMissing->setHintToolTip(tr("What to do when a frame is missing from the sequence."));
    std::vector<ChoiceOption> missingChoices;
    missingChoices.push_back(ChoiceOption("previous", tr("Hold previous").toStdString(), tr("Try to load the previous frame in the sequence, if any.").toStdString()));
    missingChoices.push_back(ChoiceOption("next", tr("Load next").toStdString(), tr("Try to load the next frame in the sequence, if any.").toStdString()));
    missingChoices.push_back(ChoiceOption("nearest", tr("Load nearest").toStdString(), tr("Try to load the nearest frame in the sequence, if any.").toStdString()));
    missingChoices.push_back(ChoiceOption("error", tr("Error").toStdString(), tr("Report an error.").toStdString()));
    missingChoices.push_back(ChoiceOption("black", tr("Black").toStdString(), tr("Render a black image.").toStdString()));
    onMissing->populateChoices(missingChoices);
    onMissing->setDefaultValue((int)ReadTimeDomain::eMissingError);
    page->addKnob(onMissing);
    _onMissingFrame = onMissing;

    KnobChoicePtr frameMode = AppManager::createKnob<KnobChoice>(holder, tr("Frame Offset Mode"));
    frameMode->setName(kKnobFrameMode);
    std::vector<ChoiceOption> modeChoices;
    modeChoices.push_back(ChoiceOption("startAt", tr("Start At").toStdString(), tr("Start At sets the output frame of the sequence frame designated by First Frame.").toStdString()));
    modeChoices.push_back(ChoiceOption("offset", tr("Offset").toStdString(), tr("Frame Offset is added to every frame number: the sequence frame designated by First Frame is output at First Frame + Frame Offset.").toStdString()));
    frameMode->populateChoices(modeChoices);
    frameMode->setDefaultValue((int)ReadTimeDomain::eFrameModeStartingTime);
    frameMode->setAnimationEnabled(false);
    frameMode->setAddNewLine(false);
    page->addKnob(frameMode);
    _frameMode = frameMode;

    KnobIntPtr startingTime = AppManager::createKnob<KnobInt>(holder, tr("Start At"));
    startingTime->setName(kKnobStartingTime);
    startingTime->setHintToolTip(tr("The output frame of the sequence frame designated by First Frame, when Frame Offset Mode is Start At."));
    startingTime->setDefaultValue(0);
    startingTime->setAnimationEnabled(false);
    startingTime->setAddNewLine(false);
    page->addKnob(startingTime);
    _startingTime = startingTime;

    KnobIntPtr timeOffset = AppManager::createKnob<KnobInt>(holder, tr("Frame Offset"));
    timeOffset->setName(kKnobTimeOffset);
    timeOffset->setHintToolTip(tr("The number of frames added to every frame number of the sequence, when Frame Offset Mode is Offset."));
    timeOffset->setDefaultValue(0);
    timeOffset->setAnimationEnabled(false);
    page->addKnob(timeOffset);
    _timeOffset = timeOffset;

    KnobBoolPtr userEdited = AppManager::createKnob<KnobBool>(holder, tr("Time Domain User Edited"));
    userEdited->setName(kKnobTimeDomainUserEdited);
    userEdited->setDefaultValue(false);
    userEdited->setAnimationEnabled(false);
    userEdited->setEvaluateOnChange(false);
    userEdited->setSecret(true);
    page->addKnob(userEdited);
    _userEdited = userEdited;

    KnobIntPtr originalRange = AppManager::createKnob<KnobInt>(holder, tr("Original Range"), 2);
    originalRange->setName(kKnobOriginalFrameRange);
    originalRange->setDimensionName(0, "min");
    originalRange->setDimensionName(1, "max");
    originalRange->setDefaultValue(std::numeric_limits<int>::min(), 0);
    originalRange->setDefaultValue(std::numeric_limits<int>::max(), 1);
    originalRange->setAnimationEnabled(false);
    originalRange->setIsPersistent(false);
    originalRange->setSecret(true);
    page->addKnob(originalRange);
    _originalFrameRange = originalRange;
}

ReadTimeDomain::Settings
ReadTimeDomainKnobs::settingsAt(double time) const
{
    ReadTimeDomain::Settings settings;
    KnobIntPtr firstFrame = _firstFrame.lock();
    KnobIntPtr lastFrame = _lastFrame.lock();
    KnobChoicePtr before = _before.lock();
    KnobChoicePtr after = _after.lock();
    KnobChoicePtr onMissing = _onMissingFrame.lock();
    KnobChoicePtr frameMode = _frameMode.lock();
    KnobIntPtr startingTime = _startingTime.lock();
    KnobIntPtr timeOffset = _timeOffset.lock();

    if (!firstFrame || !lastFrame || !before || !after || !onMissing || !frameMode || !startingTime || !timeOffset) {
        return settings;
    }
    settings.firstFrame = firstFrame->getValue();
    settings.lastFrame = lastFrame->getValue();
    settings.before = (ReadTimeDomain::BeforeAfter)before->getValueAtTime(time);
    settings.after = (ReadTimeDomain::BeforeAfter)after->getValueAtTime(time);
    settings.onMissingFrame = (ReadTimeDomain::MissingFrame)onMissing->getValueAtTime(time);
    settings.frameMode = (ReadTimeDomain::FrameMode)frameMode->getValue();
    settings.startingTime = startingTime->getValue();
    settings.timeOffset = timeOffset->getValue();

    return settings;
}

void
ReadTimeDomainKnobs::refreshKnobState()
{
    KnobChoicePtr frameMode = _frameMode.lock();
    KnobIntPtr startingTime = _startingTime.lock();
    KnobIntPtr timeOffset = _timeOffset.lock();

    if (frameMode && startingTime && timeOffset) {
        const bool byStartingTime = frameMode->getValue() == (int)ReadTimeDomain::eFrameModeStartingTime;
        startingTime->setSecret(!byStartingTime);
        timeOffset->setSecret(byStartingTime);
    }
}

bool
ReadTimeDomainKnobs::knobChanged(KnobI* k,
                                 ValueChangedReasonEnum reason)
{
    KnobIntPtr originalRange = _originalFrameRange.lock();
    KnobIntPtr firstFrame = _firstFrame.lock();
    KnobIntPtr lastFrame = _lastFrame.lock();
    KnobChoicePtr frameMode = _frameMode.lock();
    KnobIntPtr startingTime = _startingTime.lock();
    KnobIntPtr timeOffset = _timeOffset.lock();
    KnobBoolPtr userEdited = _userEdited.lock();

    if (!originalRange || !firstFrame || !lastFrame || !frameMode || !startingTime || !timeOffset || !userEdited) {
        return false;
    }
    const bool byUser = reason == eValueChangedReasonUserEdited;

    if (k == originalRange.get()) {
        const int originalFirst = originalRange->getValue(0);
        const int originalLast = originalRange->getValue(1);
        if (originalFirst == std::numeric_limits<int>::min() || originalLast == std::numeric_limits<int>::max() || originalFirst > originalLast) {
            return true;
        }
        firstFrame->setDisplayMinimum(originalFirst);
        firstFrame->setDisplayMaximum(originalLast);
        lastFrame->setDisplayMinimum(originalFirst);
        lastFrame->setDisplayMaximum(originalLast);
        // The defaults follow the file even under an edit, so that resetting it adopts this file.
        firstFrame->setDefaultValueWithoutApplying(originalFirst);
        lastFrame->setDefaultValueWithoutApplying(originalLast);
        startingTime->setDefaultValueWithoutApplying(originalFirst);
        if (!userEdited->getValue()) {
            firstFrame->setValue(originalFirst);
            lastFrame->setValue(originalLast);
            startingTime->setValue(originalFirst);
            timeOffset->setValue(0);
        }

        return true;
    }
    // Resetting a knob reaches here twice with the restore reason, so every step below is a pure
    // function of the knob values.
    const bool restored = reason == eValueChangedReasonRestoreDefault;
    const bool edited = byUser || restored;

    // A time domain back at its defaults follows the file again.
    const auto markEdited = [&]() {
        userEdited->setValue(!restored || !timeDomainAtDefaults(firstFrame, lastFrame, startingTime, timeOffset));
    };

    if (k == firstFrame.get() && edited) {
        lastFrame->setDisplayMinimum(firstFrame->getValue());
        startingTime->setValue(firstFrame->getValue() + timeOffset->getValue());
        markEdited();

        return true;
    }
    if (k == lastFrame.get() && edited) {
        firstFrame->setDisplayMaximum(lastFrame->getValue());
        markEdited();

        return true;
    }
    if (k == frameMode.get() && edited) {
        refreshKnobState();

        return true;
    }
    if (k == startingTime.get() && edited) {
        timeOffset->setValue(startingTime->getValue() - firstFrame->getValue());
        markEdited();

        return true;
    }
    if (k == timeOffset.get() && edited) {
        startingTime->setValue(timeOffset->getValue() + firstFrame->getValue());
        markEdited();

        return true;
    }

    return false;
}

void
ReadTimeDomainKnobs::limitToFrames(EffectInstance* effect,
                                   int first,
                                   int last)
{
    if (!effect) {
        return;
    }
    KnobBoolPtr userEdited = effect->getKnobByNameAndType<KnobBool>(kKnobTimeDomainUserEdited);
    KnobIntPtr firstFrame = effect->getKnobByNameAndType<KnobInt>(kKnobFirstFrame);
    KnobIntPtr lastFrame = effect->getKnobByNameAndType<KnobInt>(kKnobLastFrame);
    KnobIntPtr startingTime = effect->getKnobByNameAndType<KnobInt>(kKnobStartingTime);
    KnobIntPtr timeOffset = effect->getKnobByNameAndType<KnobInt>(kKnobTimeOffset);
    if (!userEdited || !firstFrame || !lastFrame || !startingTime || !timeOffset) {
        return;
    }
    userEdited->setValue(true);
    firstFrame->setValue(first);
    lastFrame->setValue(last);
    startingTime->setValue(first);
    timeOffset->setValue(0);
}

NATRON_NAMESPACE_EXIT
