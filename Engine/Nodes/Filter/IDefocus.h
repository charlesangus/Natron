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

#ifndef Engine_Nodes_Filter_IDefocus_h
#define Engine_Nodes_Filter_IDefocus_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Filter/Defocus.h"
#include "Engine/Nodes/Filter/MapInput.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/RectI.h"

#define PLUGINID_NATRON_IDEFOCUS "IDefocus"
#define PLUGIN_MAJOR_NATRON_IDEFOCUS 1

// The knobs Defocus has keep its names (kDefocusParam*); the Map's are MapInput's (kMapInputParam*).

// Input indices.
#define kIDefocusInputSource 0
#define kIDefocusInputMask 1
#define kIDefocusInputKernel 2
#define kIDefocusInputMap 3

NATRON_NAMESPACE_ENTER

/**
 * @brief The knobs: defocus holds Defocus's, its size being the Size, the size where the Map is
 * white; map holds the Map's, with the Size At Black in sizeAtBlack[0].
 **/
struct IDefocusParams {
    DefocusParams defocus;
    MapInputParams map;
};

/**
 * @brief Defocus with a size that varies per pixel with the Map input. Every source pixel spreads
 * into the bokeh of its own size, and each output pixel is the sum of what reaches it over the sum
 * of the weights that do (scatter as gather).
 *
 * The Map value is quantised to levels 0 .. levelCount(): level l has the size at Map l / count,
 * so the top level has exactly the Size and level 0 exactly the Size At Black. The count gives
 * steps of at most a quarter of a pixel in either radius.
 **/
class IDefocus
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new IDefocus(node);
    }

    explicit IDefocus(NodePtr node);

    virtual ~IDefocus();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual std::string getInputHint(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    IDefocusParams getParams(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    /// The highest level of params at context's scale, at least 1 and at most 1024.
    static int levelCount(const IDefocusParams& params, const PassContext& context) WARN_UNUSED_RETURN;

    /// The level of m, a remapped Map value in [0, 1], out of count levels.
    static int levelOf(float m, int count) WARN_UNUSED_RETURN;

    /// The Defocus parameters of level out of count: the size at Map level / count.
    static DefocusParams levelParams(const IDefocusParams& params, int level, int count) WARN_UNUSED_RETURN;

    /// The warning posted while the Map is disconnected.
    static std::string mapNotConnectedMessage() WARN_UNUSED_RETURN;

    virtual bool readsGpuInput(int inputNb,
                               const ImageLayerDesc& plane,
                               double time,
                               ViewIdx view) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual std::optional<GpuCostHint> gpuCostHint(double time,
                                                   ViewIdx view,
                                                   unsigned int mipmapLevel) const OVERRIDE FINAL WARN_UNUSED_RETURN;

protected:
    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    virtual bool getInputLayersRead(int inputNb,
                                    double time,
                                    ViewIdx view,
                                    std::list<ImageLayerDesc>* layers) OVERRIDE FINAL;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onEffectCreated(bool mayCreateFileDialog,
                                 const CreateNodeArgs& args) OVERRIDE FINAL;
    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    // An identity when every level the Map may take gives the single tap.
    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The passes over the full range of the Map, without CPU work; with a GPU recorder for an
    // analytic kernel whose halo is within the GPU's cap.
    virtual PassList opPasses(double time,
                              ViewIdx view,
                              const RenderScale& scale) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The Map in the layer its channel resolves in.
    virtual bool opPassesInputLayer(int inputNb,
                                    const ImageLayerDesc& plane,
                                    double time,
                                    ViewIdx view,
                                    ImageLayerDesc* layer) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The Map over the source's region of interest, as getRegionsOfInterest() asks for it.
    virtual RectI opPassesInputRoI(int inputNb,
                                   const RectI& outputRect,
                                   const RenderScale& scale) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    StatusEnum recordGpu(const PassGpuIO& io,
                         double time,
                         ViewIdx view,
                         const IDefocusParams& params,
                         const PassContext& context) const WARN_UNUSED_RETURN;

    bool isImageKernel(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    bool resolveKernelChannel(double time,
                              ViewIdx view,
                              ImageLayerDesc* layer,
                              int* channel) const WARN_UNUSED_RETURN;

    /**
     * @brief The Image kernel's values, width x height from the bottom row, read from the Kernel
     * input's channel over its region of definition; none, with *empty set while the input is
     * connected, when there is nothing to read.
     **/
    StatusEnum readKernelImage(double time,
                               ViewIdx view,
                               const RenderScale& scale,
                               std::vector<float>* values,
                               int* width,
                               int* height,
                               bool* empty);

    // Posts message, or with an empty one takes back this node's own warnings.
    void setWarning(const std::string& message);
    // The warning the inputs and the kernel type call for.
    std::string idleWarning() const WARN_UNUSED_RETURN;
    void refreshWarning();

    MapInput _map;
    KnobDoubleWPtr _size;
    KnobDoubleWPtr _aspectRatio;
    KnobChoiceWPtr _kernelType;
    KnobIntWPtr _blades;
    KnobDoubleWPtr _rotation;
    KnobDoubleWPtr _roundness;
    KnobChannelSelectWPtr _kernelChannel;
    KnobDoubleWPtr _highlightThreshold;
    KnobDoubleWPtr _highlightGain;

    struct LevelCache;
    std::unique_ptr<LevelCache> _levelCache;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_IDefocus_h
