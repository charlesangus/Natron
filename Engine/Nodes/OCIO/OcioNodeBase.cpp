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

#include "OcioNodeBase.h"

#include <cstring>

#include "Engine/AppInstance.h"
#include "Engine/Knob.h"
#include "Engine/Nodes/IO/IoColorSpace.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"

NATRON_NAMESPACE_ENTER

OcioNodeKnobs::OcioNodeKnobs()
    : _holder(nullptr)
    , _spaces()
    , _watched()
    , _watchId(-1)
{
}

OcioNodeKnobs::~OcioNodeKnobs()
{
    unwatchConfig();
}

void
OcioNodeKnobs::addSpace(KnobHolder* holder,
                        const KnobPagePtr& page,
                        const OcioSpaceKnobs::Description& description,
                        const OcioSpaceKnobs::AutomaticSpace& automaticSpace,
                        const OcioSpaceKnobs::ChoiceMade& choiceMade)
{
    _holder = holder;
    std::unique_ptr<OcioSpaceKnobs> knobs(new OcioSpaceKnobs());
    if (_spaces.empty()) {
        knobs->create(holder, page, description, automaticSpace, choiceMade);
    } else {
        knobs->createSharing(*_spaces.front(), holder, page, description, automaticSpace, choiceMade);
    }
    _spaces.push_back(std::move(knobs));
}

std::size_t
OcioNodeKnobs::spaceCount() const
{
    return _spaces.size();
}

std::string
OcioNodeKnobs::space(std::size_t index) const
{
    return index < _spaces.size() ? _spaces[index]->space() : std::string();
}

std::string
OcioNodeKnobs::workingSpace() const
{
    if (!_spaces.empty()) {
        return _spaces.front()->workingSpace();
    }
    ProjectPtr p = project();

    return p ? p->getWorkingColorSpace() : std::string();
}

std::vector<std::pair<std::string, std::string>>
OcioNodeKnobs::contextVariables(double time) const
{
    return _spaces.empty() ? std::vector<std::pair<std::string, std::string>>() : _spaces.front()->contextVariables(time);
}

bool
OcioNodeKnobs::knobChanged(KnobI* k,
                           ValueChangedReasonEnum reason)
{
    bool handled = false;

    // Every space refreshes on the shared config knobs, so none may stop the others.
    for (std::size_t i = 0; i < _spaces.size(); ++i) {
        if (_spaces[i]->knobChanged(k, reason)) {
            handled = true;
        }
    }

    return handled;
}

void
OcioNodeKnobs::refreshMenus()
{
    for (std::size_t i = 0; i < _spaces.size(); ++i) {
        _spaces[i]->refreshMenu();
    }
}

ProjectPtr
OcioNodeKnobs::project() const
{
    AppInstancePtr app = _holder ? _holder->getApp() : AppInstancePtr();

    return app ? app->getProject() : ProjectPtr();
}

OCIO_NAMESPACE::ConstTransformRcPtr
OcioNodeKnobs::spaceTransform(const std::string& from,
                              const std::string& to) const
{
    if (from.empty() || to.empty() || (from == to)) {
        return OCIO_NAMESPACE::ConstTransformRcPtr();
    }
    ProjectPtr p = project();
    ProjectColorManagementPtr colorManagement = p ? p->getColorManagement() : ProjectColorManagementPtr();
    const OCIO_NAMESPACE::ConstConfigRcPtr config = colorManagement ? colorManagement->getConfig() : OCIO_NAMESPACE::ConstConfigRcPtr();
    if (config) {
        // A role or alias names the same space as the space itself.
        const std::string resolvedFrom = IoColorSpace::nameInConfig(config, from);
        if (!resolvedFrom.empty() && (resolvedFrom == IoColorSpace::nameInConfig(config, to))) {
            return OCIO_NAMESPACE::ConstTransformRcPtr();
        }
    }
    OCIO_NAMESPACE::ColorSpaceTransformRcPtr transform = OCIO_NAMESPACE::ColorSpaceTransform::Create();
    transform->setSrc(from.c_str());
    transform->setDst(to.c_str());

    return transform;
}

OcioProcessor
OcioNodeKnobs::processor(const OCIO_NAMESPACE::ConstTransformRcPtr& transform,
                         double time) const
{
    OcioProcessor result;

    if (!transform) {
        return result;
    }
    ProjectPtr p = project();
    ProjectColorManagementPtr colorManagement = p ? p->getColorManagement() : ProjectColorManagementPtr();
    if (!colorManagement) {
        result.error = tr("No project to take the OpenColorIO config from.").toStdString();

        return result;
    }
    std::string error;
    if (p->getOCIOConfigError(&error)) {
        result.error = error;

        return result;
    }
    const ProjectColorManagement::TransformProcessorPtr built = colorManagement->getTransformProcessor(transform, contextVariables(time), &error);
    if (!built || !built->cpu) {
        result.error = tr("Cannot build the OpenColorIO transform: %1").arg(QString::fromStdString(error)).toStdString();

        return result;
    }
    if (!built->cpu->isNoOp()) {
        result.processor = built->processor;
        result.cpu = built->cpu;
    }

    return result;
}

void
OcioNodeKnobs::watchConfig(const std::function<void()>& callback)
{
    unwatchConfig();
    ProjectPtr p = project();
    ProjectColorManagementPtr colorManagement = p ? p->getColorManagement() : ProjectColorManagementPtr();
    if (!colorManagement) {
        return;
    }
    _watchId = colorManagement->addConfigChangedCallback(callback);
    _watched = colorManagement;
}

void
OcioNodeKnobs::unwatchConfig()
{
    ProjectColorManagementPtr colorManagement = _watched.lock();

    if (colorManagement && (_watchId >= 0)) {
        colorManagement->removeConfigChangedCallback(_watchId);
    }
    _watched.reset();
    _watchId = -1;
}

void
OcioNodeKnobs::applyRow(const OcioProcessor& processor,
                        const RowIO& io)
{
    if (io.dst != io.src[0]) {
        std::memcpy(io.dst, io.src[0], sizeof(float) * io.width * io.nComps);
    }
    if (processor.cpu) {
        // A row that fails keeps the source, as the node does when no processor can be built.
        std::string error;
        if (!IoColorSpace::convertRow(*processor.cpu, io.dst, io.width, io.nComps, &error) && (io.dst != io.src[0])) {
            std::memcpy(io.dst, io.src[0], sizeof(float) * io.width * io.nComps);
        }
    }
}

NATRON_NAMESPACE_EXIT
