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

#include <limits>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
RenderSettingsKnobs::CuratedSetting
setting(const char* key,
        const char* name,
        const std::string& tooltip,
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

RenderSettingsKnobs::CuratedSetting
bounded(RenderSettingsKnobs::CuratedSetting s,
        double minimum,
        double maximum)
{
    s = ranged(s, minimum, maximum);
    s.hasMaximum = true;
    s.maximum = maximum;

    return s;
}

RenderSettingsKnobs::CuratedSetting
menu(RenderSettingsKnobs::CuratedSetting s,
     const std::vector<RenderSettingsKnobs::Choice>& choices)
{
    s.choices = choices;

    return s;
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

    c.tooltip = "The settings Cycles declares.";
    c.groups.push_back(groupSpec("sampling", "", true, "How long the render runs and how clean it gets. More samples mean less noise and more time."));
    c.groups.push_back(groupSpec("denoise", "", true, "Removes leftover noise after sampling, so fewer samples are needed, at some risk to fine detail."));
    c.groups.push_back(groupSpec("lightPaths", "", true, "How far light travels through the scene, and shortcuts that trade accuracy for less noise or time."));
    c.groups.push_back(groupSpec("bounces", "lightPaths", false, "How many times light may bounce. More bounces give fuller, brighter light and take longer."));
    c.groups.push_back(groupSpec("clamping", "lightPaths", false, "Limits very bright samples to remove fireflies, at the cost of some light energy."));
    c.groups.push_back(groupSpec("caustics", "lightPaths", false, "Light focused by shiny or glassy surfaces. Pretty but noisy, so these controls can reduce or remove it."));
    c.groups.push_back(groupSpec("fastGi", "lightPaths", false, "Replaces distant indirect light with a cheap approximation. Faster and cleaner, but less accurate."));
    c.groups.push_back(groupSpec("lights", "", false, "How the render chooses which lights to sample. It affects noise in scenes with many lights."));
    c.groups.push_back(groupSpec("volumes", "", false, "How rays travel through smoke, fog and other volumes, trading accuracy against noise and time."));
    c.groups.push_back(groupSpec("pathGuiding", "", false, "Learns where light comes from and steers rays towards it. This build of Cycles has no path guiding, so nothing in this group has any effect."));
    c.groups.push_back(groupSpec("lightContributions", "", false, "Switches kinds of light on and off to break down or debug an image. Leave them all on for a correct render."));
    c.groups.push_back(groupSpec("film", "", false, "How the image is exposed and filtered, and whether the background is transparent."));
    c.groups.push_back(groupSpec("passes", "", false, "Extra render passes: mist depth, Cryptomatte, and alpha handling."));
    c.groups.push_back(groupSpec("world", "", false, "How the world background takes part in the render: whether it shines, which rays see it, and its light group."));
    c.groups.push_back(groupSpec("advanced", "", false, "Rarely needed controls of the sampling pattern, path lengths and camera rays. The defaults suit most renders."));
    c.groups.push_back(groupSpec("sampleSubset", "advanced", false, "Renders only part of the sample sequence, for example to split a render across runs. Only Sample Offset has any effect."));

    const auto add = [&c](const char* group,
                          RenderSettingsKnobs::CuratedSetting s) {
        s.group = group;
        c.settings.push_back(s);
    };

    add("sampling", ranged(setting("cycles:samples", "samples", "The most samples a pixel gets. More samples mean less noise and a longer render; try 64 to 256 with denoising and 1024 or more without.", VtValue(1024)), 1, 4096));
    add("sampling", setting("cycles:integrator:use_adaptive_sampling", "useAdaptiveSampling", "Stops sampling a pixel once it is clean enough, so the time goes to the noisy parts. It usually saves a lot of time, so leave it on unless every pixel must get the same number of samples.", VtValue(true)));
    add("sampling", setting("cycles:integrator:adaptive_threshold", "adaptiveThreshold", "How much noise a pixel may keep before it stops sampling. Lower is cleaner and slower; 0.01 is a good start, 0.001 suits very clean frames, and 0 picks a value from Samples.", VtValue(0.01f)));
    add("sampling", setting("cycles:integrator:adaptive_min_samples", "adaptiveMinSamples", "The samples every pixel takes before it may stop. Raise it if small bright details get missed, at the cost of time. 0 picks a value from Adaptive Threshold, which is 64 at 0.01.", VtValue(0)));
    add("sampling", setting("cycles:time_limit", "timeLimit", "Stops the render after this many seconds, however many samples it has. A short limit leaves more noise; 0 means no limit.", VtValue(0.0)));
    add("sampling", setting("cycles:integrator:seed", "seed", "The starting point of the noise pattern. The same seed gives the same noise, and another seed gives different noise of the same strength, so any value is fine.", VtValue(0)));

    add("denoise", setting("cycles:integrator:use_denoise", "useDenoise", "Removes noise from the image with OpenImageDenoise. It allows far fewer samples, but adds some time and can smear fine detail. Good for previews and low sample counts.", VtValue(false)));
    {
        RenderSettingsKnobs::CuratedSetting s = setting("cycles:integrator:denoiser_type", "denoiserType",
                                                        "The denoiser Use Denoise runs. There is only one choice here.", VtValue(0));
        RenderSettingsKnobs::Choice oidn;
        oidn.id = "openimagedenoise";
        oidn.label = "OpenImageDenoise";
        s.choices.push_back(oidn);
        s.disabledReason = "OpenImageDenoise is the only denoiser: OptiX needs CUDA, which this build of Cycles lacks.";
        add("denoise", s);
    }
    add("denoise", menu(setting("cycles:integrator:denoiser_quality", "denoiserQuality", "How much effort the denoiser spends. High keeps the most detail and is slowest, Fast suits quick previews, and Balanced sits in between. Use High for final frames.", VtValue(1)), { { "high", "High", VtValue(1) }, { "balanced", "Balanced", VtValue(2) }, { "fast", "Fast", VtValue(3) } }));
    add("denoise", menu(setting("cycles:integrator:denoiser_prefilter", "denoiserPrefilter", "How the albedo and normal guides are cleaned before the denoiser uses them. Accurate cleans them first and takes longest, which keeps the most detail. Fast denoises them with the image for little extra time. None suits only guides that are already free of noise.", VtValue(3)), { { "none", "None", VtValue(1) }, { "fast", "Fast", VtValue(2) }, { "accurate", "Accurate", VtValue(3) } }));
    add("denoise", setting("cycles:integrator:denoiser_passes", "denoiserPasses", "Which guide passes are rendered for the denoiser, as the sum of Albedo 1, Specular Albedo 2, Normal 4, Roughness 8, Depth 16, Motion 32 and Backward Motion 64. OpenImageDenoise reads only Albedo and Normal, and the others just use memory, so leave it at 5.", VtValue(5)));
    add("denoise", setting("cycles:integrator:denoise_start_sample", "denoiseStartSample", "The sample at which progressive updates start to be denoised; earlier updates show the raw noise. Raising it saves time on early updates, and the final image is always denoised. 0 denoises from the start.", VtValue(0)));
    add("denoise", setting("cycles:integrator:denoiser_upscale_factor", "denoiserUpscaleFactor", "Leave this at 1. The denoiser used here ignores it, so it never changes the resolution, and any other value only turns adaptive sampling off while Use Denoise is on.", VtValue(1.f)));
    {
        RenderSettingsKnobs::CuratedSetting s = setting("cycles:integrator:denoise_use_gpu", "denoiseUseGpu", "Runs the denoiser on the GPU rather than the CPU, which would be faster.", VtValue(false));
        s.overrideDefault = true;
        s.disabledReason = "Cycles renders and denoises on the CPU here.";
        add("denoise", s);
    }

    add("denoise", setting("cycles:film:denoising_pass_follow_reflections", "denoisingPassFollowReflections", "Lets the denoising guide passes follow mirror reflections, so the denoiser keeps reflected detail rather than the mirror's own surface. Turn it off if reflections come out smeared.", VtValue(true)));
    add("denoise", setting("cycles:film:denoising_pass_use_albedo_roughness_weighting", "denoisingPassUseAlbedoRoughnessWeighting", "Weights the albedo guide pass by surface roughness, so shiny surfaces guide the denoiser less than matte ones do. It helps keep reflections sharp.", VtValue(true)));

    add("bounces", ranged(setting("cycles:integrator:max_bounce", "maxBounce", "The most times light may bounce in total, whatever the kind, and a ceiling for the limits below. Higher gives fuller, brighter light and a longer render; 4 to 8 suits most shots and 0 gives direct light only.", VtValue(7)), 0, 32));
    add("bounces", ranged(setting("cycles:integrator:max_diffuse_bounce", "maxDiffuseBounce", "The most bounces off matte surfaces. Higher brightens interiors and corners and takes longer; 2 to 4 suits most shots.", VtValue(7)), 0, 32));
    add("bounces", ranged(setting("cycles:integrator:max_glossy_bounce", "maxGlossyBounce", "The most bounces off shiny surfaces. Higher shows reflections within reflections and takes longer; 2 to 4 suits most shots.", VtValue(7)), 0, 32));
    add("bounces", ranged(setting("cycles:integrator:max_transmission_bounce", "maxTransmissionBounce", "The most times light may pass through glass and other refractive surfaces. Too low turns thick or layered glass black, and higher takes longer; try 8 to 12 with a lot of glass.", VtValue(7)), 0, 32));
    add("bounces", ranged(setting("cycles:integrator:max_volume_bounce", "maxVolumeBounce", "The most times light may scatter inside a volume. Higher makes clouds and thick smoke brighter and softer and takes much longer; 0 scatters once, and 0 to 2 suits most shots.", VtValue(7)), 0, 32));
    add("bounces", ranged(setting("cycles:integrator:transparent_max_bounce", "transparentMaxBounce", "The most transparent surfaces a ray may pass through, such as layers of alpha-mapped leaves. Too low leaves dark patches, and higher takes longer; 8 suits most shots and foliage may need 32 or more.", VtValue(7)), 0, 32));

    add("clamping", ranged(setting("cycles:integrator:sample_clamp_direct", "sampleClampDirect", "Limits the brightness of direct samples. Lower values remove fireflies but lose energy and can dull highlights; leave it at 0, which turns it off, unless direct light causes fireflies, then start around 10.", VtValue(0.f)), 0, 100));
    add("clamping", ranged(setting("cycles:integrator:sample_clamp_indirect", "sampleClampIndirect", "Limits the brightness of indirect samples. Lower values remove fireflies but lose energy; 10 is a good start, 0 turns it off.", VtValue(10.f)), 0, 100));

    {
        RenderSettingsKnobs::CuratedSetting s = setting("cycles:integrator:caustics_reflective", "causticsReflective", "Lets matte surfaces receive light reflected off shiny ones, such as the glints from a mirror. Turning it off removes a common source of noise but loses that light. Leave it on unless it causes noise.", VtValue(true));
        s.sameRowAsNext = true;
        add("caustics", s);
    }
    add("caustics", setting("cycles:integrator:caustics_refractive", "causticsRefractive", "Lets matte surfaces receive light focused through glass or water, such as the bright patterns under a glass. Turning it off removes a common source of noise but loses that light. Leave it on unless it causes noise.", VtValue(true)));
    add("caustics", setting("cycles:integrator:filter_glossy", "filterGlossy", "Blurs sharp reflections seen after a blurry bounce, which removes caustic noise and fireflies but is less accurate. 0 turns it off, about 1 is a good start, and higher blurs more.", VtValue(0.f)));

    add("fastGi", setting("cycles:integrator:ao_bounces", "aoBounces", "After this many bounces, light is approximated: rays stop at AO Distance and the world light is scaled by AO Factor. Lower is faster and less accurate; try 1 or 2, and 0 turns it off. It has no effect while AO Factor is 0.", VtValue(0)));
    add("fastGi", setting("cycles:integrator:ao_factor", "aoFactor", "How bright the world light is once AO Bounces has taken over. 1 is its full strength and is a good start; lower is darker. At 0, AO Bounces has no effect.", VtValue(0.f)));
    add("fastGi", setting("cycles:integrator:ao_distance", "aoDistance", "How far rays reach once AO Bounces has taken over, in scene units. Shorter is faster but ignores distant objects, so more world light leaks in; start around the size of the room or set. The default is effectively no limit.", VtValue(std::numeric_limits<float>::max())));
    add("fastGi", setting("cycles:integrator:ao_additive_factor", "aoAdditiveFactor", "Adds ambient occlusion on top of the lighting at each matte hit, separately from AO Bounces. It lifts dark areas cheaply, but the extra light is not physically correct and each hit costs an extra ray. 0 turns it off; try 0.1 to 1.", VtValue(0.f)));

    // direct_light_sampling_type has no entry: Cycles declares it only when built with
    // WITH_CYCLES_DEBUG, and a knob here would be a control the delegate ignores.
    add("lights", setting("cycles:integrator:use_light_tree", "useLightTree", "Picks the lights that matter most to each point rather than treating them alike. It greatly reduces noise with many lights, at a small cost to prepare, so leave it on. Light Sampling Threshold works only with this off.", VtValue(true)));
    add("lights", setting("cycles:integrator:light_sampling_threshold", "lightSamplingThreshold", "Skips most shadow rays to lights too weak to matter at a point. Higher is faster and can add noise; 0.01 is a good start and 0 turns it off.", VtValue(0.f)));

    add("volumes", setting("cycles:integrator:volume_ray_marching", "volumeRayMarching", "Steps through volumes in small increments rather than using the default unbiased method. It can be faster and less noisy but is less accurate. Leave it off unless a volume stays too noisy.", VtValue(false)));
    add("volumes", setting("cycles:integrator:volume_max_steps", "volumeMaxSteps", "The most steps a ray may take through a volume of varying density. A ray that would need more takes larger steps, so raise it if a large volume looks coarse. 1024 is plenty for most scenes.", VtValue(1024)));
    add("volumes", setting("cycles:integrator:volume_step_rate", "volumeStepRate", "Scales the step size worked out for each volume. Lower shows finer detail and takes longer, and higher is faster and coarser. 1 is a good start; try 0.1 to 0.5 for fine wisps.", VtValue(1.f)));

    const std::string noGuiding = " This build of Cycles has no path guiding, so it has no effect.";
    add("pathGuiding", setting("cycles:integrator:use_guiding", "useGuiding", "Learns where light comes from during the render and steers rays towards it, which helps difficult indirect light at some cost in memory and time." + noGuiding, VtValue(false)));
    add("pathGuiding", setting("cycles:integrator:deterministic_guiding", "deterministicGuiding", "Makes repeat renders give the same pixel values, at the cost of slower training. Leave it on when frames must match." + noGuiding, VtValue(true)));
    add("pathGuiding", setting("cycles:integrator:use_surface_guiding", "useSurfaceGuiding", "Steers rays that bounce off surfaces. It is usually worth leaving on." + noGuiding, VtValue(true)));
    add("pathGuiding", setting("cycles:integrator:surface_guiding_probability", "surfaceGuidingProbability", "The share of surface bounces that follow the guide. Higher relies on it more and lower is safer when it learns badly; 0.5 is a good start and 0 turns it off." + noGuiding, VtValue(0.5f)));
    add("pathGuiding", setting("cycles:integrator:use_volume_guiding", "useVolumeGuiding", "Steers rays that scatter inside volumes. It is usually worth leaving on in scenes with volumes." + noGuiding, VtValue(true)));
    add("pathGuiding", setting("cycles:integrator:volume_guiding_probability", "volumeGuidingProbability", "The share of volume scattering that follows the guide. Higher relies on it more and lower is safer when it learns badly; 0.5 is a good start and 0 turns it off." + noGuiding, VtValue(0.5f)));
    add("pathGuiding", setting("cycles:integrator:guiding_training_samples", "guidingTrainingSamples", "How many samples teach the guide before it stops learning. More give a better guide and take longer; 128 is a good start and 0 keeps learning for the whole render." + noGuiding, VtValue(128)));
    add("pathGuiding", setting("cycles:integrator:use_guiding_direct_light", "useGuidingDirectLight", "Teaches the guide about direct light as well as bounced light. It is usually worth leaving on." + noGuiding, VtValue(true)));
    add("pathGuiding", setting("cycles:integrator:use_guiding_mis_weights", "useGuidingMisWeights", "Weights what directly visible lights teach the guide, by how likely each was to be found. It is usually worth leaving on." + noGuiding, VtValue(true)));
    add("pathGuiding", menu(setting("cycles:integrator:guiding_distribution_type", "guidingDistributionType", "How the guide stores the directions light comes from. Parallax-Aware VMM is the default and copes best with nearby lights; VMM and Directional Quad Tree are alternatives to try if it struggles." + noGuiding, VtValue(0)), { { "PARALLAX_AWARE_VMM", "Parallax-Aware VMM", VtValue(0) }, { "DIRECTIONAL_QUAD_TREE", "Directional Quad Tree", VtValue(1) }, { "VMM", "VMM", VtValue(2) } }));
    add("pathGuiding", menu(setting("cycles:integrator:guiding_directional_sampling_type", "guidingDirectionalSamplingType", "How the guide is combined with the surface's own reflection when choosing a direction. RIS is the default and a good start; Product MIS and Roughness are alternatives to try." + noGuiding, VtValue(1)), { { "MIS", "Product MIS", VtValue(0) }, { "RIS", "RIS", VtValue(1) }, { "ROUGHNESS", "Roughness", VtValue(2) } }));
    add("pathGuiding", setting("cycles:integrator:guiding_roughness_threshold", "guidingRoughnessThreshold", "Surfaces smoother than this are not guided, since mirror-like surfaces gain little from it. Lower guides more surfaces and costs more; 0.05 is a good start." + noGuiding, VtValue(0.05f)));

    add("lightContributions", setting("cycles:integrator:use_direct_light", "useDirectLight", "Whether surfaces the camera sees show light arriving straight from lights. Turn it off only to break down or debug an image, since the result is not physically correct.", VtValue(true)));
    add("lightContributions", setting("cycles:integrator:use_indirect_light", "useIndirectLight", "Whether light that has bounced counts. Off is the same as Max Bounce at 0, which is faster and darker. Turn it off only to break down or debug an image.", VtValue(true)));
    add("lightContributions", setting("cycles:integrator:use_diffuse", "useDiffuse", "Whether surfaces the camera sees show their matte shading. Turn it off only to break down or debug an image, since the result is not physically correct.", VtValue(true)));
    add("lightContributions", setting("cycles:integrator:use_glossy", "useGlossy", "Whether surfaces the camera sees show their shiny reflections. Turn it off only to break down or debug an image, since the result is not physically correct.", VtValue(true)));
    add("lightContributions", setting("cycles:integrator:use_transmission", "useTransmission", "Whether surfaces the camera sees show light passing through them. Turn it off only to break down or debug an image, since the result is not physically correct.", VtValue(true)));
    add("lightContributions", setting("cycles:integrator:use_emission", "useEmission", "Whether glowing surfaces and the world background give off light. Turn it off only to break down or debug an image, since it removes that light everywhere.", VtValue(true)));

    add("film", setting("cycles:film:exposure", "exposure", "Scales the brightness of the whole image before it is written. 1 leaves it unchanged, 2 is twice as bright.", VtValue(1.0f)));
    add("film", menu(setting("cycles:film:filter_type", "filterType", "The shape of the filter that blends samples across neighbouring pixels. Box is sharpest and noisiest, Gaussian is softer, and Blackman-Harris is in between.", VtValue(0)), { { "box", "Box", VtValue(0) }, { "gaussian", "Gaussian", VtValue(1) }, { "blackman_harris", "Blackman-Harris", VtValue(2) } }));
    add("film", setting("cycles:film:filter_width", "filterWidth", "The width of the pixel filter in pixels. Wider is softer and less noisy; 1.5 is a common choice.", VtValue(1.0f)));
    add("film", setting("cycles:background:transparent", "transparent", "Hides the world background from the camera so those pixels are transparent, while it still lights the scene. USD Render Settings' Transparent Background authors the same setting and wins when it is on. Off keeps the default Cycles picks from the scene's lights: a dome light shows behind the scene, and a scene without one, lightless scenes included, renders transparent.", VtValue(false)));
    add("film", setting("cycles:background:transparent_glass", "transparentGlass", "Makes glass show as transparent in the alpha channel where the background is visible through it. It needs Transparent.", VtValue(false)));
    add("film", setting("cycles:background:transparent_roughness_threshold", "transparentRoughnessThreshold", "Glass rougher than this shows the background through it as opaque rather than transparent. It needs Transparent Glass; 0 treats all glass as transparent.", VtValue(0.0f)));
    add("film", setting("cycles:film:use_approximate_shadow_catcher", "useApproximateShadowCatcher", "Estimates shadow catcher shadows from the lighting rather than tracing them exactly. It is faster and noisier than the default.", VtValue(false)));

    add("passes", setting("cycles:film:mist_start", "mistStart", "The distance from the camera at which the mist pass starts to fade in, in scene units.", VtValue(0.0f)));
    add("passes", setting("cycles:film:mist_depth", "mistDepth", "The distance over which the mist pass goes from clear to full, in scene units.", VtValue(100.0f)));
    add("passes", setting("cycles:film:mist_falloff", "mistFalloff", "How the mist pass grows with distance. 1 is linear, higher stays clear for longer, and lower fills in sooner.", VtValue(1.0f)));
    add("passes", bounded(setting("cycles:film:cryptomatte_passes", "cryptomattePasses", "Which Cryptomatte ID passes are rendered, as the sum of Object 1, Material 2, Asset 4 and Accurate 8, for picking out objects, materials or assets afterwards. 0 renders none.", VtValue(0)), 0, 15));
    add("passes", setting("cycles:film:cryptomatte_depth", "cryptomatteDepth", "How many overlapping IDs a Cryptomatte pixel can hold. Each level adds one ID per pixel.", VtValue(0)));
    add("passes", setting("cycles:film:pass_alpha_threshold", "passAlphaThreshold", "The alpha above which a pixel counts as covered in the data passes, such as depth and normals. Lower includes more soft edges.", VtValue(0.5f)));

    add("world", setting("cycles:background:use_shader", "useShader", "Lets the world shader light the scene and show behind it. Off gives a black world.", VtValue(true)));
    add("world", setting("cycles:background:visibility", "visibility", "Which kinds of ray can see the world, as the sum of Camera 1, Transmission 2, Diffuse 4, Glossy 8, Volume Scatter 16 and Shadow 96. The default of 127 shows it to all of them.", VtValue(127u)));
    add("world", setting("cycles:background:lightgroup", "lightgroup", "The light group the world's light is rendered into, for adjusting it separately afterwards. Empty means none.", VtValue(std::string())));

    // The enum's values are not in the order Cycles lists them: Round is 4 and First is 3.
    add("advanced", menu(setting("cycles:integrator:sampling_pattern", "samplingPattern", "How samples are spread across each pixel, which changes the look of the noise. Tabulated Sobol is the default and a good start. Sobol-Burley computes them as it goes. Blue Noise makes the noise finer and even: Pure suits a fixed number of samples, Round uses the next power of two, and First keeps the first sample separate.", VtValue(1)), { { "sobol_burley", "Sobol-Burley", VtValue(0) }, { "tabulated_sobol", "Tabulated Sobol", VtValue(1) }, { "blue_noise_pure", "Blue Noise Pure", VtValue(2) }, { "blue_noise_round", "Blue Noise Round", VtValue(4) }, { "blue_noise_first", "Blue Noise First", VtValue(3) } }));
    add("advanced", setting("cycles:integrator:scrambling_distance", "scramblingDistance", "Makes neighbouring pixels share a sampling pattern, which can speed up the render but shows artefacts if pushed too far. 1 turns it off, and try lowering it towards 0.5. It works only with Tabulated Sobol.", VtValue(1.f)));
    add("advanced", setting("cycles:integrator:aa_samples", "aaSamples", "This has no effect: each render overwrites it from Samples, so set Samples instead.", VtValue(0)));
    add("advanced", setting("cycles:integrator:min_bounce", "minBounce", "The bounces a path always takes before it may be ended early at random. Higher gives less noise in dark bounced light and takes longer; 0 is fine for most shots and 3 to 8 can help.", VtValue(0)));
    add("advanced", setting("cycles:integrator:transparent_min_bounce", "transparentMinBounce", "The transparent surfaces a path always passes through before it may be ended early at random. Higher gives less noise in layered transparency and takes longer; 0 is fine for most shots and 3 to 8 can help.", VtValue(0)));
    add("advanced", setting("cycles:integrator:motion_blur", "motionBlur", "Blurs what moves while the camera's shutter is open. It takes more time and memory; leave it off for sharp frames and previews.", VtValue(false)));
    add("advanced", setting("cycles:integrator:use_pixel_jitter", "usePixelJitter", "Shifts the camera rays within each pixel by an offset that changes at each update, for effects that combine several frames. Most renders leave it off. With Use Custom Pixel Jitter Sample on as well, the offset is the one in Custom Pixel Jitter Sample, if that holds at least two numbers.", VtValue(false)));
    add("advanced", setting("cycles:integrator:use_custom_pixel_jitter_sample", "useCustomPixelJitterSample", "Uses the offset in Custom Pixel Jitter Sample rather than one that changes. Most renders leave it off. It has an effect with Use Pixel Jitter on as well, and with fewer than two numbers in Custom Pixel Jitter Sample, the offset still changes.", VtValue(false)));
    add("advanced", setting("cycles:integrator:custom_pixel_jitter_sample", "customPixelJitterSample", "The offset within the pixel, as two numbers separated by a space, for example 0.5 0.5. Use it to place a sample exactly. It is ignored with fewer than two numbers, or unless Use Pixel Jitter and Use Custom Pixel Jitter Sample are both on.", VtValue(VtFloatArray())));

    add("sampleSubset", setting("cycles:sample_offset", "sampleOffset", "Skips this many samples and renders the rest up to Samples, so a later run can add to an earlier one without repeating its noise. An offset of Samples or more renders nothing; 0 renders them all. This is the control to use in this group.", VtValue(0)));
    add("sampleSubset", setting("cycles:integrator:use_sample_subset", "useSampleSubset", "This has no effect: each update overwrites it from Sample Offset, so use Sample Offset instead.", VtValue(false)));
    add("sampleSubset", setting("cycles:integrator:sample_subset_offset", "sampleSubsetOffset", "This has no effect: each update overwrites it from Sample Offset, so use Sample Offset instead.", VtValue(0)));
    add("sampleSubset", setting("cycles:integrator:sample_subset_length", "sampleSubsetLength", "This has no effect: each update overwrites it, so use Samples and Sample Offset instead.", VtValue(1 << 24)));

    const auto enabledBy = [&c](const char* key,
                                const char* controller,
                                bool inverted = false) {
        for (RenderSettingsKnobs::CuratedSetting& s : c.settings) {
            if (s.key == std::string("cycles:integrator:") + key) {
                RenderSettingsKnobs::EnabledBy condition;
                condition.key = std::string("cycles:integrator:") + controller;
                condition.inverted = inverted;
                s.enabledBy.push_back(condition);
            }
        }
    };

    enabledBy("adaptive_threshold", "use_adaptive_sampling");
    enabledBy("adaptive_min_samples", "use_adaptive_sampling");
    for (const char* key : { "denoiser_quality", "denoiser_prefilter", "denoiser_passes", "denoise_start_sample", "denoiser_upscale_factor" }) {
        enabledBy(key, "use_denoise");
    }
    for (const char* key : { "max_diffuse_bounce", "max_glossy_bounce", "max_transmission_bounce", "max_volume_bounce" }) {
        enabledBy(key, "max_bounce");
    }
    enabledBy("light_sampling_threshold", "use_light_tree", true);
    enabledBy("volume_max_steps", "volume_ray_marching");
    enabledBy("volume_step_rate", "volume_ray_marching");
    for (const char* key : { "deterministic_guiding", "use_surface_guiding", "surface_guiding_probability", "use_volume_guiding", "volume_guiding_probability",
                             "guiding_training_samples", "use_guiding_direct_light", "use_guiding_mis_weights", "guiding_distribution_type",
                             "guiding_directional_sampling_type", "guiding_roughness_threshold" }) {
        enabledBy(key, "use_guiding");
    }
    enabledBy("surface_guiding_probability", "use_surface_guiding");
    enabledBy("volume_guiding_probability", "use_volume_guiding");
    enabledBy("custom_pixel_jitter_sample", "use_custom_pixel_jitter_sample");
    enabledBy("sample_subset_offset", "use_sample_subset");
    enabledBy("sample_subset_length", "use_sample_subset");

    // Cycles sets this from its tile count on every sync, so a value sent here is overwritten.
    c.excludedKeys.push_back("cycles:film:use_sample_count");
    // Only Cycles' display driver reads these, and it runs on Windows only: Natron reads every
    // pass through the output driver, by name, as these leave it.
    c.excludedKeys.push_back("cycles:film:display_pass");
    c.excludedKeys.push_back("cycles:film:show_active_pixels");

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
