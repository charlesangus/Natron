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

#include "GeoSubdivide.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <QString>

#include <opensubdiv/far/stencilTable.h>
#include <opensubdiv/far/stencilTableFactory.h>
#include <opensubdiv/far/topologyDescriptor.h>
#include <opensubdiv/far/topologyRefiner.h>
#include <opensubdiv/sdc/options.h>
#include <opensubdiv/sdc/types.h>

#include "Engine/ChoiceOption.h"
#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Geometry/GeoInterpolate.h"
#include "Engine/Nodes/Geometry/GeoNormals.h"

NATRON_NAMESPACE_ENTER

namespace {
namespace Far = OpenSubdiv::Far;
namespace Sdc = OpenSubdiv::Sdc;

typedef Far::StencilTableFactoryReal<double> StencilFactory;
typedef Far::StencilTableReal<double> StencilTable;
typedef Far::TopologyRefinerFactory<Far::TopologyDescriptor> RefinerFactory;

// The base values of one vertex attribute as an OpenSubdiv face-varying channel: corners of a
// point whose values are equal share a value, so they are smoothed together, and corners whose
// values differ stay apart.
struct FaceVaryingChannel {
    const GeoAttribute* attribute = nullptr;
    std::vector<Far::Index> valueIndices;
    // A vertex holding each value.
    std::vector<GeoOffset> representatives;
};

bool
sameValue(const GeoAttribute& attribute,
          GeoOffset a,
          GeoOffset b,
          std::vector<double>& scratchA,
          std::vector<double>& scratchB)
{
    geoReadNumeric(attribute, (std::size_t)a, std::span<double>(scratchA));
    geoReadNumeric(attribute, (std::size_t)b, std::span<double>(scratchB));

    return scratchA == scratchB;
}

// Corners of a point share a value when same(representative, corner) says so.
template <typename Same>
FaceVaryingChannel
buildChannel(const std::vector<Far::Index>& cornerPoints,
             const std::vector<GeoOffset>& cornerVertices,
             std::size_t basePointCount,
             Same same)
{
    FaceVaryingChannel channel;
    channel.valueIndices.reserve(cornerPoints.size());
    std::vector<std::vector<Far::Index>> valuesAtPoint(basePointCount);
    for (std::size_t k = 0; k < cornerPoints.size(); ++k) {
        std::vector<Far::Index>& candidates = valuesAtPoint[(std::size_t)cornerPoints[k]];
        Far::Index value = -1;
        for (Far::Index candidate : candidates) {
            if (same(channel.representatives[(std::size_t)candidate], cornerVertices[k])) {
                value = candidate;
                break;
            }
        }
        if (value < 0) {
            value = (Far::Index)channel.representatives.size();
            channel.representatives.push_back(cornerVertices[k]);
            candidates.push_back(value);
        }
        channel.valueIndices.push_back(value);
    }

    return channel;
}

FaceVaryingChannel
buildAttributeChannel(const GeoAttribute& attribute,
                      const std::vector<Far::Index>& cornerPoints,
                      const std::vector<GeoOffset>& cornerVertices,
                      std::size_t basePointCount)
{
    const bool weldable = geoIsNumeric(attribute) && !attribute.desc().isArray;
    std::vector<double> a(attribute.desc().tupleSize);
    std::vector<double> b(attribute.desc().tupleSize);
    FaceVaryingChannel channel = buildChannel(cornerPoints, cornerVertices, basePointCount, [&](GeoOffset x, GeoOffset y) {
        return weldable && sameValue(attribute, x, y, a, b);
    });
    channel.attribute = &attribute;

    return channel;
}

std::unique_ptr<const StencilTable>
createStencils(const Far::TopologyRefiner& refiner,
               int depth,
               bool faceVarying,
               int channel)
{
    StencilFactory::Options options;
    options.interpolationMode = faceVarying ? StencilFactory::INTERPOLATE_FACE_VARYING : StencilFactory::INTERPOLATE_VERTEX;
    options.generateOffsets = true;
    options.generateControlVerts = false;
    options.generateIntermediateLevels = false;
    options.factorizeIntermediateLevels = true;
    options.maxLevel = (unsigned int)depth;
    options.fvarChannel = (unsigned int)channel;

    return std::unique_ptr<const StencilTable>(StencilFactory::Create(refiner, options));
}

// The stencil's sources as offsets into the base elements, with its weights.
void
stencilSources(const StencilTable& table,
               Far::Index i,
               const std::vector<GeoOffset>& baseOffsets,
               std::vector<GeoOffset>* offsets,
               std::vector<double>* weights)
{
    const Far::StencilReal<double> stencil = table.GetStencil(i);
    const int size = stencil.GetSize();
    const Far::Index* indices = stencil.GetVertexIndices();
    const double* w = stencil.GetWeights();
    offsets->resize((std::size_t)size);
    weights->resize((std::size_t)size);
    for (int s = 0; s < size; ++s) {
        (*offsets)[(std::size_t)s] = baseOffsets[(std::size_t)indices[s]];
        (*weights)[(std::size_t)s] = w[s];
    }
}

GeoOffset
heaviestSource(const std::vector<GeoOffset>& offsets,
               const std::vector<double>& weights)
{
    std::size_t best = 0;
    for (std::size_t s = 1; s < weights.size(); ++s) {
        if (weights[s] > weights[best]) {
            best = s;
        }
    }

    return offsets.empty() ? GeoIndexMap::kInvalidOffset : offsets[best];
}
} // anonymous namespace

bool
subdivideGeometry(GeoDetailBuilder& detail,
                  const GeoSubdivideParams& params,
                  std::string* errorMessage)
{
    const int depth = std::clamp(params.depth, 1, 4);
    const GeoDetail source = detail.detail();

    GeoElementMask selected;
    if (!geoSelectElements(source, eGeoAttribClassPrimitive, params.group, &selected, errorMessage)) {
        return false;
    }
    std::vector<GeoOffset> faces;
    source.primitiveMap().forEachLive([&](GeoIndex, GeoOffset primitive) {
        if (selected[(std::size_t)primitive] && (source.primitiveType(primitive) == eGeoPrimitivePolygon)
            && (source.primitiveVertices(primitive).count >= 3)) {
            faces.push_back(primitive);
        }
    });
    if (faces.empty()) {
        return true;
    }

    std::vector<int> basePointIndex((std::size_t)source.pointMap().offsetSize(), -1);
    std::vector<GeoOffset> basePoints;
    std::vector<int> faceSizes;
    std::vector<Far::Index> cornerPoints;
    std::vector<GeoOffset> cornerVertices;
    faceSizes.reserve(faces.size());
    for (GeoOffset face : faces) {
        const GeoVertexRange range = source.primitiveVertices(face);
        if ((params.algorithm == eGeoSubdivideLoop) && (range.count != 3)) {
            *errorMessage = "Loop subdivision needs triangles: a selected polygon has " + std::to_string(range.count) + " sides.";

            return false;
        }
        faceSizes.push_back((int)range.count);
        for (GeoOffset i = 0; i < range.count; ++i) {
            const GeoOffset vertex = range.start + i;
            const GeoOffset point = source.vertexPoint(vertex);
            int& index = basePointIndex[(std::size_t)point];
            if (index < 0) {
                index = (int)basePoints.size();
                basePoints.push_back(point);
            }
            cornerPoints.push_back(index);
            cornerVertices.push_back(vertex);
        }
    }

    std::vector<FaceVaryingChannel> channels;
    for (std::size_t i = 0; i < source.attributeCount(eGeoAttribClassVertex); ++i) {
        channels.push_back(buildAttributeChannel(source.attribute(eGeoAttribClassVertex, i), cornerPoints, cornerVertices, basePoints.size()));
    }
    const std::size_t firstGroupChannel = channels.size();
    const std::vector<std::string> vertexGroupNames = source.groupNames(eGeoAttribClassVertex);
    std::vector<const GeoElementGroup*> vertexGroups;
    for (const std::string& name : vertexGroupNames) {
        const GeoElementGroup* group = source.findGroup(eGeoAttribClassVertex, name);
        vertexGroups.push_back(group);
        channels.push_back(buildChannel(cornerPoints, cornerVertices, basePoints.size(), [group](GeoOffset x, GeoOffset y) {
            return !group || (group->contains(x) == group->contains(y));
        }));
    }
    std::vector<Far::TopologyDescriptor::FVarChannel> descriptorChannels(channels.size());
    for (std::size_t c = 0; c < channels.size(); ++c) {
        descriptorChannels[c].numValues = (int)channels[c].representatives.size();
        descriptorChannels[c].valueIndices = channels[c].valueIndices.data();
    }

    Far::TopologyDescriptor descriptor;
    descriptor.numVertices = (int)basePoints.size();
    descriptor.numFaces = (int)faces.size();
    descriptor.numVertsPerFace = faceSizes.data();
    descriptor.vertIndicesPerFace = cornerPoints.data();
    descriptor.numFVarChannels = (int)descriptorChannels.size();
    descriptor.fvarChannels = descriptorChannels.empty() ? nullptr : descriptorChannels.data();

    Sdc::Options options;
    options.SetVtxBoundaryInterpolation(params.boundary == eGeoSubdivideBoundaryEdgesOnly ? Sdc::Options::VTX_BOUNDARY_EDGE_ONLY
                                                                                          : Sdc::Options::VTX_BOUNDARY_EDGE_AND_CORNER);
    options.SetFVarLinearInterpolation((Sdc::Options::FVarLinearInterpolation)std::clamp((int)params.vertexInterpolation,
                                                                                         (int)eGeoSubdivideVertexSmooth,
                                                                                         (int)eGeoSubdivideVertexLinear));
    const Sdc::SchemeType scheme = params.algorithm == eGeoSubdivideLoop ? Sdc::SCHEME_LOOP
        : params.algorithm == eGeoSubdivideBilinear                      ? Sdc::SCHEME_BILINEAR
                                                                         : Sdc::SCHEME_CATMARK;
    std::unique_ptr<Far::TopologyRefiner> refiner(RefinerFactory::Create(descriptor, RefinerFactory::Options(scheme, options)));
    if (!refiner) {
        *errorMessage = "The selected polygons could not be subdivided: their topology is invalid.";

        return false;
    }
    Far::TopologyRefiner::UniformOptions uniform(depth);
    uniform.fullTopologyInLastLevel = true;
    refiner->RefineUniform(uniform);
    const Far::TopologyLevel& last = refiner->GetLevel(depth);

    std::vector<GeoOffset> offsets;
    std::vector<double> weights;

    // Points.
    const std::unique_ptr<const StencilTable> pointStencils = createStencils(*refiner, depth, false, 0);
    const int newPointCount = last.GetNumVertices();
    if (!pointStencils || (pointStencils->GetNumStencils() != newPointCount)) {
        *errorMessage = "The subdivision of the selected polygons failed.";

        return false;
    }
    const GeoOffset firstPoint = detail.appendPoints((GeoIndex)newPointCount);
    for (int i = 0; i < newPointCount; ++i) {
        stencilSources(*pointStencils, i, basePoints, &offsets, &weights);
        std::array<double, 3> p = { 0., 0., 0. };
        for (std::size_t s = 0; s < offsets.size(); ++s) {
            const std::array<double, 3> q = source.pointPosition(offsets[s]);
            for (int k = 0; k < 3; ++k) {
                p[(std::size_t)k] += weights[s] * q[(std::size_t)k];
            }
        }
        detail.setPointPosition(firstPoint + i, p[0], p[1], p[2]);
    }
    for (std::size_t a = 0; a < source.attributeCount(eGeoAttribClassPoint); ++a) {
        const GeoAttribute& from = source.attribute(eGeoAttribClassPoint, a);
        if (from.desc().name == "P") {
            continue;
        }
        GeoAttribute* to = detail.attributeForWriting(eGeoAttribClassPoint, from.desc().name);
        if (!to) {
            continue;
        }
        for (int i = 0; i < newPointCount; ++i) {
            stencilSources(*pointStencils, i, basePoints, &offsets, &weights);
            geoBlendElement(*to, (std::size_t)(firstPoint + i), from, offsets, weights);
        }
    }
    for (const std::string& name : source.groupNames(eGeoAttribClassPoint)) {
        const GeoElementGroup* from = source.findGroup(eGeoAttribClassPoint, name);
        GeoElementGroup& to = detail.addGroup(eGeoAttribClassPoint, name);
        for (int i = 0; i < newPointCount; ++i) {
            stencilSources(*pointStencils, i, basePoints, &offsets, &weights);
            const GeoOffset heaviest = heaviestSource(offsets, weights);
            if (from && (heaviest != GeoIndexMap::kInvalidOffset) && from->contains(heaviest)) {
                to.add(firstPoint + i);
            }
        }
    }

    // Faces, each traced back to the selected polygon it comes from.
    const int newFaceCount = last.GetNumFaces();
    std::vector<std::int32_t> newFaceSizes((std::size_t)newFaceCount);
    std::vector<GeoOffset> newCorners;
    std::vector<GeoOffset> parents((std::size_t)newFaceCount);
    for (int f = 0; f < newFaceCount; ++f) {
        const Far::ConstIndexArray corners = last.GetFaceVertices(f);
        newFaceSizes[(std::size_t)f] = corners.size();
        for (int k = 0; k < corners.size(); ++k) {
            newCorners.push_back(firstPoint + corners[k]);
        }
        Far::Index parent = f;
        for (int level = depth; level >= 1; --level) {
            parent = refiner->GetLevel(level).GetFaceParentFace(parent);
        }
        parents[(std::size_t)f] = faces[(std::size_t)parent];
    }
    const GeoOffset firstPrimitive = detail.appendPrimitives(eGeoPrimitivePolygon, newFaceSizes, newCorners);
    for (std::size_t a = 0; a < source.attributeCount(eGeoAttribClassPrimitive); ++a) {
        const GeoAttribute& from = source.attribute(eGeoAttribClassPrimitive, a);
        GeoAttribute* to = detail.attributeForWriting(eGeoAttribClassPrimitive, from.desc().name);
        if (!to) {
            continue;
        }
        for (int f = 0; f < newFaceCount; ++f) {
            geoCopyElement(*to, (std::size_t)(firstPrimitive + f), from, (std::size_t)parents[(std::size_t)f]);
        }
    }
    for (const std::string& name : source.groupNames(eGeoAttribClassPrimitive)) {
        const GeoElementGroup* from = source.findGroup(eGeoAttribClassPrimitive, name);
        GeoElementGroup& to = detail.addGroup(eGeoAttribClassPrimitive, name);
        for (int f = 0; f < newFaceCount; ++f) {
            if (from && from->contains(parents[(std::size_t)f])) {
                to.add(firstPrimitive + f);
            }
        }
    }

    // Vertices, face-varying.
    for (std::size_t c = 0; c < firstGroupChannel; ++c) {
        const GeoAttribute& from = *channels[c].attribute;
        GeoAttribute* to = detail.attributeForWriting(eGeoAttribClassVertex, from.desc().name);
        const std::unique_ptr<const StencilTable> stencils = createStencils(*refiner, depth, true, (int)c);
        if (!to || !stencils) {
            continue;
        }
        for (int f = 0; f < newFaceCount; ++f) {
            const GeoOffset firstVertex = detail.detail().primitiveVertices(firstPrimitive + f).start;
            const Far::ConstIndexArray values = last.GetFaceFVarValues(f, (int)c);
            for (int k = 0; k < values.size(); ++k) {
                stencilSources(*stencils, values[k], channels[c].representatives, &offsets, &weights);
                geoBlendElement(*to, (std::size_t)(firstVertex + k), from, offsets, weights);
            }
        }
    }

    // A new vertex is in a vertex group when the corner it takes most of its value from is.
    for (std::size_t g = 0; g < vertexGroups.size(); ++g) {
        const int c = (int)(firstGroupChannel + g);
        GeoElementGroup& to = detail.addGroup(eGeoAttribClassVertex, vertexGroupNames[g]);
        const std::unique_ptr<const StencilTable> stencils = createStencils(*refiner, depth, true, c);
        if (!vertexGroups[g] || !stencils) {
            continue;
        }
        for (int f = 0; f < newFaceCount; ++f) {
            const GeoOffset firstVertex = detail.detail().primitiveVertices(firstPrimitive + f).start;
            const Far::ConstIndexArray values = last.GetFaceFVarValues(f, c);
            for (int k = 0; k < values.size(); ++k) {
                stencilSources(*stencils, values[k], channels[(std::size_t)c].representatives, &offsets, &weights);
                const GeoOffset heaviest = heaviestSource(offsets, weights);
                if ((heaviest != GeoIndexMap::kInvalidOffset) && vertexGroups[g]->contains(heaviest)) {
                    to.add(firstVertex + k);
                }
            }
        }
    }

    // Both halves of a split edge stay in the edge's groups, at every level.
    for (const std::string& name : source.edgeGroupNames()) {
        const GeoEdgeGroup* from = source.findEdgeGroup(name);
        if (!from || from->empty()) {
            continue;
        }
        const Far::TopologyLevel& base = refiner->GetLevel(0);
        std::vector<char> inGroup((std::size_t)base.GetNumEdges(), 0);
        bool any = false;
        for (int e = 0; e < base.GetNumEdges(); ++e) {
            const Far::ConstIndexArray ends = base.GetEdgeVertices(e);
            if (from->contains(basePoints[(std::size_t)ends[0]], basePoints[(std::size_t)ends[1]])) {
                inGroup[(std::size_t)e] = 1;
                any = true;
            }
        }
        if (!any) {
            continue;
        }
        for (int level = 0; level < depth; ++level) {
            const Far::TopologyLevel& parent = refiner->GetLevel(level);
            std::vector<char> childInGroup((std::size_t)refiner->GetLevel(level + 1).GetNumEdges(), 0);
            for (int e = 0; e < parent.GetNumEdges(); ++e) {
                if (!inGroup[(std::size_t)e]) {
                    continue;
                }
                const Far::ConstIndexArray children = parent.GetEdgeChildEdges(e);
                for (int k = 0; k < children.size(); ++k) {
                    childInGroup[(std::size_t)children[k]] = 1;
                }
            }
            inGroup.swap(childInGroup);
        }
        GeoEdgeGroup* to = detail.edgeGroupForWriting(name);
        if (!to) {
            continue;
        }
        for (int e = 0; e < last.GetNumEdges(); ++e) {
            if (inGroup[(std::size_t)e]) {
                const Far::ConstIndexArray ends = last.GetEdgeVertices(e);
                to->add(firstPoint + ends[0], firstPoint + ends[1]);
            }
        }
    }

    GeoNormalsTargetEnum normalsTarget = eGeoNormalsTargetPoints;
    bool recomputeNormals = false;
    if (params.recomputeNormals) {
        for (GeoAttribClassEnum cls : { eGeoAttribClassVertex, eGeoAttribClassPoint, eGeoAttribClassPrimitive }) {
            if (source.findAttribute(cls, "N")) {
                normalsTarget = cls == eGeoAttribClassVertex ? eGeoNormalsTargetVertices
                    : cls == eGeoAttribClassPoint            ? eGeoNormalsTargetPoints
                                                             : eGeoNormalsTargetPrimitives;
                recomputeNormals = true;
                break;
            }
        }
    }

    detail.deletePrimitives(faces, true);

    if (recomputeNormals) {
        // The new faces are the last ones: deleting primitives keeps the others in order.
        const GeoIndex count = detail.detail().primitiveCount();
        GeoNormalsParams normals;
        normals.group = std::to_string(count - newFaceCount) + "-" + std::to_string(count - 1);
        normals.target = normalsTarget;
        if (!GeoNormals::computeNormals(detail, normals, errorMessage)) {
            return false;
        }
    }

    return true;
} // subdivideGeometry

GeoSubdivide::GeoSubdivide(NodePtr node)
    : NativeEffectBase(node)
{
}

GeoSubdivide::~GeoSubdivide()
{
}

const NodeSpec*
GeoSubdivide::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
GeoSubdivide::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    KnobStringPtr group = createKnob<KnobString>(tr("Group").toStdString());
    group->setName(kGeoSubdivideParamGroup);
    group->setHintToolTip(tr("The polygons to subdivide, as a group pattern: group names, element numbers, @attribute "
                             "comparisons, ! and ^. Empty subdivides every polygon.")
                              .toStdString());
    group->setAnimationEnabled(false);
    page->addKnob(group);
    _group = group;

    KnobChoicePtr algorithm = createKnob<KnobChoice>(tr("Algorithm").toStdString());
    algorithm->setName(kGeoSubdivideParamAlgorithm);
    algorithm->setHintToolTip(tr("The subdivision rule.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("catmullClark", tr("Catmull-Clark").toStdString(),
                                       tr("Smooths any polygons, turning each into quads.").toStdString()));
        options.push_back(ChoiceOption("loop", tr("Loop").toStdString(),
                                       tr("Smooths triangles into triangles. Every selected polygon must be a triangle.").toStdString()));
        options.push_back(ChoiceOption("bilinear", tr("Bilinear").toStdString(),
                                       tr("Splits polygons into quads without moving any point.").toStdString()));
        algorithm->populateChoices(options);
    }
    algorithm->setDefaultValue((int)eGeoSubdivideCatmullClark);
    algorithm->setAnimationEnabled(false);
    page->addKnob(algorithm);
    _algorithm = algorithm;

    KnobIntPtr depth = createKnob<KnobInt>(tr("Depth").toStdString());
    depth->setName(kGeoSubdivideParamDepth);
    depth->setHintToolTip(tr("How many times the polygons are refined. Each level multiplies the face count by about four.").toStdString());
    depth->setDefaultValue(1);
    depth->setMinimum(1);
    depth->setMaximum(4);
    depth->setDisplayMinimum(1);
    depth->setDisplayMaximum(4);
    depth->setAnimationEnabled(false);
    page->addKnob(depth);
    _depth = depth;

    KnobChoicePtr boundary = createKnob<KnobChoice>(tr("Boundary Interpolation").toStdString());
    boundary->setName(kGeoSubdivideParamBoundaryInterpolation);
    boundary->setHintToolTip(tr("How open borders are smoothed.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("edgesOnly", tr("Edges Only").toStdString(),
                                       tr("Borders are smoothed as curves, rounding off corners.").toStdString()));
        options.push_back(ChoiceOption("edgesAndCorners", tr("Edges And Corners").toStdString(),
                                       tr("Borders are smoothed as curves and corner points stay where they are.").toStdString()));
        boundary->populateChoices(options);
    }
    boundary->setDefaultValue((int)eGeoSubdivideBoundaryEdgesAndCorners);
    boundary->setAnimationEnabled(false);
    page->addKnob(boundary);
    _boundary = boundary;

    KnobChoicePtr vertexInterpolation = createKnob<KnobChoice>(tr("Vertex Interpolation").toStdString());
    vertexInterpolation->setName(kGeoSubdivideParamVertexInterpolation);
    vertexInterpolation->setHintToolTip(tr("How vertex attributes such as uv are smoothed where their values split at "
                                           "seams and along borders.")
                                            .toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("smooth", tr("Smooth").toStdString(),
                                       tr("Smoothed everywhere, seams and borders included.").toStdString()));
        options.push_back(ChoiceOption("cornersOnly", tr("Corners Only").toStdString(),
                                       tr("Smoothed, but seam corners stay put.").toStdString()));
        options.push_back(ChoiceOption("cornersPlus1", tr("Corners Plus 1").toStdString(),
                                       tr("Seam corners and the points where seams meet stay put.").toStdString()));
        options.push_back(ChoiceOption("cornersPlus2", tr("Corners Plus 2").toStdString(),
                                       tr("As Corners Plus 1, also keeping the corners of separate uv islands sharp.").toStdString()));
        options.push_back(ChoiceOption("boundaries", tr("Boundaries").toStdString(),
                                       tr("Linear along seams and borders, smoothed inside.").toStdString()));
        options.push_back(ChoiceOption("linear", tr("Linear").toStdString(),
                                       tr("Linear everywhere: the values are not smoothed.").toStdString()));
        vertexInterpolation->populateChoices(options);
    }
    vertexInterpolation->setDefaultValue((int)eGeoSubdivideVertexCornersPlus1);
    vertexInterpolation->setAnimationEnabled(false);
    page->addKnob(vertexInterpolation);
    _vertexInterpolation = vertexInterpolation;

    KnobBoolPtr recomputeNormals = createKnob<KnobBool>(tr("Recompute Normals").toStdString());
    recomputeNormals->setName(kGeoSubdivideParamRecomputeNormals);
    recomputeNormals->setHintToolTip(tr("When the input has N, computes it afresh from the subdivided faces, on the same "
                                        "points, vertices or primitives, instead of interpolating the old one.")
                                         .toStdString());
    recomputeNormals->setDefaultValue(true);
    recomputeNormals->setAnimationEnabled(false);
    page->addKnob(recomputeNormals);
    _recomputeNormals = recomputeNormals;
} // GeoSubdivide::initializeKnobs

StatusEnum
GeoSubdivide::renderGeometry(const GeoRenderActionArgs& args,
                             GeoDetailConstPtr* outputDetail,
                             std::string* errorMessage)
{
    GeoSubdivideParams params;
    if (const KnobStringPtr group = _group.lock()) {
        params.group = group->getValueAtTime(args.time, 0, args.view);
    }
    if (const KnobChoicePtr algorithm = _algorithm.lock()) {
        const int value = algorithm->getValueAtTime(args.time, 0, args.view);
        if ((value >= (int)eGeoSubdivideCatmullClark) && (value <= (int)eGeoSubdivideBilinear)) {
            params.algorithm = (GeoSubdivideAlgorithmEnum)value;
        }
    }
    if (const KnobIntPtr depth = _depth.lock()) {
        params.depth = depth->getValueAtTime(args.time, 0, args.view);
    }
    if (const KnobChoicePtr boundary = _boundary.lock()) {
        params.boundary = boundary->getValueAtTime(args.time, 0, args.view) == (int)eGeoSubdivideBoundaryEdgesOnly
            ? eGeoSubdivideBoundaryEdgesOnly
            : eGeoSubdivideBoundaryEdgesAndCorners;
    }
    if (const KnobChoicePtr vertexInterpolation = _vertexInterpolation.lock()) {
        const int value = vertexInterpolation->getValueAtTime(args.time, 0, args.view);
        if ((value >= (int)eGeoSubdivideVertexSmooth) && (value <= (int)eGeoSubdivideVertexLinear)) {
            params.vertexInterpolation = (GeoSubdivideVertexInterpolationEnum)value;
        }
    }
    if (const KnobBoolPtr recomputeNormals = _recomputeNormals.lock()) {
        params.recomputeNormals = recomputeNormals->getValueAtTime(args.time, 0, args.view);
    }

    return renderGeometryFromInput(args, 0, [&params](GeoDetailBuilder& detail, std::string* error) {
        std::string message;
        if (!subdivideGeometry(detail, params, &message)) {
            if (error) {
                *error = message;
            }

            return eStatusFailed;
        }

        return eStatusOK; }, outputDetail, errorMessage);
} // GeoSubdivide::renderGeometry

NATRON_NAMESPACE_EXIT
