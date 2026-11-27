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

#include "GeoMerge.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/Geo/GeoPacked.h"
#include "Engine/Geo/GeoTopology.h"

NATRON_NAMESPACE_ENTER

namespace {

const GeoAttribClassEnum kAllClasses[] = {
    eGeoAttribClassPoint,
    eGeoAttribClassVertex,
    eGeoAttribClassPrimitive,
    eGeoAttribClassDetail,
};

const char*
className(GeoAttribClassEnum cls)
{
    switch (cls) {
    case eGeoAttribClassPoint:
        return "point";
    case eGeoAttribClassVertex:
        return "vertex";
    case eGeoAttribClassPrimitive:
        return "primitive";
    case eGeoAttribClassDetail:
        return "detail";
    }

    return "";
}

// One merged attribute: its final description and the input whose default it carries.
struct MergedAttribute {
    GeoAttributeDesc desc;
    std::size_t defaultInput;
};

struct ClassUnion {
    std::vector<MergedAttribute> attributes;

    MergedAttribute* find(const std::string& name)
    {
        for (MergedAttribute& a : attributes) {
            if (a.desc.name == name) {
                return &a;
            }
        }

        return nullptr;
    }
};

std::vector<std::size_t>
sequence(std::size_t first,
         std::size_t count)
{
    std::vector<std::size_t> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = first + i;
    }

    return out;
}

bool
compatible(const GeoAttribute& src,
           const GeoAttributeDesc& dst)
{
    return src.isArray() == dst.isArray && (src.storage() == eGeoStorageString) == (dst.storage == eGeoStorageString);
}

// Where the elements of one appended input went.
struct AppendedInput {
    std::vector<std::size_t> srcOffsets[3];
    std::size_t dstBegin[3] = { 0, 0, 0 };
    std::vector<GeoOffset> pointRemap;
    std::vector<GeoOffset> vertexRemap;
    std::vector<GeoOffset> primitiveRemap;
};

AppendedInput
appendTopology(GeoDetailBuilder& builder,
               const GeoDetail& in)
{
    AppendedInput out;
    out.srcOffsets[eGeoAttribClassPoint] = geoLiveOffsets(in.pointMap());
    const std::vector<std::size_t>& points = out.srcOffsets[eGeoAttribClassPoint];
    out.pointRemap.assign(std::size_t(in.pointMap().offsetSize()), -1);
    const GeoOffset firstPoint = points.empty() ? builder.detail().pointMap().offsetSize() : builder.appendPoints(GeoIndex(points.size()));
    out.dstBegin[eGeoAttribClassPoint] = std::size_t(firstPoint);
    for (std::size_t i = 0; i < points.size(); ++i) {
        out.pointRemap[points[i]] = firstPoint + GeoOffset(i);
    }

    out.dstBegin[eGeoAttribClassVertex] = std::size_t(builder.detail().vertexMap().offsetSize());
    out.dstBegin[eGeoAttribClassPrimitive] = std::size_t(builder.detail().primitiveMap().offsetSize());
    out.vertexRemap.assign(std::size_t(in.vertexMap().offsetSize()), -1);
    out.primitiveRemap.assign(std::size_t(in.primitiveMap().offsetSize()), -1);
    std::vector<std::size_t>& vertices = out.srcOffsets[eGeoAttribClassVertex];
    std::vector<std::size_t>& primitives = out.srcOffsets[eGeoAttribClassPrimitive];
    primitives = geoLiveOffsets(in.primitiveMap());

    // Primitives go in runs of one type, so the appended ones keep the input's order.
    std::size_t i = 0;
    std::vector<std::int32_t> counts;
    std::vector<GeoOffset> corners;
    while (i < primitives.size()) {
        const GeoPrimitiveTypeEnum type = in.primitiveType(GeoOffset(primitives[i]));
        counts.clear();
        corners.clear();
        const std::size_t runBegin = i;
        for (; i < primitives.size() && in.primitiveType(GeoOffset(primitives[i])) == type; ++i) {
            const GeoVertexRange range = in.primitiveVertices(GeoOffset(primitives[i]));
            counts.push_back(std::int32_t(range.count));
            for (GeoOffset v = range.start; v < range.start + range.count; ++v) {
                out.vertexRemap[std::size_t(v)] = GeoOffset(out.dstBegin[eGeoAttribClassVertex] + vertices.size());
                vertices.push_back(std::size_t(v));
                corners.push_back(out.pointRemap[std::size_t(in.vertexPoint(v))]);
            }
        }
        const GeoOffset first = builder.appendPrimitives(type, counts, corners);
        for (std::size_t k = runBegin; k < i; ++k) {
            out.primitiveRemap[primitives[k]] = first + GeoOffset(k - runBegin);
        }
    }

    return out;
}

void
remapPackedDetails(GeoDetailBuilder& builder,
                   const GeoDetail& in,
                   const AppendedInput& appended)
{
    const GeoAttribute* source = in.findAttribute(eGeoAttribClassPrimitive, kGeoPackedDetailAttr);
    if (!source || source->storage() != eGeoStorageInt32 || source->isArray()) {
        return;
    }
    std::vector<std::pair<std::size_t, std::int32_t>> remapped;
    for (std::size_t prim : appended.srcOffsets[eGeoAttribClassPrimitive]) {
        const std::int32_t index = source->get<std::int32_t>(prim);
        if (index <= 0 || std::size_t(index) > in.sharedDetails().size()) {
            continue;
        }
        const std::int32_t merged = builder.addSharedDetail(in.sharedDetails()[std::size_t(index - 1)]) + 1;
        remapped.emplace_back(std::size_t(appended.primitiveRemap[prim]), merged);
    }
    GeoAttribute* target = builder.attributeForWriting(eGeoAttribClassPrimitive, kGeoPackedDetailAttr);
    if (!target || target->storage() != eGeoStorageInt32 || target->isArray()) {
        return;
    }
    for (const auto& [prim, index] : remapped) {
        target->set<std::int32_t>(prim, index);
    }
}

void
mergeGroups(GeoDetailBuilder& builder,
            const GeoDetail& in,
            const AppendedInput& appended)
{
    const std::pair<GeoAttribClassEnum, const std::vector<GeoOffset>*> classes[] = {
        { eGeoAttribClassPoint, &appended.pointRemap },
        { eGeoAttribClassVertex, &appended.vertexRemap },
        { eGeoAttribClassPrimitive, &appended.primitiveRemap },
    };
    for (const auto& [cls, remap] : classes) {
        for (const std::string& name : in.groupNames(cls)) {
            const GeoElementGroup* source = in.findGroup(cls, name);
            if (!source) {
                continue;
            }
            std::vector<GeoOffset> members;
            source->forEachLiveMember(in.indexMap(cls), [&](GeoIndex /*index*/, GeoOffset offset) {
                const GeoOffset mapped = (*remap)[std::size_t(offset)];
                if (mapped >= 0) {
                    members.push_back(mapped);
                }
            });
            builder.addGroup(cls, name).add(members);
        }
    }
    for (const std::string& name : in.edgeGroupNames()) {
        const GeoEdgeGroup* source = in.findEdgeGroup(name);
        if (!source) {
            continue;
        }
        std::vector<GeoEdge> edges;
        for (const GeoEdge& e : source->edges()) {
            if (!in.pointMap().isLive(e.p0) || !in.pointMap().isLive(e.p1)) {
                continue;
            }
            edges.emplace_back(appended.pointRemap[std::size_t(e.p0)], appended.pointRemap[std::size_t(e.p1)]);
        }
        builder.addEdgeGroup(name).add(edges);
    }
}
} // anonymous namespace

GeoDetail
GeoMerge::merge(std::span<const GeoDetailConstPtr> inputs,
                std::vector<std::string>* warnings)
{
    std::vector<const GeoDetail*> details;
    for (const GeoDetailConstPtr& input : inputs) {
        if (input) {
            details.push_back(input.get());
        }
    }
    if (details.empty()) {
        return GeoDetail();
    }
    if (details.size() == 1) {
        return *details.front();
    }

    bool precision64 = false;
    ClassUnion unions[kGeoAttribClassCount];
    for (std::size_t k = 0; k < details.size(); ++k) {
        const GeoDetail& in = *details[k];
        precision64 = precision64 || in.pointPrecision64();
        for (GeoAttribClassEnum cls : kAllClasses) {
            for (std::size_t i = 0; i < in.attributeCount(cls); ++i) {
                const GeoAttributeDesc& desc = in.attribute(cls, i).desc();
                MergedAttribute* merged = unions[cls].find(desc.name);
                if (!merged) {
                    unions[cls].attributes.push_back({ desc, k });
                    continue;
                }
                if (merged->desc == desc) {
                    continue;
                }
                const std::optional<GeoAttributeDesc> promoted = geoPromotedDesc(merged->desc, desc);
                if (promoted) {
                    merged->desc = *promoted;
                } else if (warnings) {
                    warnings->push_back(std::string("The ") + className(cls) + " attribute \"" + desc.name + "\" of input " + std::to_string(k + 1) + " cannot be combined with that of input " + std::to_string(merged->defaultInput + 1) + "; its values are dropped.");
                }
            }
        }
    }
    if (MergedAttribute* p = unions[eGeoAttribClassPoint].find(GeoDetail::kPositionName)) {
        p->desc = GeoDetail::positionDesc(precision64);
    }

    GeoDetailBuilder builder(*details.front());
    builder.setPointPrecision64(precision64);
    for (GeoAttribClassEnum cls : kAllClasses) {
        for (const MergedAttribute& merged : unions[cls].attributes) {
            const GeoAttribute* existing = builder.detail().findAttribute(cls, merged.desc.name);
            if (existing) {
                if (!(existing->desc() == merged.desc)) {
                    geoConformAttribute(builder, cls, merged.desc);
                }
                continue;
            }
            const GeoAttribute& source = *details[merged.defaultInput]->findAttribute(cls, merged.desc.name);
            GeoAttribute& added = builder.addAttribute(cls, merged.desc);
            if (cls == eGeoAttribClassDetail) {
                const std::size_t zero = 0;
                geoCopyAttributeElements(source, std::span<const std::size_t>(&zero, 1), added, std::span<const std::size_t>(&zero, 1));
            } else {
                geoAdoptDefault(source, added);
            }
        }
    }

    for (std::size_t k = 1; k < details.size(); ++k) {
        const GeoDetail& in = *details[k];
        const AppendedInput appended = appendTopology(builder, in);
        for (GeoAttribClassEnum cls : { eGeoAttribClassPoint, eGeoAttribClassVertex, eGeoAttribClassPrimitive }) {
            const std::vector<std::size_t>& srcOffsets = appended.srcOffsets[cls];
            if (srcOffsets.empty()) {
                continue;
            }
            const std::vector<std::size_t> dstOffsets = sequence(appended.dstBegin[cls], srcOffsets.size());
            for (const MergedAttribute& merged : unions[cls].attributes) {
                GeoAttribute* dst = builder.attributeForWriting(cls, merged.desc.name);
                if (!dst) {
                    continue;
                }
                const GeoAttribute* src = in.findAttribute(cls, merged.desc.name);
                if (src && compatible(*src, merged.desc)) {
                    geoCopyAttributeElements(*src, srcOffsets, *dst, dstOffsets);
                } else {
                    const GeoAttribute& defaults = *details[merged.defaultInput]->findAttribute(cls, merged.desc.name);
                    geoFillWithDefaultOf(defaults, *dst, dstOffsets.front(), dstOffsets.back() + 1);
                }
            }
        }
        remapPackedDetails(builder, in, appended);
        mergeGroups(builder, in, appended);
    }

    return builder.finish();
}

GeoMerge::GeoMerge(NodePtr node)
    : NativeEffectBase(node)
{
}

GeoMerge::~GeoMerge()
{
}

const NodeSpec*
GeoMerge::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
GeoMerge::initializeKnobs()
{
}

StatusEnum
GeoMerge::renderGeometry(const GeoRenderActionArgs& args,
                         GeoDetailConstPtr* outputDetail,
                         std::string* errorMessage)
{
    std::vector<GeoDetailConstPtr> inputs;
    for (int i = 0; i < kGeoMergeInputCount; ++i) {
        inputs.push_back(args.getInputDetail(i));
    }
    std::vector<std::string> warnings;
    try {
        *outputDetail = std::make_shared<const GeoDetail>(merge(inputs, &warnings));
    } catch (const std::exception& e) {
        if (errorMessage) {
            *errorMessage = e.what();
        }

        return eStatusFailed;
    }
    if (warnings.empty()) {
        clearPersistentMessage(false);
    } else {
        std::string text;
        for (const std::string& w : warnings) {
            text += (text.empty() ? "" : "\n") + w;
        }
        setPersistentMessage(eMessageTypeWarning, text);
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
