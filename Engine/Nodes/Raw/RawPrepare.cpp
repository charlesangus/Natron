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

/*
 * The maths are a port of darktable's rawprepare module, DARKTABLE_REF release-5.6.2 (commit
 * 20891f6c6fa7), src/iop/rawprepare.c, Copyright (C) 2015-2025 darktable developers, based on
 * code by johannes hanika, distributed under the GNU GPL version 3 or later.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "RawPrepare.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <vector>

#include <QCoreApplication>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Nodes/Raw/RawGpuParams.h"
#include "Engine/RectD.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
inline constexpr InputSpec kRawPrepareInputs[] = {
    InputSpec { .label = "Source", .kind = eDataKindRaw },
};

constexpr NodeSpec kRawPrepareNodeSpec = {
    .id = PLUGINID_NATRON_RAWPREPARE,
    .label = "RawPrepare",
    .description = "Subtracts the sensor's black levels and scales by its white level, so that black is 0 and the white level 1, "
                   "and crops the raw data to the sensor's active area. The levels and the crop come from the raw file unless "
                   "overridden here. The first node below a Read of a raw file; RawTemperature, RawHighlights and RawDemosaic "
                   "need its output.",
    .grouping = PLUGIN_GROUP_RAW,
    .majorVersion = PLUGIN_MAJOR_NATRON_RAWPREPARE,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/" PLUGINID_NATRON_RAWPREPARE ".png",
    .outputKind = eDataKindRaw,
    .inputs = kRawPrepareInputs,
};

constexpr ImagePipelineSpec kRawPreparePipeline = {
    .layers = LayerPolicy::none(),
    .op = OpKind::Custom,
};

// The cell of the 2x2 block at the CFA origin that photosite (x, y) is on.
inline int
blackCell(const RawLayout& layout,
          int x,
          int y)
{
    const int col = (x - layout.originX()) & 1;
    const int row = (y - layout.originY()) & 1;

    return row * 2 + col;
}

std::string
flatFieldHint(bool available)
{
    if (available) {
        return QCoreApplication::translate("RawPrepare", "Multiplies each photosite by the gain the raw file's embedded GainMaps give it, correcting the uneven colour cast and shading of the lens and sensor. On by default when the file has them.").toStdString();
    }

    return QCoreApplication::translate("RawPrepare", "The raw file has no embedded GainMaps that can be applied, so this does nothing. Applying needs four single-plane Bayer maps over the whole sensor that share one grid.").toStdString();
}

// The cell of the 2x2 block at the sensor's top-left corner that sensor pixel (col, row), counted
// from the top-left, is on.
inline int
sensorCell(int col,
           int row)
{
    return ((row & 1) << 1) + (col & 1);
}
} // namespace

std::shared_ptr<const RawPrepare::FlatField>
RawPrepare::acceptGainMaps(const std::vector<DngGainMap>& maps,
                           int width,
                           int height)
{
    if (maps.size() != 4) {
        return nullptr;
    }
    std::array<const DngGainMap*, 4> byCell = { nullptr, nullptr, nullptr, nullptr };
    for (const DngGainMap& g : maps) {
        if ((g.plane != 0) || (g.planes != 1) || (g.mapPlanes != 1) || (g.rowPitch != 2) || (g.colPitch != 2) || (g.mapPointsV < 2) || (g.mapPointsH < 2) || (g.top > 1) || (g.left > 1) || ((int64_t)g.bottom != height) || ((int64_t)g.right != width)) {
            return nullptr;
        }
        if (g.gains.size() < (std::size_t)g.mapPointsV * g.mapPointsH) {
            return nullptr;
        }
        byCell[(std::size_t)(((g.top & 1) << 1) + (g.left & 1))] = &g;
    }
    for (const DngGainMap* g : byCell) {
        if (!g) {
            return nullptr;
        }
    }
    for (int i = 1; i < 4; ++i) {
        const DngGainMap& a = *byCell[(std::size_t)i];
        const DngGainMap& b = *byCell[0];
        if ((a.mapPointsH != b.mapPointsH) || (a.mapPointsV != b.mapPointsV) || (a.mapSpacingH != b.mapSpacingH) || (a.mapSpacingV != b.mapSpacingV) || (a.mapOriginH != b.mapOriginH) || (a.mapOriginV != b.mapOriginV)) {
            return nullptr;
        }
    }
    auto field = std::make_shared<FlatField>();
    field->mapW = (int)byCell[0]->mapPointsH;
    field->mapH = (int)byCell[0]->mapPointsV;
    field->spacingH = (float)byCell[0]->mapSpacingH;
    field->spacingV = (float)byCell[0]->mapSpacingV;
    field->originH = (float)byCell[0]->mapOriginH;
    field->originV = (float)byCell[0]->mapOriginV;
    for (std::size_t i = 0; i < 4; ++i) {
        field->gains[i] = byCell[i]->gains;
    }

    return field;
}

RawPrepare::RawPrepare(NodePtr node)
    : StagedImageEffect(node)
    , _blackOverride()
    , _blackLevels()
    , _whiteOverride()
    , _whiteLevel()
    , _cropOverride()
    , _cropLeft()
    , _cropTop()
    , _cropRight()
    , _cropBottom()
    , _flatField()
    , _message()
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

RawPrepare::~RawPrepare()
{
}

const NodeSpec*
RawPrepare::nodeSpec() const
{
    return staticNodeSpec<kRawPrepareNodeSpec>();
}

const ImagePipelineSpec&
RawPrepare::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kRawPreparePipeline, kRawPrepareNodeSpec>();
}

void
RawPrepare::initializeKnobs()
{
    KnobPagePtr page = createControlsPage();

    KnobBoolPtr blackOverride = createKnob<KnobBool>(tr("Override Black Levels"));
    blackOverride->setName(kRawPrepareParamBlackOverride);
    blackOverride->setHintToolTip(tr("Use the black levels below instead of the ones the raw file gives."));
    blackOverride->setDefaultValue(false);
    blackOverride->setAnimationEnabled(false);
    page->addKnob(blackOverride);
    _blackOverride = blackOverride;

    KnobDoublePtr blackLevels = createKnob<KnobDouble>(tr("Black Levels"), 4);
    blackLevels->setName(kRawPrepareParamBlackLevels);
    blackLevels->setHintToolTip(tr("The black level of each cell of the 2x2 block at the colour filter array's origin, row by row, "
                                   "or of the red, green and blue channels of data without a colour filter array, in the units of the raw data."));
    for (int i = 0; i < 4; ++i) {
        blackLevels->setDefaultValue(0., i);
        blackLevels->setMinimum(0., i);
        blackLevels->setDisplayMinimum(0., i);
        blackLevels->setDisplayMaximum(4096., i);
    }
    page->addKnob(blackLevels);
    _blackLevels = blackLevels;

    KnobBoolPtr whiteOverride = createKnob<KnobBool>(tr("Override White Level"));
    whiteOverride->setName(kRawPrepareParamWhiteOverride);
    whiteOverride->setHintToolTip(tr("Use the white level below instead of the one the raw file gives."));
    whiteOverride->setDefaultValue(false);
    whiteOverride->setAnimationEnabled(false);
    page->addKnob(whiteOverride);
    _whiteOverride = whiteOverride;

    KnobDoublePtr whiteLevel = createKnob<KnobDouble>(tr("White Level"));
    whiteLevel->setName(kRawPrepareParamWhiteLevel);
    whiteLevel->setHintToolTip(tr("The value a saturated photosite has, in the units of the raw data."));
    whiteLevel->setDefaultValue(65535.);
    whiteLevel->setMinimum(0.);
    whiteLevel->setDisplayMinimum(0.);
    whiteLevel->setDisplayMaximum(65535.);
    whiteLevel->setDecimals(0);
    whiteLevel->setIncrement(1.);
    page->addKnob(whiteLevel);
    _whiteLevel = whiteLevel;

    KnobBoolPtr cropOverride = createKnob<KnobBool>(tr("Override Crop"));
    cropOverride->setName(kRawPrepareParamCropOverride);
    cropOverride->setHintToolTip(tr("Crop by the margins below instead of to the active area the raw file gives."));
    cropOverride->setDefaultValue(false);
    cropOverride->setAnimationEnabled(false);
    page->addKnob(cropOverride);
    _cropOverride = cropOverride;

    const struct {
        const char* name;
        QString label;
        QString hint;
        KnobIntWPtr* store;
    } margins[] = {
        { kRawPrepareParamCropLeft, tr("Crop Left"), tr("Photosites removed at the left of the sensor."), &_cropLeft },
        { kRawPrepareParamCropTop, tr("Crop Top"), tr("Photosites removed at the top of the sensor."), &_cropTop },
        { kRawPrepareParamCropRight, tr("Crop Right"), tr("Photosites removed at the right of the sensor."), &_cropRight },
        { kRawPrepareParamCropBottom, tr("Crop Bottom"), tr("Photosites removed at the bottom of the sensor."), &_cropBottom },
    };
    for (const auto& margin : margins) {
        KnobIntPtr knob = createKnob<KnobInt>(margin.label);
        knob->setName(margin.name);
        knob->setHintToolTip(margin.hint);
        knob->setDefaultValue(0);
        knob->setMinimum(0);
        knob->setDisplayMinimum(0);
        knob->setDisplayMaximum(256);
        knob->setAnimationEnabled(false);
        page->addKnob(knob);
        *margin.store = knob;
    }

    std::vector<ChoiceOption> flatFieldModes;
    flatFieldModes.push_back(ChoiceOption("disabled", tr("Disabled").toStdString(), ""));
    flatFieldModes.push_back(ChoiceOption("embedded", tr("Embedded GainMap").toStdString(), ""));
    KnobChoicePtr flatField = createKnob<KnobChoice>(tr("Flat Field"));
    flatField->setName(kRawPrepareParamFlatField);
    flatField->setHintToolTip(flatFieldHint(false));
    flatField->populateChoices(flatFieldModes);
    flatField->setDefaultValue(eRawPrepareFlatFieldEmbedded);
    flatField->setAnimationEnabled(false);
    page->addKnob(flatField);
    _flatField = flatField;

    refreshOverrides();
}

void
RawPrepare::showFlatFieldAvailability(double time)
{
    const KnobChoicePtr knob = _flatField.lock();

    if (!knob) {
        return;
    }
    const std::optional<Settings> settings = resolveSettings(time, ViewIdx(0));
    const bool available = settings && settings->flatFieldAvailable;

    knob->setHintToolTip(flatFieldHint(available));
    knob->setAllDimensionsEnabled(available);
    knob->setSecret(!available);
}

void
RawPrepare::refreshOverrides()
{
    const KnobBoolPtr blackOverride = _blackOverride.lock();
    const KnobBoolPtr whiteOverride = _whiteOverride.lock();
    const KnobBoolPtr cropOverride = _cropOverride.lock();
    const KnobDoublePtr blackLevels = _blackLevels.lock();
    const KnobDoublePtr whiteLevel = _whiteLevel.lock();

    if (blackLevels && blackOverride) {
        blackLevels->setAllDimensionsEnabled(blackOverride->getValue());
    }
    if (whiteLevel && whiteOverride) {
        whiteLevel->setAllDimensionsEnabled(whiteOverride->getValue());
    }
    const bool crop = cropOverride && cropOverride->getValue();
    for (const KnobIntWPtr& weak : { _cropLeft, _cropTop, _cropRight, _cropBottom }) {
        if (KnobIntPtr knob = weak.lock()) {
            knob->setAllDimensionsEnabled(crop);
        }
    }
}

void
RawPrepare::showSourceLevels(double time)
{
    const KnobBoolPtr blackOverride = _blackOverride.lock();
    const KnobBoolPtr whiteOverride = _whiteOverride.lock();
    const bool showBlack = blackOverride && !blackOverride->getValue();
    const bool showWhite = whiteOverride && !whiteOverride->getValue();

    if (!showBlack && !showWhite) {
        return;
    }
    const ImageMetadata source = getSourceMetadata(time, ViewIdx(0));
    if (showBlack) {
        const std::optional<std::vector<double>> levels = RawMetadata::getBlackLevels(source);
        KnobDoublePtr knob = _blackLevels.lock();
        if (levels && knob) {
            for (int i = 0; i < 4; ++i) {
                knob->setValue((*levels)[(std::size_t)i % levels->size()], ViewSpec::all(), i);
            }
        }
    }
    if (showWhite) {
        const std::optional<double> white = RawMetadata::getWhiteLevel(source);
        KnobDoublePtr knob = _whiteLevel.lock();
        if (white && knob) {
            knob->setValue(*white);
        }
    }
}

void
RawPrepare::onInputChanged(int /*inputNo*/)
{
    showSourceLevels(getCurrentTime());
    showFlatFieldAvailability(getCurrentTime());
}

void
RawPrepare::onKnobsLoaded()
{
    refreshOverrides();
    showSourceLevels(getCurrentTime());
    showFlatFieldAvailability(getCurrentTime());
}

bool
RawPrepare::knobChanged(KnobI* k,
                        ValueChangedReasonEnum reason,
                        ViewSpec /*view*/,
                        double time,
                        bool /*originatedFromMainThread*/)
{
    const KnobBoolPtr blackOverride = _blackOverride.lock();
    const KnobBoolPtr whiteOverride = _whiteOverride.lock();
    const KnobBoolPtr cropOverride = _cropOverride.lock();
    const bool isOverride = (blackOverride && (k == blackOverride.get())) || (whiteOverride && (k == whiteOverride.get())) || (cropOverride && (k == cropOverride.get()));

    if (!isOverride) {
        return false;
    }
    refreshOverrides();
    if (!static_cast<KnobBool*>(k)->getValue()) {
        showSourceLevels(time);
    }
    // Turning an override on in the interface starts it from what the raw file says, which the
    // user then edits; a script or a project load sets the values it wants itself.
    if (((reason != eValueChangedReasonUserEdited) && (reason != eValueChangedReasonNatronGuiEdited)) || !static_cast<KnobBool*>(k)->getValue()) {
        return true;
    }
    const ImageMetadata source = getSourceMetadata(time, ViewIdx(0));
    if (blackOverride && (k == blackOverride.get())) {
        const std::optional<std::vector<double>> levels = RawMetadata::getBlackLevels(source);
        if (levels) {
            if (KnobDoublePtr knob = _blackLevels.lock()) {
                for (int i = 0; i < 4; ++i) {
                    knob->setValue((*levels)[(std::size_t)i % levels->size()], ViewSpec::all(), i);
                }
            }
        }
    } else if (whiteOverride && (k == whiteOverride.get())) {
        const std::optional<double> white = RawMetadata::getWhiteLevel(source);
        KnobDoublePtr knob = _whiteLevel.lock();
        if (white && knob) {
            knob->setValue(*white);
        }
    } else {
        RectI rod;
        const std::optional<RectI> crop = RawMetadata::getCrop(source);
        if (crop && RawNodeSupport::getInputPixelRoD(*this, getSourceInput(), time, ViewIdx(0), &rod)) {
            const RectI kept = crop->intersect(rod);
            if (!kept.isNull()) {
                if (KnobIntPtr knob = _cropLeft.lock()) {
                    knob->setValue(kept.x1 - rod.x1);
                }
                if (KnobIntPtr knob = _cropRight.lock()) {
                    knob->setValue(rod.x2 - kept.x2);
                }
                if (KnobIntPtr knob = _cropBottom.lock()) {
                    knob->setValue(kept.y1 - rod.y1);
                }
                if (KnobIntPtr knob = _cropTop.lock()) {
                    knob->setValue(rod.y2 - kept.y2);
                }
            }
        }
    }

    return true;
} // RawPrepare::knobChanged

std::optional<RawPrepare::Settings>
RawPrepare::resolveSettings(double time,
                            ViewIdx view)
{
    Settings s;

    if (!RawNodeSupport::getInputPixelRoD(*this, getSourceInput(), time, view, &s.sourceRoD)) {
        return std::nullopt;
    }
    const ImageMetadata source = getSourceMetadata(time, view);
    const std::optional<RawStageEnum> stage = RawMetadata::getStage(source);
    s.passThrough = stage && (*stage == eRawStagePrepared);
    if (!stage) {
        s.stageError = RawMetadata::requireStage(source, eRawStageRaw, getNode()->getLabel_mt_safe());
    }
    s.layout = RawLayout::fromMetadata(source).value_or(RawLayout());
    s.hasCfa = s.layout.hasCfa();

    const KnobReader reader(time, view);
    std::vector<double> black;
    if (reader.value(_blackOverride)) {
        for (int i = 0; i < 4; ++i) {
            black.push_back(reader.value(_blackLevels, i));
        }
    } else {
        black = RawMetadata::getBlackLevels(source).value_or(std::vector<double>(1, 0.));
    }
    double white = 1.;
    if (reader.value(_whiteOverride)) {
        white = reader.value(_whiteLevel);
    } else if (const std::optional<double> fromFile = RawMetadata::getWhiteLevel(source)) {
        white = *fromFile;
    }
    for (int i = 0; i < 4; ++i) {
        s.sub[i] = (float)black[(std::size_t)i % black.size()];
        const float div = (float)white - s.sub[i];
        s.div[i] = (div != 0.f) ? div : 1.f;
    }

    s.kept = s.sourceRoD;
    if (reader.value(_cropOverride)) {
        const int left = std::max(0, (int)reader.value(_cropLeft));
        const int top = std::max(0, (int)reader.value(_cropTop));
        const int right = std::max(0, (int)reader.value(_cropRight));
        const int bottom = std::max(0, (int)reader.value(_cropBottom));
        const RectI kept(s.sourceRoD.x1 + left, s.sourceRoD.y1 + bottom, s.sourceRoD.x2 - right, s.sourceRoD.y2 - top);
        if ((kept.width() > 0) && (kept.height() > 0)) {
            s.kept = kept;
        }
    } else if (const std::optional<RectI> crop = RawMetadata::getCrop(source)) {
        const RectI kept = crop->intersect(s.sourceRoD);
        if (!kept.isNull()) {
            s.kept = kept;
        }
    }

    if (s.hasCfa && (s.layout.period() == 2)) {
        const std::vector<DngGainMap> maps = RawMetadata::getGainMaps(source);
        if (!maps.empty()) {
            if (std::shared_ptr<const FlatField> field = acceptGainMaps(maps, s.sourceRoD.width(), s.sourceRoD.height())) {
                s.flatFieldAvailable = true;
                if (reader.value(_flatField) == eRawPrepareFlatFieldEmbedded) {
                    s.flatField = std::move(field);
                }
            }
        }
    }

    return s;
} // RawPrepare::resolveSettings

StatusEnum
RawPrepare::getRegionOfDefinition(U64 hash,
                                  double time,
                                  const RenderScale& scale,
                                  ViewIdx view,
                                  RectD* rod)
{
    const std::optional<Settings> settings = resolveSettings(time, view);

    if (!settings || settings->passThrough) {
        return StagedImageEffect::getRegionOfDefinition(hash, time, scale, view, rod);
    }
    const double par = getAspectRatio(getSourceInput());
    *rod = RectD(0., 0., settings->kept.width() * par, settings->kept.height());

    return eStatusOK;
}

void
RawPrepare::getRegionsOfInterest(double time,
                                 const RenderScale& scale,
                                 const RectD& outputRoD,
                                 const RectD& renderWindow,
                                 ViewIdx view,
                                 RoIMap* ret)
{
    StagedImageEffect::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    EffectInstancePtr input = getInput(getSourceInput());
    const std::optional<Settings> settings = resolveSettings(time, view);
    if (!input || !settings || settings->passThrough) {
        return;
    }
    const double par = getAspectRatio(getSourceInput());
    RectI window = PassExec::toPixelEnclosing(renderWindow, scale, par);
    window.translate(settings->kept.x1, settings->kept.y1);
    (*ret)[input] = PassExec::toCanonical(window, scale, par);
}

ImageMetadata
RawPrepare::deriveOutputMetadata(double time,
                                 ViewIdx view)
{
    ImageMetadata metadata = StagedImageEffect::deriveOutputMetadata(time, view);
    const std::optional<Settings> settings = resolveSettings(time, view);

    if (!settings || settings->passThrough) {
        return metadata;
    }
    RawMetadata::setStage(&metadata, eRawStagePrepared);
    const std::size_t levels = RawMetadata::getBlackLevels(metadata).value_or(std::vector<double>(4, 0.)).size();
    RawMetadata::setBlackLevels(&metadata, std::vector<double>(levels, 0.));
    // The file's white level, which the normalised one no longer tells.
    if (!RawMetadata::getSensorWhiteLevel(metadata)) {
        if (const std::optional<double> white = RawMetadata::getWhiteLevel(metadata)) {
            RawMetadata::setSensorWhiteLevel(&metadata, *white);
        }
    }
    RawMetadata::setWhiteLevel(&metadata, 1.);
    RawMetadata::setProcessedMaximum(&metadata, { 1., 1., 1. });
    RawMetadata::setCrop(&metadata, RectI(0, 0, settings->kept.width(), settings->kept.height()));
    if (settings->hasCfa) {
        settings->layout.croppedTo(settings->kept.x1, settings->kept.y1).writeTo(&metadata);
    }

    return metadata;
}

bool
RawPrepare::isOpIdentity(double time,
                         const RenderScale& /*scale*/,
                         const RectI& /*roi*/,
                         ViewIdx view)
{
    const std::optional<Settings> settings = resolveSettings(time, view);

    if (settings && settings->passThrough) {
        _message.post(getNode(), eMessageTypeWarning, tr("%1: the raw data is already prepared, so it passes through unchanged.").arg(QString::fromUtf8(getNode()->getLabel_mt_safe().c_str())).toStdString());

        return true;
    }
    if (settings && settings->stageError) {
        _message.post(getNode(), eMessageTypeError, *settings->stageError);

        return false;
    }
    _message.withdraw(getNode());

    return false;
}

StatusEnum
RawPrepare::renderOp(const RenderActionArgs& args,
                     const StagePlan& /*plan*/)
{
    const std::optional<Settings> found = resolveSettings(args.time, args.view);

    if (!found || found->passThrough) {
        return eStatusFailed;
    }
    if (found->stageError) {
        _message.post(getNode(), eMessageTypeError, *found->stageError);

        return eStatusFailed;
    }
    const Settings& s = *found;
    const int sourceInput = getSourceInput();

    RawNodeSupport::LockedSource source;
    ImageLayerDesc layer;
    if (resolveInputPlaneForRender(sourceInput, args.time, args.view, &layer, NULL)) {
        RectI roiPixel;
        source.image = getImage(sourceInput, args.time, args.mappedScale, args.view, NULL, &layer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &roiPixel);
    }
    if (!source.image || (source.image->getBitDepth() != eImageBitDepthFloat)) {
        return eStatusFailed;
    }
    source.lock();

    const int dx = s.kept.x1;
    const int dy = s.kept.y1;
    const RectI& roi = args.roi;
    RenderCancellation cancel(this);
    const FlatField* flat = s.flatField.get();
    const int sensorW = s.sourceRoD.width();
    const int sensorH = s.sourceRoD.height();
    const float imToRelX = 1.0f / (float)sensorW;
    const float imToRelY = 1.0f / (float)sensorH;
    const RawKernels::ParallelFor parallel = RawNodeSupport::makeParallelFor(&cancel, appPTR->getNCPUsAvailableForEffect());

    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        const ImagePtr& dst = it->second;
        if (!dst || (dst->getBitDepth() != eImageBitDepthFloat)) {
            return eStatusFailed;
        }
        const int nComps = (int)dst->getComponentsCount();
        Image::WriteAccess access(dst.get());
        parallel(roi.height(), [&](int r) {
            const int y = roi.y1 + r;
            const int sy = y + dy;
            float* out = (float*)access.pixelAt(roi.x1, y);
            if (!out) {
                return;
            }
            const float* rows0[4] = { nullptr, nullptr, nullptr, nullptr };
            const float* rows1[4] = { nullptr, nullptr, nullptr, nullptr };
            int sensorRow = 0;
            float yFrac = 0.f;
            if (flat) {
                sensorRow = s.sourceRoD.y2 - 1 - sy;
                const float yMap = std::min(std::max(((float)sensorRow * imToRelY - flat->originV) * (1.0f / flat->spacingV), 0.f), (float)flat->mapH);
                const uint32_t y0 = std::min((uint32_t)yMap, (uint32_t)flat->mapH - 1);
                const uint32_t y1 = std::min(y0 + 1, (uint32_t)flat->mapH - 1);
                yFrac = yMap - (float)y0;
                for (std::size_t f = 0; f < 4; ++f) {
                    rows0[f] = &flat->gains[f][y0 * (std::size_t)flat->mapW];
                    rows1[f] = &flat->gains[f][y1 * (std::size_t)flat->mapW];
                }
            }
            for (int x = roi.x1; x < roi.x2; ++x, out += nComps) {
                const int sx = x + dx;
                const bool inside = (sx >= source.bounds.x1) && (sx < source.bounds.x2) && (sy >= source.bounds.y1) && (sy < source.bounds.y2);
                const float* in = inside ? (const float*)source.access->pixelAt(sx, sy) : NULL;
                if (s.hasCfa) {
                    const int id = blackCell(s.layout, sx, sy);
                    for (int c = 0; c < nComps; ++c) {
                        const float v = (in && (c < source.nComps)) ? in[c] : 0.f;
                        out[c] = (v - s.sub[id]) / s.div[id];
                    }
                    if (flat && in && (nComps > 0)) {
                        const int sensorCol = sx - s.sourceRoD.x1;
                        const int f = sensorCell(sensorCol, sensorRow);
                        const float xMap = std::min(std::max(((float)sensorCol * imToRelX - flat->originH) * (1.0f / flat->spacingH), 0.f), (float)flat->mapW);
                        const uint32_t x0 = std::min((uint32_t)xMap, (uint32_t)flat->mapW - 1);
                        const uint32_t x1 = std::min(x0 + 1, (uint32_t)flat->mapW - 1);
                        const float xFrac = xMap - (float)x0;
                        const float top = (1.0f - xFrac) * rows0[f][x0] + xFrac * rows0[f][x1];
                        const float bottom = (1.0f - xFrac) * rows1[f][x0] + xFrac * rows1[f][x1];
                        out[0] *= (1.0f - yFrac) * top + yFrac * bottom;
                    }
                } else {
                    for (int c = 0; c < nComps; ++c) {
                        const float v = (in && (c < source.nComps)) ? in[c] : 0.f;
                        const int id = std::min(c, 3);
                        out[c] = (v - s.sub[id]) / s.div[id];
                    }
                }
            }
        });
    }

    return cancel.check() ? eStatusFailed : eStatusOK;
} // RawPrepare::renderOp

bool
RawPrepare::canRenderOpOnGpu(const GpuQuery& query) const
{
#ifdef NATRON_HAVE_GPU
    if ((query.bitDepth != eImageBitDepthFloat) || (query.nComps < 1) || (query.nComps > 4)) {
        return false;
    }
    // Resolving only reads knobs and the source's metadata and region.
    const std::optional<Settings> settings = const_cast<RawPrepare*>(this)->resolveSettings(query.time, query.view);

    return settings && !settings->passThrough && !settings->stageError && !settings->flatField;
#else
    Q_UNUSED(query);

    return false;
#endif
}

bool
RawPrepare::getOpGpuInputLayer(int inputNb,
                               const ImageLayerDesc& plane,
                               double time,
                               ViewIdx view,
                               ImageLayerDesc* layer) const
{
    stageGpuInputLayer(*this, inputNb, plane, time, view, layer);

    return true;
}

RectI
RawPrepare::getOpGpuInputRoI(int inputNb,
                             const RectI& outputRect,
                             const RenderScale& /*scale*/) const
{
    const std::optional<Settings> settings = const_cast<RawPrepare*>(this)->resolveSettings(getCurrentTime(), getCurrentView());

    if ((inputNb != getSourceInput()) || !settings || settings->passThrough) {
        return outputRect;
    }
    RectI moved = outputRect;
    moved.translate(settings->kept.x1, settings->kept.y1);

    return moved;
}

StatusEnum
RawPrepare::renderOpGpu(const GpuRenderArgs& args)
{
#ifdef NATRON_HAVE_GPU
    const std::optional<Settings> found = resolveSettings(args.query.time, args.query.view);

    if (!found || found->passThrough || found->stageError) {
        return eStatusFailed;
    }
    const Settings& s = *found;
    RawPrepareGpuParams params = {};
    params.layout = toRawLayoutGpu(s.hasCfa ? s.layout.croppedTo(s.kept.x1, s.kept.y1) : RawLayout());
    for (int i = 0; i < 4; ++i) {
        params.sub[i] = s.sub[i];
        params.div[i] = s.div[i];
    }
    params.hasCfa = s.hasCfa ? 1u : 0u;

    return RawNodeSupport::recordRawPointGpu(args, getSourceInput(), (int)gpu::GpuKernelId::rawprepare_wrapped, params, s.kept.x1, s.kept.y1);
#else
    Q_UNUSED(args);

    return eStatusFailed;
#endif
}

NATRON_NAMESPACE_EXIT
