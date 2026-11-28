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

#include "KnobEditMode.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QAbstractScrollArea>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLayout>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/PythonUndoCommands.h"
#include "Engine/RFV.h"
#include "Engine/UserKnobEdit.h"
#include "Serialization/Yaml/KnobYaml.h"

#include "Gui/DockablePanel.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/KnobEditPalette.h"
#include "Gui/KnobEditProperties.h"
#include "Gui/KnobGui.h"
#include "Gui/KnobGuiGroup.h"
#include "Gui/KnobWidgetDnD.h"
#include "Gui/Label.h"

NATRON_NAMESPACE_ENTER

namespace {
int gActiveEditModes = 0;

const QColor kSelectionColor(255, 140, 0);
const int kSelectionPenWidth = 1;
const int kSelectionPadding = 2;
const int kSelectionRadius = 3;

KnobPagePtr
topPageOf(const KnobIPtr& knob)
{
    KnobIPtr cur = knob;
    while (cur) {
        KnobPagePtr isPage = std::dynamic_pointer_cast<KnobPage>(cur);
        if (isPage) {
            return isPage;
        }
        cur = cur->getParentKnob();
    }

    return KnobPagePtr();
}

std::vector<KnobIPtr>
childrenOf(const KnobIPtr& parent)
{
    if (KnobPage* isPage = dynamic_cast<KnobPage*>(parent.get())) {
        return isPage->getChildren();
    }
    if (KnobGroup* isGroup = dynamic_cast<KnobGroup*>(parent.get())) {
        return isGroup->getChildren();
    }

    return std::vector<KnobIPtr>();
}

// An RFV group, the page or group holding one, and a plug-in's own control placed in one can be
// neither edited, deleted nor moved.
bool
isPinnedKnob(const KnobIPtr& knob)
{
    if (RFV::isPluginControl(knob)) {
        return true;
    }
    KnobGroup* isGroup = dynamic_cast<KnobGroup*>(knob.get());
    if (isGroup && isGroup->isRFVGroup()) {
        return true;
    }
    if (!isGroup && !dynamic_cast<KnobPage*>(knob.get())) {
        return false;
    }
    const std::vector<KnobIPtr> children = childrenOf(knob);
    for (std::size_t i = 0; i < children.size(); ++i) {
        if (isPinnedKnob(children[i])) {
            return true;
        }
    }

    return false;
}

int
indexIn(const std::vector<KnobIPtr>& knobs,
        const KnobIPtr& knob)
{
    std::vector<KnobIPtr>::const_iterator found = std::find(knobs.begin(), knobs.end(), knob);

    return found == knobs.end() ? -1 : (int)(found - knobs.begin());
}

bool
isAncestorOrSelf(const KnobIPtr& ancestor,
                 const KnobIPtr& knob)
{
    for (KnobIPtr cur = knob; cur; cur = cur->getParentKnob()) {
        if (cur == ancestor) {
            return true;
        }
    }

    return false;
}

QString
displayName(const KnobIPtr& knob)
{
    if (!knob) {
        return QString();
    }
    const std::string label = knob->getLabel();

    return QString::fromUtf8(label.empty() ? knob->getName().c_str() : label.c_str());
}

std::vector<KnobPagePtr>
topLevelPages(KnobHolder* holder)
{
    std::vector<KnobPagePtr> ret;
    const KnobsVec& knobs = holder->getKnobs();
    for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        KnobPagePtr isPage = std::dynamic_pointer_cast<KnobPage>(*it);
        if (isPage && !isPage->getParentKnob()) {
            ret.push_back(isPage);
        }
    }

    return ret;
}

int
indexOfPage(const std::vector<KnobPagePtr>& pages,
            const KnobPagePtr& page)
{
    std::vector<KnobPagePtr>::const_iterator found = std::find(pages.begin(), pages.end(), page);

    return found == pages.end() ? -1 : (int)(found - pages.begin());
}

/**
 * @brief Where a drop on a page goes, and the line and text showing it.
 **/
struct KnobEditDropTarget {
    bool valid;
    // The drop creates the holder's User page and puts the knob there.
    bool createsUserPage;
    KnobIWPtr parent;
    int index;
    int y;
    int indent;
    QString text;

    KnobEditDropTarget()
        : valid(false)
        , createsUserPage(false)
        , parent()
        , index(-1)
        , y(0)
        , indent(0)
        , text()
    {
    }
};

/**
 * @brief Covers one page of a panel in edit mode: draws the selection, dims built-in knobs,
 * and takes the clicks, drags and drops of the editor so that the knob widgets below stay
 * inert. Every operation is posted to the mode, since it rebuilds the page and this overlay.
 **/
class KnobEditOverlay
    : public QWidget {
public:
    KnobEditOverlay(KnobEditMode* mode,
                    const KnobPagePtr& page,
                    QWidget* host,
                    QAbstractScrollArea* scrollArea)
        : QWidget(host)
        , _mode(mode)
        , _page(page)
        , _host(host)
        , _pressPos()
        , _pressed()
        , _drop()
        , _dragHint()
        , _dragPos()
    {
        setObjectName(QString::fromUtf8("KnobEditOverlay"));
        setAcceptDrops(true);
        setFocusPolicy(Qt::ClickFocus);
        setAttribute(Qt::WA_NoSystemBackground);
        host->installEventFilter(this);
        setGeometry(host->rect());
        if (scrollArea) {
            QObject::connect(scrollArea->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) { update(); });
            QObject::connect(scrollArea->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this](int) { update(); });
        }
        raise();
        show();
    }

    KnobPagePtr getPage() const
    {
        return _page.lock();
    }

private:
    struct Row {
        KnobIPtr knob;
        KnobGuiPtr gui;
        QRect rect;
        // The widgets of the row less their layouts' margins, which hold the gap to the next row.
        QRect content;
        // A knob sharing its line with an earlier one only covers its own label.
        bool partial;
    };

    QRect mapRect(QWidget* w) const
    {
        return QRect(mapFromGlobal(w->mapToGlobal(QPoint(0, 0))), w->size());
    }

    QRect mapContentRect(QWidget* w) const
    {
        const QMargins margins = w->layout() ? w->layout()->contentsMargins() : QMargins();

        return mapRect(w).marginsRemoved(margins);
    }

    std::vector<Row> rows() const
    {
        std::vector<Row> ret;
        DockablePanel* panel = _mode->getPanel();
        KnobPagePtr page = _page.lock();
        if (!panel || !page) {
            return ret;
        }
        std::set<QWidget*> seenFields;
        const KnobsGuiMapping& mapping = panel->getKnobsMapping();
        for (KnobsGuiMapping::const_iterator it = mapping.begin(); it != mapping.end(); ++it) {
            KnobIPtr knob = it->first.lock();
            const KnobGuiPtr& gui = it->second;
            if (!knob || !gui || dynamic_cast<KnobPage*>(knob.get()) || (topPageOf(knob) != page)) {
                continue;
            }
            QWidget* field = gui->getFieldContainer();
            QWidget* label = gui->getLabelContainer();
            const bool fieldShown = field && field->isVisible();
            const bool labelShown = label && label->isVisible();
            if (!fieldShown && !labelShown) {
                continue;
            }
            Row row;
            row.knob = knob;
            row.gui = gui;
            row.partial = false;
            if (fieldShown && !seenFields.insert(field).second) {
                if (!labelShown || (label == field)) {
                    continue;
                }
                row.rect = mapRect(label);
                row.content = mapContentRect(label);
                row.partial = true;
            } else {
                row.rect = fieldShown ? mapRect(field) : mapRect(label);
                row.content = fieldShown ? mapContentRect(field) : mapContentRect(label);
                if (labelShown && (label != field)) {
                    row.rect |= mapRect(label);
                    row.content |= mapContentRect(label);
                }
                row.rect.setLeft(0);
                row.rect.setRight(width() - 1);
            }
            ret.push_back(row);
        }

        return ret;
    }

    const Row* rowAt(const std::vector<Row>& rs,
                     const QPoint& pos) const
    {
        for (std::size_t i = 0; i < rs.size(); ++i) {
            if (rs[i].partial && rs[i].rect.contains(pos)) {
                return &rs[i];
            }
        }
        for (std::size_t i = 0; i < rs.size(); ++i) {
            if (!rs[i].partial && rs[i].rect.contains(pos)) {
                return &rs[i];
            }
        }

        return 0;
    }

    const Row* rowOf(const std::vector<Row>& rs,
                     const KnobIPtr& knob) const
    {
        for (std::size_t i = 0; i < rs.size(); ++i) {
            if (rs[i].knob == knob) {
                return &rs[i];
            }
        }

        return 0;
    }

    bool isUserPage() const
    {
        KnobPagePtr page = _page.lock();

        return page && page->isUserKnob();
    }

    // A built-in page takes drops only while the holder has no user page to put them on.
    bool createsUserPageOnDrop() const
    {
        KnobPagePtr page = _page.lock();
        KnobHolder* holder = _mode->getHolder();
        if (!page || page->isUserKnob() || !holder) {
            return false;
        }
        std::list<KnobPage*> userPages;
        holder->getUserPages(userPages);

        return userPages.empty();
    }

    virtual bool eventFilter(QObject* watched, QEvent* event) OVERRIDE FINAL
    {
        if ((watched == _host) && (event->type() == QEvent::Resize)) {
            setGeometry(_host->rect());
        }

        return false;
    }

    virtual void paintEvent(QPaintEvent* /*e*/) OVERRIDE FINAL
    {
        QPainter p(this);
        KnobPagePtr page = _page.lock();
        if (!page) {
            return;
        }
        if (!page->isUserKnob()) {
            p.fillRect(rect(), QColor(30, 30, 30, 140));
            paintDrop(p);

            return;
        }
        const std::vector<Row> rs = rows();
        KnobIPtr selected = _mode->getSelectedKnob();
        if (selected == page) {
            paintSelectionFrame(p, rect(), false);
        }
        for (std::size_t i = 0; i < rs.size(); ++i) {
            if (!rs[i].knob->isUserKnob() || isPinnedKnob(rs[i].knob)) {
                p.fillRect(rs[i].rect, QColor(30, 30, 30, 140));
            }
        }
        if (const Row* row = rowOf(rs, selected)) {
            paintSelectionFrame(p, selectionFrame(rs, *row), true);
        }
        if (rs.empty()) {
            p.setPen(palette().color(QPalette::Disabled, QPalette::Text));
            p.drawText(rect().adjusted(8, 8, -8, -8), Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                       QObject::tr("Empty page: click a palette entry or drag one here."));
        }
        paintDrop(p);
    } // paintEvent

    // The frame around a selected row: kSelectionPadding clear of its widgets on every side, less
    // where a neighbouring row or the page edge leaves no room for it, and a pixel short of any
    // other row's widgets.
    QRect selectionFrame(const std::vector<Row>& rs,
                         const Row& row) const
    {
        const QRect content = row.content.isValid() ? row.content : row.rect;
        QRect frame;
        for (int padding = TO_DPIY(kSelectionPadding); padding >= 0; --padding) {
            const int outset = padding + kSelectionPenWidth;
            frame = content.adjusted(-outset, -outset, outset, outset);
            frame.setLeft(std::max(frame.left(), 0));
            frame.setRight(std::min(frame.right(), width() - 1));
            if ((frame.top() < 0) || (frame.bottom() > height() - 1)) {
                continue;
            }
            const QRect clearance = frame.adjusted(-1, -1, 1, 1);
            bool clear = true;
            for (std::size_t i = 0; clear && (i < rs.size()); ++i) {
                const QRect other = rs[i].content.isValid() ? rs[i].content : rs[i].rect;
                // A row on the same line shares its widgets' container with this one.
                if ((&rs[i] != &row) && !other.intersects(content) && other.intersects(clearance)) {
                    clear = false;
                }
            }
            if (clear) {
                break;
            }
        }

        return frame;
    }

    void paintSelectionFrame(QPainter& p,
                             const QRect& frame,
                             bool filled)
    {
        const qreal half = kSelectionPenWidth / 2.0;
        const QRectF stroke = QRectF(frame).adjusted(half, half, -half, -half);
        const qreal radius = TO_DPIX(kSelectionRadius);
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(kSelectionColor, kSelectionPenWidth));
        if (filled) {
            QColor fill = kSelectionColor;
            fill.setAlpha(50);
            p.setBrush(fill);
        } else {
            p.setBrush(Qt::NoBrush);
        }
        p.drawRoundedRect(stroke, radius, radius);
        p.restore();
    }

    void paintDrop(QPainter& p)
    {
        if (_drop.valid) {
            p.setPen(QPen(kSelectionColor, 2));
            p.drawLine(_drop.indent, _drop.y, width() - 1, _drop.y);
            QFontMetrics fm(font());
            QRect textRect = fm.boundingRect(_drop.text).adjusted(-4, -2, 4, 2);
            textRect.moveTopLeft(QPoint(_drop.indent + 4, std::max(0, _drop.y - textRect.height() - 2)));
            p.fillRect(textRect, QColor(0, 0, 0, 200));
            p.setPen(kSelectionColor);
            p.drawText(textRect, Qt::AlignCenter, _drop.text);
        }
        if (!_dragHint.isEmpty()) {
            QFontMetrics fm(font());
            QRect hintRect = fm.boundingRect(_dragHint).adjusted(-4, -2, 4, 2);
            hintRect.moveTopLeft(_dragPos + QPoint(16, 16));
            if (hintRect.right() > width()) {
                hintRect.moveRight(width() - 1);
            }
            p.fillRect(hintRect, QColor(0, 0, 0, 220));
            p.setPen(Qt::white);
            p.drawText(hintRect, Qt::AlignCenter, _dragHint);
        }
    }

    virtual void mousePressEvent(QMouseEvent* e) OVERRIDE FINAL
    {
        setFocus(Qt::MouseFocusReason);
        _pressed.reset();
        const bool userPage = isUserPage();
        const std::vector<Row> rs = userPage ? rows() : std::vector<Row>();
        const Row* row = rowAt(rs, e->pos());
        const KnobIPtr editable = (row && _mode->isEditable(row->knob)) ? row->knob : KnobIPtr();
        if ((e->button() == Qt::LeftButton) && userPage) {
            if (editable) {
                _mode->setSelectedKnob(editable);
                _pressed = editable;
                _pressPos = e->pos();
            } else if (!row) {
                _mode->setSelectedKnob(KnobIPtr());
            }
        } else if (e->button() == Qt::RightButton) {
            if (editable) {
                _mode->setSelectedKnob(editable);
            }
            KnobEditMode* mode = _mode;
            const QPoint globalPos = e->globalPosition().toPoint();
            QTimer::singleShot(0, mode, [mode, globalPos, userPage]() { mode->showContextMenu(globalPos, userPage); });
        }
        update();
        e->accept();
    }

    virtual void mouseDoubleClickEvent(QMouseEvent* e) OVERRIDE FINAL
    {
        if (!isUserPage() || (e->button() != Qt::LeftButton)) {
            e->accept();

            return;
        }
        const std::vector<Row> rs = rows();
        const Row* row = rowAt(rs, e->pos());
        KnobGuiGroup* isGroup = row ? dynamic_cast<KnobGuiGroup*>(row->gui.get()) : 0;
        if (isGroup) {
            isGroup->onCheckboxChecked(!isGroup->isChecked());
            update();
        }
        e->accept();
    }

    virtual void mouseMoveEvent(QMouseEvent* e) OVERRIDE FINAL
    {
        KnobIPtr knob = _pressed.lock();
        if (!knob || !(e->buttons() & Qt::LeftButton) || ((e->pos() - _pressPos).manhattanLength() < QApplication::startDragDistance())) {
            return;
        }
        _pressed.reset();
        QDrag* drag = new QDrag(this);
        QMimeData* mimeData = new QMimeData;
        mimeData->setData(QString::fromUtf8(NATRON_KNOB_EDITOR_MIME_TYPE),
                          KnobEditMode::dragPayload(_mode, QString::fromUtf8("move"), QString::fromUtf8(knob->getName().c_str())));
        drag->setMimeData(mimeData);
        const QString text = displayName(knob);
        QFontMetrics fm(font());
        QPixmap pix(fm.horizontalAdvance(text) + 12, fm.height() + 6);
        pix.fill(QColor(0, 0, 0, 200));
        {
            QPainter p(&pix);
            p.setPen(kSelectionColor);
            p.drawText(pix.rect(), Qt::AlignCenter, text);
        }
        drag->setPixmap(pix);
        QPointer<KnobEditOverlay> self(this);
        drag->exec(Qt::MoveAction);
        if (self) {
            clearDrop();
        }
    }

    virtual void mouseReleaseEvent(QMouseEvent* e) OVERRIDE FINAL
    {
        _pressed.reset();
        e->accept();
    }

    virtual void wheelEvent(QWheelEvent* e) OVERRIDE FINAL
    {
        e->ignore();
    }

    virtual void keyPressEvent(QKeyEvent* e) OVERRIDE FINAL
    {
        const char* slot = 0;
        if (e->matches(QKeySequence::Copy)) {
            slot = "copySelection";
        } else if (e->matches(QKeySequence::Paste)) {
            slot = "pasteClipboard";
        } else if ((e->key() == Qt::Key_D) && (e->modifiers() == Qt::ControlModifier)) {
            slot = "duplicateSelection";
        } else if (((e->key() == Qt::Key_Delete) || (e->key() == Qt::Key_Backspace)) && (e->modifiers() == Qt::NoModifier)) {
            slot = "deleteSelection";
        }
        if (slot) {
            QMetaObject::invokeMethod(_mode, slot, Qt::QueuedConnection);
            e->accept();

            return;
        }
        QWidget::keyPressEvent(e);
    }

    enum DragKindEnum {
        eDragKindNone,
        eDragKindNew,
        eDragKindMove,
        eDragKindLink
    };

    DragKindEnum dragKind(const QMimeData* mimeData,
                          QString* value,
                          KnobIPtr* source) const
    {
        if (!mimeData || (!isUserPage() && !createsUserPageOnDrop())) {
            return eDragKindNone;
        }
        if (mimeData->hasFormat(QString::fromUtf8(NATRON_KNOB_EDITOR_MIME_TYPE))) {
            QString kind;
            if (!_mode->parseDragPayload(mimeData->data(QString::fromUtf8(NATRON_KNOB_EDITOR_MIME_TYPE)), &kind, value)) {
                return eDragKindNone;
            }
            if (kind == QString::fromUtf8("new")) {
                return eDragKindNew;
            }
            if (kind == QString::fromUtf8("move")) {
                KnobHolder* holder = _mode->getHolder();
                KnobIPtr knob = holder ? holder->getKnobByName(value->toStdString()) : KnobIPtr();
                if (!_mode->isEditable(knob) || dynamic_cast<KnobPage*>(knob.get())) {
                    return eDragKindNone;
                }
                *source = knob;

                return eDragKindMove;
            }

            return eDragKindNone;
        }
        if (mimeData->hasFormat(QString::fromUtf8(KNOB_DND_MIME_DATA_KEY))) {
            DockablePanel* panel = _mode->getPanel();
            Gui* gui = panel ? panel->getGui() : 0;
            GuiAppInstancePtr app = gui ? gui->getApp() : GuiAppInstancePtr();
            if (!app) {
                return eDragKindNone;
            }
            QDrag* drag = 0;
            KnobIPtr knob;
            int dimension = -1;
            app->getKnobDnDData(&drag, &knob, &dimension);
            if (!knob || (knob->getHolder() == _mode->getHolder()) || !dynamic_cast<EffectInstance*>(knob->getHolder()) || dynamic_cast<KnobPage*>(knob.get()) || dynamic_cast<KnobGroup*>(knob.get())) {
                return eDragKindNone;
            }
            *source = knob;

            return eDragKindLink;
        }

        return eDragKindNone;
    } // dragKind

    KnobEditDropTarget dropTargetAt(const QPoint& pos,
                                    const KnobIPtr& moving) const
    {
        KnobEditDropTarget ret;
        KnobPagePtr page = _page.lock();
        if (!page) {
            return ret;
        }
        if (!page->isUserKnob()) {
            if (!moving && createsUserPageOnDrop()) {
                ret.valid = true;
                ret.createsUserPage = true;
                ret.y = 4;
                ret.text = QObject::tr("Insert into a new %1 page").arg(QString::fromUtf8(NATRON_USER_MANAGED_KNOBS_PAGE_LABEL));
            }

            return ret;
        }
        const std::vector<Row> rs = rows();
        const Row* row = rowAt(rs, pos);
        ret.valid = true;
        if (!row) {
            const std::vector<KnobIPtr> children = page->getChildren();
            ret.parent = page;
            const Row* firstRow = children.empty() ? 0 : rowOf(rs, children.front());
            if (children.empty()) {
                ret.index = 0;
                ret.y = 4;
                ret.text = QObject::tr("Insert into %1").arg(displayName(page));
            } else if (firstRow && (pos.y() < firstRow->rect.top())) {
                ret.index = 0;
                ret.y = firstRow->rect.top();
                ret.text = QObject::tr("Insert before %1 (%2)").arg(displayName(children.front()), displayName(page));
            } else {
                ret.index = (int)children.size();
                int bottom = 4;
                for (std::size_t i = 0; i < rs.size(); ++i) {
                    bottom = std::max(bottom, rs[i].rect.bottom());
                }
                ret.y = bottom;
                ret.text = QObject::tr("Insert after %1 (%2)").arg(displayName(children.back()), displayName(page));
            }
        } else {
            KnobIPtr knob = row->knob;
            KnobIPtr parent = knob->getParentKnob();
            const std::vector<KnobIPtr> siblings = childrenOf(parent);
            const int index = indexIn(siblings, knob);
            const bool upper = pos.y() < row->rect.center().y();
            ret.indent = row->rect.left();
            if (upper) {
                ret.parent = parent;
                ret.index = index;
                ret.y = row->rect.top();
                if (index > 0) {
                    ret.text = QObject::tr("Insert after %1 (%2)").arg(displayName(siblings[index - 1]), displayName(parent));
                } else {
                    ret.text = QObject::tr("Insert before %1 (%2)").arg(displayName(knob), displayName(parent));
                }
            } else if (dynamic_cast<KnobGroup*>(knob.get())) {
                ret.parent = knob;
                ret.index = 0;
                ret.y = row->rect.bottom();
                ret.indent += 16;
                ret.text = QObject::tr("Insert into %1").arg(displayName(knob));
            } else {
                ret.parent = parent;
                ret.index = index + 1;
                ret.y = row->rect.bottom();
                ret.text = QObject::tr("Insert after %1 (%2)").arg(displayName(knob), displayName(parent));
            }
            if (moving && ((knob == moving) || isAncestorOrSelf(moving, ret.parent.lock()))) {
                ret.valid = false;
            }
        }

        return ret;
    } // dropTargetAt

    void clearDrop()
    {
        _drop = KnobEditDropTarget();
        _dragHint.clear();
        update();
    }

    bool updateDrop(QDropEvent* e)
    {
        QString value;
        KnobIPtr source;
        const DragKindEnum kind = dragKind(e->mimeData(), &value, &source);
        if (kind == eDragKindNone) {
            clearDrop();

            return false;
        }
        const QPoint pos = e->position().toPoint();
        _drop = dropTargetAt(pos, kind == eDragKindMove ? source : KnobIPtr());
        _dragPos = pos;
        _dragHint.clear();
        if (kind == eDragKindLink) {
            EffectInstance* sourceEffect = dynamic_cast<EffectInstance*>(source->getHolder());
            const QString nodeName = sourceEffect && sourceEffect->getNode() ? QString::fromUtf8(sourceEffect->getNode()->getLabel().c_str()) : QString();
            _dragHint = QObject::tr("%1 > %2 / release: linked | Ctrl+release: copy").arg(nodeName, displayName(source));
        }
        update();

        return _drop.valid;
    }

    virtual void dragEnterEvent(QDragEnterEvent* e) OVERRIDE FINAL
    {
        QString value;
        KnobIPtr source;
        if (dragKind(e->mimeData(), &value, &source) == eDragKindNone) {
            e->ignore();

            return;
        }
        updateDrop(e);
        e->acceptProposedAction();
    }

    virtual void dragMoveEvent(QDragMoveEvent* e) OVERRIDE FINAL
    {
        if (updateDrop(e)) {
            e->acceptProposedAction();
        } else {
            e->ignore();
        }
    }

    virtual void dragLeaveEvent(QDragLeaveEvent* e) OVERRIDE FINAL
    {
        clearDrop();
        e->accept();
    }

    virtual void dropEvent(QDropEvent* e) OVERRIDE FINAL
    {
        QString value;
        KnobIPtr source;
        const DragKindEnum kind = dragKind(e->mimeData(), &value, &source);
        const KnobEditDropTarget target = kind == eDragKindNone ? KnobEditDropTarget() : dropTargetAt(e->position().toPoint(), kind == eDragKindMove ? source : KnobIPtr());
        clearDrop();
        if (!target.valid) {
            e->ignore();

            return;
        }
        e->acceptProposedAction();
        const bool copy = (e->modifiers() & Qt::ControlModifier) != 0;
        if (kind == eDragKindLink) {
            DockablePanel* panel = _mode->getPanel();
            GuiAppInstancePtr app = panel && panel->getGui() ? panel->getGui()->getApp() : GuiAppInstancePtr();
            if (app) {
                app->setKnobDnDData(0, KnobIPtr(), -1);
            }
        }
        KnobEditMode* mode = _mode;
        const KnobIWPtr parentW = target.parent;
        const KnobIWPtr sourceW = source;
        const bool createsUserPage = target.createsUserPage;
        int index = target.index;
        QTimer::singleShot(0, mode, [mode, kind, value, parentW, sourceW, index, copy, createsUserPage]() {
            // With no parent, adding the knob creates the User page inside the same undo entry.
            if (createsUserPage) {
                if (kind == eDragKindNew) {
                    mode->addKnob(value, KnobIPtr(), -1);
                } else if (kind == eDragKindLink) {
                    KnobIPtr knob = sourceW.lock();
                    if (knob) {
                        mode->addKnobFrom(knob, copy, KnobIPtr(), -1);
                    }
                }

                return;
            }
            KnobIPtr parent = parentW.lock();
            if (!parent) {
                return;
            }
            if (kind == eDragKindNew) {
                mode->addKnob(value, parent, index);
            } else if (kind == eDragKindMove) {
                KnobIPtr knob = sourceW.lock();
                if (!knob) {
                    return;
                }
                int moveIndex = index;
                if (knob->getParentKnob() == parent) {
                    const int current = indexIn(childrenOf(parent), knob);
                    if ((current != -1) && (current < moveIndex)) {
                        --moveIndex;
                    }
                }
                mode->moveKnob(knob, parent, moveIndex);
            } else if (kind == eDragKindLink) {
                KnobIPtr knob = sourceW.lock();
                if (knob) {
                    mode->addKnobFrom(knob, copy, parent, index);
                }
            }
        });
    } // dropEvent

    KnobEditMode* _mode;
    KnobPageWPtr _page;
    QWidget* _host;
    QPoint _pressPos;
    KnobIWPtr _pressed;
    KnobEditDropTarget _drop;
    QString _dragHint;
    QPoint _dragPos;
};
} // anon namespace

struct KnobEditModePrivate {
    DockablePanel* panel;
    bool active;
    KnobIWPtr selected;
    std::string selectedName;
    QPointer<KnobEditPalette> palette;
    QPointer<KnobEditProperties> properties;
    QPointer<QWidget> linkChooser;
    QPointer<QComboBox> linkNodeCombo;
    QPointer<QComboBox> linkKnobCombo;
    std::vector<NodeWPtr> linkNodes;
    std::list<QPointer<KnobEditOverlay>> overlays;
    QPointer<QTabBar> tabBar;
    KnobPageWPtr pressedPage;
    int pressedTab;

    explicit KnobEditModePrivate(DockablePanel* panel)
        : panel(panel)
        , active(false)
        , selected()
        , selectedName()
        , palette()
        , properties()
        , linkChooser()
        , linkNodeCombo()
        , linkKnobCombo()
        , linkNodes()
        , overlays()
        , tabBar()
        , pressedPage()
        , pressedTab(-1)
    {
    }

    KnobHolder* holder() const
    {
        return panel ? panel->getHolder() : 0;
    }

    NodePtr node() const
    {
        EffectInstance* isEffect = dynamic_cast<EffectInstance*>(holder());

        return isEffect ? isEffect->getNode() : NodePtr();
    }

    bool isEditable(const KnobIPtr& knob) const
    {
        KnobHolder* h = holder();

        return h && knob && knob->isUserKnob() && (knob->getHolder() == h) && (h->getKnobByName(knob->getName()) == knob) && !isPinnedKnob(knob);
    }

    KnobPagePtr pageAtTab(int index) const
    {
        QTabWidget* tabWidget = panel ? panel->getTabWidget() : 0;
        if (!tabWidget || (index < 0) || (index >= tabWidget->count())) {
            return KnobPagePtr();
        }
        QWidget* tab = tabWidget->widget(index);
        const PagesMap& pages = panel->getPages();
        for (PagesMap::const_iterator it = pages.begin(); it != pages.end(); ++it) {
            if (it->second->tab == tab) {
                return it->second->pageKnob.lock();
            }
        }

        return KnobPagePtr();
    }

    int tabOfPage(const KnobPagePtr& page) const
    {
        QTabWidget* tabWidget = panel ? panel->getTabWidget() : 0;
        if (!tabWidget || !page) {
            return -1;
        }
        PagesMap::const_iterator found = panel->getPages().find(page);

        return found == panel->getPages().end() ? -1 : tabWidget->indexOf(found->second->tab);
    }

    void showPageOf(const KnobIPtr& knob)
    {
        KnobPagePtr page = topPageOf(knob);
        if (panel && page) {
            panel->setPageActiveIndex(page);
        }
    }

    void clearOverlays()
    {
        for (std::list<QPointer<KnobEditOverlay>>::iterator it = overlays.begin(); it != overlays.end(); ++it) {
            if (*it) {
                (*it)->hide();
                (*it)->deleteLater();
            }
        }
        overlays.clear();
    }

    void updateOverlays()
    {
        for (std::list<QPointer<KnobEditOverlay>>::iterator it = overlays.begin(); it != overlays.end(); ++it) {
            if (*it) {
                (*it)->update();
            }
        }
    }

    void rebuildOverlays(KnobEditMode* mode)
    {
        clearOverlays();
        if (!active || !panel) {
            return;
        }
        const PagesMap& pages = panel->getPages();
        for (PagesMap::const_iterator it = pages.begin(); it != pages.end(); ++it) {
            KnobPagePtr page = it->second->pageKnob.lock();
            QWidget* tab = it->second->tab;
            if (!page || !tab) {
                continue;
            }
            QAbstractScrollArea* scrollArea = qobject_cast<QAbstractScrollArea*>(tab);
            QWidget* host = scrollArea ? scrollArea->viewport() : tab;
            overlays.push_back(new KnobEditOverlay(mode, page, host, scrollArea));
        }
    }

    std::vector<NodePtr> linkableNodes() const;
    void fillLinkKnobs();
};

KnobEditMode::KnobEditMode(DockablePanel* panel)
    : QObject(panel)
    , _imp(new KnobEditModePrivate(panel))
{
    setObjectName(QString::fromUtf8("KnobEditMode"));
}

KnobEditMode::~KnobEditMode()
{
    if (_imp->active) {
        --gActiveEditModes;
    }
}

DockablePanel*
KnobEditMode::getPanel() const
{
    return _imp->panel;
}

KnobHolder*
KnobEditMode::getHolder() const
{
    return _imp->holder();
}

bool
KnobEditMode::isActive() const
{
    return _imp->active;
}

bool
KnobEditMode::isAnyPanelEditing()
{
    return gActiveEditModes > 0;
}

KnobEditProperties*
KnobEditMode::getProperties() const
{
    return _imp->properties;
}

bool
KnobEditMode::setActive(bool active)
{
    if (active == _imp->active) {
        return true;
    }
    DockablePanel* panel = _imp->panel;
    if (active) {
        if (!panel || !panel->canEditUserKnobs()) {
            return false;
        }
        NodePtr node = _imp->node();
        if (node && node->isPyPlug()) {
            StandardButtonEnum rep = Dialogs::questionDialog(tr("PyPlug").toStdString(),
                                                             tr("You are about to edit parameters of this node which will "
                                                                "automatically convert this node as a Group. Are you sure "
                                                                "you want to edit it?")
                                                                 .toStdString(),
                                                             false);
            if (rep != eStandardButtonYes) {
                return false;
            }
            node->setPyPlugEdited(true);
        }
        _imp->active = true;
        ++gActiveEditModes;
        QVBoxLayout* column = panel->getPagesColumnLayout();
        if (!_imp->palette) {
            _imp->palette = new KnobEditPalette(this, panel);
            QObject::connect(_imp->palette.data(), SIGNAL(entryClicked(QString)), this, SLOT(onPaletteEntryClicked(QString)));
            if (column) {
                column->insertWidget(0, _imp->palette);
            }
        }
        if (!_imp->properties) {
            _imp->properties = new KnobEditProperties(this, panel);
            if (column) {
                column->addWidget(_imp->properties);
            }
        }
        _imp->palette->show();
        _imp->properties->show();
        QTabWidget* tabWidget = panel->getTabWidget();
        if (tabWidget) {
            _imp->tabBar = tabWidget->tabBar();
            _imp->tabBar->installEventFilter(this);
            _imp->tabBar->setChangeCurrentOnDrag(true);
            QObject::connect(_imp->tabBar.data(), SIGNAL(tabBarClicked(int)), this, SLOT(onTabBarClicked(int)));
        }
        panel->recreateUserKnobs(true);
        _imp->properties->setKnob(getSelectedKnob());
    } else {
        if (_imp->properties) {
            _imp->properties->commitPending();
        }
        _imp->active = false;
        --gActiveEditModes;
        _imp->clearOverlays();
        if (_imp->palette) {
            _imp->palette->hide();
        }
        if (_imp->linkChooser) {
            _imp->linkChooser->hide();
        }
        if (_imp->properties) {
            _imp->properties->setKnob(KnobIPtr());
            _imp->properties->hide();
        }
        if (_imp->tabBar) {
            _imp->tabBar->removeEventFilter(this);
            _imp->tabBar->setMovable(false);
            _imp->tabBar->setChangeCurrentOnDrag(false);
            QObject::disconnect(_imp->tabBar.data(), SIGNAL(tabBarClicked(int)), this, SLOT(onTabBarClicked(int)));
            _imp->tabBar.clear();
        }
        _imp->selected.reset();
        _imp->selectedName.clear();
        if (panel && panel->isVisible()) {
            panel->recreateUserKnobs(true);
        }
        Q_EMIT selectionChanged();
    }
    Q_EMIT activeChanged(active);

    return true;
} // KnobEditMode::setActive

void
KnobEditMode::detach()
{
    if (_imp->active) {
        --gActiveEditModes;
        _imp->active = false;
    }
    _imp->overlays.clear();
    _imp->panel = 0;
}

bool
KnobEditMode::isEditable(const KnobIPtr& knob) const
{
    return _imp->isEditable(knob);
}

KnobIPtr
KnobEditMode::getSelectedKnob() const
{
    KnobIPtr knob = _imp->selected.lock();

    return _imp->isEditable(knob) ? knob : KnobIPtr();
}

void
KnobEditMode::setSelectedKnob(const KnobIPtr& knob)
{
    KnobIPtr toSelect = (_imp->active && _imp->isEditable(knob)) ? knob : KnobIPtr();
    if ((toSelect == _imp->selected.lock()) && ((bool)toSelect || _imp->selectedName.empty())) {
        return;
    }
    _imp->selected = toSelect;
    _imp->selectedName = toSelect ? toSelect->getName() : std::string();
    if (_imp->properties) {
        _imp->properties->setKnob(toSelect);
    }
    _imp->updateOverlays();
    Q_EMIT selectionChanged();
}

QString
KnobEditMode::getSelectedKnobName() const
{
    KnobIPtr knob = getSelectedKnob();

    return knob ? QString::fromUtf8(knob->getName().c_str()) : QString();
}

bool
KnobEditMode::selectKnob(const QString& name)
{
    KnobHolder* holder = _imp->holder();
    KnobIPtr knob = holder && !name.isEmpty() ? holder->getKnobByName(name.toStdString()) : KnobIPtr();
    if (knob && !_imp->isEditable(knob)) {
        return false;
    }
    setSelectedKnob(knob);

    return (bool)knob == !name.isEmpty();
}

QStringList
KnobEditMode::getChildNames(const QString& parentName) const
{
    QStringList ret;
    KnobHolder* holder = _imp->holder();
    if (!holder) {
        return ret;
    }
    if (parentName.isEmpty()) {
        const std::vector<KnobPagePtr> pages = topLevelPages(holder);
        for (std::size_t i = 0; i < pages.size(); ++i) {
            ret.push_back(QString::fromUtf8(pages[i]->getName().c_str()));
        }

        return ret;
    }
    const std::vector<KnobIPtr> children = childrenOf(holder->getKnobByName(parentName.toStdString()));
    for (std::size_t i = 0; i < children.size(); ++i) {
        ret.push_back(QString::fromUtf8(children[i]->getName().c_str()));
    }

    return ret;
}

void
KnobEditMode::onKnobsRecreated()
{
    if (!_imp->active) {
        return;
    }
    KnobIPtr knob = _imp->selected.lock();
    if (!_imp->isEditable(knob)) {
        KnobHolder* holder = _imp->holder();
        knob = holder && !_imp->selectedName.empty() ? holder->getKnobByName(_imp->selectedName) : KnobIPtr();
        if (!_imp->isEditable(knob)) {
            knob.reset();
        }
    }
    const bool changed = knob != _imp->selected.lock();
    _imp->selected = knob;
    _imp->selectedName = knob ? knob->getName() : std::string();
    if (_imp->properties) {
        _imp->properties->setKnob(knob);
    }
    _imp->rebuildOverlays(this);
    if (changed) {
        Q_EMIT selectionChanged();
    }
}

void
KnobEditMode::getInsertionPoint(KnobIPtr* parent,
                                int* index) const
{
    *parent = KnobIPtr();
    *index = -1;
    KnobIPtr selected = getSelectedKnob();
    if (std::dynamic_pointer_cast<KnobPage>(selected)) {
        *parent = selected;

        return;
    }
    if (selected) {
        KnobIPtr selectedParent = selected->getParentKnob();
        const int selectedIndex = indexIn(childrenOf(selectedParent), selected);
        if (selectedParent && (selectedIndex != -1)) {
            *parent = selectedParent;
            *index = selectedIndex + 1;

            return;
        }
    }
    KnobHolder* holder = _imp->holder();
    if (!holder) {
        return;
    }
    std::list<KnobPage*> userPages;
    holder->getUserPages(userPages);
    if (!userPages.empty()) {
        *parent = userPages.front()->shared_from_this();
    }
}

KnobIPtr
KnobEditMode::addKnob(const QString& entryId,
                      const KnobIPtr& parent,
                      int index)
{
    KnobHolder* holder = _imp->holder();
    const KnobEditTypeEntry* entry = findKnobEditTypeEntry(entryId);
    if (!holder || !entry || !_imp->active) {
        return KnobIPtr();
    }
    UserKnobSpec spec;
    spec.typeName = entry->typeName;
    spec.dimension = entry->dimension;
    spec.name = uniqueKnobName(holder, entry->baseName, true);
    spec.label = knobEditReadableLabel(spec.name);
    const bool isPage = entry->typeName == KnobPage::typeNameStatic();
    if (!isPage && parent) {
        spec.parentName = parent->getName();
        spec.index = index;
    }
    if (entry->typeName == KnobChoice::typeNameStatic()) {
        Serialization::KnobChoiceExtraSerialization choice;
        choice.entries.push_back("Option 1");
        choice.entries.push_back("Option 2");
        choice.helpStrings.resize(choice.entries.size());
        spec.extra = choice;
    } else if (entry->isLabel) {
        Serialization::KnobTextExtraSerialization text;
        text.isLabel = true;
        spec.extra = text;
    }
    std::string error;
    KnobIPtr knob = createUserKnob(holder, spec, &error);
    if (!knob) {
        Dialogs::errorDialog(tr("Add parameter").toStdString(), error);

        return KnobIPtr();
    }
    _imp->showPageOf(knob);
    setSelectedKnob(knob);
    if (_imp->properties) {
        _imp->properties->focusNameField();
    }

    return knob;
} // KnobEditMode::addKnob

void
KnobEditMode::addKnobOfEntry(const QString& entryId)
{
    if (entryId == QString::fromUtf8(NATRON_KNOB_EDITOR_LINK_ENTRY)) {
        showLinkChooser();

        return;
    }
    KnobIPtr parent;
    int index = -1;
    if (entryId != QString::fromUtf8("Page")) {
        getInsertionPoint(&parent, &index);
    }
    addKnob(entryId, parent, index);
}

void
KnobEditMode::onPaletteEntryClicked(QString entryId)
{
    addKnobOfEntry(entryId);
}

bool
KnobEditMode::moveKnob(const KnobIPtr& knob,
                       const KnobIPtr& parent,
                       int index)
{
    KnobHolder* holder = _imp->holder();
    if (!holder || !_imp->isEditable(knob) || !parent || std::dynamic_pointer_cast<KnobPage>(knob) || isAncestorOrSelf(knob, parent)) {
        return false;
    }
    std::vector<KnobIPtr> siblings = childrenOf(parent);
    siblings.erase(std::remove(siblings.begin(), siblings.end(), knob), siblings.end());
    if ((index < 0) || (index > (int)siblings.size())) {
        index = (int)siblings.size();
    }
    UserKnobSpec spec = specFromKnob(knob);
    spec.parentName = parent->getName();
    spec.index = index;
    std::string error;
    if (!applySpec(holder, knob, spec, &error)) {
        Dialogs::errorDialog(tr("Move parameter").toStdString(), error);

        return false;
    }
    _imp->showPageOf(knob);
    setSelectedKnob(knob);

    return true;
}

KnobIPtr
KnobEditMode::addKnobFrom(const KnobIPtr& source,
                          bool copy,
                          const KnobIPtr& parent,
                          int index)
{
    KnobHolder* holder = _imp->holder();
    if (!holder || !source || (source->getHolder() == holder) || !_imp->active) {
        return KnobIPtr();
    }
    KnobPagePtr page = std::dynamic_pointer_cast<KnobPage>(parent);
    KnobGroupPtr group = std::dynamic_pointer_cast<KnobGroup>(parent);
    if (page && !page->isUserKnob()) {
        page.reset();
    }
    if (group && !group->isUserKnob()) {
        group.reset();
    }
    KnobIPtr knob;
    QStringList messages;
    if (copy) {
        std::string error;
        knob = copyKnobToHolder(holder, source, page ? KnobIPtr(page) : KnobIPtr(group), (page || group) ? index : -1, &error);
        if (!knob) {
            messages.push_back(QString::fromUtf8(error.c_str()));
        }
    } else {
        const std::string name = uniqueKnobName(holder, source->getName(), false);
        UserKnobsUndoScope undo(holder, "Link parameter " + name);
        bool reported = false;
        try {
            knob = source->createDuplicateOnHolder(holder, page, group, (page || group) ? index : -1, true, name,
                                                   source->getLabel(), source->getHintToolTip(), true, true);
        } catch (const std::exception& e) {
            Dialogs::errorDialog(tr("Add parameter").toStdString(), e.what());
            reported = true;
        }
        if (!knob && !reported) {
            messages.push_back(tr("Cannot add a parameter of this type."));
        }
        if (knob) {
            undo.markChanged();
        }
    }
    AppInstancePtr app = messages.isEmpty() ? AppInstancePtr() : holder->getApp();
    if (app) {
        app->showStatusMessage(messages.join(QLatin1Char(' ')));
    }
    if (knob) {
        _imp->showPageOf(knob);
        setSelectedKnob(knob);
    }

    return knob;
} // KnobEditMode::addKnobFrom

bool
KnobEditMode::deleteKnob(const KnobIPtr& knob)
{
    KnobHolder* holder = _imp->holder();
    if (!holder || !_imp->isEditable(knob)) {
        return false;
    }
    std::list<KnobIPtr> toCheck(1, knob);
    std::list<KnobIPtr> dependents;
    while (!toCheck.empty()) {
        KnobIPtr cur = toCheck.front();
        toCheck.pop_front();
        const std::vector<KnobIPtr> children = childrenOf(cur);
        toCheck.insert(toCheck.end(), children.begin(), children.end());
        const std::list<KnobIPtr> curDependents = getUserKnobDependents(cur);
        for (std::list<KnobIPtr>::const_iterator it = curDependents.begin(); it != curDependents.end(); ++it) {
            if (!isAncestorOrSelf(knob, *it) && (std::find(dependents.begin(), dependents.end(), *it) == dependents.end())) {
                dependents.push_back(*it);
            }
        }
    }
    if (!dependents.empty()) {
        QStringList names;
        for (std::list<KnobIPtr>::const_iterator it = dependents.begin(); it != dependents.end(); ++it) {
            EffectInstance* isEffect = dynamic_cast<EffectInstance*>((*it)->getHolder());
            QString name = QString::fromUtf8((*it)->getName().c_str());
            if (isEffect && isEffect->getNode()) {
                name.prepend(QString::fromUtf8(isEffect->getNode()->getFullyQualifiedName().c_str()) + QLatin1Char('.'));
            }
            names.push_back(name);
        }
        StandardButtonEnum rep = Dialogs::questionDialog(tr("Delete parameter").toStdString(),
                                                         tr("These parameters link to %1 or use it in an expression:\n%2\n"
                                                            "Delete it anyway?")
                                                             .arg(displayName(knob), names.join(QString::fromUtf8("\n")))
                                                             .toStdString(),
                                                         false);
        if (rep != eStandardButtonYes) {
            return false;
        }
    }
    const bool deleted = deleteUserKnob(holder, knob);
    if (deleted) {
        setSelectedKnob(KnobIPtr());
    }

    return deleted;
} // KnobEditMode::deleteKnob

void
KnobEditMode::deleteSelection()
{
    KnobIPtr knob = getSelectedKnob();
    if (knob) {
        deleteKnob(knob);
    }
}

void
KnobEditMode::copySelection()
{
    KnobHolder* holder = _imp->holder();
    KnobIPtr knob = getSelectedKnob();
    if (!holder || !knob) {
        return;
    }
    const std::string text = copyUserKnobs(holder, std::list<KnobIPtr>(1, knob));
    if (text.empty()) {
        return;
    }
    QMimeData* mimeData = new QMimeData;
    mimeData->setData(QString::fromUtf8(NATRON_USER_KNOBS_MIME_TYPE), QByteArray(text.c_str(), (int)text.size()));
    if (std::dynamic_pointer_cast<KnobPage>(knob)) {
        mimeData->setData(QString::fromUtf8(NATRON_USER_PAGES_MIME_TYPE), QByteArray());
    }
    mimeData->setText(QString::fromUtf8(text.c_str()));
    QApplication::clipboard()->setMimeData(mimeData);
}

namespace {
void
pasteParent(const KnobIPtr& selected,
            bool wholePages,
            KnobIPtr* parent,
            int* index)
{
    *parent = KnobIPtr();
    *index = -1;
    if (!selected || wholePages) {
        return;
    }
    if (std::dynamic_pointer_cast<KnobPage>(selected)) {
        *parent = selected;

        return;
    }
    KnobIPtr selectedParent = selected->getParentKnob();
    const int selectedIndex = indexIn(childrenOf(selectedParent), selected);
    if (selectedParent && (selectedIndex != -1)) {
        *parent = selectedParent;
        *index = selectedIndex + 1;
    }
}

// The user knobs YAML on the clipboard, or an empty string when it holds none.
std::string
clipboardUserKnobs(bool* wholePages)
{
    *wholePages = false;
    const QMimeData* mimeData = QApplication::clipboard()->mimeData();
    if (!mimeData) {
        return std::string();
    }
    if (mimeData->hasFormat(QString::fromUtf8(NATRON_USER_KNOBS_MIME_TYPE))) {
        const QByteArray data = mimeData->data(QString::fromUtf8(NATRON_USER_KNOBS_MIME_TYPE));
        *wholePages = mimeData->hasFormat(QString::fromUtf8(NATRON_USER_PAGES_MIME_TYPE));

        return std::string(data.constData(), data.size());
    }
    if (!mimeData->hasText()) {
        return std::string();
    }
    const std::string text = mimeData->text().toStdString();
    try {
        if (!Serialization::decodeUserPagesYaml(text).empty()) {
            return text;
        }
    } catch (const std::exception&) {
    }

    return std::string();
}
} // anon namespace

void
KnobEditMode::pasteClipboard()
{
    KnobHolder* holder = _imp->holder();
    if (!holder || !_imp->active) {
        return;
    }
    bool wholePages = false;
    const std::string text = clipboardUserKnobs(&wholePages);
    if (text.empty()) {
        return;
    }
    KnobIPtr parent;
    int index = -1;
    pasteParent(getSelectedKnob(), wholePages, &parent, &index);
    std::string error;
    const std::list<KnobIPtr> created = pasteUserKnobs(holder, text, parent, index, &error);
    if (created.empty()) {
        if (!error.empty()) {
            Dialogs::errorDialog(tr("Paste parameters").toStdString(), error);
        }

        return;
    }
    _imp->showPageOf(created.front());
    setSelectedKnob(created.front());
}

void
KnobEditMode::duplicateSelection()
{
    KnobHolder* holder = _imp->holder();
    KnobIPtr knob = getSelectedKnob();
    if (!holder || !knob) {
        return;
    }
    const std::string text = copyUserKnobs(holder, std::list<KnobIPtr>(1, knob));
    KnobIPtr parent;
    int index = -1;
    pasteParent(knob, (bool)std::dynamic_pointer_cast<KnobPage>(knob), &parent, &index);
    std::string error;
    const std::list<KnobIPtr> created = pasteUserKnobs(holder, text, parent, index, &error);
    if (created.empty()) {
        if (!error.empty()) {
            Dialogs::errorDialog(tr("Duplicate parameter").toStdString(), error);
        }

        return;
    }
    _imp->showPageOf(created.front());
    setSelectedKnob(created.front());
}

void
KnobEditMode::showContextMenu(const QPoint& globalPos,
                              bool onUserPage)
{
    DockablePanel* panel = _imp->panel;
    if (!panel || !_imp->active) {
        return;
    }
    const bool hasSelection = onUserPage && (bool)getSelectedKnob();
    bool wholePages = false;
    const bool canPaste = !clipboardUserKnobs(&wholePages).empty();

    QMenu menu(panel);
    menu.setObjectName(QString::fromUtf8("KnobEditContextMenu"));
    const auto addAction = [&](const QString& text, const QKeySequence& shortcut, bool enabled, const char* slot) {
        QAction* action = menu.addAction(text);
        action->setShortcut(shortcut);
        action->setShortcutContext(Qt::WidgetShortcut);
        action->setEnabled(enabled);
        QObject::connect(action, SIGNAL(triggered()), this, slot);
    };
    addAction(tr("Copy"), QKeySequence(QKeySequence::Copy), hasSelection, SLOT(copySelection()));
    addAction(tr("Paste"), QKeySequence(QKeySequence::Paste), canPaste, SLOT(pasteClipboard()));
    addAction(tr("Duplicate"), QKeySequence(Qt::CTRL | Qt::Key_D), hasSelection, SLOT(duplicateSelection()));
    addAction(tr("Delete"), QKeySequence(Qt::Key_Delete), hasSelection, SLOT(deleteSelection()));
    menu.addSeparator();
    QMenu* addAfter = menu.addMenu(tr("Add After"));
    const std::vector<KnobEditTypeEntry>& entries = knobEditTypeEntries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        QAction* action = addAfter->addAction(entries[i].label);
        const QString id = entries[i].id;
        QObject::connect(action, &QAction::triggered, this, [this, id]() { addKnobOfEntry(id); });
    }
    QAction* link = addAfter->addAction(tr("Link knob..."));
    QObject::connect(link, &QAction::triggered, this, [this]() { showLinkChooser(); });
    menu.addSeparator();
    QAction* editParams = menu.addAction(tr("Edit parameters"));
    editParams->setCheckable(true);
    editParams->setChecked(true);
    QObject::connect(editParams, &QAction::triggered, this, [this]() { setActive(false); });

    menu.exec(globalPos);
} // KnobEditMode::showContextMenu

QByteArray
KnobEditMode::dragPayload(const KnobEditMode* mode,
                          const QString& kind,
                          const QString& value)
{
    const QString payload = QString::number((quintptr)mode) + QLatin1Char('|') + kind + QLatin1Char('|') + value;

    return payload.toUtf8();
}

bool
KnobEditMode::parseDragPayload(const QByteArray& payload,
                               QString* kind,
                               QString* value) const
{
    const QString text = QString::fromUtf8(payload);
    const int first = text.indexOf(QLatin1Char('|'));
    const int second = first == -1 ? -1 : text.indexOf(QLatin1Char('|'), first + 1);
    if (second == -1) {
        return false;
    }
    bool ok = false;
    const quintptr mode = text.left(first).toULongLong(&ok);
    if (!ok || (mode != (quintptr)this)) {
        return false;
    }
    *kind = text.mid(first + 1, second - first - 1);
    *value = text.mid(second + 1);

    return true;
}

void
KnobEditMode::onTabBarClicked(int index)
{
    KnobPagePtr page = _imp->pageAtTab(index);
    if (page && page->isUserKnob()) {
        setSelectedKnob(page);
    }
}

bool
KnobEditMode::eventFilter(QObject* watched,
                          QEvent* event)
{
    if (!_imp->active || !_imp->tabBar || (watched != _imp->tabBar.data())) {
        return false;
    }
    if (event->type() == QEvent::MouseButtonPress) {
        QMouseEvent* e = static_cast<QMouseEvent*>(event);
        const int tab = _imp->tabBar->tabAt(e->pos());
        KnobPagePtr page = _imp->pageAtTab(tab);
        const bool movable = page && page->isUserKnob() && !isPinnedKnob(page);
        _imp->pressedPage = movable ? page : KnobPagePtr();
        _imp->pressedTab = movable ? tab : -1;
        _imp->tabBar->setMovable(movable);
    } else if (event->type() == QEvent::MouseButtonRelease) {
        if (_imp->pressedPage.lock()) {
            QTimer::singleShot(0, this, [this]() {
                KnobPagePtr page = _imp->pressedPage.lock();
                const int pressedTab = _imp->pressedTab;
                _imp->pressedPage.reset();
                _imp->pressedTab = -1;
                KnobHolder* holder = _imp->holder();
                const int tab = _imp->tabOfPage(page);
                if (!holder || !page || (tab == -1) || (tab == pressedTab)) {
                    return;
                }
                KnobPagePtr left = _imp->pageAtTab(tab - 1);
                bool moved = false;
                {
                    UserKnobsUndoScope undo(holder, "Move page " + page->getName());
                    const int maxSteps = (int)holder->getKnobs().size();
                    for (int step = 0; step < maxSteps; ++step) {
                        const std::vector<KnobPagePtr> pages = topLevelPages(holder);
                        const int pos = indexOfPage(pages, page);
                        const int leftPos = left ? indexOfPage(pages, left) : -1;
                        if ((pos == -1) || (pos == leftPos + 1)) {
                            break;
                        }
                        if (!moveUserKnob(holder, page, pos > leftPos + 1)) {
                            break;
                        }
                        moved = true;
                    }
                    if (moved) {
                        undo.markChanged();
                    }
                }
                if (!moved && _imp->panel) {
                    _imp->panel->recreateUserKnobs(true);
                }
                _imp->showPageOf(page);
                setSelectedKnob(page);
            });
        }
    }

    return false;
} // KnobEditMode::eventFilter

static std::string
linkNodeGroupPath(const NodePtr& n)
{
    NodeCollectionPtr collection = n->getGroup();
    NodeGroup* isGroup = dynamic_cast<NodeGroup*>(collection.get());
    NodePtr groupNode = isGroup ? isGroup->getNode() : NodePtr();

    return groupNode ? groupNode->getFullyQualifiedName() : std::string();
}

static std::string
linkNodeDisplayName(const NodePtr& n)
{
    const std::string prefix = linkNodeGroupPath(n);

    return prefix.empty() ? n->getLabel() : prefix + "." + n->getLabel();
}

std::vector<NodePtr>
KnobEditModePrivate::linkableNodes() const
{
    std::vector<NodePtr> ret;
    NodePtr self = node();
    if (!self) {
        return ret;
    }
    const auto accepts = [&](const NodePtr& n) {
        return n && (n != self) && !n->getParentMultiInstance() && n->isActivated() && !n->getKnobs().empty() && (std::find(ret.begin(), ret.end(), n) == ret.end());
    };
    NodeCollectionPtr collection = self->getGroup();
    if (collection) {
        const NodesList nodes = collection->getNodes();
        for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
            if (accepts(*it)) {
                ret.push_back(*it);
            }
        }
        NodeGroup* isCollectionGroup = dynamic_cast<NodeGroup*>(collection.get());
        if (isCollectionGroup && accepts(isCollectionGroup->getNode())) {
            ret.push_back(isCollectionGroup->getNode());
        }
    }
    NodeGroup* isGroup = self->isEffectGroup();
    if (isGroup) {
        const NodesList nodes = isGroup->getNodes();
        for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
            if ((*it)->getNodeGui() && accepts(*it)) {
                ret.push_back(*it);
            }
        }
    }
    std::stable_sort(ret.begin(), ret.end(), [](const NodePtr& a, const NodePtr& b) {
        const bool aNested = linkNodeGroupPath(a) != std::string();
        const bool bNested = linkNodeGroupPath(b) != std::string();
        if (aNested != bNested) {
            return !aNested;
        }

        return linkNodeDisplayName(a) < linkNodeDisplayName(b);
    });

    return ret;
}

void
KnobEditModePrivate::fillLinkKnobs()
{
    if (!linkNodeCombo || !linkKnobCombo) {
        return;
    }
    linkKnobCombo->clear();
    const int nodeIndex = linkNodeCombo->currentIndex();
    NodePtr n = ((nodeIndex >= 0) && (nodeIndex < (int)linkNodes.size())) ? linkNodes[nodeIndex].lock() : NodePtr();
    if (!n) {
        return;
    }
    const KnobsVec& knobs = n->getKnobs();
    for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        if ((*it)->getIsSecret() || (*it)->getName().empty() || dynamic_cast<KnobPage*>(it->get()) || dynamic_cast<KnobGroup*>(it->get())) {
            continue;
        }
        const QString name = QString::fromUtf8((*it)->getName().c_str());
        linkKnobCombo->addItem(QString::fromUtf8("%1 (%2)").arg(displayName(*it), name), name);
    }
}

void
KnobEditMode::showLinkChooser()
{
    DockablePanel* panel = _imp->panel;
    if (!panel || !_imp->active) {
        return;
    }
    if (!_imp->linkChooser) {
        QWidget* chooser = new QWidget(panel);
        chooser->setObjectName(QString::fromUtf8("KnobEditLinkChooser"));
        QHBoxLayout* layout = new QHBoxLayout(chooser);
        layout->setContentsMargins(4, 2, 4, 2);
        layout->addWidget(new Label(tr("Node"), chooser));
        QComboBox* nodeCombo = new QComboBox(chooser);
        nodeCombo->setObjectName(QString::fromUtf8("KnobEditLinkNode"));
        layout->addWidget(nodeCombo, 1);
        layout->addWidget(new Label(tr("Parameter"), chooser));
        QComboBox* knobCombo = new QComboBox(chooser);
        knobCombo->setObjectName(QString::fromUtf8("KnobEditLinkKnob"));
        layout->addWidget(knobCombo, 1);
        const auto addButton = [&](const QString& text, const QString& name, const QString& tooltip) {
            QPushButton* button = new QPushButton(text, chooser);
            button->setObjectName(name);
            button->setToolTip(tooltip);
            layout->addWidget(button);

            return button;
        };
        QPushButton* linkButton = addButton(tr("Link"), QString::fromUtf8("KnobEditLinkButton"),
                                            tr("Add a parameter linked to the chosen one after the selection."));
        QPushButton* copyButton = addButton(tr("Copy"), QString::fromUtf8("KnobEditLinkCopyButton"),
                                            tr("Add an independent copy of the chosen parameter after the selection."));
        QPushButton* closeButton = addButton(tr("Close"), QString::fromUtf8("KnobEditLinkCloseButton"), QString());
        QObject::connect(nodeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { _imp->fillLinkKnobs(); });
        const auto add = [this](bool copy) {
            if (!_imp->linkKnobCombo || !_imp->linkNodeCombo) {
                return;
            }
            const int nodeIndex = _imp->linkNodeCombo->currentIndex();
            NodePtr n = ((nodeIndex >= 0) && (nodeIndex < (int)_imp->linkNodes.size())) ? _imp->linkNodes[nodeIndex].lock() : NodePtr();
            KnobIPtr source = n ? n->getKnobByName(_imp->linkKnobCombo->currentData().toString().toStdString()) : KnobIPtr();
            if (!source) {
                return;
            }
            KnobIPtr parent;
            int index = -1;
            getInsertionPoint(&parent, &index);
            addKnobFrom(source, copy, parent, index);
        };
        QObject::connect(linkButton, &QPushButton::clicked, this, [add]() { add(false); });
        QObject::connect(copyButton, &QPushButton::clicked, this, [add]() { add(true); });
        QObject::connect(closeButton, &QPushButton::clicked, chooser, &QWidget::hide);
        _imp->linkChooser = chooser;
        _imp->linkNodeCombo = nodeCombo;
        _imp->linkKnobCombo = knobCombo;
        QVBoxLayout* column = panel->getPagesColumnLayout();
        if (column) {
            column->insertWidget(_imp->palette ? column->indexOf(_imp->palette) + 1 : 0, chooser);
        }
    }
    const std::vector<NodePtr> nodes = _imp->linkableNodes();
    _imp->linkNodes.assign(nodes.begin(), nodes.end());
    {
        QSignalBlocker blocker(_imp->linkNodeCombo.data());
        _imp->linkNodeCombo->clear();
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            _imp->linkNodeCombo->addItem(QString::fromUtf8(linkNodeDisplayName(nodes[i]).c_str()), QString::fromUtf8(nodes[i]->getFullyQualifiedName().c_str()));
        }
    }
    _imp->fillLinkKnobs();
    _imp->linkChooser->show();
} // KnobEditMode::showLinkChooser

NATRON_NAMESPACE_EXIT
