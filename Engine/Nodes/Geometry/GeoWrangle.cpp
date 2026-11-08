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

#include "GeoWrangle.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <QString>

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Curve.h"
#include "Engine/GeoDetail.h"
#include "Engine/Kernel/KernelParser.h"
#include "Engine/KnobTypes.h"
#include "Engine/NodeMetadata.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/PythonUndoCommands.h"
#include "Engine/UndoRecorder.h"

NATRON_NAMESPACE_ENTER

namespace {

const GeoAttribClassEnum kRunOverClasses[] = {
    eGeoAttribClassPoint,
    eGeoAttribClassPrimitive,
    eGeoAttribClassVertex,
    eGeoAttribClassDetail,
};

bool
isBlank(const std::string& s)
{
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); });
}

std::string
trimmed(const std::string& s)
{
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    const auto first = std::find_if(s.begin(), s.end(), notSpace);
    const auto last = std::find_if(s.rbegin(), s.rend(), notSpace).base();

    return first < last ? std::string(first, last) : std::string();
}

// "matrixSize" and "matrix_size" both read "Matrix Size".
std::string
readableLabel(const std::string& name)
{
    std::string label;
    bool startWord = true;
    for (std::size_t k = 0; k < name.size(); ++k) {
        const unsigned char c = (unsigned char)name[k];
        if (c == '_') {
            startWord = true;
            continue;
        }
        const bool upperAfterLower = std::isupper(c) && k > 0 && std::islower((unsigned char)name[k - 1]);
        if ((startWord || upperAfterLower) && !label.empty()) {
            label += ' ';
        }
        label += (startWord || upperAfterLower) ? char(std::toupper(c)) : char(c);
        startWord = false;
    }

    return label;
}

std::string
compileKey(const std::string& snippet,
           GeoAttribClassEnum runOver,
           const GeoKernelParameters& parameters)
{
    std::string key = std::to_string(int(runOver));
    for (const auto& p : parameters) {
        key += '\n';
        key += p.first;
        key += ':';
        key += std::to_string(int(p.second.kind));
    }
    key += '\0';
    key += snippet;

    return key;
}

std::string
compileErrorText(const KernelCompileError& error)
{
    return "line " + std::to_string(error.line) + ", column " + std::to_string(error.column) + ": " + error.message;
}

} // namespace

std::vector<GeoWrangleParameterCall>
geoWrangleParameterCalls(const std::string& source)
{
    static const std::set<std::string> readers = { "ch", "chf", "chi", "chv", "chs", "chramp" };
    std::vector<KernelToken> tokens;
    KernelCompileError error;
    // A malformed token ends the scan; the calls before it still count.
    kernelLex(source, tokens, error);

    std::vector<GeoWrangleParameterCall> calls;
    std::set<std::string> seen;
    for (std::size_t k = 0; k + 2 < tokens.size(); ++k) {
        const KernelToken& t = tokens[k];
        if (t.kind != KernelToken::Kind::eIdentifier || !readers.count(t.text)
            || !tokens[k + 1].isPunct("(") || tokens[k + 2].kind != KernelToken::Kind::eString) {
            continue;
        }
        const std::string& name = tokens[k + 2].text;
        if (name.empty() || !seen.insert(name).second) {
            continue;
        }
        calls.push_back(GeoWrangleParameterCall { name, t.text });
    }

    return calls;
}

GeoWrangle::GeoWrangle(NodePtr node)
    : NativeEffectBase(node)
{
}

GeoWrangle::~GeoWrangle()
{
}

const NodeSpec*
GeoWrangle::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
GeoWrangle::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    KnobChoicePtr runOver = createKnob<KnobChoice>(tr("Run Over").toStdString());
    runOver->setName(kGeoWrangleParamRunOver);
    runOver->setHintToolTip(tr("What the snippet runs once for: each point, each primitive, each vertex, or the "
                               "geometry as a whole. @name attributes read and write that class.")
                                .toStdString());
    std::vector<ChoiceOption> options;
    options.push_back(ChoiceOption("points", tr("Points").toStdString(), tr("Run once per point.").toStdString()));
    options.push_back(ChoiceOption("primitives", tr("Primitives").toStdString(), tr("Run once per primitive.").toStdString()));
    options.push_back(ChoiceOption("vertices", tr("Vertices").toStdString(), tr("Run once per vertex.").toStdString()));
    options.push_back(ChoiceOption("detail", tr("Detail").toStdString(), tr("Run once for the whole geometry.").toStdString()));
    runOver->populateChoices(options);
    runOver->setDefaultValue(0);
    runOver->setAnimationEnabled(false);
    page->addKnob(runOver);
    _runOver = runOver;

    KnobStringPtr group = createKnob<KnobString>(tr("Group").toStdString());
    group->setName(kGeoWrangleParamGroup);
    group->setHintToolTip(tr("When set, the snippet runs only for the members of this group of the Run Over class; "
                             "the others pass through unchanged. Ignored when running over the detail.")
                              .toStdString());
    group->setAnimationEnabled(false);
    page->addKnob(group);
    _group = group;

    KnobStringPtr snippet = createKnob<KnobString>(tr("Snippet").toStdString());
    snippet->setName(kGeoWrangleParamSnippet);
    snippet->setAsMultiLine();
    snippet->setHintToolTip(tr("The geometry kernel language code to run. An empty snippet passes the input through.")
                                .toStdString());
    snippet->setAnimationEnabled(false);
    page->addKnob(snippet);
    _snippet = snippet;

    KnobButtonPtr createParameters = createKnob<KnobButton>(tr("Create Parameters").toStdString());
    createParameters->setName(kGeoWrangleParamCreateParameters);
    createParameters->setHintToolTip(tr("Add a user parameter for each name the snippet reads with ch, chf, chi, chv, chs "
                                        "or chramp that the node does not have yet: a float for ch and chf, an integer "
                                        "for chi, a 3D float for chv, a string for chs and a curve for chramp.")
                                         .toStdString());
    page->addKnob(createParameters);
    _createParameters = createParameters;
}

bool
GeoWrangle::knobChanged(KnobI* k,
                        ValueChangedReasonEnum /*reason*/,
                        ViewSpec /*view*/,
                        double /*time*/,
                        bool /*originatedFromMainThread*/)
{
    if (k && (k == _createParameters.lock().get())) {
        createParameters();

        return true;
    }

    return false;
}

std::vector<std::string>
GeoWrangle::createParameters()
{
    KnobStringPtr snippet = _snippet.lock();
    if (!snippet) {
        return {};
    }
    std::vector<GeoWrangleParameterCall> missing;
    for (const GeoWrangleParameterCall& call : geoWrangleParameterCalls(snippet->getValue())) {
        if (!getKnobByName(call.name)) {
            missing.push_back(call);
        }
    }
    if (missing.empty()) {
        return {};
    }

    std::vector<std::string> added;
    {
        UndoStepGuard step(getApp(), tr("Create Parameters").toStdString());
        PythonUserKnobsUndoGuard undo(this);
        for (const GeoWrangleParameterCall& call : missing) {
            const std::string label = readableLabel(call.name);
            KnobIPtr knob;
            if (call.function == "chi") {
                knob = createIntKnob(call.name, label, 1);
            } else if (call.function == "chv") {
                knob = createDoubleKnob(call.name, label, 3);
            } else if (call.function == "chs") {
                knob = createStringKnob(call.name, label);
            } else if (call.function == "chramp") {
                KnobParametricPtr ramp = createParametricKnob(call.name, label, 1);
                if (ramp) {
                    StatusEnum first = ramp->addControlPoint(eValueChangedReasonPluginEdited, 0, 0., 0., eKeyframeTypeLinear);
                    StatusEnum second = ramp->addControlPoint(eValueChangedReasonPluginEdited, 0, 1., 1., eKeyframeTypeLinear);
                    Q_UNUSED(first);
                    Q_UNUSED(second);
                    ramp->setDefaultCurvesFromCurves();
                }
                knob = ramp;
            } else {
                knob = createDoubleKnob(call.name, label, 1);
            }
            if (!knob) {
                continue;
            }
            KnobPagePtr userPage = knob->getParentKnob() ? KnobPagePtr() : getOrCreateUserPageKnob();
            if (userPage) {
                userPage->addKnob(knob);
            }
            added.push_back(call.name);
        }
    }
    recreateUserKnobs(true);

    return added;
}

GeoAttribClassEnum
GeoWrangle::runOver() const
{
    KnobChoicePtr knob = _runOver.lock();
    const int index = knob ? knob->getValue() : 0;

    return index >= 0 && index < int(std::size(kRunOverClasses)) ? kRunOverClasses[index] : eGeoAttribClassPoint;
}

GeoKernelParameters
GeoWrangle::readParameters(double time,
                           ViewIdx view) const
{
    GeoKernelParameters parameters;
    for (const KnobIPtr& knob : getKnobs_mt_safe()) {
        if (!knob || !knob->isUserKnob()) {
            continue;
        }
        GeoKernelParameter p;
        if (KnobParametric* ramp = dynamic_cast<KnobParametric*>(knob.get())) {
            CurvePtr curve = ramp->getParametricCurve(0);
            if (!curve) {
                continue;
            }
            // A copy, so a curve edit during the render does not race the worker threads.
            std::shared_ptr<const Curve> copy = std::make_shared<const Curve>(*curve);
            p.kind = GeoKernelParameterKind::eRamp;
            p.ramp = [copy](double x) { return copy->getValueAt(x); };
        } else if (KnobDoubleBase* number = dynamic_cast<KnobDoubleBase*>(knob.get())) {
            p.components = std::min(number->getDimension(), 3);
            for (int d = 0; d < p.components; ++d) {
                p.values[std::size_t(d)] = number->getValueAtTime(time, d, view);
            }
        } else if (KnobInt* integer = dynamic_cast<KnobInt*>(knob.get())) {
            p.components = std::min(integer->getDimension(), 3);
            for (int d = 0; d < p.components; ++d) {
                p.values[std::size_t(d)] = integer->getValueAtTime(time, d, view);
            }
        } else if (KnobChoice* choice = dynamic_cast<KnobChoice*>(knob.get())) {
            p.values[0] = choice->getValueAtTime(time, 0, view);
        } else if (KnobBool* boolean = dynamic_cast<KnobBool*>(knob.get())) {
            p.values[0] = boolean->getValueAtTime(time, 0, view) ? 1. : 0.;
        } else if (KnobString* string = dynamic_cast<KnobString*>(knob.get())) {
            p.kind = GeoKernelParameterKind::eString;
            p.string = string->getValueAtTime(time, 0, view);
        } else {
            continue;
        }
        parameters.emplace(knob->getName(), std::move(p));
    }

    return parameters;
}

std::shared_ptr<const GeoWrangle::Compiled>
GeoWrangle::compile(const std::string& snippet,
                    GeoAttribClassEnum runOver,
                    const GeoKernelParameters& parameters) const
{
    const std::string key = compileKey(snippet, runOver, parameters);
    {
        std::lock_guard<std::mutex> lock(_compiledLock);
        if (_compiled && (_compiledKey == key)) {
            return _compiled;
        }
    }

    std::shared_ptr<Compiled> compiled = std::make_shared<Compiled>();
    const GeoKernelBinding binding(runOver, &parameters);
    std::shared_ptr<KernelProgram> program = std::make_shared<KernelProgram>();
    if (kernelCompile(snippet, binding, *program, compiled->error)) {
        compiled->program = std::move(program);
    }

    std::lock_guard<std::mutex> lock(_compiledLock);
    _compiledKey = key;
    _compiled = compiled;

    return compiled;
}

Route
GeoWrangle::route(double time,
                  ViewIdx /*view*/)
{
    KnobStringPtr snippet = _snippet.lock();
    if (snippet && !isBlank(snippet->getValue())) {
        return Route::none();
    }
    // renderGeometry(), which clears a compile error once the snippet compiles, no longer runs.
    clearPersistentMessage(false);

    return Route::input(0, time);
}

StatusEnum
GeoWrangle::getPreferredMetadataAt(double time,
                                   ViewIdx view,
                                   NodeMetadata& metadata)
{
    KnobStringPtr snippet = _snippet.lock();
    if (!snippet) {
        return eStatusOK;
    }
    const std::string source = snippet->getValue();
    if (isBlank(source)) {
        return eStatusOK;
    }
    // @Frame and @Time change the output every frame with nothing animated.
    const std::shared_ptr<const Compiled> compiled = compile(source, runOver(), readParameters(time, view));
    if (compiled->program && geoKernelReadsTime(*compiled->program)) {
        metadata.setIsFrameVarying(true);
    }

    return eStatusOK;
}

StatusEnum
GeoWrangle::renderGeometry(const GeoRenderActionArgs& args,
                           GeoDetailConstPtr* outputDetail,
                           std::string* errorMessage)
{
    KnobStringPtr snippetKnob = _snippet.lock();
    KnobStringPtr groupKnob = _group.lock();
    const std::string snippet = snippetKnob ? snippetKnob->getValue() : std::string();
    const GeoAttribClassEnum cls = runOver();
    const GeoKernelParameters parameters = readParameters(args.time, args.view);

    const std::shared_ptr<const Compiled> compiled = compile(snippet, cls, parameters);
    if (!compiled->program) {
        const std::string message = compileErrorText(compiled->error);
        setPersistentMessage(eMessageTypeError, message);
        *errorMessage = message;

        return eStatusFailed;
    }

    const GeoDetailConstPtr source = args.getInputDetail(0);
    const GeoDetailConstPtr second = args.getInputDetail(1);
    const GeoDetail empty;
    const std::array<const GeoDetail*, 2> inputs = { source ? source.get() : &empty, second.get() };

    GeoKernelRunOptions options;
    options.frame = args.time;
    const double fps = getApp() ? getApp()->getProjectFrameRate() : 0.;
    options.time = fps > 0. ? args.time / fps : 0.;
    options.group = cls == eGeoAttribClassDetail || !groupKnob ? std::string() : trimmed(groupKnob->getValue());
    options.parameters = &parameters;
    // The kernel polls from pool threads, where this node's abort flag is not visible.
    const ParallelRenderArgsPtr frameArgs = getParallelRenderArgsTLS();
    const AbortableRenderInfoPtr render = frameArgs ? frameArgs->abortInfo.lock() : AbortableRenderInfoPtr();
    if (render) {
        options.aborted = [render]() { return render->isAborted(); };
    }

    GeoKernelRunResult result = geoKernelRun(*compiled->program, cls, std::span<const GeoDetail* const>(inputs), options);
    if (result.status == KernelRunStatus::eAborted) {
        return eStatusFailed;
    }
    if (result.status == KernelRunStatus::eFailed) {
        setPersistentMessage(eMessageTypeError, result.error);
        *errorMessage = result.error;

        return eStatusFailed;
    }

    clearPersistentMessage(false);
    *outputDetail = std::make_shared<const GeoDetail>(std::move(result.detail));

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
