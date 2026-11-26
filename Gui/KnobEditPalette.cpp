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

#include "KnobEditPalette.h"

#include <algorithm>
#include <cctype>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QLayout>
#include <QMimeData>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/Knob.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Utils.h" // convertFromPlainText

#include "Gui/GuiDensity.h"
#include "Gui/GuiMacros.h"
#include "Gui/KnobEditMode.h"
#include "Gui/Label.h"

NATRON_NAMESPACE_ENTER

const std::vector<KnobEditTypeEntry>&
knobEditTypeEntries()
{
    static const std::vector<KnobEditTypeEntry> entries = {
        { QString::fromUtf8("Int"), QObject::tr("Int"), KnobInt::typeNameStatic(), 1, false, "int" },
        { QString::fromUtf8("Float"), QObject::tr("Float"), KnobDouble::typeNameStatic(), 1, false, "float" },
        { QString::fromUtf8("Bool"), QObject::tr("Bool"), KnobBool::typeNameStatic(), 1, false, "bool" },
        { QString::fromUtf8("Choice"), QObject::tr("Choice"), KnobChoice::typeNameStatic(), 1, false, "choice" },
        { QString::fromUtf8("Color"), QObject::tr("Color"), KnobColor::typeNameStatic(), 3, false, "color" },
        { QString::fromUtf8("String"), QObject::tr("String"), KnobString::typeNameStatic(), 1, false, "string" },
        { QString::fromUtf8("File"), QObject::tr("File"), KnobFile::typeNameStatic(), 1, false, "file" },
        { QString::fromUtf8("OutputFile"), QObject::tr("Output File"), KnobOutputFile::typeNameStatic(), 1, false, "outputFile" },
        { QString::fromUtf8("Path"), QObject::tr("Path"), KnobPath::typeNameStatic(), 1, false, "path" },
        { QString::fromUtf8("Button"), QObject::tr("Button"), KnobButton::typeNameStatic(), 1, false, "button" },
        { QString::fromUtf8("Curve"), QObject::tr("Curve"), KnobParametric::typeNameStatic(), 1, false, "curve" },
        { QString::fromUtf8("Layer"), QObject::tr("Layer"), KnobLayerSelect::typeNameStatic(), 1, false, "layer" },
        { QString::fromUtf8("ChannelSet"), QObject::tr("Channel Set"), KnobChannelSet::typeNameStatic(), 1, false, "channelSet" },
        { QString::fromUtf8("ChannelSelect"), QObject::tr("Channel Select"), KnobChannelSelect::typeNameStatic(), 1, false, "channelSelect" },
        { QString::fromUtf8("Page"), QObject::tr("Page"), KnobPage::typeNameStatic(), 1, false, "page" },
        { QString::fromUtf8("Group"), QObject::tr("Group"), KnobGroup::typeNameStatic(), 1, false, "group" },
        { QString::fromUtf8("Separator"), QObject::tr("Separator"), KnobSeparator::typeNameStatic(), 1, false, "separator" },
        { QString::fromUtf8("Label"), QObject::tr("Label"), KnobString::typeNameStatic(), 1, true, "label" },
    };

    return entries;
}

std::string
knobEditReadableLabel(const std::string& name)
{
    std::string ret;
    char previous = 0;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (c == '_') {
            if (!ret.empty() && (ret.back() != ' ')) {
                ret.push_back(' ');
            }
            previous = ' ';
            continue;
        }
        const bool startsWord = std::isupper((unsigned char)c) ? (previous && !std::isupper((unsigned char)previous))
            : std::isdigit((unsigned char)c)                   ? (previous && !std::isdigit((unsigned char)previous))
                                                               : (previous && std::isdigit((unsigned char)previous));
        if (startsWord && !ret.empty() && (ret.back() != ' ')) {
            ret.push_back(' ');
        }
        const bool first = ret.empty() || (ret.back() == ' ');
        ret.push_back(first ? (char)std::toupper((unsigned char)c) : c);
        previous = c;
    }

    return ret;
}

const KnobEditTypeEntry*
findKnobEditTypeEntry(const QString& id)
{
    const std::vector<KnobEditTypeEntry>& entries = knobEditTypeEntries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].id == id) {
            return &entries[i];
        }
    }

    return 0;
}

const KnobEditTypeEntry*
findKnobEditTypeEntry(const std::string& typeName,
                      bool isLabel)
{
    const std::vector<KnobEditTypeEntry>& entries = knobEditTypeEntries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const KnobEditTypeEntry& entry = entries[i];
        if ((entry.typeName != typeName) || (entry.isLabel != isLabel)) {
            continue;
        }
        return &entry;
    }

    return 0;
}

NATRON_NAMESPACE_ANONYMOUS_ENTER

// Lays its items out left to right, starting a new row when the next one would not fit.
class PaletteFlowLayout
    : public QLayout {
public:
    explicit PaletteFlowLayout(QWidget* parent)
        : QLayout(parent)
        , _items()
        , _spacing(TO_DPIX(3))
    {
        setContentsMargins(0, 0, 0, 0);
    }

    virtual ~PaletteFlowLayout() OVERRIDE
    {
        while (QLayoutItem* item = takeAt(0)) {
            delete item;
        }
    }

    virtual void addItem(QLayoutItem* item) OVERRIDE FINAL
    {
        _items.append(item);
    }

    virtual int count() const OVERRIDE FINAL
    {
        return _items.size();
    }

    virtual QLayoutItem* itemAt(int index) const OVERRIDE FINAL
    {
        return ((index >= 0) && (index < _items.size())) ? _items[index] : 0;
    }

    virtual QLayoutItem* takeAt(int index) OVERRIDE FINAL
    {
        return ((index >= 0) && (index < _items.size())) ? _items.takeAt(index) : 0;
    }

    virtual Qt::Orientations expandingDirections() const OVERRIDE FINAL
    {
        return Qt::Orientations();
    }

    virtual bool hasHeightForWidth() const OVERRIDE FINAL
    {
        return true;
    }

    virtual int heightForWidth(int width) const OVERRIDE FINAL
    {
        return arrange(QRect(0, 0, width, 0), false);
    }

    virtual void setGeometry(const QRect& rect) OVERRIDE FINAL
    {
        QLayout::setGeometry(rect);
        arrange(rect, true);
    }

    virtual QSize sizeHint() const OVERRIDE FINAL
    {
        return minimumSize();
    }

    virtual QSize minimumSize() const OVERRIDE FINAL
    {
        QSize size;
        for (int i = 0; i < _items.size(); ++i) {
            size = size.expandedTo(_items[i]->minimumSize());
        }

        return size;
    }

private:
    int arrange(const QRect& rect,
                bool apply) const
    {
        int x = rect.x();
        int y = rect.y();
        int rowHeight = 0;
        for (int i = 0; i < _items.size(); ++i) {
            const QSize hint = _items[i]->sizeHint();
            if ((x > rect.x()) && (x + hint.width() > rect.right() + 1)) {
                x = rect.x();
                y += rowHeight + _spacing;
                rowHeight = 0;
            }
            if (apply) {
                _items[i]->setGeometry(QRect(QPoint(x, y), hint));
            }
            x += hint.width() + _spacing;
            rowHeight = std::max(rowHeight, hint.height());
        }

        return y + rowHeight - rect.y();
    }

    QList<QLayoutItem*> _items;
    int _spacing;
};

// A palette entry: a click adds, a drag carries the entry into the panel.
class PaletteButton
    : public QPushButton {
public:
    PaletteButton(const KnobEditMode* mode,
                  const QString& entryId,
                  const QString& text,
                  QWidget* parent)
        : QPushButton(text, parent)
        , _mode(mode)
        , _entryId(entryId)
        , _pressPos()
        , _pressed(false)
    {
    }

private:
    virtual void mousePressEvent(QMouseEvent* e) OVERRIDE FINAL
    {
        if (e->button() == Qt::LeftButton) {
            _pressPos = e->pos();
            _pressed = true;
        }
        QPushButton::mousePressEvent(e);
    }

    virtual void mouseMoveEvent(QMouseEvent* e) OVERRIDE FINAL
    {
        if (!_pressed || (_entryId == QString::fromUtf8(NATRON_KNOB_EDITOR_LINK_ENTRY)) || ((e->pos() - _pressPos).manhattanLength() < QApplication::startDragDistance())) {
            QPushButton::mouseMoveEvent(e);

            return;
        }
        _pressed = false;
        // The drag replaces the click.
        setDown(false);
        QDrag* drag = new QDrag(this);
        QMimeData* mime = new QMimeData;
        mime->setData(QString::fromUtf8(NATRON_KNOB_EDITOR_MIME_TYPE), KnobEditPalette::dragPayload(_mode, _entryId));
        drag->setMimeData(mime);
        drag->setPixmap(grab());
        drag->exec(Qt::CopyAction);
    }

    virtual void mouseReleaseEvent(QMouseEvent* e) OVERRIDE FINAL
    {
        _pressed = false;
        QPushButton::mouseReleaseEvent(e);
    }

    const KnobEditMode* _mode;
    QString _entryId;
    QPoint _pressPos;
    bool _pressed;
};

NATRON_NAMESPACE_ANONYMOUS_EXIT

KnobEditPalette::KnobEditPalette(KnobEditMode* mode,
                                 QWidget* parent)
    : QWidget(parent)
    , _mode(mode)
    , _title(0)
    , _titleText(tr("ADD PARAMETER  (click to add after the selection, or drag into the panel)"))
    , _dropToDelete(false)
{
    setObjectName(QString::fromUtf8("KnobEditPalette"));
    setAcceptDrops(true);
    setToolTip(NATRON_NAMESPACE::convertFromPlainText(tr("Drop a parameter of this panel here to delete it."), NATRON_NAMESPACE::WhiteSpaceNormal));
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(TO_DPIX(4), TO_DPIY(4), TO_DPIX(4), TO_DPIY(4));
    mainLayout->setSpacing(TO_DPIY(3));

    _title = new Label(_titleText, this);
    _title->setObjectName(QString::fromUtf8("KnobEditPaletteTitle"));
    mainLayout->addWidget(_title);

    QWidget* buttons = new QWidget(this);
    PaletteFlowLayout* flow = new PaletteFlowLayout(buttons);
    std::vector<std::pair<QString, QString>> items;
    const std::vector<KnobEditTypeEntry>& entries = knobEditTypeEntries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].id == QString::fromUtf8("Page")) {
            items.push_back(std::make_pair(QString::fromUtf8(NATRON_KNOB_EDITOR_LINK_ENTRY), tr("Link knob...")));
        }
        items.push_back(std::make_pair(entries[i].id, entries[i].label));
    }
    for (std::size_t i = 0; i < items.size(); ++i) {
        const QString id = items[i].first;
        PaletteButton* button = new PaletteButton(mode, id, items[i].second, buttons);
        button->setObjectName(QString::fromUtf8("KnobEditPalette_") + id);
        button->setFocusPolicy(Qt::NoFocus);
        if (id == QString::fromUtf8(NATRON_KNOB_EDITOR_LINK_ENTRY)) {
            button->setToolTip(NATRON_NAMESPACE::convertFromPlainText(tr("Choose a node and one of its parameters to link to from this node."), NATRON_NAMESPACE::WhiteSpaceNormal));
        } else {
            button->setToolTip(NATRON_NAMESPACE::convertFromPlainText(tr("Add a %1 parameter after the selection, or drag it to a position in the panel.").arg(items[i].second), NATRON_NAMESPACE::WhiteSpaceNormal));
        }
        QObject::connect(button, &QPushButton::clicked, this, [this, id]() {
            Q_EMIT entryClicked(id);
        });
        flow->addWidget(button);
    }
    mainLayout->addWidget(buttons);
}

KnobEditPalette::~KnobEditPalette()
{
}

QByteArray
KnobEditPalette::dragPayload(const KnobEditMode* mode,
                             const QString& entryId)
{
    return KnobEditMode::dragPayload(mode, QString::fromUtf8("new"), entryId);
}

bool
KnobEditPalette::isDropToDeleteShown() const
{
    return _dropToDelete;
}

KnobIPtr
KnobEditPalette::draggedKnob(const QMimeData* mimeData) const
{
    const QString format = QString::fromUtf8(NATRON_KNOB_EDITOR_MIME_TYPE);
    if (!_mode || !mimeData || !mimeData->hasFormat(format)) {
        return KnobIPtr();
    }
    QString kind;
    QString value;
    if (!_mode->parseDragPayload(mimeData->data(format), &kind, &value) || (kind != QString::fromUtf8("move"))) {
        return KnobIPtr();
    }
    KnobHolder* holder = _mode->getHolder();
    KnobIPtr knob = holder ? holder->getKnobByName(value.toStdString()) : KnobIPtr();

    return _mode->isEditable(knob) ? knob : KnobIPtr();
}

void
KnobEditPalette::setDropToDelete(const KnobIPtr& knob)
{
    const bool show = (bool)knob;
    if (show) {
        const std::string label = knob->getLabel().empty() ? knob->getName() : knob->getLabel();
        _title->setText(tr("DROP TO DELETE %1").arg(QString::fromUtf8(label.c_str())));
    } else {
        _title->setText(_titleText);
    }
    if (show != _dropToDelete) {
        _dropToDelete = show;
        update();
    }
}

void
KnobEditPalette::paintEvent(QPaintEvent* e)
{
    QWidget::paintEvent(e);
    if (!_dropToDelete) {
        return;
    }
    const QColor deleteColor(230, 70, 70);
    QColor fill = deleteColor;
    fill.setAlpha(50);
    const qreal penWidth = 2;
    const qreal radius = TO_DPIX(4);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(deleteColor, penWidth));
    p.setBrush(fill);
    p.drawRoundedRect(QRectF(rect()).adjusted(penWidth / 2, penWidth / 2, -penWidth / 2, -penWidth / 2), radius, radius);
}

void
KnobEditPalette::dragEnterEvent(QDragEnterEvent* e)
{
    KnobIPtr knob = draggedKnob(e->mimeData());
    setDropToDelete(knob);
    if (knob) {
        e->acceptProposedAction();
    } else {
        e->ignore();
    }
}

void
KnobEditPalette::dragMoveEvent(QDragMoveEvent* e)
{
    KnobIPtr knob = draggedKnob(e->mimeData());
    setDropToDelete(knob);
    if (knob) {
        e->acceptProposedAction();
    } else {
        e->ignore();
    }
}

void
KnobEditPalette::dragLeaveEvent(QDragLeaveEvent* e)
{
    setDropToDelete(KnobIPtr());
    e->accept();
}

void
KnobEditPalette::dropEvent(QDropEvent* e)
{
    KnobIPtr knob = draggedKnob(e->mimeData());
    setDropToDelete(KnobIPtr());
    if (!knob) {
        e->ignore();

        return;
    }
    e->acceptProposedAction();
    QPointer<KnobEditMode> mode(_mode);
    const KnobIWPtr knobW = knob;
    // The drag that brought the knob is still running from the knob's overlay, which the deletion
    // rebuilds.
    QTimer::singleShot(0, _mode, [mode, knobW]() {
        KnobIPtr toDelete = knobW.lock();
        if (mode && toDelete) {
            mode->deleteKnob(toDelete);
        }
    });
}

NATRON_NAMESPACE_EXIT
