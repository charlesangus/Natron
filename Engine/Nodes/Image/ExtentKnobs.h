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

#ifndef Engine_Nodes_Image_ExtentKnobs_h
#define Engine_Nodes_Image_ExtentKnobs_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Global/Enums.h"

#include "Engine/EngineFwd.h"
#include "Engine/RectD.h"
#include "Engine/ViewIdx.h"

#define kNativeGeneratorParamExtent "extent"
#define kNativeGeneratorParamRecenter "recenter"
#define kNativeGeneratorParamReformat "reformat"
#define kNativeGeneratorParamBottomLeft "bottomLeft"
#define kNativeGeneratorParamSize "size"
#define kNativeGeneratorParamInteractive "interactive"
#define kNativeGeneratorParamHiDPI "hidpi"
#define kNativeGeneratorParamFrameRange "frameRange"
#define kNativeGeneratorParamRectangleEnable "rectangleInteractEnable"

#define kNativeGeneratorExtentFormat "format"
#define kNativeGeneratorExtentSize "size"
#define kNativeGeneratorExtentProject "project"
#define kNativeGeneratorExtentDefault "default"

NATRON_NAMESPACE_ENTER

/**
 * @brief What differs between the nodes that share the extent knobs.
 **/
struct ExtentPolicy {
    /// The extent the node starts with, and the one it reports when its knobs are gone.
    int defaultExtent;

    /// Whether the extent row carries a Reformat toggle, shown only in Size extent. A node whose
    /// reformat behaviour is its own leaves this off and declares the knob itself.
    bool reformatToggle;

    /// Whether the rectangle overlay is on exactly while the extent is Size. Otherwise the
    /// overlay starts on and its owner switches it.
    bool overlayFollowsExtent;

    /// Whether Center aims at the source's region of definition (the project window without a
    /// source) rather than always at the project window.
    bool recenterOnSource;
};

/**
 * @brief The extent knobs shared by the generators and Crop, in the openfx-misc order: extent,
 * Center, the host-managed format choice with its secret size and pixel aspect ratio, bottomLeft
 * and size with normalised defaults, interactive, hidpi and frameRange, plus the knob that drives
 * the rectangle overlay in Size extent.
 *
 * An owning effect creates the knobs from initializeKnobs(), forwards knobChanged() and
 * onKnobsLoaded(), and keeps this object as a member for as long as it lives.
 **/
class ExtentKnobs {
public:
    enum ExtentEnum {
        eExtentFormat = 0,
        eExtentSize,
        eExtentProject,
        eExtentDefault
    };

    ExtentKnobs(EffectInstance* effect,
                const ExtentPolicy& policy);

    /**
     * @brief Declares the knobs on page. The rectangle overlay's enable knob is declared first
     * unless the policy has the overlay follow the extent, in which case it comes last.
     **/
    void createKnobs(const KnobPagePtr& page);

    /**
     * @brief Applies the visibility for the current extent and registers the rectangle overlay.
     * Call once every knob of the node is declared.
     **/
    void finishKnobs();

    ExtentEnum getExtent() const WARN_UNUSED_RETURN;

    /**
     * @brief Shows the knobs the current extent uses and hides the others.
     **/
    void updateVisibility();

    /**
     * @brief Handles a change of the extent choice or a press of Center. Returns false when k is
     * neither.
     **/
    bool onKnobChanged(KnobI* k,
                       ValueChangedReasonEnum reason,
                       double time);

    /**
     * @brief The project's default format in canonical coordinates, and its pixel aspect ratio.
     **/
    RectD getProjectExtentRect(double* par = NULL) const;

    /**
     * @brief The region of definition the knobs give at (time, view), as the openfx-misc
     * GeneratorPlugin::getRegionOfDefinition() computes it. False for Default extent, which has
     * none of its own.
     **/
    bool getExtentRegionOfDefinition(double time,
                                     ViewIdx view,
                                     RectD* rod) const WARN_UNUSED_RETURN;

    KnobChoicePtr getExtentKnob() const;
    KnobBoolPtr getReformatKnob() const;
    KnobIntPtr getFormatSizeKnob() const;
    KnobDoublePtr getFormatParKnob() const;
    KnobDoublePtr getBottomLeftKnob() const;
    KnobDoublePtr getSizeKnob() const;
    KnobBoolPtr getRectangleEnableKnob() const;
    KnobIntPtr getFrameRangeKnob() const;

private:
    void createRectangleEnable(const KnobPagePtr& page);

    void recenter(double time, ViewIdx view);

    EffectInstance* _effect;
    ExtentPolicy _policy;
    KnobChoiceWPtr _extent;
    KnobButtonWPtr _recenter;
    KnobBoolWPtr _reformat;
    KnobChoiceWPtr _format;
    KnobIntWPtr _formatSize;
    KnobDoubleWPtr _formatPar;
    KnobDoubleWPtr _bottomLeft;
    KnobDoubleWPtr _size;
    KnobBoolWPtr _interactive;
    KnobBoolWPtr _hiDPI;
    KnobIntWPtr _frameRange;
    KnobBoolWPtr _rectangleEnable;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Image_ExtentKnobs_h
