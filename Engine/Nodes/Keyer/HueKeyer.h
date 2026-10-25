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

#ifndef Engine_Nodes_Keyer_HueKeyer_h
#define Engine_Nodes_Keyer_HueKeyer_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_HUEKEYER "HueKeyer"
#define PLUGIN_MAJOR_NATRON_HUEKEYER 1

#define kHueKeyerParamHueLow "hueLow"
#define kHueKeyerParamHueLowFull "hueLowFull"
#define kHueKeyerParamHueHighFull "hueHighFull"
#define kHueKeyerParamHueHigh "hueHigh"
#define kHueKeyerParamSatLow "satLow"
#define kHueKeyerParamSatLowFull "satLowFull"
#define kHueKeyerParamSatHighFull "satHighFull"
#define kHueKeyerParamSatHigh "satHigh"
#define kHueKeyerParamInvert "invert"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kHueKeyerInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief Keys by hue and saturation: alpha is the product of a hue trapezoid, in turns and
 * wrapping across red, and a saturation trapezoid, inverted on request. Saturation is chroma over
 * the largest of R, G and B, 0 when that is not positive, and is not clamped. Only alpha is
 * written; RGB passes through, and the output is RGBA whatever the source.
 **/
struct HueKeyerOp {
    /**
     * @brief The hue trapezoid starts at hueLow and reaches 1 hueRise later, stays 1 until
     * hueFull and falls to 0 at hueFall, all going up the hue circle from hueLow. The saturation
     * trapezoid's points are in order.
     **/
    struct Params {
        float hueLow = 0.f;
        float hueRise = 0.f;
        float hueFull = 0.f;
        float hueFall = 0.f;
        float satLow = 0.f;
        float satLowFull = 0.f;
        float satHighFull = 0.f;
        float satHigh = 0.f;
        bool invert = false;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_HUEKEYER,
        .label = "HueKeyer",
        .description = "Compute a key from the hue and saturation of the source.\n"
                       "\n"
                       "The key is the product of two trapezoids: one over the hue, in turns from red at 0, which "
                       "rises from Hue Low to Hue Low Full, is 1 up to Hue High Full and falls to 0 at Hue High, going "
                       "up the hue circle and wrapping across red; and one over the saturation, from Saturation Low to "
                       "Saturation High likewise. Saturation is chroma divided by the largest of R, G and B and is not "
                       "clamped.\n"
                       "\n"
                       "Only the alpha channel is written; the colour passes through.",
        .grouping = PLUGIN_GROUP_KEYER,
        .majorVersion = PLUGIN_MAJOR_NATRON_HUEKEYER,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/HueKeyer.png",
        .inputs = kHueKeyerInputs,
    };
    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::color(false, false, false, true),
        .op = OpKind::Point,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);
    Params bake(const KnobReader& reader) const;
    bool isIdentity(const Params& params) const;
    void row(const Params& params,
             const RowIO& io) const;
    GpuOpRef gpu(const Params& params) const;

    // The key of colour rgb.
    static float key(const Params& params,
                     const float* rgb);

    KnobDoubleWPtr hueLow;
    KnobDoubleWPtr hueLowFull;
    KnobDoubleWPtr hueHighFull;
    KnobDoubleWPtr hueHigh;
    KnobDoubleWPtr satLow;
    KnobDoubleWPtr satLowFull;
    KnobDoubleWPtr satHighFull;
    KnobDoubleWPtr satHigh;
    KnobBoolWPtr invert;
};

static_assert(PointOp<HueKeyerOp>);

class HueKeyer
    : public ImageNode<HueKeyerOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new HueKeyer(node);
    }

    explicit HueKeyer(NodePtr node);

    virtual ~HueKeyer();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    /**
     * @brief The Params the op renders with at (time, view).
     **/
    HueKeyerOp::Params bakeHueKeyer(double time,
                                    ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The op's CPU kernel over one row.
     **/
    void hueKeyerRow(const HueKeyerOp::Params& params,
                     const RowIO& io) const;

private:
    // The output is RGBA whatever the source: the key is its alpha.
    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Keyer_HueKeyer_h
