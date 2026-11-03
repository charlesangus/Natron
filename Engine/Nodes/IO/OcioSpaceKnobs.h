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

#include "Engine/EngineFwd.h"
#include "Global/Enums.h"

// The context group of the native Read and Write, whose pairs are contextKey1..4 and contextValue1..4.
#define kNatronIoParamOCIOContext "ocioContext"

NATRON_NAMESPACE_ENTER

class ContentHasher;

/**
 * @brief The OpenColorIO colourspace knobs a native node shares: the hidden ocioConfigFile and
 * ocioWorkingSpace the project pushes, the space itself, and a context group of four key and
 * value pairs.
 *
 * A space is either a persistent `<prefix>` string, a cascading `<prefix>Index` menu over the
 * project's config and a `<prefix>Set` flag recording a user choice, the knobs taking the node's
 * automatic default while the user has not chosen; or, with Description::projectDefault, a single
 * persistent `<prefix>` menu whose first entry, Project default, follows the automatic default
 * live and is all that is saved of it.
 **/
class OcioSpaceKnobs {
    Q_DECLARE_TR_FUNCTIONS(OcioSpaceKnobs)

public:
    struct Description {
        std::string prefix; // the space's script name; without projectDefault, the menu and flag append Index and Set
        QString label; // the string knob's and the menu's
        QString spaceHint;
        QString menuHint;
        QString setLabel;
        bool projectDefault = false;
        // The script names of the context group and of its pairs, which append 1 to 4.
        std::string contextName = "Context";
        std::string contextKeyPrefix = "key";
        std::string contextValuePrefix = "value";
        // With a %1 for the pair's number; empty labels a pair knob with its script name.
        QString contextKeyLabel;
        QString contextValueLabel;
    };

    /**
     * @brief The id of the Project default entry of a projectDefault menu.
     **/
    static const char* projectDefaultID();

    /**
     * @brief The node's automatic default space. No value when it cannot be told now, which keeps
     * the current space. An empty string means no conversion; it is only taken on a reset.
     **/
    typedef std::function<std::optional<std::string>()> AutomaticSpace;

    /**
     * @brief Called after the user picked, typed or reset the space.
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
     * @brief As create(), for a further space of a node whose knobs \p first created: these take
     * first's hidden ocioConfigFile and ocioWorkingSpace and its Context group instead of
     * creating their own, and go on \p page before that group.
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
     * @brief The node's automatic default may have changed: takes it, unless the user chose a
     * space or the node is being loaded or pasted, which keeps the space it was saved with.
     * With projectDefault, it is taken whatever the menu shows, and the node's hash is marked
     * when Project default is chosen and the default moved.
     **/
    void sourceChanged();

    /**
     * @brief As sourceChanged() with projectDefault, also while the node is loaded or pasted.
     * Does nothing without projectDefault.
     **/
    void refreshProjectDefault();

    /**
     * @brief Appends the space Project default stands for while it is chosen, the knobs holding
     * no trace of it.
     **/
    void appendHash(ContentHasher& h) const;

    /**
     * @brief Clears the user choice and returns to the automatic default. Idempotent, since the
     * Gui delivers a reset to default twice.
     **/
    void reset();

    /**
     * @brief Rebuilds the menu from the project's config and selects the space in it, showing
     * the string knob instead when the config has no colourspace of that name.
     **/
    void refreshMenu();

    /**
     * @brief Hides every visible knob of the set, for a file the space does not apply to, or
     * shows them again.
     **/
    void setHidden(bool hidden);

    /**
     * @brief The space the node converts with; with projectDefault and Project default chosen,
     * the automatic default last taken.
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
     * @brief The Context group these knobs read their variables from.
     **/
    KnobGroupPtr contextGroup() const;

private:
    ProjectPtr project() const;

    /**
     * @brief Creates the string knob, the menu and the flag on \p page, or the projectDefault
     * menu alone, inserted from \p index on, or appended when \p index is negative.
     **/
    void createSpaceKnobs(KnobHolder* holder,
                          const KnobPagePtr& page,
                          const Description& description,
                          int index);

    KnobHolder* _holder;
    AutomaticSpace _automaticSpace;
    ChoiceMade _choiceMade;
    KnobStringWPtr _configFile;
    KnobStringWPtr _workingSpace;
    KnobStringWPtr _space;
    KnobChoiceWPtr _menu;
    KnobBoolWPtr _spaceSet;
    KnobStringWPtr _contextKeys[4];
    KnobStringWPtr _contextValues[4];
    KnobGroupWPtr _context;
    bool _hidden;
    bool _projectDefault;
    mutable std::mutex _defaultMutex;
    std::string _defaultSpace;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_IO_OcioSpaceKnobs_h
