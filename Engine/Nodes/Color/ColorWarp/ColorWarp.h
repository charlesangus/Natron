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

#ifndef Engine_Nodes_Color_ColorWarp_ColorWarp_h
#define Engine_Nodes_Color_ColorWarp_ColorWarp_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Color/ColorWarp/ColorWarpField.h"
#include "Engine/Nodes/Color/ColorWarp/ColorWarpInteract.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_COLORWARP "ColorWarp"
#define PLUGIN_MAJOR_NATRON_COLORWARP 1

#define kColorWarpParamPins "pins"
#define kColorWarpParamWarpSpace "warpSpace"
#define kColorWarpParamKernel "kernel"
#define kColorWarpParamLightnessFalloff "lightnessFalloff"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kColorWarpInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

struct ColorWarpOp {
    struct Params {
        // Null when the warp is an identity.
        std::shared_ptr<const ColorWarpField> field;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_COLORWARP,
        .label = "ColorWarp",
        .description = "Warp colours with pins. Each pin maps a source colour to a target colour with a strength; "
                       "colours near a pin move with it, and the move fades with distance in the warp space.\n"
                       "Click the viewer to add a pin on the colour under the pointer, drag a pin to pick another "
                       "colour, and push the selected pin's target with the colour global mode (Ctrl+E).\n"
                       "A pin whose target is its source, or whose strength is 0, holds its colour in place.",
        .grouping = PLUGIN_GROUP_COLOR,
        .majorVersion = PLUGIN_MAJOR_NATRON_COLORWARP,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/ColorWarp.png",
        .inputs = kColorWarpInputs,
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

    KnobColorWarpPinsWPtr pins;
    KnobChoiceWPtr warpSpace;
    KnobChoiceWPtr kernel;
    KnobDoubleWPtr lightnessFalloff;
    const KnobHolder* holder = nullptr;

private:
    // The last prepared field and what it was prepared from, so that frames with the same pin
    // values share one preparation.
    struct Prepared {
        std::vector<double> key;
        std::shared_ptr<const ColorWarpField> field;
    };

    mutable std::mutex _preparedMutex;
    mutable Prepared _prepared;
};

static_assert(PointOp<ColorWarpOp>);

class ColorWarp
    : public ImageNode<ColorWarpOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new ColorWarp(node);
    }

    explicit ColorWarp(NodePtr node);

    virtual ~ColorWarp();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    ColorWarpOp::Params bakeColorWarp(double time,
                                      ViewIdx view) const WARN_UNUSED_RETURN;

    void colorWarpRow(const ColorWarpOp::Params& params,
                      const RowIO& io) const;

    KnobColorWarpPinsPtr getPinsKnob() const WARN_UNUSED_RETURN;

    /**
     * @brief Adds a pin on the source input's colour at canonical (x, y) and time, as one undo
     * step, and selects it. Returns the new pin's id, or 0 when (x, y) is outside the input.
     **/
    int addPinAt(double x,
                 double y,
                 double time,
                 ViewIdx view);

    /**
     * @brief Removes the pin with id as one undo step.
     **/
    bool removePin(int id);

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool hasOverlay() const OVERRIDE FINAL
    {
        return true;
    }

    virtual bool hasOverlayDrawList() const OVERRIDE FINAL
    {
        return true;
    }

    virtual void buildOverlay(double time,
                              const RenderScale& renderScale,
                              ViewIdx view,
                              OverlayDrawList* list) OVERRIDE FINAL;
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
    virtual bool onOverlayKeyDown(double time,
                                  const RenderScale& renderScale,
                                  ViewIdx view,
                                  Key key,
                                  KeyboardModifiers modifiers) OVERRIDE FINAL WARN_UNUSED_RETURN;

    bool pixelScale(double* scaleX,
                    double* scaleY) const;

    /**
     * @brief The source input's colour at canonical (x, y), unpremultiplied by the (Un)premult By
     * channel when one is set and above 0, as the warp sees it. abortInfo stops the render.
     **/
    std::optional<std::array<double, 3>> sampleSource(double x,
                                                      double y,
                                                      double time,
                                                      ViewIdx view,
                                                      const AbortableRenderInfoPtr& abortInfo = AbortableRenderInfoPtr());

    void requestDragSample(double x,
                           double y,
                           double time,
                           ViewIdx view);
    void onDragSampled(std::uint64_t serial,
                       int pinId,
                       double x,
                       double y,
                       double time,
                       const std::optional<std::array<double, 3>>& source);

    /**
     * @brief Records a finished drag's move as one undo step once its last sample has landed.
     **/
    void commitMoveWhenSettled();

    /**
     * @brief Records a finished drag's move now, whether or not its last sample has landed.
     **/
    void commitMoveNow();

    struct DragSampler;

    ColorWarpInteract _interact;
    // Pen motion samples the source off the main thread, latest position first; a newer position
    // aborts the sample in flight.
    std::shared_ptr<DragSampler> _dragSampler;
    std::uint64_t _dragSerial = 0;
    std::uint64_t _dragSettledSerial = 0;
    // Samples older than this belong to an earlier drag.
    std::uint64_t _dragFirstSerial = 1;
    std::optional<ColorWarpPinSet> _moveBefore;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Color_ColorWarp_ColorWarp_h
