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

#include "Gui.h"
#include "GuiPrivate.h"

#include <algorithm> // min, max
#include <cassert>
#include <exception>
#include <fstream>
#include <stdexcept>
#include <string>

#include <QApplication> // qApp

#include "Global/FStreamsSupport.h"

#include "Engine/AppManager.h" // Dialogs
#include "Engine/Node.h"
#include "Engine/RotoLayer.h"

#include "Gui/CurveEditor.h"
#include "Gui/DockablePanel.h"
#include "Gui/DopeSheetEditor.h"
#include "Gui/FloatingWidget.h"
#include "Gui/Histogram.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGui.h"
#include "Gui/NodeSettingsPanel.h"
#include "Gui/ProjectGui.h"
#include "Gui/PropertiesBinWrapper.h"
#include "Gui/SequenceFileDialog.h"
#include "Gui/Splitter.h"
#include "Gui/TabWidget.h"
#include "Gui/ViewerTab.h"

#include "Global/QtCompat.h" // removeFileExtension

#include "Serialization/ProjectGuiSerialization.h"
#include "Serialization/Yaml/ProjectGuiYaml.h"

#ifdef __NATRON_WIN32__
#if _WIN32_WINNT < 0x0500
#define _WIN32_WINNT 0x0500
#endif
#endif

NATRON_NAMESPACE_ENTER

void
Gui::createDefaultLayout1()
{
    ///First tab widget must be created this way
    TabWidget* mainPane = new TabWidget(this, _imp->_leftRightSplitter);
    {
        QMutexLocker l(&_imp->_panesMutex);
        _imp->_panes.push_back(mainPane);
    }

    mainPane->setObjectName_mt_safe( QString::fromUtf8("pane1") );
    mainPane->setAsAnchor(true);

    _imp->_leftRightSplitter->addWidget(mainPane);

    QList<int> sizes;
    sizes << _imp->_toolBox->sizeHint().width() << width();
    _imp->_leftRightSplitter->setSizes_mt_safe(sizes);


    TabWidget* propertiesPane = mainPane->splitHorizontally(false);
    TabWidget* workshopPane = mainPane->splitVertically(false);
    Splitter* propertiesSplitter = dynamic_cast<Splitter*>( propertiesPane->parentWidget() );
    assert(propertiesSplitter);
    sizes.clear();
    sizes << width() * 0.65 << width() * 0.35;
    propertiesSplitter->setSizes_mt_safe(sizes);

    TabWidget::moveTab(_imp->_nodeGraphArea, _imp->_nodeGraphArea, workshopPane);
    TabWidget::moveTab(_imp->_curveEditor, _imp->_curveEditor, workshopPane);
    TabWidget::moveTab(_imp->_dopeSheetEditor, _imp->_dopeSheetEditor, workshopPane);
    TabWidget::moveTab(_imp->_propertiesBin, _imp->_propertiesBin, propertiesPane);

    {
        QMutexLocker l(&_imp->_viewerTabsMutex);
        for (std::list<ViewerTab*>::iterator it2 = _imp->_viewerTabs.begin(); it2 != _imp->_viewerTabs.end(); ++it2) {
            TabWidget::moveTab(*it2, *it2, mainPane);
        }
    }
    {
        QMutexLocker l(&_imp->_histogramsMutex);
        for (std::list<Histogram*>::iterator it2 = _imp->_histograms.begin(); it2 != _imp->_histograms.end(); ++it2) {
            TabWidget::moveTab(*it2, *it2, mainPane);
        }
    }


    ///Default to NodeGraph displayed
    workshopPane->makeCurrentTab(0);
}

void
Gui::exportLayout()
{
    std::vector<std::string> filters;

    filters.push_back(NATRON_LAYOUT_FILE_EXT);
    SequenceFileDialog dialog( this, filters, false, SequenceFileDialog::eFileDialogModeSave, _imp->_lastSaveProjectOpenedDir.toStdString(), this, false );
    if ( dialog.exec() ) {
        std::string filename = dialog.filesToSave();
        QString filenameCpy( QString::fromUtf8( filename.c_str() ) );
        QString ext = QtCompat::removeFileExtension(filenameCpy);
        if ( ext != QString::fromUtf8(NATRON_LAYOUT_FILE_EXT) ) {
            filename.append("." NATRON_LAYOUT_FILE_EXT);
        }

        std::string text;
        try {
            Serialization::GuiLayoutSerialization layoutSerialization;
            toSerialization(&layoutSerialization);
            text = Serialization::encodeGuiLayoutYaml(layoutSerialization);
        } catch (const std::exception& e) {
            Dialogs::errorDialog(tr("Error").toStdString(), tr("Failed to save the layout: %1").arg(QString::fromUtf8(e.what())).toStdString(), false);

            return;
        }

        FStreamsSupport::ofstream ofile;
        FStreamsSupport::open(&ofile, filename);
        if (!ofile) {
            Dialogs::errorDialog( tr("Error").toStdString()
                                  , tr("Failed to open file ").toStdString() + filename, false );

            return;
        }
        ofile << text;
        ofile.close();
        if (ofile.fail()) {
            Dialogs::errorDialog(tr("Error").toStdString(), tr("Failed to save the layout").toStdString(), false);
        }
    }
}

const QString &
Gui::getLastLoadProjectDirectory() const
{
    return _imp->_lastLoadProjectOpenedDir;
}

const QString &
Gui::getLastSaveProjectDirectory() const
{
    return _imp->_lastSaveProjectOpenedDir;
}

const QString &
Gui::getLastPluginDirectory() const
{
    return _imp->_lastPluginDir;
}

void
Gui::updateLastPluginDirectory(const QString & str)
{
    _imp->_lastPluginDir = str;
}

void
Gui::onShowApplicationConsoleActionTriggered()
{
    setApplicationConsoleActionVisible(!_imp->applicationConsoleVisible);
}

void
Gui::setApplicationConsoleActionVisible(bool visible)
{
#ifdef __NATRON_WIN32__
    if (visible == _imp->applicationConsoleVisible) {
        return;
    }
    _imp->applicationConsoleVisible = visible;
    HWND hWnd = GetConsoleWindow();
    if (hWnd) {
        if (_imp->applicationConsoleVisible) {
            ShowWindow(hWnd, SW_HIDE);
        } else {
            ShowWindow(hWnd, SW_SHOWNORMAL);
        }
    }
#else
    Q_UNUSED(visible)
#endif
}

NATRON_NAMESPACE_EXIT

