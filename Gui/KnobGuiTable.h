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

#ifndef Gui_KnobGuiLayer_h
#define Gui_KnobGuiLayer_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Gui/GuiFwd.h"
#include "Gui/KnobGui.h"

NATRON_NAMESPACE_ENTER

struct KnobGuiTablePrivate;
class KnobGuiTable
    : public KnobGui
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    KnobGuiTable(KnobIPtr knob,
                 KnobGuiContainerI *container);

    virtual ~KnobGuiTable() OVERRIDE;

    virtual void removeSpecificGui() OVERRIDE;

    int rowCount() const;

public Q_SLOTS:

    virtual void onAddButtonClicked();

    virtual void onRemoveButtonClicked();

    void onEditButtonClicked();

    void onItemDataChanged(TableItem* item);

    void onItemAboutToDrop();

    void onItemDropped();

    void onItemDoubleClicked(TableItem* item);

protected:

    virtual void setDirty(bool /*dirty*/) OVERRIDE
    {
    }

    virtual void createWidget(QHBoxLayout *layout) OVERRIDE;
    virtual void _hide() OVERRIDE;
    virtual void _show() OVERRIDE;
    virtual void setEnabled() OVERRIDE;
    virtual void setReadOnly(bool readOnly, int dimension) OVERRIDE;
    virtual void updateGUI(int dimension) OVERRIDE;
    virtual void reflectAnimationLevel(int /*dimension*/,
                                       AnimationLevelEnum /*level*/) OVERRIDE
    {
    }

    virtual void reflectExpressionState(int /*dimension*/,
                                        bool /*hasExpr*/) OVERRIDE
    {
    }

    virtual void updateToolTip() OVERRIDE;

    QStringList rowValues(int row) const;

    QList<int> selectedRowIndices() const;

    virtual void createExtraButtons(QWidget* /*parent*/,
                                    QHBoxLayout* /*layout*/)
    {
    }

    virtual bool addNewUserEntry(QStringList& /*row*/)
    {
        return false;
    }

    // row has been set-up with old value
    virtual bool editUserEntry(QStringList& /*row*/)
    {
        return false;
    }

    virtual void entryRemoved(const QStringList& /*row*/)  {}

    virtual void tableChanged(int /*row*/,
                              int /*col*/,
                              std::string* /*newEncodedValue*/)
    {
    }

private:

    virtual bool shouldAddStretch() const OVERRIDE FINAL { return false; }

    std::unique_ptr<KnobGuiTablePrivate> _imp;
};

class KnobGuiLayers
    : public KnobGuiTable
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static KnobGui * BuildKnobGui(KnobIPtr knob,
                                  KnobGuiContainerI *container)
    {
        return new KnobGuiLayers(knob, container);
    }

    KnobGuiLayers(KnobIPtr knob,
                  KnobGuiContainerI *container);

    virtual ~KnobGuiLayers() OVERRIDE;


    virtual KnobIPtr getKnob() const OVERRIDE FINAL;

public Q_SLOTS:

    virtual void onAddButtonClicked() OVERRIDE FINAL;

    virtual void onRemoveButtonClicked() OVERRIDE FINAL;

    void onRemoveUnusedButtonClicked();

private:
    virtual void createExtraButtons(QWidget* parent, QHBoxLayout* layout) OVERRIDE FINAL;

    ProjectPtr getProject() const;

    KnobLayersWPtr _knob;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_KnobGuiLayer_h
