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

#ifndef Engine_Nodes_Transform_Crop_h
#define Engine_Nodes_Transform_Crop_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Image/NativeImageEffect.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/RectD.h"

#define PLUGINID_NATRON_CROP "net.sf.openfx.CropPlugin"
#define PLUGIN_MAJOR_NATRON_CROP 2

#define kCropParamSoftness "softness"
#define kCropParamReformat "reformat"
#define kCropParamIntersect "intersect"
#define kCropParamBlackOutside "blackOutside"

NATRON_NAMESPACE_ENTER

/**
 * @brief Crop: keeps the pixels inside a rectangle, optionally fading them to black at its edges
 * (softness), translating the rectangle to the origin and making it the output format
 * (reformat), clipping it to the source's region of definition (intersect) and adding a black
 * border around it (blackOutside). The rectangle comes from the same extent knobs as a generator
 * (Format, Size, Project or Default, with Size as the initial extent), shown with the host
 * rectangle overlay while it is editable. Knob names, defaults, region of definition, clip
 * preferences and per-pixel arithmetic are the openfx-misc CropPlugin's, except that reformat
 * there sets the output format for the Format and Project extents only; it registers under that
 * plug-in's ID one major above.
 *
 * Every channel of every requested plane is cropped; the unprocessed-channel copy stays with
 * the host.
 **/
class Crop
    : public NativeImageEffect {
public:
    typedef ExtentKnobs::ExtentEnum ExtentEnum;
    static constexpr ExtentEnum eExtentFormat = ExtentKnobs::eExtentFormat;
    static constexpr ExtentEnum eExtentSize = ExtentKnobs::eExtentSize;
    static constexpr ExtentEnum eExtentProject = ExtentKnobs::eExtentProject;
    static constexpr ExtentEnum eExtentDefault = ExtentKnobs::eExtentDefault;

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Crop(node);
    }

    explicit Crop(NodePtr node);

    virtual ~Crop();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    virtual bool rendersUnprocessedChannels() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return false;
    }

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    ExtentEnum getExtent() const WARN_UNUSED_RETURN;

    /**
     * @brief The crop rectangle at (time, view) in canonical coordinates, and its pixel aspect
     * ratio, as the openfx-misc CropPlugin::getCropRectangle() computes it.
     *
     * useIntersect lets the intersect knob clip the rectangle to the source's region of
     * definition, and forceIntersect clips it whatever the knob says. useReformat moves the
     * rectangle's bottom left corner to the origin, and useBlackOutside grows it by one pixel
     * at scale on every side.
     **/
    void getCropRectangle(double time,
                          ViewIdx view,
                          const RenderScale& scale,
                          bool useIntersect,
                          bool forceIntersect,
                          bool useBlackOutside,
                          bool useReformat,
                          RectD* cropRect,
                          double* par) const;

protected:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    /**
     * @brief Keeps the rectangle overlay on while the crop is not reformatted.
     **/
    void updateRectangleEnable();

    ExtentKnobs _extentKnobs;
    KnobDoubleWPtr _softness;
    KnobBoolWPtr _reformat;
    KnobBoolWPtr _intersect;
    KnobBoolWPtr _blackOutside;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_Crop_h
