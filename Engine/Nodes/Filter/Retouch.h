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

#ifndef Engine_Nodes_Filter_Retouch_h
#define Engine_Nodes_Filter_Retouch_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <memory>
#include <string>
#include <vector>

#include <QtCore/QPointF>

#include "Engine/ChoiceOption.h"
#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Filter/RetouchRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Image/WholeFrame.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/RotoCurveInteract.h"
#include "Engine/RotoCurvePanelHook.h"
#include "Engine/ViewIdx.h"

#define PLUGINID_NATRON_RETOUCH "Retouch"
#define PLUGIN_MAJOR_NATRON_RETOUCH 1

// Node knobs.
#define kRetouchParamAlgorithm "algorithm"
#define kRetouchParamScales "scales"
#define kRetouchParamCurrentScale "currentScale"
#define kRetouchParamMergeFrom "mergeFrom"
#define kRetouchParamOutput "output"
#define kRetouchParamPreviewLevels "previewLevels"
#define kRetouchParamBlurType "blurType"
#define kRetouchParamBlurRadius "blurRadius"
#define kRetouchParamFillMode "fillMode"
#define kRetouchParamFillColor "fillColor"
#define kRetouchParamFillBrightness "fillBrightness"
#define kRetouchParamMaxHealIterations "maxHealIterations"
#define kRetouchParamSelectedShape "selectedShape"

// Per-shape knobs, on every shape and, for the selected shapes, on the node.
#define kRetouchShapeParamAlgorithm "shapeAlgorithm"
#define kRetouchShapeParamScale "shapeScale"
#define kRetouchShapeParamSourceOffset "sourceOffset"
#define kRetouchShapeParamBlurType "shapeBlurType"
#define kRetouchShapeParamBlurRadius "shapeBlurRadius"
#define kRetouchShapeParamFillMode "shapeFillMode"
#define kRetouchShapeParamFillColor "shapeFillColor"
#define kRetouchShapeParamFillBrightness "shapeFillBrightness"

#define kRetouchAlgorithmClone "clone"
#define kRetouchAlgorithmHeal "heal"
#define kRetouchAlgorithmBlur "blur"
#define kRetouchAlgorithmFill "fill"
#define kRetouchBlurTypeGaussian "gaussian"
#define kRetouchBlurTypeBilateral "bilateral"
#define kRetouchFillModeErase "erase"
#define kRetouchFillModeColor "color"
#define kRetouchOutputResult "result"
#define kRetouchOutputCurrentScale "currentScale"
#define kRetouchOutputResidual "residual"

#define kRetouchFrameResult "Retouch.result"

// The curve panel's actions.
#define kRetouchActionCopyToScale "copyToCurrentScale"
#define kRetouchActionMoveToScale "moveToCurrentScale"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kRetouchInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

inline constexpr NodeSpec kRetouchNodeSpec = {
    .id = PLUGINID_NATRON_RETOUCH,
    .label = "Retouch",
    .description = "Wavelet-decomposed spot removal, ported from darktable's retouch module.\n"
                   "The image is split into Scales wavelet detail scales and a residual. Each shape drawn in the viewer "
                   "works on one scale (0 for the whole image) with its own algorithm: Clone copies the source, Heal "
                   "copies it and matches it to the surroundings, Blur blurs under the shape and Fill paints a colour. "
                   "Clone and Heal take their source at the shape's Source Offset, which the viewer shows as a "
                   "dashed outline to drag.\n"
                   "New shapes take the node's Algorithm, Current Scale, blur and fill settings; the selected shapes' "
                   "own settings are under Selected Shape. Output shows the result, the current scale or the residual.\n"
                   "The whole image is decomposed at once, so the scales follow the image size.",
    .grouping = PLUGIN_GROUP_FILTER,
    .majorVersion = PLUGIN_MAJOR_NATRON_RETOUCH,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/Retouch.png",
    .inputs = kRetouchInputs,
};

inline constexpr ImagePipelineSpec kRetouchPipeline = {
    .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
    .layers = LayerPolicy::color(true, true, true, false),
    .op = OpKind::Custom,
};

class Retouch
    : public StagedImageEffect,
      public RotoCurvePanelHook {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Retouch(node);
    }

    explicit Retouch(NodePtr node);

    virtual ~Retouch();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return false;
    }

    virtual CurveContextKindEnum getCurveContextKind() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return eCurveContextKindGeometry;
    }

    // A geometry context registers no host transform interact, so without this the viewer never
    // calls the overlay actions below.
    virtual bool hasOverlay() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual RotoCurvePanelHook* getCurvePanelHook() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void onCurveItemCreated(const RotoDrawableItemPtr& item) OVERRIDE FINAL;

    // RotoCurvePanelHook
    virtual std::vector<Column> getColumns() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual std::string getItemText(const RotoItemPtr& item, int column) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual std::vector<MenuAction> getItemMenuActions(const std::list<RotoItemPtr>& items) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void onItemMenuActionTriggered(const std::string& actionId, const std::list<RotoItemPtr>& items) OVERRIDE FINAL;

    /**
     * @brief The "Scale / Algorithm" column text of a shape with these settings: "S2 / Heal", with
     * the residual as "Residual" and a scale the node does not process marked "(off)".
     **/
    static std::string shapeColumnText(int scale, int algorithm, int numScales);

    /**
     * @brief Copies (or, with move, moves) shapes to scale, as one undoable step. A copy is named
     * after its shape and inserted above it.
     **/
    void copyShapesToScale(const std::list<RotoItemPtr>& items, int scale, bool move);

    /**
     * @brief Sets a shape's source offset at time, keyframed when auto-keying is on or the knob is
     * already animated. Writes the node's Selected Shape knob when the shape is selected.
     **/
    void setShapeSourceOffset(const RotoDrawableItemPtr& item, double time, double dx, double dy);

    /**
     * @brief The undo step of a source drag of item at time from start to end, which the drag has
     * already written; keyed as setShapeSourceOffset keys. Null when item has no source offset.
     **/
    UndoCommandPtr makeSourceDragCommand(const RotoDrawableItemPtr& item, double time, const double* start, const double* end) const WARN_UNUSED_RETURN;

    /// The current settings of a shape for a render at time, read from its knobs.
    static RetouchCore::ShapeSettings readShapeSettings(const RotoDrawableItemPtr& item, double time);

    /// The offset from a shape to its source, canonical, at time.
    static void readShapeSourceOffset(const RotoDrawableItemPtr& item, double time, double* dx, double* dy);

    /// The per-shape knobs, owned by host or, without one, detached.
    static std::vector<KnobIPtr> createShapeKnobs(Retouch* host);

    static std::vector<ChoiceOption> algorithmOptions();
    static std::vector<ChoiceOption> blurTypeOptions();
    static std::vector<ChoiceOption> fillModeOptions();

    const RotoCurveInteractPtr& getInteract() const
    {
        return _ui;
    }

private:
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void getPluginShortcuts(std::list<PluginActionShortcut>* shortcuts) OVERRIDE FINAL;
    virtual void refreshExtraStateAfterTimeChanged(bool isPlayback, double time) OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void drawOverlay(double time, const RenderScale& renderScale, ViewIdx view) OVERRIDE FINAL;
    virtual bool onOverlayPenDown(double time,
                                  const RenderScale& renderScale,
                                  ViewIdx view,
                                  const QPointF& viewportPos,
                                  const QPointF& pos,
                                  double pressure,
                                  double timestamp,
                                  PenType pen) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool onOverlayPenMotion(double time,
                                    const RenderScale& renderScale,
                                    ViewIdx view,
                                    const QPointF& viewportPos,
                                    const QPointF& pos,
                                    double pressure,
                                    double timestamp) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool onOverlayPenUp(double time,
                                const RenderScale& renderScale,
                                ViewIdx view,
                                const QPointF& viewportPos,
                                const QPointF& pos,
                                double pressure,
                                double timestamp) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool onOverlayPenDoubleClicked(double time,
                                           const RenderScale& renderScale,
                                           ViewIdx view,
                                           const QPointF& viewportPos,
                                           const QPointF& pos) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool onOverlayKeyDown(double time,
                                  const RenderScale& renderScale,
                                  ViewIdx view,
                                  Key key,
                                  KeyboardModifiers modifiers) OVERRIDE FINAL;
    virtual bool onOverlayKeyUp(double time,
                                const RenderScale& renderScale,
                                ViewIdx view,
                                Key key,
                                KeyboardModifiers modifiers) OVERRIDE FINAL;
    virtual bool onOverlayFocusLost(double time, const RenderScale& renderScale, ViewIdx view) OVERRIDE FINAL;
    virtual void onInteractViewportSelectionCleared() OVERRIDE FINAL;
    virtual void onInteractViewportSelectionUpdated(const RectD& rectangle, bool onRelease) OVERRIDE FINAL;

    RetouchCore::Params readParams(double time, ViewIdx view, float renderScale) const WARN_UNUSED_RETURN;

    // The whole frame in pixels at scale: the source's region of definition, cut to the format when it is infinite.
    bool framePixels(double time, ViewIdx view, const RenderScale& scale, RectI* pixels) const WARN_UNUSED_RETURN;

    // Drops the kept results.
    virtual void purgeCaches() OVERRIDE FINAL;

    // The region, parameters and capped scale count of a GPU render; false when the GPU cannot render it.
    bool gpuPlan(double time, ViewIdx view, const RenderScale& scale, RectI* full, RetouchCore::Params* params, int* scales) const WARN_UNUSED_RETURN;

    /**
     * @brief The shapes of a render at time and scale over full, the source's pixels, top-down,
     * rasterised, in render order.
     **/
    std::vector<RetouchCore::Shape> collectShapes(double time, const RenderScale& scale, const RectI& full) const;

    // The finished shapes of a new drawing get darktable's default source, up and right by half their size.
    void applyDefaultSourceOffsets();

    // The shape whose source outline is under pos, if any.
    RotoDrawableItemPtr pickSource(double time, const QPointF& pos) const;

    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool canRenderOpOnGpu(const GpuQuery& query) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOpGpu(const GpuRenderArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool getOpGpuInputLayer(int inputNb,
                                    const ImageLayerDesc& plane,
                                    double time,
                                    ViewIdx view,
                                    ImageLayerDesc* layer) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual RectI getOpGpuInputRoI(int inputNb,
                                   double time,
                                   ViewIdx view,
                                   const RectI& outputRect,
                                   const RenderScale& scale) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    RotoCurveInteractPtr _ui;

    KnobChoiceWPtr _algorithm;
    KnobIntWPtr _scales;
    KnobIntWPtr _currentScale;
    KnobIntWPtr _mergeFrom;
    KnobChoiceWPtr _output;
    KnobDoubleWPtr _previewLevels;
    KnobChoiceWPtr _blurType;
    KnobDoubleWPtr _blurRadius;
    KnobChoiceWPtr _fillMode;
    KnobColorWPtr _fillColor;
    KnobDoubleWPtr _fillBrightness;
    KnobIntWPtr _maxHealIterations;

    // Shapes created since the last pen down, waiting for their default source.
    std::list<RotoDrawableItemWPtr> _newShapes;
    // The source being dragged.
    RotoDrawableItemWPtr _draggedSource;
    // The drag's offset is the offset at the press moved by the pen since the press; it is never read
    // back from the knob mid-drag.
    QPointF _dragPressPos;
    double _dragTime = 0.;
    double _dragStart[2] = { 0., 0. };
    double _dragEnd[2] = { 0., 0. };

    // The retouched frame, so tiles and windows of one frame process it once.
    WholeFrameCache _result;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_Retouch_h
