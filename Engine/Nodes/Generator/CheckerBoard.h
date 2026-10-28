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

#ifndef Engine_Nodes_Generator_CheckerBoard_h
#define Engine_Nodes_Generator_CheckerBoard_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/NativeGenerator.h"

#define PLUGINID_NATRON_CHECKERBOARD "CheckerBoard"
#define PLUGIN_MAJOR_NATRON_CHECKERBOARD 1

#define kCheckerBoardParamBoxSize "boxSize"
#define kCheckerBoardParamColor0 "color0"
#define kCheckerBoardParamColor1 "color1"
#define kCheckerBoardParamColor2 "color2"
#define kCheckerBoardParamColor3 "color3"
#define kCheckerBoardParamLineColor "lineColor"
#define kCheckerBoardParamLineWidth "lineWidth"
#define kCheckerBoardParamCenterLineColor "centerlineColor"
#define kCheckerBoardParamCenterLineWidth "centerlineWidth"

NATRON_NAMESPACE_ENTER

/**
 * @brief Where CheckerBoard draws, in pixel coordinates at the render scale, as the openfx-misc
 * CheckerBoardProcessorBase::setValues() derives it.
 **/
struct CheckerBoardGeometry {
    double boxSizeX;
    double boxSizeY;
    double lineInfX;
    double lineSupX;
    double lineInfY;
    double lineSupY;
    double centerlineInfX;
    double centerlineSupX;
    double centerlineInfY;
    double centerlineSupY;
    double centerX;
    double centerY;
};

/**
 * @brief The geometry for the knob values (box size, line widths in canonical units), the
 * region of definition in canonical coordinates, the render scale and the pixel aspect ratio.
 **/
CheckerBoardGeometry checkerBoardGeometry(double boxSizeX,
                                          double boxSizeY,
                                          double lineWidth,
                                          double centerlineWidth,
                                          const RectD& rod,
                                          double renderScaleX,
                                          double renderScaleY,
                                          double par);

/// The colour knob a pixel takes.
enum CheckerBoardColorEnum {
    eCheckerBoardColor0 = 0,
    eCheckerBoardColor1,
    eCheckerBoardColor2,
    eCheckerBoardColor3,
    eCheckerBoardColorLine,
    eCheckerBoardColorCenterline
};

/**
 * @brief What row y of the checkerboard holds: one colour throughout (a centre line or a line
 * between boxes), or boxes, with the colours of its even and odd box columns.
 **/
struct CheckerBoardRow {
    bool uniform;
    CheckerBoardColorEnum uniformColor;
    CheckerBoardColorEnum evenColor;
    CheckerBoardColorEnum oddColor;
};

CheckerBoardRow checkerBoardRow(const CheckerBoardGeometry& g,
                                int y);

/**
 * @brief The end of the run of pixels from x (x < xEnd) on a row that take pixel x's colour,
 * at most xEnd, and that colour in *color. Walking a row run by run gives every pixel the colour
 * the openfx-misc per-pixel test gives it, with a handful of divisions per box instead of two per
 * pixel.
 **/
int checkerBoardRunEnd(const CheckerBoardGeometry& g,
                       const CheckerBoardRow& row,
                       int x,
                       int xEnd,
                       CheckerBoardColorEnum* color);

/**
 * @brief CheckerBoard: boxes of boxSize canonical units in four colours around the centre of the
 * region of definition, with optional lines between boxes and centre lines, with the openfx-misc
 * CheckerBoardPlugin's knobs, defaults and pixel maths. Its pixel depends on where it is at the
 * render scale, so the op takes the render's PassContext. It registers under that plugin's ID
 * one major above.
 **/
struct CheckerBoardOp {
    // Knob values in canonical units, the region of definition in canonical coordinates and the
    // output pixel aspect ratio; the geometry at a render scale is derived from them.
    struct Params {
        double boxSizeX;
        double boxSizeY;
        double lineWidth;
        double centerlineWidth;
        double rodX1;
        double rodY1;
        double rodX2;
        double rodY2;
        double par;
        float colors[6][4];
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_CHECKERBOARD,
        .label = "CheckerBoard",
        .description = "Generate an image with a checkerboard.\n"
                       "A frame range may be specified for operators that need it.\n"
                       "See also: https://web.archive.org/web/20220807183203/http://www.opticalenquiry.com/nuke/index.php?title=Constant,_CheckerBoard,_ColorBars,_ColorWheel",
        .grouping = PLUGIN_GROUP_IMAGE,
        .majorVersion = PLUGIN_MAJOR_NATRON_CHECKERBOARD,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/CheckerBoard.png",
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = {},
        .layers = LayerPolicy::targetSelect(),
        .op = OpKind::Point,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);

    Params bake(const KnobReader& reader) const;

    bool isIdentity(const Params& params,
                    const PassContext& context) const;

    void row(const Params& params,
             const PassContext& context,
             const RowIO& io) const;

    GpuOpRef gpu(const Params& params,
                 const PassContext& context) const;

    /**
     * @brief Where params draw at context's render scale.
     **/
    static CheckerBoardGeometry geometry(const Params& params,
                                         const PassContext& context);

    const NativeGenerator* generator = nullptr;
    KnobDoubleWPtr boxSize;
    KnobColorWPtr color0;
    KnobColorWPtr color1;
    KnobColorWPtr color2;
    KnobColorWPtr color3;
    KnobColorWPtr lineColor;
    KnobDoubleWPtr lineWidth;
    KnobColorWPtr centerlineColor;
    KnobDoubleWPtr centerlineWidth;
};

static_assert(ScaledPointOp<CheckerBoardOp>);

using CheckerBoard = GeneratorNode<CheckerBoardOp>;

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Generator_CheckerBoard_h
