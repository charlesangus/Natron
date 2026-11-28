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

#ifndef Engine_TemplateNode_h
#define Engine_TemplateNode_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/NoOpBase.h"

NATRON_NAMESPACE_ENTER

struct TemplateNodePrivate;

/**
 * @brief The node that stands for a template import in the graph: it shows the import's
 * template identity and versions and runs its template actions. It has no inputs, no output and
 * never renders. Its templateKey knob names the import's entry in the project; the knobs follow
 * that entry, and edits of the name and publish folder go to it.
 **/
class TemplateNode
    : public NoOpBase {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new TemplateNode(n);
    }

    TemplateNode(NodePtr node);

    virtual ~TemplateNode();

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return PLUGINID_NATRON_TEMPLATE;
    }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return "Template";
    }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual std::string getInputLabel(int /*inputNb*/) const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return "";
    }

    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return 0;
    }

    std::string getTemplateKey() const;

    /**
     * @brief Sets the newer-version indicator: the path of a newer published version, or empty
     * when the import is current.
     **/
    void setNewerVersion(const std::string& text);

public Q_SLOTS:

    /**
     * @brief Sets the knobs from the project's entry for the template key; nothing changes when
     * the project has no such entry.
     **/
    void refreshFromEntry();

private:
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    void writeThrough();
    std::unique_ptr<TemplateNodePrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_TemplateNode_h
