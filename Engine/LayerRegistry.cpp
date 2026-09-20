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

#include "LayerRegistry.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

#include <QMutexLocker>

NATRON_NAMESPACE_ENTER

static bool
equalsCaseInsensitive(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

static bool
matchesAnyCaseInsensitive(const std::string& id, const char* const* candidates, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i) {
        if (equalsCaseInsensitive(id, candidates[i])) {
            return true;
        }
    }
    return false;
}

static std::string
joinChannels(const std::vector<std::string>& channels)
{
    std::string ret;
    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (i > 0) {
            ret += ',';
        }
        ret += channels[i];
    }
    return ret;
}

const ImageLayerDesc*
LayerRegistry::reservedAlias(const std::string& id)
{
    static const char* const aliases[] = { "Color", "rgba", "rgb", "alpha", "RGBA", "RGB", "A", "XY" };

    if (matchesAnyCaseInsensitive(id, aliases, sizeof(aliases) / sizeof(aliases[0]))) {
        static const ImageLayerDesc colorAlias = ImageLayerDesc::getRGBAComponents();
        return &colorAlias;
    }
    return 0;
}

static bool
isRefusedReservedName(const std::string& id)
{
    static const char* const refused[] = {
        "none", "all", "Backward", "Forward", "DisparityLeft", "DisparityRight", "Motion", "Disparity"
    };

    return matchesAnyCaseInsensitive(id, refused, sizeof(refused) / sizeof(refused[0]));
}

static bool
isValidChannelName(const std::string& s)
{
    if (s.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
            return false;
        }
    }
    return true;
}

bool
LayerRegistry::validate(const ImageLayerDesc& desc, bool fromFile, std::string* error)
{
    const std::string& id = desc.getLayerID();

    if (id.empty()) {
        if (error) {
            *error = "A layer ID cannot be empty.";
        }
        return false;
    }
    for (std::size_t i = 0; i < id.size(); ++i) {
        if (std::isspace(static_cast<unsigned char>(id[i]))) {
            if (error) {
                *error = "A layer ID cannot contain whitespace: \"" + id + "\".";
            }
            return false;
        }
    }
    if (id.front() == '.' || id.back() == '.') {
        if (error) {
            *error = "A layer ID cannot start or end with a dot: \"" + id + "\".";
        }
        return false;
    }
    if (!fromFile && id.find('.') != std::string::npos) {
        if (error) {
            *error = "A layer ID cannot contain a dot: \"" + id + "\".";
        }
        return false;
    }

    const std::vector<std::string>& channels = desc.getChannels();
    if (channels.empty() || (int)channels.size() > LayerRegistry::kLayerMaxChannels) {
        if (error) {
            *error = "A layer must have between 1 and 4 channels.";
        }
        return false;
    }

    std::set<std::string> seen;
    for (std::size_t i = 0; i < channels.size(); ++i) {
        const std::string& ch = channels[i];
        if (!isValidChannelName(ch)) {
            if (error) {
                *error = "Invalid channel name: \"" + ch + "\".";
            }
            return false;
        }
        if (!seen.insert(ch).second) {
            if (error) {
                *error = "Duplicate channel name: \"" + ch + "\".";
            }
            return false;
        }
    }

    return true;
} // validate

LayerRegistry::LayerRegistry()
    : _mutex()
    , _entries()
{
    std::vector<LayerRegistryEntry> initial;

    static const ImageLayerDesc* const builtins[] = {
        &ImageLayerDesc::getRGBAComponents(),
        &ImageLayerDesc::getDisparityLeftComponents(),
        &ImageLayerDesc::getDisparityRightComponents(),
        &ImageLayerDesc::getBackwardMotionComponents(),
        &ImageLayerDesc::getForwardMotionComponents()
    };
    for (std::size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); ++i) {
        LayerRegistryEntry e;
        e.desc = *builtins[i];
        e.origin = LayerRegistryEntry::eOriginBuiltin;
        initial.push_back(e);
    }

    {
        static const char* depthChannels[1] = { "Z" };
        LayerRegistryEntry e;
        e.desc = ImageLayerDesc("depth", "depth", "", depthChannels, 1);
        e.origin = LayerRegistryEntry::eOriginUser;
        initial.push_back(e);
    }

    _entries.reset(new std::vector<LayerRegistryEntry>(initial));
}

LayerRegistry::~LayerRegistry()
{
}

std::shared_ptr<const std::vector<LayerRegistryEntry>>
LayerRegistry::snapshot() const
{
    QMutexLocker l(&_mutex);
    return _entries;
}

bool
LayerRegistry::contains(const std::string& id) const
{
    ImageLayerDesc unused;
    return find(id, &unused);
}

bool
LayerRegistry::find(const std::string& id, ImageLayerDesc* out) const
{
    std::shared_ptr<const std::vector<LayerRegistryEntry>> snap = snapshot();
    for (std::size_t i = 0; i < snap->size(); ++i) {
        if ((*snap)[i].desc.getLayerID() == id) {
            if (out) {
                *out = (*snap)[i].desc;
            }
            return true;
        }
    }
    return false;
}

LayerRegistry::AddResultEnum
LayerRegistry::add(const ImageLayerDesc& descIn, LayerRegistryEntry::OriginEnum origin, std::string* error)
{
    const std::string& id = descIn.getLayerID();

    if (reservedAlias(id)) {
        return eAddResultUnchanged;
    }
    if (isRefusedReservedName(id)) {
        if (error) {
            *error = "\"" + id + "\" is a reserved name and cannot be used for a layer.";
        }
        return eAddResultRefused;
    }

    const bool fromFile = (origin == LayerRegistryEntry::eOriginFile);
    if (!validate(descIn, fromFile, error)) {
        return eAddResultRefused;
    }

    // Non-built-in layers always display as typed: label == ID.
    const ImageLayerDesc desc(id, id, descIn.getChannelsLabel(), descIn.getChannels());

    QMutexLocker l(&_mutex);
    std::shared_ptr<const std::vector<LayerRegistryEntry>> current = _entries;

    for (std::size_t i = 0; i < current->size(); ++i) {
        const LayerRegistryEntry& existingEntry = (*current)[i];
        if (existingEntry.desc.getLayerID() != id) {
            continue;
        }

        const std::vector<std::string>& existingChannels = existingEntry.desc.getChannels();
        const std::vector<std::string>& newChannels = desc.getChannels();
        if (existingChannels == newChannels) {
            return eAddResultUnchanged;
        }

        if (existingEntry.origin != LayerRegistryEntry::eOriginFile) {
            if (error) {
                *error = "A layer named \"" + id + "\" already exists with channels " + joinChannels(existingChannels) + ".";
            }
            return eAddResultRefused;
        }

        std::vector<std::string> unioned = existingChannels;
        for (std::size_t c = 0; c < newChannels.size(); ++c) {
            if (std::find(unioned.begin(), unioned.end(), newChannels[c]) == unioned.end()) {
                unioned.push_back(newChannels[c]);
            }
        }
        if ((int)unioned.size() > kLayerMaxChannels) {
            if (error) {
                *error = "A layer named \"" + id + "\" cannot grow past " + std::to_string(kLayerMaxChannels) + " channels.";
            }
            return eAddResultRefused;
        }

        std::shared_ptr<std::vector<LayerRegistryEntry>> next(new std::vector<LayerRegistryEntry>(*current));
        (*next)[i].desc = ImageLayerDesc(id, id, "", unioned);
        _entries = next;
        return eAddResultGrown;
    }

    std::shared_ptr<std::vector<LayerRegistryEntry>> next(new std::vector<LayerRegistryEntry>(*current));
    LayerRegistryEntry newEntry;
    newEntry.desc = desc;
    newEntry.origin = origin;
    next->push_back(newEntry);
    _entries = next;
    return eAddResultAdded;
} // add

bool
LayerRegistry::remove(const std::string& id, std::string* error)
{
    QMutexLocker l(&_mutex);
    std::shared_ptr<const std::vector<LayerRegistryEntry>> current = _entries;

    for (std::size_t i = 0; i < current->size(); ++i) {
        if ((*current)[i].desc.getLayerID() != id) {
            continue;
        }
        if ((*current)[i].origin == LayerRegistryEntry::eOriginBuiltin) {
            if (error) {
                *error = "\"" + id + "\" is a built-in layer and cannot be removed.";
            }
            return false;
        }
        std::shared_ptr<std::vector<LayerRegistryEntry>> next(new std::vector<LayerRegistryEntry>(*current));
        next->erase(next->begin() + i);
        _entries = next;
        return true;
    }

    if (error) {
        *error = "\"" + id + "\" is not a registered layer.";
    }
    return false;
} // remove

static bool
isBareColorChannelName(const std::string& s)
{
    return s == "R" || s == "G" || s == "B" || s == "A" || s == "I" || s == "Y";
}

namespace {

enum LayerGroupKindEnum {
    eLayerGroupKindColor,
    eLayerGroupKindDepth,
    eLayerGroupKindOther
};

struct LayerGroup {
    LayerGroupKindEnum kind;
    std::vector<std::string> channels;
};

} // anon namespace

void
LayerRegistry::groupChannelNames(const std::vector<std::string>& flat, std::vector<ImageLayerDesc>* layers)
{
    if (!layers) {
        return;
    }
    layers->clear();

    std::vector<std::string> order;
    std::map<std::string, LayerGroup> groups;

    // Distinct sentinel keys so a dotted layer literally named "Color" or "depth"
    // cannot collide with the bare-channel groupings of the same name.
    static const std::string colorKey("\x01"
                                      "Color");
    static const std::string depthKey("\x01"
                                      "depth");

    for (std::size_t i = 0; i < flat.size(); ++i) {
        const std::string& flatName = flat[i];
        std::size_t dot = flatName.find_last_of('.');

        std::string key, channel;
        LayerGroupKindEnum kind;
        if (dot == std::string::npos) {
            if (isBareColorChannelName(flatName)) {
                key = colorKey;
                channel = flatName;
                kind = eLayerGroupKindColor;
            } else if (flatName == "Z") {
                key = depthKey;
                channel = flatName;
                kind = eLayerGroupKindDepth;
            } else {
                key = flatName;
                channel = flatName;
                kind = eLayerGroupKindOther;
            }
        } else {
            key = flatName.substr(0, dot);
            channel = flatName.substr(dot + 1);
            kind = eLayerGroupKindOther;
        }

        std::map<std::string, LayerGroup>::iterator it = groups.find(key);
        if (it == groups.end()) {
            order.push_back(key);
            LayerGroup g;
            g.kind = kind;
            g.channels.push_back(channel);
            groups[key] = g;
        } else {
            it->second.channels.push_back(channel);
        }
    }

    static const char* const colorCanonicalOrder[4] = { "R", "G", "B", "A" };

    for (std::size_t i = 0; i < order.size(); ++i) {
        const LayerGroup& g = groups[order[i]];
        switch (g.kind) {
        case eLayerGroupKindColor: {
            std::vector<std::string> ordered;
            for (std::size_t c = 0; c < 4; ++c) {
                if (std::find(g.channels.begin(), g.channels.end(), colorCanonicalOrder[c]) != g.channels.end()) {
                    ordered.push_back(colorCanonicalOrder[c]);
                }
            }
            for (std::size_t c = 0; c < g.channels.size(); ++c) {
                if (std::find(ordered.begin(), ordered.end(), g.channels[c]) == ordered.end()) {
                    ordered.push_back(g.channels[c]);
                }
            }
            layers->push_back(ImageLayerDesc(kNatronColorLayerID, kNatronColorLayerLabel, "", ordered));
            break;
        }
        case eLayerGroupKindDepth:
            layers->push_back(ImageLayerDesc("depth", "depth", "", std::vector<std::string>(1, "Z")));
            break;
        case eLayerGroupKindOther:
            layers->push_back(ImageLayerDesc(order[i], order[i], "", g.channels));
            break;
        }
    }
} // groupChannelNames

NATRON_NAMESPACE_EXIT
