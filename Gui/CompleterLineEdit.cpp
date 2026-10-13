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

#include "CompleterLineEdit.h"

#include <stdexcept>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QAbstractItemView>
#include <QApplication>
#include <QKeyEvent>
#include <QListView>
#include <QRegularExpression>
#include <QStringList>
#include <QStringListModel>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppManager.h"
#include "Engine/Plugin.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/LineEdit.h"

/*
   If defined, the list will contain matches ordered by increasing match length of the regexp
   otherwise they will be ordered with their score
 */
// #define NODE_TAB_DIALOG_USE_MATCHED_LENGTH

NATRON_NAMESPACE_ENTER

struct CompleterLineEditPrivate {
    QDialog* dialog;
    QListView* listView;
    QStringListModel* model;
    CompleterLineEdit::PluginsNamesMap names;
    bool quickExitEnabled;

    CompleterLineEditPrivate(const CompleterLineEdit::PluginsNamesMap& plugins,
                             bool quickExit,
                             QDialog* parent)
        : dialog(parent)
        , listView(NULL)
        , model(NULL)
        , names(plugins)
        , quickExitEnabled(quickExit)
    {
    }

    CompleterLineEditPrivate(const QStringList& displayWords,
                             const QStringList& internalIds,
                             bool quickExit,
                             QDialog* parent)
        : dialog(parent)
        , listView(NULL)
        , model(NULL)
        , quickExitEnabled(quickExit)
    {
        assert(displayWords.size() == internalIds.size());

        for (int i = 0; i < displayWords.size(); ++i) {
            names.insert(std::make_pair(0, std::make_pair(internalIds[i], displayWords[i])));
        }
    }
};

CompleterLineEdit::CompleterLineEdit(const PluginsNamesMap& plugins,
                                     bool quickExit,
                                     QDialog* parent)
    : LineEdit(parent)
    , _imp(new CompleterLineEditPrivate(plugins, quickExit, parent))
{
    _imp->listView = new QListView(parent);
    _imp->model = new QStringListModel(this);
    _imp->listView->setWindowFlags(Qt::ToolTip);
    _imp->listView->setModel(_imp->model);

    connect(this, SIGNAL(textChanged(QString)), this, SLOT(filterText(QString)));
    connect(_imp->listView, SIGNAL(clicked(QModelIndex)), this, SLOT(setTextFromIndex(QModelIndex)));
}

CompleterLineEdit::CompleterLineEdit(const QStringList& displayWords,
                                     const QStringList& internalIds,
                                     bool quickExit,
                                     QDialog* parent)
    : LineEdit(parent)
    , _imp(new CompleterLineEditPrivate(displayWords, internalIds, quickExit, parent))
{
    _imp->listView = new QListView(parent);
    _imp->model = new QStringListModel(this);
    _imp->listView->setWindowFlags(Qt::ToolTip);
    _imp->listView->setModel(_imp->model);

    connect(this, SIGNAL(textChanged(QString)), this, SLOT(filterText(QString)));
    connect(_imp->listView, SIGNAL(clicked(QModelIndex)), this, SLOT(setTextFromIndex(QModelIndex)));
}

CompleterLineEdit::~CompleterLineEdit()
{
}

QListView*
CompleterLineEdit::getView() const
{
    return _imp->listView;
}

void
CompleterLineEdit::filterText(const QString& txt)
{
    QStringList sl;

    if (txt.isEmpty()) {
        for (PluginsNamesMap::iterator it = _imp->names.begin(); it != _imp->names.end(); ++it) {
            sl.push_front(it->second.second);
        }
    } else {
        QString pattern;
        for (int i = 0; i < txt.size(); ++i) {
            pattern.push_back(QLatin1Char('*'));
            pattern.push_back(txt[i]);
        }
        pattern.push_back(QLatin1Char('*'));
        // The pattern is wildcarded at both ends, so the anchored conversion
        // matches exactly what exactMatch() and contains() did.
        QRegularExpression expr(QRegularExpression::wildcardToRegularExpression(pattern),
                                QRegularExpression::CaseInsensitiveOption);

#ifdef NODE_TAB_DIALOG_USE_MATCHED_LENGTH
        std::map<int, QStringList> matchOrdered;
        for (PluginsNamesMap::iterator it = _imp->names.begin(); it != _imp->names.end(); ++it) {
            QRegularExpressionMatch match = expr.match(it->second.first);
            if (match.hasMatch()) {
                QStringList& matchedForLength = matchOrdered[match.capturedLength()];
                matchedForLength.push_front(it->second.second);
            }
        }
        for (std::map<int, QStringList>::iterator it = matchOrdered.begin(); it != matchOrdered.end(); ++it) {
            sl.append(it->second);
        }
#else

        for (PluginsNamesMap::iterator it = _imp->names.begin(); it != _imp->names.end(); ++it) {
            if (it->second.first.contains(expr)) {
                sl.push_front(it->second.second);
            }
        }
#endif
    }
    _imp->model->setStringList(sl);
    _imp->listView->setModel(_imp->model);

    int rowCount = _imp->model->rowCount();
    if (rowCount == 0) {
        return;
    }

    QPoint p = mapToGlobal(QPoint(0, height()));
    // QDesktopWidget* desktop = QApplication::desktop();
    // QRect screen = desktop->screenGeometry();
    // double maxHeight = ( screen.height() - p.y() ) * 0.8;
    // QFontMetrics fm = _imp->listView->fontMetrics();
    // maxHeight = std::min( maxHeight, ( rowCount * fm.height() * 1.2 + fm.height() ) );

    // Position the text edit
    //   _imp->listView->setFixedSize(width(),maxHeight);

    _imp->listView->move(p);
    _imp->listView->show();
} // CompleterLineEdit::filterText

void
CompleterLineEdit::setTextFromIndex(const QModelIndex& index)
{
    QString text = index.data().toString();

    setText(text);
    Q_EMIT itemCompletionChosen();
    _imp->listView->hide();
    if (_imp->quickExitEnabled && _imp->dialog) {
        _imp->dialog->accept();
    }
}

void
CompleterLineEdit::keyPressEvent(QKeyEvent* e)
{
    int key = e->key();
    bool viewVisible = !_imp->listView->isHidden();

    assert(_imp->listView->model());
    int count = _imp->listView->model()->rowCount();
    QModelIndex currentIndex = _imp->listView->currentIndex();

    if (key == Qt::Key_Escape) {
        if (_imp->quickExitEnabled) {
            if (_imp->dialog) {
                _imp->dialog->accept();
            }
        } else {
            if (_imp->listView->isVisible()) {
                _imp->listView->hide();
            } else {
                _imp->dialog->reject();
            }
        }
        e->accept();
    } else if (key == Qt::Key_Down) {
        if (viewVisible) {
            int row = currentIndex.row() + 1;

            /// Handle circular selection
            if (row >= count) {
                row = 0;
            }

            QModelIndex index = _imp->listView->model()->index(row, 0);
            _imp->listView->setCurrentIndex(index);
        }
    } else if (key == Qt::Key_Up) {
        if (viewVisible) {
            int row = currentIndex.row() - 1;

            /// Handle circular selection
            if (row < 0) {
                row = count - 1;
            }

            QModelIndex index = _imp->listView->model()->index(row, 0);
            _imp->listView->setCurrentIndex(index);
        }
    } else if ((key == Qt::Key_Enter) || (key == Qt::Key_Return)) {
        _imp->listView->hide();
        if (_imp->model->rowCount() == 1) {
            setText(_imp->model->index(0).data().toString());
            Q_EMIT itemCompletionChosen();
            if (_imp->quickExitEnabled && _imp->dialog) {
                _imp->dialog->accept();
            }
            e->accept();
        } else {
            const QItemSelection selection = _imp->listView->selectionModel()->selection();
            QModelIndexList indexes = selection.indexes();
            bool doDialogEnd = false;
            if (indexes.size() == 1) {
                setText(_imp->model->index(indexes[0].row()).data().toString());
                doDialogEnd = true;
            } else if (indexes.isEmpty()) {
                if (_imp->model->rowCount() > 1) {
                    doDialogEnd = true;
                    setText(_imp->model->index(0).data().toString());
                }
            }
            if (doDialogEnd) {
                Q_EMIT itemCompletionChosen();
                if (_imp->quickExitEnabled && _imp->dialog) {
                    _imp->dialog->accept();
                }
                e->accept();
            } else {
                QLineEdit::keyPressEvent(e);
            }
        }
    } else {
        QLineEdit::keyPressEvent(e);
    }
} // keyPressEvent

void
CompleterLineEdit::showCompleter()
{
    _imp->listView->setFixedWidth(width());
    filterText(text());
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_CompleterLineEdit.cpp"
