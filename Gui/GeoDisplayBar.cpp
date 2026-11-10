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

#include "GeoDisplayBar.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>

#include "Engine/AppManager.h"
#include "Engine/ViewerInstance.h"
#include "Gui/GeoDisplay.h"
#include "Gui/GuiDensity.h"

NATRON_NAMESPACE_ENTER

namespace {

QCheckBox*
addToggle(QWidget* parent,
          QHBoxLayout* layout,
          const QString& label,
          const char* name,
          const QString& tip)
{
    QCheckBox* box = new QCheckBox(label, parent);
    box->setObjectName(QString::fromUtf8(name));
    box->setFocusPolicy(Qt::NoFocus);
    box->setToolTip(tip);
    layout->addWidget(box);

    return box;
}

} // namespace

GeoDisplayBar::GeoDisplayBar(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QString::fromUtf8("geoDisplayRow"));
    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->setSpacing(TO_DPIX(NATRON_DENSITY_TOOLBAR_SPACING * 2));
    layout->setContentsMargins(TO_DPIX(NATRON_DENSITY_TOOLBAR_SPACING * 2), TO_DPIY(NATRON_DENSITY_TOOLBAR_SPACING),
                               TO_DPIX(NATRON_DENSITY_TOOLBAR_SPACING * 2), TO_DPIY(NATRON_DENSITY_TOOLBAR_SPACING));

    _pointMarkers = addToggle(this, layout, tr("Point Markers"), kViewerParamPointMarkers, tr("Marks every point."));
    _pointNumbers = addToggle(this, layout, tr("Point Numbers"), kViewerParamPointNumbers,
                              tr("Numbers the points in view, up to a limit per frame."));
    _primitiveNumbers = addToggle(this, layout, tr("Primitive Numbers"), kViewerParamPrimitiveNumbers,
                                  tr("Numbers the primitives in view at their centres, up to a limit per frame."));
    _normals = addToggle(this, layout, tr("Normals"), kViewerParamNormals, tr("Draws the N point attribute as a line from each point."));

    const QString scaleTip = tr("The length, in scene units, a normal of length 1 is drawn at.");
    QLabel* scaleLabel = new QLabel(tr("Normal Scale"), this);
    scaleLabel->setToolTip(scaleTip);
    layout->addWidget(scaleLabel);
    _normalScale = new QDoubleSpinBox(this);
    _normalScale->setObjectName(QString::fromUtf8(kViewerParamNormalScale));
    _normalScale->setDecimals(4);
    _normalScale->setRange(0., 1e6);
    _normalScale->setSingleStep(0.01);
    _normalScale->setValue(0.1);
    _normalScale->setKeyboardTracking(false);
    _normalScale->setToolTip(scaleTip);
    layout->addWidget(_normalScale);

    const QString groupTip = tr("The point group, else the primitive group, to highlight. Empty highlights nothing.");
    QLabel* groupLabel = new QLabel(tr("Highlight Group"), this);
    groupLabel->setToolTip(groupTip);
    layout->addWidget(groupLabel);
    _highlightGroup = new QLineEdit(this);
    _highlightGroup->setObjectName(QString::fromUtf8(kViewerParamHighlightGroup));
    _highlightGroup->setToolTip(groupTip);
    _highlightGroup->setMaximumWidth(TO_DPIX(160));
    layout->addWidget(_highlightGroup);
    layout->addStretch();

    for (QCheckBox* box : { _pointMarkers, _pointNumbers, _primitiveNumbers, _normals }) {
        QObject::connect(box, &QCheckBox::toggled, this, [this]() { edited(); });
    }
    QObject::connect(_normalScale, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this]() { edited(); });
    QObject::connect(_highlightGroup, &QLineEdit::editingFinished, this, [this]() { edited(); });
}

void
GeoDisplayBar::setOptions(const GeoDisplayOptions& options)
{
    QSignalBlocker b0(_pointMarkers), b1(_pointNumbers), b2(_primitiveNumbers), b3(_normals), b4(_normalScale), b5(_highlightGroup);

    _pointMarkers->setChecked(options.pointMarkers);
    _pointNumbers->setChecked(options.pointNumbers);
    _primitiveNumbers->setChecked(options.primitiveNumbers);
    _normals->setChecked(options.normals);
    if (_normalScale->value() != options.normalScale) {
        _normalScale->setValue(options.normalScale);
    }
    const QString group = QString::fromStdString(options.highlightGroup);
    if (_highlightGroup->text() != group) {
        _highlightGroup->setText(group);
    }
}

void
GeoDisplayBar::setEditCallback(const std::function<void(const GeoDisplayOptions&)>& callback)
{
    _callback = callback;
}

GeoDisplayOptions
GeoDisplayBar::currentOptions() const
{
    GeoDisplayOptions options;

    options.pointMarkers = _pointMarkers->isChecked();
    options.pointNumbers = _pointNumbers->isChecked();
    options.primitiveNumbers = _primitiveNumbers->isChecked();
    options.normals = _normals->isChecked();
    options.normalScale = _normalScale->value();
    options.highlightGroup = _highlightGroup->text().trimmed().toStdString();

    return options;
}

void
GeoDisplayBar::edited()
{
    if (_callback) {
        _callback(currentOptions());
    }
}

NATRON_NAMESPACE_EXIT
