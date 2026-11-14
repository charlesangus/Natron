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

#include "UtilitySettings.h"

#include "Engine/Nodes/Scene/Utility/UtilityRenderDelegate.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
RenderSettingsKnobs::Curated
make()
{
    RenderSettingsKnobs::Curated c;
    RenderSettingsKnobs::CuratedSetting device;

    c.tooltip = "The settings Natron Utility declares.";
    device.key = kUtilitySettingDevice;
    device.name = "device";
    device.tooltip = "Which device renders. Auto uses the GPU when it supports ray queries and otherwise renders on the CPU. GPU forces the GPU and fails with an error if it cannot render. CPU forces the CPU path.";
    device.defaultValue = VtValue(TfToken(kUtilityDeviceAuto));
    device.choices = { { "auto", "Auto", VtValue(TfToken(kUtilityDeviceAuto)) }, { "gpu", "GPU", VtValue(TfToken(kUtilityDeviceGpu)) }, { "cpu", "CPU", VtValue(TfToken(kUtilityDeviceCpu)) } };
    c.settings.push_back(device);

    RenderSettingsKnobs::CuratedSetting samples;

    samples.key = kUtilitySettingPixelSamples;
    samples.name = "pixelSamples";
    samples.tooltip = "The side of the grid of samples each pixel takes: 3 takes 9 samples a pixel. More samples smooth edges and small detail and take longer. 1 takes one sample through the pixel's centre, with no filtering. The viewer shows the frame as its samples land.";
    samples.defaultValue = VtValue(int(kUtilityDefaultPixelSamples));
    samples.hasRange = true;
    samples.minimum = 1;
    samples.displayMaximum = 8;
    samples.hasMaximum = true;
    samples.maximum = kUtilityMaxPixelSamples;
    c.settings.push_back(samples);

    RenderSettingsKnobs::CuratedSetting filter;

    filter.key = kUtilitySettingPixelFilter;
    filter.name = "pixelFilter";
    filter.tooltip = "How a pixel's samples are weighted into its value. Box weighs them all alike and is the sharpest. Triangle, Gaussian and Blackman-Harris weigh samples near the pixel's centre more, trading sharpness for smoother edges; Blackman-Harris keeps the most detail of the three.";
    filter.defaultValue = VtValue(TfToken(kUtilityFilterBlackmanHarris));
    filter.choices = { { "box", "Box", VtValue(TfToken(kUtilityFilterBox)) }, { "triangle", "Triangle", VtValue(TfToken(kUtilityFilterTriangle)) }, { "gaussian", "Gaussian", VtValue(TfToken(kUtilityFilterGaussian)) }, { "blackmanHarris", "Blackman-Harris", VtValue(TfToken(kUtilityFilterBlackmanHarris)) } };
    c.settings.push_back(filter);

    RenderSettingsKnobs::CuratedSetting width;

    width.key = kUtilitySettingFilterWidth;
    width.name = "filterWidth";
    width.tooltip = "The width, in pixels, of the square a pixel's samples are spread over and the filter covers. Wider is softer; 1 with Box keeps every sample inside its pixel.";
    width.defaultValue = VtValue(kUtilityDefaultFilterWidth);
    width.hasRange = true;
    width.minimum = 0.01;
    width.displayMaximum = 4.;
    c.settings.push_back(width);

    RenderSettingsKnobs::CuratedSetting seed;

    seed.key = kUtilitySettingSeed;
    seed.name = "seed";
    seed.tooltip = "Picks the pattern the samples are drawn in. The same seed draws the same picture; change it for a frame whose remaining noise differs.";
    seed.defaultValue = VtValue(int(0));
    seed.hasRange = true;
    seed.minimum = 0;
    seed.displayMaximum = 1000;
    c.settings.push_back(seed);

    RenderSettingsKnobs::CuratedSetting overrideShutter;

    overrideShutter.key = kUtilitySettingOverrideShutter;
    overrideShutter.name = "overrideShutter";
    overrideShutter.tooltip = "Off, the shutter is the camera's Shutter Open and Shutter Close. On, Shutter, Shutter Offset and Shutter Custom Offset set it instead.";
    overrideShutter.defaultValue = VtValue(false);
    c.settings.push_back(overrideShutter);

    const RenderSettingsKnobs::EnabledBy byOverride = { kUtilitySettingOverrideShutter, false };
    RenderSettingsKnobs::CuratedSetting shutter;

    shutter.key = kUtilitySettingShutter;
    shutter.name = "shutter";
    shutter.tooltip = "How long the shutter stays open, in frames: 0.5 blurs over half a frame. 0 renders no motion blur.";
    shutter.defaultValue = VtValue(kUtilityDefaultShutter);
    shutter.hasRange = true;
    shutter.minimum = 0.;
    shutter.displayMaximum = 2.;
    shutter.enabledBy = { byOverride };
    c.settings.push_back(shutter);

    RenderSettingsKnobs::CuratedSetting offset;

    offset.key = kUtilitySettingShutterOffset;
    offset.name = "shutterOffset";
    offset.tooltip = "Where the shutter opens. Centred opens it half its length before the frame, Start at the frame, End so it closes at the frame, Custom Shutter Custom Offset frames from the frame.";
    offset.defaultValue = VtValue(TfToken(kUtilityShutterCentred));
    offset.choices = { { "centred", "Centred", VtValue(TfToken(kUtilityShutterCentred)) }, { "start", "Start", VtValue(TfToken(kUtilityShutterStart)) }, { "end", "End", VtValue(TfToken(kUtilityShutterEnd)) }, { "custom", "Custom", VtValue(TfToken(kUtilityShutterCustom)) } };
    offset.enabledBy = { byOverride };
    c.settings.push_back(offset);

    RenderSettingsKnobs::CuratedSetting customOffset;

    customOffset.key = kUtilitySettingShutterCustomOffset;
    customOffset.name = "shutterCustomOffset";
    customOffset.tooltip = "With Shutter Offset Custom, the frames from the frame at which the shutter opens; negative opens it before the frame.";
    customOffset.defaultValue = VtValue(0.f);
    customOffset.hasRange = true;
    customOffset.minimum = -2.;
    customOffset.displayMaximum = 2.;
    customOffset.enabledBy = { byOverride };
    c.settings.push_back(customOffset);

    RenderSettingsKnobs::CuratedSetting segments;

    segments.key = kUtilitySettingShutterSegments;
    segments.name = "shutterSegments";
    segments.tooltip = "How many times within the shutter the scene is traced at, each by its share of the pixel samples. More follow curved and fast motion more closely and take longer; there are never more than the pixel samples.";
    segments.defaultValue = VtValue(int(kUtilityDefaultShutterSegments));
    segments.hasRange = true;
    segments.minimum = 1;
    segments.displayMaximum = 16;
    c.settings.push_back(segments);

    const struct {
        const char* key;
        const char* name;
        const char* tooltip;
    } toggles[] = {
        { kUtilitySettingCameraMotionBlur, "cameraMotionBlur", "Whether the camera's own motion within the shutter blurs the frame." },
        { kUtilitySettingTransformMotionBlur, "transformMotionBlur", "Whether prims moved by their transforms, instances included, blur within the shutter." },
        { kUtilitySettingDeformationMotionBlur, "deformationMotionBlur", "Whether meshes whose points move, by time samples or velocities, blur within the shutter. A mesh whose topology changes within the shutter does not, and the render warns naming it." },
    };
    for (const auto& toggle : toggles) {
        RenderSettingsKnobs::CuratedSetting s;

        s.key = toggle.key;
        s.name = toggle.name;
        s.tooltip = toggle.tooltip;
        s.defaultValue = VtValue(true);
        c.settings.push_back(s);
    }

    return c;
}
} // anon namespace

namespace UtilitySettings {
const RenderSettingsKnobs::Curated&
curated()
{
    static const RenderSettingsKnobs::Curated c = make();

    return c;
}
} // namespace UtilitySettings

NATRON_NAMESPACE_EXIT
