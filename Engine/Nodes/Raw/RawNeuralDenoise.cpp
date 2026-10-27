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
 * The Bayer path of darktable's neural restore, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7),
 * src/common/ai/restore_raw_bayer.c, restore.c and restore_common.h, Copyright (C) 2026 darktable
 * developers, distributed under the GNU GPL version 3 or later.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "RawNeuralDenoise.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <vector>

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include "Engine/Ai/InferenceSession.h"
#include "Engine/Ai/ModelDownloader.h"
#include "Engine/Ai/ModelKnob.h"
#include "Engine/Ai/ModelRegistry.h"
#include "Engine/Ai/TiledInference.h"
#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/RectD.h"

NATRON_NAMESPACE_ENTER

namespace {
inline constexpr InputSpec kRawNeuralDenoiseInputs[] = {
    InputSpec { .label = "Source", .kind = eDataKindRaw },
};

constexpr NodeSpec kRawNeuralDenoiseNodeSpec = {
    .id = PLUGINID_NATRON_RAWNEURALDENOISE,
    .label = "RawNeuralDenoise",
    .description = "Denoises and demosaics prepared Bayer raw data in one pass with RawNIND's neural network, as darktable's "
                   "neural restore does. It takes RawDemosaic's place: it outputs camera RGB with an alpha of 1, which "
                   "RawInputColor then takes to the working space. For an X-Trans sensor, use NeuralDenoise with the RawNIND "
                   "linear model after RawInputColor.",
    .grouping = PLUGIN_GROUP_RAW,
    .majorVersion = PLUGIN_MAJOR_NATRON_RAWNEURALDENOISE,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/" PLUGINID_NATRON_RAWNEURALDENOISE ".png",
    .outputKind = eDataKindImage,
    .inputs = kRawNeuralDenoiseInputs,
};

constexpr ImagePipelineSpec kRawNeuralDenoisePipeline = {
    .layers = LayerPolicy::none(),
    .op = OpKind::Custom,
    .rawStage = RawStageRequirement::Prepared,
};

// OVERLAP_PACKED: what neighbouring tiles share on each side of the edge between their cores, in
// packed (half-resolution) pixels.
const int kOverlap = 32;
const int kDefaultTile = 512;
const int kMinimumBuffer = 32;
const float kD65WhiteXyz[3] = { 0.9504f, 1.0f, 1.0889f };

ModelDownloader*&
downloaderOverride()
{
    static ModelDownloader* d = nullptr;

    return d;
}

// _mirror(): reflection without repeating the edge, periodic for long reaches.
int
mirror(int i,
       int n)
{
    if (n <= 1) {
        return 0;
    }
    if (i < 0) {
        i = -i;
    }
    const int period = 2 * (n - 1);
    i %= period;

    return (i >= n) ? (period - i) : i;
}

int
mirrorInRange(int i,
              int lo,
              int hi)
{
    return lo + mirror(i - lo, hi - lo);
}

// The indices [lo, hi) a read of [start, end) mirrored into [lo, hi) touches.
void
mirroredSpan(int start,
             int end,
             int lo,
             int hi,
             int* a,
             int* b)
{
    const int n = hi - lo;
    if ((start - lo <= -(n - 1)) || (end - hi >= n - 1)) {
        *a = lo;
        *b = hi;

        return;
    }
    *a = std::max(start, lo);
    *b = std::min(end, hi);
    if (start < lo) {
        *b = std::max(*b, std::min(hi, lo + (lo - start) + 1));
        *a = std::min(*a, lo);
    }
    if (end > hi) {
        *a = std::min(*a, std::max(lo, 2 * hi - end - 1));
        *b = std::max(*b, hi);
    }
}

// The darktable tiles, of `step` core pixels over [0, length), whose output reaches [a, b), as
// the first and last index. False when none does.
bool
tilesReaching(int a,
              int b,
              int length,
              int step,
              int* first,
              int* last)
{
    const int count = (length + step - 1) / step;

    *first = -1;
    *last = -1;
    for (int t = 0; t < count; ++t) {
        const int lo = t * step - ((t > 0) ? kOverlap : 0);
        const int hi = std::min((t + 1) * step, length) + ((t < count - 1) ? kOverlap : 0);
        if ((hi > a) && (lo < b)) {
            if (*first < 0) {
                *first = t;
            }
            *last = t;
        }
    }

    return *first >= 0;
}

std::array<float, 3>
normalisedToGreen(const std::array<double, 3>& coeffs,
                  bool* ok)
{
    *ok = (coeffs[0] > 0.) && (coeffs[1] > 0.) && (coeffs[2] > 0.);

    return *ok ? std::array<float, 3> { (float)(coeffs[0] / coeffs[1]), 1.f, (float)(coeffs[2] / coeffs[1]) } : std::array<float, 3> { 1.f, 1.f, 1.f };
}

// _bayer_wb_daylight(): the camera's response to D65 white through the inverse of the camera
// matrix, normalised to green.
std::optional<std::array<float, 3>>
daylightWb(const ImageMetadata& metadata)
{
    const std::optional<std::array<double, 9>> m = RawMetadata::getCamToXyz(metadata);
    if (!m) {
        return std::nullopt;
    }
    const std::array<double, 9>& a = *m;
    const double det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]);
    if (std::fabs(det) < 1e-12) {
        return std::nullopt;
    }
    const double inv[9] = {
        (a[4] * a[8] - a[5] * a[7]) / det,
        (a[2] * a[7] - a[1] * a[8]) / det,
        (a[1] * a[5] - a[2] * a[4]) / det,
        (a[5] * a[6] - a[3] * a[8]) / det,
        (a[0] * a[8] - a[2] * a[6]) / det,
        (a[2] * a[3] - a[0] * a[5]) / det,
        (a[3] * a[7] - a[4] * a[6]) / det,
        (a[1] * a[6] - a[0] * a[7]) / det,
        (a[0] * a[4] - a[1] * a[3]) / det,
    };
    float resp[3];
    for (int c = 0; c < 3; ++c) {
        resp[c] = (float)(inv[c * 3] * kD65WhiteXyz[0] + inv[c * 3 + 1] * kD65WhiteXyz[1] + inv[c * 3 + 2] * kD65WhiteXyz[2]);
    }
    if ((resp[0] <= 0.f) || (resp[1] <= 0.f) || (resp[2] <= 0.f)) {
        return std::nullopt;
    }

    return std::array<float, 3> { resp[1] / resp[0], 1.f, resp[1] / resp[2] };
}

std::optional<std::array<float, 3>>
asShotWb(const ImageMetadata& metadata)
{
    const std::optional<std::array<double, 3>> coeffs = RawMetadata::getWbCoeffs(metadata);
    bool ok = false;
    const std::array<float, 3> wb = coeffs ? normalisedToGreen(*coeffs, &ok) : std::array<float, 3> { 1.f, 1.f, 1.f };

    return ok ? std::optional<std::array<float, 3>>(wb) : std::nullopt;
}

std::string
stringAttribute(const QJsonObject& attributes,
                const char* key)
{
    return attributes.value(QString::fromLatin1(key)).toString().toStdString();
}
} // namespace

struct RawNeuralDenoise::Geometry {
    RectI rod;
    RawLayout layout;
    // The red photosite of the top-left 2x2 block, in rows and columns from the top-left.
    int y0 = 0;
    int x0 = 0;
    // The working region in 2x2 blocks: [y0, y0 + 2 * blocksHigh) x [x0, x0 + 2 * blocksWide).
    int blocksHigh = 0;
    int blocksWide = 0;
    std::array<float, 3> wbApplied { { 1.f, 1.f, 1.f } };
    ImageMetadata metadata;

    // The bottom-left corner of the working region in pixels.
    int ax() const
    {
        return rod.x1 + x0;
    }

    int ay() const
    {
        return rod.y2 - y0 - 2 * blocksHigh;
    }

    RectI working() const
    {
        return RectI(ax(), ay(), ax() + 2 * blocksWide, ay() + 2 * blocksHigh);
    }
};

struct RawNeuralDenoise::Policy {
    std::string file;
    int tile = kDefaultTile;
    bool matchGain = true;
    bool mirrorCropped = true;
    std::array<float, 3> wbNorm { { 1.f, 1.f, 1.f } };

    int step() const
    {
        return tile - 2 * kOverlap;
    }
};

namespace {
// Where a render's window reaches into the packed grid, and the packed rectangle its tiles cover
// (x right, y up, row Y being block row blocksHigh - 1 - Y from the top).
struct PackedPlan {
    bool any = false;
    RectI packed;
};

PackedPlan
planPacked(const RectI& window,
           const RawNeuralDenoise::Geometry& g,
           const RawNeuralDenoise::Policy& p)
{
    PackedPlan plan;
    const RectI w = window.intersect(g.working());

    if (w.isNull() || (g.blocksHigh <= 0) || (g.blocksWide <= 0)) {
        return plan;
    }
    const int u0 = (w.x1 - g.ax()) / 2;
    const int u1 = (w.x2 - g.ax() + 1) / 2;
    const int v0 = g.blocksHigh - (w.y2 - g.ay() + 1) / 2;
    const int v1 = g.blocksHigh - (w.y1 - g.ay()) / 2;
    int c0, c1, r0, r1;
    if (!tilesReaching(u0, u1, g.blocksWide, p.step(), &c0, &c1) || !tilesReaching(v0, v1, g.blocksHigh, p.step(), &r0, &r1)) {
        return plan;
    }
    const int left = c0 * p.step() - kOverlap;
    const int right = (c1 + 1) * p.step() + kOverlap;
    const int top = r0 * p.step() - kOverlap;
    const int bottom = (r1 + 1) * p.step() + kOverlap;
    plan.any = true;
    plan.packed = RectI(left, g.blocksHigh - bottom, right, g.blocksHigh - top);

    return plan;
}

// The mirror bounds in rows and columns from the top-left: the 2x2-aligned working region for
// mirror_cropped, the whole source otherwise.
void
mirrorBounds(const RawNeuralDenoise::Geometry& g,
             const RawNeuralDenoise::Policy& p,
             int* rowLo,
             int* rowHi,
             int* colLo,
             int* colHi)
{
    if (p.mirrorCropped) {
        *rowLo = g.y0;
        *rowHi = g.y0 + 2 * g.blocksHigh;
        *colLo = g.x0;
        *colHi = g.x0 + 2 * g.blocksWide;
    } else {
        *rowLo = 0;
        *rowHi = g.rod.height();
        *colLo = 0;
        *colHi = g.rod.width();
    }
}

// The source pixels the packed rectangle reads, mirrored reads included.
RectI
modelSourceRegion(const RectI& packed,
                  const RawNeuralDenoise::Geometry& g,
                  const RawNeuralDenoise::Policy& p)
{
    int rowLo, rowHi, colLo, colHi;
    mirrorBounds(g, p, &rowLo, &rowHi, &colLo, &colHi);
    if ((rowHi <= rowLo) || (colHi <= colLo)) {
        return RectI();
    }
    const int topBlock = g.blocksHigh - packed.y2;
    const int bottomBlock = g.blocksHigh - packed.y1;
    int r0, r1, c0, c1;
    mirroredSpan(g.y0 + 2 * topBlock, g.y0 + 2 * bottomBlock, rowLo, rowHi, &r0, &r1);
    mirroredSpan(g.x0 + 2 * packed.x1, g.x0 + 2 * packed.x2, colLo, colHi, &c0, &c1);

    return RectI(g.rod.x1 + c0, g.rod.y2 - r1, g.rod.x1 + c1, g.rod.y2 - r0);
}

RectI
bilinearSourceRegion(const RectI& window,
                     const RectI& rod)
{
    return RawNodeSupport::expandForKernel(window, 2, 2, kMinimumBuffer, rod);
}
} // namespace

RawNeuralDenoise::RawNeuralDenoise(NodePtr node)
    : StagedImageEffect(node)
    , _modelKnob()
    , _strength()
    , _tileSize()
    , _message()
{
    setSupportsRenderScaleMaybe(eSupportsNo);
}

RawNeuralDenoise::~RawNeuralDenoise()
{
}

const NodeSpec*
RawNeuralDenoise::nodeSpec() const
{
    return staticNodeSpec<kRawNeuralDenoiseNodeSpec>();
}

const ImagePipelineSpec&
RawNeuralDenoise::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kRawNeuralDenoisePipeline, kRawNeuralDenoiseNodeSpec>();
}

ModelDownloader*
RawNeuralDenoise::modelDownloader()
{
    ModelDownloader* d = downloaderOverride();

    return d ? d : ModelDownloader::process();
}

void
RawNeuralDenoise::setModelDownloaderForTests(ModelDownloader* downloader)
{
    downloaderOverride() = downloader;
}

void
RawNeuralDenoise::addAcceptedComponents(int inputNb,
                                        std::list<ImageLayerDesc>* comps)
{
    if (inputNb == -1) {
        comps->push_back(ImageLayerDesc::getRGBAComponents());

        return;
    }
    StagedImageEffect::addAcceptedComponents(inputNb, comps);
}

void
RawNeuralDenoise::initializeKnobs()
{
    KnobPagePtr page = createControlsPage();

    _modelKnob = ModelKnob::create(this, page, AiModelTask::eDenoiseRawBayer, modelDownloader());

    KnobDoublePtr strength = createKnob<KnobDouble>(tr("Strength"));
    strength->setName(kRawNeuralDenoiseParamStrength);
    strength->setHintToolTip(tr("Mixes the model's output with a bilinear demosaic of the same input: 0 is the bilinear demosaic, "
                                "1 the model's output."));
    strength->setDefaultValue(1.);
    strength->setMinimum(0.);
    strength->setMaximum(1.);
    strength->setDisplayMinimum(0.);
    strength->setDisplayMaximum(1.);
    page->addKnob(strength);
    _strength = strength;

    KnobIntPtr tileSize = createKnob<KnobInt>(tr("Tile Size"));
    tileSize->setName(kRawNeuralDenoiseParamTileSize);
    tileSize->setHintToolTip(tr("The edge of the tiles the model runs on, in 2x2 photosite blocks, so a tile covers twice as many "
                                "photosites across. 0 takes the size the model was exported at. A model exported at a fixed size "
                                "runs only at that size. Neighbouring tiles share 32 blocks on each side of their edge, so a tile "
                                "is at least 128."));
    tileSize->setDefaultValue(0);
    tileSize->setMinimum(0);
    tileSize->setMaximum(4096);
    tileSize->setDisplayMinimum(0);
    tileSize->setDisplayMaximum(1024);
    tileSize->setAnimationEnabled(false);
    page->addKnob(tileSize);
    _tileSize = tileSize;
}

void
RawNeuralDenoise::onEffectCreated(bool /*mayCreateFileDialog*/,
                                  const CreateNodeArgs& args)
{
    if (_modelKnob) {
        _modelKnob->onEffectCreated(args);
    }
}

bool
RawNeuralDenoise::knobChanged(KnobI* k,
                              ValueChangedReasonEnum /*reason*/,
                              ViewSpec /*view*/,
                              double /*time*/,
                              bool /*originatedFromMainThread*/)
{
    return _modelKnob && _modelKnob->knobChanged(k);
}

std::optional<RawNeuralDenoise::Geometry>
RawNeuralDenoise::resolveGeometry(double time,
                                  ViewIdx view,
                                  std::string* error)
{
    Geometry g;

    if (!RawNodeSupport::getInputPixelRoD(*this, getSourceInput(), time, view, &g.rod) || g.rod.isNull()) {
        return std::nullopt;
    }
    g.metadata = getSourceMetadata(time, view);
    g.layout = RawLayout::fromMetadata(g.metadata).value_or(RawLayout());
    if (g.layout.period() != 2) {
        const NodePtr node = getNode();
        const std::string label = node ? node->getLabel_mt_safe() : getPluginLabel();
        *error = label + ": " + (g.layout.hasCfa() ? "RawNIND's raw model takes a Bayer mosaic, not an X-Trans one." : "the source has no colour filter array; RawNIND's raw model takes a Bayer mosaic.")
            + " Use RawDemosaic and RawInputColor, then NeuralDenoise with the RawNIND linear model.";

        return std::nullopt;
    }
    // _bayer_origin(): the red photosite of the top-left 2x2 block.
    bool found = false;
    for (int r = 0; (r < 2) && !found; ++r) {
        for (int c = 0; (c < 2) && !found; ++c) {
            if (g.layout.colourAt(g.rod.x1 + c, g.rod.y2 - 1 - r) == eRawCfaColorRed) {
                g.y0 = r;
                g.x0 = c;
                found = true;
            }
        }
    }
    if (!found) {
        *error = "the source's colour filter array has no red photosite.";

        return std::nullopt;
    }
    g.blocksHigh = std::max(0, (g.rod.height() - g.y0) / 2);
    g.blocksWide = std::max(0, (g.rod.width() - g.x0) / 2);
    if (const std::optional<std::array<double, 3>> applied = RawMetadata::getWbApplied(g.metadata)) {
        for (int c = 0; c < 3; ++c) {
            g.wbApplied[c] = ((*applied)[c] > 0.) ? (float)(*applied)[c] : 1.f;
        }
    }

    return g;
}

std::optional<RawNeuralDenoise::Policy>
RawNeuralDenoise::resolvePolicy(double time,
                                ViewIdx view,
                                std::string* error)
{
    if (!_modelKnob) {
        return std::nullopt;
    }
    const std::optional<ModelKnob::Resolved> resolved = _modelKnob->resolve();
    if (!resolved) {
        return std::nullopt;
    }
    Policy p;
    if (resolved->files.empty()) {
        *error = "the model " + resolved->entry->id + " lists no ONNX file.";

        return std::nullopt;
    }
    p.file = resolved->files.front();

    const QJsonObject attributes = QJsonDocument::fromJson(QByteArray::fromStdString(resolved->config.attributesJson)).object();
    const std::string kind = resolved->config.inputKind;
    if (!kind.empty() && (kind != "bayer_v1")) {
        *error = "the model " + resolved->entry->id + " takes input of kind `" + kind + "`; this node runs bayer_v1 models only.";

        return std::nullopt;
    }
    // restore.c's defaults for a Bayer model: daylight white balance, match_gain, force_rggb and,
    // with it, mirror_cropped.
    const std::string wb = stringAttribute(attributes, "wb_norm");
    const std::string scale = stringAttribute(attributes, "output_scale");
    const std::string orientation = stringAttribute(attributes, "bayer_orientation");
    const std::string edge = stringAttribute(attributes, "edge_pad");
    if (!orientation.empty() && (orientation != "force_rggb")) {
        *error = "the model " + resolved->entry->id + " asks for bayer_orientation `" + orientation + "`; this node supports force_rggb only.";

        return std::nullopt;
    }
    p.matchGain = (scale != "absolute");
    p.mirrorCropped = (edge != "mirror");
    p.wbNorm = { { 1.f, 1.f, 1.f } };
    if (wb != "none") {
        // An unknown name falls back on the default, as restore.c's parser does.
        const ImageMetadata metadata = getSourceMetadata(time, view);
        std::optional<std::array<float, 3>> picked;
        if (wb == "as_shot") {
            picked = asShotWb(metadata);
            if (!picked) {
                picked = daylightWb(metadata);
            }
        } else {
            picked = daylightWb(metadata);
            if (!picked) {
                picked = asShotWb(metadata);
            }
        }
        if (picked) {
            p.wbNorm = *picked;
        }
    }

    const int knob = std::max(0, (int)KnobReader(time, view).value(_tileSize));
    const int configured = resolved->config.tileSize();
    p.tile = (knob > 0) ? knob : ((configured > 0) ? configured : kDefaultTile);
    if (p.tile < 4 * kOverlap) {
        *error = "a tile of " + std::to_string(p.tile) + " blocks is too small: neighbouring tiles share " + std::to_string(2 * kOverlap)
            + " blocks, so a tile is at least " + std::to_string(4 * kOverlap) + ".";

        return std::nullopt;
    }

    return p;
}

RectI
RawNeuralDenoise::sourceRegionFor(const RectI& window,
                                  double time,
                                  ViewIdx view)
{
    std::string error;
    const std::optional<Geometry> g = resolveGeometry(time, view, &error);

    if (!g) {
        return window;
    }
    RectI need = bilinearSourceRegion(window, g->rod);
    const std::optional<Policy> p = resolvePolicy(time, view, &error);
    if (p) {
        const PackedPlan plan = planPacked(window, *g, *p);
        if (plan.any) {
            const RectI model = modelSourceRegion(plan.packed, *g, *p);
            if (!model.isNull()) {
                if (need.isNull()) {
                    need = model;
                } else {
                    need.merge(model);
                }
            }
        }
    }

    return need;
}

bool
RawNeuralDenoise::isOpIdentity(double /*time*/,
                               const RenderScale& /*scale*/,
                               const RectI& /*roi*/,
                               ViewIdx /*view*/)
{
    return false;
}

void
RawNeuralDenoise::getRegionsOfInterest(double time,
                                       const RenderScale& scale,
                                       const RectD& outputRoD,
                                       const RectD& renderWindow,
                                       ViewIdx view,
                                       RoIMap* ret)
{
    StagedImageEffect::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    EffectInstancePtr input = getInput(getSourceInput());
    if (!input) {
        return;
    }
    const double par = getRenderMetadata(time, view)->getPixelAspectRatio(getSourceInput());
    const RectI need = sourceRegionFor(PassExec::toPixelEnclosing(renderWindow, scale, par), time, view);
    (*ret)[input] = PassExec::toCanonical(need, scale, par);
}

StatusEnum
RawNeuralDenoise::getPreferredMetadataAt(double /*time*/,
                                         ViewIdx /*view*/,
                                         NodeMetadata& metadata)
{
    metadata.setNComps(-1, 4);

    return eStatusOK;
}

ImageMetadata
RawNeuralDenoise::deriveOutputMetadata(double time,
                                       ViewIdx view)
{
    ImageMetadata metadata = StagedImageEffect::deriveOutputMetadata(time, view);

    // As after RawDemosaic: the camera matrix and white balance stay for RawInputColor.
    metadata.remove(kRawMetadataCfaPattern);
    metadata.remove(kRawMetadataCfaOrigin);
    metadata.remove(kRawMetadataStage);
    metadata.remove(kRawMetadataBlackLevels);
    metadata.remove(kRawMetadataWhiteLevel);

    return metadata;
}

StatusEnum
RawNeuralDenoise::renderOp(const RenderActionArgs& args,
                           const StagePlan& /*plan*/)
{
    const NodePtr node = getNode();
    std::string error;
    const std::optional<Geometry> found = resolveGeometry(args.time, args.view, &error);

    if (!found) {
        if (!error.empty()) {
            _message.post(node, eMessageTypeError, error);
        }

        return eStatusFailed;
    }
    const Geometry& g = *found;
    const std::optional<Policy> policy = resolvePolicy(args.time, args.view, &error);
    if (!policy) {
        if (error.empty()) {
            _message.withdraw(node);
        } else {
            _message.post(node, eMessageTypeError, error);
        }

        return eStatusFailed;
    }
    const Policy& p = *policy;
    const float strength = (float)std::max(0., std::min(1., (double)KnobReader(args.time, args.view).value(_strength)));
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

    RenderCancellation cancel(this);
    const RawKernels::ParallelFor parallel = RawNodeSupport::makeParallelFor(&cancel, appPTR->getNCPUsAvailableForEffect());
    const RectI& roi = args.roi;

    // The bilinear demosaic: what strength mixes towards, and the rows and columns outside the
    // working region.
    const RectI bilinearRect = bilinearSourceRegion(roi, g.rod);
    RawNodeSupport::TopDownBuffer bilinear;
    if (!bilinearRect.isNull()) {
        RawNodeSupport::TopDownBuffer mosaic;
        mosaic.allocate(bilinearRect, 1);
        RawNodeSupport::readTopDown(source, &mosaic, parallel);
        bilinear.allocate(bilinearRect, 4);
        const std::uint32_t filters = RawNodeSupport::bayerFiltersAt(g.layout, bilinearRect);
        RawKernels::vng(bilinear.data.data(), mosaic.data.data(), bilinearRect.width(), bilinearRect.height(), filters, NULL, true, parallel);
    }

    RawNodeSupport::TopDownBuffer rgb;
    rgb.allocate(roi, 3);
    parallel(roi.height(), [&](int r) {
        const int y = roi.y2 - 1 - r;
        for (int x = roi.x1; x < roi.x2; ++x) {
            float* q = &rgb.data[rgb.offset(x, y)];
            const bool inside = (x >= bilinear.rect.x1) && (x < bilinear.rect.x2) && (y >= bilinear.rect.y1) && (y < bilinear.rect.y2);
            for (int c = 0; c < 3; ++c) {
                q[c] = inside ? bilinear.data[bilinear.offset(x, y) + c] : 0.f;
            }
        }
    });

    const PackedPlan plan = (strength > 0.f) ? planPacked(roi, g, p) : PackedPlan();
    if (plan.any) {
        const RectI modelRect = modelSourceRegion(plan.packed, g, p);
        RawNodeSupport::TopDownBuffer mosaic;
        mosaic.allocate(modelRect, 1);
        RawNodeSupport::readTopDown(source, &mosaic, parallel);

        int rowLo, rowHi, colLo, colHi;
        mirrorBounds(g, p, &rowLo, &rowHi, &colLo, &colHi);
        // Prepared data is darktable's normalised value times the white balance applied since.
        std::array<float, 3> packScale, unpackScale;
        for (int c = 0; c < 3; ++c) {
            packScale[c] = p.wbNorm[c] / g.wbApplied[c];
            unpackScale[c] = g.wbApplied[c] / p.wbNorm[c];
        }

        const AiTilePack pack = [&](const RectI& rect, Tensor& tile) {
            const int width = rect.width();
            const std::size_t planeSize = (std::size_t)width * rect.height();
            float* const planes = tile.data();
            parallel(rect.height(), [&](int i) {
                const int block = g.blocksHigh - 1 - (rect.y2 - 1 - i);
                for (int j = 0; j < width; ++j) {
                    const int column = rect.x1 + j;
                    for (int k = 0; k < 4; ++k) {
                        const int r = mirrorInRange(g.y0 + 2 * block + (k >> 1), rowLo, rowHi);
                        const int c = mirrorInRange(g.x0 + 2 * column + (k & 1), colLo, colHi);
                        const int x = g.rod.x1 + c;
                        const int y = g.rod.y2 - 1 - r;
                        float v = 0.f;
                        if ((x >= modelRect.x1) && (x < modelRect.x2) && (y >= modelRect.y1) && (y < modelRect.y2)) {
                            const RawCfaColorEnum colour = g.layout.colourAt(x, y);
                            v = mosaic.data[mosaic.offset(x, y)] * ((colour <= eRawCfaColorBlue) ? packScale[colour] : 1.f);
                        }
                        planes[(std::size_t)k * planeSize + (std::size_t)i * width + j] = v;
                    }
                }
            });
        };

        // _bayer_gain_match(): the output's mean brought to the input's, over the whole tile.
        const AiTileFinish finish = [](const Tensor& input, Tensor& output) {
            double inSum = 0.;
            double outSum = 0.;
            for (float v : input.values()) {
                inSum += v;
            }
            for (float v : output.values()) {
                outSum += v;
            }
            const double inMean = inSum / (double)std::max<std::size_t>(1, input.size());
            const double outMean = outSum / (double)std::max<std::size_t>(1, output.size());
            const float gain = (std::fabs(outMean) > 1e-6 * std::fabs(inMean)) ? (float)(inMean / outMean) : 1.f;
            if (gain != 1.f) {
                for (float& v : output.values()) {
                    v *= gain;
                }
            }
        };

        const int ax = g.ax();
        const int ay = g.ay();
        const RectI working = g.working();
        const AiTileUnpack unpack = [&](const RectI& outRoI, const Tensor& result) {
            const std::size_t outWidth = (std::size_t)outRoI.width();
            const std::size_t planeSize = outWidth * outRoI.height();
            const float* const planes = result.data();
            parallel(roi.height(), [&](int r) {
                const int y = roi.y2 - 1 - r;
                if ((y < working.y1) || (y >= working.y2)) {
                    return;
                }
                const int row = outRoI.y2 - 1 - (y - ay);
                if ((row < 0) || (row >= outRoI.height())) {
                    return;
                }
                for (int x = std::max(roi.x1, working.x1); x < std::min(roi.x2, working.x2); ++x) {
                    const int col = (x - ax) - outRoI.x1;
                    if ((col < 0) || (col >= outRoI.width())) {
                        continue;
                    }
                    float* q = &rgb.data[rgb.offset(x, y)];
                    const std::size_t at = (std::size_t)row * outWidth + col;
                    for (int c = 0; c < 3; ++c) {
                        const float model = planes[(std::size_t)c * planeSize + at] * unpackScale[c];
                        q[c] = std::max(0.f, strength * model + (1.f - strength) * q[c]);
                    }
                }
            });
        };

        AiTileSpec spec;
        spec.tile = p.tile;
        spec.overlap = 2 * kOverlap;
        spec.scale = 2;
        spec.inChannels = 4;
        spec.outChannels = 3;
        spec.linearBlend = true;
        try {
            const std::shared_ptr<InferenceSession> session = InferenceSession::open(p.file, AiSessionOptions::fromPreferences(AiGraphOptimization::eAll));
            const std::vector<int64_t>& shape = session->inputs().front().shape;
            if ((shape.size() == 4) && (((shape[2] > 0) && (shape[2] != p.tile)) || ((shape[3] > 0) && (shape[3] != p.tile)))) {
                _message.post(node, eMessageTypeError, "the model runs on " + std::to_string(shape[2]) + " x " + std::to_string(shape[3]) + " tiles only; set Tile Size to 0 or " + std::to_string(shape[2]) + ".");

                return eStatusFailed;
            }
            runTiled(session, plan.packed, spec, pack, unpack, [this]() { return aborted(); }, p.matchGain ? finish : AiTileFinish());
        } catch (const AiAborted&) {
            return eStatusFailed;
        } catch (const AiError& e) {
            _message.post(node, eMessageTypeError, e.what());

            return eStatusFailed;
        }
    }
    _message.withdraw(node);

    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        const ImagePtr& dst = it->second;
        if (!dst || (dst->getBitDepth() != eImageBitDepthFloat)) {
            return eStatusFailed;
        }
        const int dstComps = (int)dst->getComponentsCount();
        Image::WriteAccess access(dst.get());
        parallel(roi.height(), [&](int r) {
            const int y = roi.y1 + r;
            float* outPix = (float*)access.pixelAt(roi.x1, y);
            if (!outPix) {
                return;
            }
            for (int x = roi.x1; x < roi.x2; ++x, outPix += dstComps) {
                const float* q = &rgb.data[rgb.offset(x, y)];
                for (int c = 0; c < dstComps; ++c) {
                    outPix[c] = (c == 3) ? 1.f : q[std::min(c, 2)];
                }
            }
        });
    }

    return cancel.check() ? eStatusFailed : eStatusOK;
} // RawNeuralDenoise::renderOp

NATRON_NAMESPACE_EXIT
