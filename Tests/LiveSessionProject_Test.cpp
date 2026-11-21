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

#include <Python.h>

#include "Global/Macros.h"

#include <cstddef>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <string>

#include <poll.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/LiveSession/SessionClient.h"
#include "Engine/LiveSession/SessionHost.h"
#include "Engine/LiveSession/SessionShared.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

namespace {

// Background apps all have the Python name app, so the client is a child Tests process with its
// own app, driven over its standard input; replies are its "@ " lines.
const char* const kPortEnv = "NATRON_TEST_LIVE_SESSION_PROJECT_PORT";
const char* const kPeerSuite = "LiveSessionProjectPeerProcess";
const int kTimeoutMs = 120000;

NodePtr
createIn(const AppInstancePtr& app,
         const char* pluginID)
{
    CreateNodeArgs args(pluginID, app->getProject());
    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    args.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);

    return app->createNode(args);
}

KnobDoubleBasePtr
doubleKnob(const NodePtr& node,
           const char* name)
{
    return std::dynamic_pointer_cast<KnobDoubleBase>(node->getKnobByName(name));
}

bool
runUntil(const std::function<bool()>& done,
         int timeoutMs = kTimeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }

    return true;
}

// The last name the peer's project gave its window.
QString&
peerWindowTitle()
{
    static QString title;

    return title;
}

void
replyToParent(const std::string& line)
{
    // A leading newline keeps the reply off the end of a line something else printed.
    std::cout << "\n@ " << line << std::endl;
}

std::string
describe(const AppInstancePtr& app)
{
    const ProjectPtr project = app->getProject();
    std::ostringstream out;
    out << "state " << int(app->getLiveSessionState()) << ' ' << int(project->getLiveSessionRole()) << ' '
        << int(project->hasProjectBeenSavedByUser()) << ' ' << int(project->getProjectPath().isEmpty());

    return out.str();
}

std::string
peerCommand(const AppInstancePtr& app,
            const std::string& line,
            bool* quit)
{
    std::istringstream in(line);
    std::string verb;
    in >> verb;
    if (verb == "state") {
        if (SessionClient* client = app->getLiveSessionClient()) {
            client->flushLocalEdits();
        }

        return describe(app);
    }
    if (verb == "save") {
        return app->save(std::string()) ? "saved" : "failed";
    }
    if (verb == "title") {
        return "title " + peerWindowTitle().toStdString();
    }
    if (verb == "ended") {
        const bool ended = runUntil([&app]() { return app->getLiveSessionState() == AppInstance::LiveSessionState::eNone; });

        return ended ? describe(app) : "timeout";
    }
    if (verb == "create") {
        NodePtr node = createIn(app, PLUGINID_NATRON_BLUR);
        if (!node) {
            return "failed";
        }
        KnobDoubleBasePtr size = doubleKnob(node, kBlurParamSize);
        if (!size) {
            return "failed";
        }
        size->setValue(4., ViewSpec::all(), 0);

        return (size->getValue(0) == 4.) ? "ok" : "failed";
    }
    if (verb == "quit") {
        *quit = true;

        return "bye";
    }

    std::string name;
    in >> name;
    NodePtr node = app->getProject()->getNodeByName(name);
    if (!node) {
        return "missing";
    }
    if (verb == "set") {
        std::string knobName;
        double value = 0.;
        in >> knobName >> value;
        KnobDoubleBasePtr knob = doubleKnob(node, knobName.c_str());
        if (!knob) {
            return "missing";
        }
        knob->setValue(value, ViewSpec::all(), 0);
        if (SessionClient* client = app->getLiveSessionClient()) {
            client->flushLocalEdits();
        }

        return "ok";
    }

    return "unknown";
}

class Peer {
public:
    explicit Peer(quint16 port)
    {
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QString::fromUtf8(kPortEnv), QString::number(port));
        _process.setProcessEnvironment(environment);
        _process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        _process.start(QCoreApplication::applicationFilePath(),
                       QStringList() << QString::fromUtf8("--gtest_filter=%1.*").arg(QString::fromUtf8(kPeerSuite)));
        _started = _process.waitForStarted(30000);
    }

    ~Peer()
    {
        if (_process.state() != QProcess::NotRunning) {
            _process.kill();
            _process.waitForFinished(30000);
        }
    }

    bool started() const
    {
        return _started;
    }

    // The next reply, or "dead" or "timeout". This process's events run meanwhile, so the host
    // keeps serving the session.
    std::string reply()
    {
        QElapsedTimer timer;
        timer.start();
        for (;;) {
            while (_process.canReadLine()) {
                const std::string line = QString::fromUtf8(_process.readLine()).trimmed().toStdString();
                if (line.compare(0, 2, "@ ") == 0) {
                    return line.substr(2);
                }
                if (!line.empty()) {
                    _output += line + "\n";
                    if (_output.size() > 16384) {
                        _output.erase(0, _output.size() - 16384);
                    }
                }
            }
            if (_process.state() == QProcess::NotRunning) {
                return "dead";
            }
            if (timer.elapsed() > kTimeoutMs) {
                return "timeout";
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        }
    }

    std::string command(const std::string& line)
    {
        const std::string withEnd = line + "\n";
        _process.write(withEnd.data(), static_cast<qint64>(withEnd.size()));
        _process.waitForBytesWritten(30000);

        return reply();
    }

    // True when the child ended cleanly, its own expectations included.
    bool finish()
    {
        if (_process.state() != QProcess::NotRunning) {
            (void)command("quit");
            _process.closeWriteChannel();
        }
        if (!_process.waitForFinished(60000)) {
            _process.kill();
            _process.waitForFinished(30000);

            return false;
        }

        return (_process.exitStatus() == QProcess::NormalExit) && (_process.exitCode() == 0);
    }

    const std::string& output() const
    {
        return _output;
    }

private:
    QProcess _process;
    bool _started = false;
    std::string _output;
};

QByteArray
readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }

    return file.readAll();
}

std::string
expectedState(AppInstance::LiveSessionState state,
              Project::LiveSessionRole role,
              bool savedByUser,
              bool pathEmpty)
{
    std::ostringstream out;
    out << "state " << int(state) << ' ' << int(role) << ' ' << int(savedByUser) << ' ' << int(pathEmpty);

    return out.str();
}

} // namespace

// Runs only in a child started by the tests below, as the client of their host.
class LiveSessionProjectPeerProcess
    : public BaseTest {
};

TEST_F(LiveSessionProjectPeerProcess, ServesTheTestThatStartedIt)
{
    const char* port = std::getenv(kPortEnv);
    if (!port) {
        return;
    }
    AppInstancePtr app = getApp();
    ASSERT_NE(app, nullptr);

    QString endReason;
    QObject::connect(app.get(), &AppInstance::liveSessionEnded, [&endReason](const QString& reason) { endReason = reason; });
    QObject::connect(app->getProject().get(), &Project::projectNameChanged, [](const QString& filePath, bool) { peerWindowTitle() = filePath; });
    app->joinLiveSession(QString::fromUtf8("127.0.0.1"), static_cast<quint16>(std::atoi(port)), QString::fromUtf8("client"));
    const bool joined = runUntil([&app]() { return app->getLiveSessionState() != AppInstance::LiveSessionState::eConnecting; })
        && (app->getLiveSessionState() == AppInstance::LiveSessionState::eJoined);
    if (!joined) {
        replyToParent("left " + endReason.toStdString());
        FAIL() << "could not join: " << endReason.toStdString();
    }
    replyToParent("joined");

    bool quit = false;
    std::string input;
    while (!quit) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        pollfd stdinPoll;
        stdinPoll.fd = STDIN_FILENO;
        stdinPoll.events = POLLIN;
        stdinPoll.revents = 0;
        if ((poll(&stdinPoll, 1, 1) > 0) && (stdinPoll.revents & (POLLIN | POLLHUP))) {
            char buffer[4096];
            const ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (count <= 0) {
                break;
            }
            input.append(buffer, static_cast<std::size_t>(count));
        }
        std::string::size_type end;
        while (!quit && ((end = input.find('\n')) != std::string::npos)) {
            const std::string line = input.substr(0, end);
            input.erase(0, end + 1);
            replyToParent(peerCommand(app, line, &quit));
        }
    }
    app->leaveLiveSession();
}

class LiveSessionProjectTest
    : public BaseTest {
protected:
    void SetUp() override
    {
        BaseTest::SetUp();
        _app = getApp();
        ASSERT_NE(_app, nullptr);
        ASSERT_TRUE(_dir.isValid());
    }

    void TearDown() override
    {
        if (_peer) {
            EXPECT_TRUE(_peer->finish()) << "the client process failed: " << _peer->output();
            _peer.reset();
        }
        if (_app) {
            _app->leaveLiveSession();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            _app->getProject()->reset(false, true);
        }
        BaseTest::TearDown();
    }

    void buildComp()
    {
        NodePtr constant = createIn(_app, PLUGINID_NATRON_CONSTANT);
        NodePtr grade = createIn(_app, PLUGINID_NATRON_GRADE);
        NodePtr blur = createIn(_app, PLUGINID_NATRON_BLUR);
        ASSERT_TRUE(constant && grade && blur);
        grade->setScriptName("SharedGrade");
        ASSERT_TRUE(NodeCollection::connectNodes(0, constant, grade, false));
        ASSERT_TRUE(NodeCollection::connectNodes(0, grade, blur, false));
        doubleKnob(grade, kGradeParamMultiply)->setValue(1.25, ViewSpec::all(), 0);
        doubleKnob(blur, kBlurParamSize)->setValueAtTime(10., 3., ViewSpec::all(), 0);
        grade->setPosition(40., 120.);
    }

    QString saveComp()
    {
        const QString dirPath = _dir.path() + QLatin1Char('/');
        const QString fileName = QString::fromUtf8("live-session.ntpz");
        QString savedFilePath;
        EXPECT_TRUE(_app->getProject()->saveProject(dirPath, fileName, &savedFilePath));

        return dirPath + fileName;
    }

    // Hosts the app's project on loopback and joins it from a child process.
    void startSession()
    {
        QString error;
        ASSERT_TRUE(_app->hostLiveSession(QString::fromUtf8("127.0.0.1"), 0, QString::fromUtf8("host"), &error)) << error.toStdString();
        ASSERT_EQ(_app->getLiveSessionState(), AppInstance::LiveSessionState::eHosting);
        SessionHost* host = _app->getLiveSessionHost();
        ASSERT_NE(host, nullptr);
        _peer = std::make_unique<Peer>(host->serverPort());
        ASSERT_TRUE(_peer->started());
        ASSERT_EQ(_peer->reply(), "joined") << _peer->output();
        ASSERT_TRUE(runUntil([this]() { return _app->getLiveSessionPeerCount() == 1; }));
    }

    AppInstancePtr _app;
    std::unique_ptr<Peer> _peer;
    QTemporaryDir _dir;
};

TEST_F(LiveSessionProjectTest, LoadingFromBytesEqualsLoadingTheFile)
{
    buildComp();
    const QString filePath = saveComp();
    const QByteArray bytes = readFile(filePath);
    ASSERT_FALSE(bytes.isEmpty());

    ASSERT_NE(_app->loadProject(filePath.toStdString()), nullptr);
    const std::string fromFile = LiveSession::canonicalProjectYaml(_app->getProject());
    ASSERT_FALSE(fromFile.empty());

    QString error;
    const std::span<const std::byte> view(reinterpret_cast<const std::byte*>(bytes.constData()), static_cast<std::size_t>(bytes.size()));
    ASSERT_TRUE(_app->getProject()->loadProjectFromBundleBytes(view, QString(), QString::fromUtf8("live-session.ntpz"), &error)) << error.toStdString();
    EXPECT_EQ(LiveSession::canonicalProjectYaml(_app->getProject()), fromFile);
    // Without a path, the copy has no file of its own to save to.
    EXPECT_FALSE(_app->getProject()->hasProjectBeenSavedByUser());
    EXPECT_TRUE(_app->getProject()->getProjectPath().isEmpty());
}

TEST_F(LiveSessionProjectTest, AClientSaveSavesTheHostFile)
{
    buildComp();
    const QString filePath = saveComp();
    const QByteArray before = readFile(filePath);
    ASSERT_FALSE(before.isEmpty());
    startSession();
    if (HasFatalFailure()) {
        return;
    }
    ASSERT_EQ(_peer->command("state"),
              expectedState(AppInstance::LiveSessionState::eJoined, Project::LiveSessionRole::eClient, false, true));

    bool saveRequested = false;
    QObject::connect(_app->getLiveSessionHost(), &SessionHost::saveRequested, [&saveRequested](quint32) { saveRequested = true; });
    ASSERT_EQ(_peer->command("set SharedGrade " + std::string(kGradeParamMultiply) + " 2.5"), "ok");
    NodePtr grade = _app->getProject()->getNodeByName("SharedGrade");
    ASSERT_NE(grade, nullptr);
    ASSERT_TRUE(runUntil([&grade]() { return doubleKnob(grade, kGradeParamMultiply)->getValue(0) == 2.5; }));
    EXPECT_FALSE(_app->getProject()->isSaveUpToDate());

    ASSERT_EQ(_peer->command("save"), "saved");
    ASSERT_TRUE(runUntil([&saveRequested]() { return saveRequested; })) << "the host got no save request";
    EXPECT_TRUE(runUntil([this]() { return _app->getProject()->isSaveUpToDate(); }));
    EXPECT_NE(readFile(filePath), before);
    // The client still has no file of its own, and is still in the session.
    EXPECT_EQ(_peer->command("state"),
              expectedState(AppInstance::LiveSessionState::eJoined, Project::LiveSessionRole::eClient, false, true));

    // The saved file is the edited project.
    const std::string edited = LiveSession::canonicalProjectYaml(_app->getProject());
    _app->leaveLiveSession();
    EXPECT_TRUE(_peer->command("ended").compare(0, 5, "state") == 0);
    ASSERT_NE(_app->loadProject(filePath.toStdString()), nullptr);
    EXPECT_EQ(LiveSession::canonicalProjectYaml(_app->getProject()), edited);
}

TEST_F(LiveSessionProjectTest, AfterTheHostStopsTheClientKeepsAnUnsavedEditableCopy)
{
    buildComp();
    (void)saveComp();
    startSession();
    if (HasFatalFailure()) {
        return;
    }
    EXPECT_EQ(_peer->command("title"), "title Live: live-session.ntpz @ 127.0.0.1");

    _app->leaveLiveSession();
    EXPECT_EQ(_app->getLiveSessionState(), AppInstance::LiveSessionState::eNone);
    EXPECT_EQ(_app->getProject()->getLiveSessionRole(), Project::LiveSessionRole::eNone);
    // The host keeps its own file.
    EXPECT_TRUE(_app->getProject()->hasProjectBeenSavedByUser());

    EXPECT_EQ(_peer->command("ended"),
              expectedState(AppInstance::LiveSessionState::eNone, Project::LiveSessionRole::eNone, false, true));
    // The copy is no longer the host's file, so the window does not show that file's name.
    EXPECT_EQ(_peer->command("title"), std::string("title ") + NATRON_PROJECT_UNTITLED);
    EXPECT_EQ(_peer->command("create"), "ok");
    EXPECT_EQ(_peer->command("set SharedGrade " + std::string(kGradeParamMultiply) + " 3"), "ok");
    EXPECT_EQ(_peer->command("state"),
              expectedState(AppInstance::LiveSessionState::eNone, Project::LiveSessionRole::eNone, false, true));
    // The detached copy's edits stay in it.
    EXPECT_EQ(doubleKnob(_app->getProject()->getNodeByName("SharedGrade"), kGradeParamMultiply)->getValue(0), 1.25);
}
