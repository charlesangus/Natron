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

#include "TimeCodeMetadata.h"

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"

NATRON_NAMESPACE_ENTER

namespace {
// The frame Start Timecode lands on while Use Start Frame is off.
const int kFixedOrigin = 1;
} // namespace

TimeCodeMetadata::TimeCodeMetadata(NodePtr node)
    : NativeEffectBase(node)
    , _startTimecode()
    , _rate()
    , _rateFromMetadata()
    , _startFrame()
    , _useStartFrame()
{
}

int
TimeCodeMetadata::roundedRate(double rate)
{
    const int rounded = (int)(rate + 0.5);

    return rounded < 1 ? 1 : rounded;
}

std::array<int, 4>
TimeCodeMetadata::parseTimecode(const std::string& text)
{
    std::array<int, 4> fields = { 0, 0, 0, 0 };
    int read = 0;
    std::size_t pos = 0;

    while ((pos < text.size()) && (read < 4)) {
        if ((text[pos] < '0') || (text[pos] > '9')) {
            ++pos;
            continue;
        }
        int value = 0;
        while ((pos < text.size()) && (text[pos] >= '0') && (text[pos] <= '9')) {
            value = value * 10 + (text[pos] - '0');
            ++pos;
        }
        fields[read++] = value;
    }
    if (read < 4) {
        fields = { 0, 0, 0, 0 };
    }

    return fields;
}

long long
TimeCodeMetadata::timecodeToFrames(const std::string& text,
                                   int rate)
{
    const std::array<int, 4> fields = parseTimecode(text);

    return (((long long)fields[0] * 60 + fields[1]) * 60 + fields[2]) * rate + fields[3];
}

std::string
TimeCodeMetadata::framesToTimecode(long long frames,
                                   int rate)
{
    const long long day = 24LL * 60 * 60 * rate;

    frames %= day;
    if (frames < 0) {
        frames += day;
    }
    const long long seconds = frames / rate;
    char buffer[32];

    std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld:%02lld",
                  seconds / 3600, (seconds / 60) % 60, seconds % 60, frames % rate);

    return std::string(buffer);
}

void
TimeCodeMetadata::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobStringPtr startTimecode = createKnob<KnobString>(tr("Start Timecode"));
    startTimecode->setName("startTimecode");
    startTimecode->setAnimationEnabled(false);
    startTimecode->setDefaultValue("01:00:00:00");
    startTimecode->setHintToolTip(tr("The non-drop-frame HH:MM:SS:FF the count starts from, carried at the "
                                     "origin frame and counted on by one frame for each frame after it, "
                                     "wrapping back to 00:00:00:00 after 23:59:59:FF. Text with fewer than "
                                     "four numbers reads as 00:00:00:00.")
                                      .toStdString());
    page->addKnob(startTimecode);
    _startTimecode = startTimecode;

    KnobDoublePtr rate = createKnob<KnobDouble>(tr("Rate"));
    rate->setName("rate");
    rate->setAnimationEnabled(false);
    rate->setDefaultValue(24.);
    rate->setMinimum(1.);
    rate->setMaximum(1000.);
    rate->setDisplayMinimum(1.);
    rate->setDisplayMaximum(120.);
    rate->setHintToolTip(tr("The frames per second the count runs at, used when the rate is not taken from "
                            "the metadata of Source. The frames field counts to the nearest whole number "
                            "of it.")
                             .toStdString());
    page->addKnob(rate);
    _rate = rate;

    KnobBoolPtr rateFromMetadata = createKnob<KnobBool>(tr("Rate From Metadata"));
    rateFromMetadata->setName("rateFromMetadata");
    rateFromMetadata->setAnimationEnabled(false);
    rateFromMetadata->setDefaultValue(true);
    rateFromMetadata->setHintToolTip(tr("Reads the frame rate from the ofx/framerate of Source rather than "
                                        "from Rate, falling back to Rate where Source carries none. The rate "
                                        "read is what the frames field counts to and what the start timecode "
                                        "is read at, so it moves the timecode as well as the rate published "
                                        "with it.")
                                         .toStdString());
    page->addKnob(rateFromMetadata);
    _rateFromMetadata = rateFromMetadata;

    KnobIntPtr startFrame = createKnob<KnobInt>(tr("Start Frame"));
    startFrame->setName("startFrame");
    startFrame->setAnimationEnabled(false);
    startFrame->setDefaultValue(kFixedOrigin);
    startFrame->setHintToolTip(tr("The frame the start timecode lands on, used only when Use Start Frame is "
                                  "on. Frames before it count backwards from the start timecode.")
                                   .toStdString());
    page->addKnob(startFrame);
    _startFrame = startFrame;

    KnobBoolPtr useStartFrame = createKnob<KnobBool>(tr("Use Start Frame"));
    useStartFrame->setName("useStartFrame");
    useStartFrame->setAnimationEnabled(false);
    useStartFrame->setDefaultValue(false);
    useStartFrame->setHintToolTip(tr("Counts from Start Frame rather than from frame 1. With this off the start "
                                     "timecode always lands on frame 1 and Start Frame is ignored, whatever "
                                     "frame the clip or the project begins at.")
                                      .toStdString());
    page->addKnob(useStartFrame);
    _useStartFrame = useStartFrame;
}

Route
TimeCodeMetadata::route(double time,
                        ViewIdx /*view*/)
{
    return Route::input(0, time);
}

ImageMetadata
TimeCodeMetadata::deriveOutputMetadata(double time,
                                       ViewIdx view)
{
    ImageMetadata metadata = getUpstreamMetadata(time, view);

    KnobDoublePtr rateKnob = _rate.lock();
    KnobBoolPtr rateFromMetadataKnob = _rateFromMetadata.lock();
    KnobIntPtr startFrameKnob = _startFrame.lock();
    KnobBoolPtr useStartFrameKnob = _useStartFrame.lock();
    KnobStringPtr startTimecodeKnob = _startTimecode.lock();

    double rate = rateKnob ? rateKnob->getValueAtTime(time, 0, view) : 24.;

    if (rateFromMetadataKnob && rateFromMetadataKnob->getValueAtTime(time, 0, view)) {
        const std::optional<double> upstreamRate = metadata.getDouble("ofx/framerate");
        if (upstreamRate) {
            rate = *upstreamRate;
        }
    }

    // The start code is read at the rate the frames field counts at, so a rate taken from
    // Source moves the timecode itself, not only the rate published with it.
    const int counted = roundedRate(rate);
    const bool useStartFrame = useStartFrameKnob && useStartFrameKnob->getValueAtTime(time, 0, view);
    const double origin = useStartFrame && startFrameKnob ? (double)startFrameKnob->getValueAtTime(time, 0, view) : (double)kFixedOrigin;
    const std::string startTimecode = startTimecodeKnob ? startTimecodeKnob->getValueAtTime(time, 0, view) : std::string();
    const long long offset = (long long)std::floor(time - origin + 0.5);

    metadata.setString("ofx/timecode", framesToTimecode(timecodeToFrames(startTimecode, counted) + offset, counted));
    metadata.setDouble("ofx/framerate", rate);

    return metadata;
}

NATRON_NAMESPACE_EXIT
