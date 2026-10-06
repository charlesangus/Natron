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

#define PLUGINID_NATRON_CHECKERBOARD "net.sf.openfx.CheckerBoardPlugin"
#define PLUGIN_MAJOR_NATRON_CHECKERBOARD 2

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
 * @brief CheckerBoard: boxes of boxSize canonical units in four colours around the centre of the
 * region of definition (of the project for Default extent), with optional lines between boxes
 * and centre lines, with the openfx-misc CheckerBoardPlugin's knobs, defaults and pixel maths.
 * Its pixel depends on where it is, so it is not a point operator; its kernel reads the row's
 * position. It registers under that plugin's ID one major above.
 **/
class CheckerBoard
    : public NativeGenerator {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new CheckerBoard(node);
    }

    explicit CheckerBoard(NodePtr node);

    virtual ~CheckerBoard();

protected:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual PixelKernelPtr makeKernel(const KernelContext& context) OVERRIDE FINAL WARN_UNUSED_RETURN;

    KnobDoubleWPtr _boxSize;
    KnobColorWPtr _color0;
    KnobColorWPtr _color1;
    KnobColorWPtr _color2;
    KnobColorWPtr _color3;
    KnobColorWPtr _lineColor;
    KnobDoubleWPtr _lineWidth;
    KnobColorWPtr _centerlineColor;
    KnobDoubleWPtr _centerlineWidth;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Generator_CheckerBoard_h
