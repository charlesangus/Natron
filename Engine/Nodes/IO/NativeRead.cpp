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

#include "NativeRead.h"

#include <cstring>
#include <list>
#include <utility>

#include <ofxImageEffect.h>
#include <ofxNatron.h>

#include "Engine/AppInstance.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

NativeRead::NativeRead(NodePtr node)
    : NativeEffectBase(node)
    , _filename()
{
}

NativePluginDescription
NativeRead::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_READ;
    desc.label = "Read";
    desc.description = tr("Read an image or image sequence.").toStdString();
    desc.grouping = PLUGIN_GROUP_IMAGE;
    desc.majorVersion = PLUGIN_MAJOR_NATRON_READ;
    desc.minorVersion = 0;
    desc.outputKind = eDataKindImage;

    return desc;
}

void
NativeRead::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("File"));
    KnobFilePtr filename = createKnob<KnobFile>(tr("File"));

    filename->setName(kOfxImageEffectFileParamName);
    filename->setAsInputImage();
    filename->setHintToolTip(tr("The image or image sequence to read."));
    // NativeEffectBase has no counterpart of an OpenFX clip-preferences slave param, so without
    // this the preferred metadata is only consulted once, at creation, before a file is chosen.
    filename->setIsMetadataSlave(true);
    page->addKnob(filename);
    _filename = filename;
}

StatusEnum
NativeRead::getPreferredMetadata(NodeMetadata& metadata)
{
    metadata.setNComps(-1, 4);
    metadata.setComponentsType(-1, kNatronColorLayerID);

    return eStatusOK;
}

StatusEnum
NativeRead::getRegionOfDefinition(U64 /*hash*/,
                                  double /*time*/,
                                  const RenderScale& /*scale*/,
                                  ViewIdx /*view*/,
                                  RectD* rod)
{
    // The base implementation leaves the RoD null for a node without inputs, and a null RoD
    // makes renderRoI return no planes at all.
    Format format;

    getApp()->getProject()->getProjectDefaultFormat(&format);
    *rod = format.toCanonicalFormat();

    return eStatusOK;
}

StatusEnum
NativeRead::render(const RenderActionArgs& args)
{
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        const ImagePtr& image = it->second;
        if (!image) {
            continue;
        }
        if (image->getBitDepth() != eImageBitDepthFloat) {
            return eStatusFailed;
        }

        const std::size_t rowBytes = (std::size_t)(args.roi.x2 - args.roi.x1) * image->getComponentsCount() * sizeof(float);
        Image::WriteAccess access(image.get());
        for (int y = args.roi.y1; y < args.roi.y2; ++y) {
            unsigned char* row = access.pixelAt(args.roi.x1, y);
            if (row) {
                std::memset(row, 0, rowBytes);
            }
        }
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
