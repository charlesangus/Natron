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

#include "NativeRead.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <OpenImageIO/imageio.h>

#include <QDateTime>
#include <QFileInfo>
#include <QString>

#include <ofxImageEffect.h>
#include <ofxMetadata.h>
#include <ofxNatron.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/FileSystemModel.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/Nodes/IO/ReadColorSpace.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/PoolParallelFor.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

namespace {
// Bands are rounded to this many rows so the blocks of a compressed file are not split.
const int kMinBandRows = 32;

// Upper bound on the decoded bytes one band holds at a time.
const std::size_t kStagingBytes = std::size_t(16) << 20;

// Past this many frames a still image's valid range is no longer enumerated.
const int kMaxStillFrames = 1 << 16;

double
pixelAspectOf(const OIIO::ImageSpec& spec)
{
    const float par = spec.get_float_attribute("PixelAspectRatio", 1.f);

    return par > 0.f ? (double)par : 1.;
}

// Where the R, G, B and A of a file sit among its channels, -1 when it has none, and its
// luminance (Y or I), which stands in for R, G and B when the file has none of them.
struct ColourChannels {
    int index[4];
    int luminance;

    ColourChannels()
        : index { -1, -1, -1, -1 }
        , luminance(-1)
    {
    }

    static ColourChannels of(const OIIO::ImageSpec& spec)
    {
        std::vector<int> index;

        for (int i = 0; i < spec.nchannels && i < (int)spec.channelnames.size(); ++i) {
            index.push_back(i);
        }

        return of(spec.channelnames, index);
    }

    // `names[k]` is the name of file channel `index[k]`.
    static ColourChannels of(const std::vector<std::string>& names,
                             const std::vector<int>& index)
    {
        ColourChannels result;

        for (std::size_t k = 0; k < index.size() && k < names.size(); ++k) {
            const int i = index[k];
            std::string name = names[k];
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
            if (name.compare(0, 5, "rgba.") == 0) {
                name.erase(0, 5);
            }
            if (name.size() != 1) {
                continue;
            }
            if (name[0] == 'y' || name[0] == 'i') {
                if (result.luminance < 0) {
                    result.luminance = i;
                }
                continue;
            }
            const std::size_t slot = std::string("rgba").find(name[0]);
            if (slot != std::string::npos && result.index[slot] < 0) {
                result.index[slot] = i;
            }
        }

        return result;
    }

    bool hasRgb() const
    {
        return index[0] >= 0 || index[1] >= 0 || index[2] >= 0;
    }

    bool spreadsLuminance() const
    {
        return !hasRgb() && luminance >= 0;
    }

    // A file with none of R, G, B, A or luminance stays a black RGBA plane rather than being
    // shuffled in.
    int nComps() const
    {
        if (hasRgb() || spreadsLuminance()) {
            return index[3] >= 0 ? 4 : 3;
        }

        return index[3] >= 0 ? 1 : 4;
    }

    int fileChannelForComponent(int nComps,
                                int component) const
    {
        if (nComps == 1) {
            return index[3];
        }
        if ((nComps == 3 || nComps == 4) && component < nComps) {
            if (spreadsLuminance() && component < 3) {
                return luminance;
            }

            return index[component];
        }

        return -1;
    }
};

// OpenEXR stores the frame rate as a rational, other formats as a float.
double
framesPerSecondOf(const OIIO::ImageSpec& spec)
{
    const OIIO::ParamValue* attribute = spec.find_attribute("FramesPerSecond");

    if (!attribute) {
        return 0.;
    }
    if (attribute->type() == OIIO::TypeRational) {
        const int* ratio = (const int*)attribute->data();

        return ratio[1] != 0 ? (double)ratio[0] / ratio[1] : 0.;
    }

    return (double)spec.get_float_attribute("FramesPerSecond", 0.f);
}

// The subimage that holds the colour in `view`: the first with channels of that view.
int
colourSubimageOf(const OiioReadSupport::Header& header,
                 const std::string& view)
{
    std::vector<OiioReadSupport::PartChannels> parts;

    OiioReadSupport::viewParts(header, view, &parts);

    return parts.empty() ? 0 : parts[0].subimage;
}

const char* const kKnobProxyThreshold = "proxyThreshold";
const char* const kKnobOriginalProxyScale = "originalProxyScale";
const char* const kKnobCustomProxyScale = "customProxyScale";
const char* const kKnobOriginalFrameRange = "originalFrameRange";
const char* const kKnobFirstFrame = "firstFrame";
const char* const kKnobLastFrame = "lastFrame";
const char* const kKnobBefore = "before";
const char* const kKnobAfter = "after";
const char* const kKnobOnMissingFrame = "onMissingFrame";
const char* const kKnobFrameMode = "frameMode";
const char* const kKnobStartingTime = "startingTime";
const char* const kKnobTimeOffset = "timeOffset";
const char* const kKnobTimeDomainUserEdited = "timeDomainUserEdited";
const char* const kKnobFrameRate = "frameRate";
const char* const kKnobCustomFps = "customFps";
const char* const kKnobOCIOConfigFile = "ocioConfigFile";
const char* const kKnobOCIOWorkingSpace = "ocioWorkingSpace";
const char* const kKnobOCIOInputSpaceIndex = "ocioInputSpaceIndex";
const char* const kKnobOCIOInputSpaceSet = "ocioInputSpaceSet";
} // anonymous namespace

NativeRead::NativeRead(NodePtr node)
    : NativeEffectBase(node)
    , _filename()
    , _proxy()
    , _proxyThreshold()
    , _originalProxyScale()
    , _customProxyScale()
    , _originalFrameRange()
    , _firstFrame()
    , _lastFrame()
    , _before()
    , _after()
    , _onMissingFrame()
    , _frameMode()
    , _startingTime()
    , _timeOffset()
    , _timeDomainUserEdited()
    , _frameRate()
    , _customFps()
    , _ocioConfigFile()
    , _ocioWorkingSpace()
    , _inputSpace()
    , _inputSpaceMenu()
    , _inputSpaceSet()
    , _availableViews()
    , _listingMutex()
    , _listing()
    , _proxyListing()
{
}

NativePluginDescription
NativeRead::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_READ;
    desc.label = "Read";
    desc.description = tr("Read an image or image sequence.").toStdString();
    desc.grouping = PLUGIN_GROUP_IMAGE;
    desc.majorVersion = PLUGIN_MAJOR_NATRON_READ;
    desc.minorVersion = 0;
    desc.outputKind = eDataKindImage;

    return desc;
}

void
NativeRead::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));
    KnobFilePtr filename = createKnob<KnobFile>(tr("File"));

    filename->setName(kOfxImageEffectFileParamName);
    filename->setAsInputImage();
    filename->setHintToolTip(tr("The image or image sequence to read."));
    // NativeEffectBase has no counterpart of an OpenFX clip-preferences slave param, so without
    // this the preferred metadata is only consulted once, at creation, before a file is chosen.
    filename->setIsMetadataSlave(true);
    page->addKnob(filename);
    _filename = filename;

    KnobFilePtr proxy = createKnob<KnobFile>(tr("Proxy File"));
    proxy->setName(kOfxImageEffectProxyParamName);
    proxy->setAsInputImage();
    proxy->setHintToolTip(tr("Filename of the proxy images. They are used instead of the images read from the File parameter "
                             "when the render scale is at or below the proxy threshold."));
    page->addKnob(proxy);
    _proxy = proxy;

    KnobDoublePtr proxyThreshold = createKnob<KnobDouble>(tr("Proxy threshold"), 2);
    proxyThreshold->setName(kKnobProxyThreshold);
    proxyThreshold->setHintToolTip(tr("When the render scale is at or below this scale, the proxy images are used instead of the "
                                      "original images. It is set from the scale of the proxy images when the proxy file is "
                                      "chosen, and can be edited by checking Custom Proxy Scale."));
    proxyThreshold->setDefaultValue(1., 0);
    proxyThreshold->setDefaultValue(1., 1);
    proxyThreshold->setMinimum(0.01, 0);
    proxyThreshold->setMinimum(0.01, 1);
    proxyThreshold->setMaximum(1., 0);
    proxyThreshold->setMaximum(1., 1);
    proxyThreshold->setDisplayMinimum(0.01, 0);
    proxyThreshold->setDisplayMinimum(0.01, 1);
    proxyThreshold->setDisplayMaximum(1., 0);
    proxyThreshold->setDisplayMaximum(1., 1);
    proxyThreshold->setAnimationEnabled(false);
    proxyThreshold->setAddNewLine(false);
    page->addKnob(proxyThreshold);
    _proxyThreshold = proxyThreshold;

    KnobBoolPtr customProxyScale = createKnob<KnobBool>(tr("Custom Proxy Scale"));
    customProxyScale->setName(kKnobCustomProxyScale);
    customProxyScale->setHintToolTip(tr("Check to edit the proxy threshold and the scale of the proxy images instead of "
                                        "following the proxy files."));
    customProxyScale->setDefaultValue(false);
    customProxyScale->setAnimationEnabled(false);
    customProxyScale->setEvaluateOnChange(false);
    page->addKnob(customProxyScale);
    _customProxyScale = customProxyScale;

    KnobDoublePtr originalProxyScale = createKnob<KnobDouble>(tr("Original Proxy Scale"), 2);
    originalProxyScale->setName(kKnobOriginalProxyScale);
    originalProxyScale->setHintToolTip(tr("The scale of the proxy images relative to the original images."));
    originalProxyScale->setDefaultValue(1., 0);
    originalProxyScale->setDefaultValue(1., 1);
    originalProxyScale->setMinimum(0.01, 0);
    originalProxyScale->setMinimum(0.01, 1);
    originalProxyScale->setMaximum(1., 0);
    originalProxyScale->setMaximum(1., 1);
    originalProxyScale->setDisplayMinimum(0.01, 0);
    originalProxyScale->setDisplayMinimum(0.01, 1);
    originalProxyScale->setDisplayMaximum(1., 0);
    originalProxyScale->setDisplayMaximum(1., 1);
    originalProxyScale->setAnimationEnabled(false);
    page->addKnob(originalProxyScale);
    _originalProxyScale = originalProxyScale;

    const std::vector<ChoiceOption> beforeAfterChoices = {
        ChoiceOption("hold", tr("Hold").toStdString(), tr("While outside the sequence, load the nearest end frame.").toStdString()),
        ChoiceOption("loop", tr("Loop").toStdString(), tr("Repeat the sequence outside its range.").toStdString()),
        ChoiceOption("bounce", tr("Bounce").toStdString(), tr("Repeat the sequence in reverse outside its range.").toStdString()),
        ChoiceOption("black", tr("Black").toStdString(), tr("Render a black image.").toStdString()),
        ChoiceOption("error", tr("Error").toStdString(), tr("Report an error.").toStdString()),
    };

    KnobIntPtr firstFrame = createKnob<KnobInt>(tr("First Frame"));
    firstFrame->setName(kKnobFirstFrame);
    firstFrame->setHintToolTip(tr("The first frame number to read from this image sequence. It cannot be less than the first "
                                  "frame of the sequence or greater than its last. If Starting Time is 1 or Time Offset is 0, "
                                  "this is also the first output frame."));
    firstFrame->setDefaultValue(0);
    firstFrame->setAnimationEnabled(false);
    firstFrame->setAddNewLine(false);
    page->addKnob(firstFrame);
    _firstFrame = firstFrame;

    KnobChoicePtr before = createKnob<KnobChoice>(tr("Before"));
    before->setName(kKnobBefore);
    before->setHintToolTip(tr("What to do before the first frame of the sequence."));
    before->populateChoices(beforeAfterChoices);
    before->setDefaultValue((int)ReadTimeDomain::eBeforeAfterHold);
    page->addKnob(before);
    _before = before;

    KnobIntPtr lastFrame = createKnob<KnobInt>(tr("Last Frame"));
    lastFrame->setName(kKnobLastFrame);
    lastFrame->setHintToolTip(tr("The last frame number to read from this image sequence. It cannot be less than the first "
                                 "frame of the sequence or greater than its last. If Starting Time is 1 or Time Offset is 0, "
                                 "this is also the last output frame."));
    lastFrame->setDefaultValue(0);
    lastFrame->setAnimationEnabled(false);
    lastFrame->setAddNewLine(false);
    page->addKnob(lastFrame);
    _lastFrame = lastFrame;

    KnobChoicePtr after = createKnob<KnobChoice>(tr("After"));
    after->setName(kKnobAfter);
    after->setHintToolTip(tr("What to do after the last frame of the sequence."));
    after->populateChoices(beforeAfterChoices);
    after->setDefaultValue((int)ReadTimeDomain::eBeforeAfterHold);
    page->addKnob(after);
    _after = after;

    KnobChoicePtr onMissing = createKnob<KnobChoice>(tr("On Missing Frame"));
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

    KnobChoicePtr frameMode = createKnob<KnobChoice>(tr("Frame Mode"));
    frameMode->setName(kKnobFrameMode);
    std::vector<ChoiceOption> modeChoices;
    modeChoices.push_back(ChoiceOption("startingTime", tr("Starting Time").toStdString(), tr("Set at what output frame the first sequence frame is output. The sequence frame designated by First Frame is output at that frame.").toStdString()));
    modeChoices.push_back(ChoiceOption("timeOffset", tr("Time Offset").toStdString(), tr("Set an offset to be applied as a number of frames. The sequence frame designated by First Frame is output at First Frame + Time Offset.").toStdString()));
    frameMode->populateChoices(modeChoices);
    frameMode->setDefaultValue((int)ReadTimeDomain::eFrameModeStartingTime);
    frameMode->setAnimationEnabled(false);
    frameMode->setAddNewLine(false);
    page->addKnob(frameMode);
    _frameMode = frameMode;

    KnobIntPtr startingTime = createKnob<KnobInt>(tr("Starting Time"));
    startingTime->setName(kKnobStartingTime);
    startingTime->setHintToolTip(tr("At what time (on the timeline) this sequence starts."));
    startingTime->setDefaultValue(0);
    startingTime->setAnimationEnabled(false);
    startingTime->setAddNewLine(false);
    page->addKnob(startingTime);
    _startingTime = startingTime;

    KnobIntPtr timeOffset = createKnob<KnobInt>(tr("Time Offset"));
    timeOffset->setName(kKnobTimeOffset);
    timeOffset->setHintToolTip(tr("Offset applied to the sequence in time units (frames)."));
    timeOffset->setDefaultValue(0);
    timeOffset->setAnimationEnabled(false);
    page->addKnob(timeOffset);
    _timeOffset = timeOffset;

    KnobBoolPtr userEdited = createKnob<KnobBool>(tr("Time Domain User Edited"));
    userEdited->setName(kKnobTimeDomainUserEdited);
    userEdited->setDefaultValue(false);
    userEdited->setAnimationEnabled(false);
    userEdited->setEvaluateOnChange(false);
    userEdited->setSecret(true);
    page->addKnob(userEdited);
    _timeDomainUserEdited = userEdited;

    KnobIntPtr originalRange = createKnob<KnobInt>(tr("Original Range"), 2);
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

    KnobDoublePtr frameRate = createKnob<KnobDouble>(tr("Frame rate"));
    frameRate->setName(kKnobFrameRate);
    frameRate->setHintToolTip(tr("By default this value is guessed from the file. You can override it by checking Custom FPS. "
                                 "The frame rate is only metadata passed to the nodes downstream."));
    frameRate->setDefaultValue(24.);
    frameRate->setMinimum(0.);
    frameRate->setDisplayMinimum(0.);
    frameRate->setDisplayMaximum(300.);
    frameRate->setAnimationEnabled(false);
    frameRate->setEvaluateOnChange(false);
    frameRate->setIsMetadataSlave(true);
    frameRate->setAddNewLine(false);
    page->addKnob(frameRate);
    _frameRate = frameRate;

    KnobBoolPtr customFps = createKnob<KnobBool>(tr("Custom FPS"));
    customFps->setName(kKnobCustomFps);
    customFps->setHintToolTip(tr("If checked, the Frame rate value can be set freely instead of following the file."));
    customFps->setDefaultValue(false);
    customFps->setAnimationEnabled(false);
    customFps->setEvaluateOnChange(false);
    customFps->setIsMetadataSlave(true);
    page->addKnob(customFps);
    _customFps = customFps;

    KnobStringPtr availableViews = createKnob<KnobString>(tr("Available Views"));
    availableViews->setName(kReadOIIOAvailableViewsKnobName);
    availableViews->setAnimationEnabled(false);
    availableViews->setEvaluateOnChange(false);
    availableViews->setIsPersistent(false);
    availableViews->setSecret(true);
    page->addKnob(availableViews);
    _availableViews = availableViews;

    KnobStringPtr ocioConfigFile = createKnob<KnobString>(tr("OCIO Config File"));
    ocioConfigFile->setName(kKnobOCIOConfigFile);
    ocioConfigFile->setAnimationEnabled(false);
    // The project pushes its config to every node after creation and after a load.
    ocioConfigFile->setIsPersistent(false);
    ocioConfigFile->setSecret(true);
    page->addKnob(ocioConfigFile);
    _ocioConfigFile = ocioConfigFile;

    KnobStringPtr ocioWorkingSpace = createKnob<KnobString>(tr("Working Colorspace"));
    ocioWorkingSpace->setName(kKnobOCIOWorkingSpace);
    ocioWorkingSpace->setAnimationEnabled(false);
    ocioWorkingSpace->setIsPersistent(false);
    ocioWorkingSpace->setSecret(true);
    page->addKnob(ocioWorkingSpace);
    _ocioWorkingSpace = ocioWorkingSpace;

    KnobStringPtr inputSpace = createKnob<KnobString>(tr("Input Colorspace"));
    inputSpace->setName(kNatronReadNodeOCIOParamInputSpace);
    inputSpace->setHintToolTip(tr("The OpenColorIO colorspace of the file. The image is converted from it to the project's "
                                  "working space. It is shown only when the project's config has no colorspace of that name."));
    inputSpace->setAnimationEnabled(false);
    inputSpace->setSecret(true);
    page->addKnob(inputSpace);
    _inputSpace = inputSpace;

    KnobChoicePtr inputSpaceMenu = createKnob<KnobChoice>(tr("Input Colorspace"));
    inputSpaceMenu->setName(kKnobOCIOInputSpaceIndex);
    inputSpaceMenu->setHintToolTip(tr("The OpenColorIO colorspace of the file. The image is converted from it to the project's "
                                      "working space. A new Read takes the colorspace the config's file rules give the file, else the "
                                      "colorspace tagged in the file, else the project's default for files of its kind. The "
                                      "config's default rule does not count."));
    inputSpaceMenu->setAnimationEnabled(false);
    inputSpaceMenu->setIsPersistent(false);
    // Only the input space string it sets decides the render.
    inputSpaceMenu->setEvaluateOnChange(false);
    page->addKnob(inputSpaceMenu);
    _inputSpaceMenu = inputSpaceMenu;

    KnobBoolPtr inputSpaceSet = createKnob<KnobBool>(tr("Input Colorspace Set"));
    inputSpaceSet->setName(kKnobOCIOInputSpaceSet);
    inputSpaceSet->setDefaultValue(false);
    inputSpaceSet->setAnimationEnabled(false);
    inputSpaceSet->setEvaluateOnChange(false);
    inputSpaceSet->setSecret(true);
    page->addKnob(inputSpaceSet);
    _inputSpaceSet = inputSpaceSet;

    refreshInputSpaceMenu();
    refreshTimeKnobState();
    refreshProxyKnobState();
}

void
NativeRead::onKnobsLoaded()
{
    refreshInputSpaceMenu();
    refreshTimeKnobState();
    refreshProxyKnobState();
    refreshAvailableViews(true);
}

void
NativeRead::refreshAvailableViews(bool silent)
{
    KnobStringPtr knob = _availableViews.lock();

    if (!knob) {
        return;
    }
    // A single view needs no project view of its own, so only a file of several lists them.
    std::string value;
    const std::string path = representativePath();
    if (!path.empty()) {
        std::string error;
        const std::shared_ptr<const OiioReadSupport::Header> header = OiioReadSupport::readHeader(path, &error);
        if (header) {
            const std::vector<std::string> names = OiioReadSupport::viewNames(*header);
            if (names.size() > 1) {
                for (std::size_t i = 0; i < names.size(); ++i) {
                    value += (i > 0 ? "," : "") + names[i];
                }
            }
        }
    }
    if (silent) {
        // A project being loaded already holds its views, so it is not asked to create them.
        knob->blockValueChanges();
        knob->setValue(value);
        knob->unblockValueChanges();
        getNode()->refreshCreatedViews(true);
    } else if (value != knob->getValue()) {
        knob->setValue(value);
    }
}

std::string
NativeRead::projectViewName(ViewIdx view) const
{
    const std::vector<std::string> names = getApp()->getProject()->getProjectViewNames();
    const int index = view.value();

    return (index >= 0 && (std::size_t)index < names.size()) ? names[(std::size_t)index] : std::string();
}

void
NativeRead::onEffectCreated(bool mayCreateFileDialog,
                            const CreateNodeArgs& /*args*/)
{
    KnobFilePtr filename = _filename.lock();

    if (!mayCreateFileDialog || !filename || !filename->getValue().empty() || getApp()->isBackground()) {
        return;
    }
    const std::string path = getApp()->openImageFileDialog();
    if (path.empty()) {
        throw std::runtime_error("");
    }
    filename->setValue(path);
}

bool
NativeRead::rejectUnsupportedFormat(const std::string& path)
{
    if (OiioReadSupport::isReadablePath(path)) {
        return false;
    }
    setPersistentMessage(eMessageTypeError, tr("Unsupported format: %1").arg(QString::fromStdString(path)).toStdString());

    return true;
}

void
NativeRead::refreshTimeKnobState()
{
    KnobChoicePtr frameMode = _frameMode.lock();
    KnobIntPtr startingTime = _startingTime.lock();
    KnobIntPtr timeOffset = _timeOffset.lock();
    KnobDoublePtr frameRate = _frameRate.lock();
    KnobBoolPtr customFps = _customFps.lock();

    if (frameMode && startingTime && timeOffset) {
        const bool byStartingTime = frameMode->getValue() == (int)ReadTimeDomain::eFrameModeStartingTime;
        startingTime->setSecret(!byStartingTime);
        timeOffset->setSecret(byStartingTime);
    }
    if (frameRate && customFps) {
        frameRate->setEnabled(0, customFps->getValue());
    }
}

std::shared_ptr<const NativeRead::FrameListing>
NativeRead::frameListing(bool proxy) const
{
    KnobFilePtr filename = proxy ? _proxy.lock() : _filename.lock();
    std::shared_ptr<const FrameListing>& cached = proxy ? _proxyListing : _listing;
    std::string pattern;

    if (filename) {
        pattern = filename->getValue();
        getApp()->getProject()->canonicalizePath(pattern);
    }
    {
        std::lock_guard<std::mutex> lock(_listingMutex);
        if (cached && cached->pattern == pattern) {
            return cached;
        }
    }

    std::shared_ptr<FrameListing> listing = std::make_shared<FrameListing>();
    listing->pattern = pattern;
    if (!pattern.empty()) {
        std::map<int, std::map<int, std::string>> sequence;
        FileSystemModel::filesListFromPattern(pattern, &sequence);
        if (sequence.size() > 1) {
            listing->singleImage = false;
            for (std::map<int, std::map<int, std::string>>::const_iterator it = sequence.begin(); it != sequence.end(); ++it) {
                listing->frames.insert(it->first);
            }
        } else if (sequence.size() == 1 && !sequence.begin()->second.empty()) {
            listing->singlePath = sequence.begin()->second.begin()->second;
        }
    }

    std::lock_guard<std::mutex> lock(_listingMutex);
    cached = listing;

    return listing;
}

void
NativeRead::invalidateFrameListing()
{
    std::lock_guard<std::mutex> lock(_listingMutex);
    _listing.reset();
    _proxyListing.reset();
}

ReadTimeDomain::Settings
NativeRead::settingsAt(double time) const
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

NativeRead::Target
NativeRead::targetAtTime(double time,
                         bool proxy) const
{
    Target target;
    KnobFilePtr filename = proxy ? _proxy.lock() : _filename.lock();

    if (!filename || filename->getValue().empty()) {
        return target;
    }

    const std::shared_ptr<const FrameListing> listing = frameListing(proxy);
    const ReadTimeDomain::Settings settings = settingsAt(time);
    ReadTimeDomain::Result result;
    if (listing->singleImage) {
        // A still image answers every frame of the range, so the range itself stands for the
        // frames on disk.
        std::set<int> inRange;
        if ((long long)settings.lastFrame - settings.firstFrame < kMaxStillFrames) {
            for (int f = settings.firstFrame; f <= settings.lastFrame; ++f) {
                inRange.insert(inRange.end(), f);
            }
        } else {
            inRange.insert(settings.firstFrame);
            inRange.insert(settings.lastFrame);
        }
        result = ReadTimeDomain::resolve(time, settings, inRange);
    } else {
        result = ReadTimeDomain::resolve(time, settings, listing->frames);
    }

    target.kind = result.kind;
    target.message = result.message;
    target.frame = result.frame;
    if (result.kind == ReadTimeDomain::Result::eFile) {
        target.path = (listing->singleImage && !listing->singlePath.empty()) ? listing->singlePath : filename->getFileName(result.frame, ViewIdx(0));
    }

    return target;
}

std::string
NativeRead::representativePath(bool proxy) const
{
    KnobFilePtr filename = proxy ? _proxy.lock() : _filename.lock();

    if (!filename || filename->getValue().empty()) {
        return std::string();
    }
    const std::shared_ptr<const FrameListing> listing = frameListing(proxy);
    if (!listing->singleImage) {
        return filename->getFileName(*listing->frames.begin(), ViewIdx(0));
    }

    return listing->singlePath.empty() ? filename->getFileName(0, ViewIdx(0)) : listing->singlePath;
}

namespace {
unsigned int
levelOfScale(double scale)
{
    if (!(scale > 0.)) {
        return 0;
    }

    return Image::getLevelFromScale(std::min(scale, 1.));
}
} // anonymous namespace

void
NativeRead::refreshProxyKnobState()
{
    KnobFilePtr proxy = _proxy.lock();
    KnobDoublePtr threshold = _proxyThreshold.lock();
    KnobDoublePtr original = _originalProxyScale.lock();
    KnobBoolPtr custom = _customProxyScale.lock();

    if (!proxy || !threshold || !original || !custom) {
        return;
    }
    const bool hasProxy = !proxy->getValue().empty();
    const bool isCustom = custom->getValue();
    threshold->setSecret(!hasProxy);
    custom->setSecret(!hasProxy);
    original->setSecret(!hasProxy || !isCustom);
    threshold->setAllDimensionsEnabled(isCustom);
    original->setAllDimensionsEnabled(isCustom);
}

void
NativeRead::refreshProxyScale()
{
    KnobDoublePtr threshold = _proxyThreshold.lock();
    KnobDoublePtr original = _originalProxyScale.lock();
    KnobBoolPtr custom = _customProxyScale.lock();

    if (!threshold || !original || !custom || custom->getValue()) {
        return;
    }

    double x = 1.;
    double y = 1.;
    const std::string proxyPath = representativePath(true);
    const std::string fullPath = representativePath(false);
    if (!proxyPath.empty() && !fullPath.empty()) {
        std::string error;
        const std::shared_ptr<const OiioReadSupport::Header> fullHeader = OiioReadSupport::readHeader(fullPath, &error);
        const std::shared_ptr<const OiioReadSupport::Header> proxyHeader = OiioReadSupport::readHeader(proxyPath, &error);
        if (fullHeader && proxyHeader && !fullHeader->subimages.empty() && !proxyHeader->subimages.empty()) {
            const OIIO::ImageSpec& fullSpec = fullHeader->subimages[0];
            const OIIO::ImageSpec& proxySpec = proxyHeader->subimages[0];
            const RectI fullWindow = OiioReadSupport::dataWindowOf(fullSpec);
            const RectI proxyWindow = OiioReadSupport::dataWindowOf(proxySpec);
            if (fullWindow.width() > 0 && fullWindow.height() > 0 && proxyWindow.width() > 0 && proxyWindow.height() > 0) {
                x = (proxyWindow.width() * pixelAspectOf(proxySpec)) / (fullWindow.width() * pixelAspectOf(fullSpec));
                y = proxyWindow.height() / (double)fullWindow.height();
            }
        }
    }
    x = std::min(x, 1.);
    y = std::min(y, 1.);
    original->setValue(x, ViewSpec::all(), 0);
    original->setValue(y, ViewSpec::all(), 1);
    threshold->setValue(x, ViewSpec::all(), 0);
    threshold->setValue(y, ViewSpec::all(), 1);
}

bool
NativeRead::proxyKnobChanged(KnobI* k,
                             ValueChangedReasonEnum reason)
{
    KnobFilePtr proxy = _proxy.lock();
    KnobBoolPtr custom = _customProxyScale.lock();

    if (proxy && k == proxy.get()) {
        if (reason != eValueChangedReasonTimeChanged) {
            invalidateFrameListing();
            refreshProxyScale();
            refreshProxyKnobState();
        }

        return true;
    }
    if (custom && k == custom.get()) {
        refreshProxyScale();
        refreshProxyKnobState();

        return true;
    }

    return false;
}

bool
NativeRead::proxySourceAt(double time,
                          unsigned int level,
                          Target* source,
                          unsigned int* fileLevel) const
{
    KnobDoublePtr threshold = _proxyThreshold.lock();
    KnobDoublePtr original = _originalProxyScale.lock();
    KnobFilePtr proxy = _proxy.lock();

    if (!threshold || !original || !proxy || proxy->getValue().empty()) {
        return false;
    }
    const unsigned int thresholdLevel = levelOfScale(std::min(threshold->getValue(0), threshold->getValue(1)));
    const unsigned int proxyLevel = levelOfScale(std::min(original->getValue(0), original->getValue(1)));
    if (level < thresholdLevel || level < proxyLevel) {
        return false;
    }
    const Target proxyTarget = targetAtTime(time, true);
    if (proxyTarget.kind != ReadTimeDomain::Result::eFile) {
        return false;
    }
    *source = proxyTarget;
    *fileLevel = proxyLevel;

    return true;
}

double
NativeRead::fileFrameRate() const
{
    const std::string path = representativePath();

    if (path.empty()) {
        return 0.;
    }
    std::string error;
    const std::shared_ptr<const OiioReadSupport::Header> header = OiioReadSupport::readHeader(path, &error);

    return (header && !header->subimages.empty()) ? framesPerSecondOf(header->subimages[0]) : 0.;
}

void
NativeRead::refreshFrameRateFromFile()
{
    KnobDoublePtr frameRate = _frameRate.lock();
    KnobBoolPtr customFps = _customFps.lock();

    if (!frameRate || !customFps || customFps->getValue()) {
        return;
    }
    const double fps = fileFrameRate();
    if (fps > 0.) {
        frameRate->setValue(fps);
    }
}

void
NativeRead::getFrameRange(double* first,
                          double* last)
{
    KnobFilePtr filename = _filename.lock();

    if (!filename || filename->getValue().empty()) {
        *first = std::numeric_limits<int>::min();
        *last = std::numeric_limits<int>::max();

        return;
    }
    const ReadTimeDomain::FrameRange range = ReadTimeDomain::outputRange(settingsAt(0.));
    *first = range.min;
    *last = range.max;
}

std::string
NativeRead::workingSpaceName() const
{
    KnobStringPtr knob = _ocioWorkingSpace.lock();
    const std::string pushed = knob ? knob->getValue() : std::string();

    if (!pushed.empty()) {
        return pushed;
    }
    AppInstancePtr app = getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();

    return project ? project->getWorkingColorSpace() : std::string();
}

void
NativeRead::refreshInputSpaceMenu()
{
    KnobChoicePtr menu = _inputSpaceMenu.lock();
    KnobStringPtr space = _inputSpace.lock();
    AppInstancePtr app = getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    ProjectColorManagementPtr colorManagement = project ? project->getColorManagement() : ProjectColorManagementPtr();

    if (!menu || !space || !colorManagement) {
        return;
    }
    const std::vector<ChoiceOption> options = Project::colorSpaceOptions(*colorManagement);
    menu->populateChoices(options);

    // A Read without a file converts nothing, which the menu shows as the working space.
    const std::string value = space->getValue();
    const std::string shown = ReadColorSpace::nameInConfig(colorManagement->getConfig(), value.empty() ? workingSpaceName() : value);
    int index = -1;
    for (std::size_t i = 0; i < options.size(); ++i) {
        if (!shown.empty() && options[i].id == shown) {
            index = (int)i;
            break;
        }
    }
    if (index >= 0 && menu->getValue() != index) {
        menu->setValue(index);
    }
    // A name the config lacks cannot be shown by the menu, so the string itself is shown.
    space->setSecret(value.empty() || index >= 0);
}

void
NativeRead::guessInputSpace()
{
    KnobStringPtr space = _inputSpace.lock();
    AppInstancePtr app = getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();

    if (!space || !project) {
        return;
    }
    const std::string path = representativePath();
    if (path.empty()) {
        return;
    }
    std::string error;
    const std::shared_ptr<const OiioReadSupport::Header> header = OiioReadSupport::readHeader(path, &error);
    if (!header || header->subimages.empty()) {
        return;
    }
    const std::string guess = ReadColorSpace::defaultInputSpace(*project, path, header->subimages[0]);
    if (!guess.empty() && guess != space->getValue()) {
        space->setValue(guess);
    }
}

bool
NativeRead::colourKnobChanged(KnobI* k,
                              ValueChangedReasonEnum reason)
{
    KnobFilePtr filename = _filename.lock();
    KnobStringPtr configFile = _ocioConfigFile.lock();
    KnobStringPtr workingSpace = _ocioWorkingSpace.lock();
    KnobStringPtr space = _inputSpace.lock();
    KnobChoicePtr menu = _inputSpaceMenu.lock();
    KnobBoolPtr spaceSet = _inputSpaceSet.lock();

    if (!filename || !configFile || !workingSpace || !space || !menu || !spaceSet || reason == eValueChangedReasonTimeChanged) {
        return false;
    }
    const bool byUser = reason == eValueChangedReasonUserEdited;

    if (k == filename.get()) {
        // A loaded or pasted node keeps the input space it was saved with.
        AppInstancePtr app = getApp();
        const bool restoring = app && (app->isCreatingNodeTree() || (app->getProject() && app->getProject()->isLoadingProject()));
        if (!spaceSet->getValue() && !restoring) {
            guessInputSpace();
        }

        return false;
    }
    if (k == configFile.get() || k == workingSpace.get()) {
        refreshInputSpaceMenu();

        return true;
    }
    if (k == menu.get()) {
        const int index = menu->getValue();
        if (byUser && index >= 0 && index < menu->getNumEntries()) {
            spaceSet->setValue(true);
            space->setValue(menu->getEntry(index).id);
        }

        return true;
    }
    if (k == space.get()) {
        if (byUser) {
            spaceSet->setValue(true);
        }
        refreshInputSpaceMenu();

        return true;
    }

    return false;
}

bool
NativeRead::knobChanged(KnobI* k,
                        ValueChangedReasonEnum reason,
                        ViewSpec /*view*/,
                        double /*time*/,
                        bool /*originatedFromMainThread*/)
{
    if (proxyKnobChanged(k, reason)) {
        return true;
    }
    if (colourKnobChanged(k, reason)) {
        return true;
    }
    KnobFilePtr filename = _filename.lock();
    KnobIntPtr originalRange = _originalFrameRange.lock();
    KnobIntPtr firstFrame = _firstFrame.lock();
    KnobIntPtr lastFrame = _lastFrame.lock();
    KnobChoicePtr frameMode = _frameMode.lock();
    KnobIntPtr startingTime = _startingTime.lock();
    KnobIntPtr timeOffset = _timeOffset.lock();
    KnobBoolPtr userEdited = _timeDomainUserEdited.lock();
    KnobBoolPtr customFps = _customFps.lock();

    if (!filename || !originalRange || !firstFrame || !lastFrame || !frameMode || !startingTime || !timeOffset || !userEdited || !customFps) {
        return false;
    }
    const bool byUser = reason == eValueChangedReasonUserEdited;

    if (k == filename.get()) {
        if (reason != eValueChangedReasonTimeChanged) {
            invalidateFrameListing();
            invalidateOutputMetadata();
            refreshAvailableViews(false);
            refreshFrameRateFromFile();
            refreshProxyScale();
        }

        return true;
    }
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
        if (!userEdited->getValue()) {
            firstFrame->setValue(originalFirst);
            firstFrame->setDefaultValueWithoutApplying(originalFirst);
            lastFrame->setValue(originalLast);
            lastFrame->setDefaultValueWithoutApplying(originalLast);
            startingTime->setValue(originalFirst);
            startingTime->setDefaultValueWithoutApplying(originalFirst);
            timeOffset->setValue(0);
        }

        return true;
    }
    if (k == firstFrame.get() && byUser) {
        lastFrame->setDisplayMinimum(firstFrame->getValue());
        startingTime->setValue(firstFrame->getValue() + timeOffset->getValue());
        userEdited->setValue(true);

        return true;
    }
    if (k == lastFrame.get() && byUser) {
        firstFrame->setDisplayMaximum(lastFrame->getValue());
        userEdited->setValue(true);

        return true;
    }
    if (k == frameMode.get() && byUser) {
        refreshTimeKnobState();

        return true;
    }
    if (k == startingTime.get() && byUser) {
        timeOffset->setValue(startingTime->getValue() - firstFrame->getValue());
        userEdited->setValue(true);

        return true;
    }
    if (k == timeOffset.get() && byUser) {
        startingTime->setValue(timeOffset->getValue() + firstFrame->getValue());
        userEdited->setValue(true);

        return true;
    }
    if (k == customFps.get()) {
        refreshTimeKnobState();
        refreshFrameRateFromFile();

        return true;
    }

    return false;
}

StatusEnum
NativeRead::getPreferredMetadata(NodeMetadata& metadata)
{
    metadata.setBitDepth(-1, eImageBitDepthFloat);
    metadata.setComponentsType(-1, kNatronColorLayerID);
    metadata.setNComps(-1, 4);

    metadata.setIsFrameVarying(true);

    KnobDoublePtr frameRate = _frameRate.lock();
    KnobBoolPtr customFps = _customFps.lock();
    if (frameRate) {
        double fps = frameRate->getValue();
        if (!customFps || !customFps->getValue()) {
            const double fileFps = fileFrameRate();
            if (fileFps > 0.) {
                fps = fileFps;
            }
        }
        if (fps > 0.) {
            metadata.setOutputFrameRate(fps);
        }
    }

    // The format is one per node, so a frame that loads nothing still reports the sequence's.
    const Target target = targetAtTime(getCurrentTime());
    const std::string path = target.kind == ReadTimeDomain::Result::eFile ? target.path : representativePath();
    if (path.empty()) {
        return eStatusOK;
    }
    std::string error;
    const std::shared_ptr<const OiioReadSupport::Header> header = OiioReadSupport::readHeader(path, &error);
    if (!header || header->subimages.empty()) {
        return eStatusOK;
    }
    const OIIO::ImageSpec& spec = header->subimages[0];

    // OpenFX formats must start at (0, 0), so a positive display-window origin only widens the
    // format. Mirroring the display window within itself top to bottom always lands it at
    // [0, full_height), which is why full_y drops out.
    metadata.setOutputFormat(RectI(0, 0, spec.full_x + spec.full_width, spec.full_height));
    metadata.setPixelAspectRatio(-1, pixelAspectOf(spec));
    metadata.setNComps(-1, ColourChannels::of(spec).nComps());

    return eStatusOK;
}

ImageMetadata
NativeRead::deriveOutputMetadata(double time,
                                 ViewIdx /*view*/)
{
    ImageMetadata metadata;

    metadata.setDouble(kOfxMetadataKeyFrameRate, getFrameRate());
    metadata.setDouble(kOfxMetadataKeyPixelAspect, getAspectRatio(-1));

    // A frame that loads black or an error has no file to describe, and a key whose value is
    // unknown is left out rather than published empty.
    const Target target = targetAtTime(time);
    if (target.kind != ReadTimeDomain::Result::eFile) {
        return metadata;
    }
    const QFileInfo info(QString::fromStdString(target.path));
    if (!info.isFile()) {
        return metadata;
    }
    metadata.setString(kOfxMetadataKeyFilePath, target.path);
    metadata.setInt(kOfxMetadataKeySourceFrame, target.frame);
    metadata.setDouble(kOfxMetadataKeyMTime, info.lastModified().toMSecsSinceEpoch() / 1000.);
    metadata.setDouble(kOfxMetadataKeyFileSize, (double)info.size());

    std::string error;
    const std::shared_ptr<const OiioReadSupport::Header> header = OiioReadSupport::readHeader(target.path, &error);
    if (header && !header->subimages.empty()) {
        const OIIO::ImageSpec& spec = header->subimages[0];
        metadata.setDouble(kOfxMetadataKeyPixelAspect, pixelAspectOf(spec));
        metadata.merge(OiioReadSupport::attributeMetadata(target.path, spec), ImageMetadata::eMergePreferThis);
    }

    return metadata;
}

StatusEnum
NativeRead::getRegionOfDefinition(U64 /*hash*/,
                                  double time,
                                  const RenderScale& /*scale*/,
                                  ViewIdx view,
                                  RectD* rod)
{
    const Target target = targetAtTime(time);

    if (target.kind == ReadTimeDomain::Result::eError) {
        setPersistentMessage(eMessageTypeError, target.message);

        return eStatusFailed;
    }
    const std::string path = target.kind == ReadTimeDomain::Result::eFile ? target.path : representativePath();

    if (path.empty()) {
        // The base implementation leaves the RoD null for a node without inputs, and a null RoD
        // makes renderRoI return no planes at all.
        Format format;
        getApp()->getProject()->getProjectDefaultFormat(&format);
        *rod = format.toCanonicalFormat();
        clearPersistentMessage(false);

        return eStatusOK;
    }
    if (rejectUnsupportedFormat(path)) {
        return eStatusFailed;
    }

    std::string error;
    const std::shared_ptr<const OiioReadSupport::Header> header = OiioReadSupport::readHeader(path, &error);
    if (!header || header->subimages.empty()) {
        setPersistentMessage(eMessageTypeError, error.empty() ? tr("Could not read %1").arg(QString::fromStdString(path)).toStdString() : error);

        return eStatusFailed;
    }
    const OIIO::ImageSpec& spec = header->subimages[(std::size_t)colourSubimageOf(*header, projectViewName(view))];
    *rod = OiioReadSupport::dataWindowOf(spec).toCanonical_noClipping(0, pixelAspectOf(spec));
    clearPersistentMessage(false);

    return eStatusOK;
}

void
NativeRead::getComponentsNeededAndProduced(double time,
                                           ViewIdx view,
                                           EffectInstance::ComponentsNeededMap* comps,
                                           double* passThroughTime,
                                           int* passThroughView,
                                           int* passThroughInputNb)
{
    EffectInstance::getComponentsNeededAndProduced(time, view, comps, passThroughTime, passThroughView, passThroughInputNb);

    // A frame that loads nothing still lists the sequence's layers, so that the layers a graph
    // can select do not come and go with the frame.
    const Target target = targetAtTime(time);
    const std::string path = target.kind == ReadTimeDomain::Result::eFile ? target.path : representativePath();
    if (path.empty()) {
        return;
    }
    std::string error;
    const std::shared_ptr<const OiioReadSupport::Header> header = OiioReadSupport::readHeader(path, &error);
    if (!header) {
        return;
    }
    std::vector<OiioReadSupport::FileLayer> layers;
    OiioReadSupport::fileLayers(*header, &layers, projectViewName(view));
    std::list<ImageLayerDesc>& produced = (*comps)[-1];
    for (std::vector<OiioReadSupport::FileLayer>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        produced.push_back(it->desc);
    }
}

namespace {
struct ReadPlane {
    Image* target;
    RectI rect;
    std::string path;
    int subimage;
    std::vector<int> fileChannels; // one per component, -1 for a component the file lacks
    OCIO_NAMESPACE::ConstCPUProcessorRcPtr toWorking; // null when the plane is not converted
};

// Decodes the rows [y1, y2) of plane.rect into plane.target, through a staging buffer holding
// the file channels from the first to the last one the plane uses.
bool
decodeBand(const ReadPlane& plane,
           Image::WriteAccess& access,
           int nComps,
           int y1,
           int y2,
           std::string* error)
{
    int lo = -1;
    int hi = -1;
    for (int c = 0; c < nComps; ++c) {
        const int idx = plane.fileChannels[(std::size_t)c];
        if (idx < 0) {
            continue;
        }
        lo = lo < 0 ? idx : std::min(lo, idx);
        hi = std::max(hi, idx);
    }
    const int width = plane.rect.width();
    if (lo < 0) {
        for (int y = y1; y < y2; ++y) {
            std::memset(access.pixelAt(plane.rect.x1, y), 0, (std::size_t)width * nComps * sizeof(float));
        }

        return true;
    }

    const int span = hi - lo + 1;
    const RectI window(plane.rect.x1, y1, plane.rect.x2, y2);
    std::vector<float> staging((std::size_t)width * (y2 - y1) * span);
    if (!OiioReadSupport::decode(plane.path, plane.subimage, lo, hi + 1, window, staging.data(), (std::size_t)width * span, error)) {
        return false;
    }
    for (int y = y1; y < y2; ++y) {
        const float* src = staging.data() + (std::size_t)(y - y1) * width * span;
        float* dst = (float*)access.pixelAt(plane.rect.x1, y);
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < nComps; ++c) {
                const int idx = plane.fileChannels[(std::size_t)c];
                dst[c] = idx < 0 ? 0.f : src[idx - lo];
            }
            src += span;
            dst += nComps;
        }
    }
    if (plane.toWorking) {
        for (int y = y1; y < y2; ++y) {
            if (!ReadColorSpace::convertRow(*plane.toWorking, (float*)access.pixelAt(plane.rect.x1, y), width, nComps, error)) {
                return false;
            }
        }
    }

    return true;
}

void
zeroRows(Image::WriteAccess& access,
         const RectI& area,
         int nComps)
{
    for (int y = area.y1; y < area.y2; ++y) {
        std::memset(access.pixelAt(area.x1, y), 0, (std::size_t)area.width() * nComps * sizeof(float));
    }
}
} // anonymous namespace

StatusEnum
NativeRead::render(const RenderActionArgs& args)
{
    const Target target = targetAtTime(args.time);

    if (target.kind == ReadTimeDomain::Result::eError) {
        setPersistentMessage(eMessageTypeError, target.message);

        return eStatusFailed;
    }
    Target source = target;
    unsigned int fileLevel = 0;
    if (target.kind == ReadTimeDomain::Result::eFile) {
        proxySourceAt(args.time, args.mappedScale.toMipmapLevel(), &source, &fileLevel);
    }
    const std::string path = source.path;

    std::string error;
    std::shared_ptr<const OiioReadSupport::Header> header;
    if (source.kind == ReadTimeDomain::Result::eFile) {
        if (rejectUnsupportedFormat(path)) {
            return eStatusFailed;
        }
        header = OiioReadSupport::readHeader(path, &error);
        if (!header || header->subimages.empty()) {
            setPersistentMessage(eMessageTypeError, error.empty() ? tr("Could not read %1").arg(QString::fromStdString(path)).toStdString() : error);

            return eStatusFailed;
        }
    }
    const std::string viewName = projectViewName(args.view);
    std::vector<OiioReadSupport::FileLayer> fileLayers;
    std::vector<OiioReadSupport::PartChannels> colourParts;
    if (header) {
        OiioReadSupport::fileLayers(*header, &fileLayers, viewName);
        OiioReadSupport::viewParts(*header, viewName, &colourParts);
    }

    OCIO_NAMESPACE::ConstCPUProcessorRcPtr toWorking;
    if (header) {
        KnobStringPtr inputSpace = _inputSpace.lock();
        std::string conversionError;
        if (!ReadColorSpace::toWorkingProcessor(*getApp()->getProject(), inputSpace ? inputSpace->getValue() : std::string(), workingSpaceName(), &toWorking, &conversionError)) {
            setPersistentMessage(eMessageTypeError, conversionError);

            return eStatusFailed;
        }
    }

    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    RenderCancellation cancel(this);
    std::atomic<bool> failed(false);
    std::mutex errorMutex;

    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        const ImagePtr& image = it->second;
        if (!image) {
            continue;
        }
        if (image->getBitDepth() != eImageBitDepthFloat) {
            return eStatusFailed;
        }

        const int nComps = (int)image->getComponentsCount();
        const RectI roi = args.roi;
        const unsigned int level = args.mappedScale.toMipmapLevel();
        const unsigned int reduce = level - fileLevel;

        RectI dataWindow;
        int subimage = 0;
        std::vector<int> fileChannels((std::size_t)nComps, -1);
        if (header && it->first.isColorLayer()) {
            if (!colourParts.empty()) {
                subimage = colourParts[0].subimage;
                dataWindow = OiioReadSupport::dataWindowOf(header->subimages[(std::size_t)subimage]);
                const ColourChannels colour = ColourChannels::of(colourParts[0].names, colourParts[0].index);
                for (int c = 0; c < nComps; ++c) {
                    fileChannels[(std::size_t)c] = colour.fileChannelForComponent(nComps, c);
                }
            }
        } else if (header) {
            // Matched by channel name: the plane asked for may be the registry's union of this
            // layer over several files, with channels this file lacks.
            const std::vector<std::string>& wanted = image->getComponents().getChannels();
            for (std::vector<OiioReadSupport::FileLayer>::const_iterator layer = fileLayers.begin(); layer != fileLayers.end(); ++layer) {
                if (layer->desc.getLayerID() != it->first.getLayerID()) {
                    continue;
                }
                subimage = layer->subimage;
                dataWindow = OiioReadSupport::dataWindowOf(header->subimages[(std::size_t)subimage]);
                const std::vector<std::string>& present = layer->desc.getChannels();
                for (int c = 0; c < nComps && (std::size_t)c < wanted.size(); ++c) {
                    const std::vector<std::string>::const_iterator found = std::find(present.begin(), present.end(), wanted[(std::size_t)c]);
                    if (found != present.end()) {
                        fileChannels[(std::size_t)c] = layer->channels[(std::size_t)(found - present.begin())];
                    }
                }
                break;
            }
        }

        // At a coarser level the rows behind the window are decoded at full size, then reduced.
        ImagePtr full;
        const RectI fullRect = roi.upscalePowerOfTwo(reduce).intersect(dataWindow);
        Image* target = image.get();
        if (reduce > 0 && !fullRect.isNull()) {
            full = std::make_shared<Image>(image->getComponents(), image->getRoD(), fullRect, fileLevel, image->getPixelAspectRatio(), eImageBitDepthFloat, image->getFieldingOrder(), false);
            target = full.get();
        }

        {
            Image::WriteAccess outAccess(image.get());
            if (level > 0 || fullRect != roi) {
                zeroRows(outAccess, roi, nComps);
            }
        }

        if (fullRect.isNull()) {
            continue;
        }

        ReadPlane plane;
        plane.target = target;
        plane.rect = fullRect;
        plane.path = path;
        plane.subimage = subimage;
        plane.fileChannels = fileChannels;
        if (it->first.isColorLayer() && (nComps == 3 || nComps == 4)) {
            plane.toWorking = toWorking;
        }

        const int width = fullRect.width();
        const int height = fullRect.height();
        const int span = std::max(1, nComps);
        const int memoryRows = std::max(1, (int)(kStagingBytes / ((std::size_t)width * span * sizeof(float))));
        int rowsPerBand = std::max(kMinBandRows, ((height + nThreads - 1) / std::max(1, nThreads) + kMinBandRows - 1) / kMinBandRows * kMinBandRows);
        rowsPerBand = std::min(rowsPerBand, memoryRows);
        const int nBands = (height + rowsPerBand - 1) / rowsPerBand;

        {
            Image::WriteAccess access(target);
            const std::function<void(int)> renderBand = [&](int band) {
                if (cancel.check() || failed.load()) {
                    return;
                }
                const int y1 = fullRect.y1 + band * rowsPerBand;
                const int y2 = std::min(y1 + rowsPerBand, fullRect.y2);
                std::string bandError;
                if (!decodeBand(plane, access, nComps, y1, y2, &bandError)) {
                    std::lock_guard<std::mutex> lock(errorMutex);
                    if (!failed.exchange(true)) {
                        error = bandError;
                    }
                }
            };
            parallelForCancellable(nBands, nThreads, cancel, renderBand);
        }

        if (failed.load()) {
            setPersistentMessage(eMessageTypeError, error);

            return eStatusFailed;
        }

        if (full && !cancel.check()) {
            full->downscaleMipmap(image->getRoD(), fullRect, fileLevel, level, false, image.get());
        }
    }

    clearPersistentMessage(false);

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
