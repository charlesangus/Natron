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

#include "Merge.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cstddef>
#include <functional>
#include <memory>
#include <sstream>
#include <vector>

#include <ofxNatron.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/PoolParallelFor.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

namespace {
// How many rows render() runs between two abort checks.
const int kAbortCheckRows = 16;

const char* const kChannelSuffixes[4] = { "R", "G", "B", "A" };

// The index in an nComps-channel pixel of the channel on colour bit `bit`, or -1.
int
channelIndexForBit(int nComps,
                   int bit)
{
    if (nComps == 1) {
        return (bit == 3) ? 0 : -1;
    }

    return (bit < nComps) ? bit : -1;
}

// The bits processed in a dstNComps-channel image holding `plane`. A one-channel plane rendered
// into a wider image sits where EffectInstance's copy back reads it: alpha for a colour plane,
// channel 0 otherwise.
std::bitset<4>
processedBitsForImage(const ImageLayerDesc& plane,
                      int dstNComps,
                      const std::bitset<4>& planeBits)
{
    if ((plane.getNumComponents() != 1) || (dstNComps == 1)) {
        return planeBits;
    }
    std::bitset<4> bits;
    bits[plane.isColorLayer() ? 3 : 0] = planeBits[3];

    return bits;
}

// The rectangle helpers of openfx-misc's ofxsCoords.h, whose empty-rectangle and touching-edge
// conventions the region of definition and identity rules depend on.
template <typename Rect>
bool
rectIsEmpty(const Rect& r)
{
    return (r.x2 <= r.x1) || (r.y2 <= r.y1);
}

template <typename Rect>
void
rectBoundingBox(const Rect& a,
                const Rect& b,
                Rect* bbox)
{
    if (rectIsEmpty(a)) {
        *bbox = b;

        return;
    }
    if (rectIsEmpty(b)) {
        *bbox = a;

        return;
    }
    bbox->x1 = (std::min)(a.x1, b.x1);
    bbox->x2 = (std::max)(bbox->x1, (std::max)(a.x2, b.x2));
    bbox->y1 = (std::min)(a.y1, b.y1);
    bbox->y2 = (std::max)(bbox->y1, (std::max)(a.y2, b.y2));
}

template <typename Rect>
bool
rectIntersection(const Rect& r1,
                 const Rect& r2,
                 Rect* intersection)
{
    if (rectIsEmpty(r1) || rectIsEmpty(r2) || (r1.x1 > r2.x2) || (r2.x1 > r1.x2) || (r1.y1 > r2.y2) || (r2.y1 > r1.y2)) {
        if (intersection) {
            intersection->x1 = 0;
            intersection->x2 = 0;
            intersection->y1 = 0;
            intersection->y2 = 0;
        }

        return false;
    }
    if (intersection) {
        const Rect a = r1;
        const Rect b = r2;
        intersection->x1 = (std::max)(a.x1, b.x1);
        intersection->x2 = (std::max)(intersection->x1, (std::min)(a.x2, b.x2));
        intersection->y1 = (std::max)(a.y1, b.y1);
        intersection->y2 = (std::max)(intersection->y1, (std::min)(a.y2, b.y2));
    }

    return true;
}

RectI
toPixelEnclosing(const RectD& rod,
                 const RenderScale& scale,
                 double par)
{
    if (rectIsEmpty(rod)) {
        return RectI();
    }

    return rod.toPixelEnclosing(scale.toMipmapLevel(), par);
}

// One value per pixel of `channel` of row y, zero wherever the image has no pixel.
void
readChannelRow(const Image* image,
               const Image::ReadAccess* access,
               const RectI& bounds,
               int channel,
               int x0,
               int y,
               int width,
               float* row,
               unsigned char* present)
{
    std::fill(row, row + width, 0.f);
    std::fill(present, present + width, (unsigned char)0);
    if (!image) {
        return;
    }
    if ((y < bounds.y1) || (y >= bounds.y2)) {
        return;
    }
    const int xStart = (std::max)(x0, bounds.x1);
    const int xEnd = (std::min)(x0 + width, bounds.x2);
    if (xStart >= xEnd) {
        return;
    }
    const int nComps = (int)image->getComponentsCount();
    const float* pix = (const float*)access->pixelAt(xStart, y);
    for (int x = xStart; x < xEnd; ++x, pix += nComps) {
        row[x - x0] = pix[channel];
        present[x - x0] = 1;
    }
}

// Image::getBounds() takes the image's lock, which a band thread must never ask for: behind a
// writer waiting on an image this render holds for reading, it blocks forever. The bounds are
// read once on the calling thread instead.
struct MergeSource {
    ImagePtr image;
    std::shared_ptr<Image::ReadAccess> access;
    RectI bounds;
};

// Row y of `source` over [x0, x0 + width), nComps floats per pixel: a pointer into the image
// itself when it has the whole row in that layout and `keep` keeps every channel, and otherwise
// a copy in `row` that is zero in the channels `keep` (indexed by pixel channel) turns off and
// wherever the image has no pixel. An image in another layout is mapped channel by colour bit, a
// channel it lacks reading as zero. *present is null when the image has every pixel of the row,
// and otherwise points at presentRow, which flags the pixels it has.
const float*
fetchRow(const MergeSource& source,
         int x0,
         int y,
         int width,
         int nComps,
         const std::bitset<4>& keep,
         float* row,
         unsigned char* presentRow,
         const unsigned char** present)
{
    const std::size_t rowSize = (std::size_t)width * nComps;
    int xStart = x0;
    int xEnd = x0;
    if (source.image && (y >= source.bounds.y1) && (y < source.bounds.y2)) {
        xStart = (std::max)(x0, source.bounds.x1);
        xEnd = (std::min)(x0 + width, source.bounds.x2);
    }
    if (xStart >= xEnd) {
        std::fill(row, row + rowSize, 0.f);
        std::fill(presentRow, presentRow + width, (unsigned char)0);
        *present = presentRow;

        return row;
    }
    const bool wholeRow = (xStart == x0) && (xEnd == x0 + width);
    if (wholeRow) {
        *present = 0;
    } else {
        std::fill(presentRow, presentRow + width, (unsigned char)0);
        std::fill(presentRow + (xStart - x0), presentRow + (xEnd - x0), (unsigned char)1);
        *present = presentRow;
    }
    const int srcNComps = (int)source.image->getComponentsCount();
    bool keepAll = true;
    int srcIndex[4] = { -1, -1, -1, -1 };
    for (int c = 0; c < nComps; ++c) {
        keepAll = keepAll && keep[c];
        if (keep[c]) {
            srcIndex[c] = channelIndexForBit(srcNComps, pixelKernelChannelBit(nComps, c));
        }
    }
    const float* srcPix = (const float*)source.access->pixelAt(xStart, y);
    if (wholeRow && keepAll && (srcNComps == nComps)) {
        return srcPix;
    }
    std::fill(row, row + (std::size_t)(xStart - x0) * nComps, 0.f);
    std::fill(row + (std::size_t)(xEnd - x0) * nComps, row + rowSize, 0.f);
    float* dstPix = row + (std::size_t)(xStart - x0) * nComps;
    if (keepAll && (srcNComps == nComps)) {
        std::copy(srcPix, srcPix + (std::size_t)(xEnd - xStart) * nComps, dstPix);

        return row;
    }
    for (int x = xStart; x < xEnd; ++x, srcPix += srcNComps, dstPix += nComps) {
        for (int c = 0; c < nComps; ++c) {
            dstPix[c] = (srcIndex[c] >= 0) ? srcPix[srcIndex[c]] : 0.f;
        }
    }

    return row;
}

struct MergePlaneJob {
    ImagePtr dst;
    std::shared_ptr<Image::WriteAccess> dstAccess;
    std::bitset<4> channels;
    MergeSource b;
    std::vector<MergeSource> as;
    // The image the channels left unprocessed are copied from, the way the host copies them
    // from the preferred input for an effect that does not: -1 none, 0 B, i + 1 the i-th A.
    int passSource;

    MergePlaneJob()
        : dst()
        , dstAccess()
        , channels()
        , b()
        , as()
        , passSource(-1)
    {
    }
};

struct MergeRowBand {
    std::size_t job;
    int y1;
    int y2;

    MergeRowBand(std::size_t jobIndex,
                 int firstRow,
                 int endRow)
        : job(jobIndex)
        , y1(firstRow)
        , y2(endRow)
    {
    }
};

bool
isFloatImage(const ImagePtr& image)
{
    return !image || (image->getBitDepth() == eImageBitDepthFloat);
}

bool
readBool(const KnobBoolWPtr& weak,
         double time,
         ViewIdx view,
         bool defaultValue)
{
    KnobBoolPtr knob = weak.lock();

    return knob ? knob->getValueAtTime(time, 0, view) : defaultValue;
}

KnobBoolPtr
addChannelToggle(KnobHolder* holder,
                 const KnobPagePtr& page,
                 const std::string& prefix,
                 int channel,
                 const std::string& hint)
{
    KnobBoolPtr knob = AppManager::createKnob<KnobBool>(holder, std::string(kChannelSuffixes[channel]));

    knob->setName(prefix + kChannelSuffixes[channel]);
    knob->setHintToolTip(hint);
    knob->setDefaultValue(true);
    if (channel < 3) {
        knob->setAddNewLine(false);
        knob->setSpacingBetweenItems(1);
    }
    page->addKnob(knob);

    return knob;
}

// The divider OpenFX hosts draw under a knob whose layout hint asks for one, with the name the
// OpenFX host gives it.
void
addSeparatorAfter(KnobHolder* holder,
                  const KnobPagePtr& page,
                  const std::string& knobName)
{
    KnobSeparatorPtr separator = AppManager::createKnob<KnobSeparator>(holder, std::string());

    separator->setName(knobName + "_separator");
    page->addKnob(separator);
}

KnobStringPtr
addRowLabel(KnobHolder* holder,
            const KnobPagePtr& page,
            const std::string& name,
            const std::string& text,
            const std::string& hint)
{
    KnobStringPtr knob = AppManager::createKnob<KnobString>(holder, std::string());

    knob->setName(name);
    knob->setHintToolTip(hint);
    knob->setDefaultValue(text);
    knob->setAsLabel();
    knob->setAllDimensionsEnabled(false);
    knob->setAddNewLine(false);
    knob->setSpacingBetweenItems(1);
    page->addKnob(knob);

    return knob;
}

// Built once: the description is asked for on every plug-in ID or label query.
const std::string&
mergeDescription()
{
    static const std::string description = []() {
        std::string help = "Pixel-by-pixel merge operation between two or more inputs.\n"
                           "Input A is first merged with B (or with a black and transparent background if B is not connected), "
                           "then A2, if connected, is merged with the intermediary result, then A3, etc.\n\n"
                           "Note that if an input with only RGB components is connected to A or B, its alpha channel "
                           "is considered to be opaque (one) by default, thus the output will be completely opaque if "
                           "the checkbox for channel A of input B is checked.\n\n"
                           "Operators:\n";
        for (int op = 0; op < MergeOperators::eOperationCount; ++op) {
            help += std::string("\n- ") + MergeOperators::operationId((MergeOperators::Operation)op) + ": " + MergeOperators::operationHint((MergeOperators::Operation)op);
        }

        return help;
    }();

    return description;
}
} // anonymous namespace

MergeNode::MergeNode(NodePtr node,
                     MergePresetEnum preset)
    : NativeImageEffect(node)
    , _preset(preset)
{
}

MergeNode::~MergeNode()
{
}

std::string
MergeNode::presetPluginID(MergePresetEnum preset)
{
    switch (preset) {
    case eMergePresetMerge:
        return PLUGINID_NATRON_MERGE;
    case eMergePresetPlus:
        return PLUGINID_NATRON_MERGE_PREFIX "Plus";
    case eMergePresetMatte:
        return PLUGINID_NATRON_MERGE_PREFIX "Matte";
    case eMergePresetMultiply:
        return PLUGINID_NATRON_MERGE_PREFIX "Multiply";
    case eMergePresetIn:
        return PLUGINID_NATRON_MERGE_PREFIX "In";
    case eMergePresetOut:
        return PLUGINID_NATRON_MERGE_PREFIX "Out";
    case eMergePresetScreen:
        return PLUGINID_NATRON_MERGE_PREFIX "Screen";
    case eMergePresetMax:
        return PLUGINID_NATRON_MERGE_PREFIX "Max";
    case eMergePresetMin:
        return PLUGINID_NATRON_MERGE_PREFIX "Min";
    case eMergePresetDifference:
        return PLUGINID_NATRON_MERGE_PREFIX "Difference";
    }

    return PLUGINID_NATRON_MERGE;
}

MergeOperators::Operation
MergeNode::presetOperation(MergePresetEnum preset)
{
    switch (preset) {
    case eMergePresetMerge:
        return MergeOperators::eOver;
    case eMergePresetPlus:
        return MergeOperators::ePlus;
    case eMergePresetMatte:
        return MergeOperators::eMatte;
    case eMergePresetMultiply:
        return MergeOperators::eMultiply;
    case eMergePresetIn:
        return MergeOperators::eIn;
    case eMergePresetOut:
        return MergeOperators::eOut;
    case eMergePresetScreen:
        return MergeOperators::eScreen;
    case eMergePresetMax:
        return MergeOperators::eMax;
    case eMergePresetMin:
        return MergeOperators::eMin;
    case eMergePresetDifference:
        return MergeOperators::eDifference;
    }

    return MergeOperators::eOver;
}

int
MergeNode::aInputIndex(int i)
{
    if (i == 0) {
        return kMergeInputA;
    }
    if ((i < 0) || (i >= kMergeMaxAInputs)) {
        return -1;
    }

    return kMergeInputMask + i;
}

ImagePtr
MergeNode::sourceDetachedFromOutputs(const ImagePtr& source,
                                     const std::vector<ImagePtr>& outputs)
{
    if (!source || (std::find(outputs.begin(), outputs.end(), source) == outputs.end())) {
        return source;
    }
    const RectI bounds = source->getBounds();
    ImagePtr copy = std::make_shared<Image>(source->getComponents(), source->getRoD(), bounds, source->getMipmapLevel(), source->getPixelAspectRatio(),
                                            source->getBitDepth(), source->getFieldingOrder(), false /*useBitmap*/, eStorageModeRAM);
    copy->pasteFrom(*source, bounds, false /*copyBitmap*/);

    return copy;
}

std::string
MergeNode::getInputLabel(int inputNb) const
{
    if (inputNb == kMergeInputB) {
        return "B";
    } else if (inputNb == kMergeInputA) {
        return "A";
    } else if (inputNb == kMergeInputMask) {
        return "Mask";
    } else if ((inputNb > kMergeInputMask) && (inputNb < getNInputs())) {
        std::ostringstream label;
        label << "A" << (inputNb - kMergeInputMask + 1);

        return label.str();
    }

    return EffectInstance::getInputLabel(inputNb);
}

std::string
MergeNode::getInputHint(int inputNb) const
{
    if (inputNb == kMergeInputB) {
        return tr("The main input. This input is passed through when the merge node is disabled.").toStdString();
    } else if (inputNb == kMergeInputA) {
        return tr("The image sequence to merge with input B.").toStdString();
    }

    return EffectInstance::getInputHint(inputNb);
}

bool
MergeNode::isInputOptional(int inputNb) const
{
    return (inputNb >= 0) && (inputNb < getNInputs());
}

bool
MergeNode::isInputMask(int inputNb) const
{
    return inputNb == kMergeInputMask;
}

DataKindEnum
MergeNode::getInputDataKind(int /*inputNb*/) const
{
    return eDataKindImage;
}

void
MergeNode::addAcceptedComponents(int inputNb,
                                 std::list<ImageLayerDesc>* comps)
{
    if (isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getAlphaComponents());

        return;
    }
    comps->push_back(ImageLayerDesc::getRGBAComponents());
    comps->push_back(ImageLayerDesc::getRGBComponents());
    comps->push_back(ImageLayerDesc::getXYComponents());
    comps->push_back(ImageLayerDesc::getAlphaComponents());
}

NativePluginDescription
MergeNode::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = presetPluginID(_preset);
    switch (_preset) {
    case eMergePresetMerge:
        desc.label = "Merge";
        break;
    case eMergePresetPlus:
        desc.label = "Plus";
        break;
    case eMergePresetMatte:
        desc.label = "Matte";
        break;
    case eMergePresetMultiply:
        desc.label = "Multiply";
        break;
    case eMergePresetIn:
        desc.label = "In";
        break;
    case eMergePresetOut:
        desc.label = "Out";
        break;
    case eMergePresetScreen:
        desc.label = "Screen";
        break;
    case eMergePresetMax:
        desc.label = "Max";
        break;
    case eMergePresetMin:
        desc.label = "Min";
        break;
    case eMergePresetDifference:
        desc.label = "Absminus";
        break;
    }

    desc.description = mergeDescription();
    desc.grouping = (_preset == eMergePresetMerge) ? std::string(PLUGIN_GROUP_MERGE) : std::string(PLUGIN_GROUP_MERGE "/Merges");
    desc.majorVersion = PLUGIN_MAJOR_NATRON_MERGE;
    desc.minorVersion = 0;
    // No inputs are listed: the input accessors are overridden, because this description is
    // rebuilt on every query and listing 66 inputs each time would make those queries quadratic.
    desc.outputKind = eDataKindImage;

    return desc;
} // MergeNode::getNativePluginDescription

void
MergeNode::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));
    const MergeOperators::Operation defaultOperation = presetOperation(_preset);

    // Node.cpp shows this knob's value in parentheses next to the node's label, and NodeGui
    // draws the operator icon from it.
    KnobStringPtr subLabel = createKnob<KnobString>(std::string(kNatronOfxParamStringSublabelName));
    subLabel->setName(kNatronOfxParamStringSublabelName);
    subLabel->setSecretByDefault(true);
    subLabel->setDefaultAllDimensionsEnabled(false);
    subLabel->setIsPersistent(false);
    subLabel->setEvaluateOnChange(false);
    subLabel->setAnimationEnabled(false);
    subLabel->setDefaultValue(std::string(MergeOperators::operationId(defaultOperation)));
    page->addKnob(subLabel);
    _subLabel = subLabel;

    KnobChoicePtr operation = createKnob<KnobChoice>(tr("Operation"));
    operation->setName(kMergeParamOperation);
    operation->setHintToolTip(tr("The operation used to merge the input A and B images.\n"
                                 "The operator formula is applied to each component: A and B represent the input component "
                                 "(Red, Green, Blue, or Alpha) of each input, and a and b represent the alpha channel of each input.\n"
                                 "If Alpha masking is checked, the output alpha is computed using a different formula (a+b - a*b).\n"
                                 "Alpha masking is always enabled for HSL modes (hue, saturation, color, luminosity)."));
    {
        std::vector<ChoiceOption> options;
        for (int op = 0; op < MergeOperators::eOperationCount; ++op) {
            const MergeOperators::Operation o = (MergeOperators::Operation)op;
            options.push_back(ChoiceOption(MergeOperators::operationId(o), MergeOperators::operationId(o), MergeOperators::operationHint(o)));
        }
        operation->populateChoices(options);
    }
    operation->setDefaultValue((int)defaultOperation);
    operation->setAddNewLine(false);
    operation->setSpacingBetweenItems(1);
    page->addKnob(operation);
    _operation = operation;

    KnobChoicePtr bbox = createKnob<KnobChoice>(tr("Bounding Box"));
    bbox->setName(kMergeParamBBox);
    bbox->setHintToolTip(tr("What to use to produce the output image's bounding box."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kMergeBBoxUnion, tr("Union").toStdString(), tr("Union of all connected inputs.").toStdString()));
        options.push_back(ChoiceOption(kMergeBBoxIntersection, tr("Intersection").toStdString(), tr("Intersection of all connected inputs.").toStdString()));
        options.push_back(ChoiceOption(kMergeBBoxA, tr("A").toStdString(), tr("Bounding box of input A.").toStdString()));
        options.push_back(ChoiceOption(kMergeBBoxB, tr("B").toStdString(), tr("Bounding box of input B.").toStdString()));
        bbox->populateChoices(options);
    }
    bbox->setDefaultValue(0);
    page->addKnob(bbox);
    _bbox = bbox;

    KnobBoolPtr alphaMasking = createKnob<KnobBool>(tr("Alpha masking"));
    alphaMasking->setName(kMergeParamAlphaMasking);
    alphaMasking->setHintToolTip(tr("When enabled, the input images are unchanged where the other image has 0 alpha, and "
                                    "the output alpha is set to a+b - a*b. When disabled the alpha channel is processed as "
                                    "any other channel. Option is disabled for operations where it does not apply or makes no difference."));
    alphaMasking->setDefaultValue(false);
    page->addKnob(alphaMasking);
    _alphaMasking = alphaMasking;
    addSeparatorAfter(this, page, kMergeParamAlphaMasking);

    const std::string channelNames[4] = { "red", "green", "blue", "alpha" };

    addRowLabel(this, page, kMergeParamAChannels, tr("A Channels").toStdString(), tr("Channels to use from A input(s) (other channels are set to zero).").toStdString());
    for (int c = 0; c < 4; ++c) {
        _aChannels[c] = addChannelToggle(this, page, kMergeParamAChannels, c, tr("Use %1 channel from A input(s).").arg(QString::fromUtf8(channelNames[c].c_str())).toStdString());
    }
    addRowLabel(this, page, kMergeParamBChannels, tr("B Channels").toStdString(), tr("Channels to use from B input (other channels are set to zero).").toStdString());
    for (int c = 0; c < 4; ++c) {
        _bChannels[c] = addChannelToggle(this, page, kMergeParamBChannels, c, tr("Use %1 channel from B input.").arg(QString::fromUtf8(channelNames[c].c_str())).toStdString());
    }
    addRowLabel(this, page, kMergeParamOutputChannels, tr("Output").toStdString(), tr("Channels from result to write to output (other channels are taken from B input).").toStdString());
    for (int c = 0; c < 4; ++c) {
        _outputChannels[c] = addChannelToggle(this, page, kMergeParamOutputChannels, c, tr("Write %1 channel to output.").arg(QString::fromUtf8(channelNames[c].c_str())).toStdString());
    }
    addSeparatorAfter(this, page, std::string(kMergeParamOutputChannels) + kChannelSuffixes[3]);

    addMaskMixKnobs(page);

    refreshOperationDependents(0.);
} // MergeNode::initializeKnobs

MergeOperators::Operation
MergeNode::getOperation(double time,
                        ViewIdx view) const
{
    KnobChoicePtr operation = _operation.lock();
    const int value = operation ? operation->getValueAtTime(time, 0, view) : (int)presetOperation(_preset);

    if ((value < 0) || (value >= MergeOperators::eOperationCount)) {
        return presetOperation(_preset);
    }

    return (MergeOperators::Operation)value;
}

void
MergeNode::refreshOperationDependents(double time)
{
    const MergeOperators::Operation op = getOperation(time, ViewIdx(0));
    KnobBoolPtr alphaMasking = _alphaMasking.lock();

    if (alphaMasking) {
        alphaMasking->setAllDimensionsEnabled(MergeOperators::isMaskable(op));
    }
    KnobStringPtr subLabel = _subLabel.lock();
    if (subLabel) {
        subLabel->setValue(std::string(MergeOperators::operationId(op)));
    }
}

bool
MergeNode::knobChanged(KnobI* k,
                       ValueChangedReasonEnum /*reason*/,
                       ViewSpec /*view*/,
                       double time,
                       bool /*originatedFromMainThread*/)
{
    KnobChoicePtr operation = _operation.lock();

    if (!operation || (k != operation.get())) {
        return false;
    }
    refreshOperationDependents(time);

    return true;
}

void
MergeNode::onKnobsLoaded()
{
    refreshOperationDependents(getCurrentTime());
}

ImageMetadata
MergeNode::deriveOutputMetadata(double time,
                                ViewIdx view)
{
    for (int i = 0; i < kMergeMaxAInputs; ++i) {
        const int inputNb = aInputIndex(i);
        if (getInput(inputNb)) {
            return getInputMetadata(inputNb, time, view);
        }
    }

    return getInputMetadata(kMergeInputB, time, view);
}

StatusEnum
MergeNode::getPreferredMetadata(NodeMetadata& metadata)
{
    // The output always has an alpha channel: an RGB B is transparent outside A, so A and B in
    // RGB still give RGBA. The inputs are read in the output's layout.
    int nComps = metadata.getNComps(-1);

    if (nComps == 3) {
        nComps = 4;
    }
    metadata.setNComps(-1, nComps);
    for (int i = 0; i < getNInputs(); ++i) {
        if (!isInputMask(i)) {
            metadata.setNComps(i, nComps);
        }
    }

    return eStatusOK;
}

bool
MergeNode::getInputRoD(int inputNb,
                       double time,
                       const RenderScale& scale,
                       ViewIdx view,
                       RectD* rod) const
{
    EffectInstancePtr input = getInput(inputNb);

    if (!input) {
        return false;
    }
    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    bool isProjectFormat = false;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, inputScale, view, rod, &isProjectFormat) == eStatusFailed) {
        // A failed region is how an input with nothing to show answers: an empty one.
        *rod = RectD();
    }

    return true;
}

StatusEnum
MergeNode::getRegionOfDefinition(U64 /*hash*/,
                                 double time,
                                 const RenderScale& scale,
                                 ViewIdx view,
                                 RectD* rod)
{
    bool found = false;

    if (getMixValue(time, view) == 0.) {
        found = getInputRoD(kMergeInputB, time, scale, view, rod);
    } else {
        KnobChoicePtr bboxKnob = _bbox.lock();
        const int bbox = bboxKnob ? bboxKnob->getValueAtTime(time, 0, view) : 0;
        if ((bbox == 0) || (bbox == 1)) {
            std::vector<RectD> rods;
            RectD inputRoD;
            if (getInputRoD(kMergeInputB, time, scale, view, &inputRoD)) {
                rods.push_back(inputRoD);
            }
            for (int i = 0; i < kMergeMaxAInputs; ++i) {
                if (getInputRoD(aInputIndex(i), time, scale, view, &inputRoD)) {
                    rods.push_back(inputRoD);
                }
            }
            if (!rods.empty()) {
                found = true;
                *rod = rods[0];
                for (std::size_t i = 1; i < rods.size(); ++i) {
                    if (bbox == 0) {
                        rectBoundingBox(*rod, rods[i], rod);
                    } else {
                        rectIntersection(*rod, rods[i], rod);
                    }
                }
            }
        } else if (bbox == 2) {
            found = getInputRoD(kMergeInputA, time, scale, view, rod);
        } else {
            found = getInputRoD(kMergeInputB, time, scale, view, rod);
        }
    }

    if (!found) {
        // OpenFX's default region in the general context when every clip is optional: the
        // project extent, from the origin.
        Format format;
        getApp()->getProject()->getProjectDefaultFormat(&format);
        const RectD canonical = format.toCanonical_noClipping(0, format.getPixelAspectRatio());
        *rod = RectD(0., 0., canonical.right(), canonical.top());
    }

    return eStatusOK;
} // MergeNode::getRegionOfDefinition

bool
MergeNode::isIdentity(double time,
                      const RenderScale& scale,
                      const RectI& roi,
                      ViewIdx view,
                      double* inputTime,
                      ViewIdx* inputView,
                      int* inputNb)
{
    *inputTime = time;
    *inputView = view;
    *inputNb = kMergeInputB;

    if (getMixValue(time, view) == 0.) {
        return true;
    }

    bool outputChannels[4];
    bool bChannels[4];
    for (int c = 0; c < 4; ++c) {
        outputChannels[c] = readBool(_outputChannels[c], time, view, true);
        bChannels[c] = readBool(_bChannels[c], time, view, true);
    }
    if (!outputChannels[0] && !outputChannels[1] && !outputChannels[2] && !outputChannels[3]) {
        return true;
    }
    for (int c = 0; c < 4; ++c) {
        if (outputChannels[c] && !bChannels[c]) {
            return false;
        }
    }

    RectI maskPixels;
    bool maskPixelsValid = false;
    if (isMaskApplied() && !getMaskInvertValue(time, view)) {
        RectD maskRoD;
        if (getInputRoD(kMergeInputMask, time, scale, view, &maskRoD)) {
            maskPixelsValid = true;
            maskPixels = toPixelEnclosing(maskRoD, scale, getAspectRatio(kMergeInputMask));
            if (!rectIntersection<RectI>(roi, maskPixels, 0)) {
                return true;
            }
        }
    }

    if (!MergeOperators::isIdentityForBOnly(getOperation(time, view))) {
        // Most operators change B where A is absent: multiply, for one, gives black there.
        return false;
    }

    // The effect only reaches where an A input, clipped to the mask, meets the window.
    for (int i = 0; i < kMergeMaxAInputs; ++i) {
        const int a = aInputIndex(i);
        RectD aRoD;
        if (!getInputRoD(a, time, scale, view, &aRoD) || rectIsEmpty(aRoD)) {
            continue;
        }
        RectI aPixels = toPixelEnclosing(aRoD, scale, getAspectRatio(a));
        bool aPixelsValid = true;
        if (maskPixelsValid) {
            aPixelsValid = rectIntersection<RectI>(aPixels, maskPixels, &aPixels);
        }
        if (aPixelsValid && rectIntersection<RectI>(roi, aPixels, 0)) {
            return false;
        }
    }

    return true;
} // MergeNode::isIdentity

StatusEnum
MergeNode::render(const RenderActionArgs& args)
{
    const double time = args.time;
    const ViewIdx view = args.view;
    const MergeOperators::Operation op = getOperation(time, view);
    KnobBoolPtr alphaMaskingKnob = _alphaMasking.lock();
    const bool alphaMasking = alphaMaskingKnob ? alphaMaskingKnob->getValueAtTime(time, 0, view) : false;
    const float mix = (float)getMixValue(time, view);
    const bool maskInvert = getMaskInvertValue(time, view);
    std::bitset<4> aChannels;
    std::bitset<4> bChannels;
    std::bitset<4> outputChannels;
    for (int c = 0; c < 4; ++c) {
        aChannels[c] = readBool(_aChannels[c], time, view, true);
        bChannels[c] = readBool(_bChannels[c], time, view, true);
        outputChannels[c] = readBool(_outputChannels[c], time, view, true);
    }

    NodePtr node = getNode();
    const int preferredInput = node ? node->getPreferredInput() : -1;

    // Every image is fetched before any is locked: fetching renders upstream, which may write
    // into a cached image this render would otherwise already hold a read lock on.
    const bool doMask = isMaskApplied();
    ImagePtr mask;
    int maskChannel = -1;
    if (doMask) {
        ImageLayerDesc maskLayer;
        if (resolveInputPlaneForRender(kMergeInputMask, time, view, &maskLayer, &maskChannel) && (maskChannel >= 0)) {
            RectI maskRoI;
            mask = getImage(kMergeInputMask, time, args.mappedScale, view, NULL, &maskLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &maskRoI);
        }
        if (mask && (maskChannel >= (int)mask->getComponentsCount())) {
            mask.reset();
        }
    }
    if (!isFloatImage(mask)) {
        return eStatusFailed;
    }

    std::vector<MergePlaneJob> jobs;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        MergePlaneJob job;
        job.dst = it->second;
        if (!job.dst) {
            continue;
        }
        const int nComps = (int)job.dst->getComponentsCount();
        if ((nComps < 1) || (nComps > 4) || !isFloatImage(job.dst)) {
            return eStatusFailed;
        }
        job.channels = processedBitsForImage(it->first, nComps, args.processChannels);

        ImageLayerDesc layer;
        RectI inputRoI;
        if (getInput(kMergeInputB) && resolveInputPlaneForRender(kMergeInputB, time, view, &layer, NULL)) {
            // Mapped to the clip's components, which is the layout the output plane is rendered in.
            job.b.image = getImage(kMergeInputB, time, args.mappedScale, view, NULL, &layer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &inputRoI);
        }
        if (preferredInput == kMergeInputB) {
            job.passSource = 0;
        }
        for (int i = 0; i < kMergeMaxAInputs; ++i) {
            const int a = aInputIndex(i);
            if (!getInput(a) || !resolveInputPlaneForRender(a, time, view, &layer, NULL)) {
                continue;
            }
            MergeSource source;
            source.image = getImage(a, time, args.mappedScale, view, NULL, &layer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &inputRoI);
            // OpenFX Merge only merges the A images it was given: a missing one does not take
            // the place of the first.
            if (!source.image) {
                continue;
            }
            if (!isFloatImage(source.image)) {
                return eStatusFailed;
            }
            if (a == preferredInput) {
                job.passSource = (int)job.as.size() + 1;
            }
            job.as.push_back(source);
        }
        if (!isFloatImage(job.b.image)) {
            return eStatusFailed;
        }
        jobs.push_back(job);
    }

    const RectI& roi = args.roi;
    const int width = roi.width();
    if (jobs.empty() || (width <= 0) || (roi.height() <= 0)) {
        return eStatusOK;
    }

    // Every output is locked for writing below, for the whole render, so no source may be one
    // of them.
    std::vector<ImagePtr> outputs;
    for (std::size_t j = 0; j < jobs.size(); ++j) {
        outputs.push_back(jobs[j].dst);
    }
    mask = sourceDetachedFromOutputs(mask, outputs);
    for (std::size_t j = 0; j < jobs.size(); ++j) {
        jobs[j].b.image = sourceDetachedFromOutputs(jobs[j].b.image, outputs);
        for (std::size_t i = 0; i < jobs[j].as.size(); ++i) {
            jobs[j].as[i].image = sourceDetachedFromOutputs(jobs[j].as[i].image, outputs);
        }
    }

    std::shared_ptr<Image::ReadAccess> maskAccess;
    RectI maskBounds;
    if (mask) {
        maskBounds = mask->getBounds();
        maskAccess = std::make_shared<Image::ReadAccess>(mask.get());
    }

    // Images are locked here, on the calling thread, for the whole render; the band threads only
    // compute pixel addresses through these accesses.
    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    std::vector<RectI> bandRects;
    makeRowBands(roi, nThreads, &bandRects);
    std::vector<MergeRowBand> bands;
    for (std::size_t j = 0; j < jobs.size(); ++j) {
        MergePlaneJob& job = jobs[j];
        job.dstAccess = std::make_shared<Image::WriteAccess>(job.dst.get());
        if (job.b.image) {
            job.b.bounds = job.b.image->getBounds();
            job.b.access = std::make_shared<Image::ReadAccess>(job.b.image.get());
        }
        for (std::size_t i = 0; i < job.as.size(); ++i) {
            job.as[i].bounds = job.as[i].image->getBounds();
            job.as[i].access = std::make_shared<Image::ReadAccess>(job.as[i].image.get());
        }
        for (std::size_t b = 0; b < bandRects.size(); ++b) {
            bands.push_back(MergeRowBand(j, bandRects[b].y1, bandRects[b].y2));
        }
    }

    const bool identityForBOnly = MergeOperators::isIdentityForBOnly(op);
    RenderCancellation cancel(this);

    const std::function<void(int)> renderBand = [&](int bandIndex) {
        const MergeRowBand& band = bands[bandIndex];
        const MergePlaneJob& job = jobs[band.job];
        const int nComps = (int)job.dst->getComponentsCount();
        const std::size_t rowSize = (std::size_t)width * nComps;
        int channelBit[4];
        std::bitset<4> keepAll;
        std::bitset<4> aKeep;
        std::bitset<4> bKeep;
        bool allOutputs = true;
        bool allProcessed = true;
        for (int c = 0; c < nComps; ++c) {
            channelBit[c] = pixelKernelChannelBit(nComps, c);
            keepAll[c] = true;
            aKeep[c] = aChannels[channelBit[c]];
            bKeep[c] = bChannels[channelBit[c]];
            allOutputs = allOutputs && outputChannels[channelBit[c]];
            allProcessed = allProcessed && job.channels[channelBit[c]];
        }
        const bool bKeepsAll = (bKeep == keepAll);
        const float aOpaqueAlpha = aChannels[3] ? 1.f : 0.f;
        const float bOpaqueAlpha = bChannels[3] ? 1.f : 0.f;
        const MergeOperators::RowFunction mergeRow = MergeOperators::mergeRowFunction(op, nComps);
        const MergeOperators::RowFunction mergeOverRow = MergeOperators::mergeOverRowFunction(op, nComps);
        const bool mixesWithB = doMask || (mix != 1.f) || !allOutputs;
        // The result goes straight into the output row when every channel of it is processed.
        const bool writesDst = allProcessed;
        std::vector<float> bRow(rowSize);
        std::vector<unsigned char> bPresentRow(width);
        std::vector<float> bKept(bKeepsAll ? 0 : rowSize);
        std::vector<float> aRow(rowSize);
        std::vector<unsigned char> aPresentRow(width);
        std::vector<float> passRow(rowSize);
        std::vector<unsigned char> passPresentRow(width);
        std::vector<float> maskRow(doMask ? width : 0);
        std::vector<unsigned char> maskPresent(doMask ? width : 0);
        std::vector<float> result(writesDst ? 0 : rowSize);
        std::vector<float> noARow(job.as.empty() ? rowSize : 0, 0.f);
        std::vector<unsigned char> noAPresent(job.as.empty() ? width : 0, (unsigned char)0);

        for (int y = band.y1; y < band.y2; ++y) {
            if ((((y - band.y1) % kAbortCheckRows) == 0) && cancel.check()) {
                return;
            }

            float* dstPix = (float*)job.dstAccess->pixelAt(roi.x1, y);
            if (!dstPix) {
                continue;
            }
            float* const result0 = writesDst ? dstPix : &result[0];

            const unsigned char* bPresent = 0;
            const float* const bPixels = fetchRow(job.b, roi.x1, y, width, nComps, keepAll, &bRow[0], &bPresentRow[0], &bPresent);

            if (job.as.empty() && identityForBOnly) {
                for (int x = 0; x < width; ++x) {
                    const float* bPix = bPixels + (std::size_t)x * nComps;
                    float* out = result0 + (std::size_t)x * nComps;
                    const bool hasB = !bPresent || bPresent[x];
                    for (int c = 0; c < nComps; ++c) {
                        out[c] = (outputChannels[channelBit[c]] && hasB) ? bPix[c] : 0.f;
                    }
                }
            } else {
                MergeOperators::RowSide bSide = { bPixels, bPresent, bOpaqueAlpha };
                if (!bKeepsAll) {
                    for (std::size_t i = 0; i < rowSize; ++i) {
                        bKept[i] = bKeep[i % nComps] ? bPixels[i] : 0.f;
                    }
                    bSide.pixels = &bKept[0];
                }

                // The first A image over B. Without one, a transparent A still goes through the
                // operator once.
                MergeOperators::RowSide aSide = { noARow.data(), noAPresent.data(), aOpaqueAlpha };
                if (!job.as.empty()) {
                    aSide.pixels = fetchRow(job.as[0], roi.x1, y, width, nComps, aKeep, &aRow[0], &aPresentRow[0], &aSide.present);
                }
                mergeRow(alphaMasking, aSide, bSide, width, result0);

                // Each later A image over the running result, whose alpha is b.
                const MergeOperators::RowSide running = { result0, 0, 1.f };
                for (std::size_t i = 1; i < job.as.size(); ++i) {
                    aSide.pixels = fetchRow(job.as[i], roi.x1, y, width, nComps, aKeep, &aRow[0], &aPresentRow[0], &aSide.present);
                    mergeOverRow(alphaMasking, aSide, running, width, result0);
                }

                if (mixesWithB) {
                    if (doMask) {
                        readChannelRow(mask.get(), maskAccess.get(), maskBounds, maskChannel, roi.x1, y, width, &maskRow[0], &maskPresent[0]);
                    }
                    for (int x = 0; x < width; ++x) {
                        float* out = result0 + (std::size_t)x * nComps;
                        const float* bPix = bPixels + (std::size_t)x * nComps;
                        const bool hasB = !bPresent || bPresent[x];
                        float maskScale = 1.f;
                        if (doMask) {
                            if (!maskPresent[x]) {
                                maskScale = maskInvert ? 1.f : 0.f;
                            } else {
                                maskScale = maskInvert ? (1.f - maskRow[x]) : maskRow[x];
                            }
                        }
                        const float alpha = maskScale * mix;
                        if (alpha == 0.f) {
                            for (int c = 0; c < nComps; ++c) {
                                out[c] = hasB ? bPix[c] : 0.f;
                            }
                        } else if (alpha != 1.f) {
                            for (int c = 0; c < nComps; ++c) {
                                out[c] = hasB ? (out[c] * alpha + (1.f - alpha) * bPix[c]) : (out[c] * alpha);
                            }
                        }
                        for (int c = 0; c < nComps; ++c) {
                            if (!outputChannels[channelBit[c]]) {
                                out[c] = hasB ? bPix[c] : 0.f;
                            }
                        }
                    }
                }
            }

            if (writesDst) {
                continue;
            }
            const float* pass = 0;
            if (job.passSource == 0) {
                pass = bPixels;
            } else if (job.passSource > 0) {
                const unsigned char* passPresent = 0;
                pass = fetchRow(job.as[job.passSource - 1], roi.x1, y, width, nComps, keepAll, &passRow[0], &passPresentRow[0], &passPresent);
            }
            for (int x = 0; x < width; ++x, dstPix += nComps) {
                const float* out = result0 + (std::size_t)x * nComps;
                for (int c = 0; c < nComps; ++c) {
                    if (job.channels[channelBit[c]]) {
                        dstPix[c] = out[c];
                    } else {
                        dstPix[c] = pass ? pass[(std::size_t)x * nComps + c] : 0.f;
                    }
                }
            }
        }
    };
    parallelForCancellable((int)bands.size(), nThreads, cancel, renderBand);

    return eStatusOK;
} // MergeNode::render

NATRON_NAMESPACE_EXIT
