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

#ifndef NATRON_GUI_KNOBEDITMODE_H
#define NATRON_GUI_KNOBEDITMODE_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <memory>
#include <string>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QByteArray>
#include <QObject>
#include <QPoint>
#include <QString>
#include <QStringList>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/EngineFwd.h"
#include "Gui/GuiFwd.h"

#define NATRON_USER_KNOBS_MIME_TYPE "application/x-natron-user-knobs"

NATRON_NAMESPACE_ENTER

struct KnobEditModePrivate;

/**
 * @brief The edit mode of a panel's user knobs. While it is on, the panel shows a palette above
 * its pages and a property section below them, user knobs, groups and pages can be selected,
 * dragged and copied, and built-in knobs are dimmed and inert. Every change goes through the
 * user knob edit API, so each is one undo step.
 **/
class KnobEditMode
    : public QObject {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON
    Q_PROPERTY(bool active READ isActive NOTIFY activeChanged)
    Q_PROPERTY(QString selectedKnob READ getSelectedKnobName NOTIFY selectionChanged)

public:
    explicit KnobEditMode(DockablePanel* panel);

    virtual ~KnobEditMode() OVERRIDE;

    DockablePanel* getPanel() const;

    KnobHolder* getHolder() const;

    bool isActive() const;

    /**
     * @brief Turns edit mode on or off. Turning it on for a PyPlug asks first and marks it
     * edited. Returns whether the mode is now the one asked for.
     **/
    bool setActive(bool active);

    /**
     * @brief Called while the panel is being destroyed: forgets it without touching its widgets.
     **/
    void detach();

    /**
     * @brief Whether any panel is in edit mode, in which case dragging a knob's label carries it
     * to that panel.
     **/
    static bool isAnyPanelEditing();

    KnobIPtr getSelectedKnob() const;

    /**
     * @brief Selects a user knob, group or page of the holder; anything else clears the selection.
     **/
    void setSelectedKnob(const KnobIPtr& knob);

    QString getSelectedKnobName() const;

    /**
     * @brief Called by the panel each time its user knobs were rebuilt.
     **/
    void onKnobsRecreated();

    /**
     * @brief Where a knob added without a position goes: after the selection, at the end of a
     * selected page, or at the end of the first user page. parent is null when the holder has no
     * user page yet.
     **/
    void getInsertionPoint(KnobIPtr* parent, int* index) const;

    /**
     * @brief Adds a knob of the palette entry entryId in parent at index (see getInsertionPoint())
     * and selects it.
     **/
    KnobIPtr addKnob(const QString& entryId, const KnobIPtr& parent, int index);

    /**
     * @brief Moves a user knob or group to index among parent's children, the knob itself not
     * counted.
     **/
    bool moveKnob(const KnobIPtr& knob, const KnobIPtr& parent, int index);

    /**
     * @brief Adds a user knob that is linked to source (an alias), or with copy an independent
     * copy of it, in parent at index, and selects it.
     **/
    KnobIPtr addKnobFrom(const KnobIPtr& source, bool copy, const KnobIPtr& parent, int index);

    /**
     * @brief Removes knob after asking when other knobs link to it or use it in an expression.
     **/
    bool deleteKnob(const KnobIPtr& knob);

    void showContextMenu(const QPoint& globalPos);

    /**
     * @brief The data of an editor drag started from this mode's panel: kind is "new" for a
     * palette entry and "move" for a knob of the panel.
     **/
    static QByteArray dragPayload(const KnobEditMode* mode, const QString& kind, const QString& value);

    /**
     * @brief Reads the data of an editor drag started from this mode's panel. Returns false for
     * a drag from another panel.
     **/
    bool parseDragPayload(const QByteArray& payload, QString* kind, QString* value) const;

    KnobEditProperties* getProperties() const;

public Q_SLOTS:

    /**
     * @brief Selects the user knob, group or page named name. Returns whether it exists.
     **/
    bool selectKnob(const QString& name);

    /**
     * @brief The script names of the children of the page or group parentName, in panel order, or
     * of every page when parentName is empty.
     **/
    QStringList getChildNames(const QString& parentName) const;

    /**
     * @brief Adds a knob of the palette entry entryId at the insertion point.
     **/
    void addKnobOfEntry(const QString& entryId);

    void copySelection();

    void pasteClipboard();

    void duplicateSelection();

    void deleteSelection();

    void showLinkChooser();

Q_SIGNALS:

    void activeChanged(bool active);

    void selectionChanged();

private Q_SLOTS:

    void onTabBarClicked(int index);

    void onPaletteEntryClicked(QString entryId);

private:
    virtual bool eventFilter(QObject* watched, QEvent* event) OVERRIDE FINAL;

    std::unique_ptr<KnobEditModePrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_KNOBEDITMODE_H
