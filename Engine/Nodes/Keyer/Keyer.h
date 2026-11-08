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

#ifndef Engine_Nodes_Keyer_Keyer_h
#define Engine_Nodes_Keyer_Keyer_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_KEYER "Keyer"
#define PLUGIN_MAJOR_NATRON_KEYER 1

#define kKeyerParamKeySource "keySource"
#define kKeyerParamLuminanceMath "luminanceMath"
#define kKeyerParamRange "range"

// Input indices, in the order scripts connect by.
#define kKeyerInputSource 0
#define kKeyerInputMask 1

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kKeyerInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief A luminance or single-channel keyer that outputs an alpha only. The key value is the
 * source's luminance (luminanceMath, by default the working space's luma weights) or one of its
 * red, green and blue channels. The alpha is the trapezoid of that value over the range knob's
 * points A <= B <= C <= D (keyerRangePoints()): 0 up to A, rising to 1 at B, 1 up to C and
 * falling to 0 at D. The range knob holds a centre and four offsets and is shown as the trapezoid
 * widget HSVKeyer's ranges use. RGB passes through untouched and nothing is premultiplied; the
 * host mask and mix blend the new alpha with the source's. The output is RGBA whatever the source.
 **/
struct KeyerOp {
    enum KeySourceEnum {
        eKeySourceLuminance = 0,
        eKeySourceRed,
        eKeySourceGreen,
        eKeySourceBlue
    };

    // The trapezoid is ordered and every weight is a float, as the GPU kernel takes them. A
    // channel source reads its channel alone, so an infinite value in another channel cannot
    // turn the key into NaN through a zero weight.
    struct Params {
        KeySourceEnum keySource = eKeySourceLuminance;
        int channel = -1;
        bool lumaMax = false;
        float weights[3] = { 0.f, 0.f, 0.f };
        float range[4] = { 0.f, 0.1f, 0.9f, 1.f };
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_KEYER,
        .label = "Keyer",
        .description = "Pull an alpha from the luminance of the source or from one of its red, green and blue channels.\n"
                       "\n"
                       "The alpha is a trapezoid over the key value, drawn and edited on the Range strip: "
                       "0 below the outer low point, rising linearly to 1 at the inner low point, 1 up to the inner high point, "
                       "falling linearly to 0 at the outer high point and 0 above it. "
                       "The range is a centre and four offsets: the inner offsets set the plateau around the centre and "
                       "the outer offsets the ramps outside it, so the points are always in order. An outer offset of 0 is a hard edge.\n"
                       "\n"
                       "With the Luminance key source the key value is the luminance of RGB; by default it uses the luma weights "
                       "of the project's working space. With Red, Green or Blue it is that channel's value.\n"
                       "\n"
                       "RGB passes through untouched and is not premultiplied. Values above 1 and below 0 are keyed "
                       "as they are, with no clamp.",
        .grouping = PLUGIN_GROUP_KEYER,
        .majorVersion = PLUGIN_MAJOR_NATRON_KEYER,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/Keyer.png",
        .inputs = kKeyerInputs,
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

    // The key value of colour rgb, in float, one rounding per statement as the kernel computes it.
    static float keyValue(const Params& params,
                          const float* rgb);

    KnobHolder* holder = nullptr;
    KnobChoiceWPtr keySource;
    KnobChoiceWPtr luminanceMath;
    KnobDoubleWPtr range;
};

static_assert(PointOp<KeyerOp>);

class Keyer
    : public ImageNode<KeyerOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Keyer(node);
    }

    explicit Keyer(NodePtr node);

    virtual ~Keyer();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    /**
     * @brief The Params the op renders with at (time, view).
     **/
    KeyerOp::Params bakeKeyer(double time,
                              ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The op's CPU kernel over one row.
     **/
    void keyerRow(const KeyerOp::Params& params,
                  const RowIO& io) const;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    // The output is RGBA whatever the source: the key is its alpha.
    virtual StatusEnum getPreferredMetadataAt(double time,
                                              ViewIdx view,
                                              NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // Shows the knobs the key source uses and writes it into the sublabel.
    void refreshKeySourceDependents();
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Keyer_Keyer_h
