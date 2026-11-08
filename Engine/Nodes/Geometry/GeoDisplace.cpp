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

#include "GeoDisplace.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <exception>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QString>

#include "Engine/ChoiceOption.h"
#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeColorSampler.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Generator/NoiseCore.h"
#include "Engine/Nodes/Geometry/GeoInterpolate.h"
#include "Engine/Nodes/Geometry/GeoNormals.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

namespace {
const char kNormalName[] = "N";

bool
isUsableNormal(const GeoAttribute* attribute)
{
    return attribute && geoIsNumeric(*attribute) && (attribute->tupleSize() >= 3);
}

float
channelValue(const float rgba[4],
             GeoDisplaceChannelEnum channel)
{
    switch (channel) {
    case eGeoDisplaceChannelRed:
        return rgba[0];
    case eGeoDisplaceChannelGreen:
        return rgba[1];
    case eGeoDisplaceChannelBlue:
        return rgba[2];
    case eGeoDisplaceChannelAlpha:
        return rgba[3];
    case eGeoDisplaceChannelLuminance:
    default:
        return 0.2126f * rgba[0] + 0.7152f * rgba[1] + 0.0722f * rgba[2];
    }
}

template <typename PIX>
void
readRow(const Image::ReadAccess& access,
        const RectI& rect,
        int y,
        int numComps,
        float scale,
        GeoDisplaceChannelEnum channel,
        float* row)
{
    for (int x = rect.x1; x < rect.x2; ++x, ++row) {
        const PIX* pix = reinterpret_cast<const PIX*>(access.pixelAt(x, y));
        if (!pix) {
            *row = 0.f;
            continue;
        }
        float rgba[4] = { 0.f, 0.f, 0.f, 1.f };
        if (numComps == 1) {
            // An alpha-only image: its one channel stands for every channel.
            rgba[0] = rgba[1] = rgba[2] = rgba[3] = pix[0] * scale;
        } else {
            for (int c = 0; c < std::min(numComps, 4); ++c) {
                rgba[c] = pix[c] * scale;
            }
        }
        *row = channelValue(rgba, channel);
    }
}

// Renders the input over its format and keeps one channel. A render resets the render state of
// the thread it runs on, and this one is in the middle of the geometry render.
bool
renderImageChannel(const NodePtr& node,
                   double time,
                   ViewIdx view,
                   GeoDisplaceChannelEnum channel,
                   GeoDisplaceImage* out,
                   std::string* failure)
{
    RectI format;
    RectI window;
    if (!getNodeImageWindow(node, time, view, &format, &window) || format.isNull()) {
        *failure = node->getFullyQualifiedName() + " has no image to displace by";

        return false;
    }
    RectI rect;
    ImagePtr image;
    if (!renderNodeImageOverWindow(node, time, view, ImageLayerDesc::getRGBAComponents(), format, &image, &rect)
        || (image && rect.isNull())) {
        *failure = node->getFullyQualifiedName() + " could not be rendered";

        return false;
    }
    out->width = format.width();
    out->height = format.height();
    out->values.assign((std::size_t)out->width * (std::size_t)out->height, 0.f);
    if (!image) {
        return true;
    }
    rect = rect.intersect(format);
    const int numComps = (int)image->getComponentsCount();
    Image::ReadAccess access(image.get());
    for (int y = rect.y1; y < rect.y2; ++y) {
        float* row = out->values.data() + (std::size_t)(y - format.y1) * (std::size_t)out->width + (std::size_t)(rect.x1 - format.x1);
        switch (image->getBitDepth()) {
        case eImageBitDepthByte:
            readRow<unsigned char>(access, rect, y, numComps, 1.f / 255.f, channel, row);
            break;
        case eImageBitDepthShort:
            readRow<unsigned short>(access, rect, y, numComps, 1.f / 65535.f, channel, row);
            break;
        case eImageBitDepthFloat:
            readRow<float>(access, rect, y, numComps, 1.f, channel, row);
            break;
        default:
            *failure = "the image of " + node->getFullyQualifiedName() + " has a bit depth GeoDisplace does not read";

            return false;
        }
    }

    return true;
} // renderImageChannel

double
noiseAt(const NoiseCore::NoiseParams& noise,
        const GeoDisplaceParams& params,
        const std::array<double, 3>& p)
{
    noise4d::NoiseCoord q;
    const noise4d::NoiseAxis x = noise4d::axisFromFloat((float)(p[0] * params.frequency[0] + params.noiseOffset[0]));
    const noise4d::NoiseAxis y = noise4d::axisFromFloat((float)(p[1] * params.frequency[1] + params.noiseOffset[1]));
    const noise4d::NoiseAxis z = noise4d::axisFromFloat((float)(p[2] * params.frequency[2] + params.noiseOffset[2]));
    const noise4d::NoiseAxis w = noise4d::axisFromFloat((float)params.evolution);
    q.cx = x.c;
    q.fx = x.f;
    q.cy = y.c;
    q.fy = y.f;
    q.cz = z.c;
    q.fz = z.f;
    q.cw = w.c;
    q.fw = w.f;

    return (double)NoiseCore::noise4(noise, q);
}

GeoNormalsTargetEnum
normalsTarget(GeoAttribClassEnum cls)
{
    switch (cls) {
    case eGeoAttribClassVertex:
        return eGeoNormalsTargetVertices;
    case eGeoAttribClassPrimitive:
        return eGeoNormalsTargetPrimitives;
    default:
        return eGeoNormalsTargetPoints;
    }
}
} // anonymous namespace

double
GeoDisplaceImage::sample(double u,
                         double v) const
{
    if ((width <= 0) || (height <= 0) || (values.size() < (std::size_t)width * (std::size_t)height)) {
        return 0.;
    }
    const double x = std::clamp(u * width - 0.5, 0., (double)(width - 1));
    const double y = std::clamp(v * height - 0.5, 0., (double)(height - 1));
    const int x0 = (int)std::floor(x);
    const int y0 = (int)std::floor(y);
    const int x1 = std::min(x0 + 1, width - 1);
    const int y1 = std::min(y0 + 1, height - 1);
    const double fx = x - x0;
    const double fy = y - y0;
    auto at = [this](int i,
                     int j) {
        return (double)values[(std::size_t)j * (std::size_t)width + (std::size_t)i];
    };
    const double bottom = at(x0, y0) * (1. - fx) + at(x1, y0) * fx;
    const double top = at(x0, y1) * (1. - fx) + at(x1, y1) * fx;

    return bottom * (1. - fy) + top * fy;
}

bool
displaceGeometry(GeoDetailBuilder& detail,
                 const GeoDisplaceParams& params,
                 const GeoDisplaceImage* image,
                 std::string* errorMessage)
{
    GeoElementMask points;
    if (!geoSelectElements(detail.detail(), eGeoAttribClassPoint, params.group, &points, errorMessage)) {
        return false;
    }
    if (params.displaceBy == eGeoDisplaceByImage) {
        if (!image) {
            if (errorMessage) {
                *errorMessage = "No image to displace by: connect the Image input.";
            }

            return false;
        }
        if (!GeoPointUVReader(detail.detail(), params.uvAttribute).valid()) {
            if (errorMessage) {
                *errorMessage = "The geometry has no texture coordinates named \"" + params.uvAttribute + "\" to sample the image at.";
            }

            return false;
        }
    }

    GeoAttribClassEnum normalClass = eGeoAttribClassPoint;
    for (GeoAttribClassEnum cls : { eGeoAttribClassPoint, eGeoAttribClassVertex, eGeoAttribClassPrimitive }) {
        if (isUsableNormal(detail.detail().findAttribute(cls, kNormalName))) {
            normalClass = cls;
            break;
        }
    }
    if (!isUsableNormal(detail.detail().findAttribute(eGeoAttribClassPoint, kNormalName))) {
        GeoNormalsParams normals;
        normals.target = eGeoNormalsTargetPoints;
        if (!GeoNormals::computeNormals(detail, normals, errorMessage)) {
            return false;
        }
    }

    std::vector<std::pair<GeoOffset, std::array<double, 3>>> moved;
    {
        const GeoDetail& d = detail.detail();
        const GeoAttribute* normal = d.findAttribute(eGeoAttribClassPoint, kNormalName);
        if (!isUsableNormal(normal)) {
            if (errorMessage) {
                *errorMessage = "The geometry has no polygons to take normals from.";
            }

            return false;
        }
        NoiseCore::NoiseParams noise = NoiseCore::defaultNoiseParams();
        noise.type = std::clamp(params.noiseType, NoiseCore::kNoiseTypePerlin, NoiseCore::kNoiseTypeWhite);
        noise.fractal = std::clamp(params.fractal, NoiseCore::kNoiseFractalNone, NoiseCore::kNoiseFractalHybrid);
        noise.octaves = std::clamp(params.octaves, 1, NoiseCore::kNoiseMaxOctaves);
        noise.lacunarity = (float)params.lacunarity;
        noise.gain = (float)params.gain;
        noise.seed = params.seed;
        const bool byImage = params.displaceBy == eGeoDisplaceByImage;
        const GeoPointUVReader uvs(d, params.uvAttribute);
        std::vector<double> n((std::size_t)normal->tupleSize());
        d.pointMap().forEachLive([&](GeoIndex, GeoOffset point) {
            if (!points[(std::size_t)point]) {
                return;
            }
            geoReadNumeric(*normal, (std::size_t)point, std::span<double>(n));
            const double length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (!(length > 0.) || !std::isfinite(length)) {
                return;
            }
            const std::array<double, 3> p = d.pointPosition(point);
            double value;
            if (byImage) {
                const std::array<double, 2> uv = uvs.uv(point);
                value = image->sample(uv[0], uv[1]);
            } else {
                value = noiseAt(noise, params, p);
            }
            const double distance = (params.amplitude * value + params.offset) / length;
            moved.emplace_back(point, std::array<double, 3> { p[0] + n[0] * distance, p[1] + n[1] * distance, p[2] + n[2] * distance });
        });
    }
    for (const auto& [point, p] : moved) {
        detail.setPointPosition(point, p[0], p[1], p[2]);
    }

    if (params.recomputeNormals) {
        GeoNormalsParams normals;
        normals.target = normalsTarget(normalClass);
        if (!GeoNormals::computeNormals(detail, normals, errorMessage)) {
            return false;
        }
    }

    return true;
} // displaceGeometry

GeoDisplace::GeoDisplace(NodePtr node)
    : NativeEffectBase(node)
{
}

GeoDisplace::~GeoDisplace()
{
}

const NodeSpec*
GeoDisplace::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
GeoDisplace::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());
    auto createDouble = [this, &page](const QString& label,
                                      const char* name,
                                      const QString& toolTip,
                                      int dimension,
                                      double defaultValue,
                                      double displayMinimum,
                                      double displayMaximum) {
        KnobDoublePtr knob = createKnob<KnobDouble>(label.toStdString(), dimension);
        knob->setName(name);
        knob->setHintToolTip(toolTip.toStdString());
        for (int i = 0; i < dimension; ++i) {
            knob->setDefaultValue(defaultValue, i);
            knob->setDisplayMinimum(displayMinimum, i);
            knob->setDisplayMaximum(displayMaximum, i);
        }
        page->addKnob(knob);

        return knob;
    };
    auto createChoice = [this, &page](const QString& label,
                                      const char* name,
                                      const QString& toolTip,
                                      const std::vector<ChoiceOption>& options,
                                      int defaultValue) {
        KnobChoicePtr knob = createKnob<KnobChoice>(label.toStdString());
        knob->setName(name);
        knob->setHintToolTip(toolTip.toStdString());
        knob->populateChoices(options);
        knob->setDefaultValue(defaultValue);
        knob->setAnimationEnabled(false);
        page->addKnob(knob);

        return knob;
    };

    KnobStringPtr group = createKnob<KnobString>(tr("Group").toStdString());
    group->setName(kGeoDisplaceParamGroup);
    group->setHintToolTip(tr("The points to move, as a point group pattern; empty for every point.").toStdString());
    group->setAnimationEnabled(false);
    page->addKnob(group);
    _group = group;

    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("noise", tr("Noise").toStdString(),
                                       tr("A noise evaluated at each point's position, between -1 and 1.").toStdString()));
        options.push_back(ChoiceOption("image", tr("Image").toStdString(),
                                       tr("The Image input, sampled at each point's texture coordinates over its format.").toStdString()));
        _displaceBy = createChoice(tr("Displace By"), kGeoDisplaceParamDisplaceBy,
                                   tr("What sets how far each point moves."), options, (int)eGeoDisplaceByNoise);
    }
    _amplitude = createDouble(tr("Amplitude"), kGeoDisplaceParamAmplitude,
                              tr("The distance a value of 1 moves a point along its normal; negative values push inwards."),
                              1, 1., -10., 10.);
    _offset = createDouble(tr("Offset"), kGeoDisplaceParamOffset,
                           tr("A distance added to every point's displacement, after Amplitude."),
                           1, 0., -10., 10.);

    {
        static const struct {
            const char* id;
            const char* label;
            const char* hint;
        } kTypes[] = {
            { "perlin", "Perlin", "Gradient noise on a square lattice." },
            { "simplex", "Simplex", "Gradient noise on a simplex lattice (OpenSimplex2S): fewer axis-aligned artefacts than Perlin, at about twice the cost." },
            { "value", "Value", "Random values at the lattice points, smoothly interpolated." },
            { "cellular", "Cellular", "Voronoi noise: the distance to the nearest jittered feature point." },
            { "white", "White", "A random value per cell, without interpolation." },
        },
          kFractals[] = {
              { "none", "None", "A single octave." },
              { "fbm", "fBm", "Fractional Brownian motion: octaves of rising frequency and falling amplitude, summed." },
              { "turbulence", "Turbulence", "fBm over the absolute value of each octave (billow)." },
              { "ridged", "Ridged", "Ridged multifractal: sharp ridges where each octave crosses zero." },
              { "hybrid", "Hybrid", "Hybrid multifractal: higher octaves add detail only where the lower ones are high." },
          };
        std::vector<ChoiceOption> types;
        for (const auto& t : kTypes) {
            types.push_back(ChoiceOption(t.id, tr(t.label).toStdString(), tr(t.hint).toStdString()));
        }
        _noiseType = createChoice(tr("Noise Type"), kGeoDisplaceParamNoiseType, tr("The basis noise."), types, NoiseCore::kNoiseTypePerlin);
        std::vector<ChoiceOption> fractals;
        for (const auto& f : kFractals) {
            fractals.push_back(ChoiceOption(f.id, tr(f.label).toStdString(), tr(f.hint).toStdString()));
        }
        _fractal = createChoice(tr("Fractal"), kGeoDisplaceParamFractal, tr("How octaves of the noise are combined."), fractals,
                                NoiseCore::kNoiseFractalNone);
    }

    KnobIntPtr octaves = createKnob<KnobInt>(tr("Octaves").toStdString());
    octaves->setName(kGeoDisplaceParamOctaves);
    octaves->setHintToolTip(tr("How many octaves a fractal sums.").toStdString());
    octaves->setDefaultValue(4);
    octaves->setMinimum(1);
    octaves->setMaximum(NoiseCore::kNoiseMaxOctaves);
    octaves->setDisplayMinimum(1);
    octaves->setDisplayMaximum(NoiseCore::kNoiseMaxOctaves);
    page->addKnob(octaves);
    _octaves = octaves;
    _lacunarity = createDouble(tr("Lacunarity"), kGeoDisplaceParamLacunarity,
                               tr("The frequency ratio between one octave and the next."), 1, 2., 1., 4.);
    _gain = createDouble(tr("Gain"), kGeoDisplaceParamGain,
                         tr("The amplitude ratio between one octave and the next."), 1, 0.5, 0., 1.);

    KnobIntPtr seed = createKnob<KnobInt>(tr("Seed").toStdString());
    seed->setName(kGeoDisplaceParamSeed);
    seed->setHintToolTip(tr("Random seed: each value gives a different pattern.").toStdString());
    seed->setDefaultValue(0);
    seed->setMinimum(INT_MIN);
    seed->setMaximum(INT_MAX);
    seed->setDisplayMinimum(0);
    seed->setDisplayMaximum(100);
    page->addKnob(seed);
    _seed = seed;

    _frequency = createDouble(tr("Frequency"), kGeoDisplaceParamFrequency,
                              tr("Noise cycles per scene unit along X, Y and Z."), 3, 1., 0., 10.);
    _noiseOffset = createDouble(tr("Noise Offset"), kGeoDisplaceParamNoiseOffset,
                                tr("Slides the noise through space, in noise cycles."), 3, 0., -10., 10.);
    _evolution = createDouble(tr("Evolution"), kGeoDisplaceParamEvolution,
                              tr("Moves through the noise's fourth dimension: animate it for a noise that changes in place."),
                              1, 0., 0., 10.);

    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("luminance", tr("Luminance").toStdString(), tr("Rec. 709 luminance of the colour.").toStdString()));
        options.push_back(ChoiceOption("red", tr("Red").toStdString(), std::string()));
        options.push_back(ChoiceOption("green", tr("Green").toStdString(), std::string()));
        options.push_back(ChoiceOption("blue", tr("Blue").toStdString(), std::string()));
        options.push_back(ChoiceOption("alpha", tr("Alpha").toStdString(), std::string()));
        _channel = createChoice(tr("Channel"), kGeoDisplaceParamChannel, tr("The image channel that sets the displacement."), options,
                                (int)eGeoDisplaceChannelLuminance);
    }

    KnobStringPtr uvAttribute = createKnob<KnobString>(tr("UV Attribute").toStdString());
    uvAttribute->setName(kGeoDisplaceParamUVAttribute);
    uvAttribute->setHintToolTip(tr("The point or vertex attribute holding the texture coordinates the image is sampled at. "
                                   "A point sharing several vertex values takes their average.")
                                    .toStdString());
    uvAttribute->setDefaultValue("uv");
    uvAttribute->setAnimationEnabled(false);
    page->addKnob(uvAttribute);
    _uvAttribute = uvAttribute;

    KnobBoolPtr recomputeNormals = createKnob<KnobBool>(tr("Recompute Normals").toStdString());
    recomputeNormals->setName(kGeoDisplaceParamRecomputeNormals);
    recomputeNormals->setHintToolTip(tr("Computes N afresh from the displaced surface, on the points, vertices or primitives "
                                        "the input had it on. Off, the points keep the normals they were pushed along.")
                                         .toStdString());
    recomputeNormals->setDefaultValue(true);
    recomputeNormals->setAnimationEnabled(false);
    page->addKnob(recomputeNormals);
    _recomputeNormals = recomputeNormals;

    updateEnabled();
} // GeoDisplace::initializeKnobs

void
GeoDisplace::updateEnabled()
{
    const KnobChoicePtr displaceBy = _displaceBy.lock();
    const bool byImage = displaceBy && (displaceBy->getValue() == (int)eGeoDisplaceByImage);
    auto enable = [](const KnobIPtr& knob,
                     bool enabled) {
        if (knob) {
            knob->setAllDimensionsEnabled(enabled);
        }
    };
    enable(_noiseType.lock(), !byImage);
    enable(_fractal.lock(), !byImage);
    enable(_octaves.lock(), !byImage);
    enable(_lacunarity.lock(), !byImage);
    enable(_gain.lock(), !byImage);
    enable(_seed.lock(), !byImage);
    enable(_frequency.lock(), !byImage);
    enable(_noiseOffset.lock(), !byImage);
    enable(_evolution.lock(), !byImage);
    enable(_channel.lock(), byImage);
    enable(_uvAttribute.lock(), byImage);
}

void
GeoDisplace::onKnobsLoaded()
{
    updateEnabled();
}

bool
GeoDisplace::knobChanged(KnobI* k,
                         ValueChangedReasonEnum /*reason*/,
                         ViewSpec /*view*/,
                         double /*time*/,
                         bool /*originatedFromMainThread*/)
{
    const KnobChoicePtr displaceBy = _displaceBy.lock();
    if (displaceBy && (k == displaceBy.get())) {
        updateEnabled();

        return true;
    }

    return false;
}

StatusEnum
GeoDisplace::getPreferredMetadataAt(double /*time*/,
                                    ViewIdx /*view*/,
                                    NodeMetadata& metadata)
{
    // The image may change from frame to frame, and nothing else tells the geometry so.
    const KnobChoicePtr displaceBy = _displaceBy.lock();
    if (displaceBy && (displaceBy->getValue() == (int)eGeoDisplaceByImage) && getInput(1)) {
        metadata.setIsFrameVarying(true);
    }

    return eStatusOK;
}

GeoDisplaceParams
GeoDisplace::readParams(double time,
                        ViewIdx view) const
{
    GeoDisplaceParams p;
    auto number = [time, view](const KnobDoubleWPtr& weak,
                               double fallback) {
        const KnobDoublePtr knob = weak.lock();

        return knob ? knob->getValueAtTime(time, 0, view) : fallback;
    };
    auto vector = [time, view](const KnobDoubleWPtr& weak,
                               std::array<double, 3>* out) {
        if (const KnobDoublePtr knob = weak.lock()) {
            for (int i = 0; i < 3; ++i) {
                (*out)[(std::size_t)i] = knob->getValueAtTime(time, i, view);
            }
        }
    };
    if (const KnobStringPtr group = _group.lock()) {
        p.group = group->getValue();
    }
    if (const KnobChoicePtr displaceBy = _displaceBy.lock()) {
        p.displaceBy = displaceBy->getValue() == (int)eGeoDisplaceByImage ? eGeoDisplaceByImage : eGeoDisplaceByNoise;
    }
    p.amplitude = number(_amplitude, p.amplitude);
    p.offset = number(_offset, p.offset);
    if (const KnobChoicePtr noiseType = _noiseType.lock()) {
        p.noiseType = noiseType->getValue();
    }
    if (const KnobChoicePtr fractal = _fractal.lock()) {
        p.fractal = fractal->getValue();
    }
    if (const KnobIntPtr octaves = _octaves.lock()) {
        p.octaves = octaves->getValueAtTime(time, 0, view);
    }
    p.lacunarity = number(_lacunarity, p.lacunarity);
    p.gain = number(_gain, p.gain);
    if (const KnobIntPtr seed = _seed.lock()) {
        p.seed = (unsigned int)seed->getValueAtTime(time, 0, view);
    }
    vector(_frequency, &p.frequency);
    vector(_noiseOffset, &p.noiseOffset);
    p.evolution = number(_evolution, p.evolution);
    if (const KnobStringPtr uvAttribute = _uvAttribute.lock()) {
        p.uvAttribute = uvAttribute->getValue();
    }
    if (const KnobBoolPtr recomputeNormals = _recomputeNormals.lock()) {
        p.recomputeNormals = recomputeNormals->getValue();
    }

    return p;
} // GeoDisplace::readParams

bool
GeoDisplace::fetchImage(double time,
                        ViewIdx view,
                        GeoDisplaceImage* image,
                        std::string* errorMessage) const
{
    const EffectInstancePtr input = getInput(1);
    const NodePtr node = input ? input->getNode() : NodePtr();
    if (!node) {
        return false;
    }
    const KnobChoicePtr channelKnob = _channel.lock();
    const int channelValue = channelKnob ? channelKnob->getValue() : 0;
    const GeoDisplaceChannelEnum channel = ((channelValue >= 0) && (channelValue <= (int)eGeoDisplaceChannelAlpha))
        ? (GeoDisplaceChannelEnum)channelValue
        : eGeoDisplaceChannelLuminance;
    bool rendered = false;
    std::string failure;
    std::thread renderer([&]() {
        try {
            rendered = renderImageChannel(node, time, view, channel, image, &failure);
        } catch (const std::exception& e) {
            rendered = false;
            failure = e.what();
        } catch (...) {
            rendered = false;
            failure = "an unknown error occurred";
        }
    });
    renderer.join();
    if (!rendered && errorMessage) {
        *errorMessage = failure;
    }

    return rendered;
}

StatusEnum
GeoDisplace::renderGeometry(const GeoRenderActionArgs& args,
                            GeoDetailConstPtr* outputDetail,
                            std::string* errorMessage)
{
    const GeoDisplaceParams params = readParams(args.time, args.view);
    GeoDisplaceImage image;
    bool haveImage = false;
    if (params.displaceBy == eGeoDisplaceByImage) {
        std::string error;
        haveImage = fetchImage(args.time, args.view, &image, &error);
        if (!haveImage && !error.empty()) {
            if (errorMessage) {
                *errorMessage = error;
            }

            return eStatusFailed;
        }
    }

    return renderGeometryFromInput(args, 0, [&params, &image, haveImage](GeoDetailBuilder& detail, std::string* error) {
        std::string message;
        if (!displaceGeometry(detail, params, haveImage ? &image : nullptr, &message)) {
            if (error) {
                *error = message;
            }

            return eStatusFailed;
        }

        return eStatusOK; }, outputDetail, errorMessage);
}

NATRON_NAMESPACE_EXIT
