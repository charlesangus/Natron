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

#ifndef Engine_Nodes_IO_OcioSpaceKnobs_h
#define Engine_Nodes_IO_OcioSpaceKnobs_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QString>

#include "Engine/ChoiceOption.h"
#include "Engine/EngineFwd.h"
#include "Global/Enums.h"

// The context group of every node with OpenColorIO spaces, whose pairs are contextKey1..4 and
// contextValue1..4.
#define kNatronOcioParamContext "ocioContext"

NATRON_NAMESPACE_ENTER

class ContentHasher;

/**
 * @brief The OpenColorIO colourspace knobs a native node shares: the hidden ocioConfigFile and
 * ocioWorkingSpace the project pushes, the space itself, and a context group of four key and
 * value pairs.
 *
 * The space is a single persistent menu over the project's config whose first entry, Project
 * default, follows the node's automatic default live, is all that is saved of it, and shows in
 * its label the space it currently stands for.
 **/
class OcioSpaceKnobs {
    Q_DECLARE_TR_FUNCTIONS(OcioSpaceKnobs)

public:
    struct Description {
        std::string name; // the menu's script name
        QString label;
        QString hint; // the menu's
        QString defaultHint; // its Project default entry's
    };

    /**
     * @brief The id of the Project default entry of a menu.
     **/
    static const char* projectDefaultID();

    /**
     * @brief A Project default entry labelled with \p resolved, the name it currently stands for,
     * or plainly while that is empty.
     **/
    static ChoiceOption projectDefaultOption(const std::string& resolved,
                                             const QString& hint);

    /**
     * @brief The node's automatic default space, which Project default stands for. No value when
     * it cannot be told now, which keeps the one taken before. An empty string means no
     * conversion.
     **/
    typedef std::function<std::optional<std::string>()> AutomaticSpace;

    /**
     * @brief Called after the user picked a space or reset the menu.
     **/
    typedef std::function<void()> ChoiceMade;

    OcioSpaceKnobs();

    /**
     * @brief Creates the knobs on \p page, in the order listed in the class description, and
     * fills the menu.
     **/
    void create(KnobHolder* holder,
                const KnobPagePtr& page,
                const Description& description,
                const AutomaticSpace& automaticSpace,
                const ChoiceMade& choiceMade = ChoiceMade());

    /**
     * @brief As create(), for a further space of a node whose knobs \p first created: this takes
     * first's hidden ocioConfigFile and ocioWorkingSpace and its context group instead of
     * creating its own, and its menu goes on \p page before that group.
     **/
    void createSharing(const OcioSpaceKnobs& first,
                       KnobHolder* holder,
                       const KnobPagePtr& page,
                       const Description& description,
                       const AutomaticSpace& automaticSpace,
                       const ChoiceMade& choiceMade = ChoiceMade());

    /**
     * @brief Handles a change of one of these knobs. Returns true when \p k is one of them,
     * false otherwise and for time changes.
     **/
    bool knobChanged(KnobI* k,
                     ValueChangedReasonEnum reason);

    /**
     * @brief The node's automatic default may have changed: takes it, unless the node is being
     * loaded or pasted, whose onKnobsLoaded() calls refreshProjectDefault().
     **/
    void sourceChanged();

    /**
     * @brief Takes the automatic default, relabels the Project default entry with it, and marks
     * the node's hash when Project default is chosen and the default moved.
     **/
    void refreshProjectDefault();

    /**
     * @brief Appends the space Project default stands for while it is chosen, the knobs holding
     * no trace of it.
     **/
    void appendHash(ContentHasher& h) const;

    /**
     * @brief Rebuilds the menu from the project's config, keeping the entry chosen. A space the
     * config lacks stays chosen, by name, until a config that has it comes back.
     **/
    void refreshMenu();

    /**
     * @brief Hides every visible knob of the set, for a file the space does not apply to, or
     * shows them again.
     **/
    void setHidden(bool hidden);

    /**
     * @brief The space the node converts with; with Project default chosen, the automatic
     * default last taken.
     **/
    std::string space() const;

    bool isProjectDefault() const;

    /**
     * @brief The working space the project pushed, else the project's.
     **/
    std::string workingSpace() const;

    /**
     * @brief The context pairs at \p time whose key is not empty.
     **/
    std::vector<std::pair<std::string, std::string>> contextVariables(double time) const;

    /**
     * @brief The context group these knobs read their variables from.
     **/
    KnobGroupPtr contextGroup() const;

private:
    ProjectPtr project() const;

    /**
     * @brief Creates the menu on \p page at \p index, or appended when \p index is negative.
     **/
    void createMenu(KnobHolder* holder,
                    const KnobPagePtr& page,
                    const Description& description,
                    int index);

    KnobHolder* _holder;
    AutomaticSpace _automaticSpace;
    ChoiceMade _choiceMade;
    QString _defaultHint;
    KnobStringWPtr _configFile;
    KnobStringWPtr _workingSpace;
    KnobChoiceWPtr _menu;
    KnobStringWPtr _contextKeys[4];
    KnobStringWPtr _contextValues[4];
    KnobGroupWPtr _context;
    mutable std::mutex _defaultMutex;
    std::string _defaultSpace;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_IO_OcioSpaceKnobs_h
