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

#ifndef Engine_Nodes_Color_ColorLookup_h
#define Engine_Nodes_Color_ColorLookup_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/Nodes/Image/CurveSnapshot.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_COLORLOOKUP "ColorLookup"
#define PLUGIN_MAJOR_NATRON_COLORLOOKUP 1

#define kColorLookupParamHasBackgroundInteract "hasBackgroundInteract"
#define kColorLookupParamLookupTable "lookupTable"
#define kColorLookupParamShowRamp "showRamp"
#define kColorLookupParamDisplay "backgroundDisplay"
#define kColorLookupParamUpdateHistogram "updateHistogram"
#define kColorLookupParamSource "source"
#define kColorLookupParamTarget "target"
#define kColorLookupParamSetMaster "setMaster"
#define kColorLookupParamSetRGB "setRGB"
#define kColorLookupParamSetRGBA "setRGBA"
#define kColorLookupParamSetA "setA"
#define kColorLookupParamMasterCurveMode "masterCurveMode"
#define kColorLookupParamClampBlack "clampBlack"
#define kColorLookupParamClampWhite "clampWhite"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kColorLookupInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

/**
 * @brief ColorLookup: per-channel curves of a parametric knob. The lookup table has five curves,
 * in this order: master, red, green, blue and alpha. How the master curve combines with the
 * channel curves depends on masterCurveMode:
 *  - standard:         c' = red(c) + master(c) - c, likewise for green and blue
 *  - weightedstandard: the three standard results, each also pulled towards the other two
 *                      channels' results by RawTherapee's weighted triangle
 *  - filmlike:         the master curve applied to the largest and smallest channels and the
 *                      middle one placed between them, times each channel's own curve ratio
 *  - luminance:        the master curve applied to the luminance, its gain times each channel's
 *                      own curve
 * The alpha curve alone gives alpha. Inside [0, 1] a 1024-entry table per curve is interpolated,
 * outside it the curve is evaluated directly. Knob names, defaults and maths are the
 * openfx-misc ColorLookupPlugin's; it registers under that plugin's ID one major above.
 **/
struct ColorLookupOp {
    enum CurveEnum {
        eCurveMaster = 0,
        eCurveRed,
        eCurveGreen,
        eCurveBlue,
        eCurveAlpha,
        eCurveCount
    };

    enum MasterCurveModeEnum {
        eMasterCurveModeStandard = 0,
        eMasterCurveModeWeightedStandard,
        eMasterCurveModeFilmLike,
        eMasterCurveModeLuminance
    };

    // The curves and the tables sampled from them, shared by every Params baked from the same
    // curves, clamps and kind of master curve mode.
    struct Tables;

    struct Params {
        MasterCurveModeEnum mode;
        ColorMath::LuminanceMathEnum luminanceMath;
        bool clampBlack;
        bool clampWhite;
        std::shared_ptr<const Tables> tables;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_COLORLOOKUP,
        .label = "ColorLookup",
        .description = "Apply a parametric lookup curve with the possibility to adjust each channel separately.\n"
                       "The master curve is combined with the red, green and blue curves, but not with the alpha curve.\n"
                       "Different algorithms are available when applying the master curve, which are selectable using the \"Master Curve Mode\" parameter.\n"
                       "\n"
                       "Note that you can easily do color remapping by setting Source and Target colors and clicking \"Set RGB\" or \"Set RGBA\" below.\n"
                       "This will add control points on the curve to match the target from the source. You can add as many point as you like.\n"
                       "This is very useful for matching color of one shot to another, or adding custom colors to a black and white ramp.\n"
                       "\n"
                       "Optionally, the RGB histogram or a color ramp can be displayed in the background of the lookup curves.\n"
                       "\n"
                       "See also: https://web.archive.org/web/20221208230906/http://opticalenquiry.com/nuke/index.php?title=ColorLookup",
        .grouping = PLUGIN_GROUP_COLOR,
        .majorVersion = PLUGIN_MAJOR_NATRON_COLORLOOKUP,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/ColorLookup.png",
        .inputs = kColorLookupInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::Premult, Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::color(true, true, true, true),
        .op = OpKind::Point,
    };

    Params bake(const KnobReader& reader) const;

    bool isIdentity(const Params& params) const;

    void row(const Params& params,
             const RowIO& io) const;

    GpuOpRef gpu(const Params& params) const;

    KnobParametricWPtr lookupTable;
    KnobChoiceWPtr masterCurveMode;
    KnobChoiceWPtr luminanceMath;
    KnobBoolWPtr clampBlack;
    KnobBoolWPtr clampWhite;

private:
    std::shared_ptr<const Tables> bakeTables(MasterCurveModeEnum mode,
                                             bool black,
                                             bool white) const;

    // One cache per kind of master curve mode (standard or not) and pair of clamps, which the
    // tables are sampled through.
    CurveTablesCache<Tables> _tables[8];
};

static_assert(PointOp<ColorLookupOp>);

class ColorLookup
    : public ImageNode<ColorLookupOp> {
public:
    using CurveEnum = ColorLookupOp::CurveEnum;
    using MasterCurveModeEnum = ColorLookupOp::MasterCurveModeEnum;
    using enum ColorLookupOp::CurveEnum;
    using enum ColorLookupOp::MasterCurveModeEnum;

    enum DisplayEnum {
        eDisplayNone = 0,
        eDisplayColorRamp,
        eDisplayHistogram
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new ColorLookup(node);
    }

    explicit ColorLookup(NodePtr node);

    virtual ~ColorLookup();

    /**
     * @brief Counts the red, green and blue values of the source at `time` into 256 bins per
     * channel across [0, 1], the first and last bins taking everything below and above it, and
     * asks the curve editor to repaint. Pressing Update Histogram calls it, on the GUI thread.
     **/
    void updateHistogram(double time, ViewIdx view);

    /**
     * @brief The last histogram as 3 x 256 counts (red bins, then green, then blue), empty when
     * none is kept. It is not persistent and is dropped when the display leaves the histogram.
     **/
    std::vector<unsigned long long> getHistogramCounts() const WARN_UNUSED_RETURN;

    /**
     * @brief The Params the op renders with at (time, view).
     **/
    ColorLookupOp::Params bakeColorLookup(double time,
                                          ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The op's CPU kernel over one row.
     **/
    void colorLookupRow(const ColorLookupOp::Params& params,
                        const RowIO& io) const;

private:
    struct BackgroundState;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    /**
     * @brief Shows or hides backgroundDisplay and updateHistogram from hasBackgroundInteract, and
     * enables updateHistogram only while the display is the histogram.
     **/
    void refreshBackgroundControls();

    void installBackgroundPainter();

    void dropHistogram();

    void addControlPointFromButton(KnobI* button, double time, ViewIdx view);

    KnobBoolWPtr _hasBackgroundInteract;
    KnobChoiceWPtr _display;
    KnobButtonWPtr _updateHistogram;
    KnobColorWPtr _source;
    KnobColorWPtr _target;
    KnobButtonWPtr _setMaster;
    KnobButtonWPtr _setRGB;
    KnobButtonWPtr _setRGBA;
    KnobButtonWPtr _setA;
    std::shared_ptr<BackgroundState> _background;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Color_ColorLookup_h
