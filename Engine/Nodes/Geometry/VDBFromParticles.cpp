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

#include "VDBFromParticles.h"

#include <exception>
#include <memory>
#include <string>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"

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
VDBFromParticles::build(const GeoDetail& source,
                        const GeoPointsToSdfParams& params,
                        GeoDetailBuilder& out,
                        std::vector<std::string>* notes,
                        std::string* error)
{
    GeoVDBConvertResult result;
    if (!geoPointsToSdf(source, params, &result, error)) {
        return false;
    }
    geoAppendVDBGrids(out, result);
    if (notes) {
        notes->insert(notes->end(), result.notes.begin(), result.notes.end());
    }

    return true;
}

VDBFromParticles::VDBFromParticles(NodePtr node)
    : NativeEffectBase(node)
{
}

VDBFromParticles::~VDBFromParticles()
{
}

const NodeSpec*
VDBFromParticles::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

namespace {

KnobDoublePtr
addDouble(VDBFromParticles* holder,
          const KnobPagePtr& page,
          const QString& label,
          const char* name,
          const QString& toolTip,
          double defaultValue,
          double minimum,
          double displayMaximum)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, label.toStdString());
    knob->setName(name);
    knob->setHintToolTip(toolTip.toStdString());
    knob->setDefaultValue(defaultValue);
    knob->setMinimum(minimum);
    knob->setDisplayMinimum(minimum);
    knob->setDisplayMaximum(displayMaximum);
    page->addKnob(knob);

    return knob;
}

} // namespace

void
VDBFromParticles::initializeKnobs()
{
    const GeoPointsToSdfParams defaults;
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    KnobStringPtr name = createKnob<KnobString>(tr("Name").toStdString());
    name->setName(kVDBFromParticlesParamName);
    name->setHintToolTip(tr("The name of the distance or density grid. Attribute grids are named after their "
                            "attributes.")
                             .toStdString());
    name->setDefaultValue(defaults.name);
    name->setAnimationEnabled(false);
    page->addKnob(name);
    _name = name;

    KnobChoicePtr outputType = createKnob<KnobChoice>(tr("Output Type").toStdString());
    outputType->setName(kVDBFromParticlesParamOutputType);
    outputType->setHintToolTip(tr("What the volume holds.").toStdString());
    {
        std::vector<ChoiceOption> types;
        types.push_back(ChoiceOption("sdf", tr("SDF").toStdString(),
                                     tr("A narrow-band signed distance field: the distance to the spheres' "
                                        "surface in world units, negative inside.")
                                         .toStdString()));
        types.push_back(ChoiceOption("fog", tr("Fog").toStdString(),
                                     tr("A density of 0 outside and 1 inside, ramping across the inner half of "
                                        "the band.")
                                         .toStdString()));
        outputType->populateChoices(types);
    }
    outputType->setAnimationEnabled(false);
    page->addKnob(outputType);
    _outputType = outputType;

    _voxelSize = addDouble(this, page, tr("Voxel Size"), kVDBFromParticlesParamVoxelSize,
                           tr("The edge length of a voxel in world units. Memory grows with the cube of its "
                              "inverse; the node's info shows what the grids use."),
                           defaults.voxelSize, 0., 1.);
    _radiusScale = addDouble(this, page, tr("Radius Scale"), kVDBFromParticlesParamRadiusScale,
                             tr("Multiplies each point's pscale, or 1 for points without one, to give its "
                                "sphere's radius in world units."),
                             defaults.radiusScale, 0., 10.);
    _minRadius = addDouble(this, page, tr("Min Radius"), kVDBFromParticlesParamMinRadius,
                           tr("Spheres with a smaller radius, in voxels, are left out; the node's info counts "
                              "them. Lower it, or the voxel size, to keep small points."),
                           defaults.minRadius, 0.001, 5.);
    _bandWidth = addDouble(this, page, tr("Band Width"), kVDBFromParticlesParamBandWidth,
                           tr("How far each side of the surface distances are stored, in voxels."),
                           defaults.bandWidth, 1., 10.);

    KnobBoolPtr trails = createKnob<KnobBool>(tr("Trails").toStdString());
    trails->setName(kVDBFromParticlesParamTrails);
    trails->setHintToolTip(tr("Stretch each sphere back along -v times Velocity Scale with a run of spheres "
                              "shrinking towards Min Radius. Points without v get no trail.")
                               .toStdString());
    trails->setDefaultValue(defaults.trails);
    trails->setAnimationEnabled(false);
    page->addKnob(trails);
    _trails = trails;

    _velocityScale = addDouble(this, page, tr("Velocity Scale"), kVDBFromParticlesParamVelocityScale,
                               tr("Multiplies v to give the length and direction of the trail in world units."),
                               defaults.velocityScale, 0., 10.);
    _trailSpacing = addDouble(this, page, tr("Trail Spacing"), kVDBFromParticlesParamTrailSpacing,
                              tr("The gap between the spheres of a trail, in radii. Smaller is smoother and "
                                 "slower."),
                              defaults.trailSpacing, 0.01, 2.);

    KnobStringPtr attributes = createKnob<KnobString>(tr("Attributes").toStdString());
    attributes->setName(kVDBFromParticlesParamAttributes);
    attributes->setHintToolTip(tr("Point attributes to carry into grids of their own: space-separated globs, ^ "
                                  "in front of one to leave out what it matches. Each voxel holds the value of "
                                  "the sphere whose surface is closest. Float and integer attributes of one or "
                                  "three components convert.")
                                   .toStdString());
    attributes->setAnimationEnabled(false);
    page->addKnob(attributes);
    _attributes = attributes;

    updateVisibility();
}

void
VDBFromParticles::updateVisibility()
{
    const KnobBoolPtr trails = _trails.lock();
    const bool on = trails && trails->getValue();
    setVisible<KnobDoublePtr>(_velocityScale, on);
    setVisible<KnobDoublePtr>(_trailSpacing, on);
}

void
VDBFromParticles::onKnobsLoaded()
{
    updateVisibility();
}

bool
VDBFromParticles::knobChanged(KnobI* k,
                              ValueChangedReasonEnum /*reason*/,
                              ViewSpec /*view*/,
                              double /*time*/,
                              bool /*originatedFromMainThread*/)
{
    const KnobBoolPtr trails = _trails.lock();
    if (trails && (k == trails.get())) {
        updateVisibility();

        return true;
    }

    return false;
}

GeoPointsToSdfParams
VDBFromParticles::readParams(double time,
                             ViewSpec view) const
{
    GeoPointsToSdfParams p;
    p.name = valueAt<std::string>(_name, p.name, time, view);
    p.kind = valueAt<int>(_outputType, 0, time, view) == 1 ? eGeoVDBVolumeFog : eGeoVDBVolumeSDF;
    p.voxelSize = valueAt<double>(_voxelSize, p.voxelSize, time, view);
    p.radiusScale = valueAt<double>(_radiusScale, p.radiusScale, time, view);
    p.minRadius = valueAt<double>(_minRadius, p.minRadius, time, view);
    p.bandWidth = valueAt<double>(_bandWidth, p.bandWidth, time, view);
    p.trails = valueAt<bool>(_trails, p.trails, time, view);
    p.velocityScale = valueAt<double>(_velocityScale, p.velocityScale, time, view);
    p.trailSpacing = valueAt<double>(_trailSpacing, p.trailSpacing, time, view);
    p.attributes = valueAt<std::string>(_attributes, p.attributes, time, view);

    return p;
}

StatusEnum
VDBFromParticles::renderGeometry(const GeoRenderActionArgs& args,
                                 GeoDetailConstPtr* outputDetail,
                                 std::string* errorMessage)
{
    const GeoPointsToSdfParams params = readParams(args.time, args.view);
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
            // An info-type persistent message is only logged or shown as a dialog, never kept on
            // the node; the quiet one is kept and shown on the node.
            if (const NodePtr node = getNode()) {
                node->setQuietInfoMessage(joinNotes(notes));
            }
        }
    }

    return status;
}

NATRON_NAMESPACE_EXIT
