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

#include "LayeredImageInput.h"

#include <algorithm>
#include <cstddef>
#include <span>

#include <QByteArray>
#include <QString>
#include <QXmlStreamReader>

#include "Engine/Nodes/IO/LayeredDocs/ZipArchive.h"

NATRON_NAMESPACE_ENTER

namespace LayeredDocs {
namespace {
    const char* const kMimeType = "image/openraster";

    std::shared_ptr<LayeredDocument>
    failed(std::string* error,
           const std::string& message)
    {
        if (error) {
            *error = message;
        }

        return std::shared_ptr<LayeredDocument>();
    }

    std::string
    attributeOf(const QXmlStreamReader& xml,
                const char* name)
    {
        return xml.attributes().value(QString::fromUtf8(name)).toString().toStdString();
    }

    int
    intAttributeOf(const QXmlStreamReader& xml,
                   const char* name)
    {
        bool ok = false;
        const double value = xml.attributes().value(QString::fromUtf8(name)).toString().toDouble(&ok);

        return ok ? (int)std::clamp(value, -1e9, 1e9) : 0;
    }

    double
    opacityOf(const QXmlStreamReader& xml)
    {
        bool ok = false;
        const double value = xml.attributes().value(QString::fromUtf8("opacity")).toString().toDouble(&ok);

        return ok ? std::clamp(value, 0., 1.) : 1.;
    }

    bool
    visibleOf(const QXmlStreamReader& xml)
    {
        return attributeOf(xml, "visibility") != "hidden";
    }

    struct Stack {
        std::string name;
        bool root = false;
        int x = 0;
        int y = 0;
        double opacity = 1.;
        bool visible = true;
    };
} // anonymous namespace

std::shared_ptr<LayeredDocument>
readOra(const std::string& path,
        std::string* error)
{
    const std::shared_ptr<Archive> archive = std::make_shared<Archive>();

    std::string readError;
    std::string zipError;
    if (!readArchive(path, archive.get(), &readError, &zipError)) {
        return failed(error, readError.empty() ? path + " is not an OpenRaster file: " + zipError : readError);
    }

    std::span<const std::byte> stackXml;
    try {
        if (archive->zip->contains("mimetype")) {
            const std::span<const std::byte> mime = archive->zip->data("mimetype");
            std::string type(reinterpret_cast<const char*>(mime.data()), mime.size());
            type.erase(type.find_last_not_of(" \t\r\n") + 1);
            if (type != kMimeType) {
                return failed(error, path + " is not an OpenRaster file: its mime type is " + type);
            }
        }
        if (!archive->zip->contains("stack.xml")) {
            return failed(error, path + " is not an OpenRaster file: it has no stack.xml");
        }
        stackXml = archive->zip->data("stack.xml");
    } catch (const Serialization::ZipError& e) {
        return failed(error, e.what());
    }

    const std::shared_ptr<LayeredDocument> document = std::make_shared<LayeredDocument>();
    std::vector<Layer> topFirst;
    std::vector<Stack> stacks;
    bool sawImage = false;

    QXmlStreamReader xml(QByteArray::fromRawData(reinterpret_cast<const char*>(stackXml.data()), (qsizetype)stackXml.size()));
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isEndElement() && xml.name() == QLatin1String("stack") && !stacks.empty()) {
            stacks.pop_back();
            continue;
        }
        if (!xml.isStartElement()) {
            continue;
        }

        std::vector<std::string> groups;
        int offsetX = 0;
        int offsetY = 0;
        double groupOpacity = 1.;
        bool groupVisible = true;
        for (const Stack& stack : stacks) {
            if (!stack.root) {
                groups.push_back(stack.name);
            }
            offsetX += stack.x;
            offsetY += stack.y;
            groupOpacity *= stack.opacity;
            groupVisible = groupVisible && stack.visible;
        }

        const QStringView element = xml.name();
        if (element == QLatin1String("image")) {
            sawImage = true;
            document->width = intAttributeOf(xml, "w");
            document->height = intAttributeOf(xml, "h");
        } else if (element == QLatin1String("stack")) {
            Stack stack;
            stack.root = stacks.empty();
            stack.name = attributeOf(xml, "name");
            stack.x = intAttributeOf(xml, "x");
            stack.y = intAttributeOf(xml, "y");
            stack.opacity = opacityOf(xml);
            stack.visible = visibleOf(xml);
            stacks.push_back(stack);
        } else if (element == QLatin1String("layer") && !stacks.empty()) {
            Layer layer;
            layer.name = attributeOf(xml, "name");
            layer.groups = groups;
            layer.blendMode = attributeOf(xml, "composite-op");
            if (layer.blendMode.empty()) {
                layer.blendMode = "svg:src-over";
            }
            layer.opacity = opacityOf(xml);
            layer.visible = visibleOf(xml);
            layer.groupOpacity = groupOpacity;
            layer.groupVisible = groupVisible;

            const std::string src = attributeOf(xml, "src");
            if (!src.empty() && archive->zip->contains(src)) {
                PngInfo info;
                if (!pngInfoOf(*archive, src, &info, error)) {
                    return std::shared_ptr<LayeredDocument>();
                }
                layer.plane.x = offsetX + intAttributeOf(xml, "x");
                layer.plane.y = offsetY + intAttributeOf(xml, "y");
                layer.plane.width = info.width;
                layer.plane.height = info.height;
                layer.plane.channels = { "R", "G", "B", "A" };
                layer.plane.type = info.type;
                layer.plane.load = [archive, src, info](std::vector<unsigned char>* pixels, std::string* loadError) {
                    return decodePng(*archive, src, info, true, pixels, loadError);
                };
            }
            topFirst.push_back(std::move(layer));
            xml.skipCurrentElement();
        } else if (!stacks.empty()) {
            std::vector<std::string> path = groups;
            path.push_back(attributeOf(xml, "name"));
            std::string joinedPath;
            for (std::size_t i = 0; i < path.size(); ++i) {
                joinedPath += (i > 0 ? "/" : "") + path[i];
            }
            document->skipped.push_back(joinedPath + " (" + element.toString().toStdString() + " element)");
            xml.skipCurrentElement();
        }
    }
    if (xml.hasError()) {
        return failed(error, path + ": stack.xml is malformed: " + xml.errorString().toStdString());
    }
    if (!sawImage) {
        return failed(error, path + ": stack.xml has no image element");
    }

    document->layers.assign(std::make_move_iterator(topFirst.rbegin()), std::make_move_iterator(topFirst.rend()));

    if (archive->zip->contains("mergedimage.png")) {
        PngInfo info;
        if (!pngInfoOf(*archive, "mergedimage.png", &info, error)) {
            return std::shared_ptr<LayeredDocument>();
        }
        document->composite.width = info.width;
        document->composite.height = info.height;
        document->composite.channels = info.channels;
        document->composite.type = info.type;
        document->composite.load = [archive, info](std::vector<unsigned char>* pixels, std::string* loadError) {
            return decodePng(*archive, "mergedimage.png", info, false, pixels, loadError);
        };
    }

    std::string finaliseError;
    if (!document->finalise(&finaliseError)) {
        return failed(error, path + ": " + finaliseError);
    }

    return document;
}
} // namespace LayeredDocs

NATRON_NAMESPACE_EXIT
