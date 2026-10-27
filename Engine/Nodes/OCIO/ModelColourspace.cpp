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

#include "ModelColourspace.h"

#include <memory>
#include <vector>

#include <QCoreApplication>

#include "Engine/KnobTypes.h"
#include "Engine/Nodes/IO/IoColorSpace.h"
#include "Engine/ProjectColorManagement.h"
#include "Global/FloatingPointExceptions.h"

NATRON_NAMESPACE_ENTER

namespace {
// The names a space goes by in the configs Natron ships and in older ones, preferred first.
std::vector<const char*>
candidateNames(AiColourSpace space)
{
    switch (space) {
    case AiColourSpace::eSrgb:
        return { ModelColourspaceKnobs::kDefaultSpace, "srgb_tx", "sRGB - Texture", "Utility - sRGB - Texture", "sRGB" };
    case AiColourSpace::eLinearRec709:
        return { "Linear Rec.709 (sRGB)", "lin_rec709_srgb", "lin_rec709", "Utility - Linear - sRGB" };
    case AiColourSpace::eLinearRec2020:
        return { "Linear Rec.2020", "lin_rec2020", "Utility - Linear - Rec.2020" };
    case AiColourSpace::eCamera:
        break;
    }

    return {};
}
} // anonymous namespace

void
ModelColourspaceKnobs::create(KnobHolder* holder,
                              const KnobPagePtr& page,
                              const ModelSpace& modelSpace)
{
    _modelSpace = modelSpace;

    OcioSpaceKnobs::Description description;
    description.prefix = kModelColourspaceParam;
    description.label = QCoreApplication::translate("ModelColourspaceKnobs", "Model Colourspace");
    description.spaceHint = QCoreApplication::translate("ModelColourspaceKnobs", "The OpenColorIO colourspace the model is given its input in and returns its output in. "
                                                                                 "Empty is the working space.");
    description.menuHint = QCoreApplication::translate("ModelColourspaceKnobs", "The OpenColorIO colourspace the model runs in: the image is converted from the working "
                                                                                "space into it, the model runs, and its output is converted back. Pick the space the model "
                                                                                "was trained in: sRGB Encoded Rec.709 for the sRGB models, Linear Rec.2020 for the linear "
                                                                                "ones. Until a space is picked, it follows the selected model. Nothing else is done to "
                                                                                "the values, so keeping them in the range the model expects is up to you.");
    description.setLabel = QCoreApplication::translate("ModelColourspaceKnobs", "Model Colourspace Set");
    _ocio.addSpace(holder, page, description, [this]() { return automaticSpace(); });

    KnobStringPtr spaceKnob = std::dynamic_pointer_cast<KnobString>(holder->getKnobByName(kModelColourspaceParam));
    if (spaceKnob) {
        spaceKnob->setDefaultValue(kDefaultSpace);
    }
}

void
ModelColourspaceKnobs::watchConfig()
{
    _ocio.watchConfig([this]() {
        _ocio.refreshMenus();
        _ocio.sourceChanged();
    });
}

void
ModelColourspaceKnobs::unwatchConfig()
{
    _ocio.unwatchConfig();
}

void
ModelColourspaceKnobs::refreshMenus()
{
    _ocio.refreshMenus();
}

void
ModelColourspaceKnobs::modelChanged()
{
    _ocio.sourceChanged();
}

bool
ModelColourspaceKnobs::knobChanged(KnobI* k,
                                   ValueChangedReasonEnum reason)
{
    return _ocio.knobChanged(k, reason);
}

std::string
ModelColourspaceKnobs::space() const
{
    return _ocio.space(0);
}

ModelColourspaceKnobs::Conversion
ModelColourspaceKnobs::conversion(double time) const
{
    Conversion result;
    const std::string working = _ocio.workingSpace();
    const std::string model = _ocio.spaceOrWorking(0);

    result.toModel = _ocio.processor(_ocio.spaceTransform(working, model), time);
    if (result.toModel.error.empty()) {
        result.fromModel = _ocio.processor(_ocio.spaceTransform(model, working), time);
    }
    const std::string& error = result.toModel.error.empty() ? result.fromModel.error : result.toModel.error;
    if (!error.empty()) {
        result.error = QCoreApplication::translate("ModelColourspaceKnobs", "Model Colourspace \"%1\": %2")
                           .arg(QString::fromStdString(model), QString::fromStdString(error))
                           .toStdString();
    }

    return result;
}

std::string
ModelColourspaceKnobs::defaultSpace(const OCIO_NAMESPACE::ConstConfigRcPtr& config,
                                    AiColourSpace space)
{
    for (const char* name : candidateNames(space)) {
        const std::string found = IoColorSpace::nameInConfig(config, name);
        if (!found.empty()) {
            return found;
        }
    }

    return std::string();
}

std::optional<std::string>
ModelColourspaceKnobs::automaticSpace() const
{
    const std::optional<AiColourSpace> trained = _modelSpace ? _modelSpace() : std::optional<AiColourSpace>(AiColourSpace::eSrgb);
    const std::vector<const char*> names = candidateNames(trained.value_or(AiColourSpace::eSrgb));
    if (names.empty()) {
        return std::nullopt;
    }
    ProjectColorManagementPtr colorManagement = _ocio.colorManagement();
    const OCIO_NAMESPACE::ConstConfigRcPtr config = colorManagement ? colorManagement->getConfig() : OCIO_NAMESPACE::ConstConfigRcPtr();
    const std::string found = defaultSpace(config, trained.value_or(AiColourSpace::eSrgb));

    // A config with none of the names keeps the preferred one, which the render then reports.
    return found.empty() ? std::string(names.front()) : found;
}

void
ModelColourspaceKnobs::apply(const OcioProcessor& processor,
                             float* rgb,
                             std::size_t count)
{
    if (!processor.cpu || !rgb || (count == 0)) {
        return;
    }
    const boost_adaptbx::floating_point::FpTrapGuard fpGuard;
    // One row of count pixels: PackedImageDesc takes long dimensions.
    OCIO_NAMESPACE::PackedImageDesc image(rgb, (long)count, 1, 3);
    processor.cpu->apply(image);
}

NATRON_NAMESPACE_EXIT
