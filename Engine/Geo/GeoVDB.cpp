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

#include "GeoVDB.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include <unistd.h>

#include "Engine/Geo/GeoOpenVDB.h"

#include "Engine/AppManager.h"
#include "Engine/CacheDiskTier.h"
#include "Engine/ContentHasher.h"
#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/Geo/GeoPage.h"
#include "Engine/StandardPaths.h"

NATRON_NAMESPACE_ENTER

namespace {

constexpr std::size_t kMinLeavesForParallelHash = 64;

using HashedGridTypes = openvdb::TypeList<openvdb::FloatGrid,
                                          openvdb::DoubleGrid,
                                          openvdb::HalfGrid,
                                          openvdb::Int32Grid,
                                          openvdb::Int64Grid,
                                          openvdb::Vec3IGrid,
                                          openvdb::Vec3SGrid,
                                          openvdb::Vec3DGrid,
                                          openvdb::BoolGrid,
                                          openvdb::MaskGrid>;

template <typename ValueT>
void
appendValue(ContentHasher& hasher,
            const ValueT& value)
{
    if constexpr (std::is_same_v<ValueT, bool>) {
        hasher.appendBool(value);
    } else if constexpr (std::is_same_v<ValueT, openvdb::ValueMask>) {
        (void)hasher;
        (void)value;
    } else {
        static_assert(std::is_trivially_copyable_v<ValueT>);
        hasher.appendBytes(&value, sizeof(ValueT));
    }
}

void
appendCoord(ContentHasher& hasher,
            const openvdb::Coord& c)
{
    hasher.appendI64(c.x());
    hasher.appendI64(c.y());
    hasher.appendI64(c.z());
}

template <typename LeafT>
U64
hashLeaf(const LeafT& leaf)
{
    using ValueT = typename LeafT::ValueType;

    ContentHasher hasher;
    appendCoord(hasher, leaf.origin());
    const typename LeafT::NodeMaskType& mask = leaf.getValueMask();
    for (openvdb::Index n = 0; n < LeafT::NodeMaskType::WORD_COUNT; ++n) {
        hasher.appendU64(mask.template getWord<openvdb::Index64>(n));
    }
    if constexpr (std::is_same_v<ValueT, bool>) {
        for (openvdb::Index i = 0; i < LeafT::SIZE; ++i) {
            hasher.appendBool(leaf.getValue(i));
        }
    } else if constexpr (!std::is_same_v<ValueT, openvdb::ValueMask>) {
        hasher.appendBytes(leaf.buffer().data(), sizeof(ValueT) * LeafT::SIZE);
    }

    return hasher.finalize();
}

template <typename TreeT>
U64
hashTree(const TreeT& tree)
{
    using LeafT = typename TreeT::LeafNodeType;

    ContentHasher hasher;
    appendValue(hasher, tree.background());

    // Tiles of the root and internal nodes only; the leaves' voxels are hashed below.
    typename TreeT::ValueAllCIter tile = tree.cbeginValueAll();
    tile.setMaxDepth(TreeT::ValueAllCIter::LEAF_DEPTH - 1);
    for (; tile; ++tile) {
        hasher.appendU64(tile.getLevel());
        appendCoord(hasher, tile.getCoord());
        hasher.appendBool(tile.isValueOn());
        appendValue(hasher, tile.getValue());
    }

    std::vector<const LeafT*> leaves;
    tree.getNodes(leaves);
    std::vector<U64> leafHashes(leaves.size());
    geoParallelFor(leaves.size(), [&](std::size_t i) { leafHashes[i] = hashLeaf(*leaves[i]); }, kMinLeavesForParallelHash);
    hasher.appendU64(leafHashes.size());
    hasher.appendBytes(leafHashes.data(), leafHashes.size() * sizeof(U64));

    return hasher.finalize();
}

struct TreeHashOp {
    U64* out;

    template <typename GridT>
    void operator()(const GridT& grid) const
    {
        *out = hashTree(grid.tree());
    }
};

// For grid types without a typed walk (point grids and custom types): the serialized tree.
U64
hashSerializedTree(const openvdb::GridBase& grid)
{
    ContentHasher hasher;
    try {
        std::ostringstream os(std::ios_base::binary);
        openvdb::io::StreamMetadata::Ptr meta = std::make_shared<openvdb::io::StreamMetadata>();
        openvdb::io::setStreamMetadataPtr(os, meta, false);
        openvdb::io::setDataCompression(os, openvdb::io::COMPRESS_NONE);
        grid.writeTopology(os);
        grid.writeBuffers(os);
        const std::string bytes = os.str();
        hasher.appendString(bytes);
    } catch (const std::exception&) {
        // Nothing else identifies the content: unique to this grid, and only for this session.
        hasher.appendU64(U64(reinterpret_cast<std::uintptr_t>(&grid)));
        hasher.appendSessionSalt();
    }

    return hasher.finalize();
}

U64
computeTreeHash(const openvdb::GridBase& grid)
{
    U64 h = 0;
    if (!grid.apply<HashedGridTypes>(TreeHashOp { &h })) {
        h = hashSerializedTree(grid);
    }

    return h == 0 ? 1 : h;
}

void
writeBounds(GeoAttribute& a,
            GeoOffset primitive,
            const GeoBounds& b)
{
    const std::array<double, 6> v = { b.min[0], b.min[1], b.min[2], b.max[0], b.max[1], b.max[2] };
    a.setTuple<double>(std::size_t(primitive), std::span<const double>(v));
}

void
ensureAttribute(GeoDetailBuilder& builder,
                const char* name,
                GeoStorageEnum storage,
                int tupleSize)
{
    GeoAttributeDesc desc;
    desc.name = name;
    desc.storage = storage;
    desc.tupleSize = tupleSize;
    const GeoAttribute* existing = builder.detail().findAttribute(eGeoAttribClassPrimitive, name);
    if (existing && existing->storage() == storage && existing->tupleSize() == tupleSize && !existing->isArray()) {
        return;
    }
    builder.addAttribute(eGeoAttribClassPrimitive, desc);
}

GeoAttribute&
writableAttribute(GeoDetailBuilder& builder,
                  const char* name)
{
    GeoAttribute* a = builder.attributeForWriting(eGeoAttribClassPrimitive, name);
    if (!a) {
        throw std::invalid_argument(std::string("geometry has no ") + name + " attribute");
    }

    return *a;
}

void
requireVDB(const GeoDetail& detail,
           GeoOffset primitive)
{
    if (primitive < 0 || primitive >= detail.primitiveMap().offsetSize() || !detail.primitiveMap().isLive(primitive)) {
        throw std::invalid_argument("not a live primitive");
    }
    if (detail.primitiveType(primitive) != eGeoPrimitiveVDB) {
        throw std::invalid_argument("primitive is not a VDB primitive");
    }
}

void
setGrid(GeoDetailBuilder& builder,
        GeoOffset primitive,
        GeoVDBGridConstPtr wrapped)
{
    const GeoBounds bounds = geoVDBGridBounds(*wrapped->grid());
    const std::int32_t ref = builder.addSharedGrid(std::move(wrapped)) + 1;
    writableAttribute(builder, kGeoVDBGridAttr).set<std::int32_t>(std::size_t(primitive), ref);
    writeBounds(writableAttribute(builder, kGeoVDBBoundsAttr), primitive, bounds);
}

openvdb::Mat4d
toVDBMatrix(const GeoMatrix4& m)
{
    // GeoMatrix4 acts on column vectors, OpenVDB's matrices on row vectors.
    openvdb::Mat4d out;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out(c, r) = m[std::size_t(r * 4 + c)];
        }
    }

    return out;
}

} // namespace

GeoVDBGrid::GeoVDBGrid(GeoVDBGridBaseConstPtr grid)
    : _grid(std::move(grid))
{
    if (!_grid) {
        throw std::invalid_argument("GeoVDBGrid: null grid");
    }
    _tree = std::make_shared<TreeState>();
    _tree->bytes = std::size_t(_grid->memUsage());
}

GeoVDBGrid::GeoVDBGrid(GeoVDBGridBaseConstPtr grid,
                       std::shared_ptr<TreeState> tree)
    : _grid(std::move(grid))
    , _tree(std::move(tree))
{
}

GeoVDBGridConstPtr
GeoVDBGrid::withGrid(GeoVDBGridBaseConstPtr grid) const
{
    if (grid && (grid->constBaseTreePtr() == _grid->constBaseTreePtr())) {
        return GeoVDBGridConstPtr(new GeoVDBGrid(std::move(grid), _tree));
    }

    return std::make_shared<const GeoVDBGrid>(std::move(grid));
}

const void*
GeoVDBGrid::treeIdentity() const
{
    return _grid->constBaseTreePtr().get();
}

U64
GeoVDBGrid::treeHash() const
{
    U64 h = _tree->hash.load(std::memory_order_acquire);
    if (h == 0) {
        h = computeTreeHash(*_grid);
        _tree->hash.store(h, std::memory_order_release);
    }

    return h;
}

U64
GeoVDBGrid::contentHash() const
{
    U64 h = _hash.load(std::memory_order_acquire);
    if (h != 0) {
        return h;
    }

    ContentHasher hasher;
    hasher.appendString(_grid->valueType());
    hasher.appendI64(std::int64_t(_grid->getGridClass()));
    hasher.appendI64(std::int64_t(_grid->getVectorType()));
    hasher.appendBool(_grid->isInWorldSpace());
    hasher.appendBool(_grid->saveFloatAsHalf());
    std::ostringstream xform(std::ios_base::binary);
    _grid->transform().write(xform);
    hasher.appendString(xform.str());
    hasher.appendU64(treeHash());

    h = hasher.finalize();
    if (h == 0) {
        h = 1;
    }
    _hash.store(h, std::memory_order_release);

    return h;
}

void
geoInitializeOpenVDB()
{
    static std::once_flag once;
    std::call_once(once, []() { openvdb::initialize(); });
}

GeoOffset
geoAppendVDB(GeoDetailBuilder& builder,
             GeoOffset point,
             GeoVDBGridBaseConstPtr grid,
             std::string_view name)
{
    return geoAppendVDB(builder, point, std::make_shared<const GeoVDBGrid>(std::move(grid)), name);
}

GeoOffset
geoAppendVDB(GeoDetailBuilder& builder,
             GeoOffset point,
             GeoVDBGridConstPtr wrapped,
             std::string_view name)
{
    if (!wrapped) {
        throw std::invalid_argument("geoAppendVDB: null grid");
    }
    ensureAttribute(builder, kGeoVDBGridAttr, eGeoStorageInt32, 1);
    ensureAttribute(builder, kGeoVDBBoundsAttr, eGeoStorageFloat64, 6);
    ensureAttribute(builder, kGeoNameAttr, eGeoStorageString, 1);

    const std::int32_t count = 1;
    const GeoOffset prim = builder.appendPrimitives(eGeoPrimitiveVDB, std::span<const std::int32_t>(&count, 1), std::span<const GeoOffset>(&point, 1));
    setGrid(builder, prim, std::move(wrapped));
    writableAttribute(builder, kGeoNameAttr).setString(std::size_t(prim), name);

    return prim;
}

GeoOffset
geoAppendVDB(GeoDetailBuilder& builder,
             GeoVDBGridBaseConstPtr grid,
             std::string_view name)
{
    if (!grid) {
        throw std::invalid_argument("geoAppendVDB: null grid");
    }
    const openvdb::Vec3d origin = grid->transform().indexToWorld(openvdb::Vec3d(0., 0., 0.));
    const GeoOffset point = builder.appendPoints(1);
    builder.setPointPosition(point, origin.x(), origin.y(), origin.z());

    return geoAppendVDB(builder, point, std::move(grid), name);
}

void
geoReplaceVDBGrid(GeoDetailBuilder& builder,
                  GeoOffset primitive,
                  GeoVDBGridBaseConstPtr grid)
{
    requireVDB(builder.detail(), primitive);
    if (!grid) {
        throw std::invalid_argument("geoReplaceVDBGrid: null grid");
    }
    setGrid(builder, primitive, geoVDB(builder.detail(), primitive)->withGrid(std::move(grid)));
}

void
geoSetVDBName(GeoDetailBuilder& builder,
              GeoOffset primitive,
              std::string_view name)
{
    requireVDB(builder.detail(), primitive);
    ensureAttribute(builder, kGeoNameAttr, eGeoStorageString, 1);
    writableAttribute(builder, kGeoNameAttr).setString(std::size_t(primitive), name);
}

void
geoTransformVDB(GeoDetailBuilder& builder,
                GeoOffset primitive,
                const GeoMatrix4& transform)
{
    const GeoVDBGridConstPtr& current = geoVDB(builder.detail(), primitive);
    openvdb::math::Transform::Ptr xform = current->grid()->transform().copy();
    xform->postMult(toVDBMatrix(transform));
    GeoVDBGridConstPtr moved = current->withGrid(current->grid()->copyGridReplacingTransform(xform));
    setGrid(builder, primitive, std::move(moved));
}

const GeoVDBGridConstPtr&
geoVDB(const GeoDetail& detail,
       GeoOffset primitive)
{
    requireVDB(detail, primitive);
    const GeoAttribute* refs = detail.findAttribute(eGeoAttribClassPrimitive, kGeoVDBGridAttr);
    if (!refs) {
        throw std::invalid_argument(std::string("geometry has no ") + kGeoVDBGridAttr + " attribute");
    }
    const std::int32_t ref = refs->get<std::int32_t>(std::size_t(primitive)) - 1;
    const std::span<const GeoVDBGridConstPtr> table = detail.sharedGrids();
    if (ref < 0 || std::size_t(ref) >= table.size()) {
        throw std::invalid_argument("VDB primitive has no grid");
    }

    return table[std::size_t(ref)];
}

const GeoVDBGridBaseConstPtr&
geoVDBGrid(const GeoDetail& detail,
           GeoOffset primitive)
{
    return geoVDB(detail, primitive)->grid();
}

std::string_view
geoVDBName(const GeoDetail& detail,
           GeoOffset primitive)
{
    requireVDB(detail, primitive);
    const GeoAttribute* names = detail.findAttribute(eGeoAttribClassPrimitive, kGeoNameAttr);
    if (!names || names->storage() != eGeoStorageString) {
        return std::string_view();
    }

    return names->getString(std::size_t(primitive));
}

GeoBounds
geoVDBBounds(const GeoDetail& detail,
             GeoOffset primitive)
{
    requireVDB(detail, primitive);
    const GeoAttribute* bounds = detail.findAttribute(eGeoAttribClassPrimitive, kGeoVDBBoundsAttr);
    if (!bounds) {
        return geoVDBGridBounds(*geoVDBGrid(detail, primitive));
    }
    std::array<double, 6> v;
    bounds->getTuple<double>(std::size_t(primitive), std::span<double>(v));

    return GeoBounds { { v[0], v[1], v[2] }, { v[3], v[4], v[5] } };
}

GeoBounds
geoVDBGridBounds(const openvdb::GridBase& grid)
{
    const openvdb::CoordBBox active = grid.evalActiveVoxelBoundingBox();
    if (active.empty()) {
        return GeoBounds();
    }
    const openvdb::BBoxd index(active.min().asVec3d() - openvdb::Vec3d(0.5), active.max().asVec3d() + openvdb::Vec3d(0.5));
    const openvdb::BBoxd world = grid.transform().indexToWorld(index);

    return GeoBounds { { world.min().x(), world.min().y(), world.min().z() },
                       { world.max().x(), world.max().y(), world.max().z() } };
}

std::string
geoVDBGridName(const openvdb::GridBase& grid)
{
    return grid.getName();
}

GeoVDBFieldInfo
geoVDBFieldInfo(const openvdb::GridBase& grid)
{
    static const std::pair<const char*, const char*> types[] = {
        { "float", "float" },
        { "double", "double" },
        { "half", "half" },
        { "int32", "int" },
        { "uint32", "uint" },
        { "int64", "int64" },
        { "vec2h", "half2" },
        { "vec2s", "float2" },
        { "vec2d", "double2" },
        { "vec2i", "int2" },
        { "vec3h", "half3" },
        { "vec3s", "float3" },
        { "vec3d", "double3" },
        { "vec3i", "int3" },
        { "mat3d", "matrix3d" },
        { "mat4d", "matrix4d" },
        { "quatd", "quatd" },
        { "bool", "bool" },
        { "mask", "mask" },
        { "string", "string" },
    };
    GeoVDBFieldInfo info;
    const std::string valueType = grid.valueType();
    for (const auto& [vdb, usd] : types) {
        if (valueType == vdb) {
            info.dataType = usd;
            break;
        }
    }
    if (valueType.rfind("vec", 0) == 0) {
        switch (grid.getVectorType()) {
        case openvdb::VEC_COVARIANT:
        case openvdb::VEC_COVARIANT_NORMALIZE:
            info.roleHint = "Normal";
            break;
        case openvdb::VEC_CONTRAVARIANT_RELATIVE:
            info.roleHint = "Vector";
            break;
        case openvdb::VEC_CONTRAVARIANT_ABSOLUTE:
            info.roleHint = "Point";
            break;
        default:
            break;
        }
    }
    switch (grid.getGridClass()) {
    case openvdb::GRID_LEVEL_SET:
        info.gridClass = "levelSet";
        break;
    case openvdb::GRID_FOG_VOLUME:
        info.gridClass = "fogVolume";
        break;
    case openvdb::GRID_STAGGERED:
        info.gridClass = "staggered";
        break;
    default:
        break;
    }

    return info;
}

namespace {

// A file lists a grid stored under a duplicate name as "name[N]", as it would a grid really named
// so; only the grid's own metadata tells the two apart.
std::string
plainGridName(openvdb::io::File& file,
              const std::string& unique)
{
    return file.readGridMetadata(unique)->getName();
}

template <typename Fn>
bool
withVDBFile(const std::string& path,
            std::string* errorMessage,
            Fn&& fn)
{
    geoInitializeOpenVDB();
    try {
        openvdb::io::File file(path);
        file.open(false);
        fn(file);
        file.close();
    } catch (const std::exception& e) {
        if (errorMessage) {
            *errorMessage = path + ": " + e.what();
        }

        return false;
    }

    return true;
}

} // namespace

bool
geoVDBFileGridNames(const std::string& path,
                    std::vector<std::string>* names,
                    std::string* errorMessage)
{
    names->clear();

    return withVDBFile(path, errorMessage, [&](openvdb::io::File& file) {
        for (auto it = file.beginName(); it != file.endName(); ++it) {
            names->push_back(plainGridName(file, it.gridName()));
        }
    });
}

bool
geoReadVDBFile(const std::string& path,
               std::string_view pattern,
               std::vector<GeoVDBGridBaseConstPtr>* grids,
               std::string* errorMessage)
{
    grids->clear();

    return withVDBFile(path, errorMessage, [&](openvdb::io::File& file) {
        for (auto it = file.beginName(); it != file.endName(); ++it) {
            const std::string unique = it.gridName();
            const std::string name = plainGridName(file, unique);
            if (!geoNameMatchesPattern(pattern, name)) {
                continue;
            }
            const openvdb::GridBase::Ptr grid = file.readGrid(unique);
            grid->setName(name);
            grids->push_back(grid);
        }
    });
}

bool
geoReadVDBGrid(const std::string& path,
               const std::string& name,
               GeoVDBGridBaseConstPtr* grid,
               std::string* errorMessage)
{
    grid->reset();
    bool found = false;
    const bool opened = withVDBFile(path, errorMessage, [&](openvdb::io::File& file) {
        if (file.hasGrid(name)) {
            *grid = file.readGrid(name);
            found = true;
        }
    });
    if (opened && !found && errorMessage) {
        *errorMessage = path + ": no grid is named " + name;
    }

    return opened && found;
}

bool
geoWriteVDBFile(const openvdb::GridBase& grid,
                std::string_view name,
                const std::string& path,
                std::string* errorMessage)
{
    geoInitializeOpenVDB();
    static std::atomic<std::uint64_t> counter { 0 };
    const std::filesystem::path target(path);
    const std::string partial = (target.parent_path() / ("." + target.filename().string() + ".partial-" + std::to_string(::getpid()) + "-" + std::to_string(counter.fetch_add(1)))).string();
    try {
        std::error_code ec;
        const std::filesystem::path parent = target.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
        }
        // The class, vector type and half-float flags are metadata too, so the map is copied whole.
        openvdb::MetaMap meta(static_cast<const openvdb::MetaMap&>(grid));
        meta.removeMeta(openvdb::GridBase::META_GRID_NAME);
        meta.insertMeta(openvdb::GridBase::META_GRID_NAME, openvdb::StringMetadata(std::string(name)));
        openvdb::GridCPtrVec out { grid.copyGridReplacingMetadata(meta) };
        openvdb::io::File file(partial);
        file.write(out);
        file.close();
        std::filesystem::rename(partial, path);
    } catch (const std::exception& e) {
        std::error_code ignored;
        std::filesystem::remove(partial, ignored);
        if (errorMessage) {
            *errorMessage = path + ": " + e.what();
        }

        return false;
    }

    return true;
}

namespace {

CacheDiskTierPtr
vdbDiskTier()
{
    const AppManager* app = AppManager::instance();

    return app ? app->getNodeCacheDiskTier() : CacheDiskTierPtr();
}

std::string
fallbackVDBCacheDirectory()
{
    const AppManager* app = AppManager::instance();
    const QString base = app ? app->getDiskCacheLocation() : StandardPaths::writableLocation(StandardPaths::eStandardLocationCache);

    return base.toStdString() + "/vdb";
}

} // namespace

bool
geoEnsureVDBFile(const GeoVDBGrid& grid,
                 std::string_view name,
                 const std::string& directory,
                 std::string* path,
                 std::string* errorMessage)
{
    ContentHasher hasher;
    hasher.appendU64(grid.contentHash());
    hasher.appendString(std::string(name));
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)hasher.finalize());
    *path = directory + "/" + hex + ".vdb";
    const CacheDiskTierPtr tier = vdbDiskTier();
    std::error_code ec;
    if (std::filesystem::exists(*path, ec)) {
        if (tier) {
            tier->noteFileUsed(*path);
        }

        return true;
    }
    if (!geoWriteVDBFile(*grid.grid(), name, *path, errorMessage)) {
        return false;
    }
    if (tier) {
        tier->noteFileWritten(*path);
    }

    return true;
}

std::string
geoVDBCacheDirectory()
{
    if (const CacheDiskTierPtr tier = vdbDiskTier()) {
        return tier->filesDirectory("vdb");
    }

    return fallbackVDBCacheDirectory();
}

void
geoClearVDBCacheDirectory()
{
    if (const CacheDiskTierPtr tier = vdbDiskTier()) {
        tier->clearFiles();
    }
    std::error_code ignored;
    std::filesystem::remove_all(fallbackVDBCacheDirectory(), ignored);
}

NATRON_NAMESPACE_EXIT
