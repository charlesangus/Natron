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

#include "SceneBreadcrumbListPopup.h"

#include <utility>

#include <QtCore/QHash>
#include <QtCore/QMimeData>
#include <QtCore/QStringView>
#include <QtGui/QCursor>
#include <QtGui/QDrag>
#include <QtGui/QFontMetrics>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QScreen>
#include <QtWidgets/QApplication>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListView>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStyledItemDelegate>
#include <QtWidgets/QVBoxLayout>

#include "Gui/SceneGraphModel.h"
#include "Gui/SceneViewport.h"

NATRON_NAMESPACE_ENTER

namespace {

// The rows the test hook lists.
const int kPublishedRows = 200;

// Space left of a row's path and above and below it.
const int kRowPadding = 4;
const int kRowVerticalPadding = 2;

const int kMaxLevel = 99;

// The ancestor of path at that depth, or path itself when it is not that deep. Views into path, so
// grouping a large selection makes no string per path.
QStringView
ancestorAt(const QString& path,
           int level)
{
    const QStringView view(path);
    qsizetype end = 0;
    for (int depth = 0; depth < level; ++depth) {
        const qsizetype next = view.indexOf(QLatin1Char('/'), end + 1);
        if (next < 0) {
            return view;
        }
        end = next;
    }

    return view.left(end);
}

struct NameSpan {
    // Where the name is in the path.
    int start = 0;
    int length = 0;
    // Where it is drawn.
    int left = 0;
    int width = 0;
};

struct RowLayout {
    QList<NameSpan> names;
    QList<int> slashes;
    int right = 0;
};

// The one layout both the painting and the hit test use, so a click lands on the name drawn there.
RowLayout
layoutRow(const QString& path,
          const QFontMetrics& metrics,
          int left)
{
    RowLayout layout;
    const int slashWidth = metrics.horizontalAdvance(QLatin1Char('/'));
    const int size = int(path.size());
    int x = left;
    int at = 0;
    while (at < size) {
        if (path.at(at) == QLatin1Char('/')) {
            layout.slashes.push_back(x);
            x += slashWidth;
            ++at;
            continue;
        }
        int end = int(path.indexOf(QLatin1Char('/'), at));
        if (end < 0) {
            end = size;
        }
        NameSpan span;
        span.start = at;
        span.length = end - at;
        span.left = x;
        span.width = metrics.horizontalAdvance(path.mid(at, end - at));
        layout.names.push_back(span);
        x += span.width;
        at = end;
    }
    layout.right = x;

    return layout;
}

int
nameAt(const RowLayout& layout,
       int x)
{
    for (int i = 0; i < layout.names.size(); ++i) {
        const NameSpan& span = layout.names[i];
        if ((x >= span.left) && (x < span.left + span.width)) {
            return i;
        }
    }

    return -1;
}

class BreadcrumbRowDelegate : public QStyledItemDelegate {
public:
    explicit BreadcrumbRowDelegate(QObject* parent)
        : QStyledItemDelegate(parent)
    {
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        opt.text.clear();
        opt.icon = QIcon();
        const QWidget* widget = opt.widget;
        QStyle* style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

        const QString path = index.data(SceneBreadcrumbListModel::ePathRole).toString();
        const int count = index.data(SceneBreadcrumbListModel::eCountRole).toInt();
        const RowLayout layout = layoutRow(path, opt.fontMetrics, opt.rect.left() + kRowPadding);

        int hovered = -1;
        const QAbstractItemView* view = qobject_cast<const QAbstractItemView*>(widget);
        if (view && (opt.state & QStyle::State_MouseOver)) {
            hovered = nameAt(layout, view->viewport()->mapFromGlobal(QCursor::pos()).x());
        }

        const QPalette::ColorGroup group = (opt.state & QStyle::State_Enabled) ? QPalette::Normal : QPalette::Disabled;
        const QColor text = opt.palette.color(group, (opt.state & QStyle::State_Selected) ? QPalette::HighlightedText : QPalette::Text);
        QColor dim = text;
        dim.setAlphaF(0.55f);

        painter->save();
        painter->setClipRect(opt.rect);
        const int top = opt.rect.top();
        const int height = opt.rect.height();
        const int flags = Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine;
        const int slashWidth = opt.fontMetrics.horizontalAdvance(QLatin1Char('/'));
        painter->setPen(dim);
        for (int x : layout.slashes) {
            painter->drawText(QRect(x, top, slashWidth, height), flags, QStringLiteral("/"));
        }
        painter->setPen(text);
        QFont underlined = opt.font;
        underlined.setUnderline(true);
        for (int i = 0; i < layout.names.size(); ++i) {
            const NameSpan& span = layout.names[i];
            painter->setFont(i == hovered ? underlined : opt.font);
            painter->drawText(QRect(span.left, top, span.width, height), flags, path.mid(span.start, span.length));
        }
        painter->setFont(opt.font);
        if (count > 0) {
            painter->setPen(dim);
            const QString suffix = QStringLiteral(" (%1)").arg(count);
            painter->drawText(QRect(layout.right, top, opt.fontMetrics.horizontalAdvance(suffix), height), flags, suffix);
        }
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        const QString text = index.data(Qt::DisplayRole).toString();

        return QSize(2 * kRowPadding + option.fontMetrics.horizontalAdvance(text), option.fontMetrics.height() + 2 * kRowVerticalPadding);
    }
};

}

struct SceneBreadcrumbListModel::Private {
    QStringList paths;
    int level = 0;
    QStringList groupPaths;
    // Parallel to groupPaths.
    QList<int> groupCounts;
};

SceneBreadcrumbListModel::SceneBreadcrumbListModel(QObject* parent)
    : QAbstractListModel(parent)
    , _imp(new Private)
{
}

SceneBreadcrumbListModel::~SceneBreadcrumbListModel()
{
    delete _imp;
}

void
SceneBreadcrumbListModel::setPaths(const QStringList& paths)
{
    beginResetModel();
    _imp->paths = paths;
    regroup();
    endResetModel();
}

void
SceneBreadcrumbListModel::setLevel(int level)
{
    level = qBound(0, level, kMaxLevel);
    if (level == _imp->level) {
        return;
    }
    beginResetModel();
    _imp->level = level;
    regroup();
    endResetModel();
}

int
SceneBreadcrumbListModel::getLevel() const
{
    return _imp->level;
}

int
SceneBreadcrumbListModel::getPathCount() const
{
    return int(_imp->paths.size());
}

void
SceneBreadcrumbListModel::regroup()
{
    _imp->groupPaths.clear();
    _imp->groupCounts.clear();
    if (_imp->level <= 0) {
        return;
    }
    // One pass in selection order; the views point into _imp->paths, which outlives the hash.
    QHash<QStringView, int> groupOf;
    for (const QString& path : std::as_const(_imp->paths)) {
        const QStringView ancestor = ancestorAt(path, _imp->level);
        const auto found = groupOf.constFind(ancestor);
        if (found == groupOf.cend()) {
            groupOf.insert(ancestor, int(_imp->groupPaths.size()));
            _imp->groupPaths.push_back(ancestor.toString());
            _imp->groupCounts.push_back(1);
        } else {
            ++_imp->groupCounts[found.value()];
        }
    }
}

QString
SceneBreadcrumbListModel::getRowPath(int row) const
{
    const QStringList& rows = (_imp->level > 0) ? _imp->groupPaths : _imp->paths;
    if ((row < 0) || (row >= rows.size())) {
        return QString();
    }

    return rows[row];
}

int
SceneBreadcrumbListModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }

    return int((_imp->level > 0) ? _imp->groupPaths.size() : _imp->paths.size());
}

QVariant
SceneBreadcrumbListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || (index.row() < 0) || (index.row() >= rowCount())) {
        return QVariant();
    }
    const int row = index.row();
    const int count = (_imp->level > 0) ? _imp->groupCounts[row] : 0;
    switch (role) {
    case Qt::DisplayRole:
    case Qt::ToolTipRole:
        return (count > 0) ? QStringLiteral("%1 (%2)").arg(getRowPath(row)).arg(count) : getRowPath(row);
    case ePathRole:
        return getRowPath(row);
    case eCountRole:
        return count;
    default:
        break;
    }

    return QVariant();
}

Qt::ItemFlags
SceneBreadcrumbListModel::flags(const QModelIndex& index) const
{
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }

    return Qt::ItemIsEnabled | Qt::ItemIsDragEnabled;
}

QStringList
SceneBreadcrumbListModel::mimeTypes() const
{
    return QStringList() << SceneGraphModel::primPathsMimeType() << QStringLiteral("text/plain");
}

QMimeData*
SceneBreadcrumbListModel::mimeData(const QModelIndexList& indexes) const
{
    QStringList paths;
    for (const QModelIndex& index : indexes) {
        const QString path = getRowPath(index.row());
        if (!path.isEmpty() && !paths.contains(path)) {
            paths.push_back(path);
        }
    }

    return SceneGraphModel::mimeDataOfPaths(paths);
}

struct SceneBreadcrumbListPopup::Private {
    QPointer<SceneViewport> viewport;
    QPointer<QWidget> anchor;
    SceneBreadcrumbListModel* model = nullptr;
    QListView* view = nullptr;
    QSpinBox* levelBox = nullptr;
    QLabel* countLabel = nullptr;
    QPoint pressPos;
    int pressRow = -1;
};

SceneBreadcrumbListPopup::SceneBreadcrumbListPopup(SceneViewport* viewport, const QStringList& paths, QWidget* anchor, QWidget* parent)
    : QFrame(parent, Qt::Popup)
    , _imp(new Private)
{
    _imp->viewport = viewport;
    _imp->anchor = anchor;
    setObjectName(QStringLiteral("sceneBreadcrumbListPopup"));
    setAttribute(Qt::WA_DeleteOnClose);
    setFrameStyle(QFrame::StyledPanel | QFrame::Raised);

    _imp->model = new SceneBreadcrumbListModel(this);
    _imp->model->setPaths(paths);

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(2);

    QHBoxLayout* header = new QHBoxLayout;
    header->setContentsMargins(2, 0, 0, 0);
    _imp->countLabel = new QLabel(this);
    _imp->countLabel->setObjectName(QStringLiteral("breadcrumbListCount"));
    header->addWidget(_imp->countLabel, 1);
    QLabel* levelLabel = new QLabel(tr("Level"), this);
    header->addWidget(levelLabel);
    _imp->levelBox = new QSpinBox(this);
    _imp->levelBox->setObjectName(QStringLiteral("breadcrumbListLevel"));
    _imp->levelBox->setRange(0, kMaxLevel);
    _imp->levelBox->setSpecialValueText(tr("Off"));
    _imp->levelBox->setToolTip(tr("Group the selected prims by their ancestor at this depth, with how many are under each. Off lists every prim."));
    levelLabel->setBuddy(_imp->levelBox);
    header->addWidget(_imp->levelBox);
    layout->addLayout(header);

    _imp->view = new QListView(this);
    _imp->view->setObjectName(QStringLiteral("breadcrumbListView"));
    _imp->view->setModel(_imp->model);
    _imp->view->setItemDelegate(new BreadcrumbRowDelegate(_imp->view));
    _imp->view->setUniformItemSizes(true);
    _imp->view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _imp->view->setSelectionMode(QAbstractItemView::NoSelection);
    _imp->view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _imp->view->setMouseTracking(true);
    _imp->view->viewport()->setAttribute(Qt::WA_Hover);
    _imp->view->viewport()->installEventFilter(this);
    layout->addWidget(_imp->view, 1);
    resize(420, 360);

    QObject::connect(_imp->levelBox, &QSpinBox::valueChanged, this, &SceneBreadcrumbListPopup::setLevel);
    if (viewport) {
        QObject::connect(viewport, &SceneViewport::selectionChanged, this, &SceneBreadcrumbListPopup::setPaths);
    }
    publish();
}

SceneBreadcrumbListPopup::~SceneBreadcrumbListPopup()
{
    delete _imp;
}

void
SceneBreadcrumbListPopup::popupUnder(QWidget* under)
{
    if (!under) {
        return;
    }
    const QWidget* edge = _imp->anchor ? _imp->anchor.data() : under;
    const int right = edge->mapToGlobal(QPoint(edge->width(), 0)).x();
    const int bottom = under->mapToGlobal(QPoint(0, under->height())).y();
    QPoint pos(right - width(), bottom);
    if (QScreen* screen = under->screen()) {
        const QRect available = screen->availableGeometry();
        pos.setX(qMax(available.left(), qMin(pos.x(), available.right() + 1 - width())));
        if (pos.y() + height() > available.bottom() + 1) {
            pos.setY(qMax(available.top(), under->mapToGlobal(QPoint(0, 0)).y() - height()));
        }
    }
    move(pos);
    show();
    _imp->view->setFocus();
}

void
SceneBreadcrumbListPopup::setPaths(const QStringList& paths)
{
    _imp->pressRow = -1;
    _imp->model->setPaths(paths);
    publish();
}

void
SceneBreadcrumbListPopup::setLevel(int level)
{
    if (_imp->levelBox->value() != level) {
        _imp->levelBox->setValue(level);
        return;
    }
    _imp->pressRow = -1;
    _imp->model->setLevel(level);
    publish();
}

void
SceneBreadcrumbListPopup::clickSegment(int row, int segment)
{
    const QModelIndex index = _imp->model->index(row, 0);
    if (!index.isValid()) {
        return;
    }
    _imp->view->scrollTo(index);
    const QRect rect = _imp->view->visualRect(index);
    const RowLayout layout = layoutRow(_imp->model->getRowPath(row), _imp->view->fontMetrics(), rect.left() + kRowPadding);
    if ((segment < 0) || (segment >= layout.names.size())) {
        return;
    }
    const NameSpan& span = layout.names[segment];
    clickAt(QPoint(span.left + span.width / 2, rect.center().y()));
}

bool
SceneBreadcrumbListPopup::clickAt(const QPoint& viewportPos)
{
    const QModelIndex index = _imp->view->indexAt(viewportPos);
    if (!index.isValid()) {
        return false;
    }
    const QString path = _imp->model->getRowPath(index.row());
    const RowLayout layout = layoutRow(path, _imp->view->fontMetrics(), _imp->view->visualRect(index).left() + kRowPadding);
    const int name = nameAt(layout, viewportPos.x());
    if (name < 0) {
        return false;
    }
    const NameSpan& span = layout.names[name];
    const QString target = path.left(span.start + span.length);
    // The popup is gone before the selection it would follow changes.
    QPointer<SceneViewport> viewport = _imp->viewport;
    close();
    if (viewport) {
        viewport->select(QStringList(target), SceneViewport::eSelectionModeReplace);
    }

    return true;
}

void
SceneBreadcrumbListPopup::exposeMimeData(int row)
{
    for (QMimeData* old : findChildren<QMimeData*>(QStringLiteral("breadcrumbListMime"), Qt::FindDirectChildrenOnly)) {
        delete old;
    }
    QMimeData* mime = _imp->model->mimeData(QModelIndexList() << _imp->model->index(row, 0));
    if (mime) {
        mime->setObjectName(QStringLiteral("breadcrumbListMime"));
        mime->setParent(this);
    }
}

void
SceneBreadcrumbListPopup::startDrag(int row)
{
    _imp->pressRow = -1;
    const QModelIndex index = _imp->model->index(row, 0);
    QMimeData* mime = _imp->model->mimeData(QModelIndexList() << index);
    if (!mime) {
        return;
    }
    const QPixmap pixmap = _imp->view->viewport()->grab(_imp->view->visualRect(index));
    // A popup keeps the pointer grabbed and would sit over the drop target, so it steps aside for the
    // drag; hidden rather than closed, it is not deleted while the drag it owns runs.
    hide();
    QPointer<SceneBreadcrumbListPopup> guard(this);
    QDrag* drag = new QDrag(this);
    drag->setMimeData(mime);
    drag->setPixmap(pixmap);
    drag->exec(Qt::CopyAction);
    if (guard) {
        close();
    }
}

void
SceneBreadcrumbListPopup::mousePressEvent(QMouseEvent* e)
{
    if (_imp->anchor && !rect().contains(e->position().toPoint())) {
        const QRect anchorRect(_imp->anchor->mapToGlobal(QPoint(0, 0)), _imp->anchor->size());
        if (anchorRect.contains(e->globalPosition().toPoint())) {
            setAttribute(Qt::WA_NoMouseReplay);
        }
    }
    QFrame::mousePressEvent(e);
}

bool
SceneBreadcrumbListPopup::eventFilter(QObject* watched, QEvent* e)
{
    if (watched != _imp->view->viewport()) {
        return QFrame::eventFilter(watched, e);
    }
    switch (e->type()) {
    case QEvent::MouseButtonPress: {
        const QMouseEvent* press = static_cast<QMouseEvent*>(e);
        if (press->button() == Qt::LeftButton) {
            _imp->pressPos = press->position().toPoint();
            _imp->pressRow = _imp->view->indexAt(_imp->pressPos).row();
        }
        break;
    }
    case QEvent::MouseMove: {
        const QMouseEvent* move = static_cast<QMouseEvent*>(e);
        const QPoint pos = move->position().toPoint();
        if ((_imp->pressRow >= 0) && (move->buttons() & Qt::LeftButton)
            && ((pos - _imp->pressPos).manhattanLength() >= QApplication::startDragDistance())) {
            startDrag(_imp->pressRow);

            return true;
        }
        const QModelIndex index = _imp->view->indexAt(pos);
        bool onName = false;
        if (index.isValid()) {
            const RowLayout layout = layoutRow(_imp->model->getRowPath(index.row()), _imp->view->fontMetrics(), _imp->view->visualRect(index).left() + kRowPadding);
            onName = (nameAt(layout, pos.x()) >= 0);
        }
        if (onName) {
            _imp->view->viewport()->setCursor(Qt::PointingHandCursor);
        } else {
            _imp->view->viewport()->unsetCursor();
        }
        // The view repaints a row when the pointer enters it, not as it moves between its names.
        _imp->view->viewport()->update();
        break;
    }
    case QEvent::MouseButtonRelease: {
        const QMouseEvent* release = static_cast<QMouseEvent*>(e);
        const bool pressed = (_imp->pressRow >= 0);
        _imp->pressRow = -1;
        if (pressed && (release->button() == Qt::LeftButton)
            && ((release->position().toPoint() - _imp->pressPos).manhattanLength() < QApplication::startDragDistance())
            && clickAt(release->position().toPoint())) {
            return true;
        }
        break;
    }
    case QEvent::Leave:
        _imp->view->viewport()->unsetCursor();
        _imp->view->viewport()->update();
        break;
    default:
        break;
    }

    return QFrame::eventFilter(watched, e);
}

void
SceneBreadcrumbListPopup::publish()
{
    QStringList rows;
    const int count = _imp->model->rowCount();
    for (int i = 0; (i < count) && (i < kPublishedRows); ++i) {
        rows.push_back(_imp->model->index(i, 0).data().toString());
    }
    const int paths = _imp->model->getPathCount();
    _imp->countLabel->setText(tr("%n selected", nullptr, paths));
    setProperty("natronListRows", rows);
    setProperty("natronListRowCount", count);
    setProperty("natronListPathCount", paths);
    setProperty("natronListLevel", _imp->model->getLevel());
}

NATRON_NAMESPACE_EXIT
