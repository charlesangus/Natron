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

#include "RenderDelegates.h"

#include <mutex>

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/errorMark.h>
#include <pxr/imaging/hd/rendererCreateArgsSchema.h>
#include <pxr/imaging/hd/rendererPlugin.h>
#include <pxr/imaging/hd/rendererPluginHandle.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hf/pluginDesc.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
std::mutex gListMutex;
bool gListed = false;
std::vector<RenderDelegates::Delegate> gDelegates;

// The commentary of the errors posted since mark, one per line.
std::string
errorsSince(const TfErrorMark& mark)
{
    std::string text;

    for (TfErrorMark::Iterator it = mark.GetBegin(); it != mark.GetEnd(); ++it) {
        if (!text.empty()) {
            text += "\n";
        }
        text += it->GetCommentary();
    }

    return text;
}

// Loads the delegate's plug-in to learn whether it can render here; the errors the loader posts
// become the delegate's, not the console's.
RenderDelegates::Delegate
probe(const HfPluginDesc& desc)
{
    RenderDelegates::Delegate delegate;
    HdRendererPluginRegistry& registry = HdRendererPluginRegistry::GetInstance();

    delegate.id = desc.id.GetString();
    delegate.displayName = UsdImagingGLEngine::GetRendererDisplayName(desc.id);
    if (delegate.displayName.empty()) {
        delegate.displayName = delegate.id;
    }
    TfErrorMark mark;
    {
        const HdRendererPluginHandle plugin = registry.GetOrCreateRendererPlugin(desc.id);
        if (!plugin) {
            delegate.error = errorsSince(mark);
            if (delegate.error.empty()) {
                delegate.error = "its plug-in could not be loaded";
            }
        } else if (RenderDelegates::isStorm(delegate.id)) {
            // Storm's support depends on the GL context it is given, which only rendering makes.
            delegate.available = true;
        } else {
            // As the engines that render it are made: no GPU, no Hgi.
            const HdRendererCreateArgsSchema args(HdRendererCreateArgsSchema::Builder()
                                                      .SetGpuEnabled(HdRetainedTypedSampledDataSource<bool>::New(false))
                                                      .Build());
            std::string reason;
            delegate.available = plugin->IsSupported(args, &reason);
            if (!delegate.available) {
                delegate.error = reason.empty() ? std::string("it cannot render without a GPU") : reason;
            }
        }
    }
    mark.Clear();

    return delegate;
}
} // anon namespace

namespace RenderDelegates {
bool
isStorm(const std::string& id)
{
    return id == kRenderDelegateStormId;
}

std::vector<Delegate>
list()
{
    std::lock_guard<std::mutex> l(gListMutex);

    if (!gListed) {
        gListed = true;
        HfPluginDescVector descs;
        HdRendererPluginRegistry::GetInstance().GetPluginDescs(&descs);
        bool haveStorm = false;
        for (const HfPluginDesc& desc : descs) {
            Delegate delegate = probe(desc);
            if (isStorm(delegate.id)) {
                haveStorm = true;
                gDelegates.insert(gDelegates.begin(), delegate);
            } else {
                gDelegates.push_back(delegate);
            }
        }
        if (!haveStorm) {
            Delegate storm;
            storm.id = kRenderDelegateStormId;
            storm.displayName = "Storm";
            storm.error = "USD's hdStorm plug-in was not found on the plug-in path";
            gDelegates.insert(gDelegates.begin(), storm);
        }
    }

    return gDelegates;
}

bool
find(const std::string& id,
     Delegate* delegate)
{
    for (const Delegate& d : list()) {
        if (d.id == id) {
            *delegate = d;

            return true;
        }
    }

    return false;
}

std::string
unavailableMessage(const std::string& id)
{
    Delegate delegate;

    if (!find(id, &delegate)) {
        return "The renderer " + id + " is not installed: no Hydra render delegate of that name is on the plug-in path.";
    }
    if (delegate.available) {
        return std::string();
    }

    return "The renderer " + delegate.displayName + " (" + delegate.id + ") is not available: " + delegate.error;
}

void
registerPluginDirectory(const std::string& path)
{
    (void)PlugRegistry::GetInstance().RegisterPlugins(path);
}
} // namespace RenderDelegates

NATRON_NAMESPACE_EXIT
