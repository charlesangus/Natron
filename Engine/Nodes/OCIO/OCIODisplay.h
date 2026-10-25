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

#ifndef Engine_Nodes_OCIO_OCIODisplay_h
#define Engine_Nodes_OCIO_OCIODisplay_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/OCIO/OcioNodeBase.h"

#define PLUGINID_NATRON_OCIODISPLAY "OCIODisplay"
#define PLUGIN_MAJOR_NATRON_OCIODISPLAY 1

#define kOCIODisplayParamInputSpace "in_colorspace"
#define kOCIODisplayParamDisplay "display"
#define kOCIODisplayParamView "view"
#define kOCIODisplayParamGain "gain"
#define kOCIODisplayParamGamma "gamma"
#define kOCIODisplayParamChannel "channel_selector"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kOCIODisplayInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief OCIODisplay: renders RGB as a display of the project's config shows it, the way the
 * viewer does: gain on the input, the channel view, the display and view transform from the input
 * space with the Context variables, then gamma. Alpha is left as it is.
 **/
struct OCIODisplayOp {
    enum ChannelEnum {
        eChannelRGB = 0,
        eChannelR,
        eChannelG,
        eChannelB,
        eChannelA,
        eChannelLuminance,
    };

    struct Params {
        OcioProcessor ocio;
        float gain;
        double gamma;
        ChannelEnum channel;
        float luma[3];
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_OCIODISPLAY,
        .label = "OCIODisplay",
        .description = "Apply an OpenColorIO display and view transform of the project's config to the RGB "
                       "channels, as the viewer does: gain before the transform, gamma after it, and an optional "
                       "single-channel or luminance view. Alpha is left as it is.",
        .grouping = PLUGIN_GROUP_COLOR "/OCIO",
        .majorVersion = PLUGIN_MAJOR_NATRON_OCIODISPLAY,
        .minorVersion = 0,
        .inputs = kOCIODisplayInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::Premult, Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::color(true, true, true, false),
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
     * @brief Fills the display menu, and the view menu with the views of the chosen display.
     **/
    void refreshDisplayMenus();

    /**
     * @brief The display changed: fills the view menu again, taking the display's default view
     * when the chosen view is not one of its views.
     **/
    void displayChanged();

    std::string chosenDisplay() const;
    std::string chosenView() const;

    OcioNodeKnobs ocio;
    KnobChoiceWPtr display;
    KnobChoiceWPtr view;
    KnobDoubleWPtr gain;
    KnobDoubleWPtr gamma;
    KnobChoiceWPtr channel;
};

static_assert(PointOp<OCIODisplayOp>);

class OCIODisplay
    : public OcioNode<OCIODisplayOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new OCIODisplay(node);
    }

    explicit OCIODisplay(NodePtr node);

    virtual ~OCIODisplay();

    /**
     * @brief The Params the op renders with at (time, view).
     **/
    OCIODisplayOp::Params bakeDisplay(double time,
                                      ViewIdx view) const WARN_UNUSED_RETURN;

private:
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL WARN_UNUSED_RETURN;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_OCIO_OCIODisplay_h
