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

#include "NativeWrite.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iterator>
#include <limits>
#include <list>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <OpenImageIO/imageio.h>

#include <ofxImageEffect.h>
#include <ofxNatron.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Channel/ChannelCopy.h"
#include "Engine/Nodes/IO/OiioWriteSupport.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"

NATRON_NAMESPACE_ENTER

namespace {
enum FormatTypeEnum {
    eFormatTypeInput = 0,
    eFormatTypeProject,
    eFormatTypeFixed
};

enum FrameRangeEnum {
    eFrameRangeUnion = 0,
    eFrameRangeProject,
    eFrameRangeManual
};

const int kMinBandPixels = 16384;

void
setSecretAndDisabled(const KnobIPtr& knob,
                     bool secret)
{
    if (!knob) {
        return;
    }
    knob->setSecret(secret);
    knob->setAllDimensionsEnabled(!secret);
}

std::string
extensionOf(const std::string& path)
{
    return std::filesystem::path(path).extension().string();
}

std::vector<ChoiceOption>
compressionOptions(const std::string& formatName)
{
    std::vector<ChoiceOption> options;
    const std::vector<OiioWriteSupport::CompressionChoice>& choices = OiioWriteSupport::compressionChoices(formatName);

    for (std::size_t i = 0; i < choices.size(); ++i) {
        options.push_back(ChoiceOption(choices[i].id, std::string(), choices[i].hint));
    }
    if (options.empty()) {
        options.push_back(ChoiceOption(kNatronWriteCompressionDefault));
    }

    return options;
}

// Every compression any writable format offers, for the menu a file name has not narrowed yet.
std::vector<ChoiceOption>
allCompressionOptions()
{
    std::vector<ChoiceOption> options;
    const std::vector<std::string>& extensions = OiioWriteSupport::writableExtensions();

    for (std::size_t e = 0; e < extensions.size(); ++e) {
        const std::vector<ChoiceOption> formatOptions = compressionOptions(OiioWriteSupport::formatNameForWritableExtension(extensions[e]));
        for (std::size_t i = 0; i < formatOptions.size(); ++i) {
            const bool known = std::any_of(options.begin(), options.end(), [&](const ChoiceOption& o) { return o.id == formatOptions[i].id; });
            if (!known) {
                options.push_back(formatOptions[i]);
            }
        }
    }
    if (options.empty()) {
        options.push_back(ChoiceOption(kNatronWriteCompressionDefault));
    }

    return options;
}

const char*
dataTypeLabel(OiioWriteSupport::DataType type)
{
    switch (type) {
    case OiioWriteSupport::DataType::UInt8:
        return "8 bits integer";
    case OiioWriteSupport::DataType::UInt10:
        return "10 bits integer";
    case OiioWriteSupport::DataType::UInt12:
        return "12 bits integer";
    case OiioWriteSupport::DataType::UInt16:
        return "16 bits integer";
    case OiioWriteSupport::DataType::Half:
        return "16 bits floating point";
    case OiioWriteSupport::DataType::UInt32:
        return "32 bits integer";
    case OiioWriteSupport::DataType::Float:
        return "32 bits floating point";
    case OiioWriteSupport::DataType::UInt64:
        return "64 bits integer";
    case OiioWriteSupport::DataType::Double:
        return "64 bits floating point";
    }

    return "";
}

std::vector<ChoiceOption>
bitDepthOptions(const std::vector<OiioWriteSupport::DataType>& types)
{
    std::vector<ChoiceOption> options;

    options.push_back(ChoiceOption(kNatronWriteBitDepthAuto, std::string(), "Guess from the output format"));
    for (std::size_t i = 0; i < types.size(); ++i) {
        options.push_back(ChoiceOption(OiioWriteSupport::dataTypeId(types[i]), dataTypeLabel(types[i]), std::string()));
    }

    return options;
}

std::vector<OiioWriteSupport::DataType>
allDataTypes()
{
    return {
        OiioWriteSupport::DataType::UInt8,
        OiioWriteSupport::DataType::UInt10,
        OiioWriteSupport::DataType::UInt12,
        OiioWriteSupport::DataType::UInt16,
        OiioWriteSupport::DataType::Half,
        OiioWriteSupport::DataType::UInt32,
        OiioWriteSupport::DataType::Float,
        OiioWriteSupport::DataType::UInt64,
        OiioWriteSupport::DataType::Double
    };
}

bool
sameIDs(const std::vector<ChoiceOption>& a,
        const std::vector<ChoiceOption>& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id) {
            return false;
        }
    }

    return true;
}

// Replaces the menu, keeping the chosen entry when the new menu has it and falling back to the
// first otherwise.
void
setMenu(const KnobChoicePtr& knob,
        const std::vector<ChoiceOption>& options)
{
    if (!knob || sameIDs(knob->getEntries_mt_safe(), options)) {
        return;
    }
    knob->populateChoices(options);
    const std::string active = knob->getActiveEntry().id;
    const bool kept = std::any_of(options.begin(), options.end(), [&](const ChoiceOption& o) { return o.id == active; });
    if (!kept) {
        knob->setValue(0);
    }
}

std::string
activeID(const KnobChoicePtr& knob)
{
    return knob ? knob->getActiveEntry().id : std::string();
}

struct FrameSource {
    const Image::ReadAccess* access;
    ImageBitDepthEnum depth;
    RectI bounds;
    int nComps;
    std::vector<int> channels;
    RectI window;
};

template <typename SRCPIX>
void
convertRow(const FrameSource& frame,
           int y,
           float* out)
{
    const int nOut = (int)frame.channels.size();
    const int width = frame.window.width();
    const bool inRows = (y >= frame.bounds.y1) && (y < frame.bounds.y2);
    const int x1 = inRows ? std::max(frame.window.x1, frame.bounds.x1) : frame.window.x2;
    const int x2 = inRows ? std::min(frame.window.x2, frame.bounds.x2) : frame.window.x2;

    std::fill(out, out + (std::size_t)width * (std::size_t)nOut, 0.f);
    if (x2 <= x1) {
        return;
    }
    const SRCPIX* in = (const SRCPIX*)frame.access->pixelAt(x1, y);
    float* dst = out + (std::size_t)(x1 - frame.window.x1) * (std::size_t)nOut;
    for (int x = x1; x < x2; ++x, in += frame.nComps, dst += nOut) {
        for (int c = 0; c < nOut; ++c) {
            const int s = frame.channels[(std::size_t)c];
            if (s >= 0) {
                dst[c] = Image::convertPixelDepth<SRCPIX, float>(in[s]);
            }
        }
    }
}

void
convertFileRow(const FrameSource& frame,
               int fileRow,
               float* out)
{
    // File rows run downwards from the top of the data window, Natron's upwards.
    const int y = frame.window.y2 - 1 - fileRow;

    switch (frame.depth) {
    case eImageBitDepthByte:
        convertRow<unsigned char>(frame, y, out);
        break;
    case eImageBitDepthShort:
        convertRow<unsigned short>(frame, y, out);
        break;
    default:
        convertRow<float>(frame, y, out);
        break;
    }
}

StatusEnum
copyPlane(const EffectInstance& effect,
          const ImagePtr& source,
          const RectI& sourceBounds,
          const std::vector<int>& srcChannels,
          const ImagePtr& dst,
          const RectI& roi)
{
    const ImageBitDepthEnum dstDepth = dst->getBitDepth();
    const RectI both = source ? roi.intersect(sourceBounds) : RectI();
    const bool hasBoth = source && (both.x2 > both.x1) && (both.y2 > both.y1);

    if (hasBoth) {
        Image::ReadAccess src(source.get());
        const StatusEnum stat = ChannelCopy::copyChannels(&src, source->getBitDepth(), dst.get(), both, srcChannels, effect);
        if (stat != eStatusOK) {
            return stat;
        }
    }

    // What the source does not cover is black.
    std::vector<RectI> blank;
    if (!hasBoth) {
        blank.push_back(roi);
    } else {
        blank.push_back(RectI(roi.x1, roi.y1, roi.x2, both.y1));
        blank.push_back(RectI(roi.x1, both.y2, roi.x2, roi.y2));
        blank.push_back(RectI(roi.x1, both.y1, both.x1, both.y2));
        blank.push_back(RectI(both.x2, both.y1, roi.x2, both.y2));
    }
    for (std::size_t i = 0; i < blank.size(); ++i) {
        if ((blank[i].x2 > blank[i].x1) && (blank[i].y2 > blank[i].y1)) {
            const StatusEnum stat = ChannelCopy::copyChannels(NULL, dstDepth, dst.get(), blank[i], srcChannels, effect);
            if (stat != eStatusOK) {
                return stat;
            }
        }
    }

    return eStatusOK;
}
} // anonymous namespace

NativeWrite::NativeWrite(NodePtr node)
    : NativeEffectBase(node)
{
}

NativePluginDescription
NativeWrite::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_WRITE;
    desc.label = "Write";
    desc.description = tr("Write an image or image sequence.").toStdString();
    desc.grouping = PLUGIN_GROUP_IMAGE;
    desc.majorVersion = PLUGIN_MAJOR_NATRON_WRITE;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("Source", false, eDataKindImage));
    desc.outputKind = eDataKindImage;
    desc.isWriter = true;

    return desc;
}

void
NativeWrite::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobOutputFilePtr filename = createKnob<KnobOutputFile>(tr("File"));
    filename->setName(kOfxImageEffectFileParamName);
    filename->setAsOutputImageFile();
    filename->setHintToolTip(tr("The image or image sequence to write."));
    page->addKnob(filename);
    _filename = filename;

    KnobBoolPtr overwrite = createKnob<KnobBool>(tr("Overwrite"));
    overwrite->setName(kNatronWriteParamOverwrite);
    overwrite->setHintToolTip(tr("Overwrite existing files when rendering."));
    overwrite->setDefaultValue(true);
    overwrite->setAnimationEnabled(false);
    page->addKnob(overwrite);
    _overwrite = overwrite;

    KnobChoicePtr formatType = createKnob<KnobChoice>(tr("Format Type"));
    formatType->setName(kNatronWriteParamFormatType);
    formatType->setHintToolTip(tr("Determines which rectangle of pixels will be written in output."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kNatronWriteFormatTypeInput, tr("Input Format").toStdString(), tr("Renders the pixels included in the input format").toStdString()));
        options.push_back(ChoiceOption(kNatronWriteFormatTypeProject, tr("Project Format").toStdString(), tr("Renders the pixels included in the project format").toStdString()));
        options.push_back(ChoiceOption(kNatronWriteFormatTypeFixed, tr("Fixed Format").toStdString(), tr("Renders the pixels included in the format indicated by the Format parameter.").toStdString()));
        formatType->populateChoices(options);
    }
    formatType->setDefaultValue(eFormatTypeProject);
    formatType->setAnimationEnabled(false);
    formatType->setAddNewLine(false);
    page->addKnob(formatType);
    _formatType = formatType;

    const ExtentKnobs::FormatKnobSet formatKnobs = ExtentKnobs::createFormatKnobs(this, page);
    _format = formatKnobs.choice;
    _formatSize = formatKnobs.size;
    _formatPar = formatKnobs.par;
    formatKnobs.choice->setSecretByDefault(true);

    KnobBoolPtr clipToRoD = createKnob<KnobBool>(tr("Clip To RoD"));
    clipToRoD->setName(kNatronWriteParamClipToRoD);
    clipToRoD->setHintToolTip(tr("When checked, the portion of the image written will be the region of definition of the image in input and not the format selected by the Format Type parameter.\n"
                                 "For the EXR file format, this will distinguish the data window (size of the image in input) from the display window (the format specified by Format Type)."));
    clipToRoD->setDefaultValue(true);
    clipToRoD->setAnimationEnabled(false);
    page->addKnob(clipToRoD);
    _clipToRoD = clipToRoD;

    KnobChoicePtr bitDepth = createKnob<KnobChoice>(tr("Bit Depth"));
    bitDepth->setName(kNatronWriteParamBitDepth);
    bitDepth->setHintToolTip(tr("Number of bits per sample in the file. Auto picks the depth from the output format."));
    bitDepth->populateChoices(bitDepthOptions(allDataTypes()));
    bitDepth->setDefaultValue(0);
    bitDepth->setAnimationEnabled(false);
    page->addKnob(bitDepth);
    _bitDepth = bitDepth;

    KnobChoicePtr compression = createKnob<KnobChoice>(tr("Compression"));
    compression->setName(kNatronWriteParamCompression);
    compression->setHintToolTip(tr("The compression of the file, for the formats that have a choice. Default lets the format decide."));
    compression->populateChoices(allCompressionOptions());
    compression->setDefaultValue(0);
    compression->setAnimationEnabled(false);
    page->addKnob(compression);
    _compression = compression;

    KnobIntPtr quality = createKnob<KnobInt>(tr("Quality"));
    quality->setName(kNatronWriteParamQuality);
    quality->setHintToolTip(tr("Indicates the quality of compression to use (0-100), for those plugins and compression methods that allow a variable amount of compression, with higher numbers indicating higher image fidelity. [JPEG, TIFF w/ JPEG comp., WEBP]"));
    quality->setMinimum(0);
    quality->setMaximum(100);
    quality->setDisplayMinimum(0);
    quality->setDisplayMaximum(100);
    quality->setDefaultValue(100);
    quality->setAnimationEnabled(false);
    page->addKnob(quality);
    _quality = quality;

    KnobDoublePtr dwa = createKnob<KnobDouble>(tr("DWA Compression Level"));
    dwa->setName(kNatronWriteParamDWACompressionLevel);
    dwa->setHintToolTip(tr("Amount of compression when using Dreamworks DWAA or DWAB compression options. These lossy formats are variable in quality and can minimize the compression artifacts. Higher values will result in greater compression and likewise smaller file size, but increases the chance for artifacts. Values from 45 to 150 are usually correct for production shots, whereas HDR vacation photos could use up to 500. Values below 45 should give no visible improvement on photographs. [EXR w/ DWAa or DWAb comp.]"));
    dwa->setMinimum(0.);
    dwa->setMaximum(100000.);
    dwa->setDisplayMinimum(0.);
    dwa->setDisplayMaximum(500.);
    dwa->setDefaultValue(45.);
    dwa->setAnimationEnabled(false);
    page->addKnob(dwa);
    _dwaCompressionLevel = dwa;

    KnobIntPtr zip = createKnob<KnobInt>(tr("ZIP Compression Level"));
    zip->setName(kNatronWriteParamZIPCompressionLevel);
    zip->setHintToolTip(tr("Compression level for zip/deflate compression, on a scale from 1 (fastest, minimal compression) to 9 (slowest, maximal compression) [EXR, TIFF or Zfile w/ zip or zips comp.]"));
    zip->setMinimum(1);
    zip->setMaximum(9);
    zip->setDisplayMinimum(1);
    zip->setDisplayMaximum(9);
    zip->setDefaultValue(4);
    zip->setAnimationEnabled(false);
    page->addKnob(zip);
    _zipCompressionLevel = zip;

    KnobChoicePtr tileSize = createKnob<KnobChoice>(tr("Tile Size"));
    tileSize->setName(kNatronWriteParamTileSize);
    tileSize->setHintToolTip(tr("Size of a tile in the output file for formats that support tiles. If scan-line based, the whole image will have a single tile."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("0", tr("Scan-Line Based").toStdString(), std::string()));
        options.push_back(ChoiceOption("64"));
        options.push_back(ChoiceOption("128"));
        options.push_back(ChoiceOption("256"));
        options.push_back(ChoiceOption("512"));
        tileSize->populateChoices(options);
    }
    tileSize->setDefaultValue(0);
    tileSize->setAnimationEnabled(false);
    page->addKnob(tileSize);
    _tileSize = tileSize;

    KnobChoicePtr frameRange = createKnob<KnobChoice>(tr("Frame Range"));
    frameRange->setName(kNatronWriteParamFrameRange);
    frameRange->setHintToolTip(tr("What frame range should be rendered."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kNatronWriteFrameRangeUnion, tr("Union of input ranges").toStdString(), tr("The union of all inputs frame ranges will be rendered.").toStdString()));
        options.push_back(ChoiceOption(kNatronWriteFrameRangeProject, tr("Project frame range").toStdString(), tr("The frame range delimited by the frame range of the project will be rendered.").toStdString()));
        options.push_back(ChoiceOption(kNatronWriteFrameRangeManual, tr("Manual").toStdString(), tr("The frame range will be the one defined by the first frame and last frame parameters.").toStdString()));
        frameRange->populateChoices(options);
    }
    frameRange->setDefaultValue(eFrameRangeProject);
    frameRange->setAnimationEnabled(false);
    page->addKnob(frameRange);
    _frameRange = frameRange;

    KnobIntPtr firstFrame = createKnob<KnobInt>(tr("First Frame"));
    firstFrame->setName(kNatronWriteParamFirstFrame);
    firstFrame->setHintToolTip(tr("The first frame to render, when the frame range is Manual."));
    firstFrame->setDefaultValue(0);
    firstFrame->setAnimationEnabled(false);
    firstFrame->disableSlider();
    firstFrame->setAddNewLine(false);
    page->addKnob(firstFrame);
    _firstFrame = firstFrame;

    KnobIntPtr lastFrame = createKnob<KnobInt>(tr("Last Frame"));
    lastFrame->setName(kNatronWriteParamLastFrame);
    lastFrame->setHintToolTip(tr("The last frame to render, when the frame range is Manual."));
    lastFrame->setDefaultValue(0);
    lastFrame->setAnimationEnabled(false);
    lastFrame->disableSlider();
    page->addKnob(lastFrame);
    _lastFrame = lastFrame;

    // Node.cpp shows this knob's value, in parentheses, next to the node's label; it follows the
    // file name there.
    KnobStringPtr sublabel = createKnob<KnobString>(tr("SubLabel"));
    sublabel->setName(kNatronOfxParamStringSublabelName);
    sublabel->setSecretByDefault(true);
    sublabel->setAnimationEnabled(false);
    sublabel->setEvaluateOnChange(false);
    page->addKnob(sublabel);

    // Node::createWriterFrameStepKnob is compiled out when the IO meta nodes are enabled.
    KnobIntPtr frameIncr = createKnob<KnobInt>(tr(kNatronWriteParamFrameStepLabel));
    frameIncr->setName(kNatronWriteParamFrameStep);
    frameIncr->setHintToolTip(tr(kNatronWriteParamFrameStepHint));
    frameIncr->setAnimationEnabled(false);
    frameIncr->setMinimum(1);
    frameIncr->setDefaultValue(1);
    page->addKnob(frameIncr);

    refreshOptions();
}

void
NativeWrite::onKnobsLoaded()
{
    refreshOptions();
}

bool
NativeWrite::rejectUnsupportedFormat(const std::string& path)
{
    if (OiioWriteSupport::isWritablePath(path)) {
        return false;
    }
    setPersistentMessage(eMessageTypeError, tr("Unsupported format: %1").arg(QString::fromStdString(path)).toStdString());

    return true;
}

void
NativeWrite::refreshOptions()
{
    KnobOutputFilePtr filename = _filename.lock();
    KnobChoicePtr compression = _compression.lock();
    KnobChoicePtr bitDepth = _bitDepth.lock();
    KnobChoicePtr formatType = _formatType.lock();

    if (!filename || !compression || !bitDepth || !formatType) {
        return;
    }
    const std::string path = filename->getValue();
    const std::string formatName = path.empty() ? std::string() : OiioWriteSupport::formatNameForWritableExtension(extensionOf(path));
    const OiioWriteSupport::FormatCaps caps = formatName.empty() ? OiioWriteSupport::FormatCaps() : OiioWriteSupport::capabilities(formatName);

    if (formatName.empty()) {
        setMenu(compression, allCompressionOptions());
        setMenu(bitDepth, bitDepthOptions(allDataTypes()));
    } else {
        setMenu(compression, compressionOptions(formatName));
        setMenu(bitDepth, bitDepthOptions(OiioWriteSupport::validDataTypes(formatName)));
    }
    const OiioWriteSupport::ParamVisibility visible = OiioWriteSupport::paramVisibility(formatName, activeID(compression));

    setSecretAndDisabled(compression, !visible.compression);
    setSecretAndDisabled(bitDepth, formatName.empty());
    setSecretAndDisabled(_quality.lock(), !visible.quality);
    setSecretAndDisabled(_dwaCompressionLevel.lock(), !visible.dwaCompressionLevel);
    setSecretAndDisabled(_zipCompressionLevel.lock(), !visible.zipCompressionLevel);
    setSecretAndDisabled(_tileSize.lock(), !visible.tileSize);
    setSecretAndDisabled(_clipToRoD.lock(), !caps.displayWindow);
    setSecretAndDisabled(_format.lock(), formatType->getValue() != eFormatTypeFixed);
    setSecretAndDisabled(_formatSize.lock(), true);
    setSecretAndDisabled(_formatPar.lock(), true);

    KnobChoicePtr frameRange = _frameRange.lock();
    const bool manual = frameRange && frameRange->getValue() == eFrameRangeManual;
    setSecretAndDisabled(_firstFrame.lock(), !manual);
    setSecretAndDisabled(_lastFrame.lock(), !manual);
}

void
NativeWrite::getFrameRange(double* first,
                           double* last)
{
    KnobChoicePtr frameRange = _frameRange.lock();
    KnobIntPtr firstFrame = _firstFrame.lock();
    KnobIntPtr lastFrame = _lastFrame.lock();
    const int choice = frameRange ? frameRange->getValue() : (int)eFrameRangeProject;

    if ((choice == eFrameRangeManual) && firstFrame && lastFrame) {
        *first = firstFrame->getValue();
        *last = lastFrame->getValue();
    } else if (choice == eFrameRangeUnion) {
        EffectInstance::getFrameRange(first, last);
    } else {
        getApp()->getFrameRange(first, last);
    }
}

void
NativeWrite::seedFrame(const KnobIntPtr& knob,
                       bool isFirst,
                       bool force)
{
    if (!knob || (!force && (knob->getValue() != 0))) {
        return;
    }
    double first = 0.;
    double last = 0.;
    getApp()->getFrameRange(&first, &last);
    knob->setValue((int)(isFirst ? first : last));
}

bool
NativeWrite::knobChanged(KnobI* k,
                         ValueChangedReasonEnum reason,
                         ViewSpec /*view*/,
                         double /*time*/,
                         bool /*originatedFromMainThread*/)
{
    if (reason == eValueChangedReasonTimeChanged) {
        return false;
    }
    KnobOutputFilePtr filename = _filename.lock();
    KnobChoicePtr compression = _compression.lock();
    KnobChoicePtr formatType = _formatType.lock();

    if (filename && (k == filename.get())) {
        refreshOptions();
        const std::string path = filename->getValue();
        if (path.empty() || !rejectUnsupportedFormat(path)) {
            clearPersistentMessage(false);
        }

        return true;
    }
    if ((compression && (k == compression.get())) || (formatType && (k == formatType.get()))) {
        refreshOptions();

        return true;
    }
    KnobChoicePtr frameRange = _frameRange.lock();
    if (frameRange && (k == frameRange.get())) {
        if (frameRange->getValue() == eFrameRangeManual) {
            seedFrame(_firstFrame.lock(), true, false);
            seedFrame(_lastFrame.lock(), false, false);
        }
        refreshOptions();

        return true;
    }
    // Resetting a knob delivers the restore reason twice; seeding from the range sets the same value both times.
    if (reason == eValueChangedReasonRestoreDefault) {
        KnobIntPtr firstFrame = _firstFrame.lock();
        KnobIntPtr lastFrame = _lastFrame.lock();
        if (firstFrame && (k == firstFrame.get())) {
            seedFrame(firstFrame, true, true);

            return true;
        }
        if (lastFrame && (k == lastFrame.get())) {
            seedFrame(lastFrame, false, true);

            return true;
        }
    }

    return false;
}

void
NativeWrite::selectedFormat(double time,
                            ViewIdx view,
                            RectI* display,
                            double* par) const
{
    KnobChoicePtr formatType = _formatType.lock();
    const int type = formatType ? formatType->getValue() : (int)eFormatTypeProject;

    if (type == eFormatTypeInput) {
        EffectInstancePtr input = getInput(0);
        if (input) {
            const RectI inputFormat = input->getOutputFormat();
            if ((inputFormat.x2 > inputFormat.x1) && (inputFormat.y2 > inputFormat.y1)) {
                *display = inputFormat;
                *par = getAspectRatio(0);

                return;
            }
        }
    } else if (type == eFormatTypeFixed) {
        KnobIntPtr size = _formatSize.lock();
        KnobDoublePtr formatPar = _formatPar.lock();
        if (size && formatPar) {
            *display = RectI(0, 0, size->getValueAtTime(time, 0, view), size->getValueAtTime(time, 1, view));
            *par = formatPar->getValueAtTime(time, 0, view);

            return;
        }
    }
    Format projectFormat;
    getApp()->getProject()->getProjectDefaultFormat(&projectFormat);
    *display = projectFormat;
    *par = projectFormat.getPixelAspectRatio();
}

bool
NativeWrite::clipToRoDActive() const
{
    KnobBoolPtr clipToRoD = _clipToRoD.lock();

    return clipToRoD && !clipToRoD->getIsSecret() && clipToRoD->getValue();
}

bool
NativeWrite::resolveWindow(double time,
                           ViewIdx view,
                           Window* window) const
{
    selectedFormat(time, view, &window->display, &window->par);
    window->rod = window->display.toCanonical_noClipping(0, window->par);

    EffectInstancePtr input = getInput(0);
    if (input && clipToRoDActive()) {
        bool isProjectFormat = false;
        if (input->getRegionOfDefinition_public(input->getRenderHash(), time, RenderScale::identity, view, &window->rod, &isProjectFormat) == eStatusFailed) {
            return false;
        }
        window->par = getAspectRatio(0);
    }

    return true;
}

StatusEnum
NativeWrite::getRegionOfDefinition(U64 /*hash*/,
                                   double time,
                                   const RenderScale& /*scale*/,
                                   ViewIdx view,
                                   RectD* rod)
{
    Window window;

    if (!resolveWindow(time, view, &window)) {
        return eStatusFailed;
    }
    *rod = window.rod;

    return eStatusOK;
}

void
NativeWrite::getRegionsOfInterest(double /*time*/,
                                  const RenderScale& /*scale*/,
                                  const RectD& outputRoD,
                                  const RectD& /*renderWindow*/,
                                  ViewIdx /*view*/,
                                  RoIMap* ret)
{
    EffectInstancePtr input = getInput(0);

    if (input) {
        ret->insert(std::make_pair(input, outputRoD));
    }
}

StatusEnum
NativeWrite::render(const RenderActionArgs& args)
{
    struct PlaneCopy {
        ImagePtr source;
        RectI sourceBounds;
        std::vector<int> srcChannels;
    };

    // Fetched before any image is locked: fetching renders upstream, which may write into a
    // cached image this render would otherwise already hold a read lock on.
    std::vector<PlaneCopy> copies;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        PlaneCopy copy;
        const ImageLayerDesc& plane = it->first;
        RectI inputRoI;

        copy.source = getImage(0, args.time, args.mappedScale, args.view, NULL, &plane, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &inputRoI);
        copy.srcChannels.assign((std::size_t)plane.getNumComponents(), -1);
        if (copy.source) {
            copy.sourceBounds = copy.source->getBounds();
            const int nAvailable = (int)copy.source->getComponentsCount();
            for (int c = 0; (c < plane.getNumComponents()) && (c < nAvailable); ++c) {
                copy.srcChannels[c] = c;
            }
        }
        copies.push_back(copy);
    }

    KnobOutputFilePtr filename = _filename.lock();
    KnobBoolPtr overwrite = _overwrite.lock();
    if (!filename || !overwrite) {
        return eStatusFailed;
    }
    const std::string path = filename->generateFileNameAtTime((SequenceTime)std::floor(args.time + 0.5), args.view).toStdString();
    if (path.empty()) {
        setPersistentMessage(eMessageTypeError, tr("No output file is set").toStdString());

        return eStatusFailed;
    }
    if (rejectUnsupportedFormat(path)) {
        return eStatusFailed;
    }

    bool exists = false;
    if (!overwrite->getValue()) {
        std::error_code ec;
        exists = std::filesystem::exists(std::filesystem::path(path), ec);
    }

    int colorPlane = -1;
    {
        int index = 0;
        for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it, ++index) {
            if ((colorPlane < 0) && it->first.isColorLayer() && (it->first.getNumComponents() > 0)) {
                colorPlane = index;
            }
        }
    }

    RenderCancellation cancel(this);
    bool wrote = false;
    std::string writeError;
    if (!exists && (colorPlane >= 0)) {
        std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator planeIt = args.outputLayers.begin();
        std::advance(planeIt, colorPlane);
        const PlaneCopy& copy = copies[(std::size_t)colorPlane];
        const int nPlaneComps = planeIt->first.getNumComponents();

        Window window;
        if (!resolveWindow(args.time, args.view, &window)) {
            return eStatusFailed;
        }
        const RectI dataWindow = window.rod.toPixelEnclosing(0u, window.par);
        if ((dataWindow.width() <= 0) || (dataWindow.height() <= 0)) {
            setPersistentMessage(eMessageTypeError, tr("There is nothing to write: the region to write is empty").toStdString());

            return eStatusFailed;
        }

        const std::string ext = extensionOf(path);
        const std::string formatName = OiioWriteSupport::formatNameForWritableExtension(ext);
        const OiioWriteSupport::FormatCaps caps = OiioWriteSupport::capabilities(formatName);

        // Colour channels in R, G, B, A order, or the alpha alone; a format with no alpha
        // loses it unless it is all there is.
        static const char* const kNames[] = { "R", "G", "B", "A" };
        std::vector<int> fileChannels;
        std::vector<std::string> names;
        int alphaChannel = -1;
        for (int c = 0; c < nPlaneComps; ++c) {
            const bool isAlpha = (nPlaneComps == 1) || (nPlaneComps == 4 && c == 3);
            if (isAlpha && !caps.alpha && nPlaneComps > 1) {
                continue;
            }
            if (isAlpha) {
                alphaChannel = (int)names.size();
            }
            names.push_back(nPlaneComps == 1 ? "A" : (c < 4 ? kNames[c] : "C" + std::to_string(c)));
            fileChannels.push_back(c);
        }

        KnobChoicePtr bitDepth = _bitDepth.lock();
        KnobChoicePtr compression = _compression.lock();
        OiioWriteSupport::DataType type = OiioWriteSupport::autoDataType(formatName, ext);
        OiioWriteSupport::DataType chosen;
        if (bitDepth && OiioWriteSupport::dataTypeFromId(activeID(bitDepth), &chosen)) {
            const std::vector<OiioWriteSupport::DataType>& valid = OiioWriteSupport::validDataTypes(formatName);
            if (std::find(valid.begin(), valid.end(), chosen) != valid.end()) {
                type = chosen;
            }
        }

        OiioWriteSupport::WritePart part;
        OIIO::ImageSpec& spec = part.spec;
        spec = OIIO::ImageSpec(dataWindow.width(), dataWindow.height(), (int)fileChannels.size(), OiioWriteSupport::dataTypeOiio(type));
        spec.channelnames = names;
        spec.alpha_channel = alphaChannel;
        spec.z_channel = -1;
        spec.attribute("oiio:BitsPerSample", OiioWriteSupport::dataTypeBitsPerSample(type));
        // Without this OIIO divides RGB by alpha on the way out for the formats it treats as
        // unassociated, and the buffer must be written as it is.
        spec.attribute("oiio:UnassociatedAlpha", 1);
        if (window.par != 1.) {
            spec.attribute("PixelAspectRatio", (float)window.par);
        }
        if (caps.displayWindow) {
            spec.x = dataWindow.x1;
            spec.y = window.display.y2 - dataWindow.y2;
            spec.full_x = window.display.x1;
            spec.full_y = window.display.y1;
            spec.full_width = window.display.width();
            spec.full_height = window.display.height();
        }

        const std::string compressionID = activeID(compression);
        const OiioWriteSupport::ParamVisibility visible = OiioWriteSupport::paramVisibility(formatName, compressionID);
        std::string compressionName = (compressionID == kNatronWriteCompressionDefault) ? std::string() : compressionID;
        if (compressionName.empty() && visible.quality && (formatName == "jpeg" || formatName == "webp" || formatName == "heic" || formatName == "avif")) {
            compressionName = formatName;
        }
        if (visible.quality) {
            KnobIntPtr quality = _quality.lock();
            const int value = quality ? quality->getValue() : 100;
            if (compressionName.empty()) {
                spec.attribute("CompressionQuality", value);
            } else {
                compressionName += ':' + std::to_string(value);
            }
        } else if (visible.dwaCompressionLevel) {
            KnobDoublePtr dwa = _dwaCompressionLevel.lock();
            compressionName += ':' + std::to_string((int)(dwa ? dwa->getValue() : 45.));
        } else if (visible.zipCompressionLevel) {
            KnobIntPtr zip = _zipCompressionLevel.lock();
            compressionName += ':' + std::to_string(zip ? zip->getValue() : 4);
        }
        if (!compressionName.empty()) {
            spec.attribute("compression", compressionName);
        }
        if (visible.tileSize) {
            KnobChoicePtr tileSize = _tileSize.lock();
            const int tile = tileSize ? std::atoi(activeID(tileSize).c_str()) : 0;
            if (tile > 0) {
                spec.tile_width = std::min(tile, spec.width);
                spec.tile_height = std::min(tile, spec.height);
                spec.tile_depth = 1;
            }
        }

        Image::ReadAccess access(copy.source.get());
        FrameSource frame;
        frame.access = copy.source ? &access : NULL;
        frame.depth = copy.source ? copy.source->getBitDepth() : eImageBitDepthFloat;
        frame.bounds = copy.source ? copy.sourceBounds : RectI();
        frame.nComps = copy.source ? (int)copy.source->getComponentsCount() : 0;
        frame.window = dataWindow;
        for (std::size_t i = 0; i < fileChannels.size(); ++i) {
            frame.channels.push_back(copy.srcChannels[(std::size_t)fileChannels[i]]);
        }

        const int nThreads = appPTR->getNCPUsAvailableForEffect();
        const int rowFloats = spec.width * spec.nchannels;
        const int rowsPerChunk = std::max(1, kMinBandPixels / std::max(1, spec.width));
        const OiioWriteSupport::RowSource rowSource = [&](int /*part*/, int rowBegin, int rowEnd, float* band, OIIO::stride_t rowStride) {
            const int count = rowEnd - rowBegin;
            const int chunks = (count + rowsPerChunk - 1) / rowsPerChunk;
            const std::function<void(int)> body = [&](int chunk) {
                const int first = rowBegin + chunk * rowsPerChunk;
                const int last = std::min(rowEnd, first + rowsPerChunk);
                for (int r = first; r < last; ++r) {
                    float* out = (float*)((char*)band + (std::ptrdiff_t)(r - rowBegin) * (std::ptrdiff_t)rowStride);
                    if (!frame.access) {
                        std::fill(out, out + rowFloats, 0.f);
                    } else {
                        convertFileRow(frame, r, out);
                    }
                    if (((r - first) % 8 == 0) && cancel.check()) {
                        return;
                    }
                }
            };
            parallelForCancellable(chunks, nThreads, cancel, body);

            return !cancel.check();
        };

        std::vector<OiioWriteSupport::WritePart> parts(1, part);
        wrote = OiioWriteSupport::writeFile(path, parts, rowSource, [&]() { return cancel.check(); }, &writeError);
        if (!wrote && !cancel.check()) {
            setPersistentMessage(eMessageTypeError, writeError.empty() ? tr("Could not write %1").arg(QString::fromStdString(path)).toStdString() : writeError);

            return eStatusFailed;
        }
    }

    std::vector<PlaneCopy>::const_iterator copy = copies.begin();
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it, ++copy) {
        const ImagePtr& outImage = it->second;
        if (!outImage) {
            continue;
        }
        const StatusEnum stat = copyPlane(*this, copy->source, copy->sourceBounds, copy->srcChannels, outImage, args.roi);
        if ((stat != eStatusOK) || aborted()) {
            return stat;
        }
    }
    if (wrote || exists) {
        clearPersistentMessage(false);
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
