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

#include <cmath>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxMessage.h"
#include "ofxMetadata.h"
#include "ofxParam.h"
#include "ofxProperty.h"

#include "NatronTestMetadataPluginIDs.h"

namespace {

OfxHost* gHost = nullptr;
const OfxImageEffectSuiteV1* gEffectSuite = nullptr;
const OfxPropertySuiteV2* gPropSuite = nullptr;
const OfxParameterSuiteV1* gParamSuite = nullptr;
const OfxMetadataSuiteV1* gMetadataSuite = nullptr;
const OfxMessageSuiteV1* gMessageSuite = nullptr;

enum class Kind {
    Contribute,
    Print,
    TimeCode
};

OfxStatus
fetchSuites()
{
    if (!gHost) {
        return kOfxStatErrMissingHostFeature;
    }
    gEffectSuite = static_cast<const OfxImageEffectSuiteV1*>(gHost->fetchSuite(gHost->host, kOfxImageEffectSuite, 1));
    gPropSuite = static_cast<const OfxPropertySuiteV2*>(gHost->fetchSuite(gHost->host, kOfxPropertySuite, 2));
    gParamSuite = static_cast<const OfxParameterSuiteV1*>(gHost->fetchSuite(gHost->host, kOfxParameterSuite, 1));
    gMetadataSuite = static_cast<const OfxMetadataSuiteV1*>(gHost->fetchSuite(gHost->host, kOfxMetadataSuite, 1));
    gMessageSuite = static_cast<const OfxMessageSuiteV1*>(gHost->fetchSuite(gHost->host, kOfxMessageSuite, 1));
    if (!gEffectSuite || !gPropSuite || !gParamSuite || !gMessageSuite) {
        return kOfxStatErrMissingHostFeature;
    }

    return kOfxStatOK;
}

// ---------------------------------------------------------------------------------------------
// Description

const char* const kPixelDepths[] = { kOfxBitDepthByte, kOfxBitDepthShort, kOfxBitDepthFloat };

void
describeEffect(OfxImageEffectHandle descriptor,
               const char* label)
{
    OfxPropertySetHandle props = nullptr;
    gEffectSuite->getPropertySet(descriptor, &props);
    gPropSuite->propSetString(props, kOfxPropLabel, 0, label);
    gPropSuite->propSetString(props, kOfxImageEffectPluginPropGrouping, 0, "Other");
    gPropSuite->propSetString(props, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);
    for (int i = 0; i < (int)(sizeof(kPixelDepths) / sizeof(kPixelDepths[0])); ++i) {
        gPropSuite->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, i, kPixelDepths[i]);
    }
    gPropSuite->propSetInt(props, kOfxImageEffectPluginPropSingleInstance, 0, 0);
    gPropSuite->propSetInt(props, kOfxImageEffectPluginPropHostFrameThreading, 0, 0);
    gPropSuite->propSetInt(props, kOfxImageEffectPropSupportsMultiResolution, 0, 1);
    gPropSuite->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 1);
    gPropSuite->propSetInt(props, kOfxImageEffectPropTemporalClipAccess, 0, 0);
    gPropSuite->propSetInt(props, kOfxImageEffectPluginPropFieldRenderTwiceAlways, 0, 0);
    gPropSuite->propSetInt(props, kOfxImageEffectPropSupportsMultipleClipPARs, 0, 0);
}

void
defineClips(OfxImageEffectHandle descriptor)
{
    OfxPropertySetHandle props = nullptr;
    gEffectSuite->clipDefine(descriptor, kOfxImageEffectSimpleSourceClipName, &props);
    gPropSuite->propSetString(props, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    gPropSuite->propSetString(props, kOfxImageEffectPropSupportedComponents, 1, kOfxImageComponentAlpha);
    gPropSuite->propSetInt(props, kOfxImageEffectPropTemporalClipAccess, 0, 0);
    gPropSuite->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 1);
    gPropSuite->propSetInt(props, kOfxImageClipPropIsMask, 0, 0);

    gEffectSuite->clipDefine(descriptor, kOfxImageEffectOutputClipName, &props);
    gPropSuite->propSetString(props, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    gPropSuite->propSetString(props, kOfxImageEffectPropSupportedComponents, 1, kOfxImageComponentAlpha);
    gPropSuite->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 1);
}

// Defines a non-animated param and adds it to the page.
OfxPropertySetHandle
defineParam(OfxParamSetHandle params,
            OfxPropertySetHandle page,
            const char* type,
            const char* name,
            const char* label,
            const char* hint)
{
    OfxPropertySetHandle props = nullptr;
    gParamSuite->paramDefine(params, type, name, &props);
    gPropSuite->propSetString(props, kOfxPropLabel, 0, label);
    gPropSuite->propSetString(props, kOfxParamPropHint, 0, hint);
    gPropSuite->propSetInt(props, kOfxParamPropAnimates, 0, 0);
    int children = 0;
    gPropSuite->propGetDimension(page, kOfxParamPropPageChild, &children);
    gPropSuite->propSetString(page, kOfxParamPropPageChild, children, name);

    return props;
}

OfxPropertySetHandle
defineSingleLineString(OfxParamSetHandle params,
                       OfxPropertySetHandle page,
                       const char* name,
                       const char* label,
                       const char* hint,
                       const char* defaultValue)
{
    OfxPropertySetHandle props = defineParam(params, page, kOfxParamTypeString, name, label, hint);
    gPropSuite->propSetString(props, kOfxParamPropStringMode, 0, kOfxParamStringIsSingleLine);
    gPropSuite->propSetString(props, kOfxParamPropDefault, 0, defaultValue);

    return props;
}

OfxPropertySetHandle
definePage(OfxParamSetHandle params)
{
    OfxPropertySetHandle page = nullptr;
    gParamSuite->paramDefine(params, kOfxParamTypePage, "Controls", &page);

    return page;
}

void
describeContributeParams(OfxParamSetHandle params)
{
    OfxPropertySetHandle page = definePage(params);

    defineSingleLineString(params, page, kNatronTestMetadataContributeParamNote, "note",
                           "text contributed as " kNatronTestMetadataContributeKeyPrefix "note at every call", "");

    OfxPropertySetHandle mode = defineParam(params, page, kOfxParamTypeChoice, kNatronTestMetadataContributeParamMode, "mode",
                                            "what this effect does to the metadata it inherits from Source, on top of what it always contributes");
    gPropSuite->propSetString(mode, kOfxParamPropChoiceOption, kNatronTestMetadataContributeModeInheritAll, "inherit all");
    gPropSuite->propSetString(mode, kOfxParamPropChoiceOption, kNatronTestMetadataContributeModeDropOneKey, "drop one key");
    gPropSuite->propSetString(mode, kOfxParamPropChoiceOption, kNatronTestMetadataContributeModeInheritNothing, "inherit nothing");
    gPropSuite->propSetInt(mode, kOfxParamPropDefault, 0, kNatronTestMetadataContributeModeInheritAll);

    defineSingleLineString(params, page, kNatronTestMetadataContributeParamDropKey, "drop key",
                           "the retained key to drop from Source's inherited metadata when mode is 'drop one key'",
                           kOfxMetadataKeySampleType);
}

void
describeTimeCodeParams(OfxParamSetHandle params)
{
    OfxPropertySetHandle page = definePage(params);

    defineSingleLineString(params, page, kNatronTestMetadataTimeCodeParamStartTimecode, "start timecode",
                           "the non drop frame HH:MM:SS:FF the count starts from at the origin frame", "01:00:00:00");

    OfxPropertySetHandle rate = defineParam(params, page, kOfxParamTypeDouble, kNatronTestMetadataTimeCodeParamRate, "rate",
                                            "the frames per second the count runs at when the rate is not taken from Source");
    gPropSuite->propSetDouble(rate, kOfxParamPropDefault, 0, 24.);
    gPropSuite->propSetDouble(rate, kOfxParamPropMin, 0, 1.);
    gPropSuite->propSetDouble(rate, kOfxParamPropMax, 0, 1000.);
    gPropSuite->propSetDouble(rate, kOfxParamPropDisplayMin, 0, 1.);
    gPropSuite->propSetDouble(rate, kOfxParamPropDisplayMax, 0, 120.);

    OfxPropertySetHandle rateFromMetadata = defineParam(params, page, kOfxParamTypeBoolean, kNatronTestMetadataTimeCodeParamRateFromMetadata,
                                                        "rate from metadata",
                                                        "read the frame rate off Source's metadata, falling back to rate where Source carries none");
    gPropSuite->propSetInt(rateFromMetadata, kOfxParamPropDefault, 0, 1);

    OfxPropertySetHandle startFrame = defineParam(params, page, kOfxParamTypeInteger, kNatronTestMetadataTimeCodeParamStartFrame, "start frame",
                                                  "the frame the start timecode lands on when use start frame is on");
    gPropSuite->propSetInt(startFrame, kOfxParamPropDefault, 0, 1);

    OfxPropertySetHandle useStartFrame = defineParam(params, page, kOfxParamTypeBoolean, kNatronTestMetadataTimeCodeParamUseStartFrame,
                                                     "use start frame", "count from start frame rather than from frame 1");
    gPropSuite->propSetInt(useStartFrame, kOfxParamPropDefault, 0, 0);
}

OfxStatus
describeInContext(Kind kind,
                  OfxImageEffectHandle descriptor)
{
    defineClips(descriptor);

    OfxParamSetHandle params = nullptr;
    gEffectSuite->getParamSet(descriptor, &params);
    switch (kind) {
    case Kind::Contribute:
        describeContributeParams(params);
        break;
    case Kind::TimeCode:
        describeTimeCodeParams(params);
        break;
    case Kind::Print:
        break;
    }

    return kOfxStatOK;
}

// ---------------------------------------------------------------------------------------------
// Instance helpers

OfxImageClipHandle
clip(OfxImageEffectHandle effect,
     const char* name)
{
    OfxImageClipHandle handle = nullptr;
    gEffectSuite->clipGetHandle(effect, name, &handle, nullptr);

    return handle;
}

OfxParamHandle
param(OfxImageEffectHandle effect,
      const char* name)
{
    OfxParamSetHandle params = nullptr;
    gEffectSuite->getParamSet(effect, &params);
    OfxParamHandle handle = nullptr;
    gParamSuite->paramGetHandle(params, name, &handle, nullptr);

    return handle;
}

std::string
stringParam(OfxImageEffectHandle effect,
            const char* name)
{
    const char* value = nullptr;
    if ((gParamSuite->paramGetValue(param(effect, name), &value) != kOfxStatOK) || !value) {
        return std::string();
    }

    return std::string(value);
}

int
intParam(OfxImageEffectHandle effect,
         const char* name)
{
    int value = 0;
    gParamSuite->paramGetValue(param(effect, name), &value);

    return value;
}

double
doubleParam(OfxImageEffectHandle effect,
            const char* name)
{
    double value = 0.;
    gParamSuite->paramGetValue(param(effect, name), &value);

    return value;
}

std::vector<std::string>
stringValues(OfxPropertySetHandle props,
             const char* name)
{
    std::vector<std::string> values;
    int count = 0;
    if (gPropSuite->propGetDimension(props, name, &count) != kOfxStatOK) {
        return values;
    }
    for (int i = 0; i < count; ++i) {
        const char* value = nullptr;
        if ((gPropSuite->propGetString(props, name, i, &value) == kOfxStatOK) && value) {
            values.push_back(value);
        }
    }

    return values;
}

// A variable-dimension property only grows when written index by index, so it is reset to zero
// dimension first or a shorter list would keep the surplus entries.
void
setStringValues(OfxPropertySetHandle props,
                const char* name,
                const std::vector<std::string>& values)
{
    gPropSuite->propReset(props, name);
    for (int i = 0; i < (int)values.size(); ++i) {
        gPropSuite->propSetString(props, name, i, values[i].c_str());
    }
}

OfxPropertySetHandle
contributionSet(OfxPropertySetHandle inArgs)
{
    void* pointer = nullptr;
    gPropSuite->propGetPointer(inArgs, kOfxImageEffectPropMetadataSet, 0, &pointer);

    return static_cast<OfxPropertySetHandle>(pointer);
}

// ---------------------------------------------------------------------------------------------
// Render: a pixel copy of Source over the render window

int
componentCount(const char* components)
{
    if (std::strcmp(components, kOfxImageComponentRGBA) == 0) {
        return 4;
    }
    if (std::strcmp(components, kOfxImageComponentAlpha) == 0) {
        return 1;
    }

    return 0;
}

int
bytesPerComponent(const char* depth)
{
    if (std::strcmp(depth, kOfxBitDepthByte) == 0) {
        return 1;
    }
    if ((std::strcmp(depth, kOfxBitDepthShort) == 0) || (std::strcmp(depth, kOfxBitDepthHalf) == 0)) {
        return 2;
    }
    if (std::strcmp(depth, kOfxBitDepthFloat) == 0) {
        return 4;
    }

    return 0;
}

struct ImageAccess {
    OfxPropertySetHandle handle = nullptr;
    char* data = nullptr;
    OfxRectI bounds = { 0, 0, 0, 0 };
    int rowBytes = 0;
    int pixelBytes = 0;

    bool fetch(OfxImageClipHandle clip,
               OfxTime time)
    {
        if (gEffectSuite->clipGetImage(clip, time, nullptr, &handle) != kOfxStatOK) {
            handle = nullptr;

            return false;
        }
        void* pointer = nullptr;
        gPropSuite->propGetPointer(handle, kOfxImagePropData, 0, &pointer);
        data = static_cast<char*>(pointer);
        gPropSuite->propGetIntN(handle, kOfxImagePropBounds, 4, &bounds.x1);
        gPropSuite->propGetInt(handle, kOfxImagePropRowBytes, 0, &rowBytes);
        const char* components = nullptr;
        const char* depth = nullptr;
        gPropSuite->propGetString(handle, kOfxImageEffectPropComponents, 0, &components);
        gPropSuite->propGetString(handle, kOfxImageEffectPropPixelDepth, 0, &depth);
        pixelBytes = (components && depth) ? componentCount(components) * bytesPerComponent(depth) : 0;

        return data != nullptr;
    }

    char* pixel(int x,
                int y) const
    {
        if ((x < bounds.x1) || (x >= bounds.x2) || (y < bounds.y1) || (y >= bounds.y2)) {
            return nullptr;
        }

        return data + (ptrdiff_t)(y - bounds.y1) * rowBytes + (ptrdiff_t)(x - bounds.x1) * pixelBytes;
    }

    ~ImageAccess()
    {
        if (handle) {
            gEffectSuite->clipReleaseImage(handle);
        }
    }
};

OfxStatus
copySourceToOutput(OfxImageEffectHandle effect,
                   OfxPropertySetHandle inArgs)
{
    OfxTime time = 0.;
    gPropSuite->propGetDouble(inArgs, kOfxPropTime, 0, &time);
    OfxRectI window = { 0, 0, 0, 0 };
    gPropSuite->propGetIntN(inArgs, kOfxImageEffectPropRenderWindow, 4, &window.x1);

    ImageAccess dst;
    ImageAccess src;
    if (!dst.fetch(clip(effect, kOfxImageEffectOutputClipName), time) || !src.fetch(clip(effect, kOfxImageEffectSimpleSourceClipName), time)) {
        return kOfxStatOK;
    }
    if ((dst.pixelBytes == 0) || (dst.pixelBytes != src.pixelBytes)) {
        return kOfxStatErrImageFormat;
    }

    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            char* to = dst.pixel(x, y);
            if (!to) {
                continue;
            }
            if (const char* from = src.pixel(x, y)) {
                std::memcpy(to, from, (size_t)dst.pixelBytes);
            } else {
                std::memset(to, 0, (size_t)dst.pixelBytes);
            }
        }
    }

    return kOfxStatOK;
}

// ---------------------------------------------------------------------------------------------
// Print

struct EnumeratedKeys {
    std::vector<std::string> keys;
};

OfxStatus
collectKey(const char* key,
           void* userData)
{
    static_cast<EnumeratedKeys*>(userData)->keys.push_back(key);

    return kOfxStatOK;
}

std::string
valueText(OfxPropertySetHandle metadata,
          const char* key,
          OfxPropDataType type)
{
    int dimension = 0;
    gPropSuite->propGetDimension(metadata, key, &dimension);
    std::ostringstream text;
    for (int i = 0; i < dimension; ++i) {
        if (i) {
            text << ",";
        }
        switch (type) {
        case kOfxPropDataTypeInteger: {
            int value = 0;
            gPropSuite->propGetInt(metadata, key, i, &value);
            text << value;
            break;
        }
        case kOfxPropDataTypeDouble: {
            double value = 0.;
            gPropSuite->propGetDouble(metadata, key, i, &value);
            text << value;
            break;
        }
        default: {
            const char* value = nullptr;
            gPropSuite->propGetString(metadata, key, i, &value);
            text << (value ? value : "");
            break;
        }
        }
    }

    return text.str();
}

const char*
typeName(OfxPropDataType type)
{
    switch (type) {
    case kOfxPropDataTypeInteger:
        return "int";
    case kOfxPropDataTypeDouble:
        return "double";
    case kOfxPropDataTypeString:
        return "string";
    default:
        return "none";
    }
}

// One log message per key: a host log takes a line at a time.
void
logSourceMetadata(OfxImageEffectHandle effect,
                  OfxTime time)
{
    if (!gMetadataSuite) {
        return;
    }
    OfxPropertySetHandle metadata = nullptr;
    if (gMetadataSuite->clipGetMetadata(clip(effect, kOfxImageEffectSimpleSourceClipName), time, &metadata) != kOfxStatOK) {
        return;
    }
    EnumeratedKeys enumerated;
    gMetadataSuite->metadataEnumerate(metadata, collectKey, &enumerated);
    for (const std::string& key : enumerated.keys) {
        OfxPropDataType type = kOfxPropDataTypeNone;
        gPropSuite->propGetType(metadata, key.c_str(), &type);
        std::ostringstream line;
        line << "clip=" << kOfxImageEffectSimpleSourceClipName
             << " frame=" << time
             << " key=" << key
             << " type=" << typeName(type)
             << " value=" << valueText(metadata, key.c_str(), type);
        gMessageSuite->message(effect, kOfxMessageLog, "", "%s", line.str().c_str());
    }
    gMetadataSuite->metadataRelease(metadata);
}

// ---------------------------------------------------------------------------------------------
// Contribute

OfxStatus
contributeMetadata(OfxImageEffectHandle effect,
                   OfxPropertySetHandle inArgs,
                   OfxPropertySetHandle outArgs)
{
    if (!gMetadataSuite) {
        return kOfxStatReplyDefault;
    }
    OfxPropertySetHandle metadata = contributionSet(inArgs);
    const std::string prefix(kNatronTestMetadataContributeKeyPrefix);

    const std::string note = stringParam(effect, kNatronTestMetadataContributeParamNote);
    gMetadataSuite->metadataSetString(metadata, (prefix + "note").c_str(), note.c_str());
    gMetadataSuite->metadataSetInt(metadata, (prefix + "revision").c_str(), 1);
    gMetadataSuite->metadataSetDouble(metadata, (prefix + "quality").c_str(), 0.75);
    const char* const tags[] = { "reviewed", "approved" };
    gMetadataSuite->metadataSetStringN(metadata, (prefix + "tags").c_str(), 2, tags);
    const int renderRegion[] = { 0, 0, 1280, 720 };
    gMetadataSuite->metadataSetIntN(metadata, (prefix + "renderRegion").c_str(), 4, renderRegion);
    const double weights[] = { 1., 0.5, 0.25 };
    gMetadataSuite->metadataSetDoubleN(metadata, (prefix + "weights").c_str(), 3, weights);
    gMetadataSuite->metadataSetDouble(metadata, kOfxMetadataKeyFrameRate, kNatronTestMetadataContributeFrameRate);

    switch (intParam(effect, kNatronTestMetadataContributeParamMode)) {
    case kNatronTestMetadataContributeModeDropOneKey: {
        const std::string dropKey = stringParam(effect, kNatronTestMetadataContributeParamDropKey);
        const std::string retainedName = std::string("OfxImageClipPropMetadataRetainedKeys_") + kOfxImageEffectSimpleSourceClipName;
        std::vector<std::string> kept;
        for (const std::string& key : stringValues(outArgs, retainedName.c_str())) {
            if (key != dropKey) {
                kept.push_back(key);
            }
        }
        setStringValues(outArgs, retainedName.c_str(), kept);
        break;
    }
    case kNatronTestMetadataContributeModeInheritNothing:
        setStringValues(outArgs, kOfxImageEffectPropMetadataSourceClip, std::vector<std::string>());
        break;
    default:
        break;
    }

    return kOfxStatOK;
}

// ---------------------------------------------------------------------------------------------
// TimeCode: non-drop-frame, wrapped into 24 hours

const int kFixedOrigin = 1;

int
roundedRate(double rate)
{
    const int rounded = int(rate + 0.5);

    return rounded < 1 ? 1 : rounded;
}

// HH:MM:SS:FF with any non-digit separating the fields; all zero with fewer than four numbers.
long long
timecodeToFrames(const std::string& text,
                 int rate)
{
    int fields[4] = { 0, 0, 0, 0 };
    int read = 0;
    size_t pos = 0;
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
        return 0;
    }

    return (((long long)fields[0] * 60 + fields[1]) * 60 + fields[2]) * rate + fields[3];
}

std::string
framesToTimecode(long long frames,
                 int rate)
{
    const long long day = 24LL * 60 * 60 * rate;
    frames %= day;
    if (frames < 0) {
        frames += day;
    }
    const long long seconds = frames / rate;
    std::ostringstream os;
    os << std::setfill('0')
       << std::setw(2) << (seconds / 3600) << ":"
       << std::setw(2) << ((seconds / 60) % 60) << ":"
       << std::setw(2) << (seconds % 60) << ":"
       << std::setw(2) << (frames % rate);

    return os.str();
}

OfxStatus
timeCodeMetadata(OfxImageEffectHandle effect,
                 OfxPropertySetHandle inArgs)
{
    if (!gMetadataSuite) {
        return kOfxStatReplyDefault;
    }
    OfxTime time = 0.;
    gPropSuite->propGetDouble(inArgs, kOfxPropTime, 0, &time);

    double rate = doubleParam(effect, kNatronTestMetadataTimeCodeParamRate);
    if (intParam(effect, kNatronTestMetadataTimeCodeParamRateFromMetadata)) {
        OfxPropertySetHandle source = nullptr;
        if (gMetadataSuite->clipGetMetadata(clip(effect, kOfxImageEffectSimpleSourceClipName), time, &source) == kOfxStatOK) {
            double sourceRate = 0.;
            if (gPropSuite->propGetDouble(source, kOfxMetadataKeyFrameRate, 0, &sourceRate) == kOfxStatOK) {
                rate = sourceRate;
            }
            gMetadataSuite->metadataRelease(source);
        }
    }
    const int counted = roundedRate(rate);

    const double origin = intParam(effect, kNatronTestMetadataTimeCodeParamUseStartFrame)
        ? double(intParam(effect, kNatronTestMetadataTimeCodeParamStartFrame))
        : double(kFixedOrigin);
    const long long offset = (long long)std::floor(time - origin + 0.5);
    const std::string startTimecode = stringParam(effect, kNatronTestMetadataTimeCodeParamStartTimecode);
    const std::string timecode = framesToTimecode(timecodeToFrames(startTimecode, counted) + offset, counted);

    OfxPropertySetHandle metadata = contributionSet(inArgs);
    gMetadataSuite->metadataSetString(metadata, kOfxMetadataKeyTimecode, timecode.c_str());
    gMetadataSuite->metadataSetDouble(metadata, kOfxMetadataKeyFrameRate, rate);

    return kOfxStatOK;
}

// ---------------------------------------------------------------------------------------------
// Entry points

OfxStatus
mainEntry(Kind kind,
          const char* action,
          const void* handle,
          OfxPropertySetHandle inArgs,
          OfxPropertySetHandle outArgs)
{
    OfxImageEffectHandle effect = (OfxImageEffectHandle)handle;

    if (std::strcmp(action, kOfxActionLoad) == 0) {
        return fetchSuites();
    }
    if (std::strcmp(action, kOfxActionDescribe) == 0) {
        switch (kind) {
        case Kind::Contribute:
            describeEffect(effect, "Metadata Contribute");
            break;
        case Kind::Print:
            describeEffect(effect, "Metadata Print");
            break;
        case Kind::TimeCode:
            describeEffect(effect, "Metadata Time Code");
            break;
        }

        return kOfxStatOK;
    }
    if (std::strcmp(action, kOfxImageEffectActionDescribeInContext) == 0) {
        return describeInContext(kind, effect);
    }
    if (std::strcmp(action, kOfxImageEffectActionRender) == 0) {
        if (kind == Kind::Print) {
            OfxTime time = 0.;
            gPropSuite->propGetDouble(inArgs, kOfxPropTime, 0, &time);
            logSourceMetadata(effect, time);
        }

        return copySourceToOutput(effect, inArgs);
    }
    if (std::strcmp(action, kOfxImageEffectActionGetMetadata) == 0) {
        switch (kind) {
        case Kind::Contribute:
            return contributeMetadata(effect, inArgs, outArgs);
        case Kind::TimeCode:
            return timeCodeMetadata(effect, inArgs);
        case Kind::Print:
            break;
        }
    }

    return kOfxStatReplyDefault;
}

OfxStatus
contributeEntry(const char* action,
                const void* handle,
                OfxPropertySetHandle inArgs,
                OfxPropertySetHandle outArgs)
{
    return mainEntry(Kind::Contribute, action, handle, inArgs, outArgs);
}

OfxStatus
printEntry(const char* action,
           const void* handle,
           OfxPropertySetHandle inArgs,
           OfxPropertySetHandle outArgs)
{
    return mainEntry(Kind::Print, action, handle, inArgs, outArgs);
}

OfxStatus
timeCodeEntry(const char* action,
              const void* handle,
              OfxPropertySetHandle inArgs,
              OfxPropertySetHandle outArgs)
{
    return mainEntry(Kind::TimeCode, action, handle, inArgs, outArgs);
}

void
setHost(OfxHost* host)
{
    gHost = host;
}

OfxPlugin gPlugins[] = {
    { kOfxImageEffectPluginApi, kOfxImageEffectPluginApiVersion, kNatronTestMetadataContributePluginID,
      kNatronTestMetadataContributeMajor, 0, setHost, contributeEntry },
    { kOfxImageEffectPluginApi, kOfxImageEffectPluginApiVersion, kNatronTestMetadataPrintPluginID,
      kNatronTestMetadataPrintMajor, 0, setHost, printEntry },
    { kOfxImageEffectPluginApi, kOfxImageEffectPluginApiVersion, kNatronTestMetadataTimeCodePluginID,
      kNatronTestMetadataTimeCodeMajor, 0, setHost, timeCodeEntry },
};

} // anonymous namespace

OfxExport int
OfxGetNumberOfPlugins(void)
{
    return (int)(sizeof(gPlugins) / sizeof(gPlugins[0]));
}

OfxExport OfxPlugin*
OfxGetPlugin(int nth)
{
    if ((nth < 0) || (nth >= OfxGetNumberOfPlugins())) {
        return nullptr;
    }

    return &gPlugins[nth];
}
