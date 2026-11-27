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
#undef Py_LIMITED_API // Needed for PyEval_SetTrace and frame-level tracing
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "GeoPython.h"

#include <exception>
#include <string>

#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Geometry/GeoPyBuffer.h"

NATRON_NAMESPACE_ENTER

namespace {

const char kTraceCapsuleName[] = "NatronEngine.GeoPythonTrace";

// Polling the abort flag costs a TLS lookup; every few hundred lines keeps a pure-Python loop
// responsive to cancellation without measurably slowing it.
const int kLinesPerAbortCheck = 256;

bool
isBlank(const std::string& s)
{
    return s.find_first_not_of(" \t\r\n") == std::string::npos;
}

struct TraceState {
    const std::function<bool()>* aborted = nullptr;
    int lines = 0;
    bool stop = false;
};

int
traceForAbort(PyObject* obj,
              PyFrameObject* /*frame*/,
              int what,
              PyObject* /*arg*/)
{
    if (what != PyTrace_LINE) {
        return 0;
    }
    TraceState* state = static_cast<TraceState*>(PyCapsule_GetPointer(obj, kTraceCapsuleName));
    if (!state) {
        return -1;
    }
    if (!state->stop && ++state->lines >= kLinesPerAbortCheck) {
        state->lines = 0;
        state->stop = *state->aborted && (*state->aborted)();
    }
    if (state->stop) {
        // Raised again on every line, so a script catching it cannot keep running.
        PyErr_SetString(PyExc_KeyboardInterrupt, "render aborted");

        return -1;
    }

    return 0;
}

std::string
utf8String(PyObject* s)
{
    Py_ssize_t size = 0;
    const char* data = s ? PyUnicode_AsUTF8AndSize(s, &size) : nullptr;
    if (!data) {
        PyErr_Clear();

        return std::string();
    }

    return std::string(data, std::size_t(size));
}

/** @brief The pending exception as a formatted traceback; clears it. */
std::string
takeTraceback()
{
    PyObject* type = nullptr;
    PyObject* value = nullptr;
    PyObject* traceback = nullptr;
    PyErr_Fetch(&type, &value, &traceback);
    if (!type) {
        return "unknown Python error";
    }
    PyErr_NormalizeException(&type, &value, &traceback);
    if (value && traceback) {
        PyException_SetTraceback(value, traceback);
    }

    std::string text;
    if (PyObject* module = PyImport_ImportModule("traceback")) {
        PyObject* lines = PyObject_CallMethod(module, "format_exception", "OOO", type, value ? value : Py_None, traceback ? traceback : Py_None);
        if (lines) {
            PyObject* empty = PyUnicode_FromString("");
            PyObject* joined = empty ? PyUnicode_Join(empty, lines) : nullptr;
            text = utf8String(joined);
            Py_XDECREF(joined);
            Py_XDECREF(empty);
            Py_DECREF(lines);
        }
        Py_DECREF(module);
    }
    PyErr_Clear();
    if (text.empty() && value) {
        PyObject* s = PyObject_Str(value);
        text = utf8String(s);
        Py_XDECREF(s);
        PyErr_Clear();
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(traceback);

    return text.empty() ? std::string("unknown Python error") : text;
}

/**
 * @brief Makes the script's source visible to traceback formatting, which otherwise prints
 * only file names and line numbers for code that came from a string.
 **/
class SourceRegistration {
public:
    SourceRegistration(const std::string& name,
                       const std::string& source)
        : _name(name)
    {
        PyObject* module = PyImport_ImportModule("linecache");
        PyObject* cache = module ? PyObject_GetAttrString(module, "cache") : nullptr;
        PyObject* text = PyUnicode_DecodeUTF8(source.data(), Py_ssize_t(source.size()), "replace");
        PyObject* lines = text ? PyObject_CallMethod(text, "splitlines", "O", Py_True) : nullptr;
        if (cache && lines && PyDict_Check(cache)) {
            _entry = Py_BuildValue("(nOOs)", Py_ssize_t(source.size()), Py_None, lines, name.c_str());
            if (_entry && PyDict_SetItemString(cache, name.c_str(), _entry) == 0) {
                _cache = cache;
                Py_INCREF(_cache);
            }
        }
        Py_XDECREF(lines);
        Py_XDECREF(text);
        Py_XDECREF(cache);
        Py_XDECREF(module);
        PyErr_Clear();
    }

    ~SourceRegistration()
    {
        if (_cache) {
            // Another run of the same node may have replaced the entry; that one is not ours.
            if (PyDict_GetItemString(_cache, _name.c_str()) == _entry) {
                PyDict_DelItemString(_cache, _name.c_str());
            }
            Py_DECREF(_cache);
        }
        Py_XDECREF(_entry);
        PyErr_Clear();
    }

    SourceRegistration(const SourceRegistration&) = delete;
    SourceRegistration& operator=(const SourceRegistration&) = delete;

private:
    std::string _name;
    PyObject* _cache = nullptr;
    PyObject* _entry = nullptr;
};

/** @brief Runs @p code with the session's `geo`; false with the exception pending on failure. */
bool
evaluateScript(PyObject* code,
               GeoPySession& session,
               double frame,
               TraceState& trace)
{
    PyObject* globals = PyDict_New();
    if (!globals) {
        return false;
    }
    bool ok = false;
    PyObject* builtins = PyImport_ImportModule("builtins");
    PyObject* frameValue = PyFloat_FromDouble(frame);
    PyObject* name = PyUnicode_FromString("__geopython__");
    PyObject* capsule = PyCapsule_New(&trace, kTraceCapsuleName, nullptr);
    if (builtins && frameValue && name && capsule
        && PyDict_SetItemString(globals, "__builtins__", builtins) == 0
        && PyDict_SetItemString(globals, "__name__", name) == 0
        && PyDict_SetItemString(globals, "geo", session.geometry()) == 0
        && PyDict_SetItemString(globals, "frame", frameValue) == 0) {
        PyEval_SetTrace(traceForAbort, capsule);
        PyObject* result = PyEval_EvalCode(code, globals, globals);
        PyObject* type = nullptr;
        PyObject* value = nullptr;
        PyObject* traceback = nullptr;
        PyErr_Fetch(&type, &value, &traceback);
        PyEval_SetTrace(nullptr, nullptr);
        PyErr_Restore(type, value, traceback);
        ok = result != nullptr;
        Py_XDECREF(result);
    }
    PyObject* type = nullptr;
    PyObject* value = nullptr;
    PyObject* traceback = nullptr;
    PyErr_Fetch(&type, &value, &traceback);
    // Functions the script defined hold the globals, so clearing them breaks those cycles now
    // rather than leaving them, and the geo object, to the garbage collector.
    PyDict_Clear(globals);
    PyErr_Restore(type, value, traceback);
    Py_DECREF(globals);
    Py_XDECREF(capsule);
    Py_XDECREF(name);
    Py_XDECREF(frameValue);
    Py_XDECREF(builtins);

    return ok;
}

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
} // anonymous namespace

GeoPythonRunResultEnum
GeoPython::runScript(GeoDetailBuilder& builder,
                     const std::string& script,
                     const std::string& sourceName,
                     double frame,
                     const std::function<bool()>& aborted,
                     std::string* error)
{
    std::string scratch;
    std::string& message = error ? *error : scratch;
    message.clear();

    PythonGILLocker gil;
    const SourceRegistration source(sourceName, script);
    PyObject* code = Py_CompileString(script.c_str(), sourceName.c_str(), Py_file_input);
    if (!code) {
        message = takeTraceback();

        return eGeoPythonRunFailed;
    }

    GeoPythonRunResultEnum result = eGeoPythonRunFailed;
    try {
        GeoPySession session(builder);
        TraceState trace;
        trace.aborted = &aborted;
        if (!evaluateScript(code, session, frame, trace)) {
            if (trace.stop || (aborted && aborted())) {
                PyErr_Clear();
                result = eGeoPythonRunAborted;
            } else {
                message = takeTraceback();
            }
        } else if (aborted && aborted()) {
            result = eGeoPythonRunAborted;
        } else if (session.commit(&message)) {
            result = eGeoPythonRunOK;
        }
    } catch (const std::exception& e) {
        PyErr_Clear();
        message = e.what();
    }
    Py_DECREF(code);

    return result;
}

GeoPython::GeoPython(NodePtr node)
    : NativeEffectBase(node)
{
}

GeoPython::~GeoPython()
{
}

const NodeSpec*
GeoPython::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
GeoPython::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    KnobStringPtr script = createKnob<KnobString>(tr("Script").toStdString());
    script->setName(kGeoPythonParamScript);
    script->setHintToolTip(tr("Python run on every render of this node, with geo (the geometry) and frame (the "
                              "current frame) defined.\n"
                              "geo.points, geo.vertices, geo.prims and geo.detail hold each class's attributes by "
                              "name: geo.points[\"P\"] reads every position as an array of (x, y, z) in point-number "
                              "order, and assigning an array, a list or a single value writes it back. Assigning to a "
                              "new name creates the attribute; del removes one. Each class also has groups(), "
                              "group(name) and set_group(name, mask).\n"
                              "Arrays support the buffer protocol, so with numpy installed numpy.asarray(geo.points[\"P\"]) "
                              "is a writable view of the positions.\n"
                              "geo.add_points, geo.add_prim, geo.add_prims, geo.remove_points and geo.remove_prims "
                              "change the elements once the script ends; prim_points, prim_vertex_counts, "
                              "vertex_points and the other read-only queries describe the topology.\n"
                              "An exception, or a value of the wrong length or type, becomes this node's error with the "
                              "traceback. A blank script passes the input through.")
                               .toStdString());
    script->setAsMultiLine();
    script->setAnimationEnabled(false);
    page->addKnob(script);
    _script = script;
}

StatusEnum
GeoPython::getPreferredMetadataAt(double /*time*/,
                                  ViewIdx /*view*/,
                                  NodeMetadata& metadata)
{
    // The input's geometry may be the same on every frame, so the cache cannot tell that a
    // script reading the frame changes the result; any mention of it counts.
    const KnobStringPtr script = _script.lock();
    if (script && script->getValue().find("frame") != std::string::npos) {
        metadata.setIsFrameVarying(true);
    }

    return eStatusOK;
}

StatusEnum
GeoPython::renderGeometry(const GeoRenderActionArgs& args,
                          GeoDetailConstPtr* outputDetail,
                          std::string* errorMessage)
{
    const std::string script = valueAt<std::string>(_script, std::string(), args.time, args.view);
    const std::string sourceName = getScriptName_mt_safe();
    const double frame = args.time;
    const GeoBuildFunc run = [&](GeoDetailBuilder& builder, std::string* error) -> StatusEnum {
        if (isBlank(script)) {
            return eStatusOK;
        }
        std::string message;
        const GeoPythonRunResultEnum result = runScript(builder, script, sourceName, frame, [this] { return aborted(); }, &message);
        if (result == eGeoPythonRunOK) {
            return eStatusOK;
        }
        if (error) {
            *error = result == eGeoPythonRunAborted ? std::string("aborted") : message;
        }

        return eStatusFailed;
    };

    if (args.getInputDetail(0)) {
        return renderGeometryFromInput(args, 0, run, outputDetail, errorMessage);
    }

    return renderGeometryGenerator(run, outputDetail, errorMessage);
}

NATRON_NAMESPACE_EXIT
