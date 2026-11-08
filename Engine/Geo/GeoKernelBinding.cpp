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

#include "GeoKernelBinding.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "Engine/Geo/GeoKernelTopology.h"
#include "Engine/Geo/GeoPointTree.h"
#include "Engine/Kernel/KernelParser.h"

NATRON_NAMESPACE_ENTER

namespace {

using B = KernelBaseType;

enum class Builtin : std::int32_t {
    ePtnum,
    ePrimnum,
    eVtxnum,
    eElemnum,
    eNumpt,
    eNumprim,
    eNumvtx,
    eNumelem,
    eFrame,
    eTime,
};

enum class Fn : std::uint8_t {
    eReadPoint,
    eReadPrimitive,
    eReadVertex,
    eReadDetail,
    eNPoints,
    eNPrimitives,
    eNVertices,
    eNearPoint,
    eNearPoints,
    eInPointGroup,
    eInPrimGroup,
    eSetPointGroup,
    eSetPrimGroup,
    eAddPoint,
    eAddPointFrom,
    eAddPrim,
    eAddVertex,
    eRemovePoint,
    eRemovePrim,
    eSetPrimVertex,
    eChFloat,
    eChInt,
    eChVector,
    eChString,
    eChRamp,
};

struct FnEntry {
    KernelFunctionSignature signature;
    Fn fn;
};

KernelParameter
constantParam(KernelType t)
{
    return KernelParameter { t, true };
}

KernelParameter
param(KernelType t)
{
    return KernelParameter { t, false };
}

std::vector<FnEntry>
buildFunctionTable()
{
    std::vector<FnEntry> table;
    const auto add = [&](const char* name, Fn fn, KernelType result, std::vector<KernelParameter> params, bool sideEffects) {
        FnEntry e;
        e.signature.name = name;
        e.signature.result = result;
        e.signature.params = std::move(params);
        e.signature.sideEffects = sideEffects;
        // Ids must be unique per signature: the front end merges calls whose id, name and result match.
        e.signature.id = std::int32_t(table.size());
        e.fn = fn;
        table.push_back(std::move(e));
    };
    const KernelType i(B::eInt);
    const KernelType f(B::eFloat);
    const KernelType v(B::eVector);
    const KernelType s(B::eString);
    const KernelType iarr(B::eInt, true);

    static const B readable[] = { B::eFloat, B::eInt, B::eVector2, B::eVector, B::eVector4, B::eMatrix3, B::eMatrix, B::eString };
    for (B t : readable) {
        add("point", Fn::eReadPoint, KernelType(t), { constantParam(i), constantParam(s), param(i) }, false);
        add("prim", Fn::eReadPrimitive, KernelType(t), { constantParam(i), constantParam(s), param(i) }, false);
        add("vertex", Fn::eReadVertex, KernelType(t), { constantParam(i), constantParam(s), param(i) }, false);
        add("detail", Fn::eReadDetail, KernelType(t), { constantParam(i), constantParam(s) }, false);
    }
    add("npoints", Fn::eNPoints, i, { constantParam(i) }, false);
    add("nprimitives", Fn::eNPrimitives, i, { constantParam(i) }, false);
    add("nvertices", Fn::eNVertices, i, { constantParam(i) }, false);
    add("nearpoint", Fn::eNearPoint, i, { constantParam(i), param(v) }, false);
    add("nearpoint", Fn::eNearPoint, i, { constantParam(i), param(v), param(f) }, false);
    add("nearpoints", Fn::eNearPoints, iarr, { constantParam(i), param(v), param(f) }, false);
    add("nearpoints", Fn::eNearPoints, iarr, { constantParam(i), param(v), param(f), param(i) }, false);
    add("inpointgroup", Fn::eInPointGroup, i, { constantParam(i), constantParam(s), param(i) }, false);
    add("inprimgroup", Fn::eInPrimGroup, i, { constantParam(i), constantParam(s), param(i) }, false);
    add("setpointgroup", Fn::eSetPointGroup, i, { constantParam(i), constantParam(s), param(i), param(i) }, true);
    add("setpointgroup", Fn::eSetPointGroup, i, { constantParam(i), constantParam(s), param(i), param(i), constantParam(s) }, true);
    add("setprimgroup", Fn::eSetPrimGroup, i, { constantParam(i), constantParam(s), param(i), param(i) }, true);
    add("setprimgroup", Fn::eSetPrimGroup, i, { constantParam(i), constantParam(s), param(i), param(i), constantParam(s) }, true);
    add("addpoint", Fn::eAddPoint, i, { constantParam(i), param(v) }, true);
    add("addpoint", Fn::eAddPointFrom, i, { constantParam(i), param(i) }, true);
    add("addprim", Fn::eAddPrim, i, { constantParam(i), constantParam(s) }, true);
    add("addprim", Fn::eAddPrim, i, { constantParam(i), constantParam(s), param(iarr) }, true);
    for (int n = 1; n <= 8; ++n) {
        std::vector<KernelParameter> params { constantParam(i), constantParam(s) };
        params.resize(std::size_t(2 + n), param(i));
        add("addprim", Fn::eAddPrim, i, std::move(params), true);
    }
    add("addvertex", Fn::eAddVertex, i, { constantParam(i), param(i), param(i) }, true);
    add("removepoint", Fn::eRemovePoint, i, { constantParam(i), param(i) }, true);
    add("removeprim", Fn::eRemovePrim, i, { constantParam(i), param(i), param(i) }, true);
    add("setprimvertex", Fn::eSetPrimVertex, i, { constantParam(i), param(i), param(i), param(i) }, true);
    add("ch", Fn::eChFloat, f, { constantParam(s) }, false);
    add("chf", Fn::eChFloat, f, { constantParam(s) }, false);
    add("chi", Fn::eChInt, i, { constantParam(s) }, false);
    add("chv", Fn::eChVector, v, { constantParam(s) }, false);
    add("chs", Fn::eChString, s, { constantParam(s) }, false);
    add("chramp", Fn::eChRamp, f, { constantParam(s), param(f) }, false);
    return table;
}

const std::vector<FnEntry>&
functionTable()
{
    static const std::vector<FnEntry> table = buildFunctionTable();
    return table;
}

bool
isParameterReader(Fn fn)
{
    return fn == Fn::eChFloat || fn == Fn::eChInt || fn == Fn::eChVector || fn == Fn::eChString || fn == Fn::eChRamp;
}

bool
parameterReadable(Fn fn, GeoKernelParameterKind kind)
{
    switch (fn) {
    case Fn::eChString:
        return kind == GeoKernelParameterKind::eString;
    case Fn::eChRamp:
        return kind == GeoKernelParameterKind::eRamp;
    default:
        return kind == GeoKernelParameterKind::eNumber;
    }
}

const char*
parameterKindName(GeoKernelParameterKind kind)
{
    switch (kind) {
    case GeoKernelParameterKind::eNumber:
        return "a number";
    case GeoKernelParameterKind::eString:
        return "a string";
    case GeoKernelParameterKind::eRamp:
        return "a ramp";
    }
    return "unknown";
}

// Empty when the parameter exists and fn can read it.
std::string
parameterError(const GeoKernelParameters* parameters, Fn fn, const char* function, const std::string& name)
{
    if (!parameters) {
        return "unknown parameter '" + name + "'";
    }
    const auto it = parameters->find(name);
    if (it == parameters->end()) {
        return "unknown parameter '" + name + "'";
    }
    if (!parameterReadable(fn, it->second.kind)) {
        return std::string("'") + function + "' cannot read the parameter '" + name + "': it is " + parameterKindName(it->second.kind);
    }
    return std::string();
}

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
    return "?";
}

GeoIndex
elementCount(const GeoDetail& detail, GeoAttribClassEnum cls)
{
    return cls == eGeoAttribClassDetail ? 1 : detail.indexMap(cls).size();
}

GeoOffset
offsetOf(const GeoDetail& detail, GeoAttribClassEnum cls, std::int64_t number)
{
    if (cls == eGeoAttribClassDetail) {
        return number == 0 ? 0 : GeoIndexMap::kInvalidOffset;
    }
    const GeoIndexMap& map = detail.indexMap(cls);
    if (number < 0 || number >= map.size()) {
        return GeoIndexMap::kInvalidOffset;
    }
    return map.offsetFromIndex(number);
}

std::int32_t
toInt32(double value)
{
    if (!(value == value)) {
        return 0;
    }
    if (value >= 2147483647.) {
        return INT32_MAX;
    }
    if (value <= -2147483648.) {
        return INT32_MIN;
    }
    return std::int32_t(value);
}

std::int32_t
clampInt32(std::int64_t value)
{
    return std::int32_t(std::clamp<std::int64_t>(value, INT32_MIN, INT32_MAX));
}

bool
isNumericStorage(GeoStorageEnum storage)
{
    return storage != eGeoStorageString;
}

double
readFloat(const GeoAttribute& a, GeoOffset offset, int component)
{
    switch (a.storage()) {
    case eGeoStorageInt32:
        return double(a.get<std::int32_t>(std::size_t(offset), component));
    case eGeoStorageInt64:
        return double(a.get<std::int64_t>(std::size_t(offset), component));
    case eGeoStorageFloat32:
        return double(a.get<float>(std::size_t(offset), component));
    case eGeoStorageFloat64:
        return a.get<double>(std::size_t(offset), component);
    case eGeoStorageString:
        break;
    }
    return 0.;
}

std::int32_t
readInt(const GeoAttribute& a, GeoOffset offset, int component)
{
    switch (a.storage()) {
    case eGeoStorageInt32:
        return a.get<std::int32_t>(std::size_t(offset), component);
    case eGeoStorageInt64:
        return clampInt32(a.get<std::int64_t>(std::size_t(offset), component));
    case eGeoStorageFloat32:
        return toInt32(a.get<float>(std::size_t(offset), component));
    case eGeoStorageFloat64:
        return toInt32(a.get<double>(std::size_t(offset), component));
    case eGeoStorageString:
        break;
    }
    return 0;
}

void
readNumericArray(const GeoAttribute& a, GeoOffset offset, std::vector<double>& out)
{
    out.clear();
    const auto take = [&](auto tag) {
        using T = decltype(tag);
        std::vector<T> values;
        a.getArray<T>(std::size_t(offset), values);
        out.assign(values.begin(), values.end());
    };
    switch (a.storage()) {
    case eGeoStorageInt32:
        take(std::int32_t());
        break;
    case eGeoStorageInt64:
        take(std::int64_t());
        break;
    case eGeoStorageFloat32:
        take(float());
        break;
    case eGeoStorageFloat64:
        take(double());
        break;
    case eGeoStorageString:
        break;
    }
}

void
writeNumericArray(GeoAttribute& a, GeoOffset offset, const std::vector<double>& in)
{
    const auto put = [&](auto tag) {
        using T = decltype(tag);
        std::vector<T> values(in.size());
        for (std::size_t k = 0; k < in.size(); ++k) {
            if constexpr (std::is_integral_v<T>) {
                values[k] = T(toInt32(in[k]));
            } else {
                values[k] = T(in[k]);
            }
        }
        a.setArray<T>(std::size_t(offset), std::span<const T>(values));
    };
    switch (a.storage()) {
    case eGeoStorageInt32:
        put(std::int32_t());
        break;
    case eGeoStorageInt64:
        put(std::int64_t());
        break;
    case eGeoStorageFloat32:
        put(float());
        break;
    case eGeoStorageFloat64:
        put(double());
        break;
    case eGeoStorageString:
        break;
    }
}

GeoAttributeDesc
descFor(const std::string& name, KernelType type)
{
    GeoAttributeDesc d;
    d.name = name;
    d.isArray = type.array;
    const int components = kernelComponentCount(type.base);
    if (type.base == B::eInt) {
        d.storage = eGeoStorageInt32;
    } else if (type.base == B::eString) {
        d.storage = eGeoStorageString;
    } else {
        d.storage = eGeoStorageFloat32;
        d.tupleSize = components;
    }
    if (kernelIsMatrix(type.base)) {
        d.typeInfo = eGeoTypeInfoMatrix;
    } else if (components == 3 && name == "N") {
        d.typeInfo = eGeoTypeInfoNormal;
    } else if (components == 3 && name == "Cd") {
        d.typeInfo = eGeoTypeInfoColour;
    } else if (components == 3 && (name == "v" || name == "up")) {
        d.typeInfo = eGeoTypeInfoVector;
    } else if (components == 4 && name == "orient") {
        d.typeInfo = eGeoTypeInfoQuaternion;
    }
    return d;
}

bool
canRead(const GeoAttribute& a, KernelType type)
{
    return a.isArray() == type.array && (a.storage() == eGeoStorageString) == (type.base == B::eString);
}

bool
canWrite(const GeoAttribute& a, KernelType type)
{
    if (!canRead(a, type)) {
        return false;
    }
    return type.base == B::eString || a.tupleSize() == kernelComponentCount(type.base);
}

std::string
typeMismatch(const std::string& name, GeoAttribClassEnum cls, const GeoAttribute& a, KernelType type)
{
    std::string stored = a.storage() == eGeoStorageString ? "string" : std::to_string(a.tupleSize()) + "-component number";
    if (a.isArray()) {
        stored += " array";
    }
    return "the " + std::string(className(cls)) + " attribute '" + name + "' holds a " + stored
        + " and cannot be used as " + kernelTypeName(type);
}

enum class Via : std::uint8_t {
    eSame, // the run class element with the same number
    eVertexPoint,
    eVertexPrimitive,
    eDetail,
};

// Where an attribute value comes from for one run-class element.
struct Source {
    const GeoDetail* detail = nullptr;
    const GeoAttribute* attribute = nullptr;
    GeoAttribClassEnum cls = eGeoAttribClassPoint;
    Via via = Via::eSame;
};

struct Slot {
    const KernelProgramBinding* binding = nullptr;
    bool builtin = false;
    Builtin builtinId = Builtin::ePtnum;
    Source source;
    GeoAttribute* writable = nullptr;
    std::mutex* lock = nullptr;
    // Numeric, not an array, and in the run class of input 0: moved a page at a time.
    bool staged = false;
};

struct CallSite {
    bool active = false;
    Fn fn = Fn::eReadPoint;
    const GeoDetail* detail = nullptr;
    GeoAttribClassEnum cls = eGeoAttribClassPoint;
    const GeoAttribute* attribute = nullptr;
    const GeoElementGroup* group = nullptr;
    std::int32_t groupId = -1;
    std::int32_t groupMode = 0;
    std::shared_ptr<const GeoPointTree> tree;
    GeoPrimitiveTypeEnum primitiveType = eGeoPrimitivePolygon;
    const GeoKernelParameter* parameter = nullptr;
};

struct Stage {
    GeoStorageEnum storage = eGeoStorageFloat32;
    int tuple = 1;
    std::size_t begin = 0;
    std::size_t end = 0;
    bool dirty = false;
    std::vector<std::int32_t> i32;
    std::vector<std::int64_t> i64;
    std::vector<float> f32;
    std::vector<double> f64;

    template <class T>
    std::vector<T>& values()
    {
        if constexpr (std::is_same_v<T, std::int32_t>) {
            return i32;
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            return i64;
        } else if constexpr (std::is_same_v<T, float>) {
            return f32;
        } else {
            return f64;
        }
    }
};

template <class F>
void
withStorage(GeoStorageEnum storage, F&& f)
{
    switch (storage) {
    case eGeoStorageInt32:
    case eGeoStorageString:
        f(std::int32_t());
        break;
    case eGeoStorageInt64:
        f(std::int64_t());
        break;
    case eGeoStorageFloat32:
        f(float());
        break;
    case eGeoStorageFloat64:
        f(double());
        break;
    }
}

class GeoKernelRun;

class GeoKernelWorker : public KernelRunWorker {
public:
    explicit GeoKernelWorker(GeoKernelRun& run);

    std::size_t beginItem(std::size_t item) override;
    void gather(std::size_t first, int laneCount, KernelRegisters& registers) override;
    void scatter(std::size_t first, int laneCount, const KernelRegisters& registers) override;
    void endItem(std::size_t item) override;
    void call(const KernelCall& call) override;

private:
    GeoOffset sourceOffset(const Source& source, std::size_t element) const;
    void gatherSlot(std::size_t slot, std::size_t first, int laneCount, KernelRegisters& registers);
    void scatterSlot(std::size_t slot, std::size_t first, int laneCount, const KernelRegisters& registers);
    void readElement(const KernelCall& call, const CallSite& site);
    void readParameter(const KernelCall& call, const GeoKernelParameter& parameter);
    GeoKernelEdit& queueEdit(GeoKernelEditKind kind, int lane);

    GeoKernelRun& _run;
    std::size_t _item = 0;
    std::size_t _batchFirst = 0;
    std::vector<GeoIndex> _indices;
    std::vector<GeoOffset> _offsets;
    std::vector<Stage> _stages;
    std::vector<double> _scratch;
    std::vector<GeoIndex> _near;
};

class GeoKernelRun : public KernelRunContext {
public:
    GeoKernelRun(const KernelProgram& program, GeoAttribClassEnum runOver, std::span<const GeoDetail* const> inputs,
                 const GeoKernelRunOptions& options);

    std::size_t itemCount() const override
    {
        return _itemCount;
    }

    std::unique_ptr<KernelRunWorker> createWorker() override
    {
        return std::make_unique<GeoKernelWorker>(*this);
    }

    GeoDetail finish()
    {
        _topology->apply(_builder, _queues);
        return _builder.finish();
    }

private:
    friend class GeoKernelWorker;

    const GeoDetail* input(std::int64_t n) const
    {
        return n >= 0 && std::size_t(n) < _inputs.size() ? _inputs[std::size_t(n)] : nullptr;
    }

    Source findSource(const GeoDetail* detail, const std::string& name) const;
    void prepareSlot(const KernelProgramBinding& binding);
    void prepareCall(std::uint32_t instruction, const KernelInstruction& in);
    std::int32_t constantInt(const KernelOperand& operand, const char* function) const;
    std::string constantString(const KernelOperand& operand, const char* function) const;

    const KernelProgram& _program;
    GeoAttribClassEnum _runOver;
    std::span<const GeoDetail* const> _inputs;
    const GeoDetail& _input;
    GeoKernelRunOptions _options;
    GeoDetailBuilder _builder;
    std::size_t _itemCount = 0;
    std::vector<Slot> _slots;
    std::vector<CallSite> _calls;
    std::deque<std::mutex> _locks;
    std::vector<GeoKernelEditQueue> _queues;
    std::unique_ptr<GeoKernelTopology> _topology;
    const GeoElementGroup* _group = nullptr;
};

GeoKernelRun::GeoKernelRun(const KernelProgram& program, GeoAttribClassEnum runOver,
                           std::span<const GeoDetail* const> inputs, const GeoKernelRunOptions& options)
    : _program(program)
    , _runOver(runOver)
    , _inputs(inputs)
    , _input(*inputs[0])
    , _options(options)
    , _builder(*inputs[0])
{
    _itemCount = runOver == eGeoAttribClassDetail ? 1 : std::size_t(_input.indexMap(runOver).pageCount());
    if (!options.group.empty() && runOver != eGeoAttribClassDetail) {
        _group = _input.findGroup(runOver, options.group);
        if (!_group) {
            throw KernelRunError(std::string("the geometry has no ") + className(runOver) + " group named '" + options.group + "'");
        }
    }
    _queues.resize(_itemCount);
    _topology = std::make_unique<GeoKernelTopology>(_input, _itemCount);
    for (const KernelProgramBinding& b : program.bindings) {
        prepareSlot(b);
    }
    _calls.resize(program.instructions.size());
    for (std::uint32_t k = 0; k < program.instructions.size(); ++k) {
        prepareCall(k, program.instructions[k]);
    }
}

Source
GeoKernelRun::findSource(const GeoDetail* detail, const std::string& name) const
{
    Source s;
    s.detail = detail;
    s.cls = _runOver;
    if (!detail) {
        return s;
    }
    if ((s.attribute = detail->findAttribute(_runOver, name))) {
        return s;
    }
    if (_runOver == eGeoAttribClassVertex) {
        if ((s.attribute = detail->findAttribute(eGeoAttribClassPoint, name))) {
            s.cls = eGeoAttribClassPoint;
            s.via = Via::eVertexPoint;
            return s;
        }
        if ((s.attribute = detail->findAttribute(eGeoAttribClassPrimitive, name))) {
            s.cls = eGeoAttribClassPrimitive;
            s.via = Via::eVertexPrimitive;
            return s;
        }
    }
    if (_runOver != eGeoAttribClassDetail && (s.attribute = detail->findAttribute(eGeoAttribClassDetail, name))) {
        s.cls = eGeoAttribClassDetail;
        s.via = Via::eDetail;
        return s;
    }
    return s;
}

void
GeoKernelRun::prepareSlot(const KernelProgramBinding& b)
{
    Slot slot;
    slot.binding = &b;
    if (b.variable.kind == KernelVariableKind::eBuiltin) {
        slot.builtin = true;
        slot.builtinId = Builtin(b.variable.id);
        _slots.push_back(slot);
        return;
    }
    slot.source = findSource(input(b.input), b.name);
    if (b.written) {
        if (b.input != 0) {
            throw KernelRunError("'@opinput" + std::to_string(b.input) + "_" + b.name + "' is read-only");
        }
        if (slot.source.attribute && slot.source.cls != _runOver) {
            throw KernelRunError("'" + b.name + "' is a " + className(slot.source.cls) + " attribute; a "
                                 + className(_runOver) + " kernel writes " + className(_runOver) + " attributes");
        }
        if (!slot.source.attribute) {
            _builder.addAttribute(_runOver, descFor(b.name, b.type));
        }
        slot.writable = _builder.attributeForWriting(_runOver, b.name);
        if (!slot.writable) {
            throw KernelRunError("cannot write the attribute '" + b.name + "'");
        }
        if (!canWrite(*slot.writable, b.type)) {
            throw KernelRunError(typeMismatch(b.name, _runOver, *slot.writable, b.type));
        }
        slot.writable->makePageTableUnique();
        slot.source.attribute = slot.writable;
        slot.source.cls = _runOver;
        slot.source.via = Via::eSame;
        if (b.type.array || b.type.base == B::eString) {
            _locks.emplace_back();
            slot.lock = &_locks.back();
        }
    } else if (slot.source.attribute && !canRead(*slot.source.attribute, b.type)) {
        throw KernelRunError(typeMismatch(b.name, slot.source.cls, *slot.source.attribute, b.type));
    }
    slot.staged = slot.source.attribute && b.input == 0 && slot.source.via == Via::eSame && !b.type.array
        && isNumericStorage(slot.source.attribute->storage());
    _slots.push_back(slot);
}

std::int32_t
GeoKernelRun::constantInt(const KernelOperand& operand, const char* function) const
{
    const std::optional<std::int32_t> v = kernelConstantInt(_program, operand);
    if (!v) {
        throw KernelRunError(std::string("'") + function + "' needs a literal input number");
    }
    return *v;
}

std::string
GeoKernelRun::constantString(const KernelOperand& operand, const char* function) const
{
    const std::string* s = kernelConstantString(_program, operand);
    if (!s) {
        throw KernelRunError(std::string("'") + function + "' needs a literal name");
    }
    return *s;
}

void
GeoKernelRun::prepareCall(std::uint32_t instruction, const KernelInstruction& in)
{
    if (in.op != KernelOp::eCall) {
        return;
    }
    const KernelProgramFunction& function = _program.functions.at(std::size_t(in.imm));
    if (function.provider != KernelFunctionProvider::eBinding) {
        return;
    }
    const std::vector<FnEntry>& table = functionTable();
    if (function.signature.id < 0 || std::size_t(function.signature.id) >= table.size()) {
        throw KernelRunError("'" + function.signature.name + "' is not a geometry function");
    }
    CallSite site;
    site.active = true;
    site.fn = table[std::size_t(function.signature.id)].fn;
    const char* name = function.signature.name.c_str();
    const KernelOperand* args = _program.operands.data() + in.operandBegin + in.b;
    if (isParameterReader(site.fn)) {
        const std::string parameter = constantString(args[0], name);
        const std::string error = parameterError(_options.parameters, site.fn, name, parameter);
        if (!error.empty()) {
            throw KernelRunError(error);
        }
        site.parameter = &_options.parameters->find(parameter)->second;
        _calls[instruction] = std::move(site);
        return;
    }
    const std::int32_t handle = constantInt(args[0], name);

    switch (site.fn) {
    case Fn::eReadPoint:
    case Fn::eReadPrimitive:
    case Fn::eReadVertex:
    case Fn::eReadDetail: {
        site.cls = site.fn == Fn::eReadPoint ? eGeoAttribClassPoint
            : site.fn == Fn::eReadPrimitive  ? eGeoAttribClassPrimitive
            : site.fn == Fn::eReadVertex     ? eGeoAttribClassVertex
                                             : eGeoAttribClassDetail;
        site.detail = input(handle);
        const std::string attribute = constantString(args[1], name);
        site.attribute = site.detail ? site.detail->findAttribute(site.cls, attribute) : nullptr;
        if (site.attribute && (site.attribute->isArray() || (site.attribute->storage() == eGeoStorageString) != (function.signature.result.base == B::eString))) {
            throw KernelRunError(typeMismatch(attribute, site.cls, *site.attribute, function.signature.result));
        }
        break;
    }
    case Fn::eNPoints:
    case Fn::eNPrimitives:
    case Fn::eNVertices:
    case Fn::eNearPoint:
    case Fn::eNearPoints:
        site.detail = input(handle);
        if (site.detail && (site.fn == Fn::eNearPoint || site.fn == Fn::eNearPoints)) {
            site.tree = GeoPointTree::forDetail(*site.detail);
        }
        break;
    case Fn::eInPointGroup:
    case Fn::eInPrimGroup:
        site.detail = input(handle);
        site.cls = site.fn == Fn::eInPointGroup ? eGeoAttribClassPoint : eGeoAttribClassPrimitive;
        site.group = site.detail ? site.detail->findGroup(site.cls, constantString(args[1], name)) : nullptr;
        break;
    case Fn::eSetPointGroup:
    case Fn::eSetPrimGroup: {
        if (handle != 0) {
            throw KernelRunError(std::string("'") + name + "' writes geometry 0 only");
        }
        site.cls = site.fn == Fn::eSetPointGroup ? eGeoAttribClassPoint : eGeoAttribClassPrimitive;
        site.groupId = _topology->group(site.cls, constantString(args[1], name));
        if (function.signature.params.size() == 5) {
            const std::string mode = constantString(args[4], name);
            if (mode == "set") {
                site.groupMode = std::int32_t(GeoKernelGroupMode::eSet);
            } else if (mode == "add") {
                site.groupMode = std::int32_t(GeoKernelGroupMode::eAdd);
            } else if (mode == "remove") {
                site.groupMode = std::int32_t(GeoKernelGroupMode::eRemove);
            } else if (mode == "toggle") {
                site.groupMode = std::int32_t(GeoKernelGroupMode::eToggle);
            } else {
                throw KernelRunError(std::string("'") + name + "': unknown mode '" + mode + "'; use set, add, remove or toggle");
            }
        }
        break;
    }
    case Fn::eAddPrim: {
        const std::string type = constantString(args[1], name);
        if (type == "poly") {
            site.primitiveType = eGeoPrimitivePolygon;
        } else if (type == "polyline") {
            site.primitiveType = eGeoPrimitivePolyline;
        } else {
            throw KernelRunError("'addprim': unknown primitive type '" + type + "'; use poly or polyline");
        }
        [[fallthrough]];
    }
    case Fn::eAddPoint:
    case Fn::eAddPointFrom:
    case Fn::eAddVertex:
    case Fn::eRemovePoint:
    case Fn::eRemovePrim:
    case Fn::eSetPrimVertex:
        if (handle != 0) {
            throw KernelRunError(std::string("'") + name + "' writes geometry 0 only");
        }
        break;
    case Fn::eChFloat:
    case Fn::eChInt:
    case Fn::eChVector:
    case Fn::eChString:
    case Fn::eChRamp:
        break;
    }
    _calls[instruction] = std::move(site);
}

GeoKernelWorker::GeoKernelWorker(GeoKernelRun& run)
    : _run(run)
    , _stages(run._slots.size())
{
}

std::size_t
GeoKernelWorker::beginItem(std::size_t item)
{
    _item = item;
    _indices.clear();
    _offsets.clear();
    if (_run._runOver == eGeoAttribClassDetail) {
        _indices.push_back(0);
        _offsets.push_back(0);
    } else {
        const GeoElementGroup* group = _run._group;
        _run._input.indexMap(_run._runOver).forEachLiveRunInPage(GeoOffset(item), [&](GeoIndex index, GeoOffset offset, GeoOffset count) {
            for (GeoOffset k = 0; k < count; ++k) {
                if (group && !group->contains(offset + k)) {
                    continue;
                }
                _indices.push_back(index + k);
                _offsets.push_back(offset + k);
            }
        });
    }
    if (_offsets.empty()) {
        return 0;
    }
    const std::size_t pageBegin = std::size_t(item) * kGeoPageSize;
    for (std::size_t s = 0; s < _run._slots.size(); ++s) {
        const Slot& slot = _run._slots[s];
        if (!slot.staged) {
            continue;
        }
        const GeoAttribute& a = *slot.source.attribute;
        Stage& stage = _stages[s];
        stage.storage = a.storage();
        stage.tuple = a.tupleSize();
        stage.begin = pageBegin;
        stage.end = std::min(pageBegin + kGeoPageSize, a.size());
        stage.dirty = false;
        withStorage(stage.storage, [&](auto tag) {
            using T = decltype(tag);
            std::vector<T>& values = stage.values<T>();
            values.resize((stage.end - stage.begin) * std::size_t(stage.tuple));
            a.copyDense<T>(stage.begin, stage.end, std::span<T>(values));
        });
    }
    return _offsets.size();
}

void
GeoKernelWorker::endItem(std::size_t)
{
    for (std::size_t s = 0; s < _run._slots.size(); ++s) {
        Stage& stage = _stages[s];
        if (!_run._slots[s].staged || !stage.dirty) {
            continue;
        }
        GeoAttribute& a = *_run._slots[s].writable;
        withStorage(stage.storage, [&](auto tag) {
            using T = decltype(tag);
            const std::vector<T>& values = stage.values<T>();
            a.scatterDense<T>(stage.begin, stage.end, std::span<const T>(values));
        });
        stage.dirty = false;
    }
}

GeoOffset
GeoKernelWorker::sourceOffset(const Source& source, std::size_t element) const
{
    if (!source.detail || !source.attribute) {
        return GeoIndexMap::kInvalidOffset;
    }
    if (source.via == Via::eDetail || _run._runOver == eGeoAttribClassDetail) {
        return 0;
    }
    GeoOffset offset = source.detail == &_run._input ? _offsets[element] : offsetOf(*source.detail, _run._runOver, _indices[element]);
    if (offset < 0) {
        return offset;
    }
    if (source.via == Via::eVertexPoint) {
        offset = source.detail->vertexPoint(offset);
    } else if (source.via == Via::eVertexPrimitive) {
        offset = source.detail->vertexPrimitive(offset);
    }
    return offset;
}

void
GeoKernelWorker::gather(std::size_t first, int laneCount, KernelRegisters& registers)
{
    _batchFirst = first;
    for (std::size_t s = 0; s < _run._slots.size(); ++s) {
        if (_run._slots[s].binding->needsGather) {
            gatherSlot(s, first, laneCount, registers);
        }
    }
}

void
GeoKernelWorker::gatherSlot(std::size_t s, std::size_t first, int laneCount, KernelRegisters& registers)
{
    const Slot& slot = _run._slots[s];
    const KernelProgramBinding& b = *slot.binding;
    const GeoDetail& in = _run._input;

    if (slot.builtin) {
        if (slot.builtinId == Builtin::eFrame || slot.builtinId == Builtin::eTime) {
            float* out = registers.f(b.registers[0]);
            const float v = float(slot.builtinId == Builtin::eFrame ? _run._options.frame : _run._options.time);
            std::fill(out, out + laneCount, v);
            return;
        }
        std::int32_t* out = registers.i(b.registers[0]);
        for (int l = 0; l < laneCount; ++l) {
            const std::size_t e = first + std::size_t(l);
            std::int64_t v = -1;
            switch (slot.builtinId) {
            case Builtin::ePtnum:
                if (_run._runOver == eGeoAttribClassPoint) {
                    v = _indices[e];
                } else if (_run._runOver == eGeoAttribClassVertex) {
                    v = in.pointMap().indexFromOffset(in.vertexPoint(_offsets[e]));
                }
                break;
            case Builtin::ePrimnum:
                if (_run._runOver == eGeoAttribClassPrimitive) {
                    v = _indices[e];
                } else if (_run._runOver == eGeoAttribClassVertex) {
                    v = in.primitiveMap().indexFromOffset(in.vertexPrimitive(_offsets[e]));
                }
                break;
            case Builtin::eVtxnum:
                if (_run._runOver == eGeoAttribClassVertex) {
                    v = _indices[e];
                }
                break;
            case Builtin::eElemnum:
                v = _indices[e];
                break;
            case Builtin::eNumpt:
                v = in.pointCount();
                break;
            case Builtin::eNumprim:
                v = in.primitiveCount();
                break;
            case Builtin::eNumvtx:
                v = in.vertexCount();
                break;
            case Builtin::eNumelem:
                v = elementCount(in, _run._runOver);
                break;
            case Builtin::eFrame:
            case Builtin::eTime:
                break;
            }
            out[l] = clampInt32(v);
        }
        return;
    }

    const KernelType type = b.type;
    const GeoAttribute* a = slot.source.attribute;

    if (slot.staged) {
        Stage& stage = _stages[s];
        const int components = kernelComponentCount(type.base);
        withStorage(stage.storage, [&](auto tag) {
            using T = decltype(tag);
            const std::vector<T>& values = stage.values<T>();
            for (int c = 0; c < components; ++c) {
                if (type.base == B::eInt) {
                    std::int32_t* out = registers.i(b.registers[std::size_t(c)]);
                    for (int l = 0; l < laneCount; ++l) {
                        const std::size_t local = std::size_t(_offsets[first + std::size_t(l)]) - stage.begin;
                        if (c >= stage.tuple) {
                            out[l] = 0;
                        } else if constexpr (std::is_same_v<T, std::int32_t>) {
                            out[l] = values[local * std::size_t(stage.tuple) + std::size_t(c)];
                        } else if constexpr (std::is_same_v<T, std::int64_t>) {
                            out[l] = clampInt32(values[local * std::size_t(stage.tuple) + std::size_t(c)]);
                        } else {
                            out[l] = toInt32(double(values[local * std::size_t(stage.tuple) + std::size_t(c)]));
                        }
                    }
                } else {
                    float* out = registers.f(b.registers[std::size_t(c)]);
                    for (int l = 0; l < laneCount; ++l) {
                        const std::size_t local = std::size_t(_offsets[first + std::size_t(l)]) - stage.begin;
                        out[l] = c < stage.tuple ? float(values[local * std::size_t(stage.tuple) + std::size_t(c)]) : 0.f;
                    }
                }
            }
        });
        return;
    }

    std::unique_lock<std::mutex> lock;
    if (slot.lock) {
        lock = std::unique_lock<std::mutex>(*slot.lock);
    }
    for (int l = 0; l < laneCount; ++l) {
        const GeoOffset offset = sourceOffset(slot.source, first + std::size_t(l));
        const bool present = offset >= 0;
        if (type.array) {
            KernelArrayValue& out = registers.a(b.registers[0])[l];
            out.clear();
            out.type = type.base;
            if (!present) {
                continue;
            }
            if (type.base == B::eString) {
                a->getStringArray(std::size_t(offset), out.strings);
                continue;
            }
            readNumericArray(*a, offset, _scratch);
            const int tuple = a->tupleSize();
            const int components = kernelComponentCount(type.base);
            const std::size_t count = _scratch.size() / std::size_t(tuple);
            for (std::size_t k = 0; k < count; ++k) {
                for (int c = 0; c < components; ++c) {
                    const double v = c < tuple ? _scratch[k * std::size_t(tuple) + std::size_t(c)] : 0.;
                    if (type.base == B::eInt) {
                        out.ints.push_back(toInt32(v));
                    } else {
                        out.floats.push_back(float(v));
                    }
                }
            }
            continue;
        }
        if (type.base == B::eString) {
            std::string& out = registers.s(b.registers[0])[l];
            if (present) {
                out.assign(a->getString(std::size_t(offset)));
            } else {
                out.clear();
            }
            continue;
        }
        const int components = kernelComponentCount(type.base);
        for (int c = 0; c < components; ++c) {
            const bool has = present && c < a->tupleSize();
            if (type.base == B::eInt) {
                registers.i(b.registers[std::size_t(c)])[l] = has ? readInt(*a, offset, c) : 0;
            } else {
                registers.f(b.registers[std::size_t(c)])[l] = has ? float(readFloat(*a, offset, c)) : 0.f;
            }
        }
    }
}

void
GeoKernelWorker::scatter(std::size_t first, int laneCount, const KernelRegisters& registers)
{
    for (std::size_t s = 0; s < _run._slots.size(); ++s) {
        if (_run._slots[s].binding->written) {
            scatterSlot(s, first, laneCount, registers);
        }
    }
}

void
GeoKernelWorker::scatterSlot(std::size_t s, std::size_t first, int laneCount, const KernelRegisters& registers)
{
    const Slot& slot = _run._slots[s];
    const KernelProgramBinding& b = *slot.binding;
    const KernelType type = b.type;
    GeoAttribute& a = *slot.writable;

    if (slot.staged) {
        Stage& stage = _stages[s];
        stage.dirty = true;
        const int components = std::min(kernelComponentCount(type.base), stage.tuple);
        withStorage(stage.storage, [&](auto tag) {
            using T = decltype(tag);
            std::vector<T>& values = stage.values<T>();
            for (int c = 0; c < components; ++c) {
                for (int l = 0; l < laneCount; ++l) {
                    const std::size_t local = std::size_t(_offsets[first + std::size_t(l)]) - stage.begin;
                    T& dst = values[local * std::size_t(stage.tuple) + std::size_t(c)];
                    if (type.base == B::eInt) {
                        const std::int32_t v = registers.i(b.registers[std::size_t(c)])[l];
                        dst = T(v);
                    } else {
                        const float v = registers.f(b.registers[std::size_t(c)])[l];
                        if constexpr (std::is_integral_v<T>) {
                            dst = T(toInt32(double(v)));
                        } else {
                            dst = T(v);
                        }
                    }
                }
            }
        });
        return;
    }

    std::lock_guard<std::mutex> lock(*slot.lock);
    for (int l = 0; l < laneCount; ++l) {
        const std::size_t offset = std::size_t(_offsets[first + std::size_t(l)]);
        if (!type.array) {
            a.setString(offset, registers.s(b.registers[0])[l]);
            continue;
        }
        const KernelArrayValue& in = registers.a(b.registers[0])[l];
        if (type.base == B::eString) {
            a.setStringArray(offset, std::span<const std::string>(in.strings));
            continue;
        }
        const int components = kernelComponentCount(type.base);
        _scratch.clear();
        if (type.base == B::eInt) {
            _scratch.assign(in.ints.begin(), in.ints.end());
        } else {
            _scratch.assign(in.floats.begin(), in.floats.end());
        }
        _scratch.resize(_scratch.size() / std::size_t(components) * std::size_t(components));
        writeNumericArray(a, GeoOffset(offset), _scratch);
    }
}

GeoKernelEdit&
GeoKernelWorker::queueEdit(GeoKernelEditKind kind, int lane)
{
    GeoKernelEditQueue& queue = _run._queues[_item];
    queue.edits.emplace_back();
    GeoKernelEdit& e = queue.edits.back();
    e.kind = kind;
    e.element = std::int32_t(_batchFirst + std::size_t(lane));
    return e;
}

void
GeoKernelWorker::readElement(const KernelCall& call, const CallSite& site)
{
    const KernelType result = call.function.signature.result;
    const int components = result.base == B::eString ? 1 : kernelComponentCount(result.base);
    const bool hasElement = site.fn != Fn::eReadDetail;
    const std::int32_t* elements = hasElement ? call.registers.i(call.arguments[2].reg) : nullptr;
    for (int l = 0; l < call.laneCount; ++l) {
        if (!call.mask[l]) {
            continue;
        }
        GeoOffset offset = GeoIndexMap::kInvalidOffset;
        if (site.attribute) {
            offset = offsetOf(*site.detail, site.cls, hasElement ? elements[l] : 0);
        }
        if (result.base == B::eString) {
            std::string& out = call.registers.s(call.results[0].reg)[l];
            if (offset >= 0) {
                out.assign(site.attribute->getString(std::size_t(offset)));
            } else {
                out.clear();
            }
            continue;
        }
        for (int c = 0; c < components; ++c) {
            const bool has = offset >= 0 && c < site.attribute->tupleSize();
            if (result.base == B::eInt) {
                call.registers.i(call.results[std::size_t(c)].reg)[l] = has ? readInt(*site.attribute, offset, c) : 0;
            } else {
                call.registers.f(call.results[std::size_t(c)].reg)[l] = has ? float(readFloat(*site.attribute, offset, c)) : 0.f;
            }
        }
    }
}

void
GeoKernelWorker::call(const KernelCall& call)
{
    const CallSite& site = _run._calls.at(call.instruction);
    if (!site.active) {
        throw KernelRunError("'" + call.function.signature.name + "' is not a geometry function");
    }
    KernelRegisters& r = call.registers;
    const auto argI = [&](std::size_t k) { return r.i(call.arguments[k].reg); };
    const auto argF = [&](std::size_t k) { return r.f(call.arguments[k].reg); };
    std::int32_t* resultI = call.results.empty() || call.results[0].bank != KernelBank::eInt ? nullptr : r.i(call.results[0].reg);

    switch (site.fn) {
    case Fn::eReadPoint:
    case Fn::eReadPrimitive:
    case Fn::eReadVertex:
    case Fn::eReadDetail:
        readElement(call, site);
        return;
    case Fn::eNPoints:
    case Fn::eNPrimitives:
    case Fn::eNVertices: {
        std::int64_t n = 0;
        if (site.detail) {
            n = site.fn == Fn::eNPoints       ? site.detail->pointCount()
                : site.fn == Fn::eNPrimitives ? site.detail->primitiveCount()
                                              : site.detail->vertexCount();
        }
        for (int l = 0; l < call.laneCount; ++l) {
            if (call.mask[l]) {
                resultI[l] = clampInt32(n);
            }
        }
        return;
    }
    case Fn::eNearPoint: {
        const bool limited = call.arguments.size() > 4;
        for (int l = 0; l < call.laneCount; ++l) {
            if (!call.mask[l]) {
                continue;
            }
            GeoIndex found = -1;
            if (site.tree) {
                const std::array<double, 3> p { argF(1)[l], argF(2)[l], argF(3)[l] };
                found = limited ? site.tree->nearest(p, double(argF(4)[l])) : site.tree->nearest(p);
            }
            resultI[l] = clampInt32(found);
        }
        return;
    }
    case Fn::eNearPoints: {
        const bool capped = call.arguments.size() > 5;
        for (int l = 0; l < call.laneCount; ++l) {
            if (!call.mask[l]) {
                continue;
            }
            KernelArrayValue& out = r.a(call.results[0].reg)[l];
            out.clear();
            out.type = B::eInt;
            if (!site.tree) {
                continue;
            }
            const std::array<double, 3> p { argF(1)[l], argF(2)[l], argF(3)[l] };
            site.tree->within(p, double(argF(4)[l]), capped ? std::int64_t(argI(5)[l]) : 0, _near);
            out.ints.reserve(_near.size());
            for (GeoIndex n : _near) {
                out.ints.push_back(clampInt32(n));
            }
        }
        return;
    }
    case Fn::eInPointGroup:
    case Fn::eInPrimGroup: {
        const std::int32_t* elements = argI(2);
        for (int l = 0; l < call.laneCount; ++l) {
            if (!call.mask[l]) {
                continue;
            }
            std::int32_t member = 0;
            if (site.group) {
                const GeoOffset offset = offsetOf(*site.detail, site.cls, elements[l]);
                member = offset >= 0 && site.group->contains(offset) ? 1 : 0;
            }
            resultI[l] = member;
        }
        return;
    }
    case Fn::eSetPointGroup:
    case Fn::eSetPrimGroup: {
        const std::int32_t* elements = argI(2);
        const std::int32_t* values = argI(3);
        for (int l = 0; l < call.laneCount; ++l) {
            if (!call.mask[l]) {
                continue;
            }
            GeoKernelEdit& e = queueEdit(GeoKernelEditKind::eSetGroup, l);
            e.a = site.groupId;
            e.b = elements[l];
            e.c = values[l];
            e.d = site.groupMode;
            resultI[l] = elements[l];
        }
        return;
    }
    case Fn::eAddPoint:
    case Fn::eAddPointFrom:
        for (int l = 0; l < call.laneCount; ++l) {
            if (!call.mask[l]) {
                continue;
            }
            const std::int32_t handle = _run._topology->newPoint(_item, _run._queues[_item]);
            GeoKernelEdit& e = queueEdit(site.fn == Fn::eAddPoint ? GeoKernelEditKind::eAddPoint : GeoKernelEditKind::eAddPointFrom, l);
            e.handle = handle;
            if (site.fn == Fn::eAddPoint) {
                e.position = { argF(1)[l], argF(2)[l], argF(3)[l] };
            } else {
                e.a = argI(1)[l];
            }
            resultI[l] = handle;
        }
        return;
    case Fn::eAddPrim: {
        const bool fromArray = call.arguments.size() == 3 && call.arguments[2].bank == KernelBank::eArray;
        for (int l = 0; l < call.laneCount; ++l) {
            if (!call.mask[l]) {
                continue;
            }
            const std::int32_t handle = _run._topology->newPrimitive(_item, _run._queues[_item]);
            GeoKernelEdit& e = queueEdit(GeoKernelEditKind::eAddPrimitive, l);
            e.handle = handle;
            e.primitiveType = site.primitiveType;
            if (fromArray) {
                const KernelArrayValue& points = r.a(call.arguments[2].reg)[l];
                e.points.assign(points.ints.begin(), points.ints.end());
            } else {
                for (std::size_t k = 2; k < call.arguments.size(); ++k) {
                    e.points.push_back(argI(k)[l]);
                }
            }
            resultI[l] = handle;
        }
        return;
    }
    case Fn::eAddVertex:
        for (int l = 0; l < call.laneCount; ++l) {
            if (!call.mask[l]) {
                continue;
            }
            const std::int32_t handle = _run._topology->newVertex(_item, _run._queues[_item]);
            GeoKernelEdit& e = queueEdit(GeoKernelEditKind::eAddVertex, l);
            e.handle = handle;
            e.a = argI(1)[l];
            e.b = argI(2)[l];
            resultI[l] = handle;
        }
        return;
    case Fn::eRemovePoint:
        for (int l = 0; l < call.laneCount; ++l) {
            if (!call.mask[l]) {
                continue;
            }
            GeoKernelEdit& e = queueEdit(GeoKernelEditKind::eRemovePoint, l);
            e.a = argI(1)[l];
            resultI[l] = e.a;
        }
        return;
    case Fn::eRemovePrim:
        for (int l = 0; l < call.laneCount; ++l) {
            if (!call.mask[l]) {
                continue;
            }
            GeoKernelEdit& e = queueEdit(GeoKernelEditKind::eRemovePrimitive, l);
            e.a = argI(1)[l];
            e.b = argI(2)[l] != 0 ? 1 : 0;
            resultI[l] = e.a;
        }
        return;
    case Fn::eSetPrimVertex:
        for (int l = 0; l < call.laneCount; ++l) {
            if (!call.mask[l]) {
                continue;
            }
            GeoKernelEdit& e = queueEdit(GeoKernelEditKind::eSetPrimitiveVertex, l);
            e.a = argI(1)[l];
            e.b = argI(2)[l];
            e.c = argI(3)[l];
            resultI[l] = e.a;
        }
        return;
    case Fn::eChFloat:
    case Fn::eChInt:
    case Fn::eChVector:
    case Fn::eChString:
    case Fn::eChRamp:
        readParameter(call, *site.parameter);
        return;
    }
}

void
GeoKernelWorker::readParameter(const KernelCall& call, const GeoKernelParameter& parameter)
{
    KernelRegisters& r = call.registers;
    const Fn fn = functionTable()[std::size_t(call.function.signature.id)].fn;
    if (fn == Fn::eChString) {
        std::string* out = r.s(call.results[0].reg);
        for (int l = 0; l < call.laneCount; ++l) {
            if (call.mask[l]) {
                out[l] = parameter.string;
            }
        }
        return;
    }
    if (fn == Fn::eChInt) {
        std::int32_t* out = r.i(call.results[0].reg);
        const std::int32_t v = toInt32(std::round(parameter.values[0]));
        for (int l = 0; l < call.laneCount; ++l) {
            if (call.mask[l]) {
                out[l] = v;
            }
        }
        return;
    }
    if (fn == Fn::eChRamp) {
        float* out = r.f(call.results[0].reg);
        const float* x = r.f(call.arguments[1].reg);
        for (int l = 0; l < call.laneCount; ++l) {
            if (call.mask[l]) {
                out[l] = parameter.ramp ? float(parameter.ramp(double(x[l]))) : 0.f;
            }
        }
        return;
    }
    for (std::size_t c = 0; c < call.results.size(); ++c) {
        float* out = r.f(call.results[c].reg);
        const float v = int(c) < parameter.components ? float(parameter.values[c]) : 0.f;
        for (int l = 0; l < call.laneCount; ++l) {
            if (call.mask[l]) {
                out[l] = v;
            }
        }
    }
}

const std::map<std::string_view, std::pair<Builtin, B>>&
builtinTable()
{
    static const std::map<std::string_view, std::pair<Builtin, B>> table = {
        { "ptnum", { Builtin::ePtnum, B::eInt } },
        { "primnum", { Builtin::ePrimnum, B::eInt } },
        { "vtxnum", { Builtin::eVtxnum, B::eInt } },
        { "elemnum", { Builtin::eElemnum, B::eInt } },
        { "numpt", { Builtin::eNumpt, B::eInt } },
        { "numprim", { Builtin::eNumprim, B::eInt } },
        { "numvtx", { Builtin::eNumvtx, B::eInt } },
        { "numelem", { Builtin::eNumelem, B::eInt } },
        { "Frame", { Builtin::eFrame, B::eFloat } },
        { "Time", { Builtin::eTime, B::eFloat } },
    };
    return table;
}

} // namespace

GeoKernelBinding::GeoKernelBinding(GeoAttribClassEnum runOver,
                                   const GeoKernelParameters* parameters)
    : _runOver(runOver)
    , _parameters(parameters)
{
}

std::optional<KernelVariable>
GeoKernelBinding::builtinVariable(std::string_view name) const
{
    const auto& table = builtinTable();
    const auto it = table.find(name);
    if (it == table.end()) {
        return std::nullopt;
    }
    KernelVariable v;
    v.kind = KernelVariableKind::eBuiltin;
    v.id = std::int32_t(it->second.first);
    v.domain = std::int32_t(_runOver);
    v.type = KernelType(it->second.second);
    v.readOnly = true;
    return v;
}

KernelBindResult
GeoKernelBinding::bindAttribute(const KernelAttributeRequest& request) const
{
    KernelBindResult result;
    if (request.name.empty()) {
        result.error = "an attribute needs a name";
        return result;
    }
    KernelVariable v;
    v.kind = KernelVariableKind::eAttribute;
    v.domain = std::int32_t(_runOver);
    v.type = request.type;
    v.readOnly = request.input != 0;
    result.variable = v;
    return result;
}

void
GeoKernelBinding::functions(std::string_view name, std::vector<KernelFunctionSignature>& overloads) const
{
    for (const FnEntry& e : functionTable()) {
        if (e.signature.name == name) {
            overloads.push_back(e.signature);
        }
    }
}

std::string
GeoKernelBinding::checkLiteralArgument(const KernelFunctionSignature& function,
                                       std::size_t argument,
                                       const KernelLiteral& value) const
{
    const std::vector<FnEntry>& table = functionTable();
    if (argument != 0 || function.id < 0 || std::size_t(function.id) >= table.size()) {
        return std::string();
    }
    const Fn fn = table[std::size_t(function.id)].fn;
    if (!isParameterReader(fn)) {
        return std::string();
    }
    return parameterError(_parameters, fn, function.name.c_str(), value.text);
}

GeoKernelRunResult
geoKernelRun(const KernelProgram& program, GeoAttribClassEnum runOver, std::span<const GeoDetail* const> inputs,
             const GeoKernelRunOptions& options)
{
    GeoKernelRunResult result;
    if (inputs.empty() || !inputs[0]) {
        result.status = KernelRunStatus::eFailed;
        result.error = "geometry kernel: input 0 is missing";
        return result;
    }
    result.detail = *inputs[0];
    try {
        GeoKernelRun run(program, runOver, inputs, options);
        KernelRunOptions runOptions;
        runOptions.threads = options.threads;
        runOptions.aborted = options.aborted;
        const KernelRunResult r = kernelRun(program, run, runOptions);
        result.status = r.status;
        result.error = r.error;
        result.batches = r.batches;
        if (r.status == KernelRunStatus::eDone) {
            result.detail = run.finish();
        }
    } catch (const std::exception& e) {
        result.status = KernelRunStatus::eFailed;
        result.error = e.what();
        result.detail = *inputs[0];
    }
    return result;
}

bool
geoKernelReadsTime(const KernelProgram& program)
{
    for (const KernelProgramBinding& b : program.bindings) {
        if (b.variable.kind == KernelVariableKind::eBuiltin
            && (Builtin(b.variable.id) == Builtin::eFrame || Builtin(b.variable.id) == Builtin::eTime)) {
            return true;
        }
    }
    return false;
}

GeoKernelRunResult
geoKernelRun(std::string_view source, GeoAttribClassEnum runOver, std::span<const GeoDetail* const> inputs,
             const GeoKernelRunOptions& options)
{
    const GeoKernelBinding binding(runOver, options.parameters);
    KernelProgram program;
    KernelCompileError error;
    if (!kernelCompile(source, binding, program, error)) {
        GeoKernelRunResult result;
        result.status = KernelRunStatus::eFailed;
        result.error = std::to_string(error.line) + ":" + std::to_string(error.column) + ": " + error.message;
        if (!inputs.empty() && inputs[0]) {
            result.detail = *inputs[0];
        }
        return result;
    }
    return geoKernelRun(program, runOver, inputs, options);
}

NATRON_NAMESPACE_EXIT
