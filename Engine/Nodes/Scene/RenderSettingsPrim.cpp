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

#include "RenderSettingsPrim.h"

#include <algorithm>
#include <set>

#include <pxr/imaging/hd/tokens.h>

#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/MoonRayAovs.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/RenderSettingsKnobs.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
bool
startsWithNamespace(const std::string& name,
                    const std::string& ns)
{
    return (name.size() > ns.size() + 1) && (name.compare(0, ns.size(), ns) == 0) && (name[ns.size()] == ':');
}

// The keys the delegate declares and the curated groups bind, and the Hydra keys it reads without
// declaring them, with the type each is sent as.
std::map<std::string, VtValue>
knownKeys(const std::string& rendererId)
{
    std::map<std::string, VtValue> keys;

    // Storm's skydome reads it from the delegate's settings, though Storm declares no such setting.
    if (RenderDelegates::isStorm(rendererId)) {
        keys[HdRenderSettingsTokens->domeLightCameraVisibility.GetString()] = VtValue(true);
    }
    if (const RenderSettingsKnobs::Curated* curated = RenderSettingsKnobs::curatedFor(rendererId)) {
        for (const RenderSettingsKnobs::CuratedSetting& c : curated->settings) {
            keys[c.key] = c.defaultValue;
        }
    }
    for (const RenderSettingsKnobs::Descriptor& d : RenderSettingsKnobs::descriptors(rendererId)) {
        keys[d.key] = d.defaultValue;
    }

    return keys;
}

// The attributes that name a key Natron sets itself or keeps off, in each of the renderer's namespaces.
std::set<std::string>
reservedAttributes(const std::string& rendererId)
{
    std::set<std::string> attributes;
    const RenderSettingsKnobs::Curated* curated = RenderSettingsKnobs::curatedFor(rendererId);

    if (!curated) {
        return attributes;
    }
    std::vector<std::string> keys = curated->excludedKeys;
    for (const std::pair<std::string, std::string>& disabled : curated->disabledKeys) {
        keys.push_back(disabled.first);
    }
    for (const RenderSettingsKnobs::CuratedSetting& s : curated->settings) {
        // A locked menu or follower still sends the value Natron picked for it, so only the rest are kept off.
        if (!s.disabledReason.empty() && s.follows.empty() && s.choices.empty()) {
            keys.push_back(s.key);
        }
    }
    const std::vector<std::string> spaces = RenderSettingsPrim::namespaces(rendererId);
    for (const std::string& key : keys) {
        for (const std::string& ns : spaces) {
            attributes.insert(startsWithNamespace(key, ns) ? key : ns + ":" + key);
        }
    }

    return attributes;
}
} // anon namespace

namespace RenderSettingsPrim {
std::vector<std::string>
namespaces(const std::string& rendererId)
{
    std::vector<std::string> out = RenderSettingsKnobs::declaredNamespaces(rendererId);

    if (!out.empty()) {
        return out;
    }
    if (CyclesAovs::isCycles(rendererId)) {
        out.push_back("cycles");
    } else if (MoonRayAovs::isMoonRay(rendererId)) {
        out.push_back("moonray");
    } else if (RenderDelegates::isStorm(rendererId)) {
        out.push_back("storm");
    } else {
        out.push_back(RenderSettingsKnobs::scriptPrefix(rendererId));
    }

    return out;
}

std::string
attributeName(const std::string& rendererId,
              const std::string& key)
{
    const std::vector<std::string> spaces = namespaces(rendererId);

    for (const std::string& ns : spaces) {
        if (startsWithNamespace(key, ns)) {
            return key;
        }
    }

    return spaces.front() + ":" + key;
}

std::string
keyFromAttribute(const std::string& rendererId,
                 const std::string& attributeName)
{
    for (const std::string& ns : namespaces(rendererId)) {
        if (!startsWithNamespace(attributeName, ns)) {
            continue;
        }
        const std::map<std::string, VtValue> keys = knownKeys(rendererId);
        const std::string bare = attributeName.substr(ns.size() + 1);
        if (!keys.count(attributeName) && keys.count(bare)) {
            return bare;
        }

        return attributeName;
    }

    return std::string();
}

Values
settings(const UsdPrim& prim,
         const std::string& rendererId,
         UsdTimeCode time)
{
    Values out;

    if (!prim) {
        return out;
    }
    TfTokenVector spaces;
    for (const std::string& ns : namespaces(rendererId)) {
        spaces.push_back(TfToken(ns));
    }
    const std::map<std::string, VtValue> keys = knownKeys(rendererId);
    const std::set<std::string> reserved = reservedAttributes(rendererId);
    // UsdRenderComputeNamespacedSettings reads default values only, so each attribute, including
    // one with time samples alone, is read again at time.
    std::map<std::string, VtValue> found;
    for (const std::pair<const std::string, VtValue>& entry : UsdRenderComputeNamespacedSettings(prim, spaces)) {
        found[entry.first] = entry.second;
    }
    for (const UsdAttribute& attribute : prim.GetAuthoredAttributes()) {
        VtValue atTime;
        if (!keyFromAttribute(rendererId, attribute.GetName().GetString()).empty() && attribute.Get(&atTime, time) && !atTime.IsEmpty()) {
            found[attribute.GetName().GetString()] = atTime;
        }
    }
    for (const std::pair<const std::string, VtValue>& entry : found) {
        const std::string key = keyFromAttribute(rendererId, entry.first);
        if (key.empty() || reserved.count(entry.first)) {
            continue;
        }
        VtValue value = entry.second;
        std::map<std::string, VtValue>::const_iterator declared = keys.find(key);
        if ((declared != keys.end()) && !declared->second.IsEmpty() && (value.GetType() != declared->second.GetType())) {
            const VtValue cast = VtValue::CastToTypeOf(value, declared->second);
            if (!cast.IsEmpty()) {
                value = cast;
            }
        }
        out[key] = value;
    }

    return out;
}

UsdRenderSettings
activePrim(const UsdStageRefPtr& stage,
           const std::string& overridePath,
           std::string* errorMessage)
{
    if (!stage) {
        return UsdRenderSettings();
    }
    std::string path = overridePath;
    std::string from = "The chosen settings prim";
    if (path.empty()) {
        if (!stage->HasAuthoredMetadata(UsdRenderTokens->renderSettingsPrimPath) || !stage->GetMetadata(UsdRenderTokens->renderSettingsPrimPath, &path) || path.empty()) {
            return UsdRenderSettings();
        }
        from = "The stage's renderSettingsPrimPath";
    }
    UsdRenderSettings settings;
    if (SdfPath::IsValidPathString(path)) {
        settings = UsdRenderSettings(stage->GetPrimAtPath(SdfPath(path)));
    }
    if (!settings || !settings.GetPrim().IsA<UsdRenderSettings>()) {
        if (errorMessage) {
            *errorMessage = from + " " + path + " is not a RenderSettings prim of the scene.";
        }

        return UsdRenderSettings();
    }

    return settings;
}

StandardAttributes
standardAttributes(const UsdRenderSettings& settings)
{
    StandardAttributes out;

    if (!settings) {
        return out;
    }
    SdfPathVector targets;
    if (settings.GetCameraRel().GetForwardedTargets(&targets) && !targets.empty()) {
        out.camera = targets.front();
    }
    (void)settings.GetResolutionAttr().Get(&out.resolution);
    (void)settings.GetPixelAspectRatioAttr().Get(&out.pixelAspectRatio);
    (void)settings.GetAspectRatioConformPolicyAttr().Get(&out.aspectRatioConformPolicy);
    (void)settings.GetDataWindowNDCAttr().Get(&out.dataWindowNDC);
    (void)settings.GetDisableMotionBlurAttr().Get(&out.disableMotionBlur);
    (void)settings.GetDisableDepthOfFieldAttr().Get(&out.disableDepthOfField);

    return out;
}

bool
rendererFor(const UsdPrim& prim,
            std::string* rendererId,
            std::string* errorMessage)
{
    rendererId->clear();
    const UsdAttribute attribute = prim ? prim.GetAttribute(TfToken(kRenderSettingsPrimRendererAttribute)) : UsdAttribute();
    TfToken id;
    if (!attribute || !attribute.HasAuthoredValue() || !attribute.Get(&id) || id.IsEmpty()) {
        return true;
    }
    RenderDelegates::Delegate delegate;
    if (!RenderDelegates::find(id.GetString(), &delegate)) {
        *errorMessage = "The render settings prim " + prim.GetPath().GetString() + " names the renderer " + id.GetString() + " in " kRenderSettingsPrimRendererAttribute ", which is not installed: no Hydra render delegate of "
                                                                                                                             "that name is on the plug-in path.";

        return false;
    }
    *rendererId = id.GetString();

    return true;
}
} // namespace RenderSettingsPrim

NATRON_NAMESPACE_EXIT
