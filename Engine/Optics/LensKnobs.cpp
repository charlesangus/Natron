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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "LensKnobs.h"

#include <string>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/FileStampRegistry.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Optics/LensDatabase.h"
#include "Engine/Optics/ZemaxParser.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_ENTER

namespace Optics {

namespace {

    const char* const kGlassVendors[] = { "Schott", "Ohara", "Hoya", "CDGM", "Hikari", "Sumita" };

    LensKnobs::BrowseHandler&
    browseHandler()
    {
        static LensKnobs::BrowseHandler handler;

        return handler;
    }

    std::string
    tr(const char* text)
    {
        return EffectInstance::tr(text).toStdString();
    }

} // namespace

LensKnobs
LensKnobs::create(KnobHolder* holder,
                  const KnobPagePtr& page,
                  const LensKnobOptions& options)
{
    LensKnobs knobs;

    {
        std::vector<ChoiceOption> entries;
        for (const LensModelOption& o : options.leadingModels) {
            entries.push_back(ChoiceOption(o.id, o.label, o.hint));
        }
        entries.push_back(ChoiceOption(kLensModelDatabase, tr("Database"),
                                       tr("A lens from the bundled lens database, chosen with Browse…")));
        entries.push_back(ChoiceOption(kLensModelFromFile, tr("From File"),
                                       tr("A Zemax (.zmx) or pbrt (.dat) lens prescription file.")));
        int defaultIndex = 0;
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].id == options.defaultModel) {
                defaultIndex = (int)i;
            }
        }
        KnobChoicePtr model = AppManager::createKnob<KnobChoice>(holder, tr("Lens Model"));
        model->setName("lensModel");
        model->setHintToolTip(tr("Where the lens prescription comes from."));
        model->populateChoices(entries);
        model->setDefaultValue(defaultIndex);
        model->setAnimationEnabled(false);
        page->addKnob(model);
        knobs._model = model;
    }
    {
        KnobStringPtr lens = AppManager::createKnob<KnobString>(holder, tr("Lens"));
        lens->setName("lens");
        lens->setHintToolTip(tr("The id of the database lens, \"<vendor>/<name>\". Browse… picks one from the lens browser."));
        lens->setDefaultValue(options.defaultLens);
        lens->setAnimationEnabled(false);
        lens->setAddNewLine(false);
        page->addKnob(lens);
        knobs._lens = lens;

        KnobButtonPtr browse = AppManager::createKnob<KnobButton>(holder, tr("Browse…"));
        browse->setName("lensBrowse");
        browse->setHintToolTip(tr("Opens the lens browser to pick a database lens."));
        browse->setEvaluateOnChange(false);
        page->addKnob(browse);
        knobs._browse = browse;
    }
    {
        KnobFilePtr file = AppManager::createKnob<KnobFile>(holder, tr("Lens File"));
        file->setName("lensFile");
        file->setHintToolTip(tr("A Zemax (.zmx) or pbrt (.dat) lens prescription file. A file that does not parse is a node "
                                "error naming the line."));
        file->setFileExtensions({ "zmx", "dat" });
        file->setAnimationEnabled(false);
        page->addKnob(file);
        knobs._file = file;
    }
    {
        std::vector<ChoiceOption> entries;
        for (const char* vendor : kGlassVendors) {
            entries.push_back(ChoiceOption(vendor, vendor, std::string()));
        }
        KnobChoicePtr vendor = AppManager::createKnob<KnobChoice>(holder, tr("Glass Vendor"));
        vendor->setName("glassVendor");
        vendor->setHintToolTip(tr("The glass catalogue each surface's index and Abbe number are matched in, which sets how "
                                  "its refractive index varies with wavelength."));
        vendor->populateChoices(entries);
        vendor->setDefaultValue(0);
        vendor->setAnimationEnabled(false);
        page->addKnob(vendor);
        knobs._glassVendor = vendor;
    }
    {
        KnobDoublePtr abbe = AppManager::createKnob<KnobDouble>(holder, tr("Abbe Offset"));
        abbe->setName("abbeOffset");
        abbe->setHintToolTip(tr("Added to every glass's Abbe number before matching: negative values give more dispersion "
                                "(stronger colour fringes), positive values less."));
        abbe->setMinimum(-20.);
        abbe->setMaximum(20.);
        abbe->setDisplayMinimum(-20.);
        abbe->setDisplayMaximum(20.);
        abbe->setDefaultValue(0.);
        page->addKnob(abbe);
        knobs._abbeOffset = abbe;
    }
    {
        KnobBoolPtr aspheric = AppManager::createKnob<KnobBool>(holder, tr("Use Aspheric"));
        aspheric->setName("useAspheric");
        aspheric->setHintToolTip(tr("When off, aspheric surfaces are traced as spheres of the same curvature."));
        aspheric->setDefaultValue(true);
        aspheric->setAnimationEnabled(false);
        page->addKnob(aspheric);
        knobs._useAspheric = aspheric;
    }
    knobs.refreshVisibility();

    return knobs;
}

void
LensKnobs::setBrowseHandler(const BrowseHandler& handler)
{
    browseHandler() = handler;
}

bool
LensKnobs::hasBrowseHandler()
{
    return bool(browseHandler());
}

std::string
LensKnobs::model(const KnobReader& reader) const
{
    KnobChoicePtr model = _model.lock();
    const int index = reader.value(model);

    return model->getEntry(index).id;
}

bool
LensKnobs::resolve(const KnobReader& reader,
                   Prescription* out,
                   std::string* error) const
{
    const std::string which = model(reader);
    if (which == kLensModelDatabase) {
        const std::string id = reader.value(_lens);
        if (id.empty()) {
            *error = tr("No lens is chosen: pick one with Browse…");

            return false;
        }

        return LensDatabase::shared().prescription(id, out, error);
    }
    if (which == kLensModelFromFile) {
        std::string path = reader.value(_file);
        if (path.empty()) {
            *error = tr("No lens file is chosen.");

            return false;
        }
        KnobFilePtr file = _file.lock();
        if (file && file->getHolder() && file->getHolder()->getApp()) {
            file->getHolder()->getApp()->getProject()->canonicalizePath(path);
        }
        if (FileStampRegistry::hasFramePattern(path)) {
            *error = tr("A lens file cannot be an image sequence.");

            return false;
        }
        LensFileError fileError;
        if (!loadLensFile(path, out, &fileError)) {
            *error = path + ": " + fileError.text();

            return false;
        }

        return true;
    }
    *error = "the lens model \"" + which + "\" has no prescription";

    return false;
}

std::string
LensKnobs::glassVendor(const KnobReader& reader) const
{
    KnobChoicePtr vendor = _glassVendor.lock();

    return vendor->getEntry(reader.value(vendor)).id;
}

double
LensKnobs::abbeOffset(const KnobReader& reader) const
{
    return reader.value(_abbeOffset);
}

bool
LensKnobs::useAspheric(const KnobReader& reader) const
{
    return reader.value(_useAspheric);
}

bool
LensKnobs::knobChanged(KnobI* k) const
{
    if (!k) {
        return false;
    }
    if (k == _model.lock().get()) {
        refreshVisibility();

        return true;
    }
    if (k == _browse.lock().get()) {
        KnobStringPtr lens = _lens.lock();
        if (lens && browseHandler()) {
            browseHandler()(lens);
        }

        return true;
    }

    return k == _lens.lock().get() || k == _file.lock().get() || k == _glassVendor.lock().get() || k == _abbeOffset.lock().get() || k == _useAspheric.lock().get();
}

void
LensKnobs::refreshVisibility() const
{
    KnobChoicePtr model = _model.lock();
    if (!model) {
        return;
    }
    const std::string which = model->getEntry(model->getValue()).id;
    const bool database = which == kLensModelDatabase;
    if (KnobStringPtr lens = _lens.lock()) {
        lens->setSecret(!database);
    }
    if (KnobButtonPtr browse = _browse.lock()) {
        browse->setSecret(!database);
    }
    if (KnobFilePtr file = _file.lock()) {
        file->setSecret(which != kLensModelFromFile);
    }
}

} // namespace Optics

NATRON_NAMESPACE_EXIT
