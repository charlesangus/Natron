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

#ifndef Engine_Nodes_Geometry_GeoUsdConvert_h
#define Engine_Nodes_Geometry_GeoUsdConvert_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Engine/Geo/GeoPacked.h"
#include "Engine/Geo/GeoVDB.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A detail int attribute, nonzero when the detail's points are in world space rather than
 * in the space of the prims they came from: each prim's points are then written relative to
 * that prim's world transform. Not exported.
 **/
inline constexpr const char* kGeoUsdWorldSpaceAttr = "usdWorldSpace";

/**
 * @brief A primitive int attribute, and a point one for points prims, nonzero on geometry read
 * from a USD prim at its `path`: such geometry overrides that prim when the input scene still
 * holds it. Geometry without it only ever defines new prims. Not exported.
 **/
inline constexpr const char* kGeoUsdSourceAttr = "usdSource";

/**
 * @brief A primitive string attribute on VDB primitives read from a USD volume: the field
 * relationship the grid came through, without its namespace. An override of that volume authors
 * the same field again rather than adding one beside it. Not exported.
 **/
inline constexpr const char* kGeoUsdFieldAttr = "usdField";

/** @brief How packed geometry primitives become USD prims. */
enum GeoUsdPackedModeEnum {
    eGeoUsdPackedPointInstancer = 0,
    eGeoUsdPackedNativeInstances,
    eGeoUsdPackedUnpack,
};

/** @brief The USD prim one partition of a detail becomes. */
enum GeoUsdPrimKindEnum {
    eGeoUsdPrimMesh = 0,
    eGeoUsdPrimCurves,
    eGeoUsdPrimPoints,
    eGeoUsdPrimInstancer,
    eGeoUsdPrimNativeInstances,
    // An over on a packed USD primitive's source path, carrying its local transform.
    eGeoUsdPrimOverride,
    // A UsdVolVolume of VDB primitives, one OpenVDBAsset field child per grid.
    eGeoUsdPrimVolume,
};

/** @brief What the stage the converted layer will be composed over holds at one prim. */
struct GeoUsdInputPrim {
    PXR_NS::TfToken typeName;
    // Row-vector USD matrices: the prim's local-to-world and parent-to-world transforms.
    PXR_NS::GfMatrix4d world { 1. };
    PXR_NS::GfMatrix4d parentWorld { 1. };
    bool resetsXformStack = false;
    // Attribute values at the conversion's time, primvars flattened; "<primvar>:indices" is
    // present when the primvar is indexed. Only captured for prims the detail itself names.
    std::unordered_map<PXR_NS::TfToken, PXR_NS::VtValue, PXR_NS::TfToken::HashFunctor> values;
    // A volume's OpenVDB field relationships, namespace stripped, and the field prims they target.
    std::vector<std::pair<PXR_NS::TfToken, PXR_NS::SdfPath>> fields;
    U64 hash = 0;
};

/** @brief The input stage's prims at a detail's paths and their ancestors; absent prims are not listed. */
struct GeoUsdInputScene {
    std::unordered_map<PXR_NS::SdfPath, GeoUsdInputPrim, PXR_NS::SdfPath::Hash> prims;
    U64 hash = 0;

    const GeoUsdInputPrim* find(const PXR_NS::SdfPath& path) const
    {
        const auto it = prims.find(path);

        return it == prims.end() ? nullptr : &it->second;
    }
};

typedef std::shared_ptr<const GeoUsdInputScene> GeoUsdInputSceneConstPtr;

struct GeoUsdConvertOptions {
    // Unpathed primitives and relative path values go under this prim.
    PXR_NS::SdfPath rootPath { "/geo" };
    GeoUsdPackedModeEnum packedMode = eGeoUsdPackedPointInstancer;
    // The width authored on UsdGeomPoints that carry no pscale; USD's own fallback is 1.0.
    double defaultPointWidth = 0.01;
    // When set, prims the input already holds are authored as overs and transforms are written
    // relative to the input's parents, so that the composed result lands where the detail puts it.
    GeoUsdInputSceneConstPtr inputScene;
    // Where VDB primitives' grids are written; empty for geoVDBCacheDirectory().
    std::string volumeDirectory;
};

/**
 * @brief The prim paths a conversion of @p detail would touch, and the packed USD source paths
 * among them. pathsFromUsd[i] is nonzero when geometry read from USD (kGeoUsdSourceAttr) names
 * paths[i].
 **/
struct GeoUsdDetailPaths {
    std::vector<PXR_NS::SdfPath> paths;
    std::vector<char> pathsFromUsd;
    std::vector<PXR_NS::SdfPath> sourcePaths;
};
GeoUsdDetailPaths geoUsdDetailPaths(const GeoDetail& detail,
                                    const GeoUsdConvertOptions& options);

/** @brief Why geoUsdPlacedPath() moved a path, if it did. */
enum GeoUsdPlacementEnum {
    eGeoUsdPlacedAsRequested = 0,
    // Geometry read from USD whose prim the input scene no longer holds.
    eGeoUsdPlacedSourceGone,
    // Geometry not read from USD on a path the input scene already holds.
    eGeoUsdPlacedCollision,
};

/**
 * @brief Where a partition that asks for @p requested is authored: @p requested itself, unless
 * @p scene is given and @p requested lies outside @p root; then the same path under @p root when
 * geometry read from USD (@p fromUsd) finds its prim gone, so that it does not reappear where it
 * was, or when other geometry finds the path taken, since only geometry read from a prim
 * overrides it.
 **/
PXR_NS::SdfPath geoUsdPlacedPath(const PXR_NS::SdfPath& requested,
                                 const PXR_NS::SdfPath& root,
                                 const GeoUsdInputScene* scene,
                                 bool fromUsd,
                                 GeoUsdPlacementEnum* placement = nullptr);

/**
 * @brief Captures @p stage at @p paths and all their ancestors at @p time. Values are read for
 * @p paths only, minus @p transformOnly.
 **/
GeoUsdInputSceneConstPtr geoUsdCaptureInputScene(const PXR_NS::UsdStageRefPtr& stage,
                                                 PXR_NS::UsdTimeCode time,
                                                 const std::vector<PXR_NS::SdfPath>& paths,
                                                 const std::vector<PXR_NS::SdfPath>& transformOnly);

/**
 * @brief The translation between a GeoDetail and USD scene description.
 *
 * The detail is partitioned into USD prims by the `path` primitive attribute, or `name` when
 * there is no `path`; a relative value goes under the root path, an empty one or none puts
 * the primitive in one prim per type under the root (mesh_0, curves_0, points_0, instancer_0).
 * Primitives of different types sharing a path become per-type child prims of that path, with
 * a warning. Polygons become a UsdGeomMesh, polylines a linear UsdGeomBasisCurves (one USD point
 * per vertex, since curves cannot share points), unconnected points a UsdGeomPoints, placed by
 * their own `path` point attribute when they have one. Packed USD primitives become an over on
 * their source path carrying xformOp:transform; packed geometry becomes a
 * UsdGeomPointInstancer, native instances or unpacked geometry, by mode. A point instancer
 * cannot hold shear, so packed geometry whose transform shears becomes native instances
 * instead, with a warning. Packed transforms are in the detail's space, which is world space:
 * each one is written relative to the world transform of the prim it lands under. Points are in
 * the space of their prim, or in world space when the detail says so (kGeoUsdWorldSpaceAttr);
 * normal and vector attributes then follow the points back into prim space.
 *
 * VDB primitives sharing a `path` (the `name` attribute names their grids, not their prim)
 * gather into one UsdVolVolume, unpathed ones into volume_0 under the root. In primitive order, a
 * field name already in the current volume starts the next one: the volumes of one path are its
 * volume_0, volume_1... children (under the root when unpathed), noted in volumeNotes(). Each grid
 * becomes an OpenVDBAsset child prim named after its field, the grid's name, else the field it
 * was read through, else "field", bound by a field:<name> relationship. All of a volume's grids
 * are written, in primitive order, to one content-addressed file under the volume directory
 * (geoEnsureVDBFile()), which every field's filePath names. The file stores each grid under, and
 * its field's fieldName names it by, the grid's name, else the field it was read through, else
 * "field"; fieldIndex tells same-named grids of the file apart and is 0 for the others. Grid
 * transforms are in the space of the volume prim, as points are: a new prim gets an identity
 * transform and an over keeps the input's, unless the detail is in world space, when the volume
 * prim's transform cancels its parents'. Packed geometry holding volumes becomes a point
 * instancer as any other, so a volume copied by packed primitives is one prototype and one file
 * however many copies there are.
 *
 * With an input scene, a partition read from USD (kGeoUsdSourceAttr) whose prim the input holds
 * becomes an over of it. Meshes, curves and points of the input's type then carry points, plus
 * the values that differ from the input's; topology only when it differs, in full. A partition
 * on an absolute path outside the root goes under the root path, with a warning, when it was read
 * from a prim the input no longer holds or was not read from USD and the input holds its path
 * (geoUsdPlacedPath()).
 *
 * Attribute classes map detail -> constant, primitive -> uniform, point -> vertex and
 * vertex -> faceVarying, and names map P -> points, N -> normals, uv -> primvars:st,
 * Cd -> primvars:displayColor, pscale -> widths (points and curves), the rest to
 * primvars:<name>. `path`, `name` and the packed intrinsics are not exported. Primitive groups
 * become UsdGeomSubsets on meshes and bool uniform primvars on curves; point groups become bool
 * vertex primvars; vertex and edge groups are dropped with a warning, as are array attributes.
 *
 * The spec structure (paths, types, field names) is built on construction. Attribute values
 * are built per partition on the first field query that needs one, and kept until the object
 * dies or is adopted by a successor through adoptUnchangedPartitions(). Const members are
 * thread-safe.
 **/
class GeoUsdConvert {
public:
    GeoUsdConvert(GeoDetailConstPtr detail,
                  GeoUsdConvertOptions options);
    ~GeoUsdConvert();

    GeoUsdConvert(const GeoUsdConvert&) = delete;
    GeoUsdConvert& operator=(const GeoUsdConvert&) = delete;

    /** @brief The converted detail: the input, or its unpacked copy in eGeoUsdPackedUnpack. */
    const GeoDetailConstPtr& detail() const
    {
        return _detail;
    }

    const GeoUsdConvertOptions& options() const
    {
        return _options;
    }

    const std::vector<std::string>& warnings() const
    {
        return _warnings;
    }

    /** @brief The prims holding packed geometry written as native instances for its shear. */
    const std::vector<PXR_NS::SdfPath>& shearedInstances() const
    {
        return _shearedInstances;
    }

    /** @brief The paths whose grids make several volumes, and the fields renamed for an input's field prims. */
    const std::vector<std::string>& volumeNotes() const
    {
        return _volumeNotes;
    }

    /** @brief The grids whose file could not be written, so their fields have none. */
    const std::vector<std::string>& volumeWriteFailures() const
    {
        return _volumeWriteFailures;
    }

    std::size_t partitionCount() const;
    const PXR_NS::SdfPath& partitionPath(std::size_t partition) const;
    GeoUsdPrimKindEnum partitionKind(std::size_t partition) const;

    /** @brief The offsets of the primitives the partition's prim is made of, in prim order. */
    const std::vector<std::size_t>& partitionPrimitives(std::size_t partition) const;

    /** @brief Whether the partition's attribute values have been built or adopted. */
    bool isPartitionBuilt(std::size_t partition) const;

    /**
     * @brief Hash of everything the partition's specs and values are built from: its primitive
     * list, attribute descriptions, the pages of attributes and topology columns it touches,
     * groups, whole string attributes, and its prototypes' content hashes. Computed once.
     **/
    U64 partitionSignature(std::size_t partition) const;

    /**
     * @brief Takes the built values of every partition of @p previous with the same path, kind
     * and signature. Call before any field query on this object. Returns how many it took.
     **/
    std::size_t adoptUnchangedPartitions(const GeoUsdConvert& previous);

    /**
     * @brief The partition whose values and structure the spec at @p path depends on, -1 for
     * the shared ancestors (root, intermediate prims) and absent paths.
     **/
    int specPartition(const PXR_NS::SdfPath& path) const;

    bool hasSpec(const PXR_NS::SdfPath& path) const;

    /** @brief SdfSpecTypeUnknown when absent. */
    PXR_NS::SdfSpecType specType(const PXR_NS::SdfPath& path) const;

    /** @brief The field's value; builds the owning partition's values when the field is a default. */
    bool field(const PXR_NS::SdfPath& path,
               const PXR_NS::TfToken& fieldName,
               PXR_NS::VtValue* value) const;

    std::vector<PXR_NS::TfToken> fieldNames(const PXR_NS::SdfPath& path) const;

    /** @brief Every spec, parents before children, properties before child prims; stops on false. */
    void visitSpecs(const std::function<bool(const PXR_NS::SdfPath&)>& visitor) const;

    /**
     * @brief Writes every spec and field into @p layer. Attribute values go in as time samples
     * at @p time unless it is the default time code or the attribute is uniform. The values read
     * back from the layer equal field()'s, field by field.
     **/
    bool author(const PXR_NS::SdfLayerHandle& layer,
                PXR_NS::UsdTimeCode time,
                std::string* errorMessage) const;

    /** @brief author() for the one spec at @p path, whose parent prims are created when absent. */
    bool authorSpec(const PXR_NS::SdfLayerHandle& layer,
                    const PXR_NS::SdfPath& path,
                    PXR_NS::UsdTimeCode time,
                    std::string* errorMessage) const;

private:
    struct Partition;
    struct SpecEntry;
    struct PropertyPlan;

    GeoUsdConvert(GeoDetailConstPtr detail,
                  GeoUsdConvertOptions options,
                  bool nested);

    void build(bool nested);
    SpecEntry& ensurePrim(const PXR_NS::SdfPath& path,
                          PXR_NS::SdfSpecifier specifier,
                          const PXR_NS::TfToken& typeName);
    SpecEntry& addProperty(const PXR_NS::SdfPath& primPath,
                           const PropertyPlan& plan);
    void planProperties(Partition& partition);
    void trimInputOverrides(Partition& partition);
    void addPartitionSpecs(std::size_t index);
    const GeoUsdInputPrim* inputPrim(const PXR_NS::SdfPath& path) const;
    PXR_NS::GfMatrix4d frameOf(const PXR_NS::SdfPath& path) const;
    PXR_NS::GfMatrix4d overrideParent(const Partition& partition) const;
    void finishSpecs();
    const SpecEntry* findEntry(const PXR_NS::SdfPath& path) const;
    const GeoUsdConvert* delegateFor(const SpecEntry& entry) const;
    std::shared_ptr<const std::unordered_map<PXR_NS::SdfPath, PXR_NS::VtValue, PXR_NS::SdfPath::Hash>> partitionValues(std::size_t partition) const;
    void buildValues(const Partition& partition,
                     std::unordered_map<PXR_NS::SdfPath, PXR_NS::VtValue, PXR_NS::SdfPath::Hash>& values) const;
    U64 computeSignature(const Partition& partition) const;
    void visitPrim(const PXR_NS::SdfPath& path,
                   const std::function<bool(const PXR_NS::SdfPath&)>& visitor,
                   bool* stop) const;

    GeoDetailConstPtr _detail;
    GeoUsdConvertOptions _options;
    std::vector<std::string> _warnings;
    std::vector<PXR_NS::SdfPath> _shearedInstances;
    std::vector<std::string> _volumeNotes;
    std::vector<std::string> _volumeWriteFailures;
    std::vector<std::unique_ptr<Partition>> _partitions;
    std::unordered_map<PXR_NS::SdfPath, SpecEntry, PXR_NS::SdfPath::Hash> _specs;
    PXR_NS::SdfPath _topPath;
};

/** @brief GeoMatrix4 (column vectors) as a GfMatrix4d (row vectors), and back. */
PXR_NS::GfMatrix4d geoUsdMatrix(const GeoMatrix4& m);

/**
 * @brief Ensures the file of every volume of @p detail and of the packed geometry in it is in
 * place as GeoUsdConvert writes it, under @p options' volume directory: marks a file found as
 * used, and writes one that is gone, such as after the disk cache was cleared or evicted it. Asks
 * @p aborted between and within files (geoEnsureVDBFile()) and returns false once it says so; a
 * write that fails is left for GeoUsdConvert to report.
 **/
bool geoUsdEnsureVolumeFiles(const GeoDetail& detail,
                             const GeoUsdConvertOptions& options,
                             const GeoVDBAbortFunc& aborted);
GeoMatrix4 geoMatrixFromUsd(const PXR_NS::GfMatrix4d& m);

/**
 * @brief The detail with every packed geometry primitive replaced by its contents, transformed
 * by its local transform, recursively. Unpacked primitives take the packed primitive's
 * primitive attributes and primitive groups. Packed USD primitives inside packed geometry are
 * dropped with a warning.
 **/
GeoDetail geoUnpackPackedGeometry(const GeoDetail& source,
                                  std::vector<std::string>* warnings);

/**
 * @brief Appends a UsdGeomMesh, UsdGeomBasisCurves, UsdGeomPoints or UsdVolVolume prim to @p builder at
 * @p time, the inverse of GeoUsdConvert's mapping, and sets each new primitive's `path` to the
 * prim path, or for a points prim, which has no primitives, each new point's `path`, and marks
 * them with kGeoUsdSourceAttr. With @p worldTransform, positions are baked into that space, and
 * normal and vector attributes too, normals by its inverse transpose and renormalised.
 * GeomSubsets of faces become primitive groups and bool primvars become groups. A volume's
 * OpenVDBAsset fields with a file become VDB primitives named after the field's grid, their grids
 * read from the files; other fields are skipped with a warning. An attribute that exists in
 * the builder with another description is skipped with a warning. Returns false with
 * *errorMessage set for another prim type or inconsistent topology, leaving the builder as it
 * was.
 **/
bool geoUsdReadPrim(const PXR_NS::UsdPrim& prim,
                    PXR_NS::UsdTimeCode time,
                    const PXR_NS::GfMatrix4d* worldTransform,
                    GeoDetailBuilder& builder,
                    std::vector<std::string>* warnings,
                    std::string* errorMessage);

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Geometry_GeoUsdConvert_h
