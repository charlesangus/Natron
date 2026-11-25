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

#include "GeoFile.h"

#include <cctype>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include <SequenceParsing.h>

#include "Engine/AppInstance.h"
#include "Engine/ContentHasher.h"
#include "Engine/FileStampRegistry.h"
#include "Engine/Geo/GeoFileFormat.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_ENTER

namespace {
bool
isDigit(char c)
{
    return c >= '0' && c <= '9';
}

const SequenceFile*
recordedFile(const std::vector<SequenceFile>& files,
             const std::string& path)
{
    for (const SequenceFile& file : files) {
        if (file.path == path) {
            return &file;
        }
    }

    return nullptr;
}

const SequenceFile*
nearestRecordedFile(const std::vector<SequenceFile>& files,
                    int frame,
                    ViewIdx view)
{
    const SequenceFile* best = nullptr;
    long long bestDistance = LLONG_MAX;
    for (std::size_t i = 0; i < files.size();) {
        std::size_t end = i;
        while ((end < files.size()) && (files[end].frame == files[i].frame)) {
            ++end;
        }
        // Without a view token in the name every file sits under one view index.
        const SequenceFile* candidate = &files[i];
        for (std::size_t j = i; j < end; ++j) {
            if (files[j].view == view.value()) {
                candidate = &files[j];
                break;
            }
        }
        const long long distance = std::llabs((long long)files[i].frame - (long long)frame);
        i = end;
        if ((distance >= bestDistance) || !candidate->identity.exists) {
            continue;
        }
        best = candidate;
        bestDistance = distance;
    }

    return best;
}

bool
readPinnedFile(const std::string& path,
               const FileIdentity& pinned,
               GeoDetailConstPtr* detail,
               std::string* error)
{
    if (FileStampRegistry::statNow(path) != pinned) {
        if (error) {
            *error = FileStampRegistry::changedOnDiskMessage(path);
        }

        return false;
    }
    GeoDetailConstPtr read;
    try {
        read = std::make_shared<const GeoDetail>(geoFileRead(path));
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }

        return false;
    }
    // A file rewritten in place while it was mapped may have decoded as either version.
    if (FileStampRegistry::statNow(path) != pinned) {
        if (error) {
            *error = FileStampRegistry::changedOnDiskMessage(path);
        }

        return false;
    }
    *detail = read;

    return true;
}

std::vector<std::string>
projectViewNames(EffectInstance* effect)
{
    std::vector<std::string> views;
    if (effect && effect->getApp() && effect->getApp()->getProject()) {
        views = effect->getApp()->getProject()->getProjectViewNames();
    }

    return views;
}

} // anonymous namespace

bool
GeoFile::isFrameSequencePattern(const std::string& pattern)
{
    if (pattern.find('#') != std::string::npos) {
        return true;
    }
    for (std::size_t i = pattern.find('%'); i != std::string::npos; i = pattern.find('%', i + 1)) {
        std::size_t j = i + 1;
        while (j < pattern.size() && isDigit(pattern[j])) {
            ++j;
        }
        if (j < pattern.size() && pattern[j] == 'd') {
            return true;
        }
    }
    for (std::size_t i = pattern.find("$F"); i != std::string::npos; i = pattern.find("$F", i + 1)) {
        const std::size_t j = i + 2;
        if (j == pattern.size() || !(std::isalpha((unsigned char)pattern[j]) || pattern[j] == '_')) {
            return true;
        }
    }

    return false;
}

std::string
GeoFile::normalizeFrameTokens(const std::string& pattern)
{
    std::string out;
    out.reserve(pattern.size());
    std::size_t i = 0;
    while (i < pattern.size()) {
        if (pattern.compare(i, 2, "$F") == 0) {
            std::size_t j = i + 2;
            while (j < pattern.size() && isDigit(pattern[j])) {
                ++j;
            }
            // $FOO is not a frame token; leave it as written.
            if (j == i + 2 && j < pattern.size() && (std::isalpha((unsigned char)pattern[j]) || pattern[j] == '_')) {
                out += pattern[i];
                ++i;
                continue;
            }
            const std::string padding = pattern.substr(i + 2, j - i - 2);
            out += padding.empty() ? std::string("%d") : "%0" + padding + "d";
            i = j;
            continue;
        }
        out += pattern[i];
        ++i;
    }

    return out;
}

std::string
GeoFile::expandPattern(EffectInstance* effect,
                       const std::string& value)
{
    std::string path = value;
    if (!path.empty() && effect && effect->getApp() && effect->getApp()->getProject()) {
        effect->getApp()->getProject()->canonicalizePath(path);
    }

    return normalizeFrameTokens(path);
}

std::string
GeoFile::resolvePath(EffectInstance* effect,
                     const std::string& pattern,
                     int frame,
                     ViewIdx view)
{
    if (!isFrameSequencePattern(pattern)) {
        return pattern;
    }

    return SequenceParsing::generateFileNameFromPattern(pattern, projectViewNames(effect), frame, view);
}

std::string
GeoFile::nearestExistingFrame(const std::string& pattern,
                              int frame,
                              ViewIdx view)
{
    if (!isFrameSequencePattern(pattern)) {
        return std::string();
    }
    const std::shared_ptr<const std::vector<SequenceFile>> files = FileStampRegistry::instance().sequenceFiles(pattern);
    const SequenceFile* nearest = files ? nearestRecordedFile(*files, frame, view) : nullptr;

    return nearest ? nearest->path : std::string();
}

U64
GeoFile::patternStamp(const std::string& pattern)
{
    if (pattern.empty()) {
        return 0;
    }
    FileStampRegistry& registry = FileStampRegistry::instance();

    return isFrameSequencePattern(pattern) ? registry.sequenceStamp(pattern) : registry.stamp(pattern);
}

bool
GeoFile::invalidatePatternIfCovered(const std::string& pattern,
                                    const std::vector<std::string>& paths)
{
    if (pattern.empty()) {
        return false;
    }
    for (const std::string& path : paths) {
        if (FileStampRegistry::coversSameFiles(path, pattern)) {
            FileStampRegistry::instance().invalidate(pattern);

            return true;
        }
    }

    return false;
}

bool
GeoFile::readFrame(EffectInstance* effect,
                   const std::string& pattern,
                   GeoFileMissingFrameEnum missing,
                   int frame,
                   ViewIdx view,
                   GeoDetailConstPtr* detail,
                   std::string* error)
{
    std::string path = resolvePath(effect, pattern, frame, view);
    if (path.empty()) {
        *detail = std::make_shared<const GeoDetail>(GeoDetailBuilder().finish());

        return true;
    }

    FileStampRegistry& registry = FileStampRegistry::instance();
    std::shared_ptr<const std::vector<SequenceFile>> files;
    FileIdentity pinned;
    if (isFrameSequencePattern(pattern)) {
        files = registry.sequenceFiles(pattern);
        const SequenceFile* recorded = files ? recordedFile(*files, path) : nullptr;
        if (recorded) {
            pinned = recorded->identity;
        }
    } else {
        pinned = registry.identity(path);
    }

    if (!pinned.exists) {
        if (FileStampRegistry::statNow(path).exists) {
            if (error) {
                *error = FileStampRegistry::changedOnDiskMessage(path);
            }

            return false;
        }
        switch (missing) {
        case eGeoFileMissingFrameEmpty:
            *detail = std::make_shared<const GeoDetail>(GeoDetailBuilder().finish());

            return true;
        case eGeoFileMissingFrameHoldNearest: {
            const SequenceFile* nearest = files ? nearestRecordedFile(*files, frame, view) : nullptr;
            if (nearest) {
                path = nearest->path;
                pinned = nearest->identity;
                break;
            }
            if (error) {
                *error = "Geometry file '" + path + "' does not exist, and no other frame of the sequence does either";
            }

            return false;
        }
        case eGeoFileMissingFrameError:
        default:
            if (error) {
                *error = "Geometry file '" + path + "' does not exist";
            }

            return false;
        }
    }

    return readPinnedFile(path, pinned, detail, error);
}

GeoFile::GeoFile(NodePtr node)
    : NativeEffectBase(node)
{
}

GeoFile::~GeoFile()
{
}

const NodeSpec*
GeoFile::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
GeoFile::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    KnobFilePtr file = createKnob<KnobFile>(tr("File").toStdString());
    file->setName(kGeoFileParamFile);
    file->setHintToolTip(tr("The geometry file to read. For a sequence, write the frame number as #### (one # per "
                            "digit), %04d, $F (unpadded) or $F4 (padded to 4 digits). A file that exists but cannot "
                            "be read is an error; an empty name gives empty geometry.")
                             .toStdString());
    file->setFileExtensions({ kGeoFileExtension });
    file->setAnimationEnabled(false);
    page->addKnob(file);
    _file = file;

    KnobButtonPtr reload = file->createReloadButton(kGeoFileParamReload,
                                                    tr("Reads the files again, so geometry changed on disk is what the node outputs."));
    page->addKnob(reload);
    _reload = reload;

    KnobChoicePtr missingFrames = createKnob<KnobChoice>(tr("Missing Frames").toStdString());
    missingFrames->setName(kGeoFileParamMissingFrames);
    missingFrames->setHintToolTip(tr("What a frame whose file does not exist gives.").toStdString());
    std::vector<ChoiceOption> options;
    options.push_back(ChoiceOption("error", tr("Error").toStdString(),
                                   tr("The frame fails to render.").toStdString()));
    options.push_back(ChoiceOption("holdNearest", tr("Hold Nearest").toStdString(),
                                   tr("The closest frame of the sequence on disk, the earlier one on a tie.").toStdString()));
    options.push_back(ChoiceOption("empty", tr("Empty").toStdString(),
                                   tr("Empty geometry.").toStdString()));
    missingFrames->populateChoices(options);
    missingFrames->setDefaultValue((int)eGeoFileMissingFrameError);
    missingFrames->setAnimationEnabled(false);
    page->addKnob(missingFrames);
    _missingFrames = missingFrames;
}

std::string
GeoFile::currentPattern() const
{
    const KnobFilePtr file = _file.lock();

    // getValue() is not const; it only reads the stored value.
    return file ? expandPattern(const_cast<GeoFile*>(this), file->getValue()) : std::string();
}

bool
GeoFile::knobChanged(KnobI* k,
                     ValueChangedReasonEnum /*reason*/,
                     ViewSpec /*view*/,
                     double /*time*/,
                     bool /*originatedFromMainThread*/)
{
    const KnobButtonPtr reload = _reload.lock();
    if (!reload || (k != reload.get())) {
        return false;
    }
    // The file knob stamps its value as written, which differs from the pattern for a $F name.
    const std::string pattern = currentPattern();
    if (!pattern.empty()) {
        FileStampRegistry::instance().invalidate(pattern);
    }
    const KnobFilePtr file = _file.lock();
    if (file) {
        file->reloadFile();
        if (!pattern.empty() && (pattern != file->getResolvedPath())) {
            KnobFile::reloadNodesReadingFiles(std::vector<std::string>(1, pattern), this);
        }
    }
    NodePtr node = getNode();
    if (node) {
        node->markHashDirty();
    }

    return true;
}

void
GeoFile::appendNodeContentHash(ContentHasher& h) const
{
    h.appendU64(patternStamp(currentPattern()));
}

bool
GeoFile::refreshFileDependencies(const std::vector<std::string>& paths)
{
    if (!invalidatePatternIfCovered(currentPattern(), paths)) {
        return false;
    }
    purgeCaches();
    clearActionsCache();
    clearPersistentMessage(false);

    return true;
}

StatusEnum
GeoFile::getPreferredMetadataAt(double /*time*/,
                                ViewIdx /*view*/,
                                NodeMetadata& metadata)
{
    // The geometry cache keys on time only for frame-varying nodes; without this every frame of a
    // sequence would be served the first frame's geometry.
    metadata.setIsFrameVarying(isFrameSequencePattern(currentPattern()));

    return eStatusOK;
}

StatusEnum
GeoFile::renderGeometry(const GeoRenderActionArgs& args,
                        GeoDetailConstPtr* outputDetail,
                        std::string* errorMessage)
{
    const KnobChoicePtr missingKnob = _missingFrames.lock();
    const GeoFileMissingFrameEnum missing = missingKnob ? (GeoFileMissingFrameEnum)missingKnob->getValue() : eGeoFileMissingFrameError;
    const int frame = (int)std::floor(args.time + 0.5);
    std::string error;
    if (!readFrame(this, currentPattern(), missing, frame, args.view, outputDetail, &error)) {
        if (errorMessage) {
            *errorMessage = error;
        }

        return eStatusFailed;
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
