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

#include "SceneTransformTarget.h"

#include <algorithm>
#include <cmath>
#include <set>

#include <QUndoCommand>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneReparentNode.h"
#include "Engine/Nodes/Scene/USDEditTransform.h"
#include "Engine/Settings.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGraphUndoRedo.h"
#include "Gui/SceneTransformWidget.h"
#include "Gui/UndoHistory.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

bool
SceneTransformRecorder::commit(const SceneTransformDrag& drag)
{
    _last = drag;
    ++_count;

    return false;
}

NATRON_NAMESPACE_ANONYMOUS_ENTER

struct SceneStringEdit {
    KnobStringWPtr knob;
    std::string oldValue;
    std::string newValue;
};

void
setSceneKnobValue(const KnobDoublePtr& knob,
                  int dimension,
                  double frame,
                  bool animated,
                  double value)
{
    if (animated) {
        knob->setValueAtTime(frame, value, ViewSpec::all(), dimension, eValueChangedReasonNatronGuiEdited, nullptr);
    } else {
        knob->setValue(value, ViewSpec::all(), dimension, eValueChangedReasonNatronGuiEdited, nullptr);
    }
}

/**
 * @brief Edits of one node's knobs at a frame, already made when the command is pushed, so that
 * the first redo does nothing. Undoing an animated dimension that had no key at the frame removes
 * the key the edit added.
 **/
class SceneKnobEditCommand
    : public QUndoCommand {
public:
    SceneKnobEditCommand(const NodePtr& node,
                         double frame,
                         const std::vector<SceneKnobEdit>& edits,
                         const std::vector<SceneStringEdit>& strings,
                         const QString& text)
        : QUndoCommand(text)
        , _node(node)
        , _frame(frame)
        , _edits(edits)
        , _strings(strings)
    {
    }

    void undo() override
    {
        apply(true);
    }

    void redo() override
    {
        if (_firstRedo) {
            _firstRedo = false;

            return;
        }
        apply(false);
    }

    static void write(const NodePtr& node,
                      double frame,
                      const std::vector<SceneKnobEdit>& edits,
                      const std::vector<SceneStringEdit>& strings,
                      bool old)
    {
        EffectInstancePtr effect = node ? node->getEffectInstance() : EffectInstancePtr();

        if (!effect) {
            return;
        }
        effect->beginChanges();
        for (const SceneStringEdit& s : strings) {
            if (KnobStringPtr knob = s.knob.lock()) {
                knob->setValue(old ? s.oldValue : s.newValue, ViewSpec::all(), 0, eValueChangedReasonNatronGuiEdited, nullptr);
            }
        }
        for (const SceneKnobEdit& d : edits) {
            KnobDoublePtr knob = d.knob.lock();
            if (!knob) {
                continue;
            }
            if (old && d.animated && !d.hadKey) {
                // Public on the knob interface only.
                const KnobIPtr base = knob;
                base->deleteValueAtTime(eCurveChangeReasonInternal, frame, ViewSpec::all(), d.dimension, false);
            } else {
                setSceneKnobValue(knob, d.dimension, frame, d.animated, old ? d.oldValue : d.newValue);
            }
        }
        effect->endChanges();
    }

private:
    void apply(bool old)
    {
        write(_node.lock(), _frame, _edits, _strings, old);
    }

    NodeWPtr _node;
    double _frame;
    std::vector<SceneKnobEdit> _edits;
    std::vector<SceneStringEdit> _strings;
    bool _firstRedo = true;
};

void
applyEdits(const NodePtr& node,
           double frame,
           const std::vector<SceneKnobEdit>& edits,
           const std::vector<SceneStringEdit>& strings,
           UndoHistory* history,
           const QString& text)
{
    if (!node || (edits.empty() && strings.empty())) {
        return;
    }
    SceneKnobEditCommand::write(node, frame, edits, strings, false);
    if (history) {
        history->push(new SceneKnobEditCommand(node, frame, edits, strings, text), UndoScope(UndoScope::holderForNode(node)));
    }
}

std::string
primPathOf(const NodePtr& node)
{
    KnobStringPtr knob = std::dynamic_pointer_cast<KnobString>(node->getKnobByName(kSceneParamPrimPath));

    return knob ? knob->getValue() : std::string();
}

// A node's translate, rotate and scale knobs, and its pivot when it has one.
struct TransformKnobs {
    KnobDoublePtr translate;
    KnobDoublePtr rotate;
    KnobDoublePtr scale;
    KnobDoublePtr uniformScale;
    KnobChoicePtr rotateOrder;
    KnobDoublePtr pivot;
};

bool
getTransformKnobs(const NodePtr& node,
                  TransformKnobs* knobs)
{
    knobs->translate = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kSceneParamTranslate));
    knobs->rotate = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kSceneParamRotate));
    knobs->scale = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kSceneParamScale));
    knobs->uniformScale = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kSceneParamUniformScale));
    knobs->rotateOrder = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kSceneParamRotateOrder));
    // USDXformPrim and USDEditTransform name theirs alike.
    knobs->pivot = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kUSDEditTransformParamPivot));

    return knobs->translate && knobs->rotate && knobs->scale && knobs->uniformScale && knobs->rotateOrder;
}

// The transform values a node's knobs hold, or that a new USDEditTransform starts from.
struct TransformValues {
    GfVec3d translate = GfVec3d(0.);
    GfVec3d rotate = GfVec3d(0.);
    GfVec3d scale = GfVec3d(1.);
    double uniformScale = 1.;
    SceneRotateOrderEnum rotateOrder = eSceneRotateOrderXYZ;
    GfVec3d pivot = GfVec3d(0.);

    GfMatrix4d compose() const
    {
        return SceneNodeAuthoring::composeTransformMatrix(translate, rotate, scale * uniformScale, pivot, rotateOrder);
    }
};

TransformValues
readTransformValues(const TransformKnobs& knobs,
                    double frame)
{
    TransformValues values;

    for (int d = 0; d < 3; ++d) {
        values.translate[d] = knobs.translate->getValueAtTime(frame, d);
        values.rotate[d] = knobs.rotate->getValueAtTime(frame, d);
        values.scale[d] = knobs.scale->getValueAtTime(frame, d);
        values.pivot[d] = knobs.pivot ? knobs.pivot->getValueAtTime(frame, d) : 0.;
    }
    values.uniformScale = knobs.uniformScale->getValueAtTime(frame, 0);
    const int order = knobs.rotateOrder->getValue();
    if ((order >= (int)eSceneRotateOrderXYZ) && (order <= (int)eSceneRotateOrderZYX)) {
        values.rotateOrder = (SceneRotateOrderEnum)order;
    }

    return values;
}

/**
 * @brief The values whose compose() is matrix, about base's pivot, with base's uniform scale and
 * rotate order. The signs of the scale and the rotate values nearest base's are kept, as the
 * matrix does not tell them apart. False for a matrix that collapses an axis.
 **/
bool
decomposeTransform(const GfMatrix4d& matrix,
                   const TransformValues& base,
                   TransformValues* values)
{
    *values = base;
    if (std::abs(base.uniformScale) < 1e-12) {
        return false;
    }
    GfMatrix4d rotation(1.);
    for (int i = 0; i < 3; ++i) {
        const GfVec3d row(matrix[i][0], matrix[i][1], matrix[i][2]);
        const double length = row.GetLength();
        if (length < 1e-12) {
            return false;
        }
        const double sign = (base.scale[i] * base.uniformScale < 0.) ? -1. : 1.;
        const double scale = sign * length;
        for (int j = 0; j < 3; ++j) {
            rotation[i][j] = row[j] / scale;
        }
        values->scale[i] = scale / base.uniformScale;
    }
    values->rotate = SceneNodeAuthoring::decomposeRotationMatrix(rotation, base.rotateOrder, base.rotate);
    // compose() moves the origin to translate + pivot - pivot * A.
    values->translate = matrix.ExtractTranslation() - base.pivot + matrix.TransformDir(base.pivot);

    return true;
}

bool
changedValue(double oldValue,
             double newValue)
{
    return std::abs(newValue - oldValue) > 1e-9 * std::max(1., std::abs(oldValue));
}

// The edits that take knobs from base to values at frame.
void
makeTransformEdits(const TransformKnobs& knobs,
                   double frame,
                   const TransformValues& base,
                   const TransformValues& values,
                   std::vector<SceneKnobEdit>* edits)
{
    for (int d = 0; d < 3; ++d) {
        if (changedValue(base.translate[d], values.translate[d])) {
            edits->push_back(makeSceneKnobEdit(knobs.translate, d, frame, values.translate[d]));
        }
        if (changedValue(base.rotate[d], values.rotate[d])) {
            edits->push_back(makeSceneKnobEdit(knobs.rotate, d, frame, values.rotate[d]));
        }
        if (changedValue(base.scale[d], values.scale[d])) {
            edits->push_back(makeSceneKnobEdit(knobs.scale, d, frame, values.scale[d]));
        }
    }
}

const SceneTransformOp*
findOp(const SceneTransformPrim& prim,
       const std::string& name)
{
    for (const SceneTransformOp& op : prim.ops) {
        if (op.name == name) {
            return &op;
        }
    }

    return nullptr;
}

/**
 * @brief The edits of node's transform knobs that move the result of its op, which space places
 * in the world, by the world delta: op' = op * space * delta * space^-1.
 **/
bool
makeOpEdits(const NodePtr& node,
            const GfMatrix4d& space,
            const GfMatrix4d& delta,
            double frame,
            std::vector<SceneKnobEdit>* edits)
{
    TransformKnobs knobs;

    if (!getTransformKnobs(node, &knobs)) {
        return false;
    }
    const TransformValues base = readTransformValues(knobs, frame);
    TransformValues values;
    if (!decomposeTransform(base.compose() * space * delta * space.GetInverse(), base, &values)) {
        return false;
    }
    makeTransformEdits(knobs, frame, base, values, edits);

    return true;
}

// The name of the first op the owner node authors on its prim.
std::string
getOwnerOpName(const NodePtr& owner)
{
    // USDXformPrim authors one matrix op; the other owners translate, rotate and scale ops.
    return dynamic_cast<SceneReparentNode*>(owner->getEffectInstance().get()) ? UsdGeomXformOp::GetOpName(UsdGeomXformOp::TypeTransform).GetString() : UsdGeomXformOp::GetOpName(UsdGeomXformOp::TypeTranslate).GetString();
}

USDEditTransform*
toEditTransform(const NodePtr& node)
{
    return node ? dynamic_cast<USDEditTransform*>(node->getEffectInstance().get()) : nullptr;
}

std::string
joinPaths(const std::vector<std::string>& paths)
{
    std::string joined;

    for (const std::string& path : paths) {
        if (!joined.empty()) {
            joined += ' ';
        }
        joined += path;
    }

    return joined;
}

/**
 * @brief The edits of an existing USDEditTransform for prims, the current one last. Its one
 * transform applies to every prim it matches, so the delta is taken in the space of the last
 * prim's op; a prim it does not match yet gets the op appended last, in the prim's own frame.
 * Prims the pattern misses are added to it.
 **/
bool
editExistingNode(const NodePtr& node,
                 const std::vector<const SceneTransformPrim*>& prims,
                 const GfMatrix4d& delta,
                 double frame,
                 UndoHistory* history,
                 const QString& text)
{
    USDEditTransform* edit = toEditTransform(node);

    if (!edit || prims.empty()) {
        return false;
    }
    std::vector<std::string> matched, opNames;
    edit->getMatchedPrims(&matched, &opNames);
    const SceneTransformPrim* reference = nullptr;
    GfMatrix4d space(1.);
    std::vector<std::string> missing;
    for (const SceneTransformPrim* prim : prims) {
        const std::vector<std::string>::const_iterator it = std::find(matched.begin(), matched.end(), prim->path);
        const SceneTransformOp* op = (it == matched.end()) ? nullptr : findOp(*prim, opNames[it - matched.begin()]);
        if (it == matched.end()) {
            missing.push_back(prim->path);
        }
        if (op) {
            reference = prim;
            space = SceneTransformWidget::loadMatrix(op->space);
        } else if (!reference) {
            space = SceneTransformWidget::loadMatrix(prim->world);
        }
    }
    std::vector<SceneKnobEdit> edits;
    if (!makeOpEdits(node, space, delta, frame, &edits)) {
        return false;
    }
    std::vector<SceneStringEdit> strings;
    if (!missing.empty()) {
        KnobStringPtr pattern = std::dynamic_pointer_cast<KnobString>(node->getKnobByName(kUSDEditTransformParamPrimPattern));
        if (pattern) {
            SceneStringEdit s;
            s.knob = pattern;
            s.oldValue = pattern->getValue();
            s.newValue = s.oldValue.empty() ? joinPaths(missing) : s.oldValue + ' ' + joinPaths(missing);
            strings.push_back(s);
        }
    }
    if (edits.empty() && strings.empty()) {
        return false;
    }
    applyEdits(node, frame, edits, strings, history, text);

    return true;
} // editExistingNode

/**
 * @brief Inserts a USDEditTransform after viewed, its pattern naming prims (the current one
 * last), set to move them by delta: its op goes last in each prim's op order, so in the last
 * prim's frame W it is W * delta * W^-1, about the widget's pivot.
 **/
NodePtr
createEditNode(const NodePtr& viewed,
               const std::vector<const SceneTransformPrim*>& prims,
               const GfMatrix4d& delta,
               const GfVec3d& pivotWorld)
{
    AppInstancePtr app = viewed ? viewed->getApp() : AppInstancePtr();

    if (!app || prims.empty()) {
        return NodePtr();
    }
    const GfMatrix4d world = SceneTransformWidget::loadMatrix(prims.back()->world);
    const GfMatrix4d worldInverse = world.GetInverse();
    TransformValues base;
    base.pivot = worldInverse.Transform(pivotWorld);
    TransformValues values;
    if (!decomposeTransform(world * delta * worldInverse, base, &values)) {
        return NodePtr();
    }
    std::vector<std::string> paths;
    for (const SceneTransformPrim* prim : prims) {
        paths.push_back(prim->path);
    }

    CreateNodeArgs args(PLUGINID_NATRON_USDEDITTRANSFORM, viewed->getGroup());
    double x = 0., y = 0.;
    viewed->getPosition(&x, &y);
    args.setProperty<double>(kCreateNodeArgsPropNodeInitialPosition, x, 0);
    args.setProperty<double>(kCreateNodeArgsPropNodeInitialPosition, y + 100., 1);
    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    args.setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
    args.addParamDefaultValue<std::string>(kUSDEditTransformParamPrimPattern, joinPaths(paths));
    args.addParamDefaultValueN<double>(kSceneParamTranslate, std::vector<double> { values.translate[0], values.translate[1], values.translate[2] });
    args.addParamDefaultValueN<double>(kSceneParamRotate, std::vector<double> { values.rotate[0], values.rotate[1], values.rotate[2] });
    args.addParamDefaultValueN<double>(kSceneParamScale, std::vector<double> { values.scale[0], values.scale[1], values.scale[2] });
    args.addParamDefaultValueN<double>(kUSDEditTransformParamPivot, std::vector<double> { base.pivot[0], base.pivot[1], base.pivot[2] });
    NodePtr node = app->createNode(args);
    if (!node) {
        return NodePtr();
    }

    // Takes viewed's place in every input it fed, the viewer's included.
    std::vector<ReconnectInputsCommand::InputChange> changes;
    for (const NodeWPtr& outputW : viewed->getOutputs()) {
        NodePtr output = outputW.lock();
        if (!output || (output == node)) {
            continue;
        }
        const int inputs = output->getNInputs();
        for (int i = 0; i < inputs; ++i) {
            if (output->getInput(i) == viewed) {
                changes.push_back(ReconnectInputsCommand::InputChange { output, i, viewed, node });
            }
        }
    }
    changes.push_back(ReconnectInputsCommand::InputChange { node, 0, NodeWPtr(), viewed });
    for (const ReconnectInputsCommand::InputChange& change : changes) {
        ignore_result(change.node.lock()->replaceInput(change.after.lock(), change.inputNb));
    }
    NodeCollectionPtr group = viewed->getGroup();
    NodeGraph* graph = group ? dynamic_cast<NodeGraph*>(group->getNodeGraph()) : nullptr;
    if (graph) {
        UndoScope scope;
        scope.touchNode(node);
        scope.touchNode(viewed);
        // The connections are made, so the command's first redo does nothing.
        graph->pushUndoCommand(new ReconnectInputsCommand(graph, changes), scope);
    }

    return node;
} // createEditNode

NodePtr
findOwner(const NodePtr& node,
          const std::string& primPath,
          std::set<Node*>* visited)
{
    if (!node || primPath.empty() || !visited->insert(node.get()).second) {
        return NodePtr();
    }
    std::string inputPath = primPath;
    if (!node->isNodeDisabled()) {
        TransformKnobs knobs;
        const bool hasTransform = getTransformKnobs(node, &knobs) && !toEditTransform(node);
        const std::string own = primPathOf(node);
        if (hasTransform && (own == primPath)) {
            return node;
        }
        if (dynamic_cast<SceneReparentNode*>(node->getEffectInstance().get())) {
            // A reparenting node grafts its inputs' root prims under its own prim; grafts renamed
            // by Auto Rename are not followed.
            if (own.empty() || (primPath.size() <= own.size() + 1) || (primPath.compare(0, own.size(), own) != 0) || (primPath[own.size()] != '/')) {
                return NodePtr();
            }
            inputPath = primPath.substr(own.size());
        }
    }
    const int inputs = node->getNInputs();
    for (int i = 0; i < inputs; ++i) {
        NodePtr found = findOwner(node->getInput(i), inputPath, visited);
        if (found) {
            return found;
        }
    }

    return NodePtr();
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

SceneKnobEdit
makeSceneKnobEdit(const KnobDoublePtr& knob,
                  int dimension,
                  double frame,
                  double value)
{
    SceneKnobEdit edit;

    edit.knob = knob;
    edit.dimension = dimension;
    edit.oldValue = knob->getValueAtTime(frame, dimension);
    edit.newValue = value;
    edit.animated = knob->isAnimated(dimension);
    edit.hadKey = edit.animated && (knob->getKeyFrameIndex(ViewSpec::current(), dimension, frame) >= 0);

    return edit;
}

void
applySceneKnobEdits(const NodePtr& node,
                    double frame,
                    const std::vector<SceneKnobEdit>& edits,
                    UndoHistory* history,
                    const QString& text)
{
    applyEdits(node, frame, edits, std::vector<SceneStringEdit>(), history, text);
}

NodePtr
findSceneTransformOwner(const NodePtr& viewed,
                        const std::string& primPath)
{
    std::set<Node*> visited;

    return findOwner(viewed, primPath, &visited);
}

SceneTransformPolicyTarget::SceneTransformPolicyTarget(const Context& context)
    : _context(context)
{
}

bool
SceneTransformPolicyTarget::commit(const SceneTransformDrag& drag)
{
    NodePtr viewed = _context.viewedNode ? _context.viewedNode() : NodePtr();

    if (!viewed || drag.prims.empty()) {
        return false;
    }
    UndoHistory* history = _context.undoHistory ? _context.undoHistory() : nullptr;
    const Settings::Viewer3DTransformTargetEnum policy = appPTR->getCurrentSettings()->getViewer3DTransformTarget();
    const GfMatrix4d delta = SceneTransformWidget::loadMatrix(drag.delta);
    const GfVec3d pivot(drag.pivot[0], drag.pivot[1], drag.pivot[2]);
    const QString text = QObject::tr("Move %1").arg(QString::fromStdString(drag.prims.back().path));

    if (history) {
        history->openStep(text);
    }
    bool written = false;
    std::vector<const SceneTransformPrim*> rest;
    for (const SceneTransformPrim& prim : drag.prims) {
        rest.push_back(&prim);
    }

    switch (policy) {
    case Settings::eViewer3DTransformTargetOwner: {
        std::vector<const SceneTransformPrim*> unowned;
        for (const SceneTransformPrim* prim : rest) {
            NodePtr owner = findSceneTransformOwner(viewed, prim->path);
            const SceneTransformOp* op = owner ? findOp(*prim, getOwnerOpName(owner)) : nullptr;
            std::vector<SceneKnobEdit> edits;
            if (op && makeOpEdits(owner, SceneTransformWidget::loadMatrix(op->space), delta, drag.frame, &edits)) {
                applySceneKnobEdits(owner, drag.frame, edits, history, text);
                written = written || !edits.empty();
            } else {
                unowned.push_back(prim);
            }
        }
        rest.clear();
        // Then the open USDEditTransform, for the prims it already moves.
        NodePtr open = _context.openEditNode ? _context.openEditNode() : NodePtr();
        if (USDEditTransform* edit = toEditTransform(open)) {
            std::vector<std::string> matched, opNames;
            edit->getMatchedPrims(&matched, &opNames);
            std::vector<const SceneTransformPrim*> targeted;
            for (const SceneTransformPrim* prim : unowned) {
                if (std::find(matched.begin(), matched.end(), prim->path) != matched.end()) {
                    targeted.push_back(prim);
                } else {
                    rest.push_back(prim);
                }
            }
            if (!targeted.empty() && editExistingNode(open, targeted, delta, drag.frame, history, text)) {
                written = true;
            }
        } else {
            rest = unowned;
        }
        break;
    }
    case Settings::eViewer3DTransformTargetOpenEdit: {
        NodePtr open = _context.openEditNode ? _context.openEditNode() : NodePtr();
        if (toEditTransform(open)) {
            written = editExistingNode(open, rest, delta, drag.frame, history, text);
            rest.clear();
        }
        break;
    }
    case Settings::eViewer3DTransformTargetAlwaysEdit: {
        if (USDEditTransform* edit = toEditTransform(viewed)) {
            std::vector<std::string> matched, opNames;
            edit->getMatchedPrims(&matched, &opNames);
            std::set<std::string> selected;
            for (const SceneTransformPrim* prim : rest) {
                selected.insert(prim->path);
            }
            if (selected == std::set<std::string>(matched.begin(), matched.end())) {
                written = editExistingNode(viewed, rest, delta, drag.frame, history, text);
                rest.clear();
            }
        }
        break;
    }
    }

    if (!rest.empty() && createEditNode(viewed, rest, delta, pivot)) {
        written = true;
    }
    if (history) {
        history->closeStep();
    }

    return written;
} // SceneTransformPolicyTarget::commit

NATRON_NAMESPACE_EXIT
