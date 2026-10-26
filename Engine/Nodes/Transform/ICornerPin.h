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

#ifndef Engine_Nodes_Transform_ICornerPin_h
#define Engine_Nodes_Transform_ICornerPin_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Transform/CornerPin.h"
#include "Engine/Nodes/Transform/IWarp.h"

#define PLUGINID_NATRON_ICORNERPIN "ICornerPin"
#define PLUGIN_MAJOR_NATRON_ICORNERPIN 1

NATRON_NAMESPACE_ENTER

/**
 * @brief CornerPin with a Map input: each output pixel gets the pin at its Map value, the "to"
 * points blended from the "from" points by it, so the Map scales the pin's Amount per pixel.
 *
 * It has CornerPin's knobs, interact and behaviours (the points follow a new source until they
 * are edited; the copy buttons are undoable), and the Map's knobs. The extra matrix blends
 * from the identity by the Map value, so a black Map leaves the source unchanged.
 **/
class ICornerPin
    : public IWarp {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new ICornerPin(node);
    }

    explicit ICornerPin(NodePtr node);

    virtual ~ICornerPin();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual std::string mapNotConnectedMessage() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief Sets the "from" points, and with alsoTo the "to" points, to the corners of the
     * source's region of definition. Nothing happens without a source or with an infinite or
     * empty source region.
     **/
    void setFromToInput(double time, bool alsoTo);

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

private:
    void updateClampEnabled(double time, ViewSpec view);

    void markPointsEdited(ValueChangedReasonEnum reason);

    CornerPinOp _op;
    KnobBoolWPtr _srcClipChanged;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_ICornerPin_h
