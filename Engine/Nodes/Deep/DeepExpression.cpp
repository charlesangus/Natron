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

#include "DeepExpression.h"

#include <map>
#include <string>
#include <vector>

#include "Engine/DeepImage.h"
#include "Engine/DeepPixelOps.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Deep/DeepExpressionEvaluator.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

namespace {
const char* const kOutputChannels[] = { "R", "G", "B", "A", "Z", "ZBack" };
const std::size_t kOutputChannelCount = sizeof(kOutputChannels) / sizeof(kOutputChannels[0]);

bool
isBlank(const std::string& s)
{
    return s.find_first_not_of(" \t\r\n") == std::string::npos;
}

bool
isDepthChannel(const std::string& name)
{
    return (name == "Z") || (name == "ZBack");
}
} // anonymous namespace

NativePluginDescription
DeepExpression::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_DEEPEXPRESSION;
    desc.label = "DeepExpression";
    desc.description = tr("Rewrite the R, G, B, A, Z and ZBack channels of the deep input, each "
                          "from its own expression evaluated once per sample. An expression reads "
                          "the input's channels by name (R, G, B, A, Z, ZBack and any AOV), the "
                          "pixel's x and y, the sample's sampleIndex and its pixel's sampleCount, "
                          "frame and pi, with the usual arithmetic, comparison, logical and "
                          "conditional operators and the functions abs, floor, ceil, round, sqrt, "
                          "exp, log, pow, min, max, clamp, lerp, step, smoothstep, sin, cos, tan "
                          "and atan2. A channel whose expression is empty is passed through.")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_DEEP;
    desc.majorVersion = 1;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("Source", false, eDataKindDeep));
    desc.outputKind = eDataKindDeep;

    return desc;
}

void
DeepExpression::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    for (std::size_t c = 0; c < kOutputChannelCount; ++c) {
        KnobStringPtr knob = createKnob<KnobString>(QString::fromUtf8(kOutputChannels[c]));
        knob->setName(std::string("expression") + kOutputChannels[c]);
        knob->setHintToolTip(tr("The expression giving every sample's %1, or nothing to leave the channel as it is.").arg(QString::fromUtf8(kOutputChannels[c])));
        page->addKnob(knob);
        _expressions.push_back(knob);
    }
}

void
DeepExpression::getExpressions(double time,
                               std::vector<std::string>* channels,
                               std::vector<std::string>* expressions) const
{
    for (std::size_t c = 0; c < _expressions.size(); ++c) {
        KnobStringPtr knob = _expressions[c].lock();

        if (!knob) {
            continue;
        }
        const std::string expression = knob->getValueAtTime(time);
        if (isBlank(expression)) {
            continue;
        }
        channels->push_back(kOutputChannels[c]);
        expressions->push_back(expression);
    }
}

StatusEnum
DeepExpression::getRegionOfDefinition(U64 hash,
                                      double time,
                                      const RenderScale& scale,
                                      ViewIdx view,
                                      RectD* rod)
{
    EffectInstancePtr input = getInput(0);

    if (!input) {
        return eStatusReplyDefault;
    }
    bool isProjectFormat = false;

    return input->getRegionOfDefinition_public(hash, time, scale, view, rod, &isProjectFormat);
}

bool
DeepExpression::isIdentity(double time,
                           const RenderScale& /*scale*/,
                           const RectI& /*roi*/,
                           ViewIdx view,
                           double* inputTime,
                           ViewIdx* inputView,
                           int* inputNb)
{
    std::vector<std::string> channels;
    std::vector<std::string> expressions;

    getExpressions(time, &channels, &expressions);
    if (!channels.empty()) {
        return false;
    }

    // Every expression is blank, so renderDeep() -- and its success-path clearPersistentMessage()
    // -- will never run again for this identity: a compile error left over from a previous,
    // non-blank set of expressions would otherwise stay displayed on a now-valid node forever.
    clearPersistentMessage(false);

    *inputTime = time;
    *inputNb = 0;
    *inputView = view;

    return true;
}

StatusEnum
DeepExpression::renderDeep(const DeepRenderActionArgs& args)
{
    const DeepImagePtr input = getInput(0) ? args.getInputDeepImage(0) : DeepImagePtr();

    if (!input) {
        setPersistentMessage(eMessageTypeError, tr("No deep data to evaluate: nothing is connected to Source.").toStdString());

        return eStatusFailed;
    }

    // The names of the channels a rewrite's input view carries, in its order.
    std::vector<std::string> inputChannelNames;
    for (std::map<std::string, DeepChannelBuffer>::const_iterator it = input->getChannels().begin(); it != input->getChannels().end(); ++it) {
        if (!isDepthChannel(it->first)) {
            inputChannelNames.push_back(it->first);
        }
    }

    std::vector<std::string> channelsToWrite;
    std::vector<std::string> sources;
    getExpressions(args.time, &channelsToWrite, &sources);

    std::vector<DeepExpressionEvaluator> programs(channelsToWrite.size());
    int alphaChannelIndex = -1;
    bool writesDepth = false;
    for (std::size_t c = 0; c < channelsToWrite.size(); ++c) {
        DeepExpressionEvaluator::CompileError error;
        if (!programs[c].compile(sources[c], inputChannelNames, &error)) {
            setPersistentMessage(eMessageTypeError, channelsToWrite[c] + ": " + error.message + " (column " + std::to_string(error.column) + ")");

            return eStatusFailed;
        }
        if (channelsToWrite[c] == "A") {
            alphaChannelIndex = (int)c;
        }
        writesDepth = writesDepth || isDepthChannel(channelsToWrite[c]);
    }
    if (channelsToWrite.empty()) {
        return args.outputDeepImage->aliasContentsOf(*input) ? eStatusOK : eStatusFailed;
    }

    const float frame = (float)args.time;

    clearPersistentMessage(false);

    const StatusEnum status = renderDeepFromInput(args, input, channelsToWrite, alphaChannelIndex, [&programs, frame](int x, int y, const DeepPixelView& in, const MutableDeepPixelView& out) {
        for (std::size_t c = 0; c < programs.size(); ++c) {
            float* const dst = out.channels[c];
            for (int s = 0; s < in.numSamples; ++s) {
                dst[s] = programs[c].evaluate(in, s, x, y, frame);
            }
        } });
    if ((status == eStatusOK) && writesDepth) {
        args.outputDeepImage->setTidy(false);
    }

    return status;
} // DeepExpression::renderDeep

NATRON_NAMESPACE_EXIT
