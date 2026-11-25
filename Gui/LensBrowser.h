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

#ifndef Gui_LensBrowser_h
#define Gui_LensBrowser_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

CLANG_DIAG_OFF(uninitialized)
#include <QDialog>
#include <QImage>
#include <QSize>
#include <QString>
CLANG_DIAG_ON(uninitialized)

#include "Engine/EngineFwd.h"
#include "Engine/Optics/Prescription.h"
#include "Gui/GuiFwd.h"

NATRON_NAMESPACE_ENTER

struct LensBrowserPrivate;

/**
 * @brief The non-modal lens browser a lensBrowse button opens: the LensDatabase lenses by vendor, a
 * searchable and filterable table, the chosen lens's details, its cross-section with a few traced rays,
 * and one tab per preview provider. OK writes the chosen id into the lens knob as one undoable edit.
 **/
class LensBrowser
    : public QDialog {
    Q_OBJECT

public:
    /**
     * @brief Renders a preview of lens at size, on a worker thread. It returns early with any image once
     * cancelled reads true, which happens when the selection changes or the browser closes.
     **/
    using PreviewProvider = std::function<QImage(const Optics::Prescription& lens,
                                                 const QSize& size,
                                                 const std::atomic<bool>& cancelled)>;

    // Adds a tab to browsers opened from now on. Main thread only.
    static void addPreviewProvider(const QString& name,
                                   const PreviewProvider& provider);

    // Opens a browser for lens, or raises the one already open for it.
    static LensBrowser* open(const KnobStringPtr& lens);

    explicit LensBrowser(const KnobStringPtr& lens,
                         QWidget* parent = 0);
    virtual ~LensBrowser() OVERRIDE;

    // The id of the selected lens, or empty when none is.
    QString selectedId() const;

public Q_SLOTS:
    void onSelectionChanged();
    void onFiltersChanged();
    void onVendorChanged();
    void onPreviewReady();
    virtual void accept() OVERRIDE;

protected:
    virtual void closeEvent(QCloseEvent* e) OVERRIDE;

private:
    std::unique_ptr<LensBrowserPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_LensBrowser_h
