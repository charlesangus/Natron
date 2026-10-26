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

#ifndef Engine_Nodes_OCIO_OcioFileKnobs_h
#define Engine_Nodes_OCIO_OcioFileKnobs_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include <QCoreApplication>
#include <QString>

#include "Engine/EngineFwd.h"
#include "Global/Enums.h"

#define kOcioFileParamFile "file"
#define kOcioFileParamCCCId "cccid"
#define kOcioFileParamCCCIdMenu "cccid_menu"
#define kOcioFileParamReload "reload"

NATRON_NAMESPACE_ENTER

class OcioNodeKnobs;

/**
 * @brief The file knobs of the native OCIO nodes that read a transform from disk: the file, the
 * CCC id with a menu of the ids a .ccc or .cdl file holds, and Reload, which makes OpenColorIO
 * read the file again.
 **/
class OcioFileKnobs {
    Q_DECLARE_TR_FUNCTIONS(OcioFileKnobs)

public:
    OcioFileKnobs();

    /**
     * @brief Creates the knobs on \p page, through \p ocio so they go before its Context group.
     **/
    void create(KnobHolder* holder,
                const KnobPagePtr& page,
                OcioNodeKnobs& ocio,
                const QString& fileHint);

    /**
     * @brief The file with project variables expanded, or empty.
     **/
    std::string path() const;

    std::string cccid() const;

    /**
     * @brief Handles a change of one of these knobs: Reload drops what OpenColorIO and \p ocio's
     * project cached of the file, then reloads the file knob, which re-keys and re-renders the
     * node; a new file refills the CCC id menu, and a pick in it sets the CCC id. Returns true
     * when \p k is one of the knobs.
     **/
    bool knobChanged(KnobI* k,
                     ValueChangedReasonEnum reason,
                     const OcioNodeKnobs& ocio);

    /**
     * @brief Fills the CCC id menu from the file, hidden when the file holds no ids.
     **/
    void refreshCCCIdMenu();

    KnobFilePtr fileKnob() const;
    KnobStringPtr cccidKnob() const;

private:
    KnobFileWPtr _file;
    KnobStringWPtr _cccid;
    KnobChoiceWPtr _cccidMenu;
    KnobButtonWPtr _reload;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_OCIO_OcioFileKnobs_h
