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

    c.tooltip = "The settings Cycles declares.";
    c.groups.push_back(groupSpec("sampling", "", true));
    c.groups.push_back(groupSpec("denoise", "", true));
    c.groups.push_back(groupSpec("lightPaths", "", true));
    c.groups.push_back(groupSpec("bounces", "lightPaths", true));
    c.groups.push_back(groupSpec("clamping", "lightPaths", true));
    c.groups.push_back(groupSpec("caustics", "lightPaths", false));
    c.groups.push_back(groupSpec("fastGi", "lightPaths", false));
    c.groups.push_back(groupSpec("lights", "", false));
    c.groups.push_back(groupSpec("volumes", "", false));
    c.groups.push_back(groupSpec("pathGuiding", "", false));
    c.groups.push_back(groupSpec("lightContributions", "", false));
    c.groups.push_back(groupSpec("advanced", "", false));
    c.groups.push_back(groupSpec("sampleSubset", "advanced", false));

    const auto add = [&c](const char* group,
                          RenderSettingsKnobs::CuratedSetting s) {
        s.group = group;
        c.settings.push_back(s);
    };

    add("sampling", ranged(setting("cycles:samples", "samples", "The samples per pixel a render takes before it stops.", VtValue(1024)), 1, 4096));
    add("sampling", setting("cycles:integrator:use_adaptive_sampling", "useAdaptiveSampling", "Use Adaptive Sampling", VtValue(true)));
    add("sampling", setting("cycles:integrator:adaptive_threshold", "adaptiveThreshold", "Adaptive Threshold", VtValue(0.01f)));
    add("sampling", setting("cycles:integrator:adaptive_min_samples", "adaptiveMinSamples", "Adaptive Min Samples", VtValue(0)));
    add("sampling", setting("cycles:time_limit", "timeLimit", "Time Limit", VtValue(0.0)));
    add("sampling", setting("cycles:integrator:seed", "seed", "The seed of the sampling pattern: renders that differ only in it have different noise.", VtValue(0)));

    add("denoise", setting("cycles:integrator:use_denoise", "useDenoise", "Denoises the render once it has its samples.", VtValue(false)));
    {
        RenderSettingsKnobs::CuratedSetting s = setting("cycles:integrator:denoiser_type", "denoiserType",
                                                        "The denoiser Use Denoise runs.", VtValue(0));
        RenderSettingsKnobs::Choice oidn;
        oidn.id = "openimagedenoise";
        oidn.label = "OpenImageDenoise";
        s.choices.push_back(oidn);
        s.disabledReason = "OpenImageDenoise is the only denoiser: OptiX needs CUDA, which this build of Cycles lacks.";
        add("denoise", s);
    }
    add("denoise", menu(setting("cycles:integrator:denoiser_quality", "denoiserQuality", "Denoiser Quality", VtValue(1)), { { "high", "High", VtValue(1) }, { "balanced", "Balanced", VtValue(2) }, { "fast", "Fast", VtValue(3) } }));
    add("denoise", menu(setting("cycles:integrator:denoiser_prefilter", "denoiserPrefilter", "Denoiser Prefilter", VtValue(3)), { { "none", "None", VtValue(1) }, { "fast", "Fast", VtValue(2) }, { "accurate", "Accurate", VtValue(3) } }));
    add("denoise", setting("cycles:integrator:denoiser_passes", "denoiserPasses", "Denoiser Passes", VtValue(5)));
    add("denoise", setting("cycles:integrator:denoise_start_sample", "denoiseStartSample", "Start Sample to Denoise", VtValue(0)));
    add("denoise", setting("cycles:integrator:denoiser_upscale_factor", "denoiserUpscaleFactor", "Denoiser Upscale Factor", VtValue(1.f)));
    {
        RenderSettingsKnobs::CuratedSetting s = setting("cycles:integrator:denoise_use_gpu", "denoiseUseGpu", "Denoise on GPU", VtValue(true));
        s.disabledReason = "Cycles renders and denoises on the CPU here.";
        add("denoise", s);
    }

    add("bounces", ranged(setting("cycles:integrator:max_bounce", "maxBounce", "The most bounces a path takes, whatever their kind.", VtValue(7)), 0, 32));
    add("bounces", ranged(setting("cycles:integrator:max_diffuse_bounce", "maxDiffuseBounce", "The most diffuse bounces a path takes.", VtValue(7)), 0, 32));
    add("bounces", ranged(setting("cycles:integrator:max_glossy_bounce", "maxGlossyBounce", "The most glossy bounces a path takes.", VtValue(7)), 0, 32));
    add("bounces", ranged(setting("cycles:integrator:max_transmission_bounce", "maxTransmissionBounce", "The most transmission bounces a path takes.", VtValue(7)), 0, 32));
    add("bounces", ranged(setting("cycles:integrator:max_volume_bounce", "maxVolumeBounce", "The most volume scattering bounces a path takes.", VtValue(7)), 0, 32));
    add("bounces", ranged(setting("cycles:integrator:transparent_max_bounce", "transparentMaxBounce", "The most transparent surfaces a path passes through.", VtValue(7)), 0, 64));

    add("clamping", ranged(setting("cycles:integrator:sample_clamp_direct", "sampleClampDirect", "The brightest a sample of direct light may be; 0 does not clamp.", VtValue(0.f)), 0, 100));
    add("clamping", ranged(setting("cycles:integrator:sample_clamp_indirect", "sampleClampIndirect", "The brightest a sample of indirect light may be, against fireflies; 0 does not clamp.", VtValue(10.f)), 0, 100));

    add("caustics", setting("cycles:integrator:caustics_reflective", "causticsReflective", "Reflective Caustics", VtValue(true)));
    add("caustics", setting("cycles:integrator:caustics_refractive", "causticsRefractive", "Refractive Caustics", VtValue(true)));
    add("caustics", setting("cycles:integrator:filter_glossy", "filterGlossy", "Filter Glossy", VtValue(0.f)));

    add("fastGi", setting("cycles:integrator:ao_bounces", "aoBounces", "AO Bounces", VtValue(0)));
    add("fastGi", setting("cycles:integrator:ao_factor", "aoFactor", "AO Factor", VtValue(0.f)));
    add("fastGi", setting("cycles:integrator:ao_distance", "aoDistance", "AO Distance", VtValue(std::numeric_limits<float>::max())));
    add("fastGi", setting("cycles:integrator:ao_additive_factor", "aoAdditiveFactor", "AO Additive Factor", VtValue(0.f)));

    // direct_light_sampling_type has no entry: Cycles declares it only when built with
    // WITH_CYCLES_DEBUG, and a knob here would be a control the delegate ignores.
    add("lights", setting("cycles:integrator:use_light_tree", "useLightTree", "Samples the lights that matter most to each point, which helps scenes with many "
                                                                              "lights.",
                          VtValue(true)));
    add("lights", setting("cycles:integrator:light_sampling_threshold", "lightSamplingThreshold", "Light Sampling Threshold", VtValue(0.f)));

    add("volumes", setting("cycles:integrator:volume_ray_marching", "volumeRayMarching", "Biased", VtValue(false)));
    add("volumes", setting("cycles:integrator:volume_max_steps", "volumeMaxSteps", "Volume Max Steps", VtValue(1024)));
    add("volumes", setting("cycles:integrator:volume_step_rate", "volumeStepRate", "Volume Step Rate", VtValue(1.f)));

    add("pathGuiding", setting("cycles:integrator:use_guiding", "useGuiding", "Guiding", VtValue(false)));
    add("pathGuiding", setting("cycles:integrator:deterministic_guiding", "deterministicGuiding", "Deterministic Guiding", VtValue(true)));
    add("pathGuiding", setting("cycles:integrator:use_surface_guiding", "useSurfaceGuiding", "Surface Guiding", VtValue(true)));
    add("pathGuiding", setting("cycles:integrator:surface_guiding_probability", "surfaceGuidingProbability", "Surface Guiding Probability", VtValue(0.5f)));
    add("pathGuiding", setting("cycles:integrator:use_volume_guiding", "useVolumeGuiding", "Volume Guiding", VtValue(true)));
    add("pathGuiding", setting("cycles:integrator:volume_guiding_probability", "volumeGuidingProbability", "Volume Guiding Probability", VtValue(0.5f)));
    add("pathGuiding", setting("cycles:integrator:guiding_training_samples", "guidingTrainingSamples", "Training Samples", VtValue(128)));
    add("pathGuiding", setting("cycles:integrator:use_guiding_direct_light", "useGuidingDirectLight", "Guide Direct Light", VtValue(true)));
    add("pathGuiding", setting("cycles:integrator:use_guiding_mis_weights", "useGuidingMisWeights", "Use MIS Weights", VtValue(true)));
    add("pathGuiding", menu(setting("cycles:integrator:guiding_distribution_type", "guidingDistributionType", "Guiding Distribution Type", VtValue(0)), { { "PARALLAX_AWARE_VMM", "Parallax-Aware VMM", VtValue(0) }, { "DIRECTIONAL_QUAD_TREE", "Directional Quad Tree", VtValue(1) }, { "VMM", "VMM", VtValue(2) } }));
    add("pathGuiding", menu(setting("cycles:integrator:guiding_directional_sampling_type", "guidingDirectionalSamplingType", "Guiding Directional Sampling Type", VtValue(1)), { { "MIS", "Product MIS", VtValue(0) }, { "RIS", "RIS", VtValue(1) }, { "ROUGHNESS", "Roughness", VtValue(2) } }));
    add("pathGuiding", setting("cycles:integrator:guiding_roughness_threshold", "guidingRoughnessThreshold", "Guiding Roughness Threshold", VtValue(0.05f)));

    add("lightContributions", setting("cycles:integrator:use_direct_light", "useDirectLight", "Use Direct Light", VtValue(true)));
    add("lightContributions", setting("cycles:integrator:use_indirect_light", "useIndirectLight", "Use Indirect Light", VtValue(true)));
    add("lightContributions", setting("cycles:integrator:use_diffuse", "useDiffuse", "Use Diffuse", VtValue(true)));
    add("lightContributions", setting("cycles:integrator:use_glossy", "useGlossy", "Use Glossy", VtValue(true)));
    add("lightContributions", setting("cycles:integrator:use_transmission", "useTransmission", "Use Transmission", VtValue(true)));
    add("lightContributions", setting("cycles:integrator:use_emission", "useEmission", "Use Emission", VtValue(true)));

    // The enum's values are not in the order Cycles lists them: Round is 4 and First is 3.
    add("advanced", menu(setting("cycles:integrator:sampling_pattern", "samplingPattern", "Sampling Pattern", VtValue(1)), { { "sobol_burley", "Sobol-Burley", VtValue(0) }, { "tabulated_sobol", "Tabulated Sobol", VtValue(1) }, { "blue_noise_pure", "Blue Noise Pure", VtValue(2) }, { "blue_noise_round", "Blue Noise Round", VtValue(4) }, { "blue_noise_first", "Blue Noise First", VtValue(3) } }));
    add("advanced", setting("cycles:integrator:scrambling_distance", "scramblingDistance", "Scrambling Distance", VtValue(1.f)));
    add("advanced", setting("cycles:integrator:aa_samples", "aaSamples", "AA Samples", VtValue(0)));
    add("advanced", setting("cycles:integrator:min_bounce", "minBounce", "Min Bounce", VtValue(0)));
    add("advanced", setting("cycles:integrator:transparent_min_bounce", "transparentMinBounce", "Transparent Min Bounce", VtValue(0)));
    add("advanced", setting("cycles:integrator:motion_blur", "motionBlur", "Motion Blur", VtValue(false)));
    add("advanced", setting("cycles:integrator:use_pixel_jitter", "usePixelJitter", "Use Pixel Jitter", VtValue(false)));
    add("advanced", setting("cycles:integrator:use_custom_pixel_jitter_sample", "useCustomPixelJitterSample", "Use custom pixel jitter sample value", VtValue(false)));
    add("advanced", setting("cycles:integrator:custom_pixel_jitter_sample", "customPixelJitterSample", "Custom pixel jitter sample overwrite value", VtValue(VtFloatArray())));

    add("sampleSubset", setting("cycles:sample_offset", "sampleOffset", "Sample Offset", VtValue(0)));
    add("sampleSubset", setting("cycles:integrator:use_sample_subset", "useSampleSubset", "Use Sample Subset", VtValue(false)));
    add("sampleSubset", setting("cycles:integrator:sample_subset_offset", "sampleSubsetOffset", "Sample Subset Offset", VtValue(0)));
    add("sampleSubset", setting("cycles:integrator:sample_subset_length", "sampleSubsetLength", "Sample Subset Length", VtValue(1 << 24)));

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
