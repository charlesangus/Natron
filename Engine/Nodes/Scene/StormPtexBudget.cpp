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

#include "StormPtexBudget.h"

#include <cstddef>
#include <memory>

#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/driver.h>
#include <pxr/imaging/hdSt/enums.h>
#include <pxr/imaging/hdSt/renderDelegate.h>
#include <pxr/imaging/hdSt/resourceRegistry.h>
#include <pxr/imaging/hgi/tokens.h>

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
// HdStPtexMipmapTextureLoader::generateBuffers (v26.08 hdSt/ptexMipmapTextureLoader.cpp) computes
// the size of its page buffer in an int, which overflows at 2 GiB. The limit counts the texels of
// the blocks, not of the pages they land on, and the packing can put a single block on a page of
// nearly four times its area: one face sets the page size and every other face is one level
// smaller. 512 MiB is the largest power of two whose worst case stays under the overflow; 1 GiB
// still overflows on a plain texture of 512 x 512 faces with one larger face.
const std::size_t kPtexMemoryRequest = 512 * 1024 * 1024;
} // anonymous namespace

void
applyStormPtexBudget(Hgi* hgi)
{
    if (!hgi) {
        return;
    }

    // The engine keeps its delegate private, but Storm holds one registry per Hgi (v26.08
    // hdSt/renderDelegate.cpp, _HgiToResourceRegistryMap), so a delegate given the engine's Hgi
    // hands back the engine's registry. No Hydra prim is ever made with this delegate.
    HdDriver driver { HgiTokens->renderDriver, VtValue(hgi) };
    HdStRenderDelegate delegate;
    delegate.SetDrivers({ &driver });

    const HdStResourceRegistrySharedPtr registry = std::dynamic_pointer_cast<HdStResourceRegistry>(delegate.GetResourceRegistry());
    if (registry) {
        registry->SetMemoryRequestForTextureType(HdStTextureType::Ptex, kPtexMemoryRequest);
    }
}

NATRON_NAMESPACE_EXIT
