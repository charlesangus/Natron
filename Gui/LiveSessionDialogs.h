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

#ifndef Gui_LiveSessionDialogs_h
#define Gui_LiveSessionDialogs_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <QDialog>
#include <QLabel>
#include <QString>

#include "Engine/EngineFwd.h"
#include "Gui/GuiFwd.h"

class QComboBox;
class QSpinBox;

NATRON_NAMESPACE_ENTER

/**
 * @brief Asks where to host a live session of the project: the address to listen on, the port
 * and the name the other users see. Warns that anyone who can reach the port can join. The
 * choices are remembered for the next session.
 **/
class HostLiveSessionDialog
    : public QDialog {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    explicit HostLiveSessionDialog(QWidget* parent);

    virtual ~HostLiveSessionDialog() OVERRIDE;

    /**
     * @brief The IP address to listen on, empty for every interface.
     **/
    QString getBindAddress() const;

    quint16 getPort() const;

    QString getDisplayName() const;

    /**
     * @brief Remembers the choices as the next dialog's defaults.
     **/
    void rememberChoices() const;

private:
    QComboBox* _bindAddress;
    QSpinBox* _port;
    LineEdit* _displayName;
};

/**
 * @brief Asks which live session to join: the host's address, its port and the name the other
 * users see. The hosts joined recently are offered again.
 **/
class JoinLiveSessionDialog
    : public QDialog {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    explicit JoinLiveSessionDialog(QWidget* parent);

    virtual ~JoinLiveSessionDialog() OVERRIDE;

    QString getHost() const;

    quint16 getPort() const;

    QString getDisplayName() const;

    /**
     * @brief Puts the host and port first in the recent hosts, and remembers the display name.
     **/
    void rememberChoices() const;

public Q_SLOTS:

    void onHostActivated(int index);

    void refreshButtons();

private:
    QComboBox* _host;
    QSpinBox* _port;
    LineEdit* _displayName;
    DialogButtonBox* _buttons;
};

/**
 * @brief The status bar's live session indicator: whether the app hosts or joined a session,
 * where, and how many users joined the host. Hidden without a session.
 **/
class LiveSessionStatus
    : public QLabel {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    LiveSessionStatus(const AppInstancePtr& app, QWidget* parent);

    virtual ~LiveSessionStatus() OVERRIDE;

public Q_SLOTS:

    void refresh();

    void onSessionMessage(const QString& message);

private:
    AppInstanceWPtr _app;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_LiveSessionDialogs_h
