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
          bool open,
          const char* tooltip)
{
    RenderSettingsKnobs::GroupSpec g;

    g.name = name;
    g.parent = parent;
    g.open = open;
    g.tooltip = tooltip;

    return g;
}

RenderSettingsKnobs::Curated
make()
{
    RenderSettingsKnobs::Curated c;

    c.tooltip = "The settings hdMoonray declares.";
    c.pageLabel = "MoonRay";
    c.groups.push_back(groupSpec("sampling", "", true, "How many samples the render takes. More samples mean less noise and more time. Each value is a square root, so doubling it takes four times the samples."));
    c.groups.push_back(groupSpec("depth", "", true, "How many times light may bounce. Lower is faster, but less light reaches the image."));
    c.groups.push_back(groupSpec("filtering", "", true, "How samples blend into pixels, trading sharpness against softness."));
    c.groups.push_back(groupSpec("denoise", "", true, "Asks MoonRay to remove noise from the image. In this build the denoiser it asks for is missing, so expect the image to stay noisy."));
    c.groups.push_back(groupSpec("sceneToggles", "", false, "Switches that change how the scene is read, mostly to speed up a render or track down a problem."));
    c.groups.push_back(groupSpec("prune", "sceneToggles", false, "Removes kinds of geometry from the render. Mostly useful for scenes that use MoonRay's studio geometry types."));
    c.groups.push_back(groupSpec("performance", "", false, "How MoonRay runs on this machine and how often the image updates."));
    c.groups.push_back(groupSpec("remote", "", false, "Rendering on other machines, which Natron does not do. These controls stay disabled."));
    c.groups.push_back(groupSpec("logging", "", false, "How much MoonRay reports while it renders, for tracking down problems. It does not change the image."));

    const auto add = [&c](const char* group,
                          RenderSettingsKnobs::CuratedSetting s) {
        s.group = group;
        c.settings.push_back(s);
    };

    add("sampling", sceneVariable("pixel_samples", "pixelSamples", "The square root of the camera samples per pixel, so 8 takes 64. Higher cleans up edges and all noise and takes longer; 4 suits previews, 8 is a good start for finals.", VtValue(8), 1, 32));
    add("sampling", sceneVariable("light_samples", "lightSamples", "The square root of the samples each light gets where a camera ray lands. Higher cleans up noisy shadows and takes longer; 2 is a good start, 3 or 4 for soft shadows.", VtValue(2), 0, 16));
    add("sampling", sceneVariable("bsdf_samples", "bsdfSamples", "The square root of the samples each part of a material gets where a camera ray lands. Higher cleans up noisy reflections and bounced light and takes longer; 2 is a good start, 3 or 4 for noisy reflections.", VtValue(2), 0, 16));
    add("sampling", sceneVariable("bssrdf_samples", "bssrdfSamples", "The square root of the subsurface scattering samples where a camera ray lands. Higher cleans up skin and wax and takes longer; 2 is a good start, and it has no cost in scenes without subsurface scattering.", VtValue(2), 0, 16));

    add("depth", sceneVariable("max_depth", "maxDepth", "The most diffuse, glossy and mirror bounces in total, and a ceiling for those three limits; volumes have their own. Lower is faster but darker; 5 is a good start.", VtValue(5), 0, 32));
    add("depth", sceneVariable("max_diffuse_depth", "maxDiffuseDepth", "The most bounces off matte surfaces. Higher brightens interiors and corners and takes longer; 2 is a good start, 3 or 4 for interiors.", VtValue(2), 0, 32));
    add("depth", sceneVariable("max_glossy_depth", "maxGlossyDepth", "The most bounces off shiny surfaces. Higher shows reflections within reflections and takes longer; 2 is a good start.", VtValue(2), 0, 32));
    add("depth", sceneVariable("max_mirror_depth", "maxMirrorDepth", "The most bounces off mirror-like surfaces. Higher shows more mirrors within mirrors and takes longer; 3 is a good start.", VtValue(3), 0, 32));
    add("depth", sceneVariable("max_volume_depth", "maxVolumeDepth", "How many bounces a ray may have taken and still see volumes; after that they are ignored. Higher shows volumes in reflections and bounced light and takes longer; 1 is a good start.", VtValue(1), 0, 32));

    {
        RenderSettingsKnobs::CuratedSetting s = sceneVariable("pixel_filter", "pixelFilter",
                                                              "How samples blend into pixels. Cubic B-Spline is the default and is smooth, Quadratic B-Spline is a little sharper, and Box with a width of 1 does almost no filtering, so it is sharpest and can show aliasing.",
                                                              VtValue(TfToken("cubic b-spline")), 0, 0);
        s.hasRange = false;
        s.choices.push_back(filter("box", "Box"));
        s.choices.push_back(filter("cubic b-spline", "Cubic B-Spline"));
        s.choices.push_back(filter("quadratic b-spline", "Quadratic B-Spline"));
        add("filtering", s);
    }
    add("filtering", sceneVariable("pixel_filter_width", "pixelFilterWidth", "The width of the pixel filter, in pixels. Wider is softer with fewer jagged edges, and narrower is sharper; 3 is a good start.", VtValue(3.f), 0, 8));

    add("denoise", declared("enableDenoise", "Asks MoonRay to remove noise from the image, for cleaner previews and low sample counts at some risk to fine detail. In this build MoonRay still asks for its OptiX denoiser, which it lacks, so expect the image to stay noisy and a message in the log.", VtValue(false)));
    {
        RenderSettingsKnobs::CuratedSetting s = declared("enableOIDN", "Meant to choose OpenImageDenoise rather than OptiX. MoonRay reads it only as another way to turn denoising on, so it does not change the denoiser.", VtValue(false));
        s.follows = "enableDenoise";
        s.disabledReason = "OpenImageDenoise is the only denoiser: OptiX needs CUDA, which this build of MoonRay lacks. "
                           "It follows Enable Denoise.";
        add("denoise", s);
    }
    {
        RenderSettingsKnobs::CuratedSetting s = declared("denoiseAlbedoGuiding", "Guides the denoiser with the surface colours, which keeps texture detail at some cost in time. It needs a render output marked as the albedo input, which nothing sets up today, so expect no effect.", VtValue(false));
        s.sameRowAsNext = true;
        add("denoise", s);
    }
    add("denoise", declared("denoiseNormalGuiding", "Guides the denoiser with the surface normals, which keeps shape detail at some cost in time. It needs a render output marked as the normal input, which nothing sets up today, so expect no effect.", VtValue(false)));

    add("sceneToggles", declared("enableMotionBlur", "Blurs what moves while the camera's shutter is open; the camera's shutter close must be later than its open. It takes more time and memory. Leave it on for finals and turn it off for faster previews.", VtValue(true)));
    {
        RenderSettingsKnobs::CuratedSetting s = declared("disableLighting", "Sets every light to zero, so only glowing surfaces show. Use it to check emission or to rule out lights while tracking down a problem; leave it off otherwise.", VtValue(false));
        // hdMoonray's setDisableLighting uses its render index before the first render pass exists,
        // so a first render with this on and any light in the scene is a null dereference in-process.
        s.disabledReason = "This build of MoonRay crashes on the first render of a scene with lights when this is on.";
        add("sceneToggles", s);
    }
    add("sceneToggles", declared("doubleSided", "Makes all geometry visible from both sides. It fixes surfaces that vanish when seen from behind, and shows geometry the scene meant to hide, so leave it off unless something is missing.", VtValue(false)));
    add("sceneToggles", declared("decodeNormals", "Treats normal map textures as raw data and remaps them from 0 to 1 into -1 to 1. Turn it on if normal maps look wrong because the material does not do this itself; otherwise leave it off.", VtValue(false)));
    add("sceneToggles", declared("forcePolygon", "Renders subdivision surfaces as plain polygons. It saves time and memory, but curved surfaces show their facets. Good for previews; leave it off for finals.", VtValue(false)));

    add("prune", declared("pruneWillow", "Removes MoonRay's WillowGeometry from the render, which is faster but loses that geometry. The MoonRay bundled with Natron has no such geometry type, so it matters only with a scene that brings its own; leave it off otherwise.", VtValue(false)));
    add("prune", declared("pruneFurDeform", "Removes MoonRay's FurDeformGeometry from the render, which is faster but loses that geometry. The MoonRay bundled with Natron has no such geometry type, so it matters only with a scene that brings its own; leave it off otherwise.", VtValue(false)));
    add("prune", declared("pruneVolume", "Removes all volumes from the render. It is much faster when smoke or fog is slowing a preview, but they are gone from the image, so leave it off for finals.", VtValue(false)));
    add("prune", declared("pruneWrapDeform", "Removes MoonRay's WrapDeformGeometry from the render, which is faster but loses that geometry. The MoonRay bundled with Natron has no such geometry type, so it matters only with a scene that brings its own; leave it off otherwise.", VtValue(false)));
    add("prune", declared("pruneCurveDeform", "Removes MoonRay's CurveDeformGeometry from the render, which is faster but loses that geometry. The MoonRay bundled with Natron has no such geometry type, so it matters only with a scene that brings its own; leave it off otherwise.", VtValue(false)));

    {
        RenderSettingsKnobs::CuratedSetting s = declared("executionMode", "Which render engine MoonRay uses. Auto picks the fastest one the scene supports and is the right choice. Scalar is slowest and supports every feature, so try it if a feature misbehaves. XPU needs a GPU path this build lacks. Changing it restarts MoonRay.", VtValue(std::string("auto")));
        s.choices.push_back(executionMode("auto", "Auto"));
        s.choices.push_back(executionMode("vectorized", "Vectorized"));
        s.choices.push_back(executionMode("xpu", "XPU"));
        s.choices.push_back(executionMode("scalar", "Scalar"));
        add("performance", s);
    }
    add("performance", declared("maxFps", "How many times a second MoonRay may update the image while it renders. Higher feels more responsive and spends more time on updates; 12 is a good start. It does not change the final image. Changing it restarts MoonRay.", VtValue(12.f)));

    // Natron gives hdMoonray its thread budget and runs it on this machine only.
    c.excludedKeys.push_back("localReservedCores");
    c.excludedKeys.push_back("maxConnectRetries");
    const char* const local = "Natron renders MoonRay on this machine only.";
    {
        RenderSettingsKnobs::CuratedSetting s = declared("useRemoteHosts", "Spreads the render over other machines, for more speed at the cost of a render farm to manage.", VtValue(false));
        s.disabledReason = local;
        add("remote", s);
    }
    {
        RenderSettingsKnobs::CuratedSetting s = declared("remoteHosts", "How many other machines Use Remote Hosts would spread the render over. More is faster up to the size of the farm.", VtValue(5));
        s.disabledReason = local;
        add("remote", s);
    }

    add("logging", declared("logLevel", "How much the connection to the renderer logs, from 1, errors only, to 5, everything. Higher helps track down problems but fills the log; leave it at 1 otherwise.", VtValue(1)));
    add("logging", declared("debug", "Logs MoonRay's debug messages, which are many. Use it to track down a problem; turning it back off takes effect only after restarting Natron.", VtValue(false)));
    add("logging", declared("info", "Logs MoonRay's progress messages, which are a moderate amount. Use it to see what a render is doing; turning it back off takes effect only after restarting Natron.", VtValue(false)));
    add("logging", declared("rdlOutput", "A file path where MoonRay writes the scene as RDLA text each time the path changes, to inspect or to render outside Natron. Writing takes a moment and the file can be large; leave it empty to write nothing.", VtValue(std::string())));

    const auto enabledBy = [&c](const char* key,
                                const char* controller) {
        for (RenderSettingsKnobs::CuratedSetting& s : c.settings) {
            if (s.key == key) {
                RenderSettingsKnobs::EnabledBy condition;
                condition.key = controller;
                s.enabledBy.push_back(condition);
            }
        }
    };

    for (const char* key : { "max_diffuse_depth", "max_glossy_depth", "max_mirror_depth" }) {
        enabledBy((std::string("moonray:sceneVariable:") + key).c_str(), "moonray:sceneVariable:max_depth");
    }
    enabledBy("denoiseAlbedoGuiding", "enableDenoise");
    enabledBy("denoiseNormalGuiding", "enableDenoise");

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
