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
         const char* tooltip,
         const VtValue& defaultValue)
{
    RenderSettingsKnobs::CuratedSetting s;

    s.key = key;
    s.name = key;
    s.tooltip = tooltip;
    s.defaultValue = defaultValue;

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

RenderSettingsKnobs::Choice
executionMode(const char* id,
              const char* label)
{
    RenderSettingsKnobs::Choice c;

    c.id = id;
    c.label = label;
    c.value = VtValue(std::string(id));

    return c;
}

RenderSettingsKnobs::GroupSpec
groupSpec(const char* name,
          const char* parent,
          bool open)
{
    RenderSettingsKnobs::GroupSpec g;

    g.name = name;
    g.parent = parent;
    g.open = open;

    return g;
}

RenderSettingsKnobs::Curated
make()
{
    RenderSettingsKnobs::Curated c;

    c.tooltip = "The settings hdMoonray declares.";
    c.pageLabel = "MoonRay";
    c.groups.push_back(groupSpec("sampling", "", true));
    c.groups.push_back(groupSpec("depth", "", true));
    c.groups.push_back(groupSpec("filtering", "", true));
    c.groups.push_back(groupSpec("denoise", "", true));
    c.groups.push_back(groupSpec("sceneToggles", "", false));
    c.groups.push_back(groupSpec("prune", "sceneToggles", false));
    c.groups.push_back(groupSpec("performance", "", false));
    c.groups.push_back(groupSpec("remote", "", false));
    c.groups.push_back(groupSpec("logging", "", false));

    const auto add = [&c](const char* group,
                          RenderSettingsKnobs::CuratedSetting s) {
        s.group = group;
        c.settings.push_back(s);
    };

    add("sampling", sceneVariable("pixel_samples", "pixelSamples", "The square root of the camera samples per pixel: 8 takes 64.", VtValue(8), 1, 32));
    add("sampling", sceneVariable("light_samples", "lightSamples", "The square root of the samples each light gets at each hit.", VtValue(2), 0, 16));
    add("sampling", sceneVariable("bsdf_samples", "bsdfSamples", "The square root of the samples each BSDF lobe gets at each hit.", VtValue(2), 0, 16));
    add("sampling", sceneVariable("bssrdf_samples", "bssrdfSamples", "The square root of the subsurface scattering samples at each hit.", VtValue(2), 0, 16));

    add("depth", sceneVariable("max_depth", "maxDepth", "The most bounces a path takes, whatever their kind.", VtValue(5), 0, 32));
    add("depth", sceneVariable("max_diffuse_depth", "maxDiffuseDepth", "The most diffuse bounces a path takes.", VtValue(2), 0, 32));
    add("depth", sceneVariable("max_glossy_depth", "maxGlossyDepth", "The most glossy bounces a path takes.", VtValue(2), 0, 32));
    add("depth", sceneVariable("max_mirror_depth", "maxMirrorDepth", "The most mirror bounces a path takes.", VtValue(3), 0, 32));
    add("depth", sceneVariable("max_volume_depth", "maxVolumeDepth", "The most volume scattering bounces a path takes.", VtValue(1), 0, 32));

    {
        RenderSettingsKnobs::CuratedSetting s = sceneVariable("pixel_filter", "pixelFilter",
                                                              "The filter that weighs each sample by its distance to the pixel's centre.",
                                                              VtValue(TfToken("cubic b-spline")), 0, 0);
        s.hasRange = false;
        s.choices.push_back(filter("box", "Box"));
        s.choices.push_back(filter("cubic b-spline", "Cubic B-Spline"));
        s.choices.push_back(filter("quadratic b-spline", "Quadratic B-Spline"));
        add("filtering", s);
    }
    add("filtering", sceneVariable("pixel_filter_width", "pixelFilterWidth", "The width, in pixels, of the pixel filter: wider is softer.", VtValue(3.f), 0, 8));

    add("denoise", declared("enableDenoise", "Denoises the render with OpenImageDenoise.", VtValue(false)));
    {
        RenderSettingsKnobs::CuratedSetting s = declared("enableOIDN", "Denoises with OpenImageDenoise rather than OptiX.", VtValue(false));
        s.follows = "enableDenoise";
        s.disabledReason = "OpenImageDenoise is the only denoiser: OptiX needs CUDA, which this build of MoonRay lacks. "
                           "It follows Enable Denoise.";
        add("denoise", s);
    }
    add("denoise", declared("denoiseAlbedoGuiding", "Guides the denoiser by the albedo, keeping texture detail.", VtValue(false)));
    add("denoise", declared("denoiseNormalGuiding", "Guides the denoiser by the normals, keeping geometric detail.", VtValue(false)));

    add("sceneToggles", declared("enableMotionBlur", "Enable Motion Blur", VtValue(true)));
    add("sceneToggles", declared("disableLighting", "Disable Lighting", VtValue(false)));
    add("sceneToggles", declared("doubleSided", "DoubleSided", VtValue(false)));
    add("sceneToggles", declared("decodeNormals", "Decode Normals", VtValue(false)));
    add("sceneToggles", declared("forcePolygon", "Force Polygon", VtValue(false)));

    add("prune", declared("pruneWillow", "Prune Willow", VtValue(false)));
    add("prune", declared("pruneFurDeform", "Prune FurDeform", VtValue(false)));
    add("prune", declared("pruneVolume", "Prune Volumes", VtValue(false)));
    add("prune", declared("pruneWrapDeform", "Prune WrapDeform", VtValue(false)));
    add("prune", declared("pruneCurveDeform", "Prune CurveDeform", VtValue(false)));

    {
        RenderSettingsKnobs::CuratedSetting s = declared("executionMode", "Execution Mode", VtValue(std::string("auto")));
        s.choices.push_back(executionMode("auto", "Auto"));
        s.choices.push_back(executionMode("vectorized", "Vectorized"));
        s.choices.push_back(executionMode("xpu", "XPU"));
        s.choices.push_back(executionMode("scalar", "Scalar"));
        add("performance", s);
    }
    add("performance", declared("maxFps", "Max FPS", VtValue(12.f)));

    // Natron gives hdMoonray its thread budget and runs it on this machine only.
    c.excludedKeys.push_back("localReservedCores");
    c.excludedKeys.push_back("maxConnectRetries");
    const char* const local = "Natron renders MoonRay on this machine only.";
    {
        RenderSettingsKnobs::CuratedSetting s = declared("useRemoteHosts", "Use Remote Hosts", VtValue(false));
        s.disabledReason = local;
        add("remote", s);
    }
    {
        RenderSettingsKnobs::CuratedSetting s = declared("remoteHosts", "Remote Hosts", VtValue(5));
        s.disabledReason = local;
        add("remote", s);
    }

    add("logging", declared("logLevel", "Log Level(1-5)", VtValue(1)));
    add("logging", declared("debug", "Show Debug Messages", VtValue(false)));
    add("logging", declared("info", "Show Info Messages", VtValue(false)));
    add("logging", declared("rdlOutput", "Rdla output", VtValue(std::string())));

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
