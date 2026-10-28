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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "CyclesSettings.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
RenderSettingsKnobs::CuratedSetting
setting(const char* key,
        const char* name,
        const char* tooltip,
        const VtValue& defaultValue)
{
    RenderSettingsKnobs::CuratedSetting s;

    s.key = key;
    s.name = name;
    s.tooltip = tooltip;
    s.defaultValue = defaultValue;

    return s;
}

RenderSettingsKnobs::CuratedSetting
ranged(RenderSettingsKnobs::CuratedSetting s,
       double minimum,
       double displayMaximum)
{
    s.hasRange = true;
    s.minimum = minimum;
    s.displayMaximum = displayMaximum;

    return s;
}

RenderSettingsKnobs::Curated
make()
{
    RenderSettingsKnobs::Curated c;

    c.tooltip = "The Cycles settings most renders change. Every other setting Cycles declares is under Other Settings.";
    c.settings.push_back(ranged(setting("cycles:samples", "samples",
                                        "The samples per pixel a render takes before it stops.", VtValue(1024)),
                                1, 4096));
    c.settings.push_back(ranged(setting("cycles:integrator:max_bounce", "maxBounce",
                                        "The most bounces a path takes, whatever their kind.", VtValue(7)),
                                0, 32));
    c.settings.push_back(ranged(setting("cycles:integrator:max_diffuse_bounce", "maxDiffuseBounce",
                                        "The most diffuse bounces a path takes.", VtValue(7)),
                                0, 32));
    c.settings.push_back(ranged(setting("cycles:integrator:max_glossy_bounce", "maxGlossyBounce",
                                        "The most glossy bounces a path takes.", VtValue(7)),
                                0, 32));
    c.settings.push_back(ranged(setting("cycles:integrator:max_transmission_bounce", "maxTransmissionBounce",
                                        "The most transmission bounces a path takes.", VtValue(7)),
                                0, 32));
    c.settings.push_back(ranged(setting("cycles:integrator:max_volume_bounce", "maxVolumeBounce",
                                        "The most volume scattering bounces a path takes.", VtValue(7)),
                                0, 32));
    c.settings.push_back(ranged(setting("cycles:integrator:transparent_max_bounce", "transparentMaxBounce",
                                        "The most transparent surfaces a path passes through.", VtValue(7)),
                                0, 64));
    c.settings.push_back(ranged(setting("cycles:integrator:sample_clamp_direct", "sampleClampDirect",
                                        "The brightest a sample of direct light may be; 0 does not clamp.", VtValue(0.f)),
                                0, 100));
    c.settings.push_back(ranged(setting("cycles:integrator:sample_clamp_indirect", "sampleClampIndirect",
                                        "The brightest a sample of indirect light may be, against fireflies; 0 does not clamp.",
                                        VtValue(10.f)),
                                0, 100));
    c.settings.push_back(setting("cycles:integrator:seed", "seed",
                                 "The seed of the sampling pattern: renders that differ only in it have different noise.",
                                 VtValue(0)));
    c.settings.push_back(setting("cycles:integrator:use_denoise", "useDenoise",
                                 "Denoises the render once it has its samples.", VtValue(false)));
    {
        RenderSettingsKnobs::CuratedSetting s = setting("cycles:integrator:denoiser_type", "denoiserType",
                                                        "The denoiser Use Denoise runs.", VtValue(0));
        RenderSettingsKnobs::Choice oidn;
        oidn.id = "openimagedenoise";
        oidn.label = "OpenImageDenoise";
        s.choices.push_back(oidn);
        s.disabledReason = "OpenImageDenoise is the only denoiser: OptiX needs CUDA, which this build of Cycles lacks.";
        c.settings.push_back(s);
    }
    c.settings.push_back(setting("cycles:integrator:use_light_tree", "useLightTree",
                                 "Samples the lights that matter most to each point, which helps scenes with many "
                                 "lights.",
                                 VtValue(true)));
    c.disabledKeys.push_back(std::make_pair(std::string("cycles:integrator:denoise_use_gpu"),
                                            std::string("Cycles renders and denoises on the CPU here.")));

    return c;
} // make
} // anon namespace

namespace CyclesSettings {
const RenderSettingsKnobs::Curated&
curated()
{
    static const RenderSettingsKnobs::Curated c = make();

    return c;
}

const char*
samplesKey()
{
    return "cycles:samples";
}
} // namespace CyclesSettings

NATRON_NAMESPACE_EXIT
