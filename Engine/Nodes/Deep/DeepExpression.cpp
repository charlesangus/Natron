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

#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/DeepImage.h"
#include "Engine/DeepLayers.h"
#include "Engine/DeepPixelOps.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Deep/DeepExpressionEvaluator.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/TimeLine.h"

NATRON_NAMESPACE_ENTER

namespace {
const char* const kDefaultSlotLabels[kDeepExpressionLayerSlotCount] = { "R", "G", "B", "A" };

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

void
addExpression(const KnobStringWPtr& weakKnob,
              double time,
              const std::string& channel,
              std::vector<std::string>* channels,
              std::vector<std::string>* expressions)
{
    KnobStringPtr knob = weakKnob.lock();

    if (!knob) {
        return;
    }
    const std::string expression = knob->getValueAtTime(time);
    if (isBlank(expression)) {
        return;
    }
    channels->push_back(channel);
    expressions->push_back(expression);
}
} // anonymous namespace

NativePluginDescription
DeepExpression::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_DEEPEXPRESSION;
    desc.label = "DeepExpression";
    desc.description = tr("Rewrite the channels of one layer of the deep input, and the Z and ZBack "
                          "depths, each from its own expression evaluated once per sample. Layer "
                          "picks the layer, and the expression fields below it take that layer's "
                          "channel names. An expression reads the input's channels by name (R, G, B, "
                          "A, Z, ZBack and any AOV, such as diffuse.R), the pixel's x and y, the "
                          "sample's sampleIndex and its pixel's sampleCount, frame and pi, with the "
                          "usual arithmetic, comparison, logical and conditional operators and the "
                          "functions abs, floor, ceil, round, sqrt, exp, log, pow, min, max, clamp, "
                          "lerp, step, smoothstep, sin, cos, tan and atan2. A channel whose "
                          "expression is empty is passed through. Writing a colour channel the input "
                          "lacks creates it; a layer the input lacks at a frame is an error there.")
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

    KnobLayerSelectPtr layer = createKnob<KnobLayerSelect>(tr("Layer"));
    layer->setName(kDeepExpressionParamLayer);
    layer->setWithChannelButtons(false);
    layer->setAllowNone(false);
    layer->setAnimationEnabled(false);
    layer->setIsMetadataSlave(true);
    layer->setDefaultValue(layer->encode(kNatronColorViewRGBA, std::vector<std::string>()));
    layer->setHintToolTip(tr("The layer of the Source whose channels the expression fields below rewrite, "
                             "one field per channel. A colour view's channels are R, G, B and A; writing "
                             "one the input lacks creates it."));
    page->addKnob(layer);
    _layer = layer;

    for (int i = 0; i < kDeepExpressionLayerSlotCount; ++i) {
        KnobStringPtr knob = createKnob<KnobString>(QString::fromUtf8(kDefaultSlotLabels[i]));
        knob->setName(std::string(kDeepExpressionParamExpressionPrefix) + std::to_string(i));
        knob->setIsMetadataSlave(true);
        knob->setHintToolTip(tr("The expression giving every sample's value of this channel of Layer, or "
                                "nothing to leave the channel as it is."));
        page->addKnob(knob);
        _layerExpressions.push_back(knob);
    }

    KnobStringPtr z = createKnob<KnobString>(QString::fromUtf8("Z"));
    z->setName(kDeepExpressionParamExpressionZ);
    z->setHintToolTip(tr("The expression giving every sample's front depth Z, or nothing to leave it as it is."));
    page->addKnob(z);
    _expressionZ = z;

    KnobStringPtr zBack = createKnob<KnobString>(QString::fromUtf8("ZBack"));
    zBack->setName(kDeepExpressionParamExpressionZBack);
    zBack->setHintToolTip(tr("The expression giving every sample's back depth ZBack, or nothing to leave it as it is."));
    page->addKnob(zBack);
    _expressionZBack = zBack;

    NodePtr node = getNode();
    if (node) {
        node->declareLayerKnob(layer, 0, LayerKnobSpec::eRoleInputBound);
    }
}

bool
DeepExpression::resolveLayerChannels(double time,
                                     ViewIdx view,
                                     std::vector<std::string>* names,
                                     std::vector<std::string>* labels)
{
    KnobLayerSelectPtr layer = _layer.lock();

    if (!layer) {
        return false;
    }
    const std::string layerID = layer->getLayer();

    if (ImageLayerDesc::isColorViewID(layerID)) {
        const std::vector<std::string>& viewChannels = ImageLayerDesc::getColorView(layerID).getChannels();
        const ImageLayerDesc& rgba = ImageLayerDesc::getRGBAComponents();
        for (int i = 0; i < kDeepExpressionLayerSlotCount; ++i) {
            const int bit = ImageLayerDesc::colorViewChannelBit(layerID, i);
            if (bit < 0) {
                break;
            }
            names->push_back(DeepLayers::channelName(rgba, bit));
            labels->push_back((i < (int)viewChannels.size()) ? viewChannels[i] : names->back());
        }

        return true;
    }

    if (!getInput(0)) {
        return false;
    }
    std::list<ImageLayerDesc> present;
    getPresentLayers(time, view, 0, &present);
    ResolvedLayer resolved;
    if (!layer->resolve(present, &resolved)) {
        return false;
    }
    const std::vector<std::string>& channels = resolved.desc.getChannels();
    for (int i = 0; (i < kDeepExpressionLayerSlotCount) && (i < resolved.desc.getNumComponents()); ++i) {
        const std::string name = DeepLayers::channelName(resolved.desc, i);
        if (name.empty()) {
            break;
        }
        names->push_back(name);
        labels->push_back((i < (int)channels.size()) ? channels[i] : name);
    }

    return true;
}

void
DeepExpression::getExpressions(double time,
                               ViewIdx view,
                               std::vector<std::string>* channels,
                               std::vector<std::string>* expressions,
                               bool* layerMissing)
{
    *layerMissing = false;

    std::vector<std::string> slotExpressions(_layerExpressions.size());
    bool anySlot = false;
    for (std::size_t i = 0; i < _layerExpressions.size(); ++i) {
        KnobStringPtr knob = _layerExpressions[i].lock();
        if (!knob) {
            continue;
        }
        const std::string expression = knob->getValueAtTime(time);
        if (!isBlank(expression)) {
            slotExpressions[i] = expression;
            anySlot = true;
        }
    }

    if (anySlot) {
        std::vector<std::string> names;
        std::vector<std::string> labels;
        if (resolveLayerChannels(time, view, &names, &labels)) {
            for (std::size_t i = 0; (i < names.size()) && (i < slotExpressions.size()); ++i) {
                if (!slotExpressions[i].empty()) {
                    channels->push_back(names[i]);
                    expressions->push_back(slotExpressions[i]);
                }
            }
        } else {
            *layerMissing = true;
        }
    }

    addExpression(_expressionZ, time, "Z", channels, expressions);
    addExpression(_expressionZBack, time, "ZBack", channels, expressions);
}

void
DeepExpression::refreshExpressionLabels()
{
    AppInstancePtr app = getApp();
    const double time = app ? app->getTimeLine()->currentFrame() : 0.;
    std::vector<std::string> names;
    std::vector<std::string> labels;

    if (!resolveLayerChannels(time, ViewIdx(0), &names, &labels)) {
        labels.clear();
        // The Source lacks the layer at this frame, so the project's definition of it is the
        // best guess at the channels the fields will write once it is there.
        KnobLayerSelectPtr layer = _layer.lock();
        ProjectPtr project = app ? app->getProject() : ProjectPtr();
        ImageLayerDesc desc;
        if (layer && project && project->findLayer(layer->getLayer(), &desc)) {
            const std::vector<std::string>& channels = desc.getChannels();
            for (std::size_t i = 0; (i < channels.size()) && (i < (std::size_t)kDeepExpressionLayerSlotCount); ++i) {
                labels.push_back(channels[i]);
            }
        }
        if (labels.empty()) {
            for (int i = 0; i < kDeepExpressionLayerSlotCount; ++i) {
                labels.push_back(std::to_string(i + 1));
            }
        }
    }

    for (std::size_t i = 0; i < _layerExpressions.size(); ++i) {
        KnobStringPtr knob = _layerExpressions[i].lock();
        if (!knob) {
            continue;
        }
        if (i < labels.size()) {
            knob->setLabel(labels[i]);
        }
        knob->setSecret(i >= labels.size());
    }
}

bool
DeepExpression::knobChanged(KnobI* k,
                            ValueChangedReasonEnum /*reason*/,
                            ViewSpec /*view*/,
                            double /*time*/,
                            bool /*originatedFromMainThread*/)
{
    KnobLayerSelectPtr layer = _layer.lock();

    if (layer && (k == layer.get())) {
        refreshExpressionLabels();

        return true;
    }

    return false;
}

void
DeepExpression::onKnobsLoaded()
{
    refreshExpressionLabels();
}

void
DeepExpression::onChannelsSelectorRefreshed()
{
    refreshExpressionLabels();
}

void
DeepExpression::getDeepLayers(double time,
                              ViewIdx view,
                              std::list<ImageLayerDesc>* layers)
{
    if (!getInput(0)) {
        return;
    }

    std::list<ImageLayerDesc> present;
    getPresentLayers(time, view, 0, &present);

    std::vector<std::string> names;
    std::set<std::string> taken;
    for (std::list<ImageLayerDesc>::const_iterator it = present.begin(); it != present.end(); ++it) {
        for (int c = 0; c < it->getNumComponents(); ++c) {
            const std::string name = DeepLayers::channelName(*it, c);
            if (!name.empty() && taken.insert(name).second) {
                names.push_back(name);
            }
        }
    }

    std::vector<std::string> written;
    std::vector<std::string> expressions;
    bool layerMissing = false;
    getExpressions(time, view, &written, &expressions, &layerMissing);
    for (std::size_t i = 0; i < written.size(); ++i) {
        if (!isDepthChannel(written[i]) && taken.insert(written[i]).second) {
            names.push_back(written[i]);
        }
    }

    DeepLayers::groupDeepChannels(names, layers);
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
    bool layerMissing = false;

    getExpressions(time, view, &channels, &expressions, &layerMissing);
    if (!channels.empty() || layerMissing) {
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
    bool layerMissing = false;
    getExpressions(args.time, args.view, &channelsToWrite, &sources, &layerMissing);
    if (layerMissing) {
        KnobLayerSelectPtr layer = _layer.lock();
        setPersistentMessage(eMessageTypeError, "Layer " + (layer ? layer->getLayer() : std::string()) + " is not in the " + getInputLabel(0) + " input");

        return eStatusFailed;
    }

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
