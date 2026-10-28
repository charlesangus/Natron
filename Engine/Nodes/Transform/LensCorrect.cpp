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

#include "LensCorrect.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <list>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#include <lensfun.h>

#include <QtCore/QString>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/Transform/LensfunRuntime.h"
#include "Engine/Nodes/Transform/WarpGpu.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

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

void
setEnabled(const KnobIPtr& knob,
           bool enabled)
{
    if (knob) {
        knob->setAllDimensionsEnabled(enabled);
    }
}

// A number the metadata holds as a double or an integer; 0 when it holds neither.
double
metadataNumber(const ImageMetadata& metadata,
               const char* key)
{
    if (const std::optional<double> d = metadata.getDouble(key)) {
        return *d;
    }
    if (const std::optional<int> i = metadata.getInt(key)) {
        return (double)*i;
    }

    return 0.;
}

std::string
metadataString(const ImageMetadata& metadata,
               const char* key)
{
    const std::optional<std::string> s = metadata.getString(key);

    return s ? *s : std::string();
}

std::string
trimmed(const std::string& s)
{
    std::size_t b = 0;
    std::size_t e = s.size();

    while ((b < e) && std::isspace((unsigned char)s[b])) {
        ++b;
    }
    while ((e > b) && std::isspace((unsigned char)s[e - 1])) {
        --e;
    }

    return s.substr(b, e - b);
}

// darktable's _lens_sanitize: the lens name up to " or " or " (", which EXIF lens names list
// alternatives and notes behind.
std::string
sanitizeLens(const std::string& lens)
{
    const std::size_t orPos = lens.find(" or ");
    const std::size_t parenthesisPos = lens.find(" (");
    const std::size_t pos = std::min(orPos, parenthesisPos);

    if ((pos != std::string::npos) && (pos > 0)) {
        return lens.substr(0, pos);
    }

    return lens;
}

std::string
mlstr(const lfMLstr s)
{
    const char* c = lf_mlstr_get(s);

    return c ? std::string(c) : std::string();
}

std::string
formatNumber(double v)
{
    std::ostringstream ss;

    ss << v;

    return ss.str();
}

// darktable's manual vignette: v + mul tanh(b (1 - r)), held at r = 1 past the corner.
float
manualVignette(float strength,
               float radius,
               float steepness,
               float r)
{
    const double b = 1.0 + radius * 10.0;
    const double v = steepness;
    const double mul = -v / std::tanh(b);
    const double rr = std::min(1.f, r);

    return std::max(0.f, 2.f * strength * (float)(v + mul * std::tanh(b * (1.0 - rr))));
}
} // namespace

LensCorrect::LensCorrect(NodePtr node)
    : StagedImageEffect(node)
{
}

LensCorrect::~LensCorrect()
{
}

const NodeSpec*
LensCorrect::nodeSpec() const
{
    return staticNodeSpec<kLensCorrectNodeSpec>();
}

const ImagePipelineSpec&
LensCorrect::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kLensCorrectPipeline, kLensCorrectNodeSpec>();
}

std::string
LensCorrect::getInputHint(int inputNb) const
{
    switch (inputNb) {
    case kLensCorrectInputSource:
        return tr("The image to correct, whose metadata names its camera and lens.").toStdString();
    case kLensCorrectInputMask:
        return tr("The mask limiting where the correction applies.").toStdString();
    default:
        break;
    }

    return std::string();
}

void
LensCorrect::initializeKnobs()
{
    KnobPagePtr page = createControlsPage();

    KnobChoicePtr method = createKnob<KnobChoice>(tr("Method"));
    method->setName(kLensCorrectParamMethod);
    method->setHintToolTip(tr("Where the correction comes from."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kLensCorrectMethodLensfun, "Lensfun Database", tr("Correct with the lensfun database's profile of the camera and lens, plus the manual vignette.").toStdString()));
        options.push_back(ChoiceOption(kLensCorrectMethodVignette, "Manual Vignette Only", tr("Apply only the manual vignette.").toStdString()));
        method->populateChoices(options);
    }
    method->setDefaultValue(0);
    method->setAnimationEnabled(false);
    page->addKnob(method);
    _method = method;

    KnobChoicePtr mode = createKnob<KnobChoice>(tr("Mode"));
    mode->setName(kLensCorrectParamMode);
    mode->setHintToolTip(tr("Correct removes the lens's distortion, TCA and vignetting; Distort adds them, as when rendering "
                            "a CG element to match the plate."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kLensCorrectModeCorrect, "Correct", tr("Remove the lens's defects.").toStdString()));
        options.push_back(ChoiceOption(kLensCorrectModeDistort, "Distort", tr("Add the lens's defects.").toStdString()));
        mode->populateChoices(options);
    }
    mode->setDefaultValue(0);
    mode->setAnimationEnabled(false);
    page->addKnob(mode);
    _mode = mode;

    KnobBoolPtr correctDistortion = createKnob<KnobBool>(tr("Correct Distortion"));
    correctDistortion->setName(kLensCorrectParamCorrectDistortion);
    correctDistortion->setHintToolTip(tr("Correct the lens's geometric distortion."));
    correctDistortion->setDefaultValue(true);
    correctDistortion->setAnimationEnabled(false);
    correctDistortion->setAddNewLine(false);
    page->addKnob(correctDistortion);
    _correctDistortion = correctDistortion;

    KnobBoolPtr correctTCA = createKnob<KnobBool>(tr("Correct TCA"));
    correctTCA->setName(kLensCorrectParamCorrectTCA);
    correctTCA->setHintToolTip(tr("Correct the lens's transverse chromatic aberration: red and blue are moved onto green. "
                                  "Never applied to a monochrome image."));
    correctTCA->setDefaultValue(true);
    correctTCA->setAnimationEnabled(false);
    correctTCA->setAddNewLine(false);
    page->addKnob(correctTCA);
    _correctTCA = correctTCA;

    KnobBoolPtr correctVignetting = createKnob<KnobBool>(tr("Correct Vignetting"));
    correctVignetting->setName(kLensCorrectParamCorrectVignetting);
    correctVignetting->setHintToolTip(tr("Correct the lens's vignetting, which depends on the aperture and focus distance."));
    correctVignetting->setDefaultValue(true);
    correctVignetting->setAnimationEnabled(false);
    page->addKnob(correctVignetting);
    _correctVignetting = correctVignetting;

    KnobStringPtr camera = createKnob<KnobString>(tr("Camera"));
    camera->setName(kLensCorrectParamCamera);
    camera->setHintToolTip(tr("The camera model to look up in the lensfun database, as EXIF names it (\"Canon EOS 5D Mark III\"). "
                              "Empty: the Source's exif/Make and exif/Model metadata. The camera gives the crop factor."));
    camera->setAnimationEnabled(false);
    page->addKnob(camera);
    _camera = camera;

    KnobStringPtr lens = createKnob<KnobString>(tr("Lens"));
    lens->setName(kLensCorrectParamLens);
    lens->setHintToolTip(tr("The lens model to look up in the lensfun database (\"Canon EF 70-200mm f/2.8L IS II USM\"). "
                            "Empty: the Source's exif/LensModel metadata, or the lens of a fixed-lens camera."));
    lens->setAnimationEnabled(false);
    page->addKnob(lens);
    _lens = lens;

    KnobDoublePtr cropFactor = createKnob<KnobDouble>(tr("Crop Factor"));
    cropFactor->setName(kLensCorrectParamCropFactor);
    cropFactor->setHintToolTip(tr("The sensor's crop factor relative to 35 mm film. 0: the camera's, from the database."));
    cropFactor->setDefaultValue(0.);
    cropFactor->setMinimum(0.);
    cropFactor->setDisplayMinimum(0.);
    cropFactor->setDisplayMaximum(10.);
    page->addKnob(cropFactor);
    _cropFactor = cropFactor;

    KnobDoublePtr focalLength = createKnob<KnobDouble>(tr("Focal Length"));
    focalLength->setName(kLensCorrectParamFocalLength);
    focalLength->setHintToolTip(tr("The focal length, in mm. 0: the Source's exif/FocalLength metadata, else the lens's shortest."));
    focalLength->setDefaultValue(0.);
    focalLength->setMinimum(0.);
    focalLength->setDisplayMinimum(0.);
    focalLength->setDisplayMaximum(300.);
    page->addKnob(focalLength);
    _focalLength = focalLength;

    KnobDoublePtr aperture = createKnob<KnobDouble>(tr("Aperture"));
    aperture->setName(kLensCorrectParamAperture);
    aperture->setHintToolTip(tr("The f-number, which vignetting depends on. 0: the Source's exif/FNumber metadata, else the "
                                "lens's widest."));
    aperture->setDefaultValue(0.);
    aperture->setMinimum(0.);
    aperture->setDisplayMinimum(0.);
    aperture->setDisplayMaximum(32.);
    page->addKnob(aperture);
    _aperture = aperture;

    KnobDoublePtr distance = createKnob<KnobDouble>(tr("Distance"));
    distance->setName(kLensCorrectParamDistance);
    distance->setHintToolTip(tr("The focus distance, in metres, which vignetting depends on. 0: the Source's "
                                "exif/SubjectDistance metadata, else 1000."));
    distance->setDefaultValue(0.);
    distance->setMinimum(0.);
    distance->setDisplayMinimum(0.);
    distance->setDisplayMaximum(1000.);
    page->addKnob(distance);
    _distance = distance;

    KnobChoicePtr targetGeometry = createKnob<KnobChoice>(tr("Target Geometry"));
    targetGeometry->setName(kLensCorrectParamTargetGeometry);
    targetGeometry->setHintToolTip(tr("The projection to correct to. Same As Lens keeps the lens's own; another reprojects, "
                                      "for instance a fisheye to rectilinear."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kLensCorrectGeometryLens, "Same As Lens", tr("Keep the lens's projection.").toStdString()));
        options.push_back(ChoiceOption(kLensCorrectGeometryRectilinear, "Rectilinear", std::string()));
        options.push_back(ChoiceOption(kLensCorrectGeometryFisheye, "Fisheye", tr("Equidistant fisheye.").toStdString()));
        options.push_back(ChoiceOption(kLensCorrectGeometryPanoramic, "Panoramic", tr("Cylindrical panorama.").toStdString()));
        options.push_back(ChoiceOption(kLensCorrectGeometryEquirectangular, "Equirectangular", std::string()));
        options.push_back(ChoiceOption(kLensCorrectGeometryOrthographic, "Orthographic", tr("Orthographic fisheye.").toStdString()));
        options.push_back(ChoiceOption(kLensCorrectGeometryStereographic, "Stereographic", tr("Stereographic fisheye.").toStdString()));
        options.push_back(ChoiceOption(kLensCorrectGeometryEquisolid, "Equisolid", tr("Equisolid-angle fisheye.").toStdString()));
        options.push_back(ChoiceOption(kLensCorrectGeometryThoby, "Thoby", tr("Thoby fisheye.").toStdString()));
        targetGeometry->populateChoices(options);
    }
    targetGeometry->setDefaultValue(0);
    targetGeometry->setAnimationEnabled(false);
    page->addKnob(targetGeometry);
    _targetGeometry = targetGeometry;

    KnobBoolPtr autoScale = createKnob<KnobBool>(tr("Auto Scale"));
    autoScale->setName(kLensCorrectParamAutoScale);
    autoScale->setHintToolTip(tr("Scale the corrected image so that no empty border shows, as lensfun computes it."));
    autoScale->setDefaultValue(true);
    autoScale->setAnimationEnabled(false);
    autoScale->setAddNewLine(false);
    page->addKnob(autoScale);
    _autoScale = autoScale;

    KnobDoublePtr scale = createKnob<KnobDouble>(tr("Scale"));
    scale->setName(kLensCorrectParamScale);
    scale->setHintToolTip(tr("The scale of the corrected image, when Auto Scale is off."));
    scale->setDefaultValue(1.);
    scale->setMinimum(0.1);
    scale->setMaximum(2.);
    scale->setDisplayMinimum(0.1);
    scale->setDisplayMaximum(2.);
    page->addKnob(scale);
    _scale = scale;

    KnobBoolPtr tcaOverride = createKnob<KnobBool>(tr("TCA Override"));
    tcaOverride->setName(kLensCorrectParamTCAOverride);
    tcaOverride->setHintToolTip(tr("Replace the database's TCA with the linear one TCA Red and TCA Blue give."));
    tcaOverride->setDefaultValue(false);
    tcaOverride->setAnimationEnabled(false);
    page->addKnob(tcaOverride);
    _tcaOverride = tcaOverride;

    KnobDoublePtr tcaRed = createKnob<KnobDouble>(tr("TCA Red"));
    tcaRed->setName(kLensCorrectParamTCARed);
    tcaRed->setHintToolTip(tr("The scale of the red channel about the lens centre, relative to green."));
    tcaRed->setDefaultValue(1.);
    tcaRed->setMinimum(0.99);
    tcaRed->setMaximum(1.01);
    tcaRed->setDisplayMinimum(0.99);
    tcaRed->setDisplayMaximum(1.01);
    tcaRed->setIncrement(0.0001);
    tcaRed->setAddNewLine(false);
    page->addKnob(tcaRed);
    _tcaRed = tcaRed;

    KnobDoublePtr tcaBlue = createKnob<KnobDouble>(tr("TCA Blue"));
    tcaBlue->setName(kLensCorrectParamTCABlue);
    tcaBlue->setHintToolTip(tr("The scale of the blue channel about the lens centre, relative to green."));
    tcaBlue->setDefaultValue(1.);
    tcaBlue->setMinimum(0.99);
    tcaBlue->setMaximum(1.01);
    tcaBlue->setDisplayMinimum(0.99);
    tcaBlue->setDisplayMaximum(1.01);
    tcaBlue->setIncrement(0.0001);
    page->addKnob(tcaBlue);
    _tcaBlue = tcaBlue;

    KnobDoublePtr vignetteStrength = createKnob<KnobDouble>(tr("Vignette Strength"));
    vignetteStrength->setName(kLensCorrectParamVignetteStrength);
    vignetteStrength->setHintToolTip(tr("How much the manual vignette brightens the image towards its corners, before the "
                                        "lensfun correction. 0 turns it off."));
    vignetteStrength->setDefaultValue(0.);
    vignetteStrength->setMinimum(0.);
    vignetteStrength->setMaximum(1.);
    vignetteStrength->setDisplayMinimum(0.);
    vignetteStrength->setDisplayMaximum(1.);
    page->addKnob(vignetteStrength);
    _vignetteStrength = vignetteStrength;

    KnobDoublePtr vignetteRadius = createKnob<KnobDouble>(tr("Vignette Radius"));
    vignetteRadius->setName(kLensCorrectParamVignetteRadius);
    vignetteRadius->setHintToolTip(tr("How far from the centre the manual vignette starts: higher keeps more of the centre "
                                      "untouched."));
    vignetteRadius->setDefaultValue(0.5);
    vignetteRadius->setMinimum(0.);
    vignetteRadius->setMaximum(1.);
    vignetteRadius->setDisplayMinimum(0.);
    vignetteRadius->setDisplayMaximum(1.);
    page->addKnob(vignetteRadius);
    _vignetteRadius = vignetteRadius;

    KnobDoublePtr vignetteSteepness = createKnob<KnobDouble>(tr("Vignette Steepness"));
    vignetteSteepness->setName(kLensCorrectParamVignetteSteepness);
    vignetteSteepness->setHintToolTip(tr("How much the manual vignette brightens the corners, relative to its strength."));
    vignetteSteepness->setDefaultValue(0.5);
    vignetteSteepness->setMinimum(0.);
    vignetteSteepness->setMaximum(1.);
    vignetteSteepness->setDisplayMinimum(0.);
    vignetteSteepness->setDisplayMaximum(1.);
    page->addKnob(vignetteSteepness);
    _vignetteSteepness = vignetteSteepness;

    KnobBoolPtr showVignetteMask = createKnob<KnobBool>(tr("Show Vignette Mask"));
    showVignetteMask->setName(kLensCorrectParamShowVignetteMask);
    showVignetteMask->setHintToolTip(tr("Write the manual vignette's gain, minus one, to alpha, to see where it applies."));
    showVignetteMask->setDefaultValue(false);
    showVignetteMask->setAnimationEnabled(false);
    page->addKnob(showVignetteMask);
    _showVignetteMask = showVignetteMask;

    KnobChoicePtr filter = createKnob<KnobChoice>(tr(kResamplerParamFilterTypeLabel));
    filter->setName(kResamplerParamFilterType);
    filter->setHintToolTip(tr(kResamplerParamFilterTypeHint));
    {
        const std::vector<Resampler::ChoiceOption>& filters = Resampler::filterOptions();
        std::vector<ChoiceOption> options;
        for (std::size_t i = 0; i < filters.size(); ++i) {
            options.push_back(ChoiceOption(filters[i].id, filters[i].label, filters[i].hint));
        }
        filter->populateChoices(options);
    }
    filter->setDefaultValue((int)Resampler::eFilterCubic);
    filter->setAddNewLine(false);
    filter->setSpacingBetweenItems(1);
    page->addKnob(filter);
    _filter = filter;

    KnobBoolPtr clamp = createKnob<KnobBool>(tr(kResamplerParamFilterClampLabel));
    clamp->setName(kResamplerParamFilterClamp);
    clamp->setHintToolTip(tr(kResamplerParamFilterClampHint));
    clamp->setDefaultValue(false);
    clamp->setAddNewLine(false);
    clamp->setSpacingBetweenItems(1);
    page->addKnob(clamp);
    _clamp = clamp;

    KnobBoolPtr blackOutside = createKnob<KnobBool>(tr(kResamplerParamFilterBlackOutsideLabel));
    blackOutside->setName(kResamplerParamFilterBlackOutside);
    blackOutside->setHintToolTip(tr(kResamplerParamFilterBlackOutsideHint));
    blackOutside->setDefaultValue(true);
    page->addKnob(blackOutside);
    _blackOutside = blackOutside;

    KnobStringPtr info = createKnob<KnobString>(tr("Info"));
    info->setName(kLensCorrectParamInfo);
    info->setHintToolTip(tr("What the lensfun database was searched for and found, and where each value came from: "
                            "knob, metadata, camera, lens or default."));
    info->setAsLabel();
    info->setIsPersistent(false);
    info->setEvaluateOnChange(false);
    page->addKnob(info);
    _info = info;

    addStageKnobs(page);
    updateVisibility();
} // LensCorrect::initializeKnobs

void
LensCorrect::updateVisibility()
{
    KnobChoicePtr method = _method.lock();
    const bool lensfun = !method || (method->getValue() == 0);
    const KnobIPtr lensfunKnobs[] = {
        _mode.lock(),
        _correctDistortion.lock(),
        _correctTCA.lock(),
        _correctVignetting.lock(),
        _camera.lock(),
        _lens.lock(),
        _cropFactor.lock(),
        _focalLength.lock(),
        _aperture.lock(),
        _distance.lock(),
        _targetGeometry.lock(),
        _autoScale.lock(),
        _scale.lock(),
        _tcaOverride.lock(),
        _tcaRed.lock(),
        _tcaBlue.lock(),
        _filter.lock(),
        _clamp.lock(),
        _blackOutside.lock(),
    };
    for (const KnobIPtr& knob : lensfunKnobs) {
        setSecretAndDisabled(knob, !lensfun);
    }
    if (!lensfun) {
        return;
    }
    KnobBoolPtr autoScale = _autoScale.lock();
    setEnabled(_scale.lock(), !autoScale || !autoScale->getValue());
    KnobBoolPtr tcaOverride = _tcaOverride.lock();
    const bool tca = tcaOverride && tcaOverride->getValue();
    setEnabled(_tcaRed.lock(), tca);
    setEnabled(_tcaBlue.lock(), tca);
}

void
LensCorrect::onKnobsLoaded()
{
    updateVisibility();
    refreshInfo();
}

void
LensCorrect::onInputChanged(int inputNo)
{
    StagedImageEffect::onInputChanged(inputNo);
    refreshInfo();
}

void
LensCorrect::onCurrentFrameMetadataChanged(const NodeMetadata& metadata)
{
    StagedImageEffect::onCurrentFrameMetadataChanged(metadata);
    refreshInfo();
}

bool
LensCorrect::knobChanged(KnobI* k,
                         ValueChangedReasonEnum /*reason*/,
                         ViewSpec /*view*/,
                         double /*time*/,
                         bool /*originatedFromMainThread*/)
{
    KnobStringPtr info = _info.lock();
    if (!k || (info && (k == info.get()))) {
        return false;
    }
    const KnobIPtr visibility[] = { _method.lock(), _autoScale.lock(), _tcaOverride.lock() };
    for (const KnobIPtr& knob : visibility) {
        if (knob && (k == knob.get())) {
            updateVisibility();
        }
    }
    const KnobIPtr lookupKnobs[] = {
        _method.lock(),
        _mode.lock(),
        _correctDistortion.lock(),
        _correctTCA.lock(),
        _correctVignetting.lock(),
        _camera.lock(),
        _lens.lock(),
        _cropFactor.lock(),
        _focalLength.lock(),
        _aperture.lock(),
        _distance.lock(),
    };
    for (const KnobIPtr& knob : lookupKnobs) {
        if (knob && (k == knob.get())) {
            refreshInfo();

            return true;
        }
    }

    return false;
}

LensCorrect::Lookup
LensCorrect::lookup(double time,
                    ViewIdx view)
{
    Lookup result;
    const ImageMetadata metadata = getSourceMetadata(time, view);

    KnobStringPtr cameraKnob = _camera.lock();
    KnobStringPtr lensKnob = _lens.lock();
    const std::string cameraTyped = cameraKnob ? trimmed(cameraKnob->getValueAtTime(time, 0, view)) : std::string();
    const std::string lensTyped = lensKnob ? trimmed(lensKnob->getValueAtTime(time, 0, view)) : std::string();

    // A typed camera is searched without a maker, as darktable does; the metadata's with its own.
    std::string maker;
    std::string model = cameraTyped;
    if (model.empty()) {
        maker = trimmed(metadataString(metadata, kExifMetadataMake));
        model = trimmed(metadataString(metadata, kExifMetadataModel));
        if (maker.empty()) {
            maker = trimmed(RawMetadata::getMake(metadata).value_or(std::string()));
        }
        if (model.empty()) {
            model = trimmed(RawMetadata::getModel(metadata).value_or(std::string()));
        }
    }
    const std::string lensName = lensTyped.empty() ? sanitizeLens(trimmed(metadataString(metadata, kExifMetadataLensModel))) : lensTyped;
    result.cameraQuery = maker.empty() ? model : (maker + " " + model);
    result.lensQuery = lensName;
    result.monochrome = RawMetadata::getMonochrome(metadata);

    Found found;
    {
        const std::string key = maker + '\n' + model + '\n' + lensName;
        std::lock_guard<std::mutex> lock(_foundMutex);
        if (_hasFound && (_foundKey == key)) {
            found = _found;
        } else {
            std::string error;
            const lfDatabase* db = LensfunRuntime::database(&error);
            if (!db) {
                found.error = tr("The lensfun database could not be loaded: %1").arg(QString::fromUtf8(error.c_str())).toStdString();
            } else {
                std::lock_guard<std::mutex> queryLock(LensfunRuntime::queryMutex());
                const lfCamera* camera = nullptr;
                const lfCamera** cameras = nullptr;
                if (!model.empty() || !maker.empty()) {
                    cameras = db->FindCamerasExt(maker.empty() ? nullptr : maker.c_str(), model.empty() ? nullptr : model.c_str(), 0);
                    if (cameras) {
                        camera = cameras[0];
                        found.cameraFound = mlstr(camera->Maker) + " " + mlstr(camera->Model);
                        found.cameraCrop = camera->CropFactor;
                    }
                }
                std::string searched = lensName;
                const lfLens** lenses = nullptr;
                if (!searched.empty() || camera) {
                    lenses = db->FindLenses(camera, nullptr, searched.c_str(), 0);
                }
                // A fixed-lens camera (lower-case mount): the lens name may be absent or not the
                // database's, so search the camera's lenses instead.
                const bool fixedLens = camera && camera->Mount && std::islower((unsigned char)camera->Mount[0]);
                if (!lenses && fixedLens) {
                    searched.clear();
                    lenses = db->FindLenses(camera, nullptr, "", 0);
                }
                if (lenses) {
                    int index = 0;
                    if (searched.empty() && fixedLens) {
                        // darktable's choice: the shortest model name.
                        std::size_t shortest = SIZE_MAX;
                        for (int i = 0; lenses[i]; ++i) {
                            const std::size_t length = mlstr(lenses[i]->Model).size();
                            if (length < shortest) {
                                shortest = length;
                                index = i;
                            }
                        }
                    }
                    if (!searched.empty() || fixedLens) {
                        found.lens = std::make_shared<const lfLens>(*lenses[index]);
                        found.lensFound = mlstr(lenses[index]->Maker) + " " + mlstr(lenses[index]->Model);
                    }
                    lf_free(lenses);
                }
                if (cameras) {
                    lf_free(cameras);
                }
            }
            _foundKey = key;
            _found = found;
            _hasFound = true;
        }
    }
    result.cameraFound = found.cameraFound;
    result.lensFound = found.lensFound;
    result.lens = found.lens;

    KnobDoublePtr cropKnob = _cropFactor.lock();
    KnobDoublePtr focalKnob = _focalLength.lock();
    KnobDoublePtr apertureKnob = _aperture.lock();
    KnobDoublePtr distanceKnob = _distance.lock();
    const double cropTyped = cropKnob ? cropKnob->getValueAtTime(time, 0, view) : 0.;
    const double focalTyped = focalKnob ? focalKnob->getValueAtTime(time, 0, view) : 0.;
    const double apertureTyped = apertureKnob ? apertureKnob->getValueAtTime(time, 0, view) : 0.;
    const double distanceTyped = distanceKnob ? distanceKnob->getValueAtTime(time, 0, view) : 0.;

    if (cropTyped > 0.) {
        result.crop = (float)cropTyped;
        result.cropFrom = "knob";
    } else if (found.cameraCrop > 0.f) {
        result.crop = found.cameraCrop;
        result.cropFrom = "camera";
    }
    const double focalMetadata = metadataNumber(metadata, kExifMetadataFocalLength);
    if (focalTyped > 0.) {
        result.focal = (float)focalTyped;
        result.focalFrom = "knob";
    } else if (focalMetadata > 0.) {
        result.focal = (float)focalMetadata;
        result.focalFrom = "metadata";
    } else if (result.lens && (result.lens->MinFocal > 0.f)) {
        result.focal = result.lens->MinFocal;
        result.focalFrom = "lens";
    }
    const double apertureMetadata = metadataNumber(metadata, kExifMetadataFNumber);
    if (apertureTyped > 0.) {
        result.aperture = (float)apertureTyped;
        result.apertureFrom = "knob";
    } else if (apertureMetadata > 0.) {
        result.aperture = (float)apertureMetadata;
        result.apertureFrom = "metadata";
    } else if (result.lens && (result.lens->MinAperture > 0.f)) {
        result.aperture = result.lens->MinAperture;
        result.apertureFrom = "lens";
    }
    const double distanceMetadata = metadataNumber(metadata, kExifMetadataSubjectDistance);
    if (distanceTyped > 0.) {
        result.distance = (float)distanceTyped;
        result.distanceFrom = "knob";
    } else if (distanceMetadata > 0.) {
        result.distance = (float)distanceMetadata;
        result.distanceFrom = "metadata";
    } else {
        result.distance = 1000.f;
        result.distanceFrom = "default";
    }

    if (!found.error.empty()) {
        result.warning = found.error;
    } else if (!result.lens) {
        if (lensName.empty() && result.cameraQuery.empty()) {
            result.warning = tr("No camera or lens to correct: the Source has no lens metadata. Type the Lens and Camera.").toStdString();
        } else if (lensName.empty()) {
            result.warning = tr("No lens to correct: the Source has no exif/LensModel metadata and camera \"%1\" has no fixed lens "
                                "in the lensfun database. Type the Lens.")
                                 .arg(QString::fromUtf8(result.cameraQuery.c_str()))
                                 .toStdString();
        } else {
            result.warning = tr("Lens \"%1\" (camera \"%2\") is not in the lensfun database; the image passes through unchanged.")
                                 .arg(QString::fromUtf8(lensName.c_str()))
                                 .arg(QString::fromUtf8(result.cameraQuery.c_str()))
                                 .toStdString();
        }
    } else if (!(result.crop > 0.f)) {
        result.warning = tr("Camera \"%1\" is not in the lensfun database, so its crop factor is unknown; the image passes through "
                            "unchanged. Type the Camera or the Crop Factor.")
                             .arg(QString::fromUtf8(result.cameraQuery.c_str()))
                             .toStdString();
    } else if (!(result.focal > 0.f)) {
        result.warning = tr("The focal length of lens \"%1\" is unknown; the image passes through unchanged. Type the Focal Length.")
                             .arg(QString::fromUtf8(result.lensFound.c_str()))
                             .toStdString();
    }

    return result;
} // LensCorrect::lookup

std::string
LensCorrect::infoText(const Lookup& lookup)
{
    std::ostringstream ss;

    ss << "Camera: " << (lookup.cameraFound.empty() ? std::string("not found") : lookup.cameraFound);
    if (!lookup.cameraQuery.empty()) {
        ss << " (searched \"" << lookup.cameraQuery << "\")";
    }
    ss << "\nLens: " << (lookup.lensFound.empty() ? std::string("not found") : lookup.lensFound);
    if (!lookup.lensQuery.empty()) {
        ss << " (searched \"" << lookup.lensQuery << "\")";
    }
    const auto value = [&ss](const char* name, float v, const char* unit, const std::string& from) {
        ss << name;
        if (v > 0.f) {
            ss << formatNumber(v) << unit << " (" << from << ")";
        } else {
            ss << "unknown";
        }
    };
    value("\nCrop factor ", lookup.crop, "", lookup.cropFrom);
    value(", focal length ", lookup.focal, " mm", lookup.focalFrom);
    value(", aperture f/", lookup.aperture, "", lookup.apertureFrom);
    value(", distance ", lookup.distance, " m", lookup.distanceFrom);
    if (lookup.monochrome) {
        ss << "\nMonochrome: TCA is not corrected.";
    }

    return ss.str();
}

void
LensCorrect::refreshInfo()
{
    KnobStringPtr info = _info.lock();
    if (!info) {
        return;
    }
    KnobChoicePtr method = _method.lock();
    if (method && (method->getValue() != 0)) {
        info->setValue(std::string());
        postWarning(std::string());

        return;
    }
    if (!getInput(kLensCorrectInputSource)) {
        info->setValue(std::string());

        return;
    }
    const Lookup found = lookup(getCurrentTime(), ViewIdx(0));
    info->setValue(infoText(found));
    postWarning(found.warning);
}

void
LensCorrect::postWarning(const std::string& warning)
{
    const NodePtr node = getNode();

    if (!node) {
        return;
    }
    std::string previous;
    {
        std::lock_guard<std::mutex> lock(_warningMutex);
        if (_postedWarning == warning) {
            return;
        }
        previous = _postedWarning;
        _postedWarning = warning;
    }
    if (!warning.empty()) {
        setPersistentMessage(eMessageTypeWarning, warning);

        return;
    }
    // Only our own warning is taken back: anything posted since has replaced it.
    QString current;
    int type = 0;
    node->getPersistentMessage(&current, &type, false);
    if (current == QString::fromUtf8(previous.c_str())) {
        node->clearPersistentMessage(false);
    }
}

void
LensCorrect::readSettings(double time,
                          ViewIdx view,
                          Settings* s) const
{
    if (KnobChoicePtr k = _method.lock()) {
        s->lensfun = (k->getValueAtTime(time, 0, view) == 0);
    }
    if (KnobChoicePtr k = _mode.lock()) {
        s->reverse = (k->getValueAtTime(time, 0, view) == 1);
    }
    if (KnobBoolPtr k = _correctDistortion.lock()) {
        s->distortion = k->getValueAtTime(time, 0, view);
    }
    if (KnobBoolPtr k = _correctTCA.lock()) {
        s->tca = k->getValueAtTime(time, 0, view);
    }
    if (KnobBoolPtr k = _correctVignetting.lock()) {
        s->vignetting = k->getValueAtTime(time, 0, view);
    }
    if (KnobChoicePtr k = _targetGeometry.lock()) {
        s->targetGeometry = k->getValueAtTime(time, 0, view);
    }
    if (KnobBoolPtr k = _autoScale.lock()) {
        s->autoScale = k->getValueAtTime(time, 0, view);
    }
    if (KnobDoublePtr k = _scale.lock()) {
        s->scale = k->getValueAtTime(time, 0, view);
    }
    if (KnobBoolPtr k = _tcaOverride.lock()) {
        s->tcaOverride = k->getValueAtTime(time, 0, view);
    }
    if (KnobDoublePtr k = _tcaRed.lock()) {
        s->tcaRed = k->getValueAtTime(time, 0, view);
    }
    if (KnobDoublePtr k = _tcaBlue.lock()) {
        s->tcaBlue = k->getValueAtTime(time, 0, view);
    }
    if (KnobDoublePtr k = _vignetteStrength.lock()) {
        s->vignetteStrength = k->getValueAtTime(time, 0, view);
    }
    if (KnobDoublePtr k = _vignetteRadius.lock()) {
        s->vignetteRadius = k->getValueAtTime(time, 0, view);
    }
    if (KnobDoublePtr k = _vignetteSteepness.lock()) {
        s->vignetteSteepness = k->getValueAtTime(time, 0, view);
    }
    if (KnobBoolPtr k = _showVignetteMask.lock()) {
        s->showVignetteMask = k->getValueAtTime(time, 0, view);
    }
    if (KnobChoicePtr k = _filter.lock()) {
        s->filter = (Resampler::FilterEnum)k->getValueAtTime(time, 0, view);
    }
    if (KnobBoolPtr k = _clamp.lock()) {
        s->clamp = k->getValueAtTime(time, 0, view);
    }
    if (KnobBoolPtr k = _blackOutside.lock()) {
        s->blackOutside = k->getValueAtTime(time, 0, view);
    }
} // LensCorrect::readSettings

LensCorrect::Model
LensCorrect::buildModel(double time,
                        ViewIdx view,
                        const Settings& settings)
{
    Model model;
    EffectInstancePtr source = getInput(kLensCorrectInputSource);

    if (!source) {
        return model;
    }
    model.vignetteStrength = (float)settings.vignetteStrength;
    model.vignetteRadius = (float)settings.vignetteRadius;
    model.vignetteSteepness = (float)settings.vignetteSteepness;
    model.showVignetteMask = settings.showVignetteMask;

    NodeMetadataConstPtr sourceMetadata = source->getRenderMetadata(time, view);
    if (sourceMetadata) {
        model.format = sourceMetadata->getOutputFormat();
    }
    if (!settings.lensfun) {
        postWarning(std::string());

        return model;
    }
    const Lookup found = lookup(time, view);
    postWarning(found.warning);
    if (!found.warning.empty() || !found.lens) {
        return model;
    }
    LensCorrectProfile::Request request;
    // The size only scales the model; route() asks before the format is known.
    request.width = model.format.isNull() ? 3000 : model.format.width();
    request.height = model.format.isNull() ? 2000 : model.format.height();
    request.focal = found.focal;
    request.crop = found.crop;
    request.aperture = found.aperture;
    request.distance = found.distance;
    request.reverse = settings.reverse;
    request.distortion = settings.distortion;
    request.tca = settings.tca && !found.monochrome;
    request.vignetting = settings.vignetting;
    request.targetGeometry = settings.targetGeometry;
    request.scale = settings.autoScale ? 0.f : (float)settings.scale;
    request.tcaOverride = settings.tcaOverride;
    request.tcaRed = (float)settings.tcaRed;
    request.tcaBlue = (float)settings.tcaBlue;
    model.profile = LensCorrectProfile::build(*found.lens, request);

    return model;
} // LensCorrect::buildModel

Route
LensCorrect::route(double time,
                   ViewIdx view)
{
    if (!getInput(kLensCorrectInputSource)) {
        return Route::black();
    }
    Settings settings;
    readSettings(time, view, &settings);
    if (!buildModel(time, view, settings).active()) {
        return Route::input(kLensCorrectInputSource, time);
    }

    return Route::none();
}

void
LensCorrect::getRegionsOfInterest(double time,
                                  const RenderScale& scale,
                                  const RectD& outputRoD,
                                  const RectD& renderWindow,
                                  ViewIdx view,
                                  RoIMap* ret)
{
    StagedImageEffect::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    EffectInstancePtr source = getInput(kLensCorrectInputSource);
    if (!source) {
        return;
    }
    // Lensfun's map has no bound short of the whole image, so the whole Source is read.
    RectD sourceRoI = renderWindow;
    RectD sourceRoD;
    bool isProjectFormat = false;
    const RenderScale inputScale = source->supportsRenderScale() ? scale : RenderScale::identity;
    if ((source->getRegionOfDefinition_public(source->getRenderHash(), time, inputScale, view, &sourceRoD, &isProjectFormat) != eStatusFailed) && !sourceRoD.isNull()) {
        sourceRoI.merge(sourceRoD);
    }
    (*ret)[source] = sourceRoI;
}

bool
LensCorrect::isOpIdentity(double /*time*/,
                          const RenderScale& /*scale*/,
                          const RectI& /*roi*/,
                          ViewIdx /*view*/)
{
    return false;
}

namespace {
// Maps output pixels at the render scale to Source pixels at the render scale through the profile.
// Lensfun's pixels have their centres on integers and y down, over the Source format at scale 1.
struct PixelMapper {
    const LensCorrectProfile::Profile* profile;
    double scaleX;
    double scaleY;
    double x1;
    double y2;

    // Lensfun coordinates of output pixel (px, py)'s centre.
    void lensfunAt(int px,
                   int py,
                   float* xu,
                   float* yu) const
    {
        *xu = (float)((px + 0.5) / scaleX - x1 - 0.5);
        *yu = (float)(y2 - 0.5 - (py + 0.5) / scaleY);
    }

    // Per channel (red, green, blue) the lensfun source position (lf) and the Source pixel
    // coordinates at the render scale (s), both as x, y pairs.
    void map(int px,
             int py,
             float lf[6],
             float s[6]) const
    {
        float xu, yu;
        lensfunAt(px, py, &xu, &yu);
        profile->map(xu, yu, lf);
        for (int c = 0; c < 3; ++c) {
            s[2 * c] = (float)((lf[2 * c] + x1 + 0.5) * scaleX);
            s[2 * c + 1] = (float)((y2 - 0.5 - lf[2 * c + 1]) * scaleY);
        }
    }
};
} // namespace

StatusEnum
LensCorrect::renderOp(const RenderActionArgs& args,
                      const StagePlan& plan)
{
    Settings settings;
    readSettings(args.time, args.view, &settings);
    if (args.draftMode) {
        settings.filter = Resampler::eFilterImpulse;
    }
    const bool jacobians = (settings.filter != Resampler::eFilterImpulse);
    // Built here, on the calling thread; the band threads only read it.
    const Model model = buildModel(args.time, args.view, settings);
    const LensCorrectProfile::Profile& profile = model.profile;
    const bool moves = profile.movesPixels();
    const bool tca = (profile.tca != LensCorrectProfile::TcaOp::None);

    PixelMapper mapper;
    mapper.profile = &profile;
    mapper.scaleX = args.mappedScale.toOfxPointD().x;
    mapper.scaleY = args.mappedScale.toOfxPointD().y;
    mapper.x1 = model.format.x1;
    mapper.y2 = model.format.y2;
    // darktable's manual vignette radius: from the image centre, 1 at the corners.
    const float w2 = 0.5f * (float)model.format.width();
    const float h2 = 0.5f * (float)model.format.height();
    const float invMaxR = (w2 > 0.f && h2 > 0.f) ? 1.f / std::hypot(w2, h2) : 0.f;

    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        if (!it->second) {
            continue;
        }
        const int nComps = (int)it->second->getComponentsCount();

        ImagePtr src;
        ImageLayerDesc sourceLayer;
        if (!model.format.isNull() && resolveInputPlaneForRender(kLensCorrectInputSource, args.time, args.view, &sourceLayer, NULL)) {
            RectI sourceRoI;
            src = getImage(kLensCorrectInputSource, args.time, args.mappedScale, args.view, NULL, &sourceLayer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &sourceRoI);
        }
        if (src && !PassExec::isFloatImage(src)) {
            return eStatusFailed;
        }

        RectI srcBounds;
        std::shared_ptr<Image::ReadAccess> srcAccess;
        std::vector<float> converted;
        Resampler::SourceImage source((const float*)0, RectI(), nComps);
        if (src) {
            srcBounds = src->getBounds();
            const int srcNComps = (int)src->getComponentsCount();
            srcAccess = std::make_shared<Image::ReadAccess>(src.get());
            if (srcBounds.isNull()) {
                src.reset();
            } else if (srcNComps == nComps) {
                source = Resampler::SourceImage((const float*)srcAccess->pixelAt(srcBounds.x1, srcBounds.y1), srcBounds, nComps);
            } else {
                const int width = srcBounds.width();
                converted.resize((std::size_t)width * srcBounds.height() * nComps);
                for (int y = srcBounds.y1; y < srcBounds.y2; ++y) {
                    PassExec::readSourceRow(src.get(), srcAccess.get(), srcBounds, srcNComps, srcBounds.x1, y, width, nComps, &converted[(std::size_t)(y - srcBounds.y1) * width * nComps]);
                }
                source = Resampler::SourceImage(&converted[0], srcBounds, nComps);
            }
        }
        const bool sampled = (bool)src;
        // Lensfun leaves alpha alone, and a one- or two-channel image has no colour to scale.
        const int colourChannels = (nComps >= 3) ? 3 : 0;
        const int alphaChannel = (nComps == 4) ? 3 : ((nComps == 1) ? 0 : -1);

        const auto row = [&](const RowIO& io) {
            const int n = io.width;
            std::vector<float> out((std::size_t)n * io.nComps, 0.f);
            if (sampled) {
                // Rows y - 1, y and y + 1, one pixel wider on each side, for the Jacobians; only
                // row y needs the red and blue positions.
                const int wide = n + 2;
                std::vector<float> lf((std::size_t)6 * wide);
                std::vector<float> sxc[3], syc[3];
                for (int c = 0; c < 3; ++c) {
                    sxc[c].resize(wide);
                    syc[c].resize(wide);
                }
                for (int i = 0; i < wide; ++i) {
                    float s[6];
                    mapper.map(io.x0 - 1 + i, io.y, &lf[(std::size_t)6 * i], s);
                    for (int c = 0; c < 3; ++c) {
                        sxc[c][i] = s[2 * c];
                        syc[c][i] = s[2 * c + 1];
                    }
                }
                std::vector<float> jacobian;
                if (jacobians && moves) {
                    std::vector<float> sxPrev(wide), syPrev(wide), sxNext(wide), syNext(wide);
                    for (int i = 0; i < wide; ++i) {
                        float l[6], s[6];
                        mapper.map(io.x0 - 1 + i, io.y - 1, l, s);
                        sxPrev[i] = s[2];
                        syPrev[i] = s[3];
                        mapper.map(io.x0 - 1 + i, io.y + 1, l, s);
                        sxNext[i] = s[2];
                        syNext[i] = s[3];
                    }
                    std::vector<float> wideJacobian((std::size_t)4 * wide);
                    Resampler::coordJacobians(&sxPrev[0], &sxc[1][0], &sxNext[0], &syPrev[0], &syc[1][0], &syNext[0], wide, &wideJacobian[0]);
                    jacobian.assign(wideJacobian.begin() + 4, wideJacobian.end() - 4);
                }
                const float* J = jacobian.empty() ? 0 : &jacobian[0];
                // Green's positions carry alpha and, without TCA, every channel.
                std::vector<float> green((std::size_t)n * io.nComps);
                Resampler::resampleRowFromCoords(source, &sxc[1][1], &syc[1][1], J, n, settings.filter, settings.clamp, settings.blackOutside,
                                                 Resampler::eWrapNone, Resampler::eWrapNone, &green[0], io.nComps);
                out = green;
                if (tca && colourChannels) {
                    std::vector<float> other((std::size_t)n * io.nComps);
                    for (int c = 0; c < 3; c += 2) {
                        Resampler::resampleRowFromCoords(source, &sxc[c][1], &syc[c][1], J, n, settings.filter, settings.clamp, settings.blackOutside,
                                                         Resampler::eWrapNone, Resampler::eWrapNone, &other[0], io.nComps);
                        for (int i = 0; i < n; ++i) {
                            out[(std::size_t)i * io.nComps + c] = other[(std::size_t)i * io.nComps + c];
                        }
                    }
                }
                // The gains: lensfun's vignetting at the Source position when correcting (darktable
                // applies it to the input) and at the output pixel when distorting (to the output);
                // the manual vignette at the Source position, as darktable applies it to the input.
                const bool lensfunGain = profile.hasVignetting();
                const bool manual = model.manualVignette();
                if (lensfunGain || manual) {
                    for (int i = 0; i < n; ++i) {
                        float* pixel = &out[(std::size_t)i * io.nComps];
                        const float* l = &lf[(std::size_t)6 * (i + 1)];
                        float xu = 0.f, yu = 0.f;
                        mapper.lensfunAt(io.x0 + i, io.y, &xu, &yu);
                        for (int c = 0; c < colourChannels; ++c) {
                            const float sx = l[2 * c];
                            const float sy = l[2 * c + 1];
                            if (!std::isfinite(sx) || !std::isfinite(sy)) {
                                pixel[c] = 0.f;
                                continue;
                            }
                            float gain = 1.f;
                            if (lensfunGain) {
                                gain *= settings.reverse ? profile.vignettingGain(xu, yu) : profile.vignettingGain(sx, sy);
                            }
                            if (manual) {
                                gain *= 1.f + manualVignette(model.vignetteStrength, model.vignetteRadius, model.vignetteSteepness, std::hypot(sx - w2, sy - h2) * invMaxR);
                            }
                            pixel[c] *= gain;
                        }
                        if (model.showVignetteMask && (alphaChannel >= 0)) {
                            const float sx = l[2];
                            const float sy = l[3];
                            pixel[alphaChannel] = (std::isfinite(sx) && std::isfinite(sy)) ? manualVignette(model.vignetteStrength, model.vignetteRadius, model.vignetteSteepness,
                                                                                                            std::hypot(sx - w2, sy - h2) * invMaxR)
                                                                                           : 0.f;
                        }
                    }
                }
            }
            for (int c = 0; c < io.nComps; ++c) {
                if (!io.channels[pixelKernelChannelBit(io.nComps, c)]) {
                    continue;
                }
                for (int i = 0; i < n; ++i) {
                    io.dst[(std::size_t)i * io.nComps + c] = out[(std::size_t)i * io.nComps + c];
                }
            }
        };

        EffectInstance::RenderActionArgs planeArgs = args;
        planeArgs.outputLayers.clear();
        planeArgs.outputLayers.push_back(*it);
        const StatusEnum status = runStagesCpu(*this, plan, planeArgs, StageRowFn(row));
        if (status != eStatusOK) {
            return status;
        }
    }

    return eStatusOK;
} // LensCorrect::renderOp

namespace {
// The parameter block of Engine/Gpu/Kernels/lenscorrect.slang, word for word.
struct LensCorrectGpuParams {
    uint32_t filter = 0;
    uint32_t flags = 0;
    uint32_t nCoordSteps = 0;
    uint32_t tca = 0;
    float scaleX = 1.f;
    float scaleY = 1.f;
    float formatX1 = 0.f;
    float formatY2 = 0.f;
    float normScale = 1.f;
    float normUnScale = 1.f;
    float centerX = 0.f;
    float centerY = 0.f;
    uint32_t vignetting = 0;
    float vignetteStrength = 0.f;
    float vignetteRadius = 0.5f;
    float vignetteSteepness = 0.5f;
    float w2 = 0.f;
    float h2 = 0.f;
    float invMaxR = 0.f;
    uint32_t pad0 = 0;
    float tcaTerms[12] = {};
    float vignettingTerms[4] = {};
    // Per coordinate step: the op, then its five terms.
    uint32_t coord[LensCorrectProfile::kMaxCoordSteps * 6] = {};
};

static_assert(offsetof(LensCorrectGpuParams, tcaTerms) == 20 * 4, "lenscorrect.slang reads the TCA terms from word 20");
static_assert(offsetof(LensCorrectGpuParams, vignettingTerms) == 32 * 4, "lenscorrect.slang reads the vignetting terms from word 32");
static_assert(offsetof(LensCorrectGpuParams, coord) == 36 * 4, "lenscorrect.slang reads the steps from word 36");
static_assert(sizeof(LensCorrectGpuParams) == 60 * 4, "lenscorrect.slang reads 60 words");

const uint32_t kLensCorrectGpuClamp = 1;
const uint32_t kLensCorrectGpuBlackOutside = 2;
const uint32_t kLensCorrectGpuJacobian = 4;
const uint32_t kLensCorrectGpuSample = 8;
const uint32_t kLensCorrectGpuReverse = 16;
const uint32_t kLensCorrectGpuManualVignette = 32;
const uint32_t kLensCorrectGpuVignetteMask = 64;

uint32_t
floatBits(float f)
{
    uint32_t u;

    std::memcpy(&u, &f, sizeof(u));

    return u;
}
} // namespace

bool
LensCorrect::canRenderOpOnGpu(const GpuQuery& query) const
{
#ifdef NATRON_HAVE_GPU
    return (query.bitDepth == eImageBitDepthFloat) && canRunStagesGpu(resolveGpuPlan(query), query);
#else
    Q_UNUSED(query);

    return false;
#endif
}

bool
LensCorrect::getOpGpuInputLayer(int inputNb,
                                const ImageLayerDesc& plane,
                                double time,
                                ViewIdx view,
                                ImageLayerDesc* layer) const
{
    stageGpuInputLayer(*this, inputNb, plane, time, view, layer);

    return true;
}

RectI
LensCorrect::getOpGpuInputRoI(int inputNb,
                              double time,
                              ViewIdx view,
                              const RectI& outputRect,
                              const RenderScale& scale) const
{
    if (inputNb == kLensCorrectInputSource) {
        return warpGpuInputRoI(*this, inputNb, kLensCorrectInputSource, -1, time, view, outputRect, scale);
    }

    return StagedImageEffect::getOpGpuInputRoI(inputNb, time, view, outputRect, scale);
}

StatusEnum
LensCorrect::renderOpGpu(const GpuRenderArgs& args)
{
#ifdef NATRON_HAVE_GPU
    const GpuQuery& query = args.query;
    if (!canRenderOpOnGpu(query)) {
        return eStatusFailed;
    }
    Settings settings;
    readSettings(query.time, query.view, &settings);
    if (query.draftMode) {
        settings.filter = Resampler::eFilterImpulse;
    }
    const Model model = buildModel(query.time, query.view, settings);
    const LensCorrectProfile::Profile& profile = model.profile;

    LensCorrectGpuParams params;
    params.filter = (uint32_t)settings.filter;
    params.flags = (settings.clamp ? kLensCorrectGpuClamp : 0u) | (settings.blackOutside ? kLensCorrectGpuBlackOutside : 0u) | ((settings.filter != Resampler::eFilterImpulse && profile.movesPixels()) ? kLensCorrectGpuJacobian : 0u) | (settings.reverse ? kLensCorrectGpuReverse : 0u) | ((model.vignetteStrength > 0.f || model.showVignetteMask) ? kLensCorrectGpuManualVignette : 0u) | (model.showVignetteMask ? kLensCorrectGpuVignetteMask : 0u);
    if (!model.format.isNull()) {
        params.flags |= kLensCorrectGpuSample;
    }
    params.nCoordSteps = (uint32_t)profile.nCoordSteps;
    params.tca = (uint32_t)profile.tca;
    params.scaleX = (float)query.mappedScale.toOfxPointD().x;
    params.scaleY = (float)query.mappedScale.toOfxPointD().y;
    params.formatX1 = (float)model.format.x1;
    params.formatY2 = (float)model.format.y2;
    params.normScale = (float)profile.normScale;
    params.normUnScale = (float)profile.normUnScale;
    params.centerX = (float)profile.centerX;
    params.centerY = (float)profile.centerY;
    params.vignetting = (uint32_t)profile.vignetting;
    params.vignetteStrength = model.vignetteStrength;
    params.vignetteRadius = model.vignetteRadius;
    params.vignetteSteepness = model.vignetteSteepness;
    params.w2 = 0.5f * (float)model.format.width();
    params.h2 = 0.5f * (float)model.format.height();
    params.invMaxR = (params.w2 > 0.f && params.h2 > 0.f) ? 1.f / std::hypot(params.w2, params.h2) : 0.f;
    std::memcpy(params.tcaTerms, profile.tcaTerms, sizeof(params.tcaTerms));
    std::memcpy(params.vignettingTerms, profile.vignettingTerms, sizeof(profile.vignettingTerms));
    for (int i = 0; i < profile.nCoordSteps; ++i) {
        params.coord[6 * i] = (uint32_t)profile.coord[i].op;
        for (int t = 0; t < 5; ++t) {
            params.coord[6 * i + 1 + t] = floatBits(profile.coord[i].terms[t]);
        }
    }

    // The Source cropped to its region of definition, as the CPU fetches it, is both sampled and
    // blended under the mask and mix.
    const StagePlan plan = resolveGpuPlan(query);
    GpuRenderArgs cropped = args;
    const int sourceInput = plan.sourceInput;
    const bool sourceStaged = (sourceInput >= 0) && (sourceInput < (int)cropped.inputs.size());
    if (sourceStaged && !stageGpuInputView(*this, args, sourceInput, &cropped.inputs[sourceInput])) {
        return eStatusFailed;
    }
    if (!sourceStaged || !cropped.inputs[sourceInput].buffer) {
        params.flags &= ~kLensCorrectGpuSample;
    }

    PixelKernelGpu op;
    op.kernel = (int)gpu::GpuKernelId::lenscorrect;
    op.setParams(params);

    return recordStagesGpuKernelInputs(plan, cropped, op, std::span<const GpuImageView>());
#else
    Q_UNUSED(args);

    return eStatusFailed;
#endif
} // LensCorrect::renderOpGpu

NATRON_NAMESPACE_EXIT
