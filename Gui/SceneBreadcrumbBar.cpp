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

#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtGui/QAction>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QToolButton>

#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Gui/SceneViewport.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {

// How long a read that an edit kept out waits before it asks again.
const int kRetryReadMs = 50;

const char* const kRootText = "/";

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
    QLabel* pathLabel = nullptr;
    QToolButton* elisionButton = nullptr;
    QLabel* elisionSeparator = nullptr;
    QToolButton* moreButton = nullptr;
    QTimer retryTimer;

    QStringList selection;
    QList<Segment> segments;
    // Parallel to segments.
    QList<QToolButton*> buttons;
    QList<QLabel*> separators;
    Qt::KeyboardModifiers pressModifiers = Qt::NoModifier;
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

    _imp->pathLabel = new QLabel(this);
    _imp->pathLabel->setObjectName(QStringLiteral("breadcrumbPath"));
    _imp->layout->addWidget(_imp->pathLabel);

    _imp->elisionSeparator = new QLabel(QString::fromLatin1(kRootText), this);
    _imp->layout->addWidget(_imp->elisionSeparator);
    _imp->elisionButton = makeButton(QStringLiteral("breadcrumbElision"), QString::fromUtf8("\xE2\x80\xA6"));
    _imp->elisionButton->setPopupMode(QToolButton::InstantPopup);
    _imp->layout->addWidget(_imp->elisionButton);

    _imp->layout->addStretch(1);

    _imp->moreButton = makeButton(QStringLiteral("breadcrumbMore"), QString());
    _imp->moreButton->setToolTip(tr("More prims are selected"));
    _imp->layout->addWidget(_imp->moreButton);

    _imp->retryTimer.setSingleShot(true);
    _imp->retryTimer.setInterval(kRetryReadMs);
    QObject::connect(&_imp->retryTimer, &QTimer::timeout, this, &SceneBreadcrumbBar::refresh);
    QObject::connect(_imp->rootButton, &QToolButton::clicked, this, [this]() {
        _imp->pressModifiers = Qt::NoModifier;
        if (_imp->viewport) {
            _imp->viewport->select(QStringList(), SceneViewport::eSelectionModeReplace);
        }
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
    for (QLabel* separator : _imp->separators) {
        if (separator) {
            _imp->layout->removeWidget(separator);
            separator->hide();
            separator->deleteLater();
        }
    }
    _imp->buttons.clear();
    _imp->separators.clear();

    // The buttons go between the root button and the elision button.
    const bool known = !_imp->segments.isEmpty() && (_imp->segments.front().state != eStateUnknown);
    int at = _imp->layout->indexOf(_imp->rootButton) + 1;
    if (known) {
        for (const Segment& segment : _imp->segments) {
            QLabel* separator = new QLabel(QString::fromLatin1(kRootText), this);
            separator->setEnabled(segment.state == eStatePresent);
            QToolButton* button = makeButton(QStringLiteral("breadcrumbSegment"), segment.text);
            button->setEnabled(segment.state == eStatePresent);
            button->setProperty("natronBreadcrumbPath", segment.path);
            // The root button's own slash leads the first segment.
            if (_imp->separators.isEmpty()) {
                delete separator;
                separator = nullptr;
            } else {
                _imp->layout->insertWidget(at++, separator);
            }
            _imp->layout->insertWidget(at++, button);
            const Segment copy = segment;
            QObject::connect(button, &QToolButton::clicked, this, [this, copy]() { clickSegment(copy); });
            _imp->buttons.push_back(button);
            _imp->separators.push_back(separator);
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
        if (_imp->separators[i]) {
            _imp->separators[i]->setVisible(true);
        }
    }
    _imp->elisionButton->setVisible(false);
    _imp->elisionSeparator->setVisible(false);

    const int margins = _imp->layout->contentsMargins().left() + _imp->layout->contentsMargins().right();
    int needed = margins + _imp->rootButton->sizeHint().width();
    for (int i = 0; i < count; ++i) {
        needed += _imp->buttons[i]->sizeHint().width();
        if (_imp->separators[i]) {
            needed += _imp->separators[i]->sizeHint().width();
        }
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
        // The elision button comes right after the first segment.
        _imp->layout->removeWidget(_imp->elisionSeparator);
        _imp->layout->removeWidget(_imp->elisionButton);
        const int after = _imp->layout->indexOf(_imp->buttons.front()) + 1;
        _imp->layout->insertWidget(after, _imp->elisionSeparator);
        _imp->layout->insertWidget(after + 1, _imp->elisionButton);
        for (int i = 1; i < count - 1 && needed > width(); ++i) {
            needed -= _imp->buttons[i]->sizeHint().width() + _imp->separators[i]->sizeHint().width();
            _imp->buttons[i]->setVisible(false);
            _imp->separators[i]->setVisible(false);
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
    // A button's clicked() carries no modifiers.
    if (e->type() == QEvent::MouseButtonPress) {
        _imp->pressModifiers = static_cast<QMouseEvent*>(e)->modifiers();
    }

    return QWidget::eventFilter(watched, e);
}

NATRON_NAMESPACE_EXIT
