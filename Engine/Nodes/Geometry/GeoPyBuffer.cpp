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
#undef Py_LIMITED_API // Needed for the buffer protocol (Py_buffer, bf_getbuffer)
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "GeoPyBuffer.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Engine/Geo/GeoOpUtils.h"

NATRON_NAMESPACE_ENTER

namespace {

enum ArrayKindEnum {
    eArrayKindInt32 = 0,
    eArrayKindInt64,
    eArrayKindFloat32,
    eArrayKindFloat64,
    eArrayKindBool,
};

std::size_t
kindBytes(ArrayKindEnum kind)
{
    switch (kind) {
    case eArrayKindInt32:
    case eArrayKindFloat32:
        return 4;
    case eArrayKindInt64:
    case eArrayKindFloat64:
        return 8;
    case eArrayKindBool:
        return 1;
    }

    return 1;
}

const char*
kindFormat(ArrayKindEnum kind)
{
    switch (kind) {
    case eArrayKindInt32:
        return "i";
    case eArrayKindInt64:
        return "q";
    case eArrayKindFloat32:
        return "f";
    case eArrayKindFloat64:
        return "d";
    case eArrayKindBool:
        return "?";
    }

    return "B";
}

const char*
kindName(ArrayKindEnum kind)
{
    switch (kind) {
    case eArrayKindInt32:
        return "int32";
    case eArrayKindInt64:
        return "int64";
    case eArrayKindFloat32:
        return "float32";
    case eArrayKindFloat64:
        return "float64";
    case eArrayKindBool:
        return "bool";
    }

    return "";
}

bool
kindIsFloat(ArrayKindEnum kind)
{
    return kind == eArrayKindFloat32 || kind == eArrayKindFloat64;
}

ArrayKindEnum
kindOfStorage(GeoStorageEnum storage)
{
    switch (storage) {
    case eGeoStorageInt64:
        return eArrayKindInt64;
    case eGeoStorageFloat32:
        return eArrayKindFloat32;
    case eGeoStorageFloat64:
        return eArrayKindFloat64;
    case eGeoStorageInt32:
    case eGeoStorageString:
        return eArrayKindInt32;
    }

    return eArrayKindInt32;
}

GeoStorageEnum
storageOfKind(ArrayKindEnum kind)
{
    switch (kind) {
    case eArrayKindInt64:
        return eGeoStorageInt64;
    case eArrayKindFloat32:
        return eGeoStorageFloat32;
    case eArrayKindFloat64:
        return eGeoStorageFloat64;
    case eArrayKindInt32:
    case eArrayKindBool:
        return eGeoStorageInt32;
    }

    return eGeoStorageInt32;
}

const char*
storageName(GeoStorageEnum storage)
{
    switch (storage) {
    case eGeoStorageInt32:
        return "int32";
    case eGeoStorageInt64:
        return "int64";
    case eGeoStorageFloat32:
        return "float32";
    case eGeoStorageFloat64:
        return "float64";
    case eGeoStorageString:
        return "string";
    }

    return "";
}

const char*
typeInfoName(GeoTypeInfoEnum typeInfo)
{
    switch (typeInfo) {
    case eGeoTypeInfoNone:
        return "none";
    case eGeoTypeInfoVector:
        return "vector";
    case eGeoTypeInfoNormal:
        return "normal";
    case eGeoTypeInfoPoint:
        return "point";
    case eGeoTypeInfoColour:
        return "colour";
    case eGeoTypeInfoMatrix:
        return "matrix";
    case eGeoTypeInfoQuaternion:
        return "quaternion";
    }

    return "none";
}

const char*
className(int cls)
{
    switch (cls) {
    case eGeoAttribClassPoint:
        return "points";
    case eGeoAttribClassVertex:
        return "vertices";
    case eGeoAttribClassPrimitive:
        return "prims";
    default:
        return "detail";
    }
}

/** @brief The type info a new attribute of a well-known name takes. */
GeoTypeInfoEnum
typeInfoForName(std::string_view name, int tupleSize)
{
    if (name == "N" && tupleSize == 3) {
        return eGeoTypeInfoNormal;
    }
    if (name == "Cd" && (tupleSize == 3 || tupleSize == 4)) {
        return eGeoTypeInfoColour;
    }
    if ((name == "v" || name == "up" || name == "accel" || name == "scale") && tupleSize == 3) {
        return eGeoTypeInfoVector;
    }
    if ((name == "rest" || name == "pivot" || name == "trans") && tupleSize == 3) {
        return eGeoTypeInfoPoint;
    }
    if (name == "orient" && tupleSize == 4) {
        return eGeoTypeInfoQuaternion;
    }
    if (name == "transform" && (tupleSize == 9 || tupleSize == 16)) {
        return eGeoTypeInfoMatrix;
    }

    return eGeoTypeInfoNone;
}

/** @brief A dense copy of one attribute or group, owned by the Python object exposing it. */
struct ArrayData {
    ArrayKindEnum kind = eArrayKindFloat32;
    Py_ssize_t count = 0;
    int tupleSize = 1;
    bool readonly = false;
    // Set by any write and by any buffer export of a writable array, since a consumer such as
    // memoryview may write through a buffer it did not ask to be writable.
    bool dirty = false;
    std::vector<unsigned char> bytes;
    Py_ssize_t shape[2] = { 0, 0 };
    Py_ssize_t strides[2] = { 0, 0 };
    int ndim = 1;

    void allocate(ArrayKindEnum k, Py_ssize_t n, int t)
    {
        kind = k;
        count = n;
        tupleSize = t;
        bytes.assign(std::size_t(n) * std::size_t(t) * kindBytes(k), 0);
        const Py_ssize_t item = Py_ssize_t(kindBytes(k));
        if (t == 1) {
            ndim = 1;
            shape[0] = n;
            strides[0] = item;
        } else {
            ndim = 2;
            shape[0] = n;
            shape[1] = t;
            strides[0] = item * t;
            strides[1] = item;
        }
    }

    Py_ssize_t componentCount() const
    {
        return count * tupleSize;
    }

    template <typename T>
    T* as()
    {
        return reinterpret_cast<T*>(bytes.data());
    }

    template <typename T>
    const T* as() const
    {
        return reinterpret_cast<const T*>(bytes.data());
    }

    double getDouble(Py_ssize_t i) const
    {
        switch (kind) {
        case eArrayKindInt32:
            return as<std::int32_t>()[i];
        case eArrayKindInt64:
            return double(as<std::int64_t>()[i]);
        case eArrayKindFloat32:
            return as<float>()[i];
        case eArrayKindFloat64:
            return as<double>()[i];
        case eArrayKindBool:
            return as<unsigned char>()[i] ? 1. : 0.;
        }

        return 0.;
    }

    long long getInt(Py_ssize_t i) const
    {
        switch (kind) {
        case eArrayKindInt32:
            return as<std::int32_t>()[i];
        case eArrayKindInt64:
            return as<std::int64_t>()[i];
        case eArrayKindFloat32:
            return (long long)as<float>()[i];
        case eArrayKindFloat64:
            return (long long)as<double>()[i];
        case eArrayKindBool:
            return as<unsigned char>()[i] ? 1 : 0;
        }

        return 0;
    }

    void setDouble(Py_ssize_t i, double v)
    {
        switch (kind) {
        case eArrayKindFloat32:
            as<float>()[i] = float(v);
            break;
        case eArrayKindFloat64:
            as<double>()[i] = v;
            break;
        case eArrayKindBool:
            as<unsigned char>()[i] = v != 0. ? 1 : 0;
            break;
        case eArrayKindInt32:
            as<std::int32_t>()[i] = std::int32_t(v);
            break;
        case eArrayKindInt64:
            as<std::int64_t>()[i] = std::int64_t(v);
            break;
        }
    }

    void setInt(Py_ssize_t i, long long v)
    {
        switch (kind) {
        case eArrayKindInt32:
            as<std::int32_t>()[i] = std::int32_t(v);
            break;
        case eArrayKindInt64:
            as<std::int64_t>()[i] = std::int64_t(v);
            break;
        case eArrayKindFloat32:
            as<float>()[i] = float(v);
            break;
        case eArrayKindFloat64:
            as<double>()[i] = double(v);
            break;
        case eArrayKindBool:
            as<unsigned char>()[i] = v != 0 ? 1 : 0;
            break;
        }
    }
};

struct ArrayObject {
    PyObject_HEAD ArrayData* data;
};

struct ElementsObject {
    PyObject_HEAD
        GeoPySession::Impl* session;
    int cls;
};

struct GeometryObject {
    PyObject_HEAD
        GeoPySession::Impl* session;
};

struct PyTypes {
    PyObject* array = nullptr;
    PyObject* elements = nullptr;
    PyObject* geometry = nullptr;
};

PyTypes&
pyTypes()
{
    static PyTypes types;

    return types;
}

bool
isArrayObject(PyObject* o)
{
    return pyTypes().array && Py_TYPE(o) == reinterpret_cast<PyTypeObject*>(pyTypes().array);
}

ArrayData&
arrayData(PyObject* o)
{
    return *reinterpret_cast<ArrayObject*>(o)->data;
}

PyObject*
newArray(std::unique_ptr<ArrayData> data)
{
    PyObject* o = PyType_GenericAlloc(reinterpret_cast<PyTypeObject*>(pyTypes().array), 0);
    if (!o) {
        return nullptr;
    }
    reinterpret_cast<ArrayObject*>(o)->data = data.release();

    return o;
}

template <typename R, typename F>
R
guarded(R failure,
        F&& f)
{
    try {
        return f();
    } catch (const std::bad_alloc&) {
        PyErr_NoMemory();
    } catch (const std::invalid_argument& e) {
        PyErr_SetString(PyExc_ValueError, e.what());
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
    } catch (...) {
        PyErr_SetString(PyExc_RuntimeError, "unexpected error in the geometry module");
    }

    return failure;
}

bool
utf8Of(PyObject* s,
       std::string_view* out)
{
    Py_ssize_t size = 0;
    const char* data = PyUnicode_AsUTF8AndSize(s, &size);
    if (!data) {
        return false;
    }
    *out = std::string_view(data, std::size_t(size));

    return true;
}

PyObject*
stringToPy(std::string_view s)
{
    return PyUnicode_DecodeUTF8(s.data(), Py_ssize_t(s.size()), "replace");
}

bool
isNumberScalar(PyObject* o)
{
    return PyFloat_Check(o) || PyLong_Check(o) || (PyNumber_Check(o) && !PySequence_Check(o) && !PyUnicode_Check(o));
}

bool
isNonStringSequence(PyObject* o)
{
    return PySequence_Check(o) && !PyUnicode_Check(o) && !PyBytes_Check(o) && !PyByteArray_Check(o);
}

PyObject*
componentToPy(const ArrayData& d,
              Py_ssize_t i)
{
    switch (d.kind) {
    case eArrayKindBool:
        return PyBool_FromLong(long(d.getInt(i)));
    case eArrayKindInt32:
    case eArrayKindInt64:
        return PyLong_FromLongLong(d.getInt(i));
    case eArrayKindFloat32:
    case eArrayKindFloat64:
        return PyFloat_FromDouble(d.getDouble(i));
    }

    Py_RETURN_NONE;
}

bool
componentFromPy(ArrayData& d,
                Py_ssize_t i,
                PyObject* o)
{
    if (d.kind == eArrayKindBool) {
        const int truth = PyObject_IsTrue(o);
        if (truth < 0) {
            return false;
        }
        d.setInt(i, truth);

        return true;
    }
    if (kindIsFloat(d.kind)) {
        const double v = PyFloat_AsDouble(o);
        if (v == -1. && PyErr_Occurred()) {
            return false;
        }
        d.setDouble(i, v);

        return true;
    }
    if (PyFloat_Check(o)) {
        PyErr_Format(PyExc_TypeError, "an %s attribute takes integers, not floats", kindName(d.kind));

        return false;
    }
    const long long v = PyLong_AsLongLong(o);
    if (v == -1 && PyErr_Occurred()) {
        return false;
    }
    if (d.kind == eArrayKindInt32 && (v < std::numeric_limits<std::int32_t>::min() || v > std::numeric_limits<std::int32_t>::max())) {
        PyErr_Format(PyExc_OverflowError, "%lld does not fit an int32 attribute", v);

        return false;
    }
    d.setInt(i, v);

    return true;
}

/** @brief Element i as a scalar for a tuple size of 1, a tuple otherwise. */
PyObject*
elementToPy(const ArrayData& d,
            Py_ssize_t i)
{
    if (d.tupleSize == 1) {
        return componentToPy(d, i);
    }
    PyObject* tuple = PyTuple_New(d.tupleSize);
    if (!tuple) {
        return nullptr;
    }
    for (int c = 0; c < d.tupleSize; ++c) {
        PyObject* v = componentToPy(d, i * d.tupleSize + c);
        if (!v) {
            Py_DECREF(tuple);

            return nullptr;
        }
        PyTuple_SET_ITEM(tuple, c, v);
    }

    return tuple;
}

bool
elementFromPy(ArrayData& d,
              Py_ssize_t i,
              PyObject* value)
{
    if (d.tupleSize == 1) {
        return componentFromPy(d, i, value);
    }
    if (!isNonStringSequence(value)) {
        PyErr_Format(PyExc_TypeError, "expected a sequence of %d values", d.tupleSize);

        return false;
    }
    PyObject* fast = PySequence_Fast(value, "expected a sequence");
    if (!fast) {
        return false;
    }
    bool ok = true;
    if (PySequence_Fast_GET_SIZE(fast) != d.tupleSize) {
        PyErr_Format(PyExc_ValueError, "expected %d values, got %zd", d.tupleSize, PySequence_Fast_GET_SIZE(fast));
        ok = false;
    }
    for (int c = 0; ok && c < d.tupleSize; ++c) {
        ok = componentFromPy(d, i * d.tupleSize + c, PySequence_Fast_GET_ITEM(fast, c));
    }
    Py_DECREF(fast);

    return ok;
}

/** @brief A buffer format this module reads: one native-order code and its storage. */
struct BufferFormat {
    char code = 0;
    std::size_t size = 0;
    bool isFloat = false;
    bool isBool = false;
};

bool
parseBufferFormat(const char* format,
                  std::size_t itemsize,
                  BufferFormat* out)
{
    std::string_view f = format ? std::string_view(format) : std::string_view("B");
    if (!f.empty() && (f[0] == '@' || f[0] == '=' || f[0] == '<')) {
        f.remove_prefix(1);
    }
    if (f.size() != 1) {
        return false;
    }
    out->code = f[0];
    out->size = itemsize;
    switch (out->code) {
    case 'f':
    case 'd':
        out->isFloat = true;
        break;
    case '?':
        out->isBool = true;
        break;
    case 'b':
    case 'B':
    case 'h':
    case 'H':
    case 'i':
    case 'I':
    case 'l':
    case 'L':
    case 'q':
    case 'Q':
    case 'n':
    case 'N':
        break;
    default:
        return false;
    }

    return true;
}

bool
bufferFormatIsSigned(char code)
{
    return code == 'b' || code == 'h' || code == 'i' || code == 'l' || code == 'q' || code == 'n';
}

double
bufferDouble(const BufferFormat& fmt,
             const unsigned char* p)
{
    if (fmt.code == 'f') {
        float v;
        std::memcpy(&v, p, sizeof(v));

        return v;
    }
    if (fmt.code == 'd') {
        double v;
        std::memcpy(&v, p, sizeof(v));

        return v;
    }

    return 0.;
}

long long
bufferInt(const BufferFormat& fmt,
          const unsigned char* p)
{
    if (fmt.isBool) {
        return *p ? 1 : 0;
    }
    const bool isSigned = bufferFormatIsSigned(fmt.code);
    switch (fmt.size) {
    case 1:
        return isSigned ? (long long)*reinterpret_cast<const std::int8_t*>(p) : (long long)*p;
    case 2: {
        std::uint16_t v;
        std::memcpy(&v, p, 2);

        return isSigned ? (long long)std::int16_t(v) : (long long)v;
    }
    case 4: {
        std::uint32_t v;
        std::memcpy(&v, p, 4);

        return isSigned ? (long long)std::int32_t(v) : (long long)v;
    }
    case 8: {
        std::int64_t v;
        std::memcpy(&v, p, 8);

        return (long long)v;
    }
    default:
        return 0;
    }
}

enum FillResultEnum {
    eFillOK = 0,
    eFillError,
    eFillNotHandled,
};

/** @brief The values of a C-contiguous buffer; eFillNotHandled when @p value exports none this reads. */
FillResultEnum
fillFromBuffer(ArrayData& dst,
               PyObject* value)
{
    if (!PyObject_CheckBuffer(value) || PyBytes_Check(value) || PyByteArray_Check(value)) {
        return eFillNotHandled;
    }
    Py_buffer view;
    if (PyObject_GetBuffer(value, &view, PyBUF_FORMAT | PyBUF_C_CONTIGUOUS) != 0) {
        PyErr_Clear();

        return eFillNotHandled;
    }
    FillResultEnum result = eFillOK;
    BufferFormat fmt;
    if (!parseBufferFormat(view.format, std::size_t(view.itemsize), &fmt)) {
        PyBuffer_Release(&view);

        return eFillNotHandled;
    }
    const Py_ssize_t total = view.itemsize > 0 ? view.len / view.itemsize : 0;
    const bool shapeOK = total == dst.componentCount() && (view.ndim <= 1 || (view.ndim == 2 && view.shape[1] == dst.tupleSize));
    if (!shapeOK) {
        PyErr_Format(PyExc_ValueError, "expected %zd values of size %d, got an array of %zd values", dst.count, dst.tupleSize, view.ndim >= 1 ? view.shape[0] : Py_ssize_t(1));
        result = eFillError;
    } else if (fmt.isFloat && !kindIsFloat(dst.kind) && dst.kind != eArrayKindBool) {
        PyErr_Format(PyExc_TypeError, "an %s attribute takes integers, not floats", kindName(dst.kind));
        result = eFillError;
    } else {
        const unsigned char* p = static_cast<const unsigned char*>(view.buf);
        for (Py_ssize_t i = 0; i < total; ++i, p += view.itemsize) {
            if (fmt.isFloat) {
                dst.setDouble(i, bufferDouble(fmt, p));
            } else {
                dst.setInt(i, bufferInt(fmt, p));
            }
        }
    }
    PyBuffer_Release(&view);

    return result;
}

/**
 * @brief Fills every element of @p dst from @p value: another array, a buffer, a scalar
 * (every component of every element), a sequence of count elements, or a flat sequence of
 * count * tupleSize scalars. Raises and returns false on a wrong length or type.
 **/
bool
fillArray(ArrayData& dst,
          PyObject* value)
{
    if (isArrayObject(value)) {
        const ArrayData& src = arrayData(value);
        if (src.componentCount() != dst.componentCount() || (src.count != dst.count && src.tupleSize != 1)) {
            PyErr_Format(PyExc_ValueError, "expected %zd values of size %d, got %zd of size %d", dst.count, dst.tupleSize, src.count, src.tupleSize);

            return false;
        }
        if (kindIsFloat(src.kind) && !kindIsFloat(dst.kind) && dst.kind != eArrayKindBool) {
            PyErr_Format(PyExc_TypeError, "an %s attribute takes integers, not floats", kindName(dst.kind));

            return false;
        }
        for (Py_ssize_t i = 0; i < dst.componentCount(); ++i) {
            if (kindIsFloat(src.kind)) {
                dst.setDouble(i, src.getDouble(i));
            } else {
                dst.setInt(i, src.getInt(i));
            }
        }

        return true;
    }
    const FillResultEnum fromBuffer = fillFromBuffer(dst, value);
    if (fromBuffer != eFillNotHandled) {
        return fromBuffer == eFillOK;
    }
    if (isNumberScalar(value)) {
        if (dst.componentCount() == 0) {
            return true;
        }
        if (!componentFromPy(dst, 0, value)) {
            return false;
        }
        const std::size_t item = kindBytes(dst.kind);
        for (Py_ssize_t i = 1; i < dst.componentCount(); ++i) {
            std::memcpy(dst.bytes.data() + std::size_t(i) * item, dst.bytes.data(), item);
        }

        return true;
    }
    if (!isNonStringSequence(value)) {
        PyErr_Format(PyExc_TypeError, "cannot write a %s to a numeric attribute", Py_TYPE(value)->tp_name);

        return false;
    }
    PyObject* fast = PySequence_Fast(value, "expected a sequence");
    if (!fast) {
        return false;
    }
    const Py_ssize_t length = PySequence_Fast_GET_SIZE(fast);
    bool ok = true;
    const bool flat = dst.tupleSize > 1 && length == dst.componentCount() && length > 0 && !isNonStringSequence(PySequence_Fast_GET_ITEM(fast, 0));
    if (flat) {
        for (Py_ssize_t i = 0; ok && i < length; ++i) {
            ok = componentFromPy(dst, i, PySequence_Fast_GET_ITEM(fast, i));
        }
    } else if (length != dst.count) {
        PyErr_Format(PyExc_ValueError, "expected %zd values, got %zd", dst.count, length);
        ok = false;
    } else {
        for (Py_ssize_t i = 0; ok && i < length; ++i) {
            ok = elementFromPy(dst, i, PySequence_Fast_GET_ITEM(fast, i));
        }
    }
    Py_DECREF(fast);

    return ok;
}

/**
 * @brief A new list of @p count strings (tuple size 1) or tuples of @p tupleSize strings, from
 * one string (every element), a sequence of count elements, or for a single element a flat
 * sequence of tupleSize strings. Raises and returns nullptr otherwise.
 **/
PyObject*
stringListFrom(PyObject* value,
               Py_ssize_t count,
               int tupleSize)
{
    auto element = [tupleSize](PyObject* item) -> PyObject* {
        if (tupleSize == 1) {
            if (!PyUnicode_Check(item)) {
                PyErr_Format(PyExc_TypeError, "a string attribute takes str values, not %s", Py_TYPE(item)->tp_name);

                return nullptr;
            }
            Py_INCREF(item);

            return item;
        }
        if (!isNonStringSequence(item)) {
            PyErr_Format(PyExc_TypeError, "expected a sequence of %d strings", tupleSize);

            return nullptr;
        }
        PyObject* tuple = PySequence_Tuple(item);
        if (!tuple) {
            return nullptr;
        }
        if (PyTuple_GET_SIZE(tuple) != tupleSize) {
            PyErr_Format(PyExc_ValueError, "expected %d strings, got %zd", tupleSize, PyTuple_GET_SIZE(tuple));
            Py_DECREF(tuple);

            return nullptr;
        }
        for (int c = 0; c < tupleSize; ++c) {
            if (!PyUnicode_Check(PyTuple_GET_ITEM(tuple, c))) {
                PyErr_SetString(PyExc_TypeError, "a string attribute takes str values");
                Py_DECREF(tuple);

                return nullptr;
            }
        }

        return tuple;
    };

    PyObject* list = PyList_New(count);
    if (!list) {
        return nullptr;
    }
    if (PyUnicode_Check(value)) {
        for (Py_ssize_t i = 0; i < count; ++i) {
            PyObject* item = nullptr;
            if (tupleSize == 1) {
                Py_INCREF(value);
                item = value;
            } else {
                item = PyTuple_New(tupleSize);
                for (int c = 0; item && c < tupleSize; ++c) {
                    Py_INCREF(value);
                    PyTuple_SET_ITEM(item, c, value);
                }
            }
            if (!item) {
                Py_DECREF(list);

                return nullptr;
            }
            PyList_SET_ITEM(list, i, item);
        }

        return list;
    }
    if (!isNonStringSequence(value)) {
        PyErr_Format(PyExc_TypeError, "a string attribute takes str values, not %s", Py_TYPE(value)->tp_name);
        Py_DECREF(list);

        return nullptr;
    }
    PyObject* fast = PySequence_Fast(value, "expected a sequence");
    if (!fast) {
        Py_DECREF(list);

        return nullptr;
    }
    const Py_ssize_t length = PySequence_Fast_GET_SIZE(fast);
    if (count == 1 && tupleSize > 1 && length == tupleSize && PyUnicode_Check(PySequence_Fast_GET_ITEM(fast, 0))) {
        PyObject* item = element(fast);
        Py_DECREF(fast);
        if (!item) {
            Py_DECREF(list);

            return nullptr;
        }
        PyList_SET_ITEM(list, 0, item);

        return list;
    }
    if (length != count) {
        PyErr_Format(PyExc_ValueError, "expected %zd values, got %zd", count, length);
        Py_DECREF(fast);
        Py_DECREF(list);

        return nullptr;
    }
    for (Py_ssize_t i = 0; i < count; ++i) {
        PyObject* item = element(PySequence_Fast_GET_ITEM(fast, i));
        if (!item) {
            Py_DECREF(fast);
            Py_DECREF(list);

            return nullptr;
        }
        PyList_SET_ITEM(list, i, item);
    }
    Py_DECREF(fast);

    return list;
}

/** @brief What a value assigned to an absent attribute creates. */
struct NewAttribute {
    bool isString = false;
    ArrayKindEnum kind = eArrayKindFloat32;
    int tupleSize = 1;
};

/** @brief Raises and returns false when nothing sensible can be created from @p value. */
bool
inferNewAttribute(PyObject* value,
                  Py_ssize_t count,
                  NewAttribute* out)
{
    auto tupleSizeOf = [count](Py_ssize_t elements, Py_ssize_t perElement, Py_ssize_t total, int* tupleSize) {
        Py_ssize_t t = -1;
        if (elements == count) {
            t = perElement;
        } else if (count == 1) {
            t = total;
        }
        if (t < 1 || t > GeoAttribute::kMaxTupleSize) {
            if (t > GeoAttribute::kMaxTupleSize) {
                PyErr_Format(PyExc_ValueError, "a tuple size of %zd is more than %d", t, GeoAttribute::kMaxTupleSize);
            } else {
                PyErr_Format(PyExc_ValueError, "expected %zd values, got %zd", count, elements);
            }

            return false;
        }
        *tupleSize = int(t);

        return true;
    };

    if (PyUnicode_Check(value)) {
        out->isString = true;

        return true;
    }
    if (PyBool_Check(value) || PyLong_Check(value)) {
        out->kind = eArrayKindInt32;

        return true;
    }
    if (PyFloat_Check(value)) {
        out->kind = eArrayKindFloat32;

        return true;
    }
    if (isArrayObject(value)) {
        const ArrayData& src = arrayData(value);
        out->kind = src.kind == eArrayKindBool ? eArrayKindInt32 : src.kind;

        return tupleSizeOf(src.count, src.tupleSize, src.componentCount(), &out->tupleSize);
    }
    if (PyObject_CheckBuffer(value) && !PyBytes_Check(value) && !PyByteArray_Check(value)) {
        Py_buffer view;
        if (PyObject_GetBuffer(value, &view, PyBUF_RECORDS_RO) == 0) {
            BufferFormat fmt;
            const bool known = parseBufferFormat(view.format, std::size_t(view.itemsize), &fmt) && view.ndim <= 2;
            Py_ssize_t elements = 1;
            Py_ssize_t perElement = 1;
            if (view.ndim >= 1) {
                elements = view.shape[0];
            }
            if (view.ndim == 2) {
                perElement = view.shape[1];
            }
            const Py_ssize_t total = view.itemsize > 0 ? view.len / view.itemsize : 0;
            if (fmt.isFloat) {
                out->kind = fmt.size == 8 ? eArrayKindFloat64 : eArrayKindFloat32;
            } else if (fmt.isBool || (fmt.size <= 4 && !(fmt.size == 4 && !bufferFormatIsSigned(fmt.code)))) {
                out->kind = eArrayKindInt32;
            } else {
                out->kind = eArrayKindInt64;
            }
            PyBuffer_Release(&view);
            if (known) {
                if (view.ndim == 0) {
                    out->tupleSize = 1;

                    return true;
                }

                return tupleSizeOf(elements, perElement, total, &out->tupleSize);
            }
        } else {
            PyErr_Clear();
        }
    }
    if (!isNonStringSequence(value)) {
        PyErr_Format(PyExc_TypeError, "cannot make an attribute from a %s", Py_TYPE(value)->tp_name);

        return false;
    }
    PyObject* fast = PySequence_Fast(value, "expected a sequence");
    if (!fast) {
        return false;
    }
    const Py_ssize_t length = PySequence_Fast_GET_SIZE(fast);
    bool ok = true;
    if (length == 0) {
        out->kind = eArrayKindFloat32;
        ok = tupleSizeOf(0, 1, 0, &out->tupleSize);
    } else {
        PyObject* first = PySequence_Fast_GET_ITEM(fast, 0);
        bool allInts = true;
        if (PyUnicode_Check(first)) {
            out->isString = true;
            ok = tupleSizeOf(length, 1, length, &out->tupleSize);
        } else if (isNonStringSequence(first)) {
            const Py_ssize_t perElement = PySequence_Size(first);
            if (perElement < 0) {
                ok = false;
            } else {
                PyObject* firstFast = PySequence_Fast(first, "expected a sequence");
                if (!firstFast) {
                    ok = false;
                } else {
                    out->isString = perElement > 0 && PyUnicode_Check(PySequence_Fast_GET_ITEM(firstFast, 0));
                    Py_DECREF(firstFast);
                }
                for (Py_ssize_t i = 0; ok && !out->isString && allInts && i < length; ++i) {
                    PyObject* inner = PySequence_Fast(PySequence_Fast_GET_ITEM(fast, i), "expected a sequence of sequences");
                    if (!inner) {
                        ok = false;
                        break;
                    }
                    for (Py_ssize_t c = 0; c < PySequence_Fast_GET_SIZE(inner); ++c) {
                        if (!PyLong_Check(PySequence_Fast_GET_ITEM(inner, c))) {
                            allInts = false;
                            break;
                        }
                    }
                    Py_DECREF(inner);
                }
                if (ok) {
                    ok = tupleSizeOf(length, perElement, length * perElement, &out->tupleSize);
                }
            }
        } else {
            for (Py_ssize_t i = 0; allInts && i < length; ++i) {
                allInts = PyLong_Check(PySequence_Fast_GET_ITEM(fast, i));
            }
            ok = tupleSizeOf(length, 1, length, &out->tupleSize);
        }
        out->kind = allInts ? eArrayKindInt32 : eArrayKindFloat32;
    }
    Py_DECREF(fast);

    return ok;
}

/** @brief Element numbers from an int array, a buffer or a sequence of integers. */
bool
indicesFrom(PyObject* value,
            std::vector<GeoIndex>* out)
{
    out->clear();
    if (isArrayObject(value)) {
        const ArrayData& src = arrayData(value);
        if (kindIsFloat(src.kind)) {
            PyErr_SetString(PyExc_TypeError, "element numbers must be integers");

            return false;
        }
        out->reserve(std::size_t(src.componentCount()));
        for (Py_ssize_t i = 0; i < src.componentCount(); ++i) {
            out->push_back(GeoIndex(src.getInt(i)));
        }

        return true;
    }
    if (PyLong_Check(value)) {
        const long long v = PyLong_AsLongLong(value);
        if (v == -1 && PyErr_Occurred()) {
            return false;
        }
        out->push_back(GeoIndex(v));

        return true;
    }
    if (!isNonStringSequence(value)) {
        PyErr_Format(PyExc_TypeError, "expected element numbers, not %s", Py_TYPE(value)->tp_name);

        return false;
    }
    PyObject* fast = PySequence_Fast(value, "expected a sequence");
    if (!fast) {
        return false;
    }
    const Py_ssize_t length = PySequence_Fast_GET_SIZE(fast);
    out->reserve(std::size_t(length));
    bool ok = true;
    for (Py_ssize_t i = 0; ok && i < length; ++i) {
        PyObject* item = PySequence_Fast_GET_ITEM(fast, i);
        if (PyFloat_Check(item)) {
            PyErr_SetString(PyExc_TypeError, "element numbers must be integers");
            ok = false;
            break;
        }
        const long long v = PyLong_AsLongLong(item);
        if (v == -1 && PyErr_Occurred()) {
            ok = false;
            break;
        }
        out->push_back(GeoIndex(v));
    }
    Py_DECREF(fast);

    return ok;
}

bool
primitiveTypeFromName(const char* name,
                      GeoPrimitiveTypeEnum* type)
{
    const std::string_view n = name ? std::string_view(name) : std::string_view("polygon");
    if (n == "polygon") {
        *type = eGeoPrimitivePolygon;

        return true;
    }
    if (n == "polyline") {
        *type = eGeoPrimitivePolyline;

        return true;
    }
    PyErr_Format(PyExc_ValueError, "unknown primitive type \"%s\": use \"polygon\" or \"polyline\"", name);

    return false;
}

const char*
primitiveTypeName(GeoPrimitiveTypeEnum type)
{
    switch (type) {
    case eGeoPrimitivePolygon:
        return "polygon";
    case eGeoPrimitivePolyline:
        return "polyline";
    case eGeoPrimitivePackedGeometry:
        return "packed_geometry";
    case eGeoPrimitivePackedUSD:
        return "packed_usd";
    }

    return "unknown";
}

PyObject*
newRange(GeoIndex begin,
         GeoIndex end)
{
    return PyObject_CallFunction(reinterpret_cast<PyObject*>(&PyRange_Type), "LL", (long long)begin, (long long)end);
}

} // anonymous namespace

struct GeoPySession::Impl {
    enum ValueKindEnum {
        eValueAttribute = 0,
        eValueGroup,
    };

    struct Key {
        int what;
        int cls;
        std::string name;

        bool operator<(const Key& o) const
        {
            if (what != o.what) {
                return what < o.what;
            }
            if (cls != o.cls) {
                return cls < o.cls;
            }

            return name < o.name;
        }
    };

    struct PrimBatch {
        GeoPrimitiveTypeEnum type = eGeoPrimitivePolygon;
        std::vector<std::int32_t> counts;
        std::vector<GeoIndex> points;
    };

    struct PrimRemoval {
        std::vector<GeoIndex> numbers;
        bool deleteUnusedPoints = false;
    };

    GeoDetailBuilder* builder = nullptr;
    PyObject* geometry = nullptr;
    PyObject* elements[kGeoAttribClassCount] = {};
    GeoIndex counts[kGeoAttribClassCount] = {};
    std::vector<std::size_t> offsets[3];
    bool offsetsReady[3] = { false, false, false };
    // Owned references: an array for numeric attributes and groups, a list for strings.
    std::map<Key, PyObject*> values;

    std::vector<double> newPositions;
    std::vector<PrimBatch> newPrims;
    GeoIndex newPrimCount = 0;
    std::vector<GeoIndex> removedPoints;
    std::vector<PrimRemoval> removedPrims;

    const GeoDetail& detail() const
    {
        return builder->detail();
    }

    GeoIndex newPointCount() const
    {
        return GeoIndex(newPositions.size() / 3);
    }

    const std::vector<std::size_t>& liveOffsets(int cls)
    {
        if (!offsetsReady[cls]) {
            offsets[cls] = geoLiveOffsets(detail().indexMap(GeoAttribClassEnum(cls)));
            offsetsReady[cls] = true;
        }

        return offsets[cls];
    }

    bool isDense(int cls) const
    {
        return cls == eGeoAttribClassDetail || detail().indexMap(GeoAttribClassEnum(cls)).tombstoneCount() == 0;
    }

    template <typename T>
    void readDense(const GeoAttribute& a,
                   int cls,
                   ArrayData& d)
    {
        const std::span<T> out(d.as<T>(), std::size_t(d.componentCount()));
        if (isDense(cls)) {
            a.copyDense<T>(0, std::size_t(counts[cls]), out);
        } else {
            a.copyDense<T>(std::span<const std::size_t>(liveOffsets(cls)), out);
        }
    }

    template <typename T>
    void writeDense(GeoAttribute& a,
                    int cls,
                    std::span<const T> in)
    {
        if (isDense(cls)) {
            a.scatterDense<T>(0, std::size_t(counts[cls]), in);
        } else {
            a.scatterDense<T>(std::span<const std::size_t>(liveOffsets(cls)), in);
        }
    }

    std::size_t offsetOf(int cls,
                         Py_ssize_t i)
    {
        if (cls == eGeoAttribClassDetail) {
            return 0;
        }
        if (isDense(cls)) {
            return std::size_t(i);
        }

        return liveOffsets(cls)[std::size_t(i)];
    }

    void storeValue(const Key& key,
                    PyObject* value)
    {
        std::map<Key, PyObject*>::iterator found = values.find(key);
        if (found != values.end()) {
            PyObject* old = found->second;
            found->second = value;
            Py_DECREF(old);
        } else {
            values.emplace(key, value);
        }
    }

    void dropValue(const Key& key)
    {
        std::map<Key, PyObject*>::iterator found = values.find(key);
        if (found != values.end()) {
            PyObject* old = found->second;
            values.erase(found);
            Py_DECREF(old);
        }
    }

    PyObject* cached(const Key& key) const
    {
        std::map<Key, PyObject*>::const_iterator found = values.find(key);

        return found == values.end() ? nullptr : found->second;
    }

    PyObject* readStrings(const GeoAttribute& a,
                          int cls)
    {
        const Py_ssize_t n = Py_ssize_t(counts[cls]);
        const int t = a.tupleSize();
        PyObject* list = PyList_New(n);
        if (!list) {
            return nullptr;
        }
        for (Py_ssize_t i = 0; i < n; ++i) {
            const std::size_t offset = offsetOf(cls, i);
            PyObject* item = nullptr;
            if (t == 1) {
                item = stringToPy(a.getString(offset, 0));
            } else {
                item = PyTuple_New(t);
                for (int c = 0; item && c < t; ++c) {
                    PyObject* s = stringToPy(a.getString(offset, c));
                    if (!s) {
                        Py_CLEAR(item);
                        break;
                    }
                    PyTuple_SET_ITEM(item, c, s);
                }
            }
            if (!item) {
                Py_DECREF(list);

                return nullptr;
            }
            PyList_SET_ITEM(list, i, item);
        }

        return list;
    }

    PyObject* getAttribute(int cls,
                           const std::string& name)
    {
        const Key key { eValueAttribute, cls, name };
        if (PyObject* found = cached(key)) {
            Py_INCREF(found);

            return found;
        }
        const GeoAttribute* a = detail().findAttribute(GeoAttribClassEnum(cls), name);
        if (!a) {
            PyErr_Format(PyExc_KeyError, "geo.%s has no attribute \"%s\"", className(cls), name.c_str());

            return nullptr;
        }
        if (a->isArray()) {
            PyErr_Format(PyExc_TypeError, "\"%s\" is an array attribute, which scripts cannot read", name.c_str());

            return nullptr;
        }
        PyObject* value = nullptr;
        if (a->storage() == eGeoStorageString) {
            value = readStrings(*a, cls);
        } else {
            std::unique_ptr<ArrayData> d(new ArrayData);
            d->allocate(kindOfStorage(a->storage()), Py_ssize_t(counts[cls]), a->tupleSize());
            switch (a->storage()) {
            case eGeoStorageInt32:
                readDense<std::int32_t>(*a, cls, *d);
                break;
            case eGeoStorageInt64:
                readDense<std::int64_t>(*a, cls, *d);
                break;
            case eGeoStorageFloat32:
                readDense<float>(*a, cls, *d);
                break;
            case eGeoStorageFloat64:
                readDense<double>(*a, cls, *d);
                break;
            case eGeoStorageString:
                break;
            }
            value = newArray(std::move(d));
        }
        if (!value) {
            return nullptr;
        }
        Py_INCREF(value);
        storeValue(key, value);

        return value;
    }

    /** @brief Copies @p tmp into the cached array of @p key when it has the same layout, else caches a new one. */
    bool cacheArray(const Key& key,
                    std::unique_ptr<ArrayData> tmp)
    {
        PyObject* existing = cached(key);
        if (existing && isArrayObject(existing)) {
            ArrayData& d = arrayData(existing);
            if (d.kind == tmp->kind && d.tupleSize == tmp->tupleSize && d.count == tmp->count) {
                std::memcpy(d.bytes.data(), tmp->bytes.data(), d.bytes.size());
                d.dirty = true;

                return true;
            }
        }
        tmp->dirty = true;
        PyObject* fresh = newArray(std::move(tmp));
        if (!fresh) {
            return false;
        }
        storeValue(key, fresh);

        return true;
    }

    bool cacheStrings(const Key& key,
                      PyObject* list)
    {
        PyObject* existing = cached(key);
        if (existing && PyList_Check(existing)) {
            const int r = PyList_SetSlice(existing, 0, PyList_GET_SIZE(existing), list);
            Py_DECREF(list);

            return r == 0;
        }
        storeValue(key, list);

        return true;
    }

    bool setAttribute(int cls,
                      const std::string& name,
                      PyObject* value)
    {
        const Key key { eValueAttribute, cls, name };
        const Py_ssize_t n = Py_ssize_t(counts[cls]);
        const GeoAttribute* a = detail().findAttribute(GeoAttribClassEnum(cls), name);
        if (a) {
            if (a->isArray()) {
                PyErr_Format(PyExc_TypeError, "\"%s\" is an array attribute, which scripts cannot write", name.c_str());

                return false;
            }
            if (a->storage() == eGeoStorageString) {
                PyObject* list = stringListFrom(value, n, a->tupleSize());

                return list && cacheStrings(key, list);
            }
            std::unique_ptr<ArrayData> tmp(new ArrayData);
            tmp->allocate(kindOfStorage(a->storage()), n, a->tupleSize());
            if (!fillArray(*tmp, value)) {
                return false;
            }

            return cacheArray(key, std::move(tmp));
        }

        NewAttribute spec;
        if (!inferNewAttribute(value, n, &spec)) {
            return false;
        }
        GeoAttributeDesc desc;
        desc.name = name;
        desc.storage = spec.isString ? eGeoStorageString : storageOfKind(spec.kind);
        desc.tupleSize = spec.tupleSize;
        desc.typeInfo = spec.isString ? eGeoTypeInfoNone : typeInfoForName(name, spec.tupleSize);
        if (spec.isString) {
            PyObject* list = stringListFrom(value, n, spec.tupleSize);
            if (!list) {
                return false;
            }
            builder->addAttribute(GeoAttribClassEnum(cls), desc);

            return cacheStrings(key, list);
        }
        std::unique_ptr<ArrayData> tmp(new ArrayData);
        tmp->allocate(spec.kind, n, spec.tupleSize);
        if (!fillArray(*tmp, value)) {
            return false;
        }
        builder->addAttribute(GeoAttribClassEnum(cls), desc);

        return cacheArray(key, std::move(tmp));
    }

    bool removeAttribute(int cls,
                         const std::string& name)
    {
        if (!builder->removeAttribute(GeoAttribClassEnum(cls), name)) {
            PyErr_Format(PyExc_KeyError, "geo.%s has no attribute \"%s\"", className(cls), name.c_str());

            return false;
        }
        dropValue(Key { eValueAttribute, cls, name });

        return true;
    }

    PyObject* getGroup(int cls,
                       const std::string& name)
    {
        const Key key { eValueGroup, cls, name };
        if (PyObject* found = cached(key)) {
            Py_INCREF(found);

            return found;
        }
        const GeoElementGroup* g = detail().findGroup(GeoAttribClassEnum(cls), name);
        if (!g) {
            PyErr_Format(PyExc_KeyError, "geo.%s has no group \"%s\"", className(cls), name.c_str());

            return nullptr;
        }
        std::unique_ptr<ArrayData> d(new ArrayData);
        d->allocate(eArrayKindBool, Py_ssize_t(counts[cls]), 1);
        unsigned char* mask = d->as<unsigned char>();
        for (Py_ssize_t i = 0; i < d->count; ++i) {
            mask[i] = g->contains(GeoOffset(offsetOf(cls, i))) ? 1 : 0;
        }
        PyObject* value = newArray(std::move(d));
        if (!value) {
            return nullptr;
        }
        Py_INCREF(value);
        storeValue(key, value);

        return value;
    }

    bool setGroup(int cls,
                  const std::string& name,
                  PyObject* value)
    {
        std::unique_ptr<ArrayData> tmp(new ArrayData);
        tmp->allocate(eArrayKindBool, Py_ssize_t(counts[cls]), 1);
        if (!fillArray(*tmp, value)) {
            return false;
        }
        if (name.empty()) {
            PyErr_SetString(PyExc_ValueError, "a group needs a name");

            return false;
        }
        if (!detail().findGroup(GeoAttribClassEnum(cls), name)) {
            builder->addGroup(GeoAttribClassEnum(cls), name);
        }

        return cacheArray(Key { eValueGroup, cls, name }, std::move(tmp));
    }

    bool removeGroup(int cls,
                     const std::string& name)
    {
        if (!builder->removeGroup(GeoAttribClassEnum(cls), name)) {
            PyErr_Format(PyExc_KeyError, "geo.%s has no group \"%s\"", className(cls), name.c_str());

            return false;
        }
        dropValue(Key { eValueGroup, cls, name });

        return true;
    }

    bool commitStrings(int cls,
                       const std::string& name,
                       PyObject* list,
                       std::string* error)
    {
        GeoAttribute* a = builder->attributeForWriting(GeoAttribClassEnum(cls), name);
        if (!a) {
            return true;
        }
        const Py_ssize_t n = Py_ssize_t(counts[cls]);
        const int t = a->tupleSize();
        const std::string where = std::string("geo.") + className(cls) + "[\"" + name + "\"]";
        if (PyList_GET_SIZE(list) != n) {
            *error = where + " holds " + std::to_string(PyList_GET_SIZE(list)) + " values at the end of the script, expected " + std::to_string(n);

            return false;
        }
        std::vector<std::int32_t> indices(std::size_t(n) * std::size_t(t));
        for (Py_ssize_t i = 0; i < n; ++i) {
            PyObject* item = PyList_GET_ITEM(list, i);
            for (int c = 0; c < t; ++c) {
                PyObject* s = item;
                if (t > 1) {
                    if (!PyTuple_Check(item) || PyTuple_GET_SIZE(item) != t) {
                        *error = where + "[" + std::to_string(i) + "] is not a tuple of " + std::to_string(t) + " strings";

                        return false;
                    }
                    s = PyTuple_GET_ITEM(item, c);
                }
                std::string_view text;
                if (!PyUnicode_Check(s) || !utf8Of(s, &text)) {
                    PyErr_Clear();
                    *error = where + "[" + std::to_string(i) + "] is not a string";

                    return false;
                }
                std::int32_t index = a->stringTable().find(text);
                if (index < 0) {
                    index = a->internString(text);
                }
                indices[std::size_t(i) * std::size_t(t) + std::size_t(c)] = index;
            }
        }
        writeDense<std::int32_t>(*a, cls, std::span<const std::int32_t>(indices));

        return true;
    }

    bool commitArray(int cls,
                     const std::string& name,
                     const ArrayData& d)
    {
        GeoAttribute* a = builder->attributeForWriting(GeoAttribClassEnum(cls), name);
        if (!a || a->tupleSize() != d.tupleSize || kindOfStorage(a->storage()) != d.kind) {
            return true;
        }
        const std::size_t n = std::size_t(d.componentCount());
        switch (a->storage()) {
        case eGeoStorageInt32:
            writeDense<std::int32_t>(*a, cls, std::span<const std::int32_t>(d.as<std::int32_t>(), n));
            break;
        case eGeoStorageInt64:
            writeDense<std::int64_t>(*a, cls, std::span<const std::int64_t>(d.as<std::int64_t>(), n));
            break;
        case eGeoStorageFloat32:
            writeDense<float>(*a, cls, std::span<const float>(d.as<float>(), n));
            break;
        case eGeoStorageFloat64:
            writeDense<double>(*a, cls, std::span<const double>(d.as<double>(), n));
            break;
        case eGeoStorageString:
            break;
        }

        return true;
    }

    void commitGroup(int cls,
                     const std::string& name,
                     const ArrayData& d)
    {
        const GeoElementGroup* current = detail().findGroup(GeoAttribClassEnum(cls), name);
        if (!current) {
            return;
        }
        const unsigned char* mask = d.as<unsigned char>();
        std::vector<std::pair<GeoOffset, bool>> changes;
        for (Py_ssize_t i = 0; i < d.count; ++i) {
            const GeoOffset offset = GeoOffset(offsetOf(cls, i));
            const bool member = mask[i] != 0;
            if (current->contains(offset) != member) {
                changes.emplace_back(offset, member);
            }
        }
        if (changes.empty()) {
            return;
        }
        GeoElementGroup* group = builder->groupForWriting(GeoAttribClassEnum(cls), name);
        if (!group) {
            return;
        }
        for (const std::pair<GeoOffset, bool>& change : changes) {
            group->set(change.first, change.second);
        }
    }

    bool commitValues(std::string* error)
    {
        for (const std::pair<const Key, PyObject*>& entry : values) {
            const Key& key = entry.first;
            if (key.what == eValueGroup) {
                if (isArrayObject(entry.second) && arrayData(entry.second).dirty) {
                    commitGroup(key.cls, key.name, arrayData(entry.second));
                }
            } else if (PyList_Check(entry.second)) {
                if (!commitStrings(key.cls, key.name, entry.second, error)) {
                    return false;
                }
            } else if (isArrayObject(entry.second) && arrayData(entry.second).dirty) {
                commitArray(key.cls, key.name, arrayData(entry.second));
            }
        }

        return true;
    }

    bool pointOffsets(const std::vector<GeoIndex>& numbers,
                      GeoOffset firstNew,
                      std::vector<GeoOffset>* out,
                      std::string* error)
    {
        const GeoIndex existing = counts[eGeoAttribClassPoint];
        const GeoIndex total = existing + newPointCount();
        out->clear();
        out->reserve(numbers.size());
        for (GeoIndex number : numbers) {
            if (number < 0 || number >= total) {
                *error = "point " + std::to_string(number) + " does not exist (there are " + std::to_string(total) + ")";

                return false;
            }
            out->push_back(number < existing ? GeoOffset(offsetOf(eGeoAttribClassPoint, Py_ssize_t(number)))
                                             : firstNew + GeoOffset(number - existing));
        }

        return true;
    }

    bool commitTopology(std::string* error)
    {
        const GeoIndex existingPoints = counts[eGeoAttribClassPoint];
        const GeoIndex existingPrims = counts[eGeoAttribClassPrimitive];
        // Removals name elements by their numbers before the script's additions, so they are
        // resolved to offsets before anything is appended.
        std::vector<GeoOffset> doomedPrims;
        bool deleteUnusedPoints = false;
        for (const PrimRemoval& removal : removedPrims) {
            for (GeoIndex number : removal.numbers) {
                if (number < 0 || number >= existingPrims) {
                    *error = "geo.remove_prims: primitive " + std::to_string(number) + " does not exist (there are " + std::to_string(existingPrims) + ")";

                    return false;
                }
                doomedPrims.push_back(GeoOffset(offsetOf(eGeoAttribClassPrimitive, Py_ssize_t(number))));
            }
            deleteUnusedPoints = deleteUnusedPoints || removal.deleteUnusedPoints;
        }
        std::vector<GeoOffset> doomedPoints;
        for (GeoIndex number : removedPoints) {
            if (number < 0 || number >= existingPoints) {
                *error = "geo.remove_points: point " + std::to_string(number) + " does not exist (there are " + std::to_string(existingPoints) + ")";

                return false;
            }
            doomedPoints.push_back(GeoOffset(offsetOf(eGeoAttribClassPoint, Py_ssize_t(number))));
        }

        GeoOffset firstNewPoint = 0;
        if (newPointCount() > 0) {
            firstNewPoint = builder->appendPoints(newPointCount());
            for (GeoIndex i = 0; i < newPointCount(); ++i) {
                const std::size_t base = std::size_t(i) * 3;
                builder->setPointPosition(firstNewPoint + GeoOffset(i), newPositions[base], newPositions[base + 1], newPositions[base + 2]);
            }
        }
        for (const PrimBatch& batch : newPrims) {
            std::vector<GeoOffset> points;
            if (!pointOffsets(batch.points, firstNewPoint, &points, error)) {
                *error = "geo.add_prims: " + *error;

                return false;
            }
            try {
                builder->appendPrimitives(batch.type, std::span<const std::int32_t>(batch.counts), std::span<const GeoOffset>(points));
            } catch (const std::invalid_argument& e) {
                *error = std::string("geo.add_prims: ") + e.what();

                return false;
            }
        }

        if (!doomedPrims.empty()) {
            builder->deletePrimitives(doomedPrims, deleteUnusedPoints);
        }
        if (!doomedPoints.empty()) {
            const GeoDetail& d = detail();
            std::vector<unsigned char> doomedMask(d.elementOffsetSize(eGeoAttribClassPoint), 0);
            for (GeoOffset p : doomedPoints) {
                doomedMask[std::size_t(p)] = 1;
            }
            std::vector<GeoOffset> users;
            d.primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
                const GeoVertexRange range = d.primitiveVertices(prim);
                for (GeoOffset v = range.start; v < range.start + range.count; ++v) {
                    if (!d.vertexMap().isLive(v)) {
                        continue;
                    }
                    if (doomedMask[std::size_t(d.vertexPoint(v))]) {
                        users.push_back(prim);
                        break;
                    }
                }
            });
            if (!users.empty()) {
                builder->deletePrimitives(users, false);
            }
            builder->deletePoints(doomedPoints, eGeoPointDeleteDegenerate);
        }

        return true;
    }

    void detach()
    {
        if (geometry) {
            reinterpret_cast<GeometryObject*>(geometry)->session = nullptr;
        }
        for (PyObject* e : elements) {
            if (e) {
                reinterpret_cast<ElementsObject*>(e)->session = nullptr;
            }
        }
    }
};

namespace {

const char kExpiredMessage[] = "geo is only available while its script runs";

void
deallocHeapObject(PyObject* self)
{
    PyTypeObject* type = Py_TYPE(self);
    type->tp_free(self);
    Py_DECREF(type);
}

// ----- array -----

void
arrayDealloc(PyObject* self)
{
    delete reinterpret_cast<ArrayObject*>(self)->data;
    reinterpret_cast<ArrayObject*>(self)->data = nullptr;
    deallocHeapObject(self);
}

Py_ssize_t
arrayLength(PyObject* self)
{
    return arrayData(self).count;
}

PyObject*
arrayItem(PyObject* self,
          Py_ssize_t i)
{
    const ArrayData& d = arrayData(self);
    if (i < 0 || i >= d.count) {
        PyErr_SetString(PyExc_IndexError, "element number out of range");

        return nullptr;
    }

    return elementToPy(d, i);
}

int
arrayAssignItem(PyObject* self,
                Py_ssize_t i,
                PyObject* value)
{
    ArrayData& d = arrayData(self);
    if (!value) {
        PyErr_SetString(PyExc_TypeError, "elements cannot be deleted from an attribute array; use geo.remove_points or geo.remove_prims");

        return -1;
    }
    if (d.readonly) {
        PyErr_SetString(PyExc_TypeError, "this array is read only");

        return -1;
    }
    if (i < 0 || i >= d.count) {
        PyErr_SetString(PyExc_IndexError, "element number out of range");

        return -1;
    }
    if (!elementFromPy(d, i, value)) {
        return -1;
    }
    d.dirty = true;

    return 0;
}

int
arrayGetBuffer(PyObject* self,
               Py_buffer* view,
               int flags)
{
    ArrayData& d = arrayData(self);
    if ((flags & PyBUF_WRITABLE) && d.readonly) {
        PyErr_SetString(PyExc_BufferError, "this array is read only");
        view->obj = nullptr;

        return -1;
    }
    view->buf = d.bytes.data();
    view->obj = self;
    Py_INCREF(self);
    view->len = Py_ssize_t(d.bytes.size());
    view->readonly = d.readonly ? 1 : 0;
    view->itemsize = Py_ssize_t(kindBytes(d.kind));
    view->format = (flags & PyBUF_FORMAT) ? const_cast<char*>(kindFormat(d.kind)) : nullptr;
    if ((flags & PyBUF_ND) == PyBUF_ND) {
        view->ndim = d.ndim;
        view->shape = d.shape;
    } else {
        view->ndim = 1;
        view->shape = nullptr;
    }
    view->strides = (flags & PyBUF_STRIDES) == PyBUF_STRIDES ? d.strides : nullptr;
    view->suboffsets = nullptr;
    view->internal = nullptr;
    if (!d.readonly) {
        d.dirty = true;
    }

    return 0;
}

PyObject*
arrayToList(PyObject* self,
            PyObject* /*unused*/)
{
    const ArrayData& d = arrayData(self);
    PyObject* list = PyList_New(d.count);
    if (!list) {
        return nullptr;
    }
    for (Py_ssize_t i = 0; i < d.count; ++i) {
        PyObject* item = elementToPy(d, i);
        if (!item) {
            Py_DECREF(list);

            return nullptr;
        }
        PyList_SET_ITEM(list, i, item);
    }

    return list;
}

PyObject*
arrayTupleSize(PyObject* self,
               void* /*closure*/)
{
    return PyLong_FromLong(arrayData(self).tupleSize);
}

PyObject*
arrayDtype(PyObject* self,
           void* /*closure*/)
{
    return PyUnicode_FromString(kindName(arrayData(self).kind));
}

PyObject*
arrayReadonly(PyObject* self,
              void* /*closure*/)
{
    return PyBool_FromLong(arrayData(self).readonly ? 1 : 0);
}

PyObject*
arrayShape(PyObject* self,
           void* /*closure*/)
{
    const ArrayData& d = arrayData(self);
    if (d.ndim == 1) {
        return Py_BuildValue("(n)", d.count);
    }

    return Py_BuildValue("(ni)", d.count, d.tupleSize);
}

PyObject*
arrayRepr(PyObject* self)
{
    const ArrayData& d = arrayData(self);

    return PyUnicode_FromFormat("<geo array of %zd %s[%d]%s>", d.count, kindName(d.kind), d.tupleSize, d.readonly ? ", read only" : "");
}

PyMethodDef arrayMethods[] = {
    { "tolist", arrayToList, METH_NOARGS, "The values as a list of numbers, or of tuples for a tuple size above 1." },
    { nullptr, nullptr, 0, nullptr },
};

PyGetSetDef arrayGetSets[] = {
    { "tuple_size", arrayTupleSize, nullptr, "Components per element.", nullptr },
    { "dtype", arrayDtype, nullptr, "The numpy name of the component type.", nullptr },
    { "readonly", arrayReadonly, nullptr, "Whether writes are refused.", nullptr },
    { "shape", arrayShape, nullptr, "(N,) for a tuple size of 1, (N, tuple_size) otherwise.", nullptr },
    { nullptr, nullptr, nullptr, nullptr, nullptr },
};

PyType_Slot arraySlots[] = {
    { Py_tp_dealloc, reinterpret_cast<void*>(arrayDealloc) },
    { Py_tp_repr, reinterpret_cast<void*>(arrayRepr) },
    { Py_tp_methods, arrayMethods },
    { Py_tp_getset, arrayGetSets },
    { Py_sq_length, reinterpret_cast<void*>(arrayLength) },
    { Py_sq_item, reinterpret_cast<void*>(arrayItem) },
    { Py_sq_ass_item, reinterpret_cast<void*>(arrayAssignItem) },
    { Py_bf_getbuffer, reinterpret_cast<void*>(arrayGetBuffer) },
    { Py_tp_doc, const_cast<char*>("Dense values of one attribute or group, in element-number order.") },
    { 0, nullptr },
};

// ----- element classes -----

GeoPySession::Impl*
elementsSession(PyObject* self)
{
    GeoPySession::Impl* s = reinterpret_cast<ElementsObject*>(self)->session;
    if (!s) {
        PyErr_SetString(PyExc_RuntimeError, kExpiredMessage);
    }

    return s;
}

int
elementsClass(PyObject* self)
{
    return reinterpret_cast<ElementsObject*>(self)->cls;
}

bool
nameOf(PyObject* key,
       std::string* out)
{
    if (!PyUnicode_Check(key)) {
        PyErr_Format(PyExc_TypeError, "attribute names are str, not %s", Py_TYPE(key)->tp_name);

        return false;
    }
    std::string_view s;
    if (!utf8Of(key, &s)) {
        return false;
    }
    out->assign(s.data(), s.size());

    return true;
}

Py_ssize_t
elementsLength(PyObject* self)
{
    GeoPySession::Impl* s = elementsSession(self);

    return s ? Py_ssize_t(s->counts[elementsClass(self)]) : -1;
}

PyObject*
elementsGet(PyObject* self,
            PyObject* key)
{
    GeoPySession::Impl* s = elementsSession(self);
    std::string name;
    if (!s || !nameOf(key, &name)) {
        return nullptr;
    }

    return guarded<PyObject*>(nullptr, [&] { return s->getAttribute(elementsClass(self), name); });
}

int
elementsSet(PyObject* self,
            PyObject* key,
            PyObject* value)
{
    GeoPySession::Impl* s = elementsSession(self);
    std::string name;
    if (!s || !nameOf(key, &name)) {
        return -1;
    }
    const int cls = elementsClass(self);
    const bool ok = guarded<bool>(false, [&] {
        return value ? s->setAttribute(cls, name, value) : s->removeAttribute(cls, name);
    });

    return ok ? 0 : -1;
}

int
elementsContains(PyObject* self,
                 PyObject* key)
{
    GeoPySession::Impl* s = elementsSession(self);
    if (!s) {
        return -1;
    }
    if (!PyUnicode_Check(key)) {
        return 0;
    }
    std::string_view name;
    if (!utf8Of(key, &name)) {
        return -1;
    }

    return s->detail().findAttribute(GeoAttribClassEnum(elementsClass(self)), name) ? 1 : 0;
}

PyObject*
elementsAttributes(PyObject* self,
                   PyObject* /*unused*/)
{
    GeoPySession::Impl* s = elementsSession(self);
    if (!s) {
        return nullptr;
    }
    const GeoAttribClassEnum cls = GeoAttribClassEnum(elementsClass(self));
    const std::size_t n = s->detail().attributeCount(cls);
    PyObject* list = PyList_New(Py_ssize_t(n));
    if (!list) {
        return nullptr;
    }
    for (std::size_t i = 0; i < n; ++i) {
        PyObject* name = stringToPy(s->detail().attribute(cls, i).desc().name);
        if (!name) {
            Py_DECREF(list);

            return nullptr;
        }
        PyList_SET_ITEM(list, Py_ssize_t(i), name);
    }

    return list;
}

PyObject*
elementsAttributeInfo(PyObject* self,
                      PyObject* args)
{
    GeoPySession::Impl* s = elementsSession(self);
    const char* name = nullptr;
    if (!s || !PyArg_ParseTuple(args, "s:attribute_info", &name)) {
        return nullptr;
    }
    const int cls = elementsClass(self);
    const GeoAttribute* a = s->detail().findAttribute(GeoAttribClassEnum(cls), name);
    if (!a) {
        PyErr_Format(PyExc_KeyError, "geo.%s has no attribute \"%s\"", className(cls), name);

        return nullptr;
    }

    return Py_BuildValue("{s:s,s:i,s:s,s:O}",
                         "storage", storageName(a->storage()),
                         "tuple_size", a->tupleSize(),
                         "type_info", typeInfoName(a->desc().typeInfo),
                         "is_array", a->isArray() ? Py_True : Py_False);
}

bool
checkGroupClass(int cls)
{
    if (cls == eGeoAttribClassDetail) {
        PyErr_SetString(PyExc_TypeError, "geo.detail has no groups");

        return false;
    }

    return true;
}

PyObject*
elementsGroups(PyObject* self,
               PyObject* /*unused*/)
{
    GeoPySession::Impl* s = elementsSession(self);
    const int cls = elementsClass(self);
    if (!s || !checkGroupClass(cls)) {
        return nullptr;
    }

    return guarded<PyObject*>(nullptr, [&]() -> PyObject* {
        const std::vector<std::string> names = s->detail().groupNames(GeoAttribClassEnum(cls));
        PyObject* list = PyList_New(Py_ssize_t(names.size()));
        if (!list) {
            return nullptr;
        }
        for (std::size_t i = 0; i < names.size(); ++i) {
            PyObject* name = stringToPy(names[i]);
            if (!name) {
                Py_DECREF(list);

                return nullptr;
            }
            PyList_SET_ITEM(list, Py_ssize_t(i), name);
        }

        return list;
    });
}

PyObject*
elementsGroup(PyObject* self,
              PyObject* args)
{
    GeoPySession::Impl* s = elementsSession(self);
    const int cls = elementsClass(self);
    const char* name = nullptr;
    if (!s || !checkGroupClass(cls) || !PyArg_ParseTuple(args, "s:group", &name)) {
        return nullptr;
    }

    return guarded<PyObject*>(nullptr, [&] { return s->getGroup(cls, name); });
}

PyObject*
elementsSetGroup(PyObject* self,
                 PyObject* args)
{
    GeoPySession::Impl* s = elementsSession(self);
    const int cls = elementsClass(self);
    const char* name = nullptr;
    PyObject* value = nullptr;
    if (!s || !checkGroupClass(cls) || !PyArg_ParseTuple(args, "sO:set_group", &name, &value)) {
        return nullptr;
    }
    if (!guarded<bool>(false, [&] { return s->setGroup(cls, name, value); })) {
        return nullptr;
    }
    Py_RETURN_NONE;
}

PyObject*
elementsRemoveGroup(PyObject* self,
                    PyObject* args)
{
    GeoPySession::Impl* s = elementsSession(self);
    const int cls = elementsClass(self);
    const char* name = nullptr;
    if (!s || !checkGroupClass(cls) || !PyArg_ParseTuple(args, "s:remove_group", &name)) {
        return nullptr;
    }
    if (!guarded<bool>(false, [&] { return s->removeGroup(cls, name); })) {
        return nullptr;
    }
    Py_RETURN_NONE;
}

PyObject*
elementsRepr(PyObject* self)
{
    GeoPySession::Impl* s = reinterpret_cast<ElementsObject*>(self)->session;
    const int cls = elementsClass(self);
    if (!s) {
        return PyUnicode_FromFormat("<geo.%s, expired>", className(cls));
    }

    return PyUnicode_FromFormat("<geo.%s: %lld elements>", className(cls), (long long)s->counts[cls]);
}

PyMethodDef elementsMethods[] = {
    { "attributes", elementsAttributes, METH_NOARGS, "The names of this class's attributes." },
    { "attribute_info", elementsAttributeInfo, METH_VARARGS, "A dict of an attribute's storage, tuple_size, type_info and is_array." },
    { "groups", elementsGroups, METH_NOARGS, "The names of this class's groups." },
    { "group", elementsGroup, METH_VARARGS, "A group's membership as a bool array, one value per element." },
    { "set_group", elementsSetGroup, METH_VARARGS, "Creates or overwrites a group from one truth value per element, or one for all." },
    { "remove_group", elementsRemoveGroup, METH_VARARGS, "Removes a group." },
    { nullptr, nullptr, 0, nullptr },
};

PyType_Slot elementsSlots[] = {
    { Py_tp_dealloc, reinterpret_cast<void*>(deallocHeapObject) },
    { Py_tp_repr, reinterpret_cast<void*>(elementsRepr) },
    { Py_tp_methods, elementsMethods },
    { Py_mp_length, reinterpret_cast<void*>(elementsLength) },
    { Py_mp_subscript, reinterpret_cast<void*>(elementsGet) },
    { Py_mp_ass_subscript, reinterpret_cast<void*>(elementsSet) },
    { Py_sq_contains, reinterpret_cast<void*>(elementsContains) },
    { Py_tp_doc, const_cast<char*>("The attributes and groups of one element class, by name.") },
    { 0, nullptr },
};

// ----- geometry -----

GeoPySession::Impl*
geometrySession(PyObject* self)
{
    GeoPySession::Impl* s = reinterpret_cast<GeometryObject*>(self)->session;
    if (!s) {
        PyErr_SetString(PyExc_RuntimeError, kExpiredMessage);
    }

    return s;
}

PyObject*
geometryElements(PyObject* self,
                 void* closure)
{
    GeoPySession::Impl* s = geometrySession(self);
    if (!s) {
        return nullptr;
    }
    PyObject* e = s->elements[reinterpret_cast<std::intptr_t>(closure)];
    Py_INCREF(e);

    return e;
}

/** @brief Calls @p f(primOffset) for every live primitive and @p g(vertexOffset) for its live vertices. */
template <typename F, typename G>
void
forEachPrimitiveVertex(const GeoDetail& d,
                       F&& f,
                       G&& g)
{
    const GeoIndexMap& vertices = d.vertexMap();
    d.primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
        f(prim);
        const GeoVertexRange range = d.primitiveVertices(prim);
        for (GeoOffset v = range.start; v < range.start + range.count; ++v) {
            if (vertices.isLive(v)) {
                g(v);
            }
        }
    });
}

PyObject*
readonlyArray(ArrayKindEnum kind,
              const std::vector<std::int64_t>& values)
{
    std::unique_ptr<ArrayData> d(new ArrayData);
    d->allocate(kind, Py_ssize_t(values.size()), 1);
    for (std::size_t i = 0; i < values.size(); ++i) {
        d->setInt(Py_ssize_t(i), values[i]);
    }
    d->readonly = true;

    return newArray(std::move(d));
}

PyObject*
geometryPrimTypes(PyObject* self,
                  PyObject* /*unused*/)
{
    GeoPySession::Impl* s = geometrySession(self);
    if (!s) {
        return nullptr;
    }
    const GeoDetail& d = s->detail();
    PyObject* list = PyList_New(0);
    if (!list) {
        return nullptr;
    }
    bool ok = true;
    d.primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
        if (!ok) {
            return;
        }
        PyObject* name = PyUnicode_FromString(primitiveTypeName(d.primitiveType(prim)));
        ok = name && PyList_Append(list, name) == 0;
        Py_XDECREF(name);
    });
    if (!ok) {
        Py_DECREF(list);

        return nullptr;
    }

    return list;
}

PyObject*
geometryPrimVertexCounts(PyObject* self,
                         PyObject* /*unused*/)
{
    GeoPySession::Impl* s = geometrySession(self);
    if (!s) {
        return nullptr;
    }

    return guarded<PyObject*>(nullptr, [&] {
        std::vector<std::int64_t> counts;
        forEachPrimitiveVertex(s->detail(), [&](GeoOffset) { counts.push_back(0); }, [&](GeoOffset) { ++counts.back(); });

        return readonlyArray(eArrayKindInt32, counts);
    });
}

PyObject*
geometryPrimVertices(PyObject* self,
                     PyObject* /*unused*/)
{
    GeoPySession::Impl* s = geometrySession(self);
    if (!s) {
        return nullptr;
    }

    return guarded<PyObject*>(nullptr, [&] {
        const GeoDetail& d = s->detail();
        std::vector<std::int64_t> numbers;
        forEachPrimitiveVertex(d, [](GeoOffset) {}, [&](GeoOffset v) { numbers.push_back(d.vertexMap().indexFromOffset(v)); });

        return readonlyArray(eArrayKindInt64, numbers);
    });
}

PyObject*
geometryPrimPoints(PyObject* self,
                   PyObject* args)
{
    GeoPySession::Impl* s = geometrySession(self);
    long long number = 0;
    if (!s || !PyArg_ParseTuple(args, "L:prim_points", &number)) {
        return nullptr;
    }
    const GeoIndex count = s->counts[eGeoAttribClassPrimitive];
    if (number < 0) {
        number += count;
    }
    if (number < 0 || number >= count) {
        PyErr_SetString(PyExc_IndexError, "primitive number out of range");

        return nullptr;
    }

    return guarded<PyObject*>(nullptr, [&]() -> PyObject* {
        const GeoDetail& d = s->detail();
        const GeoOffset prim = GeoOffset(s->offsetOf(eGeoAttribClassPrimitive, Py_ssize_t(number)));
        const GeoVertexRange range = d.primitiveVertices(prim);
        std::vector<GeoIndex> points;
        for (GeoOffset v = range.start; v < range.start + range.count; ++v) {
            if (d.vertexMap().isLive(v)) {
                points.push_back(d.pointMap().indexFromOffset(d.vertexPoint(v)));
            }
        }
        PyObject* tuple = PyTuple_New(Py_ssize_t(points.size()));
        if (!tuple) {
            return nullptr;
        }
        for (std::size_t i = 0; i < points.size(); ++i) {
            PyObject* p = PyLong_FromLongLong(points[i]);
            if (!p) {
                Py_DECREF(tuple);

                return nullptr;
            }
            PyTuple_SET_ITEM(tuple, Py_ssize_t(i), p);
        }

        return tuple;
    });
}

PyObject*
geometryVertexNumbers(PyObject* self,
                      bool points)
{
    GeoPySession::Impl* s = geometrySession(self);
    if (!s) {
        return nullptr;
    }

    return guarded<PyObject*>(nullptr, [&] {
        const GeoDetail& d = s->detail();
        std::vector<std::int64_t> numbers;
        numbers.reserve(std::size_t(s->counts[eGeoAttribClassVertex]));
        d.vertexMap().forEachLive([&](GeoIndex /*index*/, GeoOffset v) {
            numbers.push_back(points ? d.pointMap().indexFromOffset(d.vertexPoint(v))
                                     : d.primitiveMap().indexFromOffset(d.vertexPrimitive(v)));
        });

        return readonlyArray(eArrayKindInt64, numbers);
    });
}

PyObject*
geometryVertexPoints(PyObject* self,
                     PyObject* /*unused*/)
{
    return geometryVertexNumbers(self, true);
}

PyObject*
geometryVertexPrims(PyObject* self,
                    PyObject* /*unused*/)
{
    return geometryVertexNumbers(self, false);
}

PyObject*
geometryAddPoints(PyObject* self,
                  PyObject* arg)
{
    GeoPySession::Impl* s = geometrySession(self);
    if (!s) {
        return nullptr;
    }
    const GeoIndex first = s->counts[eGeoAttribClassPoint] + s->newPointCount();
    std::vector<double> positions;
    if (PyLong_Check(arg)) {
        const long long count = PyLong_AsLongLong(arg);
        if (count == -1 && PyErr_Occurred()) {
            return nullptr;
        }
        if (count < 0) {
            PyErr_SetString(PyExc_ValueError, "a point count cannot be negative");

            return nullptr;
        }
        positions.assign(std::size_t(count) * 3, 0.);
    } else {
        const Py_ssize_t count = isArrayObject(arg) ? arrayData(arg).count : PySequence_Length(arg);
        if (count < 0) {
            PyErr_Clear();
            PyErr_Format(PyExc_TypeError, "geo.add_points takes a count or positions, not %s", Py_TYPE(arg)->tp_name);

            return nullptr;
        }
        ArrayData d;
        d.allocate(eArrayKindFloat64, count, 3);
        if (!fillArray(d, arg)) {
            return nullptr;
        }
        positions.assign(d.as<double>(), d.as<double>() + d.componentCount());
    }
    s->newPositions.insert(s->newPositions.end(), positions.begin(), positions.end());

    return newRange(first, first + GeoIndex(positions.size() / 3));
}

bool
checkPointNumbers(GeoPySession::Impl* s,
                  const std::vector<GeoIndex>& points)
{
    const GeoIndex total = s->counts[eGeoAttribClassPoint] + s->newPointCount();
    for (GeoIndex p : points) {
        if (p < 0 || p >= total) {
            PyErr_Format(PyExc_IndexError, "point %lld does not exist (there are %lld)", (long long)p, (long long)total);

            return false;
        }
    }

    return true;
}

PyObject*
queuePrims(GeoPySession::Impl* s,
           const char* typeName,
           std::vector<std::int32_t> counts,
           std::vector<GeoIndex> points)
{
    GeoPrimitiveTypeEnum type = eGeoPrimitivePolygon;
    if (!primitiveTypeFromName(typeName, &type) || !checkPointNumbers(s, points)) {
        return nullptr;
    }
    std::size_t total = 0;
    for (std::int32_t c : counts) {
        if (c < 1) {
            PyErr_Format(PyExc_ValueError, "a primitive needs at least one point, got a count of %d", int(c));

            return nullptr;
        }
        total += std::size_t(c);
    }
    if (total != points.size()) {
        PyErr_Format(PyExc_ValueError, "the counts add up to %zu points but %zu were given", total, points.size());

        return nullptr;
    }
    const GeoIndex first = s->counts[eGeoAttribClassPrimitive] + s->newPrimCount;
    GeoPySession::Impl::PrimBatch batch;
    batch.type = type;
    batch.counts = std::move(counts);
    batch.points = std::move(points);
    s->newPrimCount += GeoIndex(batch.counts.size());
    s->newPrims.push_back(std::move(batch));

    return newRange(first, s->counts[eGeoAttribClassPrimitive] + s->newPrimCount);
}

PyObject*
geometryAddPrim(PyObject* self,
                PyObject* args,
                PyObject* kwargs)
{
    GeoPySession::Impl* s = geometrySession(self);
    PyObject* pointsArg = nullptr;
    const char* typeName = "polygon";
    static const char* keywords[] = { "points", "type", nullptr };
    if (!s || !PyArg_ParseTupleAndKeywords(args, kwargs, "O|s:add_prim", const_cast<char**>(keywords), &pointsArg, &typeName)) {
        return nullptr;
    }
    std::vector<GeoIndex> points;
    if (!indicesFrom(pointsArg, &points)) {
        return nullptr;
    }
    const std::int32_t count = std::int32_t(points.size());
    PyObject* range = queuePrims(s, typeName, std::vector<std::int32_t>(1, count), std::move(points));
    if (!range) {
        return nullptr;
    }
    Py_DECREF(range);

    return PyLong_FromLongLong(s->counts[eGeoAttribClassPrimitive] + s->newPrimCount - 1);
}

PyObject*
geometryAddPrims(PyObject* self,
                 PyObject* args,
                 PyObject* kwargs)
{
    GeoPySession::Impl* s = geometrySession(self);
    PyObject* countsArg = nullptr;
    PyObject* pointsArg = nullptr;
    const char* typeName = "polygon";
    static const char* keywords[] = { "counts", "points", "type", nullptr };
    if (!s || !PyArg_ParseTupleAndKeywords(args, kwargs, "OO|s:add_prims", const_cast<char**>(keywords), &countsArg, &pointsArg, &typeName)) {
        return nullptr;
    }
    std::vector<GeoIndex> counts64;
    std::vector<GeoIndex> points;
    if (!indicesFrom(countsArg, &counts64) || !indicesFrom(pointsArg, &points)) {
        return nullptr;
    }
    std::vector<std::int32_t> counts;
    counts.reserve(counts64.size());
    for (GeoIndex c : counts64) {
        if (c > std::numeric_limits<std::int32_t>::max()) {
            PyErr_SetString(PyExc_ValueError, "a primitive vertex count is too large");

            return nullptr;
        }
        counts.push_back(std::int32_t(c < std::numeric_limits<std::int32_t>::min() ? 0 : c));
    }

    return queuePrims(s, typeName, std::move(counts), std::move(points));
}

PyObject*
geometryRemovePoints(PyObject* self,
                     PyObject* arg)
{
    GeoPySession::Impl* s = geometrySession(self);
    std::vector<GeoIndex> numbers;
    if (!s || !indicesFrom(arg, &numbers)) {
        return nullptr;
    }
    const GeoIndex count = s->counts[eGeoAttribClassPoint];
    for (GeoIndex n : numbers) {
        if (n < 0 || n >= count) {
            PyErr_Format(PyExc_IndexError, "point %lld does not exist (there are %lld before the script's additions)", (long long)n, (long long)count);

            return nullptr;
        }
    }
    s->removedPoints.insert(s->removedPoints.end(), numbers.begin(), numbers.end());
    Py_RETURN_NONE;
}

PyObject*
geometryRemovePrims(PyObject* self,
                    PyObject* args,
                    PyObject* kwargs)
{
    GeoPySession::Impl* s = geometrySession(self);
    PyObject* numbersArg = nullptr;
    int deleteUnusedPoints = 0;
    static const char* keywords[] = { "numbers", "delete_unused_points", nullptr };
    if (!s || !PyArg_ParseTupleAndKeywords(args, kwargs, "O|p:remove_prims", const_cast<char**>(keywords), &numbersArg, &deleteUnusedPoints)) {
        return nullptr;
    }
    GeoPySession::Impl::PrimRemoval removal;
    if (!indicesFrom(numbersArg, &removal.numbers)) {
        return nullptr;
    }
    const GeoIndex count = s->counts[eGeoAttribClassPrimitive];
    for (GeoIndex n : removal.numbers) {
        if (n < 0 || n >= count) {
            PyErr_Format(PyExc_IndexError, "primitive %lld does not exist (there are %lld before the script's additions)", (long long)n, (long long)count);

            return nullptr;
        }
    }
    removal.deleteUnusedPoints = deleteUnusedPoints != 0;
    s->removedPrims.push_back(std::move(removal));
    Py_RETURN_NONE;
}

PyObject*
geometryRepr(PyObject* self)
{
    GeoPySession::Impl* s = reinterpret_cast<GeometryObject*>(self)->session;
    if (!s) {
        return PyUnicode_FromString("<geo, expired>");
    }

    return PyUnicode_FromFormat("<geo: %lld points, %lld vertices, %lld prims>",
                                (long long)s->counts[eGeoAttribClassPoint],
                                (long long)s->counts[eGeoAttribClassVertex],
                                (long long)s->counts[eGeoAttribClassPrimitive]);
}

PyMethodDef geometryMethods[] = {
    { "prim_types", geometryPrimTypes, METH_NOARGS, "Each primitive's type name." },
    { "prim_vertex_counts", geometryPrimVertexCounts, METH_NOARGS, "Each primitive's vertex count, read only." },
    { "prim_vertices", geometryPrimVertices, METH_NOARGS, "Every primitive's vertex numbers, concatenated, read only." },
    { "prim_points", geometryPrimPoints, METH_VARARGS, "The point numbers of one primitive's vertices." },
    { "vertex_points", geometryVertexPoints, METH_NOARGS, "Each vertex's point number, read only." },
    { "vertex_prims", geometryVertexPrims, METH_NOARGS, "Each vertex's primitive number, read only." },
    { "add_points", geometryAddPoints, METH_O, "Adds a count of points at the origin, or one point per position; returns their numbers." },
    { "add_prim", reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)(void)>(geometryAddPrim)), METH_VARARGS | METH_KEYWORDS,
      "Adds a polygon or polyline through point numbers; returns its number." },
    { "add_prims", reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)(void)>(geometryAddPrims)), METH_VARARGS | METH_KEYWORDS,
      "Adds one primitive per count, taking that many point numbers each; returns their numbers." },
    { "remove_points", geometryRemovePoints, METH_O, "Removes points and every primitive using them." },
    { "remove_prims", reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)(void)>(geometryRemovePrims)), METH_VARARGS | METH_KEYWORDS,
      "Removes primitives, and optionally the points nothing else uses." },
    { nullptr, nullptr, 0, nullptr },
};

PyGetSetDef geometryGetSets[] = {
    { "points", geometryElements, nullptr, "Point attributes and groups.", reinterpret_cast<void*>(std::intptr_t(eGeoAttribClassPoint)) },
    { "vertices", geometryElements, nullptr, "Vertex attributes and groups.", reinterpret_cast<void*>(std::intptr_t(eGeoAttribClassVertex)) },
    { "prims", geometryElements, nullptr, "Primitive attributes and groups.", reinterpret_cast<void*>(std::intptr_t(eGeoAttribClassPrimitive)) },
    { "detail", geometryElements, nullptr, "Attributes of the whole geometry, one element.", reinterpret_cast<void*>(std::intptr_t(eGeoAttribClassDetail)) },
    { nullptr, nullptr, nullptr, nullptr, nullptr },
};

PyType_Slot geometrySlots[] = {
    { Py_tp_dealloc, reinterpret_cast<void*>(deallocHeapObject) },
    { Py_tp_repr, reinterpret_cast<void*>(geometryRepr) },
    { Py_tp_methods, geometryMethods },
    { Py_tp_getset, geometryGetSets },
    { Py_tp_doc, const_cast<char*>("The geometry a script edits.") },
    { 0, nullptr },
};

#ifdef Py_TPFLAGS_DISALLOW_INSTANTIATION
const unsigned int kTypeFlags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION;
#else
const unsigned int kTypeFlags = Py_TPFLAGS_DEFAULT;
#endif

PyType_Spec arraySpec = { "NatronEngine.GeoArray", int(sizeof(ArrayObject)), 0, kTypeFlags, arraySlots };
PyType_Spec elementsSpec = { "NatronEngine.GeoElements", int(sizeof(ElementsObject)), 0, kTypeFlags, elementsSlots };
PyType_Spec geometrySpec = { "NatronEngine.GeoGeometry", int(sizeof(GeometryObject)), 0, kTypeFlags, geometrySlots };

/** @brief Creates the types once; they live for the rest of the process. */
bool
ensureTypes()
{
    PyTypes& t = pyTypes();
    if (!t.array) {
        t.array = PyType_FromSpec(&arraySpec);
    }
    if (!t.elements) {
        t.elements = PyType_FromSpec(&elementsSpec);
    }
    if (!t.geometry) {
        t.geometry = PyType_FromSpec(&geometrySpec);
    }

    return t.array && t.elements && t.geometry;
}

std::string
takePythonErrorText()
{
    PyObject* type = nullptr;
    PyObject* value = nullptr;
    PyObject* traceback = nullptr;
    PyErr_Fetch(&type, &value, &traceback);
    std::string text = "unknown Python error";
    if (value) {
        if (PyObject* s = PyObject_Str(value)) {
            std::string_view v;
            if (utf8Of(s, &v)) {
                text.assign(v.data(), v.size());
            }
            Py_DECREF(s);
        }
    }
    PyErr_Clear();
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(traceback);

    return text;
}

} // anonymous namespace

GeoPySession::GeoPySession(GeoDetailBuilder& builder)
    : _imp(new Impl)
{
    if (!ensureTypes()) {
        throw std::runtime_error("cannot create the Python geometry types: " + takePythonErrorText());
    }
    Impl& s = *_imp;
    s.builder = &builder;
    const GeoDetail& d = builder.detail();
    s.counts[eGeoAttribClassPoint] = d.pointCount();
    s.counts[eGeoAttribClassVertex] = d.vertexCount();
    s.counts[eGeoAttribClassPrimitive] = d.primitiveCount();
    s.counts[eGeoAttribClassDetail] = 1;

    s.geometry = PyType_GenericAlloc(reinterpret_cast<PyTypeObject*>(pyTypes().geometry), 0);
    if (!s.geometry) {
        throw std::runtime_error("cannot create the geo object: " + takePythonErrorText());
    }
    reinterpret_cast<GeometryObject*>(s.geometry)->session = &s;
    for (int cls = 0; cls < kGeoAttribClassCount; ++cls) {
        s.elements[cls] = PyType_GenericAlloc(reinterpret_cast<PyTypeObject*>(pyTypes().elements), 0);
        if (!s.elements[cls]) {
            const std::string why = takePythonErrorText();
            s.detach();
            for (PyObject*& e : s.elements) {
                Py_CLEAR(e);
            }
            Py_CLEAR(s.geometry);
            throw std::runtime_error("cannot create the geo object: " + why);
        }
        reinterpret_cast<ElementsObject*>(s.elements[cls])->session = &s;
        reinterpret_cast<ElementsObject*>(s.elements[cls])->cls = cls;
    }
}

GeoPySession::~GeoPySession()
{
    Impl& s = *_imp;
    s.detach();
    for (PyObject*& e : s.elements) {
        Py_CLEAR(e);
    }
    Py_CLEAR(s.geometry);
    std::map<Impl::Key, PyObject*> values;
    values.swap(s.values);
    for (std::pair<const Impl::Key, PyObject*>& entry : values) {
        Py_DECREF(entry.second);
    }
}

PyObject*
GeoPySession::geometry() const
{
    return _imp->geometry;
}

bool
GeoPySession::commit(std::string* error)
{
    std::string scratch;
    std::string& message = error ? *error : scratch;
    try {
        return _imp->commitValues(&message) && _imp->commitTopology(&message);
    } catch (const std::exception& e) {
        message = e.what();
    }

    return false;
}

NATRON_NAMESPACE_EXIT
