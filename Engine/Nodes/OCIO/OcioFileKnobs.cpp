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

#include "OcioFileKnobs.h"

#include <exception>
#include <vector>

#include <QFileInfo>

#include <OpenColorIO/OpenColorIO.h>

#include "Global/FloatingPointExceptions.h"

#include "Engine/AppManager.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/OCIO/OcioNodeBase.h"

NATRON_NAMESPACE_ENTER

OcioFileKnobs::OcioFileKnobs()
    : _file()
    , _cccid()
    , _cccidMenu()
    , _reload()
{
}

void
OcioFileKnobs::create(KnobHolder* holder,
                      const KnobPagePtr& page,
                      OcioNodeKnobs& ocio,
                      const QString& fileHint)
{
    KnobFilePtr file = AppManager::createKnob<KnobFile>(holder, tr("File").toStdString());
    file->setName(kOcioFileParamFile);
    file->setHintToolTip(fileHint.toStdString());
    file->setAnimationEnabled(false);
    ocio.addKnob(page, file);
    _file = file;

    KnobButtonPtr reload = AppManager::createKnob<KnobButton>(holder, tr("Reload").toStdString());
    reload->setName(kOcioFileParamReload);
    reload->setHintToolTip(tr("Reads the file again, so a file changed on disk is what the node applies.").toStdString());
    ocio.addKnob(page, reload);
    _reload = reload;

    KnobStringPtr cccid = AppManager::createKnob<KnobString>(holder, tr("CCC Id").toStdString());
    cccid->setName(kOcioFileParamCCCId);
    cccid->setHintToolTip(tr("For a .ccc or .cdl file, which holds several colour corrections, the id or the index of the "
                             "one to apply; the first when empty.")
                              .toStdString());
    cccid->setAnimationEnabled(false);
    cccid->setAddNewLine(false);
    ocio.addKnob(page, cccid);
    _cccid = cccid;

    KnobChoicePtr menu = AppManager::createKnob<KnobChoice>(holder, tr("CCC Ids").toStdString());
    menu->setName(kOcioFileParamCCCIdMenu);
    menu->setHintToolTip(tr("The ids of the colour corrections in the file. Picking one sets the CCC Id.").toStdString());
    menu->setAnimationEnabled(false);
    menu->setIsPersistent(false);
    menu->setEvaluateOnChange(false);
    menu->setLabel(std::string());
    menu->setSecret(true);
    ocio.addKnob(page, menu);
    _cccidMenu = menu;
}

KnobFilePtr
OcioFileKnobs::fileKnob() const
{
    return _file.lock();
}

KnobStringPtr
OcioFileKnobs::cccidKnob() const
{
    return _cccid.lock();
}

std::string
OcioFileKnobs::path() const
{
    KnobFilePtr file = _file.lock();

    return file ? file->getResolvedPath() : std::string();
}

std::string
OcioFileKnobs::cccid() const
{
    KnobStringPtr knob = _cccid.lock();

    return knob ? knob->getValue() : std::string();
}

void
OcioFileKnobs::refreshCCCIdMenu()
{
    KnobChoicePtr menu = _cccidMenu.lock();

    if (!menu) {
        return;
    }
    std::vector<std::string> ids;
    const std::string src = path();
    const QString suffix = QFileInfo(QString::fromStdString(src)).suffix().toLower();
    // Only the CDL formats hold ids; a LUT is not parsed here.
    if ((suffix == QString::fromUtf8("cc")) || (suffix == QString::fromUtf8("ccc")) || (suffix == QString::fromUtf8("cdl"))) {
        try {
            const boost_adaptbx::floating_point::FpTrapGuard fpGuard;
            OCIO_NAMESPACE::GroupTransformRcPtr group = OCIO_NAMESPACE::CDLTransform::CreateGroupFromFile(src.c_str());
            for (int i = 0; i < group->getNumTransforms(); ++i) {
                OCIO_NAMESPACE::ConstCDLTransformRcPtr cdl = OCIO_NAMESPACE::DynamicPtrCast<const OCIO_NAMESPACE::CDLTransform>(group->getTransform(i));
                const char* id = cdl ? cdl->getID() : nullptr;
                if (id && *id) {
                    ids.push_back(id);
                }
            }
        } catch (const std::exception&) {
            // The render reports a file it cannot read.
            ids.clear();
        }
    }
    OcioNodeKnobs::populateMenu(menu, ids);
    menu->setSecret(ids.empty());
    const std::string chosen = cccid();
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (ids[i] == chosen) {
            menu->setValueFromID(chosen, 0);
            break;
        }
    }
}

bool
OcioFileKnobs::knobChanged(KnobI* k,
                           ValueChangedReasonEnum reason,
                           const OcioNodeKnobs& ocio)
{
    if (reason == eValueChangedReasonTimeChanged) {
        return false;
    }
    KnobFilePtr file = _file.lock();
    KnobButtonPtr reload = _reload.lock();
    KnobStringPtr cccidKnob = _cccid.lock();
    KnobChoicePtr menu = _cccidMenu.lock();

    if (reload && (k == reload.get())) {
        // OpenColorIO keeps what it read of a file for the whole process, and the project keeps
        // the processors built from it: both go before the file knob re-keys and re-renders this
        // node and every other node whose file knob reads the same file.
        ocio.reloadFile(path());
        if (file) {
            file->reloadFile();
        }
        refreshCCCIdMenu();

        return true;
    }
    if (file && (k == file.get())) {
        refreshCCCIdMenu();

        return true;
    }
    if (menu && (k == menu.get())) {
        if ((reason == eValueChangedReasonUserEdited) && cccidKnob) {
            cccidKnob->setValue(menu->getActiveEntry().id);
        }

        return true;
    }
    if (cccidKnob && (k == cccidKnob.get())) {
        return true;
    }

    return false;
}

NATRON_NAMESPACE_EXIT
