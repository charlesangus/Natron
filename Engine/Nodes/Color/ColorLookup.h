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
#include "Engine/Nodes/Image/NativeImageEffect.h"

#define PLUGINID_NATRON_COLORLOOKUP "net.sf.openfx.ColorLookupPlugin"
#define PLUGIN_MAJOR_NATRON_COLORLOOKUP 2

#define kColorLookupParamHasBackgroundInteract "hasBackgroundInteract"
#define kColorLookupParamRange "range"
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
 * The alpha curve alone gives alpha. Inside `range` a 1024-entry table per curve is interpolated,
 * outside it the curve is evaluated directly. Knob names, defaults and maths are the
 * openfx-misc ColorLookupPlugin's; it registers under that plugin's ID one major above.
 **/
class ColorLookup
    : public NativeImageEffect {
public:
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

    virtual bool isPointOp() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    /**
     * @brief Counts the red, green and blue values of the source at `time` into 256 bins per
     * channel across `range`, the first and last bins taking everything below and above it, and
     * asks the curve editor to repaint. Pressing Update Histogram calls it, on the GUI thread.
     **/
    void updateHistogram(double time, ViewIdx view);

    /**
     * @brief The last histogram as 3 x 256 counts (red bins, then green, then blue), empty when
     * none is kept. It is not persistent and is dropped when the display leaves the histogram.
     **/
    std::vector<unsigned long long> getHistogramCounts() const WARN_UNUSED_RETURN;

protected:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    struct BackgroundState;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual PixelKernelPtr makeKernel(const KernelContext& context) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief Shows or hides backgroundDisplay and updateHistogram from hasBackgroundInteract, and
     * enables updateHistogram only while the display is the histogram.
     **/
    void refreshBackgroundControls();

    void installBackgroundPainter();

    void dropHistogram();

    void addControlPointFromButton(KnobI* button, double time, ViewIdx view);

    KnobBoolWPtr _hasBackgroundInteract;
    KnobDoubleWPtr _range;
    KnobParametricWPtr _lookupTable;
    KnobChoiceWPtr _display;
    KnobButtonWPtr _updateHistogram;
    KnobColorWPtr _source;
    KnobColorWPtr _target;
    KnobButtonWPtr _setMaster;
    KnobButtonWPtr _setRGB;
    KnobButtonWPtr _setRGBA;
    KnobButtonWPtr _setA;
    KnobChoiceWPtr _masterCurveMode;
    KnobChoiceWPtr _luminanceMath;
    KnobBoolWPtr _clampBlack;
    KnobBoolWPtr _clampWhite;
    std::shared_ptr<BackgroundState> _background;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Color_ColorLookup_h
