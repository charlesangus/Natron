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

#include "Serialization/Yaml/RotoYaml.h"

#include <cstring>

#include "Serialization/Yaml/YamlPrivate.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {
namespace YamlDetail {

    namespace {

        const char* const kCoordinateNames[6] = { "x", "y", "lx", "ly", "rx", "ry" };
        const char* const kFeatherCoordinateNames[6] = { "fx", "fy", "flx", "fly", "frx", "fry" };

        const CurveSerialization&
        pointCurve(const BezierCPSerialization& point,
                   int coordinate)
        {
            switch (coordinate) {
            case 0:
                return point.xCurve;
            case 1:
                return point.yCurve;
            case 2:
                return point.leftXCurve;
            case 3:
                return point.leftYCurve;
            case 4:
                return point.rightXCurve;
            default:
                return point.rightYCurve;
            }
        }

        CurveSerialization&
        pointCurve(BezierCPSerialization& point,
                   int coordinate)
        {
            return const_cast<CurveSerialization&>(pointCurve(static_cast<const BezierCPSerialization&>(point), coordinate));
        }

        double&
        pointStatic(BezierCPSerialization& point,
                    int coordinate)
        {
            switch (coordinate) {
            case 0:
                return point.x;
            case 1:
                return point.y;
            case 2:
                return point.leftX;
            case 3:
                return point.leftY;
            case 4:
                return point.rightX;
            default:
                return point.rightY;
            }
        }

        double
        pointStatic(const BezierCPSerialization& point,
                    int coordinate)
        {
            return pointStatic(const_cast<BezierCPSerialization&>(point), coordinate);
        }

        bool
        sameBits(double a,
                 double b)
        {
            return std::memcmp(&a, &b, sizeof(double)) == 0;
        }

        std::string
        blobPath(const BlobEncodeContext& blobs,
                 const std::string& itemName,
                 const char* kind)
        {
            return uniqueBlobPath("blobs/" + sanitizeBlobPathComponent(blobs.nodeFullName) + "/" + sanitizeBlobPathComponent(itemName) + "." + kind + ".bin", blobs.usedPaths);
        }

        void
        emitInterpolationKey(YAML::Emitter& em,
                             int interpolation)
        {
            emitKey(em, "interp");
            em << YAML::Value << std::string(keyframeInterpolationToken(interpolation));
        }

        int
        readInterpolationToken(const YAML::Node& node,
                               const std::string& context)
        {
            std::optional<int> interpolation = keyframeInterpolationFromToken(readString(node, context));
            if (!interpolation) {
                fail(node, context, "expected an interpolation token (K, L, S, R, C, H, F, X or N)");
            }

            return *interpolation;
        }

        void
        emitItemCommon(YAML::Emitter& em,
                       const RotoItemSerialization& item,
                       const std::string& expectedParent)
        {
            emitKey(em, "name");
            em << YAML::Value;
            emitString(em, item.name);
            if (item.label != item.name) {
                emitKey(em, "label");
                em << YAML::Value;
                emitString(em, item.label);
            }
            emitKey(em, "activated");
            em << YAML::Value;
            emitBool(em, item.activated);
            if (item.locked) {
                emitKey(em, "locked");
                em << YAML::Value;
                emitBool(em, true);
            }
            if (item.parentLayerName != expectedParent) {
                emitKey(em, "parent");
                em << YAML::Value;
                emitString(em, item.parentLayerName);
            }
        }

        void
        readItemCommon(MapReader& reader,
                       RotoItemSerialization* item,
                       const std::string& expectedParent)
        {
            const std::string& context = reader.context();
            item->name = readString(reader.get("name"), context + ".name");
            if (YAML::Node label = reader.get("label")) {
                item->label = readString(label, context + ".label");
            } else {
                item->label = item->name;
            }
            item->activated = readBool(reader.get("activated"), context + ".activated");
            if (YAML::Node locked = reader.get("locked")) {
                item->locked = readBool(locked, context + ".locked");
            }
            if (YAML::Node parent = reader.get("parent")) {
                item->parentLayerName = readString(parent, context + ".parent");
            } else {
                item->parentLayerName = expectedParent;
            }
        }

        void
        emitDrawableCommon(YAML::Emitter& em,
                           const RotoDrawableItemSerialization& item)
        {
            emitKey(em, "overlay");
            em << YAML::Value << YAML::Flow << YAML::BeginSeq;
            for (int i = 0; i < 4; ++i) {
                emitDouble(em, item.overlayColor[i]);
            }
            em << YAML::EndSeq;
            if (!item.knobs.empty()) {
                emitKey(em, "knobs");
                em << YAML::Value;
                emitKnobMap(em, item.knobs);
            }
        }

        void
        readDrawableCommon(MapReader& reader,
                           RotoDrawableItemSerialization* item)
        {
            const std::string& context = reader.context();
            std::vector<double> overlay = readDoubleVector(reader.get("overlay"), context + ".overlay", 4);
            for (int i = 0; i < 4; ++i) {
                item->overlayColor[i] = overlay[i];
            }
            if (YAML::Node knobs = reader.get("knobs")) {
                item->knobs = readKnobMap(knobs, context + ".knobs", reader.warnings());
            }
        }

        void
        addPointColumns(Table* table,
                        const std::vector<const BezierCPSerialization*>& points,
                        const char* const names[6],
                        const char* brokenName)
        {
            for (int c = 0; c < 6; ++c) {
                std::vector<double> values;
                values.reserve(points.size());
                for (const BezierCPSerialization* point : points) {
                    values.push_back(pointStatic(*point, c));
                }
                table->addF64(names[c], std::move(values));
            }
            bool anyBroken = false;
            for (const BezierCPSerialization* point : points) {
                anyBroken = anyBroken || point->broken;
            }
            if (anyBroken) {
                std::vector<std::uint8_t> broken;
                broken.reserve(points.size());
                for (const BezierCPSerialization* point : points) {
                    broken.push_back(point->broken ? 1 : 0);
                }
                table->addU8(brokenName, std::move(broken));
            }
        }

        void
        readPointColumns(const Table& table,
                         std::vector<BezierCPSerialization>* points,
                         const char* const names[6],
                         const char* brokenName)
        {
            points->resize(table.rows);
            for (int c = 0; c < 6; ++c) {
                const std::vector<double> values = table.f64(names[c]);
                for (std::size_t i = 0; i < table.rows; ++i) {
                    pointStatic((*points)[i], c) = values[i];
                }
            }
            const std::vector<std::uint8_t> broken = table.u8(brokenName);
            for (std::size_t i = 0; i < table.rows; ++i) {
                (*points)[i].broken = broken[i] != 0;
            }
        }

        bool
        isAnimated(const std::vector<const BezierCPSerialization*>& points)
        {
            for (const BezierCPSerialization* point : points) {
                for (int c = 0; c < 6; ++c) {
                    if (!pointCurve(*point, c).keyframes.empty()) {
                        return true;
                    }
                }
            }

            return false;
        }

        // One row per keyframe and control point holds every coordinate of the point (and of its
        // feather point): this needs every curve to have keys at the same times, one interpolation per
        // point and key, and no stored derivatives. Anything else stays inline, losslessly.
        bool
        buildShapeTable(const std::vector<const BezierCPSerialization*>& points,
                        const std::vector<const BezierCPSerialization*>& feathers,
                        Table* table)
        {
            if (points.empty()) {
                return false;
            }
            const std::vector<KeyFrameSerialization>& reference = points[0]->xCurve.keyframes;
            const std::size_t keyCount = reference.size();
            if (keyCount == 0) {
                return false;
            }
            const bool withFeather = !feathers.empty();
            for (std::size_t p = 0; p < points.size(); ++p) {
                for (int set = 0; set < (withFeather ? 2 : 1); ++set) {
                    const BezierCPSerialization& point = (set == 0) ? *points[p] : *feathers[p];
                    for (int c = 0; c < 6; ++c) {
                        const std::vector<KeyFrameSerialization>& keys = pointCurve(point, c).keyframes;
                        if (keys.size() != keyCount) {
                            return false;
                        }
                        for (std::size_t k = 0; k < keyCount; ++k) {
                            const int interpolation = points[p]->xCurve.keyframes[k].interpolation;
                            if (!sameBits(keys[k].time, reference[k].time) || (keys[k].interpolation != interpolation) || !isTabulableInterpolation(interpolation)) {
                                return false;
                            }
                        }
                    }
                }
            }

            const std::size_t rows = keyCount * points.size();
            std::vector<double> time;
            std::vector<std::int32_t> cp;
            std::vector<std::uint8_t> interp;
            std::vector<std::vector<double>> coordinates(withFeather ? 12 : 6);
            time.reserve(rows);
            cp.reserve(rows);
            interp.reserve(rows);
            for (std::vector<double>& column : coordinates) {
                column.reserve(rows);
            }
            for (std::size_t k = 0; k < keyCount; ++k) {
                for (std::size_t p = 0; p < points.size(); ++p) {
                    time.push_back(reference[k].time);
                    cp.push_back((std::int32_t)p);
                    for (int c = 0; c < 6; ++c) {
                        coordinates[c].push_back(pointCurve(*points[p], c).keyframes[k].value);
                        if (withFeather) {
                            coordinates[6 + c].push_back(pointCurve(*feathers[p], c).keyframes[k].value);
                        }
                    }
                    interp.push_back((std::uint8_t)points[p]->xCurve.keyframes[k].interpolation);
                }
            }
            table->rows = rows;
            table->addF64("time", std::move(time));
            table->addI32("cp", std::move(cp));
            for (int c = 0; c < 6; ++c) {
                table->addF64(kCoordinateNames[c], std::move(coordinates[c]));
            }
            if (withFeather) {
                for (int c = 0; c < 6; ++c) {
                    table->addF64(kFeatherCoordinateNames[c], std::move(coordinates[6 + c]));
                }
            }
            table->addU8("interp", std::move(interp));

            return true;
        }

        void
        readShapeTable(const Table& table,
                       std::vector<BezierCPSerialization>* points,
                       std::vector<BezierCPSerialization>* feathers,
                       const YAML::Node& node,
                       const std::string& context)
        {
            if (table.rows == 0) {
                return;
            }
            const std::size_t pointCount = points->size();
            if ((pointCount == 0) || ((table.rows % pointCount) != 0)) {
                fail(node, context, "a shape table of " + std::to_string(table.rows) + " rows does not fit " + std::to_string(pointCount) + " control points");
            }
            const bool withFeather = (feathers->size() == pointCount) && table.find(kFeatherCoordinateNames[0]);
            const std::vector<double> time = table.f64("time");
            const std::vector<std::int32_t> cp = table.i32("cp");
            const std::vector<std::uint8_t> interp = table.u8("interp", kYamlKeyframeLinear);
            std::vector<std::vector<double>> coordinates;
            for (int c = 0; c < 6; ++c) {
                coordinates.push_back(table.f64(kCoordinateNames[c]));
            }
            for (int c = 0; c < 6; ++c) {
                coordinates.push_back(table.f64(kFeatherCoordinateNames[c]));
            }
            for (std::size_t row = 0; row < table.rows; ++row) {
                const std::size_t p = row % pointCount;
                if (cp[row] != (std::int32_t)p) {
                    fail(node, context, "row " + std::to_string(row) + " of the shape table is for control point " + std::to_string(cp[row]) + ", expected " + std::to_string(p));
                }
                KeyFrameSerialization key;
                key.time = time[row];
                key.interpolation = checkedInterpolation(interp[row], node, context);
                for (int c = 0; c < 6; ++c) {
                    key.value = coordinates[c][row];
                    pointCurve((*points)[p], c).keyframes.push_back(key);
                    if (withFeather) {
                        key.value = coordinates[6 + c][row];
                        pointCurve((*feathers)[p], c).keyframes.push_back(key);
                    }
                }
            }
        }

        void
        emitPointCurves(YAML::Emitter& em,
                        const std::vector<const BezierCPSerialization*>& points)
        {
            em << YAML::BeginSeq;
            for (const BezierCPSerialization* point : points) {
                em << YAML::Flow << YAML::BeginMap;
                for (int c = 0; c < 6; ++c) {
                    const CurveSerialization& curve = pointCurve(*point, c);
                    if (!curve.keyframes.empty()) {
                        emitKey(em, kCoordinateNames[c]);
                        em << YAML::Value;
                        emitCurve(em, curve);
                    }
                }
                em << YAML::EndMap;
            }
            em << YAML::EndSeq;
        }

        void
        readPointCurves(const YAML::Node& node,
                        std::vector<BezierCPSerialization>* points,
                        const std::string& context,
                        YamlWarnings* warnings)
        {
            if (!node.IsSequence() || (node.size() != points->size())) {
                fail(node, context, "expected one entry per point (" + std::to_string(points->size()) + ")");
            }
            for (std::size_t p = 0; p < points->size(); ++p) {
                const std::string pointContext = context + "[" + std::to_string(p) + "]";
                MapReader reader(node[p], pointContext, warnings);
                for (int c = 0; c < 6; ++c) {
                    if (YAML::Node curve = reader.get(kCoordinateNames[c])) {
                        pointCurve((*points)[p], c) = readCurve(curve, pointContext + "." + kCoordinateNames[c]);
                    }
                }
                reader.finish();
            }
        }

        // Points that follow a track, by index into the control or feather points: the track's
        // tracker and name, the offsets of the point and its tangents from the track's centre, and
        // the centre used while the track cannot be resolved.
        void
        emitTrackLinks(YAML::Emitter& em,
                       const std::vector<const BezierCPSerialization*>& points,
                       const std::vector<const BezierCPSerialization*>& feathers)
        {
            bool any = false;
            for (int set = 0; set < 2; ++set) {
                for (const BezierCPSerialization* point : (set == 0) ? points : feathers) {
                    any = any || point->hasTrackLink;
                }
            }
            if (!any) {
                return;
            }
            emitKey(em, "trackLinks");
            em << YAML::Value << YAML::BeginSeq;
            for (int set = 0; set < 2; ++set) {
                const std::vector<const BezierCPSerialization*>& list = (set == 0) ? points : feathers;
                for (std::size_t i = 0; i < list.size(); ++i) {
                    const BezierCPSerialization& point = *list[i];
                    if (!point.hasTrackLink) {
                        continue;
                    }
                    em << YAML::BeginMap;
                    emitKey(em, "index");
                    em << YAML::Value;
                    emitInt(em, static_cast<long long>(i));
                    if (set == 1) {
                        emitKey(em, "feather");
                        em << YAML::Value;
                        emitBool(em, true);
                    }
                    emitKey(em, "tracker");
                    em << YAML::Value;
                    emitString(em, point.trackLinkTracker);
                    emitKey(em, "track");
                    em << YAML::Value;
                    emitString(em, point.trackLinkTrack);
                    emitKey(em, "offsets");
                    em << YAML::Value << YAML::Flow << YAML::BeginSeq;
                    const double offsets[6] = { point.trackOffsetX, point.trackOffsetY, point.trackOffsetLeftX, point.trackOffsetLeftY, point.trackOffsetRightX, point.trackOffsetRightY };
                    for (double v : offsets) {
                        emitDouble(em, v);
                    }
                    em << YAML::EndSeq;
                    emitDoublePair(em, "frozen", point.trackFrozenX, point.trackFrozenY);
                    em << YAML::EndMap;
                }
            }
            em << YAML::EndSeq;
        }

        void
        readTrackLinks(const YAML::Node& node,
                       std::vector<BezierCPSerialization>* points,
                       std::vector<BezierCPSerialization>* feathers,
                       const std::string& context,
                       YamlWarnings* warnings)
        {
            if (!node.IsSequence()) {
                fail(node, context, "expected a sequence");
            }
            for (std::size_t i = 0; i < node.size(); ++i) {
                const std::string entryContext = context + "[" + std::to_string(i) + "]";
                MapReader reader(node[i], entryContext, warnings);
                const int index = readInt(reader.get("index"), entryContext + ".index");
                bool feather = false;
                if (YAML::Node featherNode = reader.get("feather")) {
                    feather = readBool(featherNode, entryContext + ".feather");
                }
                std::vector<BezierCPSerialization>* list = feather ? feathers : points;
                if ((index < 0) || (static_cast<std::size_t>(index) >= list->size())) {
                    fail(node[i], entryContext, "point index out of range");
                }
                BezierCPSerialization& point = (*list)[index];
                point.hasTrackLink = true;
                point.trackLinkTracker = readString(reader.get("tracker"), entryContext + ".tracker");
                point.trackLinkTrack = readString(reader.get("track"), entryContext + ".track");
                const std::vector<double> offsets = readDoubleVector(reader.get("offsets"), entryContext + ".offsets", 6);
                point.trackOffsetX = offsets[0];
                point.trackOffsetY = offsets[1];
                point.trackOffsetLeftX = offsets[2];
                point.trackOffsetLeftY = offsets[3];
                point.trackOffsetRightX = offsets[4];
                point.trackOffsetRightY = offsets[5];
                const std::vector<double> frozen = readDoubleVector(reader.get("frozen"), entryContext + ".frozen", 2);
                point.trackFrozenX = frozen[0];
                point.trackFrozenY = frozen[1];
                reader.finish();
            }
        }

        void
        emitBezier(YAML::Emitter& em,
                   const BezierSerialization& bezier,
                   const std::string& expectedParent,
                   const BlobEncodeContext& blobs)
        {
            std::vector<const BezierCPSerialization*> points;
            for (const BezierCPSerialization& point : bezier.controlPoints) {
                points.push_back(&point);
            }
            std::vector<const BezierCPSerialization*> feathers;
            for (const BezierCPSerialization& point : bezier.featherPoints) {
                feathers.push_back(&point);
            }
            const bool featherAligned = !feathers.empty() && (feathers.size() == points.size());

            em << YAML::BeginMap;
            emitItemCommon(em, bezier, expectedParent);
            emitDrawableCommon(em, bezier);
            emitKey(em, "closed");
            em << YAML::Value;
            emitBool(em, bezier.closed);
            emitKey(em, "open");
            em << YAML::Value;
            emitBool(em, bezier.isOpenBezier);

            Table pointsTable;
            pointsTable.rows = points.size();
            addPointColumns(&pointsTable, points, kCoordinateNames, "broken");
            if (featherAligned) {
                addPointColumns(&pointsTable, feathers, kFeatherCoordinateNames, "fbroken");
            }
            emitKey(em, "points");
            em << YAML::Value;
            emitTable(em, pointsTable, blobPath(blobs, bezier.name, "points"), *blobs.sink);

            if (!feathers.empty() && !featherAligned) {
                Table featherTable;
                featherTable.rows = feathers.size();
                addPointColumns(&featherTable, feathers, kCoordinateNames, "broken");
                emitKey(em, "featherPoints");
                em << YAML::Value;
                emitTable(em, featherTable, blobPath(blobs, bezier.name, "feather"), *blobs.sink);
            }

            if (isAnimated(points) || isAnimated(feathers)) {
                Table shape;
                const bool tabled = (feathers.empty() || featherAligned) && buildShapeTable(points, featherAligned ? feathers : std::vector<const BezierCPSerialization*>(), &shape);
                if (tabled) {
                    emitKey(em, "shape");
                    em << YAML::Value;
                    emitTable(em, shape, blobPath(blobs, bezier.name, "shape"), *blobs.sink);
                } else {
                    emitKey(em, "curves");
                    em << YAML::Value;
                    emitPointCurves(em, points);
                    if (!feathers.empty()) {
                        emitKey(em, "featherCurves");
                        em << YAML::Value;
                        emitPointCurves(em, feathers);
                    }
                }
            }
            emitTrackLinks(em, points, feathers);
            em << YAML::EndMap;
        }

        void
        readBezier(const YAML::Node& node,
                   BezierSerialization* bezier,
                   const std::string& expectedParent,
                   const BlobSource& source,
                   const std::string& context,
                   YamlWarnings* warnings)
        {
            MapReader reader(node, context, warnings);
            readItemCommon(reader, bezier, expectedParent);
            readDrawableCommon(reader, bezier);
            bezier->closed = readBool(reader.get("closed"), context + ".closed");
            bezier->isOpenBezier = readBool(reader.get("open"), context + ".open");

            std::vector<BezierCPSerialization> points;
            std::vector<BezierCPSerialization> feathers;
            if (YAML::Node pointsNode = reader.get("points")) {
                Table table = readTable(pointsNode, source, context + ".points", warnings);
                readPointColumns(table, &points, kCoordinateNames, "broken");
                if (table.find(kFeatherCoordinateNames[0])) {
                    readPointColumns(table, &feathers, kFeatherCoordinateNames, "fbroken");
                }
            }
            if (YAML::Node featherNode = reader.get("featherPoints")) {
                Table table = readTable(featherNode, source, context + ".featherPoints", warnings);
                readPointColumns(table, &feathers, kCoordinateNames, "broken");
            }
            if (YAML::Node shape = reader.get("shape")) {
                Table table = readTable(shape, source, context + ".shape", warnings);
                readShapeTable(table, &points, &feathers, shape, context + ".shape");
            }
            if (YAML::Node curves = reader.get("curves")) {
                readPointCurves(curves, &points, context + ".curves", warnings);
            }
            if (YAML::Node featherCurves = reader.get("featherCurves")) {
                readPointCurves(featherCurves, &feathers, context + ".featherCurves", warnings);
            }
            if (YAML::Node trackLinks = reader.get("trackLinks")) {
                readTrackLinks(trackLinks, &points, &feathers, context + ".trackLinks", warnings);
            }
            reader.finish();
            bezier->controlPoints.assign(points.begin(), points.end());
            bezier->featherPoints.assign(feathers.begin(), feathers.end());
        }

        // The three curves of each pen-down movement merged into one row per sample: they need the
        // same key times and one interpolation per sample, with no stored derivatives.
        bool
        buildStrokeTable(const RotoStrokeItemSerialization& stroke,
                         Table* table,
                         std::vector<long long>* segments,
                         std::optional<int>* uniformInterpolation)
        {
            std::size_t rows = 0;
            for (const RotoStrokeCurvesSerialization& curves : stroke.strokes) {
                const std::vector<KeyFrameSerialization>& x = curves.xCurve.keyframes;
                const std::vector<KeyFrameSerialization>& y = curves.yCurve.keyframes;
                const std::vector<KeyFrameSerialization>& pressure = curves.pressureCurve.keyframes;
                if ((y.size() != x.size()) || (pressure.size() != x.size())) {
                    return false;
                }
                for (std::size_t k = 0; k < x.size(); ++k) {
                    const int interpolation = x[k].interpolation;
                    if (!sameBits(y[k].time, x[k].time) || !sameBits(pressure[k].time, x[k].time) || (y[k].interpolation != interpolation) || (pressure[k].interpolation != interpolation) || !isTabulableInterpolation(interpolation)) {
                        return false;
                    }
                }
                rows += x.size();
            }

            std::vector<double> x, y, pressure, time;
            std::vector<std::uint8_t> interpolations;
            x.reserve(rows);
            y.reserve(rows);
            pressure.reserve(rows);
            time.reserve(rows);
            interpolations.reserve(rows);
            bool uniform = true;
            for (const RotoStrokeCurvesSerialization& curves : stroke.strokes) {
                segments->push_back((long long)curves.xCurve.keyframes.size());
                for (std::size_t k = 0; k < curves.xCurve.keyframes.size(); ++k) {
                    x.push_back(curves.xCurve.keyframes[k].value);
                    y.push_back(curves.yCurve.keyframes[k].value);
                    pressure.push_back(curves.pressureCurve.keyframes[k].value);
                    time.push_back(curves.xCurve.keyframes[k].time);
                    interpolations.push_back((std::uint8_t)curves.xCurve.keyframes[k].interpolation);
                    uniform = uniform && (interpolations.back() == interpolations.front());
                }
            }
            table->rows = rows;
            table->addF64("x", std::move(x));
            table->addF64("y", std::move(y));
            table->addF64("pressure", std::move(pressure));
            table->addF64("time", std::move(time));
            if (!interpolations.empty()) {
                if (uniform) {
                    *uniformInterpolation = interpolations.front();
                } else {
                    table->addU8("interp", std::move(interpolations));
                }
            }

            return true;
        }

        void
        emitStroke(YAML::Emitter& em,
                   const RotoStrokeItemSerialization& stroke,
                   const std::string& expectedParent,
                   const BlobEncodeContext& blobs)
        {
            em << YAML::BeginMap;
            emitItemCommon(em, stroke, expectedParent);
            emitDrawableCommon(em, stroke);
            emitKey(em, "brush");
            em << YAML::Value;
            emitInt(em, stroke.brushType);

            Table table;
            std::vector<long long> segments;
            std::optional<int> uniformInterpolation;
            if (buildStrokeTable(stroke, &table, &segments, &uniformInterpolation)) {
                emitKey(em, "segments");
                em << YAML::Value << YAML::Flow << YAML::BeginSeq;
                for (long long count : segments) {
                    emitInt(em, count);
                }
                em << YAML::EndSeq;
                if (uniformInterpolation) {
                    emitInterpolationKey(em, *uniformInterpolation);
                }
                emitKey(em, "samples");
                em << YAML::Value;
                emitTable(em, table, blobPath(blobs, stroke.name, "stroke"), *blobs.sink);
            } else {
                emitKey(em, "curves");
                em << YAML::Value << YAML::BeginSeq;
                for (const RotoStrokeCurvesSerialization& curves : stroke.strokes) {
                    em << YAML::Flow << YAML::BeginMap;
                    emitKey(em, "x");
                    em << YAML::Value;
                    emitCurve(em, curves.xCurve);
                    emitKey(em, "y");
                    em << YAML::Value;
                    emitCurve(em, curves.yCurve);
                    emitKey(em, "pressure");
                    em << YAML::Value;
                    emitCurve(em, curves.pressureCurve);
                    em << YAML::EndMap;
                }
                em << YAML::EndSeq;
            }
            em << YAML::EndMap;
        }

        void
        readStroke(const YAML::Node& node,
                   RotoStrokeItemSerialization* stroke,
                   const std::string& expectedParent,
                   const BlobSource& source,
                   const std::string& context,
                   YamlWarnings* warnings)
        {
            MapReader reader(node, context, warnings);
            readItemCommon(reader, stroke, expectedParent);
            readDrawableCommon(reader, stroke);
            stroke->brushType = readInt(reader.get("brush"), context + ".brush");

            YAML::Node segmentsNode = reader.get("segments");
            YAML::Node interpolationNode = reader.get("interp");
            if (YAML::Node samples = reader.get("samples")) {
                bool missing = false;
                Table table = readTable(samples, source, context + ".samples", warnings, &missing);
                std::vector<long long> segments;
                if (segmentsNode) {
                    if (!segmentsNode.IsSequence()) {
                        fail(segmentsNode, context + ".segments", "expected a list of sample counts");
                    }
                    for (std::size_t i = 0; i < segmentsNode.size(); ++i) {
                        segments.push_back(readInt64(segmentsNode[i], context + ".segments"));
                    }
                } else {
                    segments.push_back((long long)table.rows);
                }
                if (!missing) {
                    // Each count is checked against the rows still unclaimed rather than summed,
                    // so hand-edited counts cannot overflow into a total that matches the table.
                    std::size_t remaining = table.rows;
                    for (long long count : segments) {
                        if (count < 0) {
                            fail(segmentsNode, context + ".segments", "a negative sample count");
                        }
                        if ((unsigned long long)count > remaining) {
                            fail(segmentsNode, context + ".segments", "the segments hold more samples than the table's " + std::to_string(table.rows));
                        }
                        remaining -= (std::size_t)count;
                    }
                    if (remaining != 0) {
                        fail(segmentsNode ? segmentsNode : samples, context + ".segments", "the segments hold " + std::to_string(table.rows - remaining) + " samples, the table " + std::to_string(table.rows));
                    }
                    const int uniform = interpolationNode ? readInterpolationToken(interpolationNode, context + ".interp") : kYamlKeyframeLinear;
                    if (keyframeStoresDerivatives(uniform)) {
                        fail(interpolationNode, context + ".interp", "stroke samples cannot be free or broken keyframes");
                    }
                    const std::vector<double> x = table.f64("x");
                    const std::vector<double> y = table.f64("y");
                    const std::vector<double> pressure = table.f64("pressure");
                    const std::vector<double> time = table.f64("time");
                    const std::vector<std::uint8_t> interp = table.u8("interp", (std::uint8_t)uniform);
                    std::size_t row = 0;
                    for (long long count : segments) {
                        stroke->strokes.push_back(RotoStrokeCurvesSerialization());
                        RotoStrokeCurvesSerialization& curves = stroke->strokes.back();
                        for (long long i = 0; (i < count) && (row < table.rows); ++i, ++row) {
                            KeyFrameSerialization key;
                            key.time = time[row];
                            key.interpolation = checkedInterpolation(interp[row], samples, context + ".samples");
                            key.value = x[row];
                            curves.xCurve.keyframes.push_back(key);
                            key.value = y[row];
                            curves.yCurve.keyframes.push_back(key);
                            key.value = pressure[row];
                            curves.pressureCurve.keyframes.push_back(key);
                        }
                    }
                }
            } else if (YAML::Node curves = reader.get("curves")) {
                if (!curves.IsSequence()) {
                    fail(curves, context + ".curves", "expected a list of stroke curves");
                }
                for (std::size_t i = 0; i < curves.size(); ++i) {
                    const std::string strokeContext = context + ".curves[" + std::to_string(i) + "]";
                    MapReader curvesReader(curves[i], strokeContext, warnings);
                    RotoStrokeCurvesSerialization data;
                    data.xCurve = readCurve(curvesReader.get("x"), strokeContext + ".x");
                    data.yCurve = readCurve(curvesReader.get("y"), strokeContext + ".y");
                    data.pressureCurve = readCurve(curvesReader.get("pressure"), strokeContext + ".pressure");
                    curvesReader.finish();
                    stroke->strokes.push_back(data);
                }
            }
            reader.finish();
        }

        void
        emitLayer(YAML::Emitter& em,
                  const RotoLayerSerialization& layer,
                  const std::string& expectedParent,
                  const BlobEncodeContext& blobs)
        {
            em << YAML::BeginMap;
            emitItemCommon(em, layer, expectedParent);
            if (!layer.children.empty()) {
                emitKey(em, "items");
                em << YAML::Value << YAML::BeginSeq;
                for (const RotoLayerChildSerialization& child : layer.children) {
                    em << YAML::BeginMap;
                    switch (child.type) {
                    case eRotoLayerChildTypeBezier:
                        emitKey(em, "bezier");
                        em << YAML::Value;
                        emitBezier(em, child.bezier, layer.name, blobs);
                        break;
                    case eRotoLayerChildTypeStroke:
                        emitKey(em, "stroke");
                        em << YAML::Value;
                        emitStroke(em, child.stroke, layer.name, blobs);
                        break;
                    case eRotoLayerChildTypeLayer:
                        emitKey(em, "layer");
                        em << YAML::Value;
                        emitLayer(em, child.layer, layer.name, blobs);
                        break;
                    }
                    em << YAML::EndMap;
                }
                em << YAML::EndSeq;
            }
            em << YAML::EndMap;
        }

        void
        readLayer(const YAML::Node& node,
                  RotoLayerSerialization* layer,
                  const std::string& expectedParent,
                  const BlobSource& source,
                  const std::string& context,
                  YamlWarnings* warnings)
        {
            MapReader reader(node, context, warnings);
            readItemCommon(reader, layer, expectedParent);
            if (YAML::Node items = reader.get("items")) {
                if (!items.IsSequence()) {
                    fail(items, context + ".items", "expected a list of items");
                }
                for (std::size_t i = 0; i < items.size(); ++i) {
                    const std::string itemContext = context + ".items[" + std::to_string(i) + "]";
                    MapReader itemReader(items[i], itemContext, warnings);
                    layer->children.push_back(RotoLayerChildSerialization());
                    RotoLayerChildSerialization& child = layer->children.back();
                    if (YAML::Node bezier = itemReader.get("bezier")) {
                        child.type = eRotoLayerChildTypeBezier;
                        readBezier(bezier, &child.bezier, layer->name, source, itemContext + ".bezier", warnings);
                    } else if (YAML::Node stroke = itemReader.get("stroke")) {
                        child.type = eRotoLayerChildTypeStroke;
                        readStroke(stroke, &child.stroke, layer->name, source, itemContext + ".stroke", warnings);
                    } else if (YAML::Node childLayer = itemReader.get("layer")) {
                        child.type = eRotoLayerChildTypeLayer;
                        readLayer(childLayer, &child.layer, layer->name, source, itemContext + ".layer", warnings);
                    } else {
                        layer->children.pop_back();
                        warn(warnings, itemContext + ": an item that is not a bezier, a stroke or a layer is ignored");
                        continue;
                    }
                    itemReader.finish();
                }
            }
            reader.finish();
        }

        void
        emitSplineWarpValue(YAML::Emitter& em,
                            const SplineWarpAnimatedValueSerialization& value)
        {
            em << YAML::BeginMap;
            emitKey(em, "value");
            em << YAML::Value;
            emitDouble(em, value.value);
            if (!value.curve.keyframes.empty()) {
                emitKey(em, "curve");
                em << YAML::Value;
                emitCurve(em, value.curve);
            }
            em << YAML::EndMap;
        }

        SplineWarpAnimatedValueSerialization
        readSplineWarpValue(const YAML::Node& node,
                            const std::string& context,
                            YamlWarnings* warnings)
        {
            SplineWarpAnimatedValueSerialization value;
            MapReader reader(node, context, warnings);
            value.value = readDouble(reader.get("value"), context + ".value");
            if (YAML::Node curve = reader.get("curve")) {
                value.curve = readCurve(curve, context + ".curve");
            }
            reader.finish();

            return value;
        }

        void
        emitSplineWarp(YAML::Emitter& em,
                       const SplineWarpSerialization& warp)
        {
            em << YAML::BeginMap;
            if (!warp.boundaries.empty()) {
                emitKey(em, "boundaries");
                em << YAML::Value;
                emitStringList(em, warp.boundaries);
            }
            if (!warp.links.empty()) {
                emitKey(em, "links");
                em << YAML::Value << YAML::BeginSeq;
                for (const SplineWarpLinkSerialization& link : warp.links) {
                    em << YAML::BeginMap;
                    emitKey(em, "source");
                    em << YAML::Value;
                    emitString(em, link.source);
                    emitKey(em, "destination");
                    em << YAML::Value;
                    emitString(em, link.destination);
                    emitKey(em, "startOffset");
                    em << YAML::Value;
                    emitDouble(em, link.startOffset);
                    emitKey(em, "reversed");
                    em << YAML::Value;
                    emitBool(em, link.reversed);
                    emitKey(em, "amount");
                    em << YAML::Value;
                    emitSplineWarpValue(em, link.amount);
                    if (!link.correspondences.empty()) {
                        emitKey(em, "correspondences");
                        em << YAML::Value << YAML::BeginSeq;
                        for (const SplineWarpCorrespondenceSerialization& c : link.correspondences) {
                            em << YAML::BeginMap;
                            emitKey(em, "source");
                            em << YAML::Value;
                            emitSplineWarpValue(em, c.source);
                            emitKey(em, "destination");
                            em << YAML::Value;
                            emitSplineWarpValue(em, c.destination);
                            em << YAML::EndMap;
                        }
                        em << YAML::EndSeq;
                    }
                    em << YAML::EndMap;
                }
                em << YAML::EndSeq;
            }
            em << YAML::EndMap;
        }

        SplineWarpSerialization
        readSplineWarp(const YAML::Node& node,
                       const std::string& context,
                       YamlWarnings* warnings)
        {
            SplineWarpSerialization warp;
            MapReader reader(node, context, warnings);
            if (YAML::Node boundaries = reader.get("boundaries")) {
                warp.boundaries = readStringList(boundaries, context + ".boundaries");
            }
            if (YAML::Node links = reader.get("links")) {
                if (!links.IsSequence()) {
                    fail(links, context + ".links", "expected a sequence");
                }
                for (std::size_t i = 0; i < links.size(); ++i) {
                    const std::string linkContext = context + ".links[" + std::to_string(i) + "]";
                    SplineWarpLinkSerialization link;
                    MapReader linkReader(links[i], linkContext, warnings);
                    link.source = readString(linkReader.get("source"), linkContext + ".source");
                    link.destination = readString(linkReader.get("destination"), linkContext + ".destination");
                    link.startOffset = readDouble(linkReader.get("startOffset"), linkContext + ".startOffset");
                    link.reversed = readBool(linkReader.get("reversed"), linkContext + ".reversed");
                    link.amount = readSplineWarpValue(linkReader.get("amount"), linkContext + ".amount", warnings);
                    if (YAML::Node correspondences = linkReader.get("correspondences")) {
                        if (!correspondences.IsSequence()) {
                            fail(correspondences, linkContext + ".correspondences", "expected a sequence");
                        }
                        for (std::size_t c = 0; c < correspondences.size(); ++c) {
                            const std::string cContext = linkContext + ".correspondences[" + std::to_string(c) + "]";
                            SplineWarpCorrespondenceSerialization correspondence;
                            MapReader cReader(correspondences[c], cContext, warnings);
                            correspondence.source = readSplineWarpValue(cReader.get("source"), cContext + ".source", warnings);
                            correspondence.destination = readSplineWarpValue(cReader.get("destination"), cContext + ".destination", warnings);
                            cReader.finish();
                            link.correspondences.push_back(correspondence);
                        }
                    }
                    linkReader.finish();
                    warp.links.push_back(link);
                }
            }
            reader.finish();

            return warp;
        }

    } // namespace

    void
    emitRotoContext(YAML::Emitter& em,
                    const RotoContextSerialization& roto,
                    const BlobEncodeContext& blobs)
    {
        em << YAML::BeginMap;
        emitKey(em, "autoKeying");
        em << YAML::Value;
        emitBool(em, roto.autoKeying);
        emitKey(em, "rippleEdit");
        em << YAML::Value;
        emitBool(em, roto.rippleEdit);
        emitKey(em, "featherLink");
        em << YAML::Value;
        emitBool(em, roto.featherLink);
        if (!roto.selectedItems.empty()) {
            emitKey(em, "selected");
            em << YAML::Value;
            emitStringList(em, roto.selectedItems);
        }
        emitKey(em, "layer");
        em << YAML::Value;
        emitLayer(em, roto.baseLayer, std::string(), blobs);
        if (roto.hasSplineWarp) {
            emitKey(em, "splineWarp");
            em << YAML::Value;
            emitSplineWarp(em, roto.splineWarp);
        }
        em << YAML::EndMap;
    }

    RotoContextSerialization
    readRotoContext(const YAML::Node& node,
                    const BlobSource& source,
                    const std::string& context,
                    YamlWarnings* warnings)
    {
        RotoContextSerialization roto;
        MapReader reader(node, context, warnings);
        roto.autoKeying = readBool(reader.get("autoKeying"), context + ".autoKeying");
        roto.rippleEdit = readBool(reader.get("rippleEdit"), context + ".rippleEdit");
        roto.featherLink = readBool(reader.get("featherLink"), context + ".featherLink");
        if (YAML::Node selected = reader.get("selected")) {
            roto.selectedItems = readStringList(selected, context + ".selected");
        }
        if (YAML::Node layer = reader.get("layer")) {
            readLayer(layer, &roto.baseLayer, std::string(), source, context + ".layer", warnings);
        }
        if (YAML::Node warp = reader.get("splineWarp")) {
            roto.hasSplineWarp = true;
            roto.splineWarp = readSplineWarp(warp, context + ".splineWarp", warnings);
        }
        reader.finish();

        return roto;
    }

} // namespace YamlDetail

std::string
encodeRotoYaml(const RotoContextSerialization& roto,
               const std::string& nodeFullName,
               BlobSink& sink)
{
    YAML::Emitter em;
    std::set<std::string> usedPaths;
    YamlDetail::BlobEncodeContext blobs { &sink, nodeFullName, &usedPaths };
    YamlDetail::emitRotoContext(em, roto, blobs);

    return YamlDetail::documentText(em);
}

RotoContextSerialization
decodeRotoYaml(std::string_view text,
               const BlobSource& source,
               YamlWarnings* warnings)
{
    YAML::Node node = YamlDetail::loadDocument(text, "the roto data");

    return YamlDetail::readRotoContext(node, source, "roto", warnings);
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
