/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

#ifndef Engine_Optics_LensKnobs_h
#define Engine_Optics_LensKnobs_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Optics/Prescription.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

namespace Optics {

// lensModel entry ids LensKnobs always adds, after the caller's own.
#define kLensModelDatabase "database"
#define kLensModelFromFile "fromFile"

struct LensModelOption {
    std::string id;
    std::string label;
    std::string hint;
};

struct LensKnobOptions {
    // lensModel entries before Database and From File, such as a procedural lens.
    std::vector<LensModelOption> leadingModels;
    // The id of the default lensModel entry.
    std::string defaultModel = kLensModelDatabase;
    // A LensDatabase id.
    std::string defaultLens = "Classic Designs/dgauss50";
};

/**
 * @brief The lens selection knobs nodes share: lensModel, lens with its lensBrowse button, lensFile,
 * glassVendor, abbeOffset and useAspheric, added to the page in that order. lens and lensBrowse show
 * for the Database model, lensFile for From File.
 *
 * The holder forwards its knobChanged() to knobChanged() here, which keeps that visibility and opens
 * the lens browser for lensBrowse through the handler the GUI installs with setBrowseHandler(), so the
 * engine has no GUI dependency.
 **/
class LensKnobs {
public:
    // Called on the main thread with the node's lens knob; writes the chosen id into it.
    using BrowseHandler = std::function<void(const KnobStringPtr& lens)>;

    static LensKnobs create(KnobHolder* holder,
                            const KnobPagePtr& page,
                            const LensKnobOptions& options = LensKnobOptions());

    static void setBrowseHandler(const BrowseHandler& handler);
    static bool hasBrowseHandler();

    // The id of the active lensModel entry.
    std::string model(double time,
                      ViewIdx view = ViewIdx(0)) const WARN_UNUSED_RETURN;

    /**
     * @brief The Database or From File prescription. False with *error set for another model, a lens
     * file that does not parse (the parser's line and message), or a lens id the database lacks (the
     * error names the id; the knob keeps its value).
     **/
    bool resolve(double time,
                 Prescription* out,
                 std::string* error,
                 ViewIdx view = ViewIdx(0)) const WARN_UNUSED_RETURN;

    // A GlassCatalogue vendor name.
    std::string glassVendor(double time,
                            ViewIdx view = ViewIdx(0)) const WARN_UNUSED_RETURN;
    double abbeOffset(double time,
                      ViewIdx view = ViewIdx(0)) const WARN_UNUSED_RETURN;
    bool useAspheric(double time,
                     ViewIdx view = ViewIdx(0)) const WARN_UNUSED_RETURN;

    // True when k is one of these knobs.
    bool knobChanged(KnobI* k) const;
    // Shows lens, lensBrowse and lensFile for the active model; knobChanged() calls it.
    void refreshVisibility() const;

    KnobChoicePtr lensModelKnob() const { return _model.lock(); }
    KnobStringPtr lensKnob() const { return _lens.lock(); }
    KnobButtonPtr lensBrowseKnob() const { return _browse.lock(); }
    KnobFilePtr lensFileKnob() const { return _file.lock(); }

private:
    KnobChoiceWPtr _model;
    KnobStringWPtr _lens;
    KnobButtonWPtr _browse;
    KnobFileWPtr _file;
    KnobChoiceWPtr _glassVendor;
    KnobDoubleWPtr _abbeOffset;
    KnobBoolWPtr _useAspheric;
};

} // namespace Optics

NATRON_NAMESPACE_EXIT

#endif // Engine_Optics_LensKnobs_h
