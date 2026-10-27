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

#ifndef Gui_RenderDialog_h
#define Gui_RenderDialog_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <memory>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QDialog>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppInstance.h"

#include "Gui/GuiFwd.h"

NATRON_NAMESPACE_ENTER

enum RenderRangePresetEnum {
    eRenderRangePresetWriteNodeRange,
    eRenderRangePresetProjectRange,
    eRenderRangePresetCustom
};

struct RenderRangeCustom {
    int first;
    int last;
    int step;

    RenderRangeCustom()
        : first(1)
        , last(1)
        , step(1)
    {
    }
};

enum RenderRangeErrorEnum {
    eRenderRangeErrorNone,
    eRenderRangeErrorFirstAfterLast,
    eRenderRangeErrorStepBelowOne,
    eRenderRangeErrorNegativeFirstInSeparateProcess
};

struct RenderRange {
    int first;
    int last;
    int step;
    bool error;
    RenderRangeErrorEnum errorKind;
};

// Writer bounds of INT_MIN/INT_MAX (an unresolved Write range) fall back to the project
// range, matching AppInstancePrivate::validateRenderOptions. The frame step never comes
// from the project; it is either the writer's own step or, for Custom, the typed one.
// A separate render process takes its range on the command line, whose syntax cannot express
// a negative frame, so separateProcess rejects a first frame below 0.
RenderRange resolveRenderRange(RenderRangePresetEnum preset,
                               int writerFirst,
                               int writerLast,
                               int writerStep,
                               int projectFirst,
                               int projectLast,
                               const RenderRangeCustom& custom,
                               bool separateProcess = false);

struct RenderDialogPrivate;

class RenderDialog
    : public QDialog {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    RenderDialog(Gui* gui, const std::list<NodePtr>& writers);

    virtual ~RenderDialog();

    std::list<AppInstance::RenderWork> getRenderWorks() const;

    bool isBackgroundChecked() const;

public Q_SLOTS:

    void onPresetChanged(int index);
    void onRangeValueChanged(double value);
    void onBackgroundToggled(bool checked);
    void onOkClicked();

private:
    void refreshCustomFieldsEnabled();
    void refreshValidation();

    std::unique_ptr<RenderDialogPrivate> _imp;
};

// Opens the render dialog for the given writers and, if accepted, starts the render.
// Returns false if the dialog was not shown or was cancelled.
// A free function, rather than a Gui method, so callers only need this header.
bool showRenderDialog(Gui* gui, const std::list<NodePtr>& writers);

NATRON_NAMESPACE_EXIT

#endif // Gui_RenderDialog_h
