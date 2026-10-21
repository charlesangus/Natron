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

#include <algorithm>
#include <cmath>
#include <list>
#include <string>
#include <vector>

#include "ofxNatron.h"
#include "ofxsImageEffect.h"

#include "NatronTestPluginIDs.h"
#include "PluginList.h"

using namespace OFX;

namespace {

// --- Planes ------------------------------------------------------------------------------------
//
// The option IDs below are the ones openfx-supportext's ofxsMultiPlane builds, since the host
// recognises and rewrites a plug-in's plane and channel choices by those IDs: a plane option is
// the plane's ID, a channel option is "<planeID>.<channel>", and "0" and "1" are constants.

const char* const kChannelConstant0 = "0";
const char* const kChannelConstant1 = "1";
const char* const kColourChannels[4] = { "R", "G", "B", "A" };

struct PlaneDesc {
    std::string ofxPlane;
    std::string id;
    std::string label;
    std::vector<std::string> channels;
};

bool
describePlane(const std::string& ofxPlane,
              PlaneDesc* plane)
{
    plane->ofxPlane = ofxPlane;
    plane->channels.clear();
    if (ofxPlane == kFnOfxImagePlaneColour) {
        plane->id = ofxPlane;
        plane->label = "Color";
        plane->channels.assign(kColourChannels, kColourChannels + 4);

        return true;
    }
    if ((ofxPlane == kFnOfxImagePlaneBackwardMotionVector) || (ofxPlane == kFnOfxImagePlaneForwardMotionVector)) {
        plane->id = ofxPlane;
        plane->label = (ofxPlane == kFnOfxImagePlaneBackwardMotionVector) ? "Backward" : "Forward";
        plane->channels.push_back("U");
        plane->channels.push_back("V");

        return true;
    }
    if ((ofxPlane == kFnOfxImagePlaneStereoDisparityLeft) || (ofxPlane == kFnOfxImagePlaneStereoDisparityRight)) {
        plane->id = ofxPlane;
        plane->label = (ofxPlane == kFnOfxImagePlaneStereoDisparityLeft) ? "DisparityLeft" : "DisparityRight";
        plane->channels.push_back("X");
        plane->channels.push_back("Y");

        return true;
    }
    std::string channelsLabel;
    if (!extractCustomPlane(ofxPlane, &plane->id, &plane->label, &channelsLabel, &plane->channels) || plane->channels.empty()) {
        return false;
    }
    if (plane->label.empty()) {
        plane->label = plane->id;
    }

    return true;
}

PlaneDesc
colourPlane()
{
    PlaneDesc plane;
    describePlane(kFnOfxImagePlaneColour, &plane);

    return plane;
}

// Colour first, whether or not the clip has it, so a choice's default index means colour.
std::vector<PlaneDesc>
planesOf(const Clip* clip)
{
    std::vector<PlaneDesc> planes(1, colourPlane());
    if (!clip || !clip->isConnected()) {
        return planes;
    }
    std::vector<std::string> present;
    clip->getPlanesPresent(&present);
    for (std::size_t i = 0; i < present.size(); ++i) {
        PlaneDesc plane;
        if (describePlane(present[i], &plane) && (plane.id != planes[0].id)) {
            planes.push_back(plane);
        }
    }

    return planes;
}

const PlaneDesc*
findPlane(const std::vector<PlaneDesc>& planes,
          const std::string& id)
{
    for (std::size_t i = 0; i < planes.size(); ++i) {
        if (planes[i].id == id) {
            return &planes[i];
        }
    }

    return 0;
}

std::string
selectedOption(ChoiceParam* param,
               double time)
{
    const int index = param->getValueAtTime(time);
    std::string id;
    if ((index >= 0) && (index < param->getNOptions())) {
        param->getEnum(index, id);
    }

    return id;
}

// Only touches the menu when its options change, since every reset makes the host repopulate it.
void
setOptions(ChoiceParam* param,
           const std::vector<std::string>& labels,
           const std::vector<std::string>& hints,
           const std::vector<std::string>& ids)
{
    std::vector<std::string> currentLabels, currentHints, currentIDs;
    param->getOptions(&currentLabels, &currentHints, &currentIDs);
    if ((currentIDs == ids) && (currentLabels == labels)) {
        return;
    }
    param->resetOptions(labels, hints, ids);
}

void
appendPlaneOption(const PlaneDesc& plane,
                  std::vector<std::string>* labels,
                  std::vector<std::string>* hints,
                  std::vector<std::string>* ids)
{
    labels->push_back(plane.label);
    hints->push_back(std::string());
    ids->push_back(plane.id);
}

void
appendChannelOptions(const PlaneDesc& plane,
                     const std::string& clipName,
                     std::vector<std::string>* labels,
                     std::vector<std::string>* hints,
                     std::vector<std::string>* ids)
{
    for (std::size_t c = 0; c < plane.channels.size(); ++c) {
        labels->push_back(plane.label + "." + plane.channels[c]);
        // The host reads the clip a single-clip channel choice belongs to from this hint.
        hints->push_back(plane.channels[c] + " channel from input " + clipName);
        ids->push_back(plane.id + "." + plane.channels[c]);
    }
}

void
appendConstantOptions(std::vector<std::string>* labels,
                      std::vector<std::string>* hints,
                      std::vector<std::string>* ids)
{
    labels->push_back(kChannelConstant0);
    hints->push_back("0 constant channel");
    ids->push_back(kChannelConstant0);
    labels->push_back(kChannelConstant1);
    hints->push_back("1 constant channel");
    ids->push_back(kChannelConstant1);
}

void
describePlaneChoice(ChoiceParamDescriptor* param)
{
    std::vector<std::string> labels, hints, ids;
    appendPlaneOption(colourPlane(), &labels, &hints, &ids);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        param->appendOption(labels[i], hints[i], ids[i]);
    }
}

void
describeChannelChoice(ChoiceParamDescriptor* param,
                      const std::string& clipName)
{
    std::vector<std::string> labels, hints, ids;
    appendChannelOptions(colourPlane(), clipName, &labels, &hints, &ids);
    appendConstantOptions(&labels, &hints, &ids);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        param->appendOption(labels[i], hints[i], ids[i]);
    }
}

void
rebuildPlaneChoice(ChoiceParam* param,
                   const std::vector<PlaneDesc>& planes)
{
    std::vector<std::string> labels, hints, ids;
    for (std::size_t i = 0; i < planes.size(); ++i) {
        appendPlaneOption(planes[i], &labels, &hints, &ids);
    }
    setOptions(param, labels, hints, ids);
}

// The colour channels and the constants keep their describe-time indices, so a default stays put
// as the menu grows.
void
rebuildChannelChoice(ChoiceParam* param,
                     const std::vector<PlaneDesc>& planes,
                     const std::string& clipName)
{
    std::vector<std::string> labels, hints, ids;
    appendChannelOptions(planes[0], clipName, &labels, &hints, &ids);
    appendConstantOptions(&labels, &hints, &ids);
    for (std::size_t i = 1; i < planes.size(); ++i) {
        appendChannelOptions(planes[i], clipName, &labels, &hints, &ids);
    }
    setOptions(param, labels, hints, ids);
}

// A channel choice resolved against its clip: a constant, or one channel of one plane.
struct ChannelSource {
    bool isConstant;
    double constant;
    std::string ofxPlane;
    bool colour;
    int channel;

    ChannelSource()
        : isConstant(true)
        , constant(0.)
        , ofxPlane()
        , colour(false)
        , channel(-1)
    {
    }
};

// An option naming a plane or channel the clip does not have reads as the constant 0.
ChannelSource
resolveChannelOption(const std::string& option,
                     const Clip* clip)
{
    ChannelSource ret;
    if (option == kChannelConstant1) {
        ret.constant = 1.;

        return ret;
    }
    const std::string::size_type dot = option.rfind('.');
    if ((dot == std::string::npos) || !clip || !clip->isConnected()) {
        return ret;
    }
    const PlaneDesc* plane = 0;
    const std::vector<PlaneDesc> planes = planesOf(clip);
    plane = findPlane(planes, option.substr(0, dot));
    if (!plane) {
        return ret;
    }
    std::vector<std::string>::const_iterator found = std::find(plane->channels.begin(), plane->channels.end(), option.substr(dot + 1));
    if (found == plane->channels.end()) {
        return ret;
    }
    ret.isConstant = false;
    ret.ofxPlane = plane->ofxPlane;
    ret.colour = plane->ofxPlane == kFnOfxImagePlaneColour;
    ret.channel = (int)(found - plane->channels.begin());

    return ret;
}

// The component of an image holding `channel` of its plane, or -1. A one-component colour image
// is alpha.
int
componentOf(const Image* image,
            bool colour,
            int channel)
{
    const int nComps = image->getPixelComponentCount();
    if (colour && (nComps == 1)) {
        return channel == 3 ? 0 : -1;
    }

    return channel < nComps ? channel : -1;
}

// Outside the image, or on a channel the image lacks, reads 0.
double
channelValue(const ChannelSource& source,
             const Image* image,
             int x,
             int y)
{
    if (source.isConstant) {
        return source.constant;
    }
    if (!image) {
        return 0.;
    }
    const int comp = componentOf(image, source.colour, source.channel);
    const float* pix = static_cast<const float*>(image->getPixelAddress(x, y));

    return (pix && (comp >= 0)) ? pix[comp] : 0.;
}

// --- Shared ------------------------------------------------------------------------------------

void
describeCommon(ImageEffectDescriptor& desc)
{
    desc.addSupportedBitDepth(eBitDepthFloat);
    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setSupportsMultiResolution(true);
    desc.setSupportsTiles(true);
    desc.setTemporalClipAccess(false);
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(false);
    desc.setSupportsMultipleClipDepths(false);
    desc.setRenderThreadSafety(eRenderFullySafe);
}

ClipDescriptor*
defineColourClip(ImageEffectDescriptor& desc,
                 const char* name)
{
    ClipDescriptor* clip = desc.defineClip(name);
    clip->addSupportedComponent(ePixelComponentRGBA);
    clip->addSupportedComponent(ePixelComponentRGB);
    clip->addSupportedComponent(ePixelComponentAlpha);
    clip->setSupportsTiles(true);
    clip->setTemporalClipAccess(false);

    return clip;
}

void
checkFloat(const Image* image)
{
    if (image && (image->getPixelDepth() != eBitDepthFloat)) {
        throwSuiteStatusException(kOfxStatErrImageFormat);
    }
}

void
copyPixels(const Image* src,
           Image* dst,
           const OfxRectI& window)
{
    const int nComps = dst->getPixelComponentCount();
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            float* out = static_cast<float*>(dst->getPixelAddress(x, y));
            if (!out) {
                continue;
            }
            const float* in = src ? static_cast<const float*>(src->getPixelAddress(x, y)) : 0;
            const int srcComps = src ? src->getPixelComponentCount() : 0;
            for (int c = 0; c < nComps; ++c) {
                out[c] = (in && (c < srcComps)) ? in[c] : 0.f;
            }
        }
    }
}

// --- Multiplanar pass-through ------------------------------------------------------------------

class MultiPlanePlugin
    : public ImageEffect {
public:
    explicit MultiPlanePlugin(OfxImageEffectHandle handle)
        : ImageEffect(handle)
        , _dstClip(fetchClip(kOfxImageEffectOutputClipName))
        , _srcClip(fetchClip(kOfxImageEffectSimpleSourceClipName))
        , _inputPlane(fetchChoiceParam(kNatronTestMultiPlaneParamInputPlane))
        , _allPlanes(fetchBooleanParam(kNatronTestMultiPlaneParamProcessAllPlanes))
        , _byChannel(fetchChoiceParam(kNatronTestMultiPlaneParamByChannel))
    {
        _process[0] = fetchBooleanParam(kNatronOfxParamProcessR);
        _process[1] = fetchBooleanParam(kNatronOfxParamProcessG);
        _process[2] = fetchBooleanParam(kNatronOfxParamProcessB);
        _process[3] = fetchBooleanParam(kNatronOfxParamProcessA);
    }

private:
    // The OFX plane string of the chosen plane; colour when the input lacks it.
    std::string selectedPlane(double time) const
    {
        const std::vector<PlaneDesc> planes = planesOf(_srcClip);
        const PlaneDesc* plane = findPlane(planes, selectedOption(_inputPlane, time));

        return plane ? plane->ofxPlane : std::string(kFnOfxImagePlaneColour);
    }

    virtual void render(const RenderArguments& args) override
    {
        const bool allPlanes = _allPlanes->getValueAtTime(args.time);
        const std::string chosen = allPlanes ? std::string() : selectedPlane(args.time);
        const ChannelSource by = resolveChannelOption(selectedOption(_byChannel, args.time), _srcClip);
        bool process[4];
        for (int c = 0; c < 4; ++c) {
            process[c] = _process[c]->getValueAtTime(args.time);
        }

        auto_ptr<const Image> byImage(by.isConstant ? 0 : _srcClip->fetchImagePlane(args.time, args.renderView, by.ofxPlane.c_str()));
        checkFloat(byImage.get());

        for (std::list<std::string>::const_iterator it = args.planes.begin(); it != args.planes.end(); ++it) {
            auto_ptr<Image> dst(_dstClip->fetchImagePlane(args.time, args.renderView, it->c_str()));
            if (!dst.get()) {
                throwSuiteStatusException(kOfxStatFailed);
            }
            checkFloat(dst.get());
            auto_ptr<const Image> src(_srcClip->isConnected() ? _srcClip->fetchImagePlane(args.time, args.renderView, it->c_str()) : 0);
            checkFloat(src.get());
            copyPixels(src.get(), dst.get(), args.renderWindow);
            if (!allPlanes && (*it != chosen)) {
                continue;
            }

            const bool colour = *it == kFnOfxImagePlaneColour;
            const int nComps = dst->getPixelComponentCount();
            for (int y = args.renderWindow.y1; y < args.renderWindow.y2; ++y) {
                for (int x = args.renderWindow.x1; x < args.renderWindow.x2; ++x) {
                    float* pix = static_cast<float*>(dst->getPixelAddress(x, y));
                    if (!pix) {
                        continue;
                    }
                    const double factor = channelValue(by, byImage.get(), x, y);
                    for (int c = 0; c < nComps; ++c) {
                        const int quad = (colour && (nComps == 1)) ? 3 : c;
                        if ((quad < 4) && process[quad]) {
                            pix[c] = (float)(pix[c] * factor);
                        }
                    }
                }
            }
        }
    }

    virtual bool isIdentity(const IsIdentityArguments& args,
                            Clip*& identityClip,
                            double& /*identityTime*/,
                            int& /*view*/,
                            std::string& /*plane*/) override
    {
        if (!_srcClip->isConnected()) {
            return false;
        }
        bool anyProcessed = false;
        for (int c = 0; c < 4; ++c) {
            anyProcessed = anyProcessed || _process[c]->getValueAtTime(args.time);
        }
        if (anyProcessed && (selectedOption(_byChannel, args.time) != kChannelConstant1)) {
            return false;
        }
        identityClip = _srcClip;

        return true;
    }

    virtual OfxStatus getClipComponents(const ClipComponentsArguments& args,
                                        ClipComponentsSetter& clipComponents) override
    {
        std::vector<std::string> processed;
        if (_allPlanes->getValueAtTime(args.time)) {
            const std::vector<PlaneDesc> planes = planesOf(_srcClip);
            for (std::size_t i = 0; i < planes.size(); ++i) {
                processed.push_back(planes[i].ofxPlane);
            }
        } else {
            processed.push_back(selectedPlane(args.time));
        }
        for (std::size_t i = 0; i < processed.size(); ++i) {
            clipComponents.addClipPlane(*_srcClip, processed[i]);
            clipComponents.addClipPlane(*_dstClip, processed[i]);
        }
        const ChannelSource by = resolveChannelOption(selectedOption(_byChannel, args.time), _srcClip);
        if (!by.isConstant && (std::find(processed.begin(), processed.end(), by.ofxPlane) == processed.end())) {
            clipComponents.addClipPlane(*_srcClip, by.ofxPlane);
        }
        clipComponents.setPassThroughClip(_srcClip, args.time, args.view);

        return kOfxStatOK;
    }

    virtual void getClipPreferences(ClipPreferencesSetter& /*clipPreferences*/) override
    {
        const std::vector<PlaneDesc> planes = planesOf(_srcClip);
        rebuildPlaneChoice(_inputPlane, planes);
        rebuildChannelChoice(_byChannel, planes, kOfxImageEffectSimpleSourceClipName);
    }

    Clip* _dstClip;
    Clip* _srcClip;
    ChoiceParam* _inputPlane;
    BooleanParam* _allPlanes;
    ChoiceParam* _byChannel;
    BooleanParam* _process[4];
};

mDeclarePluginFactory(MultiPlanePluginFactory, {}, {});

void
MultiPlanePluginFactory::describe(ImageEffectDescriptor& desc)
{
    desc.setLabel("MultiPlane");
    desc.setPluginGrouping("Filter");
    desc.setPluginDescription("Test fixture: multiplies the chosen channels of one plane, or of every plane, by a channel of the input.");
    desc.addSupportedContext(eContextFilter);
    desc.addSupportedContext(eContextGeneral);
    describeCommon(desc);
    desc.setChannelSelector(ePixelComponentNone);
    desc.setIsMultiPlanar(true);
    desc.setPassThroughForNotProcessedPlanes(ePassThroughLevelPassThroughNonRenderedPlanes);
}

void
MultiPlanePluginFactory::describeInContext(ImageEffectDescriptor& desc,
                                           ContextEnum /*context*/)
{
    ClipDescriptor* src = defineColourClip(desc, kOfxImageEffectSimpleSourceClipName);
    src->setIsMask(false);
    defineColourClip(desc, kOfxImageEffectOutputClipName);

    PageParamDescriptor* page = desc.definePageParam("Controls");
    static const char* const names[4] = { kNatronOfxParamProcessR, kNatronOfxParamProcessG, kNatronOfxParamProcessB, kNatronOfxParamProcessA };
    static const char* const labels[4] = { kNatronOfxParamProcessRLabel, kNatronOfxParamProcessGLabel, kNatronOfxParamProcessBLabel, kNatronOfxParamProcessALabel };
    for (int c = 0; c < 4; ++c) {
        BooleanParamDescriptor* param = desc.defineBooleanParam(names[c]);
        param->setLabel(labels[c]);
        param->setHint("Multiply this channel.");
        // As Premult's: alpha is left out, rather than multiplied by itself.
        param->setDefault(c != 3);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        page->addChild(*param);
    }
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kNatronTestMultiPlaneParamInputPlane);
        param->setLabel("Plane");
        param->setHint("The plane whose channels are multiplied; the others pass through.");
        describePlaneChoice(param);
        param->setDefault(0);
        param->setAnimates(false);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        desc.addClipPreferencesSlaveParam(*param);
        page->addChild(*param);
    }
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kNatronTestMultiPlaneParamByChannel);
        param->setLabel("Multiply by");
        param->setHint("The channel of the input the plane is multiplied by.");
        describeChannelChoice(param, kOfxImageEffectSimpleSourceClipName);
        param->setDefault(3);
        param->setAnimates(false);
        desc.addClipPreferencesSlaveParam(*param);
        page->addChild(*param);
    }
    {
        BooleanParamDescriptor* param = desc.defineBooleanParam(kNatronTestMultiPlaneParamProcessAllPlanes);
        param->setLabel("All Planes");
        param->setHint("Multiply every plane of the input, not just the chosen one.");
        param->setDefault(false);
        param->setAnimates(false);
        desc.addClipPreferencesSlaveParam(*param);
        page->addChild(*param);
    }
}

ImageEffect*
MultiPlanePluginFactory::createInstance(OfxImageEffectHandle handle,
                                        ContextEnum /*context*/)
{
    return new MultiPlanePlugin(handle);
}

// --- Matrix warp -------------------------------------------------------------------------------

struct Matrix3 {
    double m[3][3];

    static Matrix3 identity()
    {
        Matrix3 ret;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                ret.m[r][c] = (r == c) ? 1. : 0.;
            }
        }

        return ret;
    }

    static Matrix3 scale(double sx,
                         double sy)
    {
        Matrix3 ret = identity();
        ret.m[0][0] = sx;
        ret.m[1][1] = sy;

        return ret;
    }

    static Matrix3 fromRowMajor(const double v[9])
    {
        Matrix3 ret;
        for (int i = 0; i < 9; ++i) {
            ret.m[i / 3][i % 3] = v[i];
        }

        return ret;
    }

    void toRowMajor(double v[9]) const
    {
        for (int i = 0; i < 9; ++i) {
            v[i] = m[i / 3][i % 3];
        }
    }

    Matrix3 operator*(const Matrix3& o) const
    {
        Matrix3 ret;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                ret.m[r][c] = m[r][0] * o.m[0][c] + m[r][1] * o.m[1][c] + m[r][2] * o.m[2][c];
            }
        }

        return ret;
    }

    bool isIdentity() const
    {
        const Matrix3 id = identity();
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                if (m[r][c] != id.m[r][c]) {
                    return false;
                }
            }
        }

        return true;
    }

    bool inverse(Matrix3* inv) const
    {
        const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
            - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
            + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        if (det == 0.) {
            return false;
        }
        const double s = 1. / det;
        inv->m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * s;
        inv->m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * s;
        inv->m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * s;
        inv->m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * s;
        inv->m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * s;
        inv->m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * s;
        inv->m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * s;
        inv->m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * s;
        inv->m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * s;

        return true;
    }

    // False when the point maps to infinity.
    bool apply(double x,
               double y,
               double* ox,
               double* oy) const
    {
        const double w = m[2][0] * x + m[2][1] * y + m[2][2];
        if (w == 0.) {
            return false;
        }
        *ox = (m[0][0] * x + m[0][1] * y + m[0][2]) / w;
        *oy = (m[1][0] * x + m[1][1] * y + m[1][2]) / w;

        return true;
    }
};

Matrix3
canonicalToPixel(double par,
                 const OfxPointD& renderScale)
{
    return Matrix3::scale(renderScale.x / par, renderScale.y);
}

Matrix3
pixelToCanonical(double par,
                 const OfxPointD& renderScale)
{
    return Matrix3::scale(par / renderScale.x, 1. / renderScale.y);
}

class MatrixWarpPlugin
    : public ImageEffect {
public:
    explicit MatrixWarpPlugin(OfxImageEffectHandle handle)
        : ImageEffect(handle)
        , _dstClip(fetchClip(kOfxImageEffectOutputClipName))
        , _srcClip(fetchClip(kOfxImageEffectSimpleSourceClipName))
        , _uvClip(fetchClip(kNatronTestMatrixWarpClipUV))
        , _rows { fetchDouble3DParam(kNatronTestMatrixWarpParamRow1), fetchDouble3DParam(kNatronTestMatrixWarpParamRow2),
                  fetchDouble3DParam(kNatronTestMatrixWarpParamRow3) }
        , _uvChannels { fetchChoiceParam(kNatronTestMatrixWarpParamChannelU), fetchChoiceParam(kNatronTestMatrixWarpParamChannelV),
                        fetchChoiceParam(kNatronTestMatrixWarpParamChannelA) }
    {
    }

private:
    // In canonical coordinates, from source to output.
    Matrix3 matrixAtTime(double time) const
    {
        Matrix3 ret;
        for (int r = 0; r < 3; ++r) {
            _rows[r]->getValueAtTime(time, ret.m[r][0], ret.m[r][1], ret.m[r][2]);
        }

        return ret;
    }

    void uvSources(double time,
                   ChannelSource sources[3]) const
    {
        for (int i = 0; i < 3; ++i) {
            sources[i] = resolveChannelOption(selectedOption(_uvChannels[i], time), _uvClip);
        }
    }

    // With no UV offset and an opaque UV alpha the warp is the matrix alone.
    bool uvIsNeutral(double time) const
    {
        if (!_uvClip->isConnected()) {
            return true;
        }
        ChannelSource sources[3];
        uvSources(time, sources);

        return sources[0].isConstant && (sources[0].constant == 0.) && sources[1].isConstant && (sources[1].constant == 0.)
            && sources[2].isConstant && (sources[2].constant == 1.);
    }

    virtual void render(const RenderArguments& args) override
    {
        auto_ptr<Image> dst(_dstClip->fetchImage(args.time));
        if (!dst.get()) {
            throwSuiteStatusException(kOfxStatFailed);
        }
        checkFloat(dst.get());
        auto_ptr<const Image> src(_srcClip->isConnected() ? _srcClip->fetchImage(args.time) : 0);
        checkFloat(src.get());

        ChannelSource uv[3];
        uvSources(args.time, uv);
        auto_ptr<const Image> uvImages[3];
        for (int i = 0; i < 3; ++i) {
            if (!uv[i].isConstant && _uvClip->isConnected()) {
                uvImages[i].reset(_uvClip->fetchImagePlane(args.time, args.renderView, uv[i].ofxPlane.c_str()));
                checkFloat(uvImages[i].get());
            }
        }
        if (!_uvClip->isConnected()) {
            uv[0] = uv[1] = uv[2] = ChannelSource();
            uv[2].constant = 1.;
        }

        const double srcPar = src.get() ? src->getPixelAspectRatio() : 1.;
        Matrix3 forward = canonicalToPixel(dst->getPixelAspectRatio(), args.renderScale) * matrixAtTime(args.time)
            * pixelToCanonical(srcPar, args.renderScale);
        // A source fetched through a concatenated transform comes with that transform still to apply.
        if (src.get() && !src->getTransformIsIdentity()) {
            double attached[9];
            src->getTransform(attached);
            forward = forward * Matrix3::fromRowMajor(attached);
        }
        Matrix3 inverse;
        const bool invertible = forward.inverse(&inverse);

        const int nComps = dst->getPixelComponentCount();
        for (int y = args.renderWindow.y1; y < args.renderWindow.y2; ++y) {
            for (int x = args.renderWindow.x1; x < args.renderWindow.x2; ++x) {
                float* out = static_cast<float*>(dst->getPixelAddress(x, y));
                if (!out) {
                    continue;
                }
                std::fill(out, out + nComps, 0.f);
                double sx = 0., sy = 0.;
                const double u = channelValue(uv[0], uvImages[0].get(), x, y) * args.renderScale.x;
                const double v = channelValue(uv[1], uvImages[1].get(), x, y) * args.renderScale.y;
                if (!src.get() || !invertible || !inverse.apply(x + u + 0.5, y + v + 0.5, &sx, &sy)) {
                    continue;
                }
                const float* in = static_cast<const float*>(src->getPixelAddress((int)std::floor(sx), (int)std::floor(sy)));
                if (!in) {
                    continue;
                }
                const double a = channelValue(uv[2], uvImages[2].get(), x, y);
                for (int c = 0; c < std::min(nComps, src->getPixelComponentCount()); ++c) {
                    out[c] = (float)(in[c] * a);
                }
            }
        }
    }

    virtual bool isIdentity(const IsIdentityArguments& args,
                            Clip*& identityClip,
                            double& /*identityTime*/,
                            int& /*view*/,
                            std::string& /*plane*/) override
    {
        if (!_srcClip->isConnected() || !uvIsNeutral(args.time) || !matrixAtTime(args.time).isIdentity()) {
            return false;
        }
        identityClip = _srcClip;

        return true;
    }

    virtual bool getTransform(const TransformArguments& args,
                              Clip*& transformClip,
                              double transformMatrix[9]) override
    {
        if (!_srcClip->isConnected() || !uvIsNeutral(args.time)) {
            return false;
        }
        const Matrix3 pixel = canonicalToPixel(_dstClip->getPixelAspectRatio(), args.renderScale) * matrixAtTime(args.time)
            * pixelToCanonical(_srcClip->getPixelAspectRatio(), args.renderScale);
        transformClip = _srcClip;
        pixel.toRowMajor(transformMatrix);

        return true;
    }

    // The bounding box of the source's region of definition under the matrix.
    virtual bool getRegionOfDefinition(const RegionOfDefinitionArguments& args,
                                       OfxRectD& rod) override
    {
        if (!_srcClip->isConnected()) {
            return false;
        }
        const OfxRectD srcRoD = _srcClip->getRegionOfDefinition(args.time);
        const Matrix3 matrix = matrixAtTime(args.time);
        const double xs[2] = { srcRoD.x1, srcRoD.x2 };
        const double ys[2] = { srcRoD.y1, srcRoD.y2 };
        for (int i = 0; i < 4; ++i) {
            double x = 0., y = 0.;
            if (!matrix.apply(xs[i % 2], ys[i / 2], &x, &y)) {
                return false;
            }
            rod.x1 = (i == 0) ? x : std::min(rod.x1, x);
            rod.x2 = (i == 0) ? x : std::max(rod.x2, x);
            rod.y1 = (i == 0) ? y : std::min(rod.y1, y);
            rod.y2 = (i == 0) ? y : std::max(rod.y2, y);
        }

        return true;
    }

    // The whole source, rather than the window's preimage: the test images are small.
    virtual void getRegionsOfInterest(const RegionsOfInterestArguments& args,
                                      RegionOfInterestSetter& rois) override
    {
        if (_srcClip->isConnected()) {
            rois.setRegionOfInterest(*_srcClip, _srcClip->getRegionOfDefinition(args.time));
        }
        rois.setRegionOfInterest(*_uvClip, args.regionOfInterest);
    }

    virtual OfxStatus getClipComponents(const ClipComponentsArguments& args,
                                        ClipComponentsSetter& clipComponents) override
    {
        ChannelSource uv[3];
        uvSources(args.time, uv);
        std::vector<std::string> added;
        for (int i = 0; i < 3; ++i) {
            if (!uv[i].isConstant && (std::find(added.begin(), added.end(), uv[i].ofxPlane) == added.end())) {
                added.push_back(uv[i].ofxPlane);
                clipComponents.addClipPlane(*_uvClip, uv[i].ofxPlane);
            }
        }
        clipComponents.setPassThroughClip(_srcClip, args.time, args.view);

        return kOfxStatOK;
    }

    virtual void getClipPreferences(ClipPreferencesSetter& /*clipPreferences*/) override
    {
        const std::vector<PlaneDesc> planes = planesOf(_uvClip);
        for (int i = 0; i < 3; ++i) {
            rebuildChannelChoice(_uvChannels[i], planes, kNatronTestMatrixWarpClipUV);
        }
    }

    Clip* _dstClip;
    Clip* _srcClip;
    Clip* _uvClip;
    Double3DParam* _rows[3];
    ChoiceParam* _uvChannels[3];
};

mDeclarePluginFactory(MatrixWarpPluginFactory, {}, {});

void
MatrixWarpPluginFactory::describe(ImageEffectDescriptor& desc)
{
    desc.setLabel("MatrixWarp");
    desc.setPluginGrouping("Transform");
    desc.setPluginDescription("Test fixture: transforms the Source by a 3x3 matrix, offset per pixel by UV.");
    desc.addSupportedContext(eContextFilter);
    desc.addSupportedContext(eContextGeneral);
    describeCommon(desc);
    desc.setChannelSelector(ePixelComponentNone);
    desc.setCanTransform(true);
    desc.setIsMultiPlanar(true);
    // Every plane the host asks for is warped, each in its own render call.
    desc.setPassThroughForNotProcessedPlanes(ePassThroughLevelRenderAllRequestedPlanes);
}

void
MatrixWarpPluginFactory::describeInContext(ImageEffectDescriptor& desc,
                                           ContextEnum /*context*/)
{
    ClipDescriptor* src = defineColourClip(desc, kOfxImageEffectSimpleSourceClipName);
    src->setIsMask(false);
    src->setCanTransform(true);
    ClipDescriptor* uv = defineColourClip(desc, kNatronTestMatrixWarpClipUV);
    uv->setOptional(true);
    uv->setIsMask(false);
    defineColourClip(desc, kOfxImageEffectOutputClipName);

    PageParamDescriptor* page = desc.definePageParam("Controls");
    const char* const rowNames[3] = { kNatronTestMatrixWarpParamRow1, kNatronTestMatrixWarpParamRow2, kNatronTestMatrixWarpParamRow3 };
    for (int r = 0; r < 3; ++r) {
        Double3DParamDescriptor* param = desc.defineDouble3DParam(rowNames[r]);
        param->setLabel(std::string("Row ") + (char)('1' + r));
        param->setHint("One row of the matrix mapping source to output, in canonical coordinates.");
        param->setDefault(r == 0 ? 1. : 0., r == 1 ? 1. : 0., r == 2 ? 1. : 0.);
        param->setAnimates(true);
        page->addChild(*param);
    }

    const char* const channelNames[3] = { kNatronTestMatrixWarpParamChannelU, kNatronTestMatrixWarpParamChannelV, kNatronTestMatrixWarpParamChannelA };
    const char* const channelLabels[3] = { "U Channel", "V Channel", "Alpha Channel" };
    const char* const channelHints[3] = { "The UV channel giving the horizontal offset, in pixels.",
                                          "The UV channel giving the vertical offset, in pixels.",
                                          "The UV channel the output is multiplied by." };
    const int channelDefaults[3] = { 0, 1, 3 };
    for (int i = 0; i < 3; ++i) {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(channelNames[i]);
        param->setLabel(channelLabels[i]);
        param->setHint(channelHints[i]);
        describeChannelChoice(param, kNatronTestMatrixWarpClipUV);
        param->setDefault(channelDefaults[i]);
        param->setAnimates(false);
        desc.addClipPreferencesSlaveParam(*param);
        page->addChild(*param);
    }
}

ImageEffect*
MatrixWarpPluginFactory::createInstance(OfxImageEffectHandle handle,
                                        ContextEnum /*context*/)
{
    return new MatrixWarpPlugin(handle);
}

// --- Frame window ------------------------------------------------------------------------------

class FrameWindowPlugin
    : public ImageEffect {
public:
    explicit FrameWindowPlugin(OfxImageEffectHandle handle)
        : ImageEffect(handle)
        , _dstClip(fetchClip(kOfxImageEffectOutputClipName))
        , _srcClip(fetchClip(kOfxImageEffectSimpleSourceClipName))
        , _frameRange(fetchInt2DParam(kNatronTestFrameWindowParamFrameRange))
        , _absolute(fetchBooleanParam(kNatronTestFrameWindowParamAbsolute))
        , _timeOffset(fetchIntParam(kNatronTestFrameWindowParamTimeOffset))
    {
    }

private:
    OfxRangeD framesAtTime(double time) const
    {
        int first = 0, last = 0;
        _frameRange->getValueAtTime(time, first, last);
        if (first > last) {
            std::swap(first, last);
        }
        const double origin = _absolute->getValueAtTime(time) ? 0. : time - _timeOffset->getValueAtTime(time);
        OfxRangeD range;
        range.min = origin + first;
        range.max = origin + last;

        return range;
    }

    virtual void render(const RenderArguments& args) override
    {
        auto_ptr<Image> dst(_dstClip->fetchImage(args.time));
        if (!dst.get()) {
            throwSuiteStatusException(kOfxStatFailed);
        }
        checkFloat(dst.get());
        const OfxRectI& window = args.renderWindow;
        const int nComps = dst->getPixelComponentCount();
        const int width = window.x2 - window.x1;
        std::vector<double> sum((std::size_t)width * (window.y2 - window.y1) * nComps, 0.);

        const OfxRangeD range = framesAtTime(args.time);
        int count = 0;
        for (double frame = range.min; frame <= range.max; frame += 1., ++count) {
            auto_ptr<const Image> src(_srcClip->isConnected() ? _srcClip->fetchImage(frame) : 0);
            checkFloat(src.get());
            if (!src.get()) {
                continue;
            }
            const int srcComps = src->getPixelComponentCount();
            for (int y = window.y1; y < window.y2; ++y) {
                for (int x = window.x1; x < window.x2; ++x) {
                    const float* in = static_cast<const float*>(src->getPixelAddress(x, y));
                    if (!in) {
                        continue;
                    }
                    double* acc = &sum[(((std::size_t)(y - window.y1) * width) + (x - window.x1)) * nComps];
                    for (int c = 0; c < std::min(nComps, srcComps); ++c) {
                        acc[c] += in[c];
                    }
                }
            }
        }

        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                float* out = static_cast<float*>(dst->getPixelAddress(x, y));
                if (!out) {
                    continue;
                }
                const double* acc = &sum[(((std::size_t)(y - window.y1) * width) + (x - window.x1)) * nComps];
                for (int c = 0; c < nComps; ++c) {
                    out[c] = count ? (float)(acc[c] / count) : 0.f;
                }
            }
        }
    }

    virtual bool isIdentity(const IsIdentityArguments& args,
                            Clip*& identityClip,
                            double& identityTime,
                            int& /*view*/,
                            std::string& /*plane*/) override
    {
        const OfxRangeD range = framesAtTime(args.time);
        if (range.min != range.max) {
            return false;
        }
        identityClip = _srcClip;
        identityTime = range.min;

        return true;
    }

    virtual void getFramesNeeded(const FramesNeededArguments& args,
                                 FramesNeededSetter& frames) override
    {
        frames.setFramesNeeded(*_srcClip, framesAtTime(args.time));
    }

    virtual bool getRegionOfDefinition(const RegionOfDefinitionArguments& args,
                                       OfxRectD& rod) override
    {
        if (!_srcClip->isConnected()) {
            return false;
        }
        const OfxRangeD range = framesAtTime(args.time);
        for (double frame = range.min; frame <= range.max; frame += 1.) {
            const OfxRectD frameRoD = _srcClip->getRegionOfDefinition(frame);
            if (frame == range.min) {
                rod = frameRoD;
            } else {
                rod.x1 = std::min(rod.x1, frameRoD.x1);
                rod.y1 = std::min(rod.y1, frameRoD.y1);
                rod.x2 = std::max(rod.x2, frameRoD.x2);
                rod.y2 = std::max(rod.y2, frameRoD.y2);
            }
        }

        return true;
    }

    Clip* _dstClip;
    Clip* _srcClip;
    Int2DParam* _frameRange;
    BooleanParam* _absolute;
    IntParam* _timeOffset;
};

mDeclarePluginFactory(FrameWindowPluginFactory, {}, {});

void
FrameWindowPluginFactory::describe(ImageEffectDescriptor& desc)
{
    desc.setLabel("FrameWindow");
    desc.setPluginGrouping("Time");
    desc.setPluginDescription("Test fixture: averages the Source over a range of frames.");
    desc.addSupportedContext(eContextFilter);
    desc.addSupportedContext(eContextGeneral);
    describeCommon(desc);
    desc.setTemporalClipAccess(true);
    desc.setChannelSelector(ePixelComponentNone);
}

void
FrameWindowPluginFactory::describeInContext(ImageEffectDescriptor& desc,
                                            ContextEnum /*context*/)
{
    ClipDescriptor* src = defineColourClip(desc, kOfxImageEffectSimpleSourceClipName);
    src->setTemporalClipAccess(true);
    src->setIsMask(false);
    defineColourClip(desc, kOfxImageEffectOutputClipName);

    PageParamDescriptor* page = desc.definePageParam("Controls");
    {
        Int2DParamDescriptor* param = desc.defineInt2DParam(kNatronTestFrameWindowParamFrameRange);
        param->setLabel("Frame Range");
        param->setHint("The first and last frame averaged.");
        param->setDimensionLabels("min", "max");
        param->setDefault(0, 0);
        param->setAnimates(true);
        page->addChild(*param);
    }
    {
        BooleanParamDescriptor* param = desc.defineBooleanParam(kNatronTestFrameWindowParamAbsolute);
        param->setLabel("Absolute");
        param->setHint("When checked, the frame range is in absolute frames; otherwise it is relative to the time minus the offset.");
        param->setDefault(false);
        param->setAnimates(true);
        page->addChild(*param);
    }
    {
        IntParamDescriptor* param = desc.defineIntParam(kNatronTestFrameWindowParamTimeOffset);
        param->setLabel("Time Offset");
        param->setHint("Frames the relative range is moved back by.");
        param->setDefault(0);
        param->setAnimates(true);
        page->addChild(*param);
    }
}

ImageEffect*
FrameWindowPluginFactory::createInstance(OfxImageEffectHandle handle,
                                         ContextEnum /*context*/)
{
    return new FrameWindowPlugin(handle);
}

// --- OCIO config -------------------------------------------------------------------------------

class OCIOConfigPlugin
    : public ImageEffect {
public:
    explicit OCIOConfigPlugin(OfxImageEffectHandle handle)
        : ImageEffect(handle)
        , _dstClip(fetchClip(kOfxImageEffectOutputClipName))
        , _srcClip(fetchClip(kOfxImageEffectSimpleSourceClipName))
    {
    }

private:
    // Never reached while the Source is connected: every frame is an identity of it.
    virtual void render(const RenderArguments& args) override
    {
        auto_ptr<Image> dst(_dstClip->fetchImage(args.time));
        if (!dst.get()) {
            throwSuiteStatusException(kOfxStatFailed);
        }
        checkFloat(dst.get());
        auto_ptr<const Image> src(_srcClip->isConnected() ? _srcClip->fetchImage(args.time) : 0);
        checkFloat(src.get());
        copyPixels(src.get(), dst.get(), args.renderWindow);
    }

    virtual bool isIdentity(const IsIdentityArguments& /*args*/,
                            Clip*& identityClip,
                            double& /*identityTime*/,
                            int& /*view*/,
                            std::string& /*plane*/) override
    {
        if (!_srcClip->isConnected()) {
            return false;
        }
        identityClip = _srcClip;

        return true;
    }

    Clip* _dstClip;
    Clip* _srcClip;
};

mDeclarePluginFactory(OCIOConfigPluginFactory, {}, {});

void
OCIOConfigPluginFactory::describe(ImageEffectDescriptor& desc)
{
    desc.setLabel("OCIOConfig");
    desc.setPluginGrouping("Color/OCIO");
    desc.setPluginDescription("Test fixture: passes the Source through, carrying the OCIO params the project sets.");
    desc.addSupportedContext(eContextFilter);
    desc.addSupportedContext(eContextGeneral);
    describeCommon(desc);
    desc.setChannelSelector(ePixelComponentNone);
}

void
OCIOConfigPluginFactory::describeInContext(ImageEffectDescriptor& desc,
                                           ContextEnum /*context*/)
{
    ClipDescriptor* src = defineColourClip(desc, kOfxImageEffectSimpleSourceClipName);
    src->setIsMask(false);
    defineColourClip(desc, kOfxImageEffectOutputClipName);

    PageParamDescriptor* page = desc.definePageParam("Controls");
    {
        // Declared as the openfx-io OCIO plug-ins declare it, a file path the host may also
        // set to a built-in "ocio://" config.
        StringParamDescriptor* param = desc.defineStringParam(kNatronTestOCIOConfigParamConfigFile);
        param->setLabel("OCIO Config File");
        param->setHint("The OpenColorIO config, set by the project.");
        param->setStringType(eStringTypeFilePath);
        param->setFilePathExists(true);
        param->setAnimates(false);
        desc.addClipPreferencesSlaveParam(*param);
        page->addChild(*param);
    }
    {
        StringParamDescriptor* param = desc.defineStringParam(kNatronTestOCIOConfigParamWorkingSpace);
        param->setLabel("OCIO Working Space");
        param->setDefault("");
        param->setIsSecretAndDisabled(true);
        param->setAnimates(false);
        page->addChild(*param);
    }
    const char* const spaceNames[2] = { kNatronTestOCIOConfigParamInputSpace, kNatronTestOCIOConfigParamOutputSpace };
    const char* const spaceLabels[2] = { "Input Colorspace", "Output Colorspace" };
    for (int i = 0; i < 2; ++i) {
        StringParamDescriptor* param = desc.defineStringParam(spaceNames[i]);
        param->setLabel(spaceLabels[i]);
        param->setHint("A colorspace name of the config; the project flags one the config lacks.");
        param->setDefault("");
        param->setAnimates(true);
        page->addChild(*param);
    }
}

ImageEffect*
OCIOConfigPluginFactory::createInstance(OfxImageEffectHandle handle,
                                        ContextEnum /*context*/)
{
    return new OCIOConfigPlugin(handle);
}

} // anonymous namespace

namespace NatronTestOfx {
void
getTraitPluginIDs(OFX::PluginFactoryArray& ids)
{
    static MultiPlanePluginFactory multiPlane(kNatronTestMultiPlanePluginID, kNatronTestMultiPlaneMajor, 0);
    static MatrixWarpPluginFactory matrixWarp(kNatronTestMatrixWarpPluginID, kNatronTestMatrixWarpMajor, 0);
    static FrameWindowPluginFactory frameWindow(kNatronTestFrameWindowPluginID, kNatronTestFrameWindowMajor, 0);
    static OCIOConfigPluginFactory ocioConfig(kNatronTestOCIOConfigPluginID, kNatronTestOCIOConfigMajor, 0);

    ids.push_back(&multiPlane);
    ids.push_back(&matrixWarp);
    ids.push_back(&frameWindow);
    ids.push_back(&ocioConfig);
}
} // namespace NatronTestOfx
