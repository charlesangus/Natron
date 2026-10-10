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

#include "OfxMetadataBridge.h"

#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <ofxImageEffect.h>
// ofxhClip.h is not self-contained: ofxhImageEffect.h has to pull in the property headers first.
// clang-format off
#include <ofxhImageEffect.h>
#include <ofxhClip.h>
#include <ofxhPropertySuite.h>
// clang-format on

#include "Engine/EffectInstance.h"
#include "Engine/NoOpBase.h"
#include "Engine/Node.h"
#include "Engine/OfxEffectInstance.h"
#include "Engine/OfxImageEffectInstance.h"
#include "Engine/ReadNode.h"
#include "Engine/WriteNode.h"

NATRON_NAMESPACE_ENTER

namespace OfxMetadataBridge {
namespace {
    template <class PropertyT, class ValueT>
    std::vector<ValueT>
    readValues(PropertyT& property)
    {
        const int dimension = property.getDimension();
        std::vector<ValueT> values;

        values.reserve(dimension);
        for (int i = 0; i < dimension; ++i) {
            values.push_back(property.getValue(i));
        }

        return values;
    }

    template <class PropertyT, class ValueT>
    void
    writeValues(const std::string& key,
                const std::vector<ValueT>& values,
                OFX::Host::Property::Set* properties)
    {
        std::unique_ptr<PropertyT> property(new PropertyT(key, (int)values.size(), false, ValueT()));

        for (std::size_t i = 0; i < values.size(); ++i) {
            property->setValue(values[i], (int)i);
        }
        properties->addProperty(property.release());
    }

#ifdef OFX_SUPPORTS_METADATA
    struct ReleaseMetadataSet {
        void operator()(OFX::Host::ImageEffect::MetadataSet* set) const
        {
            set->releaseReference();
        }
    };
#endif
} // anonymous namespace

ImageMetadata
fromOfxPropertySet(const OFX::Host::Property::Set& properties)
{
    ImageMetadata metadata;
    const OFX::Host::Property::PropertyMap& props = properties.getProperties();

    for (OFX::Host::Property::PropertyMap::const_iterator it = props.begin(); it != props.end(); ++it) {
        OFX::Host::Property::Property* property = it->second;

        if (!property) {
            continue;
        }

        switch (property->getType()) {
        case OFX::Host::Property::eInt: {
            OFX::Host::Property::Int* typed = dynamic_cast<OFX::Host::Property::Int*>(property);
            if (!typed) {
                break;
            }
            std::vector<int> values = readValues<OFX::Host::Property::Int, int>(*typed);
            if (values.size() == 1) {
                metadata.setInt(it->first, values[0]);
            } else {
                metadata.setIntVector(it->first, values);
            }
            break;
        }
        case OFX::Host::Property::eDouble: {
            OFX::Host::Property::Double* typed = dynamic_cast<OFX::Host::Property::Double*>(property);
            if (!typed) {
                break;
            }
            std::vector<double> values = readValues<OFX::Host::Property::Double, double>(*typed);
            if (values.size() == 1) {
                metadata.setDouble(it->first, values[0]);
            } else {
                metadata.setDoubleVector(it->first, values);
            }
            break;
        }
        case OFX::Host::Property::eString: {
            OFX::Host::Property::String* typed = dynamic_cast<OFX::Host::Property::String*>(property);
            if (typed && (typed->getDimension() == 1)) {
                metadata.setString(it->first, typed->getValue(0));
            }
            break;
        }
        case OFX::Host::Property::ePointer:
        case OFX::Host::Property::eNone:
        default:
            break;
        }
    }

    return metadata;
} // fromOfxPropertySet

void
toOfxPropertySet(const ImageMetadata& metadata,
                 OFX::Host::Property::Set* properties)
{
    if (!properties) {
        return;
    }

    for (ImageMetadata::const_iterator it = metadata.begin(); it != metadata.end(); ++it) {
        const std::string& key = it->first;
        const ImageMetadata::Value& value = it->second;

        if (const int* i = std::get_if<int>(&value)) {
            writeValues<OFX::Host::Property::Int, int>(key, std::vector<int>(1, *i), properties);
        } else if (const double* d = std::get_if<double>(&value)) {
            writeValues<OFX::Host::Property::Double, double>(key, std::vector<double>(1, *d), properties);
        } else if (const std::string* s = std::get_if<std::string>(&value)) {
            std::unique_ptr<OFX::Host::Property::String> property(new OFX::Host::Property::String(key, 1, false, ""));
            property->setValue(*s, 0);
            properties->addProperty(property.release());
        } else if (const std::vector<int>* iv = std::get_if<std::vector<int>>(&value)) {
            if (!iv->empty()) {
                writeValues<OFX::Host::Property::Int, int>(key, *iv, properties);
            }
        } else if (const std::vector<double>* dv = std::get_if<std::vector<double>>(&value)) {
            if (!dv->empty()) {
                writeValues<OFX::Host::Property::Double, double>(key, *dv, properties);
            }
        }
    }
}

OFX::Host::ImageEffect::ClipInstance*
getOfxOutputClip(const EffectInstancePtr& effect)
{
    OfxEffectInstance* ofxEffect = dynamic_cast<OfxEffectInstance*>(effect.get());

    if (!ofxEffect) {
        // A bundled reader or writer stands in the graph as a Read/Write container, which
        // is not itself an OFX effect: the clips are the decoder's or encoder's, and so is
        // the metadata that has to reach whatever is connected downstream of the container.
        NodePtr embedded;
        ReadNode* isReadNode = dynamic_cast<ReadNode*>(effect.get());
        WriteNode* isWriteNode = dynamic_cast<WriteNode*>(effect.get());
        if (isReadNode) {
            embedded = isReadNode->getEmbeddedReader();
        } else if (isWriteNode) {
            embedded = isWriteNode->getEmbeddedWriter();
        }
        if (embedded) {
            ofxEffect = dynamic_cast<OfxEffectInstance*>(embedded->getEffectInstance().get());
        }
    }
    if (!ofxEffect) {
        return NULL;
    }

    OfxImageEffectInstance* instance = ofxEffect->effectInstance();

    return instance ? instance->getClip(kOfxImageEffectOutputClipName) : NULL;
}

EffectInstancePtr
skipPassThroughNodes(const EffectInstancePtr& effect)
{
    EffectInstancePtr current = effect;
    std::set<const EffectInstance*> visited;

    while (current && dynamic_cast<NoOpBase*>(current.get()) && current->getNInputs() > 0) {
        if (!visited.insert(current.get()).second) {
            break;
        }
        EffectInstancePtr next = current->getInput(0);
        if (!next) {
            break;
        }
        current = next;
    }

    return current;
}

ImageMetadata
getOfxEffectOutputMetadata(const EffectInstancePtr& effect,
                           double time)
{
#ifdef OFX_SUPPORTS_METADATA
    OFX::Host::ImageEffect::ClipInstance* output = getOfxOutputClip(effect);

    if (!output) {
        return ImageMetadata();
    }

    // getMetadata() hands out a reference that pins the clip's cache entry until it is given
    // back, so it is dropped however this scope is left, a throwing conversion included.
    std::unique_ptr<OFX::Host::ImageEffect::MetadataSet, ReleaseMetadataSet> set(output->getMetadata(time));

    return set ? fromOfxPropertySet(*set) : ImageMetadata();
#else
    (void)effect;
    (void)time;

    return ImageMetadata();
#endif
}
} // namespace OfxMetadataBridge

NATRON_NAMESPACE_EXIT
