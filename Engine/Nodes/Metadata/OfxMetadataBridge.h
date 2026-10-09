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

#ifndef Engine_Nodes_Metadata_OfxMetadataBridge_h
#define Engine_Nodes_Metadata_OfxMetadataBridge_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"

namespace OFX {
namespace Host {
    namespace Property {
        class Set;
    }
    namespace ImageEffect {
        class ClipInstance;
    }
}
}

NATRON_NAMESPACE_ENTER

namespace OfxMetadataBridge {
/**
 * @brief Converts an OpenFX metadata property set to ImageMetadata.
 *
 * An int or double property of dimension 1 becomes a scalar; of any other dimension,
 * including 0, a vector of that type. A string property of dimension 1 becomes a string.
 * Skipped, because ImageMetadata has no type for them: string properties of any other
 * dimension (ofx/viewnames, for one), pointer properties, and properties of no known type.
 * A chained set is not followed.
 **/
ImageMetadata fromOfxPropertySet(const OFX::Host::Property::Set& properties) WARN_UNUSED_RETURN;

/**
 * @brief Writes every entry of metadata into properties, replacing a property of the same
 * name whatever its type. Scalars become properties of dimension 1, vectors properties whose
 * dimension is the vector's size. Every ImageMetadata type has an OpenFX counterpart, so
 * nothing is skipped; but a one-element vector reads back through fromOfxPropertySet() as a
 * scalar, since dimension is all OpenFX records.
 **/
void toOfxPropertySet(const ImageMetadata& metadata, OFX::Host::Property::Set* properties);

/**
 * @brief The OpenFX output clip whose metadata is what effect puts out, or nullptr if effect
 * is not backed by an OpenFX plug-in. A Read or Write container resolves to the clip of the
 * decoder or encoder embedded in it.
 **/
OFX::Host::ImageEffect::ClipInstance* getOfxOutputClip(const EffectInstancePtr& effect) WARN_UNUSED_RETURN;

/**
 * @brief The metadata effect's OpenFX output clip carries at time, read through the clip's own
 * cache exactly as a downstream OpenFX input clip reads it, or empty if effect has no such clip.
 * Takes the clip-cache and instance metadata locks of effect and of the OpenFX effects upstream
 * of it, so the caller must not hold a lock that an invalidation of those effects could wait on.
 **/
ImageMetadata getOfxEffectOutputMetadata(const EffectInstancePtr& effect, double time) WARN_UNUSED_RETURN;
} // namespace OfxMetadataBridge

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Metadata_OfxMetadataBridge_h
