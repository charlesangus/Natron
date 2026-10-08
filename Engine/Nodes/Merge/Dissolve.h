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

#ifndef Engine_Nodes_Merge_Dissolve_h
#define Engine_Nodes_Merge_Dissolve_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/NativeImageEffect.h"
#include "Engine/ParallelRenderArgs.h"

#define PLUGINID_NATRON_DISSOLVE "net.sf.openfx.DissolvePlugin"
#define PLUGIN_MAJOR_NATRON_DISSOLVE 2

#define kDissolveParamWhich "which"

// Source inputs "0".."63". The Mask input sits at index 2, between "1" and "2", as in the OpenFX plug-in.
#define kDissolveSourceCount 64
#define kDissolveMaskInput 2

NATRON_NAMESPACE_ENTER

/**
 * @brief Dissolve: a weighted average of the two source inputs `floor(which)` and `ceil(which)`
 * out of 64, the blend being the fractional part of `which`, optionally masked. Knob names,
 * defaults, region of definition, identity rules and per-pixel arithmetic are the openfx-misc
 * DissolvePlugin's; it registers under that plug-in's ID one major above.
 **/
class Dissolve
    : public NativeImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Dissolve(node);
    }

    explicit Dissolve(NodePtr node);

    virtual ~Dissolve();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The input number of source clip k (0..63).
     **/
    static int sourceInput(int k)
    {
        return (k < kDissolveMaskInput) ? k : k + 1;
    }

protected:
    virtual NativePluginDescription getNativePluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool isIdentity(double time,
                            const RenderScale& scale,
                            const RectI& roi,
                            ViewIdx view,
                            double* inputTime,
                            ViewIdx* inputView,
                            int* inputNb) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    virtual FramesNeededMap getFramesNeeded(double time,
                                            ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;

    // The `which` value clamped to [0, 63].
    double getWhich(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    bool isSourceConnected(int k) const WARN_UNUSED_RETURN;

    bool getDissolveMaskInvert(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    // Sets the display range of `which` to the highest connected source input.
    void updateRange();

    KnobDoubleWPtr _which;
    KnobBoolWPtr _dissolveMaskInvert;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Merge_Dissolve_h
