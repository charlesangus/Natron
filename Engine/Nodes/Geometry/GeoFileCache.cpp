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

#include "GeoFileCache.h"

#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ContentHasher.h"
#include "Engine/FileStampRegistry.h"
#include "Engine/Geo/GeoFileFormat.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Geometry/GeoFile.h"
#include "Engine/OutputSchedulerThread.h"

NATRON_NAMESPACE_ENTER

namespace {
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

bool
upstreamNeedsOrderedFrames(const NodePtr& node,
                           std::set<const Node*>* visited)
{
    if (!node || !visited->insert(node.get()).second) {
        return false;
    }
    const NativeEffectBase* native = dynamic_cast<const NativeEffectBase*>(node->getEffectInstance().get());
    if (native && native->needsOrderedFrames()) {
        return true;
    }
    // A cache loading from disk does not evaluate what is above it.
    const GeoFileCache* cache = dynamic_cast<const GeoFileCache*>(native);
    if (cache && (cache->mode() == GeoFileCache::eModeLoad)) {
        return false;
    }
    for (int i = 0; i < node->getNInputs(); ++i) {
        if (upstreamNeedsOrderedFrames(node->getInput(i), visited)) {
            return true;
        }
    }

    return false;
}
} // anonymous namespace

std::string
GeoFileCache::substituteVersion(const std::string& value,
                                int version)
{
    std::string out;
    out.reserve(value.size());
    std::size_t i = 0;
    while (i < value.size()) {
        if (value.compare(i, 4, "$VER") == 0) {
            std::size_t j = i + 4;
            while (j < value.size() && value[j] >= '0' && value[j] <= '9') {
                ++j;
            }
            const int padding = (j > i + 4) ? std::atoi(value.substr(i + 4, j - i - 4).c_str()) : 0;
            std::string digits = std::to_string(version < 0 ? -version : version);
            if ((int)digits.size() < padding) {
                digits.insert(0, padding - digits.size(), '0');
            }
            out += (version < 0 ? "-" : "") + digits;
            i = j;
            continue;
        }
        out += value[i];
        ++i;
    }

    return out;
}

GeoFileCache::GeoFileCache(NodePtr node)
    : NativeEffectBase(node)
{
}

GeoFileCache::~GeoFileCache()
{
}

const NodeSpec*
GeoFileCache::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
GeoFileCache::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    KnobChoicePtr mode = createKnob<KnobChoice>(tr("Mode").toStdString());
    mode->setName(kGeoFileCacheParamMode);
    mode->setHintToolTip(tr("What the node outputs, and whether rendering it writes the cache.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("save", tr("Save To Disk").toStdString(),
                                       tr("Output the input; rendering the node writes each frame of the frame range.").toStdString()));
        options.push_back(ChoiceOption("load", tr("Load From Disk").toStdString(),
                                       tr("Output the cached files, without evaluating the input.").toStdString()));
        options.push_back(ChoiceOption("passThrough", tr("Pass Through").toStdString(),
                                       tr("Output the input; rendering the node fails.").toStdString()));
        mode->populateChoices(options);
    }
    mode->setDefaultValue((int)eModeSave);
    mode->setAnimationEnabled(false);
    page->addKnob(mode);
    _mode = mode;

    KnobOutputFilePtr file = createKnob<KnobOutputFile>(tr("Filename").toStdString());
    file->setName(kGeoFileCacheParamFile);
    file->setHintToolTip(tr("The cache files, one per frame. Write the frame number as #### (one # per digit), %04d, "
                            "$F (unpadded) or $F4 (padded to 4 digits), and the Version as $VER, or $VER3 padded to 3 "
                            "digits. Missing directories are created when the cache is written.")
                             .toStdString());
    file->setAnimationEnabled(false);
    page->addKnob(file);
    _file = file;

    KnobIntPtr version = createKnob<KnobInt>(tr("Version").toStdString());
    version->setName(kGeoFileCacheParamVersion);
    version->setHintToolTip(tr("The number $VER in the Filename stands for, so that a new version of the cache "
                               "can be written beside the previous ones.")
                                .toStdString());
    version->setDefaultValue(1);
    version->setMinimum(0);
    version->setAnimationEnabled(false);
    version->disableSlider();
    page->addKnob(version);
    _version = version;

    KnobChoicePtr missingFrames = createKnob<KnobChoice>(tr("Missing Frames").toStdString());
    missingFrames->setName(kGeoFileCacheParamMissingFrames);
    missingFrames->setHintToolTip(tr("In Load From Disk mode, what a frame whose file does not exist gives.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("error", tr("Error").toStdString(),
                                       tr("The frame fails to render.").toStdString()));
        options.push_back(ChoiceOption("holdNearest", tr("Hold Nearest").toStdString(),
                                       tr("The closest frame of the cache on disk, the earlier one on a tie.").toStdString()));
        options.push_back(ChoiceOption("empty", tr("Empty").toStdString(),
                                       tr("Empty geometry.").toStdString()));
        missingFrames->populateChoices(options);
    }
    missingFrames->setDefaultValue((int)eGeoFileMissingFrameError);
    missingFrames->setAnimationEnabled(false);
    page->addKnob(missingFrames);
    _missingFrames = missingFrames;

    KnobChoicePtr frameRange = createKnob<KnobChoice>(tr("Frame Range").toStdString());
    frameRange->setName(kGeoFileCacheParamFrameRange);
    frameRange->setHintToolTip(tr("The frames rendering the node writes.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("project", tr("Project").toStdString(),
                                       tr("The project's frame range.").toStdString()));
        options.push_back(ChoiceOption("manual", tr("Manual").toStdString(),
                                       tr("First Frame to Last Frame.").toStdString()));
        frameRange->populateChoices(options);
    }
    frameRange->setDefaultValue((int)eFrameRangeProject);
    frameRange->setAnimationEnabled(false);
    page->addKnob(frameRange);
    _frameRange = frameRange;

    KnobIntPtr firstFrame = createKnob<KnobInt>(tr("First Frame").toStdString());
    firstFrame->setName(kGeoFileCacheParamFirstFrame);
    firstFrame->setHintToolTip(tr("The first frame to write, when the Frame Range is Manual.").toStdString());
    firstFrame->setDefaultValue(0);
    firstFrame->setAnimationEnabled(false);
    firstFrame->disableSlider();
    firstFrame->setAddNewLine(false);
    page->addKnob(firstFrame);
    _firstFrame = firstFrame;

    KnobIntPtr lastFrame = createKnob<KnobInt>(tr("Last Frame").toStdString());
    lastFrame->setName(kGeoFileCacheParamLastFrame);
    lastFrame->setHintToolTip(tr("The last frame to write, when the Frame Range is Manual.").toStdString());
    lastFrame->setDefaultValue(0);
    lastFrame->setAnimationEnabled(false);
    lastFrame->disableSlider();
    page->addKnob(lastFrame);
    _lastFrame = lastFrame;

    refreshKnobVisibility();

    // The node scopes the connection, so the slot runs on the main thread and never after the node
    // is gone; a render may keep this effect alive past its node, hence the check that the node
    // still holds it.
    NodePtr node = getNode();
    RenderEnginePtr engine = getRenderEngine();
    if (engine && node) {
        NodeWPtr weakNode = node;
        QObject::connect(engine.get(), &RenderEngine::renderFinished, node.get(), [this, weakNode](int retCode) {
            NodePtr aliveNode = weakNode.lock();
            EffectInstancePtr alive = aliveNode ? aliveNode->getEffectInstance() : EffectInstancePtr();
            if (alive.get() == this) {
                onRenderFinished(retCode);
            }
        });
    }
} // GeoFileCache::initializeKnobs

void
GeoFileCache::onKnobsLoaded()
{
    refreshKnobVisibility();
}

GeoFileCache::ModeEnum
GeoFileCache::mode() const
{
    const KnobChoicePtr knob = _mode.lock();
    const int value = knob ? knob->getValue() : (int)eModeSave;

    return (value == (int)eModeLoad || value == (int)eModePassThrough) ? (ModeEnum)value : eModeSave;
}

void
GeoFileCache::refreshKnobVisibility()
{
    const KnobChoicePtr frameRange = _frameRange.lock();
    const bool manual = frameRange && (frameRange->getValue() == (int)eFrameRangeManual);

    setSecretAndDisabled(_firstFrame.lock(), !manual);
    setSecretAndDisabled(_lastFrame.lock(), !manual);
    setSecretAndDisabled(_missingFrames.lock(), mode() != eModeLoad);
}

bool
GeoFileCache::knobChanged(KnobI* k,
                          ValueChangedReasonEnum reason,
                          ViewSpec /*view*/,
                          double /*time*/,
                          bool /*originatedFromMainThread*/)
{
    if (reason == eValueChangedReasonTimeChanged) {
        return false;
    }
    const KnobChoicePtr frameRange = _frameRange.lock();
    const KnobChoicePtr modeKnob = _mode.lock();
    if (frameRange && (k == frameRange.get())) {
        const KnobIntPtr firstFrame = _firstFrame.lock();
        const KnobIntPtr lastFrame = _lastFrame.lock();
        // Switching to Manual starts from the project's range rather than from 0 to 0.
        if ((frameRange->getValue() == (int)eFrameRangeManual) && firstFrame && lastFrame && (firstFrame->getValue() == 0) && (lastFrame->getValue() == 0)) {
            double first = 0.;
            double last = 0.;
            getApp()->getFrameRange(&first, &last);
            firstFrame->setValue((int)std::floor(first));
            lastFrame->setValue((int)std::floor(last));
        }
        refreshKnobVisibility();

        return true;
    }
    if (modeKnob && (k == modeKnob.get())) {
        refreshKnobVisibility();

        return true;
    }

    return false;
}

void
GeoFileCache::getFrameRange(double* first,
                            double* last)
{
    const KnobChoicePtr frameRange = _frameRange.lock();
    const KnobIntPtr firstFrame = _firstFrame.lock();
    const KnobIntPtr lastFrame = _lastFrame.lock();

    if (frameRange && (frameRange->getValue() == (int)eFrameRangeManual) && firstFrame && lastFrame) {
        *first = firstFrame->getValue();
        *last = lastFrame->getValue();
    } else {
        getApp()->getFrameRange(first, last);
    }
}

bool
GeoFileCache::dependsOnProjectFrameRange() const
{
    const KnobChoicePtr frameRange = _frameRange.lock();

    return !frameRange || (frameRange->getValue() != (int)eFrameRangeManual);
}

std::string
GeoFileCache::currentPattern() const
{
    const KnobOutputFilePtr file = _file.lock();
    const KnobIntPtr version = _version.lock();
    if (!file) {
        return std::string();
    }

    // getValue() is not const; it only reads the stored value.
    return GeoFile::expandPattern(const_cast<GeoFileCache*>(this), substituteVersion(file->getValue(), version ? version->getValue() : 1));
}

void
GeoFileCache::appendNodeContentHash(ContentHasher& h) const
{
    // Only loading reads the files; saving outputs the input.
    h.appendU64(mode() == eModeLoad ? GeoFile::patternStamp(currentPattern()) : 0);
}

bool
GeoFileCache::refreshFileDependencies(const std::vector<std::string>& paths)
{
    if ((mode() != eModeLoad) || !GeoFile::invalidatePatternIfCovered(currentPattern(), paths)) {
        return false;
    }
    purgeCaches();
    clearActionsCache();
    clearPersistentMessage(false);

    return true;
}

void
GeoFileCache::onRenderFinished(int retCode)
{
    std::vector<std::string> written;
    {
        std::lock_guard<std::mutex> lock(_writtenPathsMutex);
        written.swap(_writtenPaths);
    }
    if (retCode == 0) {
        clearPersistentMessage(false);
    }
    if (written.empty()) {
        return;
    }
    FileStampRegistry& registry = FileStampRegistry::instance();
    for (const std::string& path : written) {
        registry.invalidate(path);
    }
    purgeCaches();
    clearActionsCache();
    NodePtr node = getNode();
    if (node) {
        appPTR->removeAllCacheEntriesForHolder(node.get(), false);
        node->markHashDirty();
    }
    KnobFile::reloadNodesReadingFiles(written, this);
}

FramesNeededMap
GeoFileCache::getFramesNeeded(double time,
                              ViewIdx view)
{
    // Loading must not evaluate the chain the cache stands in for.
    if (mode() == eModeLoad) {
        return FramesNeededMap();
    }

    return NativeEffectBase::getFramesNeeded(time, view);
}

StatusEnum
GeoFileCache::getPreferredMetadataAt(double /*time*/,
                                     ViewIdx /*view*/,
                                     NodeMetadata& metadata)
{
    // In Load mode the geometry cache keys on time only for a frame-varying node; the input's
    // variation is what keys it in the other modes.
    if (mode() == eModeLoad && GeoFile::isFrameSequencePattern(currentPattern())) {
        metadata.setIsFrameVarying(true);
    }

    return eStatusOK;
}

StatusEnum
GeoFileCache::renderGeometry(const GeoRenderActionArgs& args,
                             GeoDetailConstPtr* outputDetail,
                             std::string* errorMessage)
{
    if (mode() == eModeLoad) {
        const KnobChoicePtr missingKnob = _missingFrames.lock();
        const GeoFileMissingFrameEnum missing = missingKnob ? (GeoFileMissingFrameEnum)missingKnob->getValue() : eGeoFileMissingFrameError;
        std::string error;
        if (!GeoFile::readFrame(this, currentPattern(), missing, (int)std::floor(args.time + 0.5), args.view, outputDetail, &error)) {
            if (errorMessage) {
                *errorMessage = error;
            }

            return eStatusFailed;
        }

        return eStatusOK;
    }

    GeoDetailConstPtr input = args.getInputDetail(0);
    *outputDetail = input ? input : std::make_shared<const GeoDetail>(GeoDetailBuilder().finish());

    return eStatusOK;
}

StatusEnum
GeoFileCache::writeGeometryFrame(double time,
                                 ViewIdx view,
                                 const GeoDetailConstPtr& detail,
                                 std::string* error)
{
    // Also a persistent message, so the node shows why its render failed.
    const auto fail = [this, error](const std::string& message) {
        if (error) {
            *error = message;
        }
        setPersistentMessage(eMessageTypeError, message);

        return eStatusFailed;
    };

    if (mode() != eModeSave) {
        return fail(tr("Set the Mode to Save To Disk to write the cache.").toStdString());
    }
    const std::string pattern = currentPattern();
    if (pattern.empty()) {
        return fail(tr("No file to write to.").toStdString());
    }
    int firstFrame = 0;
    int lastFrame = 0;
    if (getRunningRenderRequestedRange(&firstFrame, &lastFrame) && (lastFrame > firstFrame) && !GeoFile::isFrameSequencePattern(pattern)) {
        return fail(tr("The Filename has no frame number, so every frame from %1 to %2 would overwrite the same file. "
                       "Write the frame number in it as #### or $F4.")
                        .arg(firstFrame)
                        .arg(lastFrame)
                        .toStdString());
    }
    const int frame = (int)std::floor(time + 0.5);
    const std::string path = GeoFile::resolvePath(this, pattern, frame, view);
    if (!detail) {
        return fail(tr("Nothing to write at frame %1.").arg(frame).toStdString());
    }

    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return fail(tr("Cannot create the directory '%1': %2")
                            .arg(QString::fromStdString(parent.string()))
                            .arg(QString::fromStdString(ec.message()))
                            .toStdString());
        }
    }
    try {
        geoFileWrite(*detail, path);
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    {
        std::lock_guard<std::mutex> lock(_writtenPathsMutex);
        _writtenPaths.push_back(path);
    }

    return eStatusOK;
}

bool
GeoFileCache::rendersFramesInOrder() const
{
    std::set<const Node*> visited;
    const NodePtr node = getNode();
    if (!node) {
        return false;
    }
    for (int i = 0; i < node->getNInputs(); ++i) {
        if (upstreamNeedsOrderedFrames(node->getInput(i), &visited)) {
            return true;
        }
    }

    return false;
}

NATRON_NAMESPACE_EXIT
