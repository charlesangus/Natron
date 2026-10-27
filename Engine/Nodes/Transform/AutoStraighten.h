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

#ifndef Engine_Nodes_Transform_AutoStraighten_h
#define Engine_Nodes_Transform_AutoStraighten_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/ResampleExecutor.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Image/TransformMath.h"
#include "Engine/Nodes/Transform/AshiftFit.h"
#include "Engine/Nodes/Transform/AshiftLines.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RotoCurvePanelHook.h"
#include "Engine/ViewIdx.h"

#define PLUGINID_NATRON_AUTOSTRAIGHTEN "AutoStraighten"
#define PLUGIN_MAJOR_NATRON_AUTOSTRAIGHTEN 1

#define kAutoStraightenParamRotation "rotation"
#define kAutoStraightenParamLensShiftV "lensShiftV"
#define kAutoStraightenParamLensShiftH "lensShiftH"
#define kAutoStraightenParamShear "shear"
#define kAutoStraightenParamAutoCrop "autoCrop"
#define kAutoStraightenParamLensModel "lensModel"
#define kAutoStraightenParamFocalLength "focalLength"
#define kAutoStraightenParamCropFactor "cropFactor"
#define kAutoStraightenParamLensDependence "lensDependence"
#define kAutoStraightenParamAspectAdjust "aspectAdjust"
#define kAutoStraightenParamStructure "structure"
#define kAutoStraightenParamFitParameters "fitParameters"
#define kAutoStraightenParamFitVertical "fitVertical"
#define kAutoStraightenParamFitHorizontal "fitHorizontal"
#define kAutoStraightenParamFitBoth "fitBoth"
#define kAutoStraightenParamTopLeft "topLeft"
#define kAutoStraightenParamTopRight "topRight"
#define kAutoStraightenParamBottomLeft "bottomLeft"
#define kAutoStraightenParamBottomRight "bottomRight"

// The orientation of a drawn guide line: each line has its own knob of that name, and the node's
// knob shows the selected line's.
#define kAutoStraightenParamLineOrientation "lineOrientation"
#define kAutoStraightenParamLineOrientationLabel "Line Orientation"

// The viewer toolbar group and action that draw guide lines.
#define kAutoStraightenUIParamGuideLinesToolButton "guideLinesTool"
#define kAutoStraightenUIParamGuideLinesToolButtonLabel "Guide Lines Tool"
#define kAutoStraightenUIParamDrawLineToolButtonAction "drawLineTool"
#define kAutoStraightenUIParamDrawLineToolButtonActionLabel "Draw Line Tool"
#define kAutoStraightenUIParamDrawLineToolButtonActionHint "Draw a guide line: click and drag along an edge that should be vertical or horizontal."

// The base name of drawn guide lines.
#define kAutoStraightenLineBaseName "Line"

// The curve panel's context menu actions.
#define kAutoStraightenActionSetVertical "setVertical"
#define kAutoStraightenActionSetHorizontal "setHorizontal"

NATRON_NAMESPACE_ENTER

class AutoStraighten;

/**
 * @brief The knob values one AutoStraighten evaluation reads. The model is darktable's as the
 * pipeline uses it (commit_params()): the stored rotation is the negated knob value.
 **/
struct AutoStraightenParams {
    double time = 0.;
    ViewIdx view;
    Ashift::Model model;
    Ashift::CropModeEnum crop = Ashift::eCropLargest;
    Resampler::FilterEnum filter = Resampler::eFilterCubic;
    bool clamp = false;
    bool blackOutside = false;
};

inline constexpr InputSpec kAutoStraightenInputs[] = {
    InputSpec { .label = "Source" },
};

/**
 * @brief The Resample op of AutoStraighten, in the matrix form: the homography and the crop depend
 * on the source format, so transform() asks the node.
 **/
struct AutoStraightenOp {
    using Params = AutoStraightenParams;

    static constexpr bool kResamplesOnGpu = true;

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_AUTOSTRAIGHTEN,
        .label = "AutoStraighten",
        .description = "Correct converging lines and a tilted horizon: rotation, vertical and horizontal lens shift "
                       "(keystone) and shear, fitted to vertical and horizontal guide lines drawn in the viewer or to a "
                       "rectangle placed in the viewer, with an automatic crop of the empty borders.\n"
                       "A port of darktable's rotate and perspective module (ashift).",
        .grouping = PLUGIN_GROUP_TRANSFORM,
        .majorVersion = PLUGIN_MAJOR_NATRON_AUTOSTRAIGHTEN,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/AutoStraighten.png",
        .inputs = kAutoStraightenInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = {},
        .layers = LayerPolicy::all(),
        .op = OpKind::Resample,
    };

    Params bake(const KnobReader& reader) const;

    bool isIdentity(const Params& params,
                    const PassContext& context) const;

    ResampleSettings settings(const Params& params,
                              const PassContext& context) const;

    bool transform(const Params& params,
                   const PassContext& context,
                   double amount,
                   bool invert,
                   TransformMath::Mat3* matrix) const;

    /**
     * @brief Never: the output is cropped to the corrected format, which no matrix carries.
     **/
    bool canConcatenate() const;

    std::optional<GpuCostHint> costHint(const Params& params,
                                        const PassContext& context) const;

    const AutoStraighten* node = nullptr;
    KnobDoubleWPtr rotation;
    KnobDoubleWPtr lensShiftV;
    KnobDoubleWPtr lensShiftH;
    KnobDoubleWPtr shear;
    KnobChoiceWPtr autoCrop;
    KnobChoiceWPtr lensModel;
    KnobDoubleWPtr focalLength;
    KnobDoubleWPtr cropFactor;
    KnobDoubleWPtr lensDependence;
    KnobDoubleWPtr aspectAdjust;
    KnobChoiceWPtr filter;
    KnobBoolWPtr clamp;
    KnobBoolWPtr blackOutside;
};

static_assert(GpuResampleOp<AutoStraightenOp>);

/**
 * @brief AutoStraighten: darktable's rotate and perspective module. The source is warped by the
 * module's homography through Resampler; the output format is the corrected image's bounding box,
 * cropped by autoCrop. The node hosts guide lines, open curves of its curve context each tagged
 * vertical or horizontal; the fit buttons fit the rotation, the lens shifts and the shear to them
 * (or to the four sides of the rectangle in Rectangle mode).
 **/
class AutoStraighten
    : public ImageNode<AutoStraightenOp>,
      public RotoCurvePanelHook {
public:
    enum LensModelEnum {
        eLensModelGeneric = 0,
        eLensModelSpecific
    };

    enum StructureEnum {
        eStructureDrawnLines = 0,
        eStructureRectangle
    };

    enum FitParametersEnum {
        eFitParametersAll = 0,
        eFitParametersRotation,
        eFitParametersLensShift,
        eFitParametersRotationAndLensShift
    };

    enum LineOrientationEnum {
        eLineOrientationVertical = 0,
        eLineOrientationHorizontal
    };

    enum FitButtonEnum {
        eFitButtonVertical = 0,
        eFitButtonHorizontal,
        eFitButtonBoth
    };

    /**
     * @brief The geometry the warp is computed on: the source format in pixels, its pixel aspect
     * ratio, darktable's square-pixel image size, the crop box and the output format.
     **/
    struct Geometry {
        RectI source;
        double par = 1.;
        int width = 0;
        int height = 0;
        Ashift::CropBox crop;
        int outWidth = 0;
        int outHeight = 0;
        RectI output;
        bool bypass = true;
    };

    using CropOverride = std::function<bool(const Ashift::Model& model, int width, int height, Ashift::CropModeEnum mode, Ashift::CropBox* box)>;

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new AutoStraighten(node);
    }

    explicit AutoStraighten(NodePtr node);

    virtual ~AutoStraighten();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    virtual CurveContextKindEnum getCurveContextKind() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return eCurveContextKindGeometry;
    }

    virtual RotoCurvePanelHook* getCurvePanelHook() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void onCurveItemCreated(const RotoDrawableItemPtr& item) OVERRIDE FINAL;

    const RotoCurveInteractPtr& getInteract() const
    {
        return _ui;
    }

    virtual bool supportsMultipleClipPARs() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool supportsRenderQuality() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The model the knobs give at (time, view).
     **/
    Ashift::Model getModel(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The source format and pixel aspect ratio at (time, view): the source's output format,
     * or the project's without a source.
     **/
    void getSourceFormat(double time, ViewIdx view, RectI* format, double* par) const;

    /**
     * @brief The geometry for a source format, a model and a crop mode.
     **/
    Geometry computeGeometry(const RectI& source, double par, const Ashift::Model& model, Ashift::CropModeEnum crop) const WARN_UNUSED_RETURN;

    /**
     * @brief The geometry at (time, view), from the knobs and the source format.
     **/
    Geometry getGeometry(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The canonical matrix from output to source (invert false) or from source to output
     * (invert true), or false when the warp is bypassed.
     **/
    static bool canonicalMatrix(const Geometry& geometry, const Ashift::Model& model, bool invert, TransformMath::Mat3* matrix) WARN_UNUSED_RETURN;

    /**
     * @brief Replaces the automatic crop, for tests that pin darktable's crop box.
     **/
    void setCropOverride(const CropOverride& cropOverride);

    /**
     * @brief Runs a fit button at (time, view): takes the drawn guide lines (or the rectangle),
     * fits and sets the knobs as one undo step. Returns false, with a message on the node, when
     * the fit failed and the knobs are unchanged.
     **/
    bool fit(FitButtonEnum button, double time, ViewIdx view);

    /**
     * @brief The lines the last fit at time used, in darktable's square-pixel source coordinates
     * (y down), for drawing in the viewer; empty when no fit ran at time.
     **/
    Ashift::LineSet lines(double time) const WARN_UNUSED_RETURN;

    /**
     * @brief The fit axes darktable runs for a button and a fitParameters choice.
     **/
    static int fitAxes(FitButtonEnum button, FitParametersEnum parameters) WARN_UNUSED_RETURN;

    /**
     * @brief The four rectangle lines at (time, view) in darktable's source coordinates.
     **/
    Ashift::LineSet rectangleLines(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The drawn guide lines at (time, view) in darktable's source coordinates: one segment
     * per pair of consecutive control points of each activated open curve with a line orientation.
     **/
    Ashift::LineSet drawnLines(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The orientation of a guide line, false when item is not one.
     **/
    static bool getLineOrientation(const RotoItemPtr& item, LineOrientationEnum* orientation) WARN_UNUSED_RETURN;

    /**
     * @brief Sets a guide line's orientation (and the node's knob while the line is selected).
     **/
    void setLineOrientation(const RotoDrawableItemPtr& item, LineOrientationEnum orientation);

    /**
     * @brief Tags a newly drawn line vertical or horizontal by the direction of its end points at time.
     **/
    void setLineOrientationFromDirection(const BezierPtr& line, double time);

    // RotoCurvePanelHook
    virtual std::vector<Column> getColumns() const OVERRIDE FINAL;
    virtual std::string getItemText(const RotoItemPtr& item, int column) const OVERRIDE FINAL;
    virtual std::vector<MenuAction> getItemMenuActions(const std::list<RotoItemPtr>& items) const OVERRIDE FINAL;
    virtual void onItemMenuActionTriggered(const std::string& actionId, const std::list<RotoItemPtr>& items) OVERRIDE FINAL;

protected:
    virtual StatusEnum getPreferredMetadataAt(double time,
                                              ViewIdx view,
                                              NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual void getPluginShortcuts(std::list<PluginActionShortcut>* shortcuts) OVERRIDE FINAL;

    virtual void refreshExtraStateAfterTimeChanged(bool isPlayback, double time) OVERRIDE FINAL;

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

    virtual bool onOverlayFocusLost(double time,
                                    const RenderScale& renderScale,
                                    ViewIdx view) OVERRIDE FINAL;

    virtual void onInteractViewportSelectionCleared() OVERRIDE FINAL;

    virtual void onInteractViewportSelectionUpdated(const RectD& rectangle,
                                                    bool onRelease) OVERRIDE FINAL;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    void refreshVisibility();

    void updateClampEnabled(double time, ViewSpec view);

    RotoCurveInteractPtr _ui;
    KnobChoiceWPtr _structure;
    KnobChoiceWPtr _lineOrientation;
    // Set while setLineOrientation() shows an item's value through the node's knob.
    bool _settingLineOrientation;
    KnobChoiceWPtr _fitParameters;
    KnobButtonWPtr _fitVertical;
    KnobButtonWPtr _fitHorizontal;
    KnobButtonWPtr _fitBoth;
    KnobDoubleWPtr _corners[4];
    mutable std::mutex _mutex;
    CropOverride _cropOverride;
    mutable Geometry _cachedGeometry;
    mutable Ashift::Model _cachedModel;
    mutable Ashift::CropModeEnum _cachedCrop = Ashift::eCropLargest;
    mutable bool _hasCachedGeometry = false;
    Ashift::LineSet _lastLines;
    double _lastLinesTime = 0.;
    bool _hasLastLines = false;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_AutoStraighten_h
