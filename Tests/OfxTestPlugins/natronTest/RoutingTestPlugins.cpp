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
#include <string>

#include "ofxNatron.h"
#include "ofxsImageEffect.h"

#include "NatronTestPluginIDs.h"
#include "PluginList.h"

using namespace OFX;

namespace {

// The bundle declares float as its only bit depth, so every image it is handed is float.
// Pixels are widened to RGBA by component count: one component is alpha, and a missing alpha
// reads as opaque. Outside an image's bounds, or with no image, every channel reads 0.
void
readRGBA(const Image* image,
         int x,
         int y,
         float rgba[4])
{
    rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0.f;
    const float* pix = image ? static_cast<const float*>(image->getPixelAddress(x, y)) : 0;
    if (!pix) {
        return;
    }
    const int nComps = image->getPixelComponentCount();
    if (nComps == 1) {
        rgba[3] = pix[0];

        return;
    }
    for (int c = 0; c < std::min(nComps, 4); ++c) {
        rgba[c] = pix[c];
    }
    if (nComps < 4) {
        rgba[3] = 1.f;
    }
}

void
writeRGBA(Image* image,
          int x,
          int y,
          const float rgba[4])
{
    float* pix = static_cast<float*>(image->getPixelAddress(x, y));
    if (!pix) {
        return;
    }
    const int nComps = image->getPixelComponentCount();
    if (nComps == 1) {
        pix[0] = rgba[3];

        return;
    }
    for (int c = 0; c < std::min(nComps, 4); ++c) {
        pix[c] = rgba[c];
    }
}

Image*
fetchDstImage(Clip* dstClip,
              double time)
{
    Image* dst = dstClip->fetchImage(time);
    if (!dst) {
        throwSuiteStatusException(kOfxStatFailed);
    }
    if (dst->getPixelDepth() != eBitDepthFloat) {
        delete dst;
        throwSuiteStatusException(kOfxStatErrImageFormat);
    }

    return dst;
}

const Image*
fetchSrcImage(Clip* clip,
              double time)
{
    if (!clip || !clip->isConnected()) {
        return 0;
    }
    Image* src = clip->fetchImage(time);
    if (src && (src->getPixelDepth() != eBitDepthFloat)) {
        delete src;
        throwSuiteStatusException(kOfxStatErrImageFormat);
    }

    return src;
}

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

void
addColorComponents(ClipDescriptor* clip)
{
    clip->addSupportedComponent(ePixelComponentRGBA);
    clip->addSupportedComponent(ePixelComponentRGB);
    clip->addSupportedComponent(ePixelComponentAlpha);
    clip->setSupportsTiles(true);
}

void
describeProcessQuad(ImageEffectDescriptor& desc,
                    PageParamDescriptor* page,
                    bool processADefault = true)
{
    static const char* const names[4] = { kNatronOfxParamProcessR, kNatronOfxParamProcessG, kNatronOfxParamProcessB, kNatronOfxParamProcessA };
    static const char* const labels[4] = { kNatronOfxParamProcessRLabel, kNatronOfxParamProcessGLabel, kNatronOfxParamProcessBLabel, kNatronOfxParamProcessALabel };
    static const char* const hints[4] = { kNatronOfxParamProcessRHint, kNatronOfxParamProcessGHint, kNatronOfxParamProcessBHint, kNatronOfxParamProcessAHint };

    for (int c = 0; c < 4; ++c) {
        BooleanParamDescriptor* param = desc.defineBooleanParam(names[c]);
        param->setLabel(labels[c]);
        param->setHint(hints[c]);
        param->setDefault((c < 3) || processADefault);
        if (c < 3) {
            param->setLayoutHint(eLayoutHintNoNewLine, 1);
        }
        page->addChild(*param);
    }
}

void
fetchProcessQuad(ImageEffect& effect,
                 BooleanParam* quad[4])
{
    quad[0] = effect.fetchBooleanParam(kNatronOfxParamProcessR);
    quad[1] = effect.fetchBooleanParam(kNatronOfxParamProcessG);
    quad[2] = effect.fetchBooleanParam(kNatronOfxParamProcessB);
    quad[3] = effect.fetchBooleanParam(kNatronOfxParamProcessA);
}

// --- Two-input copy ----------------------------------------------------------------------------

class TwoInputCopyPlugin
    : public ImageEffect {
public:
    explicit TwoInputCopyPlugin(OfxImageEffectHandle handle)
        : ImageEffect(handle)
        , _dstClip(fetchClip(kOfxImageEffectOutputClipName))
        , _srcClips { fetchClip(kNatronTestTwoInputCopyClipA), fetchClip(kNatronTestTwoInputCopyClipB) }
        , _which(fetchIntParam(kNatronTestTwoInputCopyParamWhich))
        , _passThrough(fetchBooleanParam(kNatronTestTwoInputCopyParamPassThrough))
    {
    }

private:
    Clip* selectedClip(double time) const
    {
        return _srcClips[std::clamp(_which->getValueAtTime(time), 0, 1)];
    }

    virtual void render(const RenderArguments& args) override
    {
        auto_ptr<Image> dst(fetchDstImage(_dstClip, args.time));
        auto_ptr<const Image> src(fetchSrcImage(selectedClip(args.time), args.time));

        for (int y = args.renderWindow.y1; y < args.renderWindow.y2; ++y) {
            for (int x = args.renderWindow.x1; x < args.renderWindow.x2; ++x) {
                float rgba[4];
                readRGBA(src.get(), x, y, rgba);
                writeRGBA(dst.get(), x, y, rgba);
            }
        }
    }

    virtual bool isIdentity(const IsIdentityArguments& args,
                            Clip*& identityClip,
                            double& /*identityTime*/,
                            int& /*view*/,
                            std::string& /*plane*/) override
    {
        if (!_passThrough->getValueAtTime(args.time)) {
            return false;
        }
        identityClip = selectedClip(args.time);

        return true;
    }

    virtual void getRegionsOfInterest(const RegionsOfInterestArguments& args,
                                      RegionOfInterestSetter& rois) override
    {
        const OfxRectD emptyRoI = { 0., 0., 0., 0. };
        Clip* selected = selectedClip(args.time);

        for (int i = 0; i < 2; ++i) {
            rois.setRegionOfInterest(*_srcClips[i], _srcClips[i] == selected ? args.regionOfInterest : emptyRoI);
        }
    }

    virtual bool getRegionOfDefinition(const RegionOfDefinitionArguments& args,
                                       OfxRectD& rod) override
    {
        Clip* selected = selectedClip(args.time);
        if (!selected->isConnected()) {
            return false;
        }
        rod = selected->getRegionOfDefinition(args.time);

        return true;
    }

    // As Switch does: the output takes the chosen input's format, which is only well defined
    // while the choice is not animated.
    virtual void getClipPreferences(ClipPreferencesSetter& clipPreferences) override
    {
        if (_which->getNumKeys() != 0) {
            return;
        }
        Clip* selected = _srcClips[std::clamp(_which->getValue(), 0, 1)];
        if (!selected->isConnected()) {
            return;
        }
        OfxRectI format;
        selected->getFormat(format);
        clipPreferences.setOutputFormat(format);
        clipPreferences.setPixelAspectRatio(*_dstClip, selected->getPixelAspectRatio());
    }

    Clip* _dstClip;
    Clip* _srcClips[2];
    IntParam* _which;
    BooleanParam* _passThrough;
};

mDeclarePluginFactory(TwoInputCopyPluginFactory, {}, {});

void
TwoInputCopyPluginFactory::describe(ImageEffectDescriptor& desc)
{
    desc.setLabel("TwoInputCopy");
    // The host gives routing plug-ins a channel set defaulting to every layer, but it lists the
    // shipped ones by identifier; filing this one under Filter gets it the same default.
    desc.setPluginGrouping("Filter");
    desc.setPluginDescription("Test fixture: outputs input 0 or input 1, chosen by \"which\".");
    desc.addSupportedContext(eContextGeneral);
    describeCommon(desc);
    desc.setChannelSelector(ePixelComponentNone);
}

void
TwoInputCopyPluginFactory::describeInContext(ImageEffectDescriptor& desc,
                                             ContextEnum /*context*/)
{
    const char* const srcNames[2] = { kNatronTestTwoInputCopyClipA, kNatronTestTwoInputCopyClipB };
    for (int i = 0; i < 2; ++i) {
        ClipDescriptor* src = desc.defineClip(srcNames[i]);
        addColorComponents(src);
        src->setOptional(true);
        src->setTemporalClipAccess(false);
        src->setIsMask(false);
    }
    addColorComponents(desc.defineClip(kOfxImageEffectOutputClipName));

    PageParamDescriptor* page = desc.definePageParam("Controls");
    {
        IntParamDescriptor* param = desc.defineIntParam(kNatronTestTwoInputCopyParamWhich);
        param->setLabel("Which");
        param->setHint("The input copied to the output.");
        param->setDefault(0);
        param->setRange(0, 1);
        param->setDisplayRange(0, 1);
        param->setAnimates(true);
        desc.addClipPreferencesSlaveParam(*param);
        page->addChild(*param);
    }
    {
        BooleanParamDescriptor* param = desc.defineBooleanParam(kNatronTestTwoInputCopyParamPassThrough);
        param->setLabel("Pass Through");
        param->setHint("When checked, the chosen input is returned as an identity; otherwise it is rendered into a copy.");
        param->setDefault(true);
        param->setAnimates(true);
        page->addChild(*param);
    }
}

ImageEffect*
TwoInputCopyPluginFactory::createInstance(OfxImageEffectHandle handle,
                                          ContextEnum /*context*/)
{
    return new TwoInputCopyPlugin(handle);
}

// --- Generator ---------------------------------------------------------------------------------

enum GeneratorExtentEnum {
    eGeneratorExtentProject = 0,
    eGeneratorExtentFormat,
};

class GeneratorPlugin
    : public ImageEffect {
public:
    explicit GeneratorPlugin(OfxImageEffectHandle handle)
        : ImageEffect(handle)
        , _dstClip(fetchClip(kOfxImageEffectOutputClipName))
        , _srcClip(fetchClip(kOfxImageEffectSimpleSourceClipName))
        , _color(fetchRGBAParam(kNatronTestGeneratorParamColor))
        , _extent(fetchChoiceParam(kNatronTestGeneratorParamExtent))
        , _formatSize(fetchInt2DParam(kNatronParamFormatSize))
        , _formatPar(fetchDoubleParam(kNatronParamFormatPar))
        , _outputComponents(fetchChoiceParam(kNatronTestGeneratorParamOutputComponents))
    {
        fetchProcessQuad(*this, _process);
    }

private:
    bool extentAtTime(double time,
                      OfxRectD* rod) const
    {
        if (_extent->getValueAtTime(time) == eGeneratorExtentFormat) {
            int w = 0, h = 0;
            _formatSize->getValueAtTime(time, w, h);
            const double par = _formatPar->getValueAtTime(time);
            rod->x1 = 0.;
            rod->y1 = 0.;
            rod->x2 = w * par;
            rod->y2 = h;

            return true;
        }
        const OfxPointD offset = getProjectOffset();
        const OfxPointD size = getProjectSize();
        rod->x1 = offset.x;
        rod->y1 = offset.y;
        rod->x2 = offset.x + size.x;
        rod->y2 = offset.y + size.y;

        return true;
    }

    virtual void render(const RenderArguments& args) override
    {
        auto_ptr<Image> dst(fetchDstImage(_dstClip, args.time));
        auto_ptr<const Image> src(fetchSrcImage(_srcClip, args.time));

        OfxRectD extent;
        extentAtTime(args.time, &extent);
        const double par = dst->getPixelAspectRatio();
        const int x1 = (int)std::floor(extent.x1 * args.renderScale.x / par);
        const int x2 = (int)std::ceil(extent.x2 * args.renderScale.x / par);
        const int y1 = (int)std::floor(extent.y1 * args.renderScale.y);
        const int y2 = (int)std::ceil(extent.y2 * args.renderScale.y);

        double color[4];
        _color->getValueAtTime(args.time, color[0], color[1], color[2], color[3]);
        bool process[4];
        for (int c = 0; c < 4; ++c) {
            process[c] = _process[c]->getValueAtTime(args.time);
        }

        for (int y = args.renderWindow.y1; y < args.renderWindow.y2; ++y) {
            for (int x = args.renderWindow.x1; x < args.renderWindow.x2; ++x) {
                float rgba[4];
                readRGBA(src.get(), x, y, rgba);
                if ((x >= x1) && (x < x2) && (y >= y1) && (y < y2)) {
                    for (int c = 0; c < 4; ++c) {
                        if (process[c]) {
                            rgba[c] = (float)color[c];
                        }
                    }
                }
                writeRGBA(dst.get(), x, y, rgba);
            }
        }
    }

    virtual bool getRegionOfDefinition(const RegionOfDefinitionArguments& args,
                                       OfxRectD& rod) override
    {
        return extentAtTime(args.time, &rod);
    }

    virtual void getClipPreferences(ClipPreferencesSetter& clipPreferences) override
    {
        static const PixelComponentEnum components[3] = { ePixelComponentRGBA, ePixelComponentRGB, ePixelComponentAlpha };
        clipPreferences.setClipComponents(*_dstClip, components[std::clamp(_outputComponents->getValue(), 0, 2)]);

        if (_extent->getValue() == eGeneratorExtentFormat) {
            int w = 0, h = 0;
            _formatSize->getValue(w, h);
            const OfxRectI format = { 0, 0, w, h };
            clipPreferences.setOutputFormat(format);
            clipPreferences.setPixelAspectRatio(*_dstClip, _formatPar->getValue());
        } else {
            clipPreferences.setPixelAspectRatio(*_dstClip, getProjectPixelAspectRatio());
        }
    }

    Clip* _dstClip;
    Clip* _srcClip;
    RGBAParam* _color;
    ChoiceParam* _extent;
    Int2DParam* _formatSize;
    DoubleParam* _formatPar;
    ChoiceParam* _outputComponents;
    BooleanParam* _process[4];
};

mDeclarePluginFactory(GeneratorPluginFactory, {}, {});
mDeclarePluginFactory(RGBGeneratorPluginFactory, {}, {});

void
describeGenerator(ImageEffectDescriptor& desc,
                  const char* label,
                  const char* description)
{
    desc.setLabel(label);
    desc.setPluginGrouping("Image");
    desc.setPluginDescription(description);
    desc.addSupportedContext(eContextGenerator);
    desc.addSupportedContext(eContextGeneral);
    describeCommon(desc);
    desc.setChannelSelector(ePixelComponentNone);
}

void
describeGeneratorInContext(ImageEffectDescriptor& desc,
                           bool processADefault)
{
    // Optional in both contexts, as the openfx-misc generators declare it, so the host offers the
    // same input whichever context it instantiates.
    ClipDescriptor* src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    addColorComponents(src);
    src->setOptional(true);
    addColorComponents(desc.defineClip(kOfxImageEffectOutputClipName));

    PageParamDescriptor* page = desc.definePageParam("Controls");
    describeProcessQuad(desc, page, processADefault);
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kNatronTestGeneratorParamExtent);
        param->setLabel("Extent");
        param->setHint("The region filled: the project's, or the format below.");
        param->appendOption("Project", "Fill the project extent.", kNatronTestGeneratorExtentProject);
        param->appendOption("Format", "Fill the chosen format.", kNatronTestGeneratorExtentFormat);
        param->setDefault(eGeneratorExtentProject);
        param->setAnimates(false);
        desc.addClipPreferencesSlaveParam(*param);
        page->addChild(*param);
    }
    {
        // The host replaces these options with the project's formats and keeps the size and
        // aspect ratio below in step with the choice.
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kNatronParamFormatChoice);
        param->setLabel("Format");
        param->appendOption("PC_Video 640x480", "", "PC_Video");
        param->setDefault(0);
        param->setAnimates(false);
        desc.addClipPreferencesSlaveParam(*param);
        page->addChild(*param);
    }
    {
        Int2DParamDescriptor* param = desc.defineInt2DParam(kNatronParamFormatSize);
        param->setLabel("Size");
        param->setDefault(640, 480);
        param->setIsSecretAndDisabled(true);
        param->setAnimates(false);
        desc.addClipPreferencesSlaveParam(*param);
        page->addChild(*param);
    }
    {
        DoubleParamDescriptor* param = desc.defineDoubleParam(kNatronParamFormatPar);
        param->setLabel("Pixel Aspect Ratio");
        param->setDefault(1.);
        param->setRange(0., 100.);
        param->setIsSecretAndDisabled(true);
        param->setAnimates(false);
        desc.addClipPreferencesSlaveParam(*param);
        page->addChild(*param);
    }
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kNatronTestGeneratorParamOutputComponents);
        param->setLabel("Output Components");
        param->appendOption("RGBA", "", "RGBA");
        param->appendOption("RGB", "", "RGB");
        param->appendOption("Alpha", "", "Alpha");
        param->setDefault(0);
        param->setAnimates(false);
        desc.addClipPreferencesSlaveParam(*param);
        page->addChild(*param);
    }
    {
        RGBAParamDescriptor* param = desc.defineRGBAParam(kNatronTestGeneratorParamColor);
        param->setLabel("Color");
        param->setHint("The colour filled.");
        param->setDefault(1., 1., 1., 1.);
        param->setAnimates(true);
        page->addChild(*param);
    }
}

void
GeneratorPluginFactory::describe(ImageEffectDescriptor& desc)
{
    describeGenerator(desc, "Generator", "Test fixture: fills its colour over the project or a chosen format, over the Source if one is connected.");
}

void
GeneratorPluginFactory::describeInContext(ImageEffectDescriptor& desc,
                                          ContextEnum /*context*/)
{
    describeGeneratorInContext(desc, true);
}

ImageEffect*
GeneratorPluginFactory::createInstance(OfxImageEffectHandle handle,
                                       ContextEnum /*context*/)
{
    return new GeneratorPlugin(handle);
}

void
RGBGeneratorPluginFactory::describe(ImageEffectDescriptor& desc)
{
    describeGenerator(desc, "RGBGenerator", "Test fixture: the Generator whose process quad leaves alpha off by default.");
}

void
RGBGeneratorPluginFactory::describeInContext(ImageEffectDescriptor& desc,
                                             ContextEnum /*context*/)
{
    describeGeneratorInContext(desc, false);
}

ImageEffect*
RGBGeneratorPluginFactory::createInstance(OfxImageEffectHandle handle,
                                          ContextEnum /*context*/)
{
    return new GeneratorPlugin(handle);
}

// --- RGBA point op -----------------------------------------------------------------------------

#define kParamUnPremultBy "unPremultBy"
#define kParamUnPremultByChannel "unPremultByChannel"
#define kParamPremultChanged "premultChanged"

class PointMatrixPlugin
    : public ImageEffect {
public:
    explicit PointMatrixPlugin(OfxImageEffectHandle handle)
        : ImageEffect(handle)
        , _dstClip(fetchClip(kOfxImageEffectOutputClipName))
        , _srcClip(fetchClip(kOfxImageEffectSimpleSourceClipName))
        , _maskClip(fetchClip(kNatronTestPointMatrixClipMask))
        , _rows { fetchRGBAParam(kNatronTestPointMatrixParamOutputRed), fetchRGBAParam(kNatronTestPointMatrixParamOutputGreen),
                  fetchRGBAParam(kNatronTestPointMatrixParamOutputBlue), fetchRGBAParam(kNatronTestPointMatrixParamOutputAlpha) }
        , _unPremultBy(fetchBooleanParam(kParamUnPremultBy))
        , _unPremultByChannel(fetchChoiceParam(kParamUnPremultByChannel))
        , _mix(fetchDoubleParam(kNatronTestPointMatrixParamMix))
        , _maskInvert(fetchBooleanParam(kNatronTestPointMatrixParamMaskInvert))
    {
        fetchProcessQuad(*this, _process);
    }

private:
    virtual void render(const RenderArguments& args) override
    {
        auto_ptr<Image> dst(fetchDstImage(_dstClip, args.time));
        auto_ptr<const Image> src(fetchSrcImage(_srcClip, args.time));
        auto_ptr<const Image> mask(fetchSrcImage(_maskClip, args.time));

        double m[4][4];
        for (int r = 0; r < 4; ++r) {
            _rows[r]->getValueAtTime(args.time, m[r][0], m[r][1], m[r][2], m[r][3]);
        }
        bool process[4];
        for (int c = 0; c < 4; ++c) {
            process[c] = _process[c]->getValueAtTime(args.time);
        }
        const bool unPremult = _unPremultBy->getValueAtTime(args.time);
        const int unPremultChannel = std::clamp(_unPremultByChannel->getValueAtTime(args.time), 0, 3);
        const double mix = _mix->getValueAtTime(args.time);
        const bool maskInvert = _maskInvert->getValueAtTime(args.time);

        for (int y = args.renderWindow.y1; y < args.renderWindow.y2; ++y) {
            for (int x = args.renderWindow.x1; x < args.renderWindow.x2; ++x) {
                float in[4];
                readRGBA(src.get(), x, y, in);

                double s[4] = { in[0], in[1], in[2], in[3] };
                const double divisor = in[unPremultChannel];
                const bool divide = unPremult && (divisor != 0.);
                if (divide) {
                    for (int c = 0; c < 3; ++c) {
                        s[c] /= divisor;
                    }
                }
                double out[4];
                for (int r = 0; r < 4; ++r) {
                    out[r] = m[r][0] * s[0] + m[r][1] * s[1] + m[r][2] * s[2] + m[r][3] * s[3];
                }
                if (divide) {
                    for (int c = 0; c < 3; ++c) {
                        out[c] *= divisor;
                    }
                }

                double amount = mix;
                if (mask.get()) {
                    float maskRGBA[4];
                    readRGBA(mask.get(), x, y, maskRGBA);
                    amount *= maskInvert ? 1. - maskRGBA[3] : maskRGBA[3];
                }

                float result[4];
                for (int c = 0; c < 4; ++c) {
                    result[c] = process[c] ? (float)(in[c] + (out[c] - in[c]) * amount) : in[c];
                }
                writeRGBA(dst.get(), x, y, result);
            }
        }
    }

    virtual bool isIdentity(const IsIdentityArguments& args,
                            Clip*& identityClip,
                            double& /*identityTime*/,
                            int& /*view*/,
                            std::string& /*plane*/) override
    {
        bool anyProcessed = false;
        for (int c = 0; c < 4; ++c) {
            anyProcessed = anyProcessed || _process[c]->getValueAtTime(args.time);
        }
        if (anyProcessed && (_mix->getValueAtTime(args.time) != 0.)) {
            return false;
        }
        identityClip = _srcClip;

        return true;
    }

    Clip* _dstClip;
    Clip* _srcClip;
    Clip* _maskClip;
    RGBAParam* _rows[4];
    BooleanParam* _process[4];
    BooleanParam* _unPremultBy;
    ChoiceParam* _unPremultByChannel;
    DoubleParam* _mix;
    BooleanParam* _maskInvert;
};

mDeclarePluginFactory(PointMatrixPluginFactory, {}, {});
mDeclarePluginFactory(RGBPointPluginFactory, {}, {});

void
describePointMatrix(ImageEffectDescriptor& desc,
                    const char* label,
                    const char* description)
{
    desc.setLabel(label);
    desc.setPluginGrouping("Color/Math");
    desc.setPluginDescription(description);
    desc.addSupportedContext(eContextFilter);
    desc.addSupportedContext(eContextGeneral);
    describeCommon(desc);
    desc.setChannelSelector(ePixelComponentNone);
}

void
describePointMatrixInContext(ImageEffectDescriptor& desc,
                             bool processADefault)
{
    ClipDescriptor* src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    addColorComponents(src);
    src->setTemporalClipAccess(false);
    src->setIsMask(false);
    addColorComponents(desc.defineClip(kOfxImageEffectOutputClipName));
    ClipDescriptor* mask = desc.defineClip(kNatronTestPointMatrixClipMask);
    mask->addSupportedComponent(ePixelComponentAlpha);
    mask->setTemporalClipAccess(false);
    mask->setOptional(true);
    mask->setSupportsTiles(true);
    mask->setIsMask(true);

    PageParamDescriptor* page = desc.definePageParam("Controls");
    describeProcessQuad(desc, page, processADefault);

    const char* const rowNames[4] = { kNatronTestPointMatrixParamOutputRed, kNatronTestPointMatrixParamOutputGreen,
                                      kNatronTestPointMatrixParamOutputBlue, kNatronTestPointMatrixParamOutputAlpha };
    const char* const rowLabels[4] = { "Output Red", "Output Green", "Output Blue", "Output Alpha" };
    for (int r = 0; r < 4; ++r) {
        RGBAParamDescriptor* param = desc.defineRGBAParam(rowNames[r]);
        param->setLabel(rowLabels[r]);
        param->setHint("The weights of R, G, B and A summed into this output channel.");
        param->setDefault(r == 0 ? 1. : 0., r == 1 ? 1. : 0., r == 2 ? 1. : 0., r == 3 ? 1. : 0.);
        param->setAnimates(true);
        page->addChild(*param);
    }
    {
        BooleanParamDescriptor* param = desc.defineBooleanParam(kParamUnPremultBy);
        param->setLabel("(Un)premult by");
        param->setHint("Divide R, G and B by the channel below before the matrix, and multiply them back after.");
        param->setDefault(false);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        page->addChild(*param);
    }
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamUnPremultByChannel);
        param->setLabel("");
        param->appendOption("R", "R channel from input", "r");
        param->appendOption("G", "G channel from input", "g");
        param->appendOption("B", "B channel from input", "b");
        param->appendOption("A", "A channel from input", "a");
        param->setDefault(3);
        page->addChild(*param);
    }
    {
        BooleanParamDescriptor* param = desc.defineBooleanParam(kNatronTestPointMatrixParamMaskInvert);
        param->setLabel("Invert Mask");
        param->setHint("When checked, the effect is fully applied where the mask is 0.");
        param->setDefault(false);
        page->addChild(*param);
    }
    {
        DoubleParamDescriptor* param = desc.defineDoubleParam(kNatronTestPointMatrixParamMix);
        param->setLabel("Mix");
        param->setHint("Mix factor between the original and the transformed image.");
        param->setDefault(1.);
        param->setRange(0., 1.);
        param->setDisplayRange(0., 1.);
        page->addChild(*param);
    }
    {
        // ColorMatrix keeps this hidden flag; the host hides it by name, as it does ColorMatrix's.
        BooleanParamDescriptor* param = desc.defineBooleanParam(kParamPremultChanged);
        param->setDefault(false);
        param->setIsSecretAndDisabled(true);
        param->setAnimates(false);
        param->setEvaluateOnChange(false);
        page->addChild(*param);
    }
}

void
PointMatrixPluginFactory::describe(ImageEffectDescriptor& desc)
{
    describePointMatrix(desc, "PointMatrix", "Test fixture: multiplies RGBA by a 4x4 matrix, one output* row per output channel.");
}

void
PointMatrixPluginFactory::describeInContext(ImageEffectDescriptor& desc,
                                            ContextEnum /*context*/)
{
    describePointMatrixInContext(desc, true);
}

ImageEffect*
PointMatrixPluginFactory::createInstance(OfxImageEffectHandle handle,
                                         ContextEnum /*context*/)
{
    return new PointMatrixPlugin(handle);
}

void
RGBPointPluginFactory::describe(ImageEffectDescriptor& desc)
{
    describePointMatrix(desc, "RGBPoint", "Test fixture: the PointMatrix whose process quad leaves alpha off by default.");
}

void
RGBPointPluginFactory::describeInContext(ImageEffectDescriptor& desc,
                                         ContextEnum /*context*/)
{
    describePointMatrixInContext(desc, false);
}

ImageEffect*
RGBPointPluginFactory::createInstance(OfxImageEffectHandle handle,
                                      ContextEnum /*context*/)
{
    return new PointMatrixPlugin(handle);
}

} // anonymous namespace

namespace NatronTestOfx {
void
getRoutingPluginIDs(OFX::PluginFactoryArray& ids)
{
    static TwoInputCopyPluginFactory twoInputCopy(kNatronTestTwoInputCopyPluginID, kNatronTestTwoInputCopyMajor, 0);
    static GeneratorPluginFactory generator(kNatronTestGeneratorPluginID, kNatronTestGeneratorMajor, 0);
    static PointMatrixPluginFactory pointMatrix(kNatronTestPointMatrixPluginID, kNatronTestPointMatrixMajor, 0);
    static RGBGeneratorPluginFactory rgbGenerator(kNatronTestRGBGeneratorPluginID, kNatronTestRGBGeneratorMajor, 0);
    static RGBPointPluginFactory rgbPoint(kNatronTestRGBPointPluginID, kNatronTestRGBPointMajor, 0);

    ids.push_back(&twoInputCopy);
    ids.push_back(&generator);
    ids.push_back(&pointMatrix);
    ids.push_back(&rgbGenerator);
    ids.push_back(&rgbPoint);
}
} // namespace NatronTestOfx
