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

#include "KnobDriverRow.h"

#include <algorithm>
#include <cmath>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QApplication>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>
#include <QtCore/QMetaObject>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Loop/LoopEnd.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/RenderContext.h"

#include "Gui/ComboBox.h"
#include "Gui/Gui.h"
#include "Gui/GuiMacros.h"
#include "Gui/KnobGui.h"
#include "Gui/Label.h"
#include "Gui/SpinBox.h"
#include "Gui/UndoHistory.h"

NATRON_NAMESPACE_ENTER

namespace {
const char* const kLoopVariables[] = { "t", "iteration", "count", "frame" };
const int kSetKnobDriverCommandId = 0x4c445256;
const double kPointPickRadius = 6.;

// The easing choices, in their order in the easing menus.
const KnobDriver::EasingEnum kEasings[] = { KnobDriver::eEasingLinear, KnobDriver::eEasingSmooth, KnobDriver::eEasingCurve };
} // anonymous namespace

//////////////////////////////// SetKnobDriverCommand

SetKnobDriverCommand::SetKnobDriverCommand(const KnobIPtr& knob,
                                           const std::vector<KnobDriverPtr>& drivers,
                                           const QString& text,
                                           quint64 gesture)
    : QUndoCommand(text)
    , _knob(knob)
    , _oldDrivers(LoopDriverEdit::driversOf(knob))
    , _newDrivers(drivers)
    , _gesture(gesture)
{
    _newDrivers.resize(_oldDrivers.size());
}

void
SetKnobDriverCommand::apply(const std::vector<KnobDriverPtr>& drivers)
{
    KnobIPtr knob = _knob.lock();
    KnobHelper* helper = dynamic_cast<KnobHelper*>(knob.get());

    if (!helper) {
        return;
    }
    const int n = std::min((int)drivers.size(), knob->getDimension());
    for (int i = 0; i < n; ++i) {
        if (helper->getDriver(i) != drivers[i]) {
            helper->setDriver(i, drivers[i]);
        }
    }
}

void
SetKnobDriverCommand::undo()
{
    apply(_oldDrivers);
}

void
SetKnobDriverCommand::redo()
{
    apply(_newDrivers);
}

int
SetKnobDriverCommand::id() const
{
    return kSetKnobDriverCommandId;
}

bool
SetKnobDriverCommand::mergeWith(const QUndoCommand* other)
{
    const SetKnobDriverCommand* command = dynamic_cast<const SetKnobDriverCommand*>(other);

    if (!command || (_gesture == 0) || (command->_gesture != _gesture) || (command->_knob.lock() != _knob.lock())) {
        return false;
    }
    _newDrivers = command->_newDrivers;

    return true;
}

void
SetKnobDriverCommand::push(Gui* gui,
                           const KnobIPtr& knob,
                           const std::vector<KnobDriverPtr>& drivers,
                           const QString& text,
                           quint64 gesture)
{
    if (!knob) {
        return;
    }
    SetKnobDriverCommand* command = new SetKnobDriverCommand(knob, drivers, text, gesture);
    UndoHistory* history = gui ? gui->getUndoHistory() : 0;
    if (!history) {
        command->redo();
        delete command;

        return;
    }
    history->push(command, UndoScope(UndoScope::holderForKnob(knob)));
}

//////////////////////////////// LoopDriverEdit

namespace LoopDriverEdit {
NodePtr
nodeOf(const KnobIPtr& knob)
{
    EffectInstance* effect = knob ? dynamic_cast<EffectInstance*>(knob->getHolder()) : 0;

    return effect ? effect->getNode() : NodePtr();
}

QStringList
loopVariables(const NodePtr& node)
{
    QStringList variables;
    const std::vector<NodePtr> ends = LoopEnd::enclosingLoopEnds(node);

    if (ends.empty()) {
        return variables;
    }
    for (const char* name : kLoopVariables) {
        variables.push_back(QString::fromUtf8("loop.") + QString::fromUtf8(name));
    }
    for (const NodePtr& end : ends) {
        const LoopEnd* loopEnd = dynamic_cast<const LoopEnd*>(end->getEffectInstance().get());
        const std::string loopName = loopEnd ? loopEnd->getLoopName() : std::string();
        if (loopName.empty()) {
            continue;
        }
        for (const char* name : kLoopVariables) {
            variables.push_back(QString::fromUtf8("loops.%1.%2").arg(QString::fromUtf8(loopName.c_str()), QString::fromUtf8(name)));
        }
    }

    return variables;
}

bool
canDrive(const KnobIPtr& knob)
{
    const KnobHelper* helper = dynamic_cast<const KnobHelper*>(knob.get());

    return helper && helper->canHaveDriver() && nodeOf(knob);
}

std::vector<KnobDriverPtr>
driversOf(const KnobIPtr& knob)
{
    std::vector<KnobDriverPtr> drivers;
    const KnobHelper* helper = dynamic_cast<const KnobHelper*>(knob.get());

    if (!helper) {
        return drivers;
    }
    for (int i = 0; i < knob->getDimension(); ++i) {
        drivers.push_back(helper->getDriver(i));
    }

    return drivers;
}

bool
isDriven(const KnobIPtr& knob,
         int dimension)
{
    const KnobHelper* helper = dynamic_cast<const KnobHelper*>(knob.get());

    if (!helper || (dimension >= knob->getDimension())) {
        return false;
    }

    return (dimension < 0) ? helper->hasDrivers() : bool(helper->getDriver(dimension));
}

bool
isStale(const KnobDriverPtr& driver,
        const QStringList& variables)
{
    // A plain name is a project or rfv variable, which no loop has to enclose.
    return driver && !KnobDriver::parseVariable(driver->getVariable()).ns.empty() && !variables.contains(QString::fromUtf8(driver->getVariable().c_str()));
}

double
previewValue(const KnobIPtr& knob,
             int dimension)
{
    KnobDoubleBase* isDouble = dynamic_cast<KnobDoubleBase*>(knob.get());
    KnobIntBase* isInt = dynamic_cast<KnobIntBase*>(knob.get());

    if (!isDouble && !isInt) {
        return 0.;
    }
    const double time = knob->getCurrentTime();
    const RenderContextPtr context = LoopEnd::previewContextOf(nodeOf(knob), time);
    ActionContextScope scope(bool(context), context);

    return isDouble ? isDouble->getValueAtTime(time, dimension) : (double)isInt->getValueAtTime(time, dimension);
}

KnobDriverPtr
makeDriver(const std::string& variable,
           double start,
           double end,
           KnobDriver::EasingEnum easing,
           const Curve* curve)
{
    return std::make_shared<const KnobDriver>(variable, start, end, easing, curve);
}

KnobDriverPtr
applyChange(const KnobDriverPtr& driver,
            const DriverChange& change)
{
    if (!driver) {
        return driver;
    }
    std::string variable = driver->getVariable();
    double start = driver->getStart();
    double end = driver->getEnd();
    KnobDriver::EasingEnum easing = driver->getEasing();
    const Curve* curve = driver->getCurve();
    switch (change.field) {
    case DriverChange::eFieldVariable:
        variable = change.variable;
        break;
    case DriverChange::eFieldStart:
        start = change.value;
        break;
    case DriverChange::eFieldEnd:
        end = change.value;
        break;
    case DriverChange::eFieldEasing:
        easing = change.easing;
        if (change.curve) {
            curve = change.curve.get();
        }
        break;
    }

    return makeDriver(variable, start, end, easing, curve);
}

void
editDrivers(Gui* gui,
            const KnobIPtr& knob,
            int dimension,
            const DriverChange& change,
            quint64 gesture)
{
    std::vector<KnobDriverPtr> drivers = driversOf(knob);
    bool changed = false;

    for (int i = 0; i < (int)drivers.size(); ++i) {
        if (drivers[i] && ((dimension < 0) || (dimension == i))) {
            drivers[i] = applyChange(drivers[i], change);
            changed = true;
        }
    }
    if (changed) {
        SetKnobDriverCommand::push(gui, knob, drivers, QObject::tr("Edit loop driver of %1").arg(knobLabel(knob, dimension)), gesture);
    }
}

bool
driveFromLoop(Gui* gui,
              const KnobIPtr& knob,
              int dimension)
{
    if (!canDrive(knob) || loopVariables(nodeOf(knob)).empty()) {
        return false;
    }
    bool hasOtherSource = false;
    for (int i = 0; i < knob->getDimension(); ++i) {
        if ((dimension < 0) || (dimension == i)) {
            hasOtherSource = hasOtherSource || !knob->getExpression(i).empty() || knob->isAnimated(i);
        }
    }
    if (hasOtherSource) {
        const QString question = QObject::tr("%1 has an expression or animation. The loop driver takes over from it "
                                             "while it is set, and removing the driver restores it. Drive it from "
                                             "the loop?")
                                     .arg(knobLabel(knob, dimension));
        if (Dialogs::questionDialog(QObject::tr("Drive from loop").toStdString(), question.toStdString(), false) != eStandardButtonYes) {
            return false;
        }
    }

    KnobDoubleBase* isDouble = dynamic_cast<KnobDoubleBase*>(knob.get());
    KnobIntBase* isInt = dynamic_cast<KnobIntBase*>(knob.get());
    std::vector<KnobDriverPtr> drivers = driversOf(knob);
    for (int i = 0; i < (int)drivers.size(); ++i) {
        if ((dimension < 0) || (dimension == i)) {
            const double start = isDouble ? isDouble->getDefaultValue(i) : (double)isInt->getDefaultValue(i);
            const double end = isDouble ? isDouble->getValue(i) : (double)isInt->getValue(i);
            drivers[i] = makeDriver(kKnobDriverDefaultVariable, start, end, KnobDriver::eEasingLinear, NULL);
        }
    }
    SetKnobDriverCommand::push(gui, knob, drivers, QObject::tr("Drive %1 from loop").arg(knobLabel(knob, dimension)));

    return true;
}

void
removeLoopDriver(Gui* gui,
                 const KnobIPtr& knob,
                 int dimension)
{
    std::vector<KnobDriverPtr> drivers = driversOf(knob);
    bool changed = false;

    for (int i = 0; i < (int)drivers.size(); ++i) {
        if (drivers[i] && ((dimension < 0) || (dimension == i))) {
            drivers[i].reset();
            changed = true;
        }
    }
    if (changed) {
        SetKnobDriverCommand::push(gui, knob, drivers, QObject::tr("Remove loop driver of %1").arg(knobLabel(knob, dimension)));
    }
}

QString
knobLabel(const KnobIPtr& knob,
          int dimension)
{
    QString label = QString::fromUtf8(knob->getName().c_str());

    if ((dimension >= 0) && (knob->getDimension() > 1)) {
        label += QString::fromUtf8(".") + QString::fromUtf8(knob->getDimensionName(dimension).c_str());
    }

    return label;
}
} // namespace LoopDriverEdit

//////////////////////////////// EasingCurveWidget

EasingCurveWidget::EasingCurveWidget(QWidget* parent)
    : QWidget(parent)
    , _points()
    , _dragged(-1)
{
    setObjectName(QString::fromUtf8("EasingCurveWidget"));
    setMinimumSize(TO_DPIX(200), TO_DPIY(200));
    setPoints(pointsOf(NULL));
}

void
EasingCurveWidget::setPoints(const std::vector<QPointF>& points)
{
    _points = points;
    std::sort(_points.begin(), _points.end(), [](const QPointF& a, const QPointF& b) {
        return a.x() < b.x();
    });
    if (_points.size() < 2) {
        _points = pointsOf(NULL);
    }
    update();
}

std::vector<QPointF>
EasingCurveWidget::pointsOf(const Curve* curve)
{
    std::vector<QPointF> points;

    if (curve) {
        const KeyFrameSet keys = curve->getKeyFrames_mt_safe();
        for (KeyFrameSet::const_iterator it = keys.begin(); it != keys.end(); ++it) {
            points.push_back(QPointF(it->getTime(), it->getValue()));
        }
    }
    if (points.size() < 2) {
        points.clear();
        points.push_back(QPointF(0., 0.));
        points.push_back(QPointF(1., 1.));
    }

    return points;
}

std::shared_ptr<Curve>
EasingCurveWidget::makeCurve() const
{
    std::shared_ptr<Curve> curve = std::make_shared<Curve>();

    for (const QPointF& p : _points) {
        curve->addKeyFrame(KeyFrame(p.x(), p.y(), 0., 0., eKeyframeTypeCatmullRom));
    }

    return curve;
}

QSize
EasingCurveWidget::sizeHint() const
{
    return QSize(TO_DPIX(260), TO_DPIY(260));
}

QRectF
EasingCurveWidget::plotRect() const
{
    const double margin = TO_DPIX(12);

    return QRectF(margin, margin, std::max(1., width() - 2 * margin), std::max(1., height() - 2 * margin));
}

QPointF
EasingCurveWidget::toWidget(const QPointF& p) const
{
    const QRectF r = plotRect();

    return QPointF(r.left() + p.x() * r.width(), r.bottom() - p.y() * r.height());
}

QPointF
EasingCurveWidget::toCurve(const QPointF& p) const
{
    const QRectF r = plotRect();

    return QPointF((p.x() - r.left()) / r.width(), (r.bottom() - p.y()) / r.height());
}

int
EasingCurveWidget::pointAt(const QPointF& pos) const
{
    for (std::size_t i = 0; i < _points.size(); ++i) {
        const QPointF d = toWidget(_points[i]) - pos;
        if ((std::abs(d.x()) <= kPointPickRadius) && (std::abs(d.y()) <= kPointPickRadius)) {
            return (int)i;
        }
    }

    return -1;
}

void
EasingCurveWidget::paintEvent(QPaintEvent* /*e*/)
{
    QPainter p(this);

    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), palette().base());
    const QRectF r = plotRect();
    p.setPen(QPen(palette().mid().color(), 1., Qt::DotLine));
    for (int i = 1; i < 4; ++i) {
        const double f = i / 4.;
        p.drawLine(QPointF(r.left() + f * r.width(), r.top()), QPointF(r.left() + f * r.width(), r.bottom()));
        p.drawLine(QPointF(r.left(), r.top() + f * r.height()), QPointF(r.right(), r.top() + f * r.height()));
    }
    p.setPen(QPen(palette().mid().color(), 1.));
    p.drawRect(r);

    const std::shared_ptr<Curve> curve = makeCurve();
    QPainterPath path;
    const int samples = std::max(2, (int)r.width() / 2);
    for (int i = 0; i <= samples; ++i) {
        const double t = (double)i / samples;
        const QPointF w = toWidget(QPointF(t, curve->getValueAt(t, false)));
        if (i == 0) {
            path.moveTo(w);
        } else {
            path.lineTo(w);
        }
    }
    p.setPen(QPen(palette().highlight().color(), 2.));
    p.drawPath(path);

    p.setPen(QPen(palette().text().color(), 1.));
    p.setBrush(palette().button());
    for (const QPointF& point : _points) {
        const QPointF w = toWidget(point);
        p.drawRect(QRectF(w.x() - 3., w.y() - 3., 6., 6.));
    }
}

void
EasingCurveWidget::mousePressEvent(QMouseEvent* e)
{
    const int index = pointAt(e->pos());

    if (e->button() == Qt::RightButton) {
        // The end points stay: the curve always spans [0, 1].
        if ((index > 0) && (index < (int)_points.size() - 1)) {
            _points.erase(_points.begin() + index);
            update();
            Q_EMIT pointsChanged();
        }

        return;
    }
    _dragged = index;
}

void
EasingCurveWidget::mouseMoveEvent(QMouseEvent* e)
{
    if ((_dragged < 0) || (_dragged >= (int)_points.size())) {
        return;
    }
    QPointF p = toCurve(e->pos());
    const int last = (int)_points.size() - 1;
    if ((_dragged == 0) || (_dragged == last)) {
        p.setX(_points[_dragged].x());
    } else {
        // A point stays between its neighbours, so the curve stays a function of t.
        const double gap = 1e-3;
        p.setX(std::max(_points[_dragged - 1].x() + gap, std::min(_points[_dragged + 1].x() - gap, p.x())));
    }
    _points[_dragged] = p;
    update();
    Q_EMIT pointsChanged();
}

void
EasingCurveWidget::mouseReleaseEvent(QMouseEvent* /*e*/)
{
    _dragged = -1;
}

void
EasingCurveWidget::mouseDoubleClickEvent(QMouseEvent* e)
{
    const QPointF p = toCurve(e->pos());

    if ((p.x() <= 0.) || (p.x() >= 1.) || (pointAt(e->pos()) >= 0)) {
        return;
    }
    _points.push_back(p);
    setPoints(_points);
    Q_EMIT pointsChanged();
}

//////////////////////////////// EasingCurveDialog

EasingCurveDialog::EasingCurveDialog(const Curve* curve,
                                     QWidget* parent)
    : QDialog(parent)
    , _editor(0)
{
    setObjectName(QString::fromUtf8("EasingCurveDialog"));
    setWindowTitle(tr("Easing Curve"));
    QVBoxLayout* layout = new QVBoxLayout(this);
    Label* hint = new Label(tr("Drag a point to move it, double-click to add one, right-click to remove one."), this);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    _editor = new EasingCurveWidget(this);
    _editor->setPoints(EasingCurveWidget::pointsOf(curve));
    layout->addWidget(_editor, 1);
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, this);
    QObject::connect(buttons, SIGNAL(accepted()), this, SLOT(accept()));
    QObject::connect(buttons, SIGNAL(rejected()), this, SLOT(reject()));
    layout->addWidget(buttons);
}

std::shared_ptr<Curve>
EasingCurveDialog::makeCurve() const
{
    return _editor->makeCurve();
}

//////////////////////////////// KnobDriverRow

KnobDriverRow::KnobDriverRow(const KnobGuiPtr& knobGui,
                             QWidget* parent)
    : QWidget(parent)
    , _knobGui(knobGui)
    , _knob(knobGui->getKnob())
    , _driving(false)
    , _refreshPending(false)
    , _scope(-1)
    , _scopes()
    , _watchedLoopEnds()
    , _variables()
    , _variable(0)
    , _dimension(0)
    , _start(0)
    , _end(0)
    , _easing(0)
    , _editCurve(0)
    , _preview(0)
{
    setObjectName(QString::fromUtf8("KnobDriverRow"));
    KnobIPtr knob = _knob.lock();
    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(TO_DPIX(3));

    _variable = new ComboBox(this);
    _variable->setObjectName(QString::fromUtf8("driverVariable"));
    _variable->setToolTip(tr("The loop variable that drives the parameter: loop.* are the nearest loop's, "
                             "loops.<name>.* those of the loop named name. t goes from 0 at the first "
                             "iteration to 1 at the last."));
    QObject::connect(_variable, SIGNAL(currentIndexChanged(int)), this, SLOT(onVariableChanged(int)));
    layout->addWidget(_variable);

    _dimension = new ComboBox(this);
    _dimension->setObjectName(QString::fromUtf8("driverDimension"));
    _dimension->setToolTip(tr("The dimensions the row shows and edits: All, when every dimension is driven, "
                              "or one of them."));
    QObject::connect(_dimension, SIGNAL(currentIndexChanged(int)), this, SLOT(onDimensionChanged(int)));
    layout->addWidget(_dimension);

    _start = new SpinBox(this, SpinBox::eSpinBoxTypeDouble);
    _start->setObjectName(QString::fromUtf8("driverStart"));
    _start->setToolTip(tr("The value at t = 0."));
    QObject::connect(_start, SIGNAL(valueChanged(double)), this, SLOT(onStartChanged(double)));
    layout->addWidget(_start);

    Label* arrow = new Label(QString(QChar(0x2192)), this);
    layout->addWidget(arrow);

    _end = new SpinBox(this, SpinBox::eSpinBoxTypeDouble);
    _end->setObjectName(QString::fromUtf8("driverEnd"));
    _end->setToolTip(tr("The value at t = 1."));
    QObject::connect(_end, SIGNAL(valueChanged(double)), this, SLOT(onEndChanged(double)));
    layout->addWidget(_end);

    _easing = new ComboBox(this);
    _easing->setObjectName(QString::fromUtf8("driverEasing"));
    _easing->setToolTip(tr("How the value goes from Start to End: Linear goes on past them when t leaves "
                           "0..1, Smooth eases in and out, Curve follows a curve you draw."));
    _easing->addItem(tr("Linear"));
    _easing->addItem(tr("Smooth"));
    _easing->addItem(tr("Curve"));
    QObject::connect(_easing, SIGNAL(currentIndexChanged(int)), this, SLOT(onEasingChanged(int)));
    layout->addWidget(_easing);

    _editCurve = new QPushButton(QString::fromUtf8("..."), this);
    _editCurve->setObjectName(QString::fromUtf8("driverEditCurve"));
    _editCurve->setToolTip(tr("Edit the easing curve."));
    _editCurve->setFixedWidth(TO_DPIX(24));
    QObject::connect(_editCurve, SIGNAL(clicked()), this, SLOT(onEditCurveClicked()));
    layout->addWidget(_editCurve);

    _preview = new Label(this);
    _preview->setObjectName(QString::fromUtf8("driverPreview"));
    _preview->setToolTip(tr("The value at the iteration the LoopEnd's Single Pass Iteration names."));
    layout->addWidget(_preview);
    layout->addStretch();

    KnobHelper* helper = dynamic_cast<KnobHelper*>(knob.get());
    if (helper && helper->getSignalSlotHandler()) {
        KnobSignalSlotHandler* handler = helper->getSignalSlotHandler().get();
        QObject::connect(handler, SIGNAL(driverChanged(int)), this, SLOT(scheduleRefresh()));
        QObject::connect(handler, SIGNAL(valueChanged(ViewSpec, int, int)), this, SLOT(refreshPreview()));
        QObject::connect(handler, SIGNAL(expressionChanged(int)), this, SLOT(refreshPreview()));
    }
    // The knob's own widgets, which carry its menu, are hidden while the row shows.
    setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(this, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(onContextMenuRequested(QPoint)));
    setVisible(false);
}

KnobDriverRow::~KnobDriverRow()
{
}

void
KnobDriverRow::scheduleRefresh()
{
    if (_refreshPending) {
        return;
    }
    _refreshPending = true;
    QMetaObject::invokeMethod(this, "refresh", Qt::QueuedConnection);
}

void
KnobDriverRow::connectLoopEnds(const NodePtr& node)
{
    for (const NodeWPtr& weak : _watchedLoopEnds) {
        NodePtr end = weak.lock();
        if (!end) {
            continue;
        }
        const std::vector<KnobIPtr>& knobs = end->getKnobs();
        for (const KnobIPtr& knob : knobs) {
            KnobHelper* helper = dynamic_cast<KnobHelper*>(knob.get());
            if (helper && helper->getSignalSlotHandler()) {
                QObject::disconnect(helper->getSignalSlotHandler().get(), 0, this, 0);
            }
        }
    }
    _watchedLoopEnds.clear();
    // Every knob of a LoopEnd may move the preview iteration or the variables a body reads.
    const std::vector<NodePtr> ends = LoopEnd::enclosingLoopEnds(node);
    for (const NodePtr& end : ends) {
        _watchedLoopEnds.push_back(end);
        const std::vector<KnobIPtr>& knobs = end->getKnobs();
        for (const KnobIPtr& knob : knobs) {
            KnobHelper* helper = dynamic_cast<KnobHelper*>(knob.get());
            if (helper && helper->getSignalSlotHandler()) {
                QObject::connect(helper->getSignalSlotHandler().get(), SIGNAL(valueChanged(ViewSpec, int, int)), this, SLOT(scheduleRefresh()));
            }
        }
    }
}

KnobDriverPtr
KnobDriverRow::currentDriver() const
{
    const std::vector<KnobDriverPtr> drivers = LoopDriverEdit::driversOf(_knob.lock());

    if ((_scope >= 0) && (_scope < (int)drivers.size()) && drivers[_scope]) {
        return drivers[_scope];
    }
    for (const KnobDriverPtr& driver : drivers) {
        if (driver) {
            return driver;
        }
    }

    return KnobDriverPtr();
}

void
KnobDriverRow::edit(const LoopDriverEdit::DriverChange& change,
                    quint64 gesture)
{
    KnobGuiPtr knobGui = _knobGui.lock();

    LoopDriverEdit::editDrivers(knobGui ? knobGui->getGui() : 0, _knob.lock(), _scope, change, gesture);
}

void
KnobDriverRow::refresh()
{
    _refreshPending = false;
    KnobIPtr knob = _knob.lock();
    KnobGuiPtr knobGui = _knobGui.lock();
    if (!knob || !knobGui) {
        return;
    }
    const NodePtr node = LoopDriverEdit::nodeOf(knob);
    connectLoopEnds(node);
    _variables = LoopDriverEdit::loopVariables(node);

    const std::vector<KnobDriverPtr> drivers = LoopDriverEdit::driversOf(knob);
    std::vector<int> scopes;
    bool all = drivers.size() > 1;
    for (int i = 0; i < (int)drivers.size(); ++i) {
        if (drivers[i]) {
            scopes.push_back(i);
        } else {
            all = false;
        }
    }
    const bool driving = !scopes.empty();
    if (driving != _driving) {
        _driving = driving;
        knobGui->reflectDriverState(driving);
    }
    setVisible(driving);
    if (!driving) {
        knobGui->setWarningValue(KnobGui::eKnobWarningLoopDriverStale, QString());

        return;
    }

    if (all) {
        scopes.insert(scopes.begin(), -1);
    }
    if (std::find(scopes.begin(), scopes.end(), _scope) == scopes.end()) {
        _scope = scopes.front();
    }
    _scopes = scopes;
    _dimension->clear();
    for (int scope : _scopes) {
        _dimension->addItem((scope < 0) ? tr("All") : QString::fromUtf8(knob->getDimensionName(scope).c_str()));
    }
    _dimension->setCurrentIndex_no_emit((int)(std::find(_scopes.begin(), _scopes.end(), _scope) - _scopes.begin()));
    _dimension->setVisible(drivers.size() > 1);

    const KnobDriverPtr driver = currentDriver();
    const QString variable = QString::fromUtf8(driver->getVariable().c_str());
    bool stale = false;
    for (const KnobDriverPtr& d : drivers) {
        stale = stale || LoopDriverEdit::isStale(d, _variables);
    }

    _variable->clear();
    for (const QString& name : _variables) {
        _variable->addItem(name);
    }
    if (!_variables.contains(variable)) {
        _variable->addItem(variable);
    }
    _variable->setCurrentText_no_emit(variable);

    _start->setValue(driver->getStart());
    _end->setValue(driver->getEnd());
    const int easing = (int)(std::find(kEasings, kEasings + 3, driver->getEasing()) - kEasings);
    _easing->setCurrentIndex_no_emit(easing);
    _editCurve->setVisible(driver->getEasing() == KnobDriver::eEasingCurve);

    knobGui->setWarningValue(KnobGui::eKnobWarningLoopDriverStale,
                             stale ? tr("The loop driver reads a variable that no loop around this node sets, so the "
                                        "parameter uses its own value. Choose another variable or remove the driver.")
                                   : QString());
    refreshPreview();
}

void
KnobDriverRow::refreshPreview()
{
    KnobIPtr knob = _knob.lock();

    if (!knob || !_driving) {
        return;
    }
    QStringList values;
    for (int i = 0; i < knob->getDimension(); ++i) {
        if ((_scope < 0) || (_scope == i)) {
            values.push_back(QString::number(LoopDriverEdit::previewValue(knob, i), 'g', 6));
        }
    }
    _preview->setText(QString::fromUtf8("= ") + values.join(QString::fromUtf8(", ")));
}

void
KnobDriverRow::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    refreshPreview();
}

void
KnobDriverRow::onVariableChanged(int index)
{
    const KnobDriverPtr driver = currentDriver();

    if (!driver || (index < 0) || (_variable->itemText(index).toStdString() == driver->getVariable())) {
        return;
    }
    LoopDriverEdit::DriverChange change(LoopDriverEdit::DriverChange::eFieldVariable);
    change.variable = _variable->itemText(index).toStdString();
    edit(change, 0);
}

void
KnobDriverRow::onDimensionChanged(int index)
{
    if ((index >= 0) && (index < (int)_scopes.size())) {
        _scope = _scopes[index];
        scheduleRefresh();
    }
}

void
KnobDriverRow::onStartChanged(double value)
{
    LoopDriverEdit::DriverChange change(LoopDriverEdit::DriverChange::eFieldStart);

    change.value = value;
    edit(change, _start->getGestureKey());
}

void
KnobDriverRow::onEndChanged(double value)
{
    LoopDriverEdit::DriverChange change(LoopDriverEdit::DriverChange::eFieldEnd);

    change.value = value;
    edit(change, _end->getGestureKey());
}

void
KnobDriverRow::onEasingChanged(int index)
{
    const KnobDriverPtr driver = currentDriver();

    if (!driver || (index < 0) || (index >= 3) || (kEasings[index] == driver->getEasing())) {
        return;
    }
    LoopDriverEdit::DriverChange change(LoopDriverEdit::DriverChange::eFieldEasing);
    change.easing = kEasings[index];
    edit(change, 0);
}

void
KnobDriverRow::onEditCurveClicked()
{
    const KnobDriverPtr driver = currentDriver();

    if (!driver) {
        return;
    }
    EasingCurveDialog dialog(driver->getCurve(), this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    LoopDriverEdit::DriverChange change(LoopDriverEdit::DriverChange::eFieldEasing);
    change.easing = KnobDriver::eEasingCurve;
    change.curve = dialog.makeCurve();
    edit(change, 0);
}

void
KnobDriverRow::onContextMenuRequested(const QPoint& pos)
{
    KnobGuiPtr knobGui = _knobGui.lock();

    if (knobGui) {
        knobGui->showRightClickMenuForDimension(pos, _scope);
    }
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_KnobDriverRow.cpp"
