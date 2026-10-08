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

#ifndef Tests_PassThroughRoDTestEffect_h
#define Tests_PassThroughRoDTestEffect_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EffectInstance.h"
#include "Engine/EngineFwd.h"
#include "Engine/Node.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#define kTestPluginIDPassThroughRoD "test.natron.built-in.PassThroughRoD"

NATRON_NAMESPACE_ENTER

const int kPassThroughRoDTestSize = 32;

/**
 * @brief Takes its RoD from its input's RoD query, with the input's own render hash, the way an OpenFX filter's
 * default RoD reads its source clip; without an input its RoD is a fixed square.
 **/
class PassThroughRoDTestEffect
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new PassThroughRoDTestEffect(n);
    }

    explicit PassThroughRoDTestEffect(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return false;
    }

    virtual StatusEnum getRegionOfDefinition(U64 /*hash*/,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        const EffectInstancePtr input = getInput(0);

        if (!input) {
            rod->x1 = 0.;
            rod->y1 = 0.;
            rod->x2 = kPassThroughRoDTestSize;
            rod->y2 = kPassThroughRoDTestSize;

            return eStatusOK;
        }

        bool isProjectFormat = false;

        return input->getRegionOfDefinition_public(input->getRenderHash(), time, scale, view, rod, &isProjectFormat);
    }

private:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        NativePluginDescription desc;

        desc.id = kTestPluginIDPassThroughRoD;
        desc.label = "Test Pass Through RoD";
        desc.description = "";
        desc.inputs.push_back(NativeInputDescription("Source", true, eDataKindImage));
        desc.outputKind = eDataKindImage;

        return desc;
    }
};

NATRON_NAMESPACE_EXIT

#endif // Tests_PassThroughRoDTestEffect_h
