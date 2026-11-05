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

#ifndef Tests_SplineWarpHostTestEffect_h
#define Tests_SplineWarpHostTestEffect_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpContext.h"

#define kTestPluginIDSplineWarpHost "test.natron.built-in.SplineWarpHost"

NATRON_NAMESPACE_ENTER

// A geometry curve host whose curve context is a SplineWarpContext, so the SplineWarp model tests
// can edit links and roles on real curves before the SplineWarp node exists. No input, no knobs,
// renders nothing.
class SplineWarpHostTestEffect
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new SplineWarpHostTestEffect(n);
    }

    explicit SplineWarpHostTestEffect(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    virtual CurveContextKindEnum getCurveContextKind() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return eCurveContextKindGeometry;
    }

    virtual RotoCurveContextPtr createGeometryCurveContext(const NodePtr& node) const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return SplineWarpContext::create(node);
    }

private:
    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDSplineWarpHost,
        .label = "Test SplineWarp Host",
        .outputKind = eDataKindImage,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual void initializeKnobs() OVERRIDE FINAL
    {
    }
};

NATRON_NAMESPACE_EXIT

#endif // Tests_SplineWarpHostTestEffect_h
