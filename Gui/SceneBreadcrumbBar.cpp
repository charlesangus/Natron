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

#include "SceneBreadcrumbBar.h"

#include <QtCore/QMimeData>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtGui/QAction>
#include <QtGui/QClipboard>
#include <QtGui/QDrag>
#include <QtGui/QGuiApplication>
#include <QtGui/QHelpEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPalette>
#include <QtWidgets/QApplication>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QToolTip>

#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Gui/SceneBreadcrumbListPopup.h"
#include "Gui/SceneGraphModel.h"
#include "Gui/ScenePrimListPopup.h"
#include "Gui/SceneViewport.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {

// How long a read that an edit kept out waits before it asks again.
const int kRetryReadMs = 50;

const char* const kRootText = "/";

const char* const kChevronText = "\xE2\x80\xBA";

const char*
stateName(SceneBreadcrumbBar::StateEnum state)
{
    switch (state) {
    case SceneBreadcrumbBar::eStatePresent:
        return "present";
    case SceneBreadcrumbBar::eStateMissing:
        return "missing";
    case SceneBreadcrumbBar::eStateUnknown:
        break;
    }

    return "unknown";
}

}

QVariant
SceneBreadcrumbBar::Segment::toVariant() const
{
    QVariantMap map;
    map.insert(QStringLiteral("text"), text);
    map.insert(QStringLiteral("path"), path);
    map.insert(QStringLiteral("state"), QString::fromLatin1(stateName(state)));
    map.insert(QStringLiteral("elided"), elided);

    return map;
}

struct SceneBreadcrumbBar::Private {
    QPointer<SceneViewport> viewport;
    QHBoxLayout* layout = nullptr;
    QToolButton* rootButton = nullptr;
    QToolButton* rootChevron = nullptr;
    QLabel* pathLabel = nullptr;
    QToolButton* elisionButton = nullptr;
    QLabel* elisionSeparator = nullptr;
    QPointer<ScenePrimListPopup> popup;
    QPointer<SceneBreadcrumbListPopup> listPopup;
    QToolButton* moreButton = nullptr;
    QTimer retryTimer;

    QStringList selection;
    QList<Segment> segments;
    // Parallel to segments.
    QList<QToolButton*> buttons;
    // Parallel to segments: the chevron after each segment's button.
    QList<QToolButton*> chevrons;
    Qt::KeyboardModifiers pressModifiers = Qt::NoModifier;
    QPointer<QToolButton> pressButton;
    QPoint pressPos;
    bool rebuilding = false;
};

SceneBreadcrumbBar::SceneBreadcrumbBar(SceneViewport* viewport, QWidget* parent)
    : QWidget(parent)
    , _imp(new Private)
{
    _imp->viewport = viewport;
    setObjectName(QStringLiteral("sceneBreadcrumbBar"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);

    _imp->layout = new QHBoxLayout(this);
    _imp->layout->setContentsMargins(4, 1, 4, 1);
    _imp->layout->setSpacing(0);

    _imp->rootButton = makeButton(QStringLiteral("breadcrumbRoot"), QString::fromLatin1(kRootText));
    _imp->rootButton->setToolTip(tr("Clear the selection"));
    _imp->layout->addWidget(_imp->rootButton);
    _imp->rootChevron = makeChevron(QStringLiteral("breadcrumbRootChevron"), QString::fromLatin1(kRootText));
    _imp->layout->addWidget(_imp->rootChevron);

    _imp->pathLabel = new QLabel(this);
    _imp->pathLabel->setObjectName(QStringLiteral("breadcrumbPath"));
    _imp->layout->addWidget(_imp->pathLabel);

    _imp->elisionSeparator = new QLabel(QString::fromUtf8(kChevronText), this);
    _imp->layout->addWidget(_imp->elisionSeparator);
    _imp->elisionButton = makeButton(QStringLiteral("breadcrumbElision"), QString::fromUtf8("\xE2\x80\xA6"));
    _imp->elisionButton->setPopupMode(QToolButton::InstantPopup);
    _imp->layout->addWidget(_imp->elisionButton);

    _imp->layout->addStretch(1);

    _imp->moreButton = makeButton(QStringLiteral("breadcrumbMore"), QString());
    _imp->moreButton->setToolTip(tr("More prims are selected: click to list them all, drag to carry them all"));
    _imp->layout->addWidget(_imp->moreButton);

    _imp->retryTimer.setSingleShot(true);
    _imp->retryTimer.setInterval(kRetryReadMs);
    QObject::connect(&_imp->retryTimer, &QTimer::timeout, this, &SceneBreadcrumbBar::refresh);
    QObject::connect(_imp->rootChevron, &QToolButton::clicked, this, [this]() {
        openPopup(QString::fromLatin1(kRootText), QString(), _imp->rootChevron);
    });
    QObject::connect(_imp->rootButton, &QToolButton::clicked, this, [this]() {
        _imp->pressModifiers = Qt::NoModifier;
        if (_imp->viewport) {
            _imp->viewport->select(QStringList(), SceneViewport::eSelectionModeReplace);
        }
    });
    QObject::connect(_imp->moreButton, &QToolButton::clicked, this, [this]() {
        _imp->pressModifiers = Qt::NoModifier;
        if (_imp->listPopup && _imp->listPopup->isVisible()) {
            _imp->listPopup->close();

            return;
        }
        if (!_imp->viewport) {
            return;
        }
        SceneBreadcrumbListPopup* popup = new SceneBreadcrumbListPopup(_imp->viewport, _imp->selection, _imp->moreButton, window());
        _imp->listPopup = popup;
        popup->popupUnder(this);
    });
    if (viewport) {
        QObject::connect(viewport, &SceneViewport::selectionChanged, this, &SceneBreadcrumbBar::refresh);
        QObject::connect(viewport, &SceneViewport::sceneSynced, this, &SceneBreadcrumbBar::refresh);
    }
    refresh();
}

SceneBreadcrumbBar::~SceneBreadcrumbBar()
{
    delete _imp;
}

QToolButton*
SceneBreadcrumbBar::makeButton(const QString& name, const QString& text)
{
    QToolButton* button = new QToolButton(this);
    button->setObjectName(name);
    button->setText(text);
    button->setAutoRaise(true);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    button->setFocusPolicy(Qt::NoFocus);
    button->installEventFilter(this);

    return button;
}

QToolButton*
SceneBreadcrumbBar::makeChevron(const QString& name, const QString& path)
{
    QToolButton* chevron = makeButton(name, QString::fromUtf8(kChevronText));
    chevron->setToolTip(tr("List the children"));
    chevron->setProperty("natronBreadcrumbPath", path);

    return chevron;
}

void
SceneBreadcrumbBar::openPopup(const QString& listedPath, const QString& highlightName, QWidget* anchor)
{
    if (!_imp->viewport || !anchor) {
        return;
    }
    if (_imp->popup) {
        _imp->popup->close();
    }
    ScenePrimListPopup* popup = new ScenePrimListPopup(_imp->viewport, listedPath, highlightName, window());
    _imp->popup = popup;
    popup->popupAt(anchor->mapToGlobal(anchor->rect().bottomLeft()));
}

QList<SceneBreadcrumbBar::Segment>
SceneBreadcrumbBar::getSegments() const
{
    return _imp->segments;
}

QString
SceneBreadcrumbBar::getPath() const
{
    return _imp->selection.isEmpty() ? QString() : _imp->selection.back();
}

int
SceneBreadcrumbBar::getMoreCount() const
{
    return qMax(0, int(_imp->selection.size()) - 1);
}

QSize
SceneBreadcrumbBar::minimumSizeHint() const
{
    return QSize(0, sizeHint().height());
}

void
SceneBreadcrumbBar::refresh()
{
    if (_imp->rebuilding) {
        return;
    }
    _imp->retryTimer.stop();
    _imp->selection = _imp->viewport ? _imp->viewport->getSelection() : QStringList();

    QList<Segment> segments;
    QString path;
    const QStringList names = getPath().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString& name : names) {
        path += QLatin1Char('/') + name;
        Segment segment;
        segment.text = name;
        segment.path = path;
        segments.push_back(segment);
    }
    readStates(segments);
    _imp->segments = segments;
    rebuild();
}

void
SceneBreadcrumbBar::readStates(QList<Segment>& segments)
{
    if (segments.isEmpty() || !_imp->viewport || !_imp->viewport->getRenderer().hasScene() || !_imp->viewport->isSceneSynced()) {
        return;
    }
    const bool read = _imp->viewport->getRenderer().tryReadStage([&segments](const SceneStage* stage) {
        const UsdStageRefPtr usdStage = stage ? stage->getStage() : UsdStageRefPtr();
        if (!usdStage) {
            return;
        }
        for (Segment& segment : segments) {
            const std::string path = segment.path.toStdString();
            const bool present = SdfPath::IsValidPathString(path) && bool(usdStage->GetPrimAtPath(SdfPath(path)));
            segment.state = present ? eStatePresent : eStateMissing;
        }
    });
    if (!read) {
        _imp->retryTimer.start();
    }
}

void
SceneBreadcrumbBar::rebuild()
{
    _imp->rebuilding = true;
    // A click on one of these buttons can be what got here, so they go once its signal has ended.
    for (QToolButton* button : _imp->buttons) {
        _imp->layout->removeWidget(button);
        button->hide();
        button->deleteLater();
    }
    for (QToolButton* chevron : _imp->chevrons) {
        _imp->layout->removeWidget(chevron);
        chevron->hide();
        chevron->deleteLater();
    }
    _imp->buttons.clear();
    _imp->chevrons.clear();

    // The buttons go between the root's chevron and the elision button.
    const bool known = !_imp->segments.isEmpty() && (_imp->segments.front().state != eStateUnknown);
    int at = _imp->layout->indexOf(_imp->rootChevron) + 1;
    if (known) {
        for (const Segment& segment : _imp->segments) {
            QToolButton* button = makeButton(QStringLiteral("breadcrumbSegment"), segment.text);
            button->setProperty("natronBreadcrumbPath", segment.path);
            button->setProperty("natronBreadcrumbState", QString::fromLatin1(stateName(segment.state)));
            if (segment.state == eStateMissing) {
                // Disabled buttons take no mouse events, and a missing segment still has its menu and drag.
                QPalette greyed = button->palette();
                greyed.setColor(QPalette::ButtonText, greyed.color(QPalette::Disabled, QPalette::ButtonText));
                greyed.setColor(QPalette::WindowText, greyed.color(QPalette::Disabled, QPalette::WindowText));
                button->setPalette(greyed);
            }
            QToolButton* chevron = makeChevron(QStringLiteral("breadcrumbChevron"), segment.path);
            chevron->setEnabled(segment.state == eStatePresent);
            _imp->layout->insertWidget(at++, button);
            _imp->layout->insertWidget(at++, chevron);
            const Segment copy = segment;
            QObject::connect(button, &QToolButton::clicked, this, [this, copy]() { clickSegment(copy); });
            QObject::connect(chevron, &QToolButton::clicked, this, [this, copy, chevron]() { openPopup(copy.path, QString(), chevron); });
            _imp->buttons.push_back(button);
            _imp->chevrons.push_back(chevron);
        }
    }

    _imp->pathLabel->setVisible(!known && !_imp->segments.isEmpty());
    _imp->pathLabel->setText(getPath().mid(1));
    const int more = getMoreCount();
    _imp->moreButton->setVisible(more > 0);
    _imp->moreButton->setText(QStringLiteral("+%1").arg(more));
    _imp->rebuilding = false;

    layoutButtons();
}

void
SceneBreadcrumbBar::layoutButtons()
{
    const int count = _imp->buttons.size();
    for (Segment& segment : _imp->segments) {
        segment.elided = false;
    }
    for (int i = 0; i < count; ++i) {
        _imp->buttons[i]->setVisible(true);
        _imp->chevrons[i]->setVisible(true);
    }
    _imp->elisionButton->setVisible(false);
    _imp->elisionSeparator->setVisible(false);

    const int margins = _imp->layout->contentsMargins().left() + _imp->layout->contentsMargins().right();
    int needed = margins + _imp->rootButton->sizeHint().width() + _imp->rootChevron->sizeHint().width();
    for (int i = 0; i < count; ++i) {
        needed += _imp->buttons[i]->sizeHint().width();
        needed += _imp->chevrons[i]->sizeHint().width();
    }
    if (_imp->moreButton->isVisibleTo(this)) {
        needed += _imp->moreButton->sizeHint().width();
    }

    // The first and last segments stay; the ones between fold from the first one on until the rest
    // and the "..." button fit.
    if (count > 2 && needed > width()) {
        needed += _imp->elisionButton->sizeHint().width() + _imp->elisionSeparator->sizeHint().width();
        _imp->elisionButton->setVisible(true);
        _imp->elisionSeparator->setVisible(true);
        // The elision button comes right after the first segment's chevron.
        _imp->layout->removeWidget(_imp->elisionSeparator);
        _imp->layout->removeWidget(_imp->elisionButton);
        const int after = _imp->layout->indexOf(_imp->chevrons.front()) + 1;
        _imp->layout->insertWidget(after, _imp->elisionButton);
        _imp->layout->insertWidget(after + 1, _imp->elisionSeparator);
        for (int i = 1; i < count - 1 && needed > width(); ++i) {
            needed -= _imp->buttons[i]->sizeHint().width() + _imp->chevrons[i]->sizeHint().width();
            _imp->buttons[i]->setVisible(false);
            _imp->chevrons[i]->setVisible(false);
            _imp->segments[i].elided = true;
        }
        // The menu may be the one whose action started this refresh: it is replaced, not cleared.
        if (QMenu* old = _imp->elisionButton->menu()) {
            old->deleteLater();
        }
        QMenu* menu = new QMenu(_imp->elisionButton);
        _imp->elisionButton->setMenu(menu);
        for (const Segment& segment : _imp->segments) {
            if (!segment.elided) {
                continue;
            }
            QAction* action = menu->addAction(segment.text);
            action->setEnabled(segment.state == eStatePresent);
            const Segment copy = segment;
            QObject::connect(action, &QAction::triggered, this, [this, copy]() { clickSegment(copy); });
        }
    }

    QVariantList published;
    for (const Segment& segment : _imp->segments) {
        published.push_back(segment.toVariant());
    }
    setProperty("natronBreadcrumbSegments", published);
    setProperty("natronBreadcrumbPath", getPath());
    setProperty("natronBreadcrumbMore", getMoreCount());
    setProperty("natronBreadcrumbElided", _imp->elisionButton->isVisibleTo(this));
}

void
SceneBreadcrumbBar::clickSegment(const Segment& segment)
{
    const Qt::KeyboardModifiers modifiers = _imp->pressModifiers;
    _imp->pressModifiers = Qt::NoModifier;
    if (!_imp->viewport || (segment.state != eStatePresent)) {
        return;
    }
    const SceneViewport::SelectionModeEnum mode = (modifiers & Qt::ShiftModifier) ? SceneViewport::eSelectionModeAdd : SceneViewport::eSelectionModeReplace;
    _imp->viewport->select(QStringList(segment.path), mode);
}

void
SceneBreadcrumbBar::frameSegment(Segment segment)
{
    _imp->pressModifiers = Qt::NoModifier;
    if (!_imp->viewport || (segment.state == eStateMissing)) {
        return;
    }
    _imp->viewport->select(QStringList(segment.path), SceneViewport::eSelectionModeReplace);
    _imp->viewport->frameSelection();
}

const SceneBreadcrumbBar::Segment*
SceneBreadcrumbBar::segmentAt(const QString& path) const
{
    for (const Segment& segment : _imp->segments) {
        if (segment.path == path) {
            return &segment;
        }
    }

    return nullptr;
}

QString
SceneBreadcrumbBar::segmentToolTip(const Segment& segment) const
{
    StateEnum state = segment.state;
    QString type;
    QString kind;
    QString loaded;
    if (_imp->viewport && _imp->viewport->getRenderer().hasScene() && _imp->viewport->isSceneSynced()) {
        // A read an edit keeps out leaves the state the last refresh found.
        _imp->viewport->getRenderer().tryReadStage([&](const SceneStage* stage) {
            const UsdStageRefPtr usdStage = stage ? stage->getStage() : UsdStageRefPtr();
            const std::string path = segment.path.toStdString();
            if (!usdStage || !SdfPath::IsValidPathString(path)) {
                return;
            }
            const UsdPrim prim = usdStage->GetPrimAtPath(SdfPath(path));
            if (!prim) {
                state = eStateMissing;

                return;
            }
            state = eStatePresent;
            type = QString::fromStdString(prim.GetTypeName().GetString());
            TfToken kindToken;
            if (prim.GetKind(&kindToken)) {
                kind = QString::fromStdString(kindToken.GetString());
            }
            loaded = prim.IsLoaded() ? tr("yes") : tr("no");
        });
    }
    if (state == eStateMissing) {
        return tr("Not in this scene");
    }
    if (state != eStatePresent) {
        return segment.path;
    }

    return tr("%1\nType: %2\nKind: %3\nLoaded: %4")
        .arg(segment.path, type.isEmpty() ? tr("none") : type, kind.isEmpty() ? tr("none") : kind, loaded.isEmpty() ? tr("unknown") : loaded);
}

QStringList
SceneBreadcrumbBar::dragPaths(const QString& segmentPath) const
{
    if (!segmentPath.isEmpty()) {
        return QStringList(segmentPath);
    }

    return _imp->selection;
}

void
SceneBreadcrumbBar::exposeMimeData(const QString& segmentPath)
{
    for (QMimeData* old : findChildren<QMimeData*>(QStringLiteral("breadcrumbMime"), Qt::FindDirectChildrenOnly)) {
        delete old;
    }
    QMimeData* mime = SceneGraphModel::mimeDataOfPaths(dragPaths(segmentPath));
    if (mime) {
        mime->setObjectName(QStringLiteral("breadcrumbMime"));
        mime->setParent(this);
    }
}

void
SceneBreadcrumbBar::startDrag(QToolButton* button)
{
    QMimeData* mime = SceneGraphModel::mimeDataOfPaths(dragPaths(button->property("natronBreadcrumbPath").toString()));
    _imp->pressButton = nullptr;
    _imp->pressModifiers = Qt::NoModifier;
    if (!mime) {
        return;
    }
    QPointer<QToolButton> guard(button);
    QDrag* drag = new QDrag(button);
    drag->setMimeData(mime);
    drag->setPixmap(button->grab());
    drag->exec(Qt::CopyAction);
    // The release went to the drag, so the button never saw it.
    if (guard) {
        guard->setDown(false);
    }
}

void
SceneBreadcrumbBar::openSegmentMenu(Segment segment, QWidget* anchor)
{
    for (QMenu* old : findChildren<QMenu*>(QStringLiteral("breadcrumbSegmentMenu"), Qt::FindDirectChildrenOnly)) {
        old->close();
    }
    QMenu* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("breadcrumbSegmentMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);

    const QString path = segment.path;
    const bool present = (segment.state != eStateMissing);
    QPointer<QWidget> anchorGuard(anchor);

    QAction* siblings = menu->addAction(tr("Siblings"));
    siblings->setObjectName(QStringLiteral("breadcrumbSiblings"));
    siblings->setEnabled(present);
    QObject::connect(siblings, &QAction::triggered, this, [this, path, anchorGuard]() {
        // The menu is still closing: a popup opened now would be closed with it.
        QTimer::singleShot(0, this, [this, path, anchorGuard]() {
            if (!anchorGuard) {
                return;
            }
            const int slash = path.lastIndexOf(QLatin1Char('/'));
            openPopup(slash > 0 ? path.left(slash) : QString::fromLatin1(kRootText), path.mid(slash + 1), anchorGuard);
        });
    });

    menu->addSeparator();

    QAction* copyPath = menu->addAction(tr("Copy Path"));
    copyPath->setObjectName(QStringLiteral("breadcrumbCopyPath"));
    QObject::connect(copyPath, &QAction::triggered, this, [path]() { QGuiApplication::clipboard()->setText(path); });

    QAction* copySelected = menu->addAction(tr("Copy Selected Paths"));
    copySelected->setObjectName(QStringLiteral("breadcrumbCopySelectedPaths"));
    copySelected->setEnabled(!_imp->selection.isEmpty());
    QObject::connect(copySelected, &QAction::triggered, this, [this]() {
        const QStringList selection = _imp->viewport ? _imp->viewport->getSelection() : _imp->selection;
        QGuiApplication::clipboard()->setText(selection.join(QLatin1Char('\n')));
    });

    menu->addSeparator();

    QAction* frame = menu->addAction(tr("Frame"));
    frame->setObjectName(QStringLiteral("breadcrumbFrame"));
    frame->setEnabled(present && _imp->viewport);
    const Segment copy = segment;
    QObject::connect(frame, &QAction::triggered, this, [this, copy]() { frameSegment(copy); });

    QAction* showInGraph = menu->addAction(tr("Show In Scene Graph"));
    showInGraph->setObjectName(QStringLiteral("breadcrumbShowInSceneGraph"));
    showInGraph->setEnabled(present);
    QObject::connect(showInGraph, &QAction::triggered, this, [this, path]() { Q_EMIT showInSceneGraphRequested(QStringList(path)); });

    menu->popup(anchor->mapToGlobal(anchor->rect().bottomLeft()));
}

void
SceneBreadcrumbBar::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    layoutButtons();
}

void
SceneBreadcrumbBar::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    refresh();
}

bool
SceneBreadcrumbBar::eventFilter(QObject* watched, QEvent* e)
{
    QToolButton* button = qobject_cast<QToolButton*>(watched);
    const bool isSegment = button && (button->objectName() == QLatin1String("breadcrumbSegment"));
    const Segment* segment = isSegment ? segmentAt(button->property("natronBreadcrumbPath").toString()) : nullptr;

    switch (e->type()) {
    case QEvent::MouseButtonPress: {
        const QMouseEvent* press = static_cast<QMouseEvent*>(e);
        // A button's clicked() carries no modifiers.
        _imp->pressModifiers = press->modifiers();
        if (segment && (press->button() == Qt::RightButton)) {
            openSegmentMenu(*segment, button);

            return true;
        }
        if (press->button() == Qt::LeftButton) {
            const bool draggable = segment || (button && (button == _imp->moreButton));
            _imp->pressButton = draggable ? button : nullptr;
            _imp->pressPos = press->position().toPoint();
        }
        break;
    }
    case QEvent::MouseMove: {
        const QMouseEvent* move = static_cast<QMouseEvent*>(e);
        if (button && (button == _imp->pressButton) && (move->buttons() & Qt::LeftButton)
            && ((move->position().toPoint() - _imp->pressPos).manhattanLength() >= QApplication::startDragDistance())) {
            startDrag(button);

            return true;
        }
        break;
    }
    case QEvent::MouseButtonRelease:
        _imp->pressButton = nullptr;
        break;
    case QEvent::MouseButtonDblClick:
        if (segment && (static_cast<QMouseEvent*>(e)->button() == Qt::LeftButton)) {
            frameSegment(*segment);

            return true;
        }
        break;
    case QEvent::ToolTip:
        if (segment) {
            QToolTip::showText(static_cast<QHelpEvent*>(e)->globalPos(), segmentToolTip(*segment), button);

            return true;
        }
        break;
    default:
        break;
    }

    return QWidget::eventFilter(watched, e);
}

NATRON_NAMESPACE_EXIT
