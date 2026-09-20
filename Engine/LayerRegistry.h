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

#ifndef LAYERREGISTRY_H
#define LAYERREGISTRY_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <string>
#include <vector>

#include <QMutex>

#include "Engine/ImageLayerDesc.h"

NATRON_NAMESPACE_ENTER

struct LayerRegistryEntry {
    ImageLayerDesc desc;

    enum OriginEnum {
        eOriginBuiltin,
        eOriginUser,
        eOriginFile,
        eOriginPlugin
    };

    OriginEnum origin;
};

/**
 * @brief A project-level registry of the layers a script knows about, independent of
 * any node. Pure value class: no Qt signals, no Project wiring (see Project::addLayer
 * and projectLayersChanged()).
 **/
class LayerRegistry {
public:
    enum AddResultEnum {
        eAddResultAdded,
        eAddResultUnchanged,
        eAddResultGrown,
        eAddResultRefused
    };

    static const int kLayerMaxChannels = 4;

    LayerRegistry();

    ~LayerRegistry();

    AddResultEnum add(const ImageLayerDesc& desc, LayerRegistryEntry::OriginEnum origin, std::string* error);

    bool remove(const std::string& id, std::string* error);

    bool contains(const std::string& id) const;

    bool find(const std::string& id, ImageLayerDesc* out) const;

    std::shared_ptr<const std::vector<LayerRegistryEntry>> snapshot() const;

    static bool validate(const ImageLayerDesc& desc, bool fromFile, std::string* error);

    static const ImageLayerDesc* reservedAlias(const std::string& id);

    static void groupChannelNames(const std::vector<std::string>& flat, std::vector<ImageLayerDesc>* layers);

private:
    mutable QMutex _mutex;
    std::shared_ptr<const std::vector<LayerRegistryEntry>> _entries;
};

NATRON_NAMESPACE_EXIT

#endif // LAYERREGISTRY_H
