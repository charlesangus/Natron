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

#ifndef Engine_Nodes_Scene_RenderSettingsKnobs_h
#define Engine_Nodes_Scene_RenderSettingsKnobs_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include <pxr/base/vt/value.h>

NATRON_NAMESPACE_ENTER

/**
 * @brief The knobs of the settings Hydra render delegates declare
 * (HdRenderDelegate::GetRenderSettingDescriptors): for each renderer, an optional curated group of
 * hand-labelled knobs and an "Other Settings" group with a knob for every other declared setting,
 * on a page of the renderer's own. Every renderer's page is shown.
 *
 * A knob's script name is the renderer's prefix (scriptPrefix()) followed by the setting's name in
 * camel case; its label is that name without the prefix, made readable (labelFromScriptName()).
 **/
class RenderSettingsKnobs {
public:
    typedef std::map<std::string, PXR_NS::VtValue> Values;

    struct Descriptor {
        std::string key;
        std::string name;
        PXR_NS::VtValue defaultValue;
    };
    typedef std::vector<Descriptor> DescriptorList;

    // An entry of a curated menu and the value it sends.
    struct Choice {
        std::string id;
        std::string label;
        PXR_NS::VtValue value;
    };

    // A condition on another curated setting of the same renderer: a bool holds when true, a
    // number when above zero; inverted flips it.
    struct EnabledBy {
        std::string key;
        bool inverted = false;
    };

    struct CuratedSetting {
        std::string key;
        // Without the renderer's prefix, which the knob's script name gains.
        std::string name;
        std::string tooltip;
        // The setting's type, and its default when the delegate does not declare the key or when
        // overrideDefault is set; otherwise the delegate's declared default is the knob's.
        PXR_NS::VtValue defaultValue;
        bool overrideDefault = false;
        // A menu when not empty: the value sent is the chosen entry's, or the declared default
        // for an entry with an empty value.
        std::vector<Choice> choices;
        // The knob is shown disabled, this appended to its tooltip, when not empty.
        std::string disabledReason;
        // The key of another curated setting of the same renderer, listed before this one, whose
        // value this one takes.
        std::string follows;
        // The knob is enabled only while every condition holds. Ignored with disabledReason, which
        // keeps the knob disabled.
        std::vector<EnabledBy> enabledBy;
        double minimum = 0.;
        double displayMaximum = 0.;
        bool hasRange = false;
        // A hard maximum as well, with hasRange.
        double maximum = 0.;
        bool hasMaximum = false;
        // The name of the GroupSpec the knob goes in; empty for the curated group.
        std::string group;
        // The next setting's knob shares this one's row.
        bool sameRowAsNext = false;
    };

    // A named section of the renderer's page. Its script name is the renderer's prefix followed by name.
    struct GroupSpec {
        std::string name;
        // The name of the group this one is inside, empty for one directly on the page. A parent
        // cannot itself have a parent.
        std::string parent;
        // Whether the group starts expanded.
        bool open = true;
        std::string tooltip;
    };

    struct Curated {
        // The group's tooltip.
        std::string tooltip;
        // The page's label; empty for the delegate's own display name.
        std::string pageLabel;
        std::vector<CuratedSetting> settings;
        // The sections settings name in CuratedSetting::group. One is made when its first setting
        // (or a setting of a group inside it) is added, so the order of settings sets the order of groups.
        std::vector<GroupSpec> groups;
        // Declared keys Natron sets itself or keeps off, given no knob.
        std::vector<std::string> excludedKeys;
        // Declared keys of the "Other Settings" group whose knob is shown disabled, with the reason.
        std::vector<std::pair<std::string, std::string>> disabledKeys;
    };

    /**
     * @brief What the renderer's delegate declares, from a delegate made once per process; empty
     * when it is not installed, cannot render here or could not be made.
     **/
    static DescriptorList descriptors(const std::string& rendererId);

    // What the same delegate returns from HdRenderDelegate::GetRenderSettingsNamespaces().
    static std::vector<std::string> declaredNamespaces(const std::string& rendererId);

    /**
     * @brief The name of the group a renderer without a curated table puts a setting of this
     * key in, from the token prefixes of the key's last segment (split on '_' and camel case);
     * empty when no rule matches, which means "Other Settings".
     **/
    static std::string classifyKey(const std::string& key);

    // The curated group of the renderer: Cycles' or MoonRay's, null for any other.
    static const Curated* curatedFor(const std::string& rendererId);

    /**
     * @brief Whether curated's groups can be made: names are non-empty and unique, none is
     * "Settings" or "OtherSettings" or a curated setting's name, each parent is a group of
     * the table without a parent of its own, and each setting's group is in the table. Otherwise
     * false with *errorMessage set; addRenderer() then ignores the groups and the settings
     * all go in the curated group.
     **/
    static bool validateGroups(const Curated& curated, std::string* errorMessage);

    // Every available delegate's declared settings, one per line: renderer, key, type, default, name.
    static std::string dump();

    // The ids of the renderers that get knobs: the available delegates and those of setDescriptorsForTests().
    static std::vector<std::string> rendererIds();

    // Makes curatedFor(rendererId) return curated, null for no override.
    static void setCuratedForTests(const std::string& rendererId, const Curated* curated);

    // Makes descriptors(rendererId) return list, and rendererIds() name it; an empty list undoes it.
    static void setDescriptorsForTests(const std::string& rendererId, const DescriptorList& list);

    // "HdCyclesPlugin" -> "cycles": the id without "Hd" and "RendererPlugin" or "Plugin", in camel case.
    static std::string scriptPrefix(const std::string& rendererId);

    // "cyclesMaxDiffuseBounce" with prefix "cycles" -> "Max Diffuse Bounce".
    static std::string labelFromScriptName(const std::string& scriptName, const std::string& prefix);

    // "cycles:integrator:max_diffuse_bounce" -> "maxDiffuseBounce".
    static std::string camelCaseFromKey(const std::string& key, bool wholeKey);

    // The text a value is stored as, and whether value's type has a knob.
    static std::string valueText(const PXR_NS::VtValue& value);
    static bool isSupported(const PXR_NS::VtValue& value);

    explicit RenderSettingsKnobs(KnobHolder* holder);
    ~RenderSettingsKnobs();

    /**
     * @brief Makes the renderer's page, named scriptPrefix(rendererId) and labelled with the
     * renderer's name, holding its groups.
     **/
    void addRenderer(const std::string& rendererId, const DescriptorList& declared, const Curated* curated);

    // The page addRenderer() made for rendererId, null if none.
    KnobPagePtr page(const std::string& rendererId) const;

    // Sets the knobs of the settings that follow knob's; true if knob is one of the settings'.
    bool knobChanged(KnobI* knob);

    // Enables or disables each knob with enabledBy conditions by the current values of the knobs
    // they name. Only the knob is disabled, never its group, and values() is unaffected.
    void refreshEnabled();

    // The settings of rendererId to send its delegate: those that differ from the delegate's default.
    Values values(const std::string& rendererId) const;

    // The knob that reaches rendererId's key, null if none does.
    KnobIPtr knob(const std::string& rendererId, const std::string& key) const;

    // For each key rendererId declares, the script name of the knob reaching it, or empty if none does.
    std::vector<std::pair<std::string, std::string>> audit(const std::string& rendererId) const;

    /**
     * @brief Authors on prim every renderer's values() as attributes named by
     * RenderSettingsPrim::attributeName(). False with *errorMessage set if one cannot be authored.
     **/
    bool author(const PXR_NS::UsdPrim& prim, std::string* errorMessage) const;

private:
    struct Impl;
    std::unique_ptr<Impl> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_RenderSettingsKnobs_h
