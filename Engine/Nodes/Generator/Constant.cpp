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

#include "Constant.h"

#include <cfloat>
#include <memory>
#include <string>

#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

namespace {
class ConstantKernel
    : public PixelKernel {
public:
    explicit ConstantKernel(const float rgba[4])
    {
        for (int i = 0; i < 4; ++i) {
            _rgba[i] = rgba[i];
        }
    }

    virtual void processRow(const RowIO& io) const OVERRIDE FINAL
    {
        const GeneratorRowChannels channels(io);
        float* dst = io.dst;

        for (int x = 0; x < io.width; ++x, dst += io.nComps) {
            channels.write(_rgba, dst);
        }
    }

private:
    float _rgba[4];
};
} // anonymous namespace

Constant::Constant(NodePtr node,
                   bool solid)
    : NativeGenerator(node)
    , _solid(solid)
    , _color()
{
}

Constant::~Constant()
{
}

void
Constant::addAcceptedComponents(int inputNb,
                                std::list<ImageLayerDesc>* comps)
{
    NativeGenerator::addAcceptedComponents(inputNb, comps);
    comps->push_back(ImageLayerDesc::getXYComponents());
}

NativePluginDescription
Constant::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_CONSTANT;
    desc.label = "Constant";
    desc.description = tr("Generate an image with a constant color.\n"
                          "See also: https://web.archive.org/web/20220807183203/http://www.opticalenquiry.com/nuke/index.php?title=Constant,_CheckerBoard,_ColorBars,_ColorWheel")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_IMAGE;
    desc.majorVersion = PLUGIN_MAJOR_NATRON_CONSTANT;
    desc.minorVersion = 0;
    describeSourceInput(&desc);
    desc.outputKind = eDataKindImage;

    return desc;
}

void
Constant::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    initializeGeneratorKnobs(page);

    const int dims = _solid ? 3 : 4;
    KnobColorPtr color = createKnob<KnobColor>(tr("Color"), dims);
    color->setName(kConstantParamColor);
    color->setHintToolTip(tr("Color to fill the image with."));
    for (int i = 0; i < dims; ++i) {
        color->setDefaultValue(0., i);
        color->setMinimum(-DBL_MAX, i);
        color->setMaximum(DBL_MAX, i);
        color->setDisplayMinimum(0., i);
        color->setDisplayMaximum(1., i);
    }
    page->addKnob(color);
    _color = color;
}

PixelKernelPtr
Constant::makeKernel(const KernelContext& context)
{
    KnobColorPtr color = _color.lock();
    float rgba[4] = { 0.f, 0.f, 0.f, _solid ? 1.f : 0.f };

    if (color) {
        const int dims = _solid ? 3 : 4;
        for (int i = 0; i < dims; ++i) {
            rgba[i] = (float)color->getValueAtTime(context.time, i, context.view);
        }
    }

    return std::make_shared<ConstantKernel>(rgba);
}

Solid::Solid(NodePtr node)
    : Constant(node, true)
{
}

Solid::~Solid()
{
}

NativePluginDescription
Solid::getNativePluginDescription() const
{
    NativePluginDescription desc = Constant::getNativePluginDescription();

    desc.id = PLUGINID_NATRON_SOLID;
    desc.label = "Solid";
    desc.description = tr("Generate an image with a constant opaque color.").toStdString();

    return desc;
}

NATRON_NAMESPACE_EXIT
