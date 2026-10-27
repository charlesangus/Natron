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

#ifndef Engine_Nodes_Transform_ITransform_h
#define Engine_Nodes_Transform_ITransform_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Transform/IWarp.h"
#include "Engine/Nodes/Transform/Transform.h"

#define PLUGINID_NATRON_ITRANSFORM "ITransform"
#define PLUGIN_MAJOR_NATRON_ITRANSFORM 1

NATRON_NAMESPACE_ENTER

/**
 * @brief Transform with a Map input: each output pixel gets the transform at its Map value, the
 * identity where the Map is black and the Transform's own where it is white.
 *
 * It has Transform's knobs, interact and behaviours (the centre resets on a new source until it
 * is moved; Directional Blur disables the shutter), and the Map's knobs. Translate, rotate and
 * skew scale linearly with the Map value and scale geometrically, exactly as Transform's Amount
 * blends them, so a Map of 0.5 renders as Transform at an Amount of 0.5.
 **/
class ITransform
    : public IWarp {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new ITransform(node);
    }

    explicit ITransform(NodePtr node);

    virtual ~ITransform();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual std::string mapNotConnectedMessage() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief Moves the centre to the middle of the source's region, keeping the image where it
     * is, as Transform's Reset Center does.
     **/
    void resetCenter(double time);

protected:
    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;

    virtual void readWarpSettings(double time, ViewIdx view, WarpSettings* settings) const OVERRIDE FINAL;

    virtual TransformInterp::MatrixFn canonicalMatrixFn(double time, ViewIdx view, bool invert) const OVERRIDE FINAL;

    virtual bool isTransformIdentity(double time, ViewIdx view) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual int curveSegments(double time, ViewIdx view) const OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    void updateShutterEnabled();

    void updateClampEnabled(double time, ViewSpec view);

    void updateUniformVisibility(bool allowHiding);

    TransformOp _op;
    KnobBoolWPtr _srcClipChanged;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_ITransform_h
