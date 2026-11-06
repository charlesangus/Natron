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

#ifndef Engine_Nodes_Keyer_KeyerRange_h
#define Engine_Nodes_Keyer_KeyerRange_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cfloat>
#include <string>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/KnobReader.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A range knob for a keyer: a KnobDouble of KnobDouble::eRangeWidgetDimensionCount
 * dimensions, a centre and four offsets (KnobDouble::RangeWidgetDimensionEnum), the offsets
 * at least 0. The caller shows it as a trapezoid with KnobDouble::setRangeWidget().
 **/
inline KnobDoublePtr
createKeyerRangeKnob(KnobHolder* holder,
                     const KnobPagePtr& page,
                     const char* name,
                     const std::string& label,
                     const std::string& hint,
                     const double defaults[KnobDouble::eRangeWidgetDimensionCount])
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, label, KnobDouble::eRangeWidgetDimensionCount);
    const char* const dimensionNames[KnobDouble::eRangeWidgetDimensionCount] = {
        "Outer Low", "Inner Low", "Centre", "Inner High", "Outer High"
    };

    knob->setName(name);
    knob->setHintToolTip(hint);
    for (int i = 0; i < KnobDouble::eRangeWidgetDimensionCount; ++i) {
        const bool offset = (i != KnobDouble::eRangeWidgetDimensionCentre);
        knob->setDimensionName(i, dimensionNames[i]);
        knob->setMinimum(offset ? 0. : -DBL_MAX, i);
        knob->setMaximum(DBL_MAX, i);
        knob->setDisplayMinimum(0., i);
        knob->setDisplayMaximum(offset ? 0.5 : 1., i);
        knob->setDefaultValue(defaults[i], i);
    }
    page->addKnob(knob);

    return knob;
}

/**
 * @brief The tooltip text every keyer range knob shares, on how its offsets key.
 **/
inline std::string
keyerRangeOffsetsHint()
{
    return EffectInstance::tr("The key is 1 from the centre minus the inner low offset to the centre plus the inner high offset, and falls to 0 over the outer low offset below that and over the outer high offset above it, so an outer offset of 0 is a hard edge. Offsets below 0 count as 0.").toStdString();
}

/**
 * @brief A range knob's centre at the reader's time and its offsets as they key
 * (KnobDouble::rangeOffsetsOf()).
 **/
inline double
keyerRangeAt(const KnobReader& reader,
             const KnobDoubleWPtr& knob,
             double* offsets)
{
    double values[KnobDouble::eRangeWidgetDimensionCount];

    for (int i = 0; i < KnobDouble::eRangeWidgetDimensionCount; ++i) {
        values[i] = reader.value(knob, i);
    }
    KnobDouble::rangeOffsetsOf(values, offsets);

    return values[KnobDouble::eRangeWidgetDimensionCentre];
}

/**
 * @brief The outer low, inner low, inner high and outer high points of the range values, in
 * dimension order, as they key: always in increasing order.
 **/
inline void
keyerRangePoints(const double* values,
                 double* points)
{
    double o[4];

    KnobDouble::rangeOffsetsOf(values, o);
    const double centre = values[KnobDouble::eRangeWidgetDimensionCentre];
    const double innerLow = centre - o[1];
    const double innerHigh = centre + o[2];

    points[0] = innerLow - o[0];
    points[1] = innerLow;
    points[2] = innerHigh;
    points[3] = innerHigh + o[3];
}

/**
 * @brief keyerRangePoints() of a linear range knob at the reader's time, in float.
 **/
inline void
keyerRangePoints(const KnobReader& reader,
                 const KnobDoubleWPtr& knob,
                 float* points)
{
    double values[KnobDouble::eRangeWidgetDimensionCount];

    for (int i = 0; i < KnobDouble::eRangeWidgetDimensionCount; ++i) {
        values[i] = reader.value(knob, i);
    }
    double p[4];
    keyerRangePoints(values, p);
    for (int i = 0; i < 4; ++i) {
        points[i] = (float)p[i];
    }
}

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Keyer_KeyerRange_h
