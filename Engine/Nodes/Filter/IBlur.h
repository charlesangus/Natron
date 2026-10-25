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

#ifndef Engine_Nodes_Filter_IBlur_h
#define Engine_Nodes_Filter_IBlur_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <memory>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Filter/MapInput.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/RectI.h"

#define PLUGINID_NATRON_IBLUR "fr.natron.IBlur"
#define PLUGIN_MAJOR_NATRON_IBLUR 1

#define kIBlurParamSize "size"
#define kIBlurParamCropToFormat "cropToFormat"

// Input indices.
#define kIBlurInputSource 0
#define kIBlurInputMask 1
#define kIBlurInputMap 2

NATRON_NAMESPACE_ENTER

/**
 * @brief The knob values one IBlur evaluation reads: the size at white, in canonical units like
 * Blur's, and the Map knobs with the size at black.
 **/
struct IBlurParams {
    double sizeX = 0.;
    double sizeY = 0.;
    bool cropToFormat = true;
    MapInputParams map;
};

/**
 * @brief A blur whose size varies per pixel with a Map input: Size where the Map is white, Size
 * At Black where it is black, in between elsewhere. The blur is a box of the pixel's radius
 * applied three times, an approximation of the Gaussian of standard deviation size/2.4 that Blur
 * uses, each box an O(1) lookup in a summed-area table, so the cost does not depend on the size.
 * A pixel whose size is 0 is left exactly as it is. A disconnected Map blurs at Size everywhere,
 * and the node says so in a warning.
 *
 * Its passes, over two scratch slots: the radii from the Map, then three times the rows and
 * columns of a double-precision summed-area table per plane, and the box pass that reads it into
 * the other slot. The region of interest and the region of definition grow by three times the
 * larger radius of the two sizes, since a tile's Map is not known when they are asked; a tile
 * whose Map gives a radius of 0 throughout is a pass-through copy.
 *
 * It is a Passes pipeline driven by its own render rather than an ImageNode op, because its
 * passes read the Map, an Aux input the host never blends.
 **/
class IBlur
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new IBlur(node);
    }

    explicit IBlur(NodePtr node);

    virtual ~IBlur();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual std::string getInputHint(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    IBlurParams getParams(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The passes for params at context's scale over a tile whose Map values lie in range,
     * reading map; with no map, the passes have no work and only carry the halo and growth, as the
     * region actions need. No pass at all when the range gives a radius of 0 throughout.
     **/
    static PassList passes(const IBlurParams& params,
                           const PassContext& context,
                           const MapRange& range,
                           const std::shared_ptr<const MapPlane>& map) WARN_UNUSED_RETURN;

    /**
     * @brief The text of the warning a disconnected Map posts.
     **/
    static std::string mapNotConnectedMessage() WARN_UNUSED_RETURN;

protected:
    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    /**
     * @brief Blur's crop to format: only the borders the source had inside its format are clamped
     * back to it.
     **/
    virtual bool clipRegionOfDefinition(double time,
                                        const RenderScale& scale,
                                        ViewIdx view,
                                        const EffectInstancePtr& source,
                                        const RectI& srcRoD,
                                        RectI* rod) OVERRIDE FINAL;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual void onEffectCreated(bool mayCreateFileDialog,
                                 const CreateNodeArgs& args) OVERRIDE FINAL;

    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;

    // An identity when both sizes give a radius of 0.
    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The passes over the full range of the Map, without work.
    virtual PassList opPasses(double time,
                              ViewIdx view,
                              const RenderScale& scale) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    // No GPU path yet.
    virtual bool opPassesOnGpu() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return false;
    }

    // Posts the warning while the Map is disconnected, and takes back its own once connected.
    void refreshMapWarning();

    MapInput _map;
    KnobDoubleWPtr _size;
    KnobBoolWPtr _cropToFormat;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_IBlur_h
