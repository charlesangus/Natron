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

#ifndef Engine_Nodes_OCIO_ModelColourspace_h
#define Engine_Nodes_OCIO_ModelColourspace_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

#include <OpenColorIO/OpenColorIO.h>

#include "Engine/Ai/ModelPackage.h"
#include "Engine/EngineFwd.h"
#include "Engine/Nodes/OCIO/OcioNodeBase.h"

#define kModelColourspaceParam "modelColourspace"

NATRON_NAMESPACE_ENTER

/**
 * @brief The "Model Colourspace" knob of an AI image node: an OCIO colourspace of the project's
 * config, listed as the OCIO nodes list theirs. The node's pack converts the working space into
 * it before building the tensor and its unpack converts the model's output back. Nothing else is
 * done to the pixels: no clamp, exposure or range handling.
 *
 * Until the user picks a space, the knob follows the space the selected model was trained in
 * (sRGB Encoded Rec.709 for an sRGB model, Linear Rec.2020 for a linear Rec.2020 one), resolved
 * through the config's names and aliases.
 *
 * The node calls watchConfig() from initializeKnobs(), refreshMenus() from onKnobsLoaded(),
 * modelChanged() from onEffectCreated() and whenever its model changes, knobChanged() from
 * knobChanged(), and unwatchConfig() from its destructor.
 **/
class ModelColourspaceKnobs {
public:
    // The space the selected model was trained in, or nothing when no model is known.
    typedef std::function<std::optional<AiColourSpace>()> ModelSpace;

    static constexpr const char* kDefaultSpace = "sRGB Encoded Rec.709 (sRGB)";

    // The two processors of one evaluation. A null cpu is an identity.
    struct Conversion {
        OcioProcessor toModel;
        OcioProcessor fromModel;
        // Why either cannot be built; empty when both can.
        std::string error;
    };

    ModelColourspaceKnobs() = default;
    ModelColourspaceKnobs(const ModelColourspaceKnobs&) = delete;
    ModelColourspaceKnobs& operator=(const ModelColourspaceKnobs&) = delete;

    void create(KnobHolder* holder,
                const KnobPagePtr& page,
                const ModelSpace& modelSpace);

    void watchConfig();
    void unwatchConfig();
    void refreshMenus();
    void modelChanged();
    bool knobChanged(KnobI* k,
                     ValueChangedReasonEnum reason);

    // The chosen space as the knob holds it.
    std::string space() const;

    Conversion conversion(double time) const;

    /**
     * @brief The name in config of the space a model trained in space expects, or empty when the
     * config has none of the names it is known by, and for camera RGB.
     **/
    static std::string defaultSpace(const OCIO_NAMESPACE::ConstConfigRcPtr& config,
                                    AiColourSpace space);

    /**
     * @brief Applies processor to count packed RGB pixels in place. Leaves them for an identity.
     * Throws when OpenColorIO fails.
     **/
    static void apply(const OcioProcessor& processor,
                      float* rgb,
                      std::size_t count);

    const OcioNodeKnobs& ocio() const
    {
        return _ocio;
    }

private:
    std::optional<std::string> automaticSpace() const;

    OcioNodeKnobs _ocio;
    ModelSpace _modelSpace;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_OCIO_ModelColourspace_h
