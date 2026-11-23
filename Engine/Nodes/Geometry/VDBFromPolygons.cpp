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

#include "VDBFromPolygons.h"

#include <exception>
#include <memory>
#include <string>
#include <vector>

#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

namespace {

template <typename T>
T
valueAt(const std::weak_ptr<Knob<T>>& knob,
        T fallback,
        double time,
        ViewSpec view)
{
    if (const std::shared_ptr<Knob<T>> locked = knob.lock()) {
        return locked->getValueAtTime(time, 0, view);
    }

    return fallback;
}

template <typename KnobPtrT>
void
setVisible(const std::weak_ptr<typename KnobPtrT::element_type>& knob,
           bool visible)
{
    if (const KnobPtrT locked = knob.lock()) {
        locked->setSecret(!visible);
    }
}

std::string
joinNotes(const std::vector<std::string>& notes)
{
    std::string text;
    for (const std::string& note : notes) {
        text += (text.empty() ? "" : "\n") + note;
    }

    return text;
}

} // namespace

bool
VDBFromPolygons::build(const GeoDetail& source,
                       const VDBFromPolygonsParams& params,
                       GeoDetailBuilder& out,
                       std::vector<std::string>* notes,
                       std::string* error)
{
    GeoPolygonsToSdfParams convert = params.convert;
    if (params.useVoxelCount) {
        const double counted = geoVoxelSizeForCount(source, params.voxelCount, params.voxelCountAxis);
        if (counted > 0.) {
            convert.voxelSize = counted;
        } else if (source.pointCount() > 0) {
            if (notes) {
                notes->push_back("The source has no extent along the voxel count's axis to divide, so no volume was made.");
            }

            return true;
        }
    }
    GeoVDBConvertResult result;
    if (!geoPolygonsToSdf(source, convert, &result, error)) {
        return false;
    }
    geoAppendVDBGrids(out, result);
    if (notes) {
        notes->insert(notes->end(), result.notes.begin(), result.notes.end());
    }

    return true;
}

VDBFromPolygons::VDBFromPolygons(NodePtr node)
    : NativeEffectBase(node)
{
}

VDBFromPolygons::~VDBFromPolygons()
{
}

const NodeSpec*
VDBFromPolygons::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
VDBFromPolygons::initializeKnobs()
{
    const VDBFromPolygonsParams defaults;
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    KnobStringPtr name = createKnob<KnobString>(tr("Name").toStdString());
    name->setName(kVDBFromPolygonsParamName);
    name->setHintToolTip(tr("The name of the distance or density grid. Attribute grids are named after their "
                            "attributes.")
                             .toStdString());
    name->setDefaultValue(defaults.convert.name);
    name->setAnimationEnabled(false);
    page->addKnob(name);
    _name = name;

    KnobChoicePtr outputType = createKnob<KnobChoice>(tr("Output Type").toStdString());
    outputType->setName(kVDBFromPolygonsParamOutputType);
    outputType->setHintToolTip(tr("What the volume holds.").toStdString());
    {
        std::vector<ChoiceOption> types;
        types.push_back(ChoiceOption("sdf", tr("SDF").toStdString(),
                                     tr("A narrow-band signed distance field: the distance to the surface in "
                                        "world units, negative inside.")
                                         .toStdString()));
        types.push_back(ChoiceOption("fog", tr("Fog").toStdString(),
                                     tr("A density of 0 outside and 1 inside, ramping across the interior "
                                        "band.")
                                         .toStdString()));
        outputType->populateChoices(types);
    }
    outputType->setAnimationEnabled(false);
    page->addKnob(outputType);
    _outputType = outputType;

    KnobBoolPtr useVoxelCount = createKnob<KnobBool>(tr("Use Voxel Count").toStdString());
    useVoxelCount->setName(kVDBFromPolygonsParamUseVoxelCount);
    useVoxelCount->setHintToolTip(tr("Size the voxels so that Voxel Count of them span the source along Voxel "
                                     "Count Axis, instead of giving the voxel size.")
                                      .toStdString());
    useVoxelCount->setDefaultValue(defaults.useVoxelCount);
    useVoxelCount->setAnimationEnabled(false);
    page->addKnob(useVoxelCount);
    _useVoxelCount = useVoxelCount;

    KnobDoublePtr voxelSize = createKnob<KnobDouble>(tr("Voxel Size").toStdString());
    voxelSize->setName(kVDBFromPolygonsParamVoxelSize);
    voxelSize->setHintToolTip(tr("The edge length of a voxel in world units. Memory grows with the cube of its "
                                 "inverse; the node's info shows what the grids use.")
                                  .toStdString());
    voxelSize->setDefaultValue(defaults.convert.voxelSize);
    voxelSize->setMinimum(0.);
    voxelSize->setDisplayMinimum(0.001);
    voxelSize->setDisplayMaximum(1.);
    page->addKnob(voxelSize);
    _voxelSize = voxelSize;

    KnobIntPtr voxelCount = createKnob<KnobInt>(tr("Voxel Count").toStdString());
    voxelCount->setName(kVDBFromPolygonsParamVoxelCount);
    voxelCount->setHintToolTip(tr("How many voxels span the source's bounds along Voxel Count Axis.").toStdString());
    voxelCount->setDefaultValue(defaults.voxelCount);
    voxelCount->setMinimum(1);
    voxelCount->setDisplayMinimum(1);
    voxelCount->setDisplayMaximum(500);
    page->addKnob(voxelCount);
    _voxelCount = voxelCount;

    KnobChoicePtr voxelCountAxis = createKnob<KnobChoice>(tr("Voxel Count Axis").toStdString());
    voxelCountAxis->setName(kVDBFromPolygonsParamVoxelCountAxis);
    voxelCountAxis->setHintToolTip(tr("The axis of the source's bounds that Voxel Count divides.").toStdString());
    {
        std::vector<ChoiceOption> axes;
        axes.push_back(ChoiceOption("x", tr("X").toStdString(), ""));
        axes.push_back(ChoiceOption("y", tr("Y").toStdString(), ""));
        axes.push_back(ChoiceOption("z", tr("Z").toStdString(), ""));
        axes.push_back(ChoiceOption("longest", tr("Longest").toStdString(),
                                    tr("Whichever of the three is longest.").toStdString()));
        voxelCountAxis->populateChoices(axes);
    }
    voxelCountAxis->setDefaultValue((int)defaults.voxelCountAxis);
    voxelCountAxis->setAnimationEnabled(false);
    page->addKnob(voxelCountAxis);
    _voxelCountAxis = voxelCountAxis;

    KnobDoublePtr exteriorBand = createKnob<KnobDouble>(tr("Exterior Band").toStdString());
    exteriorBand->setName(kVDBFromPolygonsParamExteriorBand);
    exteriorBand->setHintToolTip(tr("How far outside the surface distances are stored, in voxels. At least 1.")
                                     .toStdString());
    exteriorBand->setDefaultValue(defaults.convert.exteriorBand);
    exteriorBand->setMinimum(1.);
    exteriorBand->setDisplayMinimum(1.);
    exteriorBand->setDisplayMaximum(10.);
    page->addKnob(exteriorBand);
    _exteriorBand = exteriorBand;

    KnobDoublePtr interiorBand = createKnob<KnobDouble>(tr("Interior Band").toStdString());
    interiorBand->setName(kVDBFromPolygonsParamInteriorBand);
    interiorBand->setHintToolTip(tr("How far inside the surface distances are stored, in voxels. At least 1. "
                                    "For a fog volume this is the width of the density ramp.")
                                     .toStdString());
    interiorBand->setDefaultValue(defaults.convert.interiorBand);
    interiorBand->setMinimum(1.);
    interiorBand->setDisplayMinimum(1.);
    interiorBand->setDisplayMaximum(10.);
    page->addKnob(interiorBand);
    _interiorBand = interiorBand;

    KnobBoolPtr fillInterior = createKnob<KnobBool>(tr("Fill Interior").toStdString());
    fillInterior->setName(kVDBFromPolygonsParamFillInterior);
    fillInterior->setHintToolTip(tr("Store distances through the whole inside instead of stopping at the "
                                    "interior band. Needs a closed mesh.")
                                     .toStdString());
    fillInterior->setDefaultValue(defaults.convert.fillInterior);
    fillInterior->setAnimationEnabled(false);
    page->addKnob(fillInterior);
    _fillInterior = fillInterior;

    KnobStringPtr attributes = createKnob<KnobString>(tr("Attributes").toStdString());
    attributes->setName(kVDBFromPolygonsParamAttributes);
    attributes->setHintToolTip(tr("Point, vertex and primitive attributes to carry into grids of their own: "
                                  "space-separated globs, ^ in front of one to leave out what it matches. Each "
                                  "voxel of the band holds the value at the closest point of the surface. Float "
                                  "and integer attributes of one or three components convert.")
                                   .toStdString());
    attributes->setAnimationEnabled(false);
    page->addKnob(attributes);
    _attributes = attributes;

    updateVisibility();
}

void
VDBFromPolygons::updateVisibility()
{
    const KnobBoolPtr useVoxelCount = _useVoxelCount.lock();
    const bool count = useVoxelCount && useVoxelCount->getValue();
    setVisible<KnobDoublePtr>(_voxelSize, !count);
    setVisible<KnobIntPtr>(_voxelCount, count);
    setVisible<KnobChoicePtr>(_voxelCountAxis, count);
}

void
VDBFromPolygons::onKnobsLoaded()
{
    updateVisibility();
}

bool
VDBFromPolygons::knobChanged(KnobI* k,
                             ValueChangedReasonEnum /*reason*/,
                             ViewSpec /*view*/,
                             double /*time*/,
                             bool /*originatedFromMainThread*/)
{
    const KnobBoolPtr useVoxelCount = _useVoxelCount.lock();
    if (useVoxelCount && (k == useVoxelCount.get())) {
        updateVisibility();

        return true;
    }

    return false;
}

VDBFromPolygonsParams
VDBFromPolygons::readParams(double time,
                            ViewSpec view) const
{
    VDBFromPolygonsParams p;
    p.convert.name = valueAt<std::string>(_name, p.convert.name, time, view);
    p.convert.kind = valueAt<int>(_outputType, 0, time, view) == 1 ? eGeoVDBVolumeFog : eGeoVDBVolumeSDF;
    p.useVoxelCount = valueAt<bool>(_useVoxelCount, p.useVoxelCount, time, view);
    p.convert.voxelSize = valueAt<double>(_voxelSize, p.convert.voxelSize, time, view);
    p.voxelCount = valueAt<int>(_voxelCount, p.voxelCount, time, view);
    const int axis = valueAt<int>(_voxelCountAxis, (int)p.voxelCountAxis, time, view);
    if ((axis >= eGeoVDBAxisX) && (axis <= eGeoVDBAxisLongest)) {
        p.voxelCountAxis = (GeoVDBAxisEnum)axis;
    }
    p.convert.exteriorBand = valueAt<double>(_exteriorBand, p.convert.exteriorBand, time, view);
    p.convert.interiorBand = valueAt<double>(_interiorBand, p.convert.interiorBand, time, view);
    p.convert.fillInterior = valueAt<bool>(_fillInterior, p.convert.fillInterior, time, view);
    p.convert.attributes = valueAt<std::string>(_attributes, p.convert.attributes, time, view);

    return p;
}

StatusEnum
VDBFromPolygons::renderGeometry(const GeoRenderActionArgs& args,
                                GeoDetailConstPtr* outputDetail,
                                std::string* errorMessage)
{
    const VDBFromPolygonsParams params = readParams(args.time, args.view);
    const GeoDetailConstPtr input = args.getInputDetail(0);
    const GeoDetail empty;
    const GeoDetail& source = input ? *input : empty;
    std::vector<std::string> notes;
    const StatusEnum status = renderGeometryGenerator([&](GeoDetailBuilder& detail, std::string* error) {
        try {
            return build(source, params, detail, &notes, error) ? eStatusOK : eStatusFailed;
        } catch (const std::exception& e) {
            if (error) {
                *error = e.what();
            }

            return eStatusFailed;
        }
    },
                                                      outputDetail, errorMessage);
    if (status == eStatusOK) {
        if (notes.empty()) {
            clearPersistentMessage(false);
        } else {
            setPersistentMessage(eMessageTypeInfo, joinNotes(notes));
        }
    }

    return status;
}

NATRON_NAMESPACE_EXIT
