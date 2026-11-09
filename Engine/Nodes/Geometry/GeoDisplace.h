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

#ifndef Engine_Nodes_Geometry_GeoDisplace_h
#define Engine_Nodes_Geometry_GeoDisplace_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/NativeEffectBase.h"

#define PLUGINID_NATRON_GEODISPLACE "GeoDisplace"

#define kGeoDisplaceParamGroup "group"
#define kGeoDisplaceParamDisplaceBy "displaceBy"
#define kGeoDisplaceParamAmplitude "amplitude"
#define kGeoDisplaceParamOffset "offset"
#define kGeoDisplaceParamNoiseType "noiseType"
#define kGeoDisplaceParamFractal "fractal"
#define kGeoDisplaceParamOctaves "octaves"
#define kGeoDisplaceParamLacunarity "lacunarity"
#define kGeoDisplaceParamGain "gain"
#define kGeoDisplaceParamSeed "seed"
#define kGeoDisplaceParamFrequency "frequency"
#define kGeoDisplaceParamNoiseOffset "noiseOffset"
#define kGeoDisplaceParamEvolution "evolution"
#define kGeoDisplaceParamChannel "channel"
#define kGeoDisplaceParamUVAttribute "uvAttribute"
#define kGeoDisplaceParamRecomputeNormals "recomputeNormals"

NATRON_NAMESPACE_ENTER

enum GeoDisplaceByEnum {
    eGeoDisplaceByNoise = 0,
    eGeoDisplaceByImage,
};

enum GeoDisplaceChannelEnum {
    eGeoDisplaceChannelLuminance = 0,
    eGeoDisplaceChannelRed,
    eGeoDisplaceChannelGreen,
    eGeoDisplaceChannelBlue,
    eGeoDisplaceChannelAlpha,
};

/**
 * @brief One channel of an image as the displacement samples it: width x height values, rows
 * bottom to top, uv (0, 0) at the bottom-left corner of the first pixel and (1, 1) at the
 * top-right corner of the last.
 **/
struct GeoDisplaceImage {
    int width = 0;
    int height = 0;
    std::vector<float> values;

    /** @brief Bilinear, the edge pixels extending past 0 and 1. Zero for an empty image. */
    double sample(double u,
                  double v) const;
};

struct GeoDisplaceParams {
    // A point group pattern (see geoSelectElements()); empty for every point.
    std::string group;
    GeoDisplaceByEnum displaceBy = eGeoDisplaceByNoise;
    double amplitude = 1.;
    double offset = 0.;
    // NoiseCore's type and fractal numbers.
    int noiseType = 0;
    int fractal = 0;
    int octaves = 4;
    double lacunarity = 2.;
    double gain = 0.5;
    unsigned int seed = 0;
    std::array<double, 3> frequency = { 1., 1., 1. };
    std::array<double, 3> noiseOffset = { 0., 0., 0. };
    double evolution = 0.;
    std::string uvAttribute = "uv";
    bool recomputeNormals = true;
};

/**
 * @brief Pushes each point the group selects along its normal by amplitude x value + offset,
 * value being the noise at the point's position (-1 to 1) or the image sampled at its uv (a point
 * uv, else the average of its vertices'). Point normals are used; with N only on the vertices or
 * the primitives, a point goes along the mean of the unit normals of its vertices or of the
 * primitives using it, and that N is left as it is; with no N at all, N is computed on the points
 * first. With recomputeNormals, N is then computed from the displaced polygons, on the element
 * class that held it in the input (points when it had none).
 *
 * Fails, with *errorMessage set, for a malformed group pattern, for the image with no image
 * given, or for the image when the detail has no uvAttribute.
 **/
bool displaceGeometry(GeoDetailBuilder& detail,
                      const GeoDisplaceParams& params,
                      const GeoDisplaceImage* image,
                      std::string* errorMessage);

/**
 * @brief Pushes points along their normals by noise or by an image.
 **/
class GeoDisplace
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Source", .kind = eDataKindGeometry },
        InputSpec { .label = "Image", .kind = eDataKindImage, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_GEODISPLACE,
        .label = "GeoDisplace",
        .description = "Push points along their normals (N) by a noise evaluated at their position, or by the Image "
                       "input sampled at their texture coordinates (uv). Normals are computed first when the input "
                       "has none, and can be recomputed from the displaced surface afterwards.",
        .grouping = PLUGIN_GROUP_3D,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindGeometry,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new GeoDisplace(node);
    }

    explicit GeoDisplace(NodePtr node);

    virtual ~GeoDisplace();

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum getPreferredMetadataAt(double time,
                                              ViewIdx view,
                                              NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum renderGeometry(const GeoRenderActionArgs& args,
                                      GeoDetailConstPtr* outputDetail,
                                      std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    GeoDisplaceParams readParams(double time,
                                 ViewIdx view) const;
    bool fetchImage(double time,
                    ViewIdx view,
                    GeoDisplaceImage* image,
                    std::string* errorMessage) const;
    void updateEnabled();

    KnobStringWPtr _group;
    KnobChoiceWPtr _displaceBy;
    KnobDoubleWPtr _amplitude;
    KnobDoubleWPtr _offset;
    KnobChoiceWPtr _noiseType;
    KnobChoiceWPtr _fractal;
    KnobIntWPtr _octaves;
    KnobDoubleWPtr _lacunarity;
    KnobDoubleWPtr _gain;
    KnobIntWPtr _seed;
    KnobDoubleWPtr _frequency;
    KnobDoubleWPtr _noiseOffset;
    KnobDoubleWPtr _evolution;
    KnobChoiceWPtr _channel;
    KnobStringWPtr _uvAttribute;
    KnobBoolWPtr _recomputeNormals;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoDisplace_h
