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
