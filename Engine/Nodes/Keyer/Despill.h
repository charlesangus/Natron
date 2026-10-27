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

#ifndef Engine_Nodes_Keyer_Despill_h
#define Engine_Nodes_Keyer_Despill_h

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

#define PLUGINID_NATRON_DESPILL "Despill"
#define PLUGIN_MAJOR_NATRON_DESPILL 1

#define kDespillParamScreenColor "screenColor"
#define kDespillParamLimit "limit"
#define kDespillParamLimitWeights "limitWeights"
#define kDespillParamExpand "expand"
#define kDespillParamStrength "strength"
#define kDespillParamReplaceColor "replaceColor"
#define kDespillParamLumaRestore "lumaRestore"
#define kDespillParamSpillMap "spillMap"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kDespillInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief Removes the screen colour's spill from the source.
 *
 * The source is rotated about the grey axis into the screen's hue frame, where the screen's hue
 * is green's: there the spill is the rotated green less a limit taken from the rotated red and
 * blue, which for a green screen are the source's own channels. A positive spill takes a
 * multiple of the screen's chroma direction out of the pixel, sized so that a colour of the
 * screen's hue drops to its limit, and adds back the replacement colour's chroma and, by luma
 * restore, the luminance it took out.
 **/
struct DespillOp {
    enum LimitEnum {
        eLimitAverage = 0,
        eLimitMax,
        eLimitMin,
        eLimitCustom,
        eLimitCount
    };

    enum SpillMapEnum {
        eSpillMapNone = 0,
        eSpillMapAlpha,
        eSpillMapCount
    };

    // How the limit combines the two other channels: a weighted sum, or their largest or
    // smallest.
    enum LimitKindEnum {
        eLimitKindWeighted = 0,
        eLimitKindMax,
        eLimitKindMin
    };

    /**
     * @brief rot is the hue-frame rotation, row-major, applied to the source's RGB. The spill of
     * a pixel is max(g' - expandScale·limit(r', b'), 0) times spillScale; the pixel moves by its
     * spill times strength along move.
     **/
    struct Params {
        bool hasAxis = false;
        bool spillMapAlpha = false;
        int limitKind = eLimitKindWeighted;
        float rot[9] = { 1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f };
        float weights[2] = { 0.5f, 0.5f };
        float expandScale = 1.f;
        float spillScale = 1.f;
        float strength = 1.f;
        float move[3] = { 0.f, 0.f, 0.f };
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_DESPILL,
        .label = "Despill",
        .description = "Remove the spill of a screen colour from the source.\n"
                       "\n"
                       "The screen colour can be any colour. The source is rotated about the grey axis so that the "
                       "screen's hue sits on green, and the spill is the rotated green less a limit taken from the two "
                       "other rotated channels; for a green screen those are red and blue, and for a blue screen green "
                       "and red. The spill is taken out along the screen's chroma, so a pixel of the screen's hue drops "
                       "to its limit and a pixel of grey plus some screen colour drops to the grey. Setting the screen "
                       "colour to magenta, cyan, yellow, red or blue with the Max limit suppresses that colour as "
                       "ColorSuppress did.\n"
                       "\n"
                       "The replacement colour's chroma is added back in proportion to the spill, and Luma Restore adds "
                       "back the luminance that was taken out, in the project's working space. Nothing is clamped. A grey "
                       "screen colour has no spill axis and leaves the image as it is.",
        .grouping = PLUGIN_GROUP_KEYER,
        .majorVersion = PLUGIN_MAJOR_NATRON_DESPILL,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/Despill.png",
        .inputs = kDespillInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::Premult, Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::color(true, true, true, true),
        .op = OpKind::Point,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);
    Params bake(const KnobReader& reader) const;
    bool isIdentity(const Params& params) const;
    void row(const Params& params,
             const RowIO& io) const;
    GpuOpRef gpu(const Params& params) const;

    /**
     * @brief The spill map of the colour rgb, 1 for the screen colour's chroma direction and 0
     * where there is no spill.
     **/
    static float spill(const Params& params,
                       const float* rgb);

    /**
     * @brief Despills rgb in place and returns its spill map.
     **/
    static float apply(const Params& params,
                       float* rgb);

    // The project whose working space gives the luma weights.
    KnobHolder* holder = nullptr;
    KnobColorWPtr screenColor;
    KnobChoiceWPtr limit;
    KnobDoubleWPtr limitWeights;
    KnobDoubleWPtr expand;
    KnobDoubleWPtr strength;
    KnobColorWPtr replaceColor;
    KnobDoubleWPtr lumaRestore;
    KnobChoiceWPtr spillMap;
};

static_assert(PointOp<DespillOp>);

class Despill
    : public ImageNode<DespillOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Despill(node);
    }

    explicit Despill(NodePtr node);

    virtual ~Despill();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    /**
     * @brief The Params the op renders with at (time, view).
     **/
    DespillOp::Params bakeDespill(double time,
                                  ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The op's CPU kernel over one row.
     **/
    void despillRow(const DespillOp::Params& params,
                    const RowIO& io) const;

private:
    // With the spill map written to alpha the output is RGBA whatever the source.
    virtual StatusEnum getPreferredMetadataAt(double time,
                                              ViewIdx view,
                                              NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Keyer_Despill_h
