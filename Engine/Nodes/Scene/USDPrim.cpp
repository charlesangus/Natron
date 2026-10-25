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

#include "USDPrim.h"

#include <string>
#include <vector>

#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

namespace {
enum KindEnum {
    eKindNone = 0,
    eKindGroup,
    eKindAssembly,
    eKindComponent,
    eKindSubcomponent
};
} // anonymous namespace

USDPrim::USDPrim(NodePtr node)
    : NativeEffectBase(node)
{
}

USDPrim::~USDPrim()
{
}

const NodeSpec*
USDPrim::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
USDPrim::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    _primPath = createScenePrimPathKnob(this, page, "/World", tr("The path of the prim the node authors."));

    KnobChoicePtr primType = createKnob<KnobChoice>(tr("Type"));
    primType->setName(kUSDPrimParamType);
    primType->setHintToolTip(tr("The prim's type: an Xform, which has a transform, or a Scope, which groups prims without one."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("Xform", "Xform", tr("A transformable prim.").toStdString()));
        options.push_back(ChoiceOption("Scope", "Scope", tr("A grouping prim with no transform.").toStdString()));
        primType->populateChoices(options);
    }
    primType->setDefaultValue((int)ePrimTypeXform);
    primType->setAnimationEnabled(false);
    page->addKnob(primType);
    _primType = primType;

    KnobChoicePtr kind = createKnob<KnobChoice>(tr("Kind"));
    kind->setName(kUSDPrimParamKind);
    kind->setHintToolTip(tr("The prim's model kind, which places it in the scene's model hierarchy. None authors no kind."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("none", tr("None").toStdString(), ""));
        options.push_back(ChoiceOption("group", "group", ""));
        options.push_back(ChoiceOption("assembly", "assembly", ""));
        options.push_back(ChoiceOption("component", "component", ""));
        options.push_back(ChoiceOption("subcomponent", "subcomponent", ""));
        kind->populateChoices(options);
    }
    kind->setDefaultValue((int)eKindNone);
    kind->setAnimationEnabled(false);
    page->addKnob(kind);
    _kind = kind;

    _transform.create(this, page);
} // USDPrim::initializeKnobs

void
USDPrim::refreshKnobVisibility()
{
    KnobChoicePtr primType = _primType.lock();

    _transform.setSecret(primType && (primType->getValue() == (int)ePrimTypeScope));
}

bool
USDPrim::knobChanged(KnobI* k,
                     ValueChangedReasonEnum /*reason*/,
                     ViewSpec /*view*/,
                     double /*time*/,
                     bool /*originatedFromMainThread*/)
{
    KnobChoicePtr primType = _primType.lock();

    if (!primType || (k != primType.get())) {
        return false;
    }
    refreshKnobVisibility();

    return true;
}

void
USDPrim::onKnobsLoaded()
{
    refreshKnobVisibility();
}

bool
USDPrim::authorLayer(const SceneLayerEdit& edit,
                     std::string* errorMessage) const
{
    PXR_NS::SdfPath path;

    if (!SceneNodeAuthoring::getPrimPath(_primPath.lock(), &path, errorMessage)) {
        return false;
    }
    PXR_NS::UsdStageRefPtr stage = SceneNodeAuthoring::openLayerStage(edit, errorMessage);
    if (!stage) {
        return false;
    }
    KnobChoicePtr primTypeKnob = _primType.lock();
    KnobChoicePtr kindKnob = _kind.lock();
    const int primType = primTypeKnob ? primTypeKnob->getValue() : (int)ePrimTypeXform;
    const int kind = kindKnob ? kindKnob->getValue() : (int)eKindNone;

    PXR_NS::UsdPrim prim;
    if (primType == (int)ePrimTypeScope) {
        prim = PXR_NS::UsdGeomScope::Define(stage, path).GetPrim();
    } else {
        PXR_NS::UsdGeomXform xform = PXR_NS::UsdGeomXform::Define(stage, path);
        if (xform && !SceneNodeAuthoring::authorTransform(_transform, xform, edit, errorMessage)) {
            return false;
        }
        prim = xform.GetPrim();
    }
    if (!prim) {
        *errorMessage = tr("USD could not define a prim at %1.").arg(QString::fromStdString(path.GetString())).toStdString();

        return false;
    }

    PXR_NS::TfToken kindToken;
    switch ((KindEnum)kind) {
    case eKindGroup:
        kindToken = PXR_NS::KindTokens->group;
        break;
    case eKindAssembly:
        kindToken = PXR_NS::KindTokens->assembly;
        break;
    case eKindComponent:
        kindToken = PXR_NS::KindTokens->component;
        break;
    case eKindSubcomponent:
        kindToken = PXR_NS::KindTokens->subcomponent;
        break;
    case eKindNone:
        break;
    }
    if (!kindToken.IsEmpty() && !PXR_NS::UsdModelAPI(prim).SetKind(kindToken)) {
        *errorMessage = tr("USD could not set the kind of %1.").arg(QString::fromStdString(path.GetString())).toStdString();

        return false;
    }

    return true;
} // USDPrim::authorLayer

StatusEnum
USDPrim::renderScene(const SceneRenderActionArgs& args,
                     ScenePayload* outputScene,
                     std::string* errorMessage)
{
    if (!emitSceneLayer(args.getInputScene(0), [this](const SceneLayerEdit& edit, std::string* error) { return authorLayer(edit, error); }, outputScene, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }
    clearPersistentMessage(false);

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
