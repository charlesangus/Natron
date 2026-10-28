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

#include "MoonRaySettings.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
// Its default is MoonRay's own (scene_rdl2's SceneVariables), which a value equal to it leaves alone.
RenderSettingsKnobs::CuratedSetting
sceneVariable(const char* variable,
              const char* name,
              const char* tooltip,
              const VtValue& defaultValue,
              double minimum,
              double displayMaximum)
{
    RenderSettingsKnobs::CuratedSetting s;

    s.key = std::string("moonray:sceneVariable:") + variable;
    s.name = name;
    s.tooltip = tooltip;
    s.defaultValue = defaultValue;
    s.hasRange = true;
    s.minimum = minimum;
    s.displayMaximum = displayMaximum;

    return s;
}

RenderSettingsKnobs::CuratedSetting
declared(const char* key,
         const char* tooltip)
{
    RenderSettingsKnobs::CuratedSetting s;

    s.key = key;
    s.name = key;
    s.tooltip = tooltip;
    s.defaultValue = VtValue(false);

    return s;
}

RenderSettingsKnobs::Choice
filter(const char* id,
       const char* label)
{
    RenderSettingsKnobs::Choice c;

    c.id = id;
    c.label = label;
    c.value = VtValue(TfToken(id));

    return c;
}

RenderSettingsKnobs::Curated
make()
{
    RenderSettingsKnobs::Curated c;

    c.tooltip = "The MoonRay settings most renders change. Every other setting hdMoonray declares is under Other Settings.";
    c.settings.push_back(sceneVariable("pixel_samples", "pixelSamples",
                                       "The square root of the camera samples per pixel: 8 takes 64.", VtValue(8), 1, 32));
    c.settings.push_back(sceneVariable("light_samples", "lightSamples",
                                       "The square root of the samples each light gets at each hit.", VtValue(2), 0, 16));
    c.settings.push_back(sceneVariable("bsdf_samples", "bsdfSamples",
                                       "The square root of the samples each BSDF lobe gets at each hit.", VtValue(2), 0, 16));
    c.settings.push_back(sceneVariable("bssrdf_samples", "bssrdfSamples",
                                       "The square root of the subsurface scattering samples at each hit.", VtValue(2), 0, 16));
    c.settings.push_back(sceneVariable("max_depth", "maxDepth",
                                       "The most bounces a path takes, whatever their kind.", VtValue(5), 0, 32));
    c.settings.push_back(sceneVariable("max_diffuse_depth", "maxDiffuseDepth",
                                       "The most diffuse bounces a path takes.", VtValue(2), 0, 32));
    c.settings.push_back(sceneVariable("max_glossy_depth", "maxGlossyDepth",
                                       "The most glossy bounces a path takes.", VtValue(2), 0, 32));
    c.settings.push_back(sceneVariable("max_mirror_depth", "maxMirrorDepth",
                                       "The most mirror bounces a path takes.", VtValue(3), 0, 32));
    c.settings.push_back(sceneVariable("max_volume_depth", "maxVolumeDepth",
                                       "The most volume scattering bounces a path takes.", VtValue(1), 0, 32));
    {
        RenderSettingsKnobs::CuratedSetting s = sceneVariable("pixel_filter", "pixelFilter",
                                                              "The filter that weighs each sample by its distance to the pixel's centre.",
                                                              VtValue(TfToken("cubic b-spline")), 0, 0);
        s.hasRange = false;
        s.choices.push_back(filter("box", "Box"));
        s.choices.push_back(filter("cubic b-spline", "Cubic B-Spline"));
        s.choices.push_back(filter("quadratic b-spline", "Quadratic B-Spline"));
        c.settings.push_back(s);
    }
    c.settings.push_back(sceneVariable("pixel_filter_width", "pixelFilterWidth",
                                       "The width, in pixels, of the pixel filter: wider is softer.", VtValue(3.f), 0, 8));
    c.settings.push_back(declared("enableDenoise", "Denoises the render with OpenImageDenoise."));
    {
        RenderSettingsKnobs::CuratedSetting s = declared("enableOIDN", "Denoises with OpenImageDenoise rather than OptiX.");
        s.follows = "enableDenoise";
        s.disabledReason = "OpenImageDenoise is the only denoiser: OptiX needs CUDA, which this build of MoonRay lacks. "
                           "It follows Enable Denoise.";
        c.settings.push_back(s);
    }
    c.settings.push_back(declared("denoiseAlbedoGuiding", "Guides the denoiser by the albedo, keeping texture detail."));
    c.settings.push_back(declared("denoiseNormalGuiding", "Guides the denoiser by the normals, keeping geometric detail."));
    // Natron gives hdMoonray its thread budget and runs it on this machine only.
    c.excludedKeys.push_back("localReservedCores");
    c.excludedKeys.push_back("maxConnectRetries");
    const std::string local("Natron renders MoonRay on this machine only.");
    c.disabledKeys.push_back(std::make_pair(std::string("useRemoteHosts"), local));
    c.disabledKeys.push_back(std::make_pair(std::string("remoteHosts"), local));

    return c;
} // make
} // anon namespace

namespace MoonRaySettings {
const RenderSettingsKnobs::Curated&
curated()
{
    static const RenderSettingsKnobs::Curated c = make();

    return c;
}

const char*
samplesKey()
{
    return "moonray:sceneVariable:pixel_samples";
}
} // namespace MoonRaySettings

NATRON_NAMESPACE_EXIT
