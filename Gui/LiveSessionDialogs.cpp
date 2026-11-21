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

#include "LiveSessionDialogs.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHostAddress>
#include <QLabel>
#include <QNetworkInterface>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStringList>
#include <QSysInfo>
#include <QVBoxLayout>

#include "Engine/AppInstance.h"

#include "Gui/DialogButtonBox.h"
#include "Gui/LineEdit.h"

NATRON_NAMESPACE_ENTER

namespace {

const int kDefaultPort = 47600;
const int kMaxRecentHosts = 10;

QString
settingsKey(const char* name)
{
    return QString::fromUtf8("liveSession/") + QString::fromUtf8(name);
}

QString
defaultDisplayName()
{
    QSettings settings;
    QString name = settings.value(settingsKey("displayName")).toString();
    if (name.isEmpty()) {
        name = qEnvironmentVariable("USER");
    }
    if (name.isEmpty()) {
        name = QSysInfo::machineHostName();
    }

    return name;
}

QSpinBox*
makePortBox(QWidget* parent,
            int value)
{
    QSpinBox* port = new QSpinBox(parent);
    port->setObjectName(QString::fromUtf8("port"));
    port->setRange(1, 65535);
    port->setValue(value);

    return port;
}

LineEdit*
makeDisplayNameEdit(QWidget* parent)
{
    LineEdit* edit = new LineEdit(parent);
    edit->setObjectName(QString::fromUtf8("displayName"));
    edit->setToolTip(QObject::tr("The name the other users of the session see for you."));
    edit->setText(defaultDisplayName());
    edit->setMinimumWidth(280);

    return edit;
}

// "host:port" as kept in the recent hosts; the port follows the last colon, so IPv6 hosts work.
bool
splitRecentHost(const QString& entry,
                QString* host,
                int* port)
{
    const int colon = entry.lastIndexOf(QLatin1Char(':'));
    if (colon <= 0) {
        return false;
    }
    bool ok = false;
    *port = entry.mid(colon + 1).toInt(&ok);
    *host = entry.left(colon);

    return ok && (*port > 0) && (*port <= 65535);
}

} // namespace

HostLiveSessionDialog::HostLiveSessionDialog(QWidget* parent)
    : QDialog(parent)
    , _bindAddress(0)
    , _port(0)
    , _displayName(0)
{
    setObjectName(QString::fromUtf8("hostLiveSessionDialog"));
    setWindowTitle(tr("Host Live Session"));

    QSettings settings;
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    QFormLayout* form = new QFormLayout;
    mainLayout->addLayout(form);

    _bindAddress = new QComboBox(this);
    _bindAddress->setObjectName(QString::fromUtf8("bindAddress"));
    _bindAddress->setToolTip(tr("The network address users join on. This Computer Only (127.0.0.1) lets only "
                                "other Natron windows on this computer join."));
    _bindAddress->addItem(tr("All Network Interfaces"), QString());
    _bindAddress->addItem(tr("This Computer Only (127.0.0.1)"), QString::fromUtf8("127.0.0.1"));
    const QList<QHostAddress> addresses = QNetworkInterface::allAddresses();
    for (const QHostAddress& address : addresses) {
        if (address.isLoopback() || address.isLinkLocal() || (address.protocol() != QAbstractSocket::IPv4Protocol)) {
            continue;
        }
        _bindAddress->addItem(address.toString(), address.toString());
    }
    const int remembered = _bindAddress->findData(settings.value(settingsKey("bindAddress"), QString()).toString());
    _bindAddress->setCurrentIndex((remembered >= 0) ? remembered : 0);
    form->addRow(tr("Bind Address"), _bindAddress);

    _port = makePortBox(this, settings.value(settingsKey("hostPort"), kDefaultPort).toInt());
    _port->setToolTip(tr("The TCP port users join on."));
    form->addRow(tr("Port"), _port);

    _displayName = makeDisplayNameEdit(this);
    form->addRow(tr("Display Name"), _displayName);

    QLabel* warning = new QLabel(this);
    warning->setObjectName(QString::fromUtf8("securityWarning"));
    warning->setWordWrap(true);
    warning->setTextFormat(Qt::RichText);
    warning->setText(tr("<b>Warning: a live session has no password.</b> Anyone who can reach this computer on this "
                        "port can join, see the whole project, change it and save it over its file. Host only on a "
                        "network you trust, or choose This Computer Only."));
    warning->setMinimumWidth(420);
    mainLayout->addWidget(warning);

    DialogButtonBox* buttons = new DialogButtonBox(QDialogButtonBox::StandardButtons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel), Qt::Horizontal, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Host"));
    mainLayout->addWidget(buttons);
    QObject::connect(buttons, SIGNAL(accepted()), this, SLOT(accept()));
    QObject::connect(buttons, SIGNAL(rejected()), this, SLOT(reject()));
}

HostLiveSessionDialog::~HostLiveSessionDialog()
{
}

QString
HostLiveSessionDialog::getBindAddress() const
{
    return _bindAddress->currentData().toString();
}

quint16
HostLiveSessionDialog::getPort() const
{
    return static_cast<quint16>(_port->value());
}

QString
HostLiveSessionDialog::getDisplayName() const
{
    return _displayName->text().trimmed();
}

void
HostLiveSessionDialog::rememberChoices() const
{
    QSettings settings;
    settings.setValue(settingsKey("bindAddress"), getBindAddress());
    settings.setValue(settingsKey("hostPort"), _port->value());
    settings.setValue(settingsKey("displayName"), getDisplayName());
}

JoinLiveSessionDialog::JoinLiveSessionDialog(QWidget* parent)
    : QDialog(parent)
    , _host(0)
    , _port(0)
    , _displayName(0)
    , _buttons(0)
{
    setObjectName(QString::fromUtf8("joinLiveSessionDialog"));
    setWindowTitle(tr("Join Live Session"));

    QSettings settings;
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    QFormLayout* form = new QFormLayout;
    mainLayout->addLayout(form);

    _host = new QComboBox(this);
    _host->setObjectName(QString::fromUtf8("host"));
    _host->setEditable(true);
    _host->setInsertPolicy(QComboBox::NoInsert);
    _host->setToolTip(tr("The name or IP address of the computer hosting the session. The hosts joined "
                         "recently are in the list."));
    _host->setMinimumWidth(280);
    const QStringList recent = settings.value(settingsKey("recentHosts")).toStringList();
    for (const QString& entry : recent) {
        QString host;
        int port = 0;
        if (splitRecentHost(entry, &host, &port)) {
            _host->addItem(host, port);
        }
    }
    form->addRow(tr("Host"), _host);

    _port = makePortBox(this, (_host->count() > 0) ? _host->itemData(0).toInt() : kDefaultPort);
    _port->setToolTip(tr("The port the host chose."));
    form->addRow(tr("Port"), _port);

    _displayName = makeDisplayNameEdit(this);
    form->addRow(tr("Display Name"), _displayName);

    QLabel* note = new QLabel(tr("Joining replaces this window's project with the host's. Saving saves the "
                                 "host's file; Save As saves your own copy and leaves the session."),
                              this);
    note->setObjectName(QString::fromUtf8("joinNote"));
    note->setWordWrap(true);
    note->setMinimumWidth(420);
    mainLayout->addWidget(note);

    _buttons = new DialogButtonBox(QDialogButtonBox::StandardButtons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel), Qt::Horizontal, this);
    _buttons->button(QDialogButtonBox::Ok)->setText(tr("Join"));
    mainLayout->addWidget(_buttons);
    QObject::connect(_buttons, SIGNAL(accepted()), this, SLOT(accept()));
    QObject::connect(_buttons, SIGNAL(rejected()), this, SLOT(reject()));
    QObject::connect(_host, SIGNAL(activated(int)), this, SLOT(onHostActivated(int)));
    QObject::connect(_host, SIGNAL(editTextChanged(QString)), this, SLOT(refreshButtons()));
    QObject::connect(_displayName, SIGNAL(textChanged(QString)), this, SLOT(refreshButtons()));
    refreshButtons();
}

JoinLiveSessionDialog::~JoinLiveSessionDialog()
{
}

void
JoinLiveSessionDialog::onHostActivated(int index)
{
    if ((index >= 0) && (index < _host->count())) {
        _port->setValue(_host->itemData(index).toInt());
    }
    refreshButtons();
}

void
JoinLiveSessionDialog::refreshButtons()
{
    _buttons->button(QDialogButtonBox::Ok)->setEnabled(!getHost().isEmpty() && !getDisplayName().isEmpty());
}

QString
JoinLiveSessionDialog::getHost() const
{
    return _host->currentText().trimmed();
}

quint16
JoinLiveSessionDialog::getPort() const
{
    return static_cast<quint16>(_port->value());
}

QString
JoinLiveSessionDialog::getDisplayName() const
{
    return _displayName->text().trimmed();
}

void
JoinLiveSessionDialog::rememberChoices() const
{
    QSettings settings;
    const QString entry = getHost() + QLatin1Char(':') + QString::number(_port->value());
    QStringList recent = settings.value(settingsKey("recentHosts")).toStringList();
    recent.removeAll(entry);
    recent.prepend(entry);
    while (recent.size() > kMaxRecentHosts) {
        recent.removeLast();
    }
    settings.setValue(settingsKey("recentHosts"), recent);
    settings.setValue(settingsKey("displayName"), getDisplayName());
}

LiveSessionStatus::LiveSessionStatus(const AppInstancePtr& app,
                                     QWidget* parent)
    : QLabel(parent)
    , _app(app)
{
    setObjectName(QString::fromUtf8("liveSessionStatus"));
    QObject::connect(app.get(), SIGNAL(liveSessionChanged()), this, SLOT(refresh()));
    QObject::connect(app.get(), SIGNAL(liveSessionMessage(QString)), this, SLOT(onSessionMessage(QString)));
    refresh();
}

LiveSessionStatus::~LiveSessionStatus()
{
}

void
LiveSessionStatus::refresh()
{
    AppInstancePtr app = _app.lock();
    if (!app) {
        hide();

        return;
    }
    const QString address = app->getLiveSessionAddress();
    switch (app->getLiveSessionState()) {
    case AppInstance::LiveSessionState::eNone:
        setText(QString());
        hide();

        return;
    case AppInstance::LiveSessionState::eHosting: {
        const int peers = app->getLiveSessionPeerCount();
        setText(tr("Live: hosting on %1, %n user(s) joined", "", peers).arg(address));
        break;
    }
    case AppInstance::LiveSessionState::eConnecting:
        setText(tr("Live: joining %1...").arg(address));
        break;
    case AppInstance::LiveSessionState::eJoined:
        setText(tr("Live: joined %1").arg(address));
        break;
    }
    show();
}

void
LiveSessionStatus::onSessionMessage(const QString& message)
{
    setToolTip(message);
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_LiveSessionDialogs.cpp"
