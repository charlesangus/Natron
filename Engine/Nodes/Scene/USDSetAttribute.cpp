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

#include "USDSetAttribute.h"

#include <string>
#include <vector>

#include <QCoreApplication>
#include <QString>
#include <QStringList>
#include <QtConcurrentRun> // QtCore on Qt4, QtConcurrent on Qt5

#include "Engine/ChoiceOption.h"
#include "Engine/ContentHasher.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/SchemaKnobMapper.h"
#include "Engine/Nodes/Scene/USDSetAttributeLookup.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"
#include "Serialization/KnobSerialization.h"
#include "Serialization/NodeSerialization.h"

NATRON_NAMESPACE_ENTER

namespace {
const char* const kDefaultValueType = "float";

const std::string kPrimvarsPrefix("primvars:");

template <typename T>
bool
wrapInArray(const PXR_NS::VtValue& value,
            PXR_NS::VtValue* array)
{
    if (!value.IsHolding<T>()) {
        return false;
    }
    *array = PXR_NS::VtValue(PXR_NS::VtArray<T>(1, value.UncheckedGet<T>()));

    return true;
}

// value as a one-element array of its type, or empty for a type kValueTypes lacks.
PXR_NS::VtValue
toOneElementArray(const PXR_NS::VtValue& value)
{
    PXR_NS::VtValue array;

    (void)(wrapInArray<bool>(value, &array) || wrapInArray<int>(value, &array) || wrapInArray<unsigned int>(value, &array) || wrapInArray<PXR_NS::GfHalf>(value, &array) || wrapInArray<float>(value, &array) || wrapInArray<double>(value, &array) || wrapInArray<PXR_NS::GfVec2f>(value, &array) || wrapInArray<PXR_NS::GfVec3f>(value, &array) || wrapInArray<PXR_NS::GfVec4f>(value, &array) || wrapInArray<PXR_NS::GfVec2d>(value, &array) || wrapInArray<PXR_NS::GfVec3d>(value, &array) || wrapInArray<PXR_NS::GfVec4d>(value, &array) || wrapInArray<std::string>(value, &array) || wrapInArray<PXR_NS::TfToken>(value, &array) || wrapInArray<PXR_NS::SdfAssetPath>(value, &array));

    return array;
}

// What the node authors on one matched prim.
struct Target {
    PXR_NS::SdfPath path;
    // False for an attribute the prim's schemas define.
    bool custom = true;
    bool uniform = false;
};
} // anonymous namespace

USDSetAttribute::USDSetAttribute(NodePtr node)
    : NativeEffectBase(node)
    , _guiRefreshPending(false)
    , _lookupGeneration(0)
{
}

USDSetAttribute::~USDSetAttribute()
{
}

const NodeSpec*
USDSetAttribute::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

std::string
USDSetAttribute::getBuiltType() const
{
    std::lock_guard<std::mutex> l(_lock);

    return _builtType;
}

void
USDSetAttribute::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));
    _page = page;

    _primPattern.create(this, page, kUSDSetAttributeParamPrimPattern, tr("Prim Pattern"), tr("The prims to set the attribute on."));

    KnobStringPtr attributeName = createKnob<KnobString>(tr("Attribute Name"));
    attributeName->setName(kUSDSetAttributeParamAttributeName);
    attributeName->setHintToolTip(tr("The attribute's name, namespaces separated by colons (userProperties:shotName). As "
                                     "a primvar, the name after primvars:."));
    attributeName->setAnimationEnabled(false);
    page->addKnob(attributeName);
    _attributeName = attributeName;

    KnobChoicePtr valueType = createKnob<KnobChoice>(tr("Value Type"));
    valueType->setName(kUSDSetAttributeParamValueType);
    valueType->setHintToolTip(tr("The attribute's USD value type. A schema attribute must be set with the type the "
                                 "schema gives it."));
    {
        std::vector<ChoiceOption> options;
        for (const std::string& type : USDSetAttributeLookup::getValueTypes()) {
            options.push_back(ChoiceOption(type, type, ""));
        }
        valueType->populateChoices(options);
    }
    valueType->setAnimationEnabled(false);
    page->addKnob(valueType);
    valueType->setDefaultValueFromID(kDefaultValueType);
    _valueType = valueType;

    KnobBoolPtr primvar = createKnob<KnobBool>(tr("Primvar"));
    primvar->setName(kUSDSetAttributeParamPrimvar);
    primvar->setHintToolTip(tr("Sets the primvar of that name instead: primvars:<name>, with constant interpolation, holding "
                               "the value as a one-element array of the type (color3f[] for color3f), as schema primvars "
                               "such as displayColor are."));
    primvar->setDefaultValue(false);
    primvar->setAnimationEnabled(false);
    page->addKnob(primvar);
    _primvar = primvar;

    rebuildValueKnob();
} // USDSetAttribute::initializeKnobs

std::string
USDSetAttribute::getSelectedType() const
{
    KnobChoicePtr valueType = _valueType.lock();
    const std::string selected = valueType ? valueType->getActiveEntry().id : std::string();

    return selected.empty() ? std::string(kDefaultValueType) : selected;
}

void
USDSetAttribute::rebuildValueKnob()
{
    const std::string selected = getSelectedType();
    KnobIPtr old;
    {
        std::lock_guard<std::mutex> l(_lock);
        if (_value && (selected == _builtType)) {
            return;
        }
        old = _value;
    }
    SchemaKnobMapper::AttributeKnob description;
    KnobPagePtr page = _page.lock();
    if (!page || !SchemaKnobMapper::describeValueType(selected, &description)) {
        return;
    }
    if (old) {
        deleteKnob(old.get(), false);
    }
    KnobIPtr value = SchemaKnobMapper::createKnob(this, description, kUSDSetAttributeParamValue);
    if (!value) {
        return;
    }
    value->setHintToolTip(tr("The value to set, of the Value Type.").toStdString());
    page->addKnob(value);
    {
        std::lock_guard<std::mutex> l(_lock);
        _builtType = description.typeName;
        _value = value;
    }

    // Deferred: the knob that triggered the rebuild may be the one whose GUI is being edited.
    if (old && qApp && !_guiRefreshPending.exchange(true)) {
        NodeWPtr weakNode = getNode();
        QMetaObject::invokeMethod(qApp, [weakNode]() {
            NodePtr node = weakNode.lock();
            USDSetAttribute* effect = node ? dynamic_cast<USDSetAttribute*>( node->getEffectInstance().get() ) : NULL;
            if (!effect) {
                return;
            }
            effect->_guiRefreshPending.store(false);
            effect->recreateKnobs(true); }, Qt::QueuedConnection);
    }
} // USDSetAttribute::rebuildValueKnob

bool
USDSetAttribute::knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec /*view*/,
                             double /*time*/,
                             bool /*originatedFromMainThread*/)
{
    KnobChoicePtr valueType = _valueType.lock();

    if (valueType && (k == valueType.get())) {
        rebuildValueKnob();

        return true;
    }
    const bool edited = (reason == eValueChangedReasonUserEdited) || (reason == eValueChangedReasonNatronGuiEdited) || (reason == eValueChangedReasonNatronInternalEdited);
    if (edited && ((k == _attributeName.lock().get()) || (k == _primvar.lock().get()) || (k == _primPattern.getKnob().get()))) {
        startTypeLookup();

        return true;
    }

    return false;
}

// Builds the saved type's Value knob before the saved values load, so the saved value lands in it.
void
USDSetAttribute::onKnobsAboutToBeLoaded(const Serialization::NodeSerializationPtr& serialization)
{
    KnobChoicePtr valueType = _valueType.lock();

    if (!serialization || !valueType) {
        return;
    }
    for (const Serialization::KnobSerialization& saved : serialization->knobs) {
        if (saved.name != kUSDSetAttributeParamValueType) {
            continue;
        }
        // A project file saves a choice by its option id, which completing the serialization reads.
        const Serialization::KnobSerialization knob = completeKnobSerializationForKnob(*valueType, saved);
        if (knob.choiceOptionID && !knob.choiceOptionID->empty()) {
            valueType->setValueFromID(*knob.choiceOptionID, 0);
            rebuildValueKnob();
            break;
        }
    }
}

void
USDSetAttribute::onKnobsLoaded()
{
    rebuildValueKnob();
}

void
USDSetAttribute::onInputChanged(int inputNo)
{
    NativeEffectBase::onInputChanged(inputNo);
    startTypeLookup();
}

void
USDSetAttribute::startTypeLookup()
{
    AppInstancePtr app = getApp();
    KnobStringPtr nameKnob = _attributeName.lock();
    KnobBoolPtr primvarKnob = _primvar.lock();
    // A saved type must survive the load, which connects inputs and sets every knob.
    if (!app || !app->getProject() || app->getProject()->isLoadingProject() || !nameKnob || !primvarKnob) {
        return;
    }
    const int generation = ++_lookupGeneration;
    const std::string pattern = _primPattern.getExpression();
    const std::string name = nameKnob->getValue();
    const bool primvar = primvarKnob->getValue();
    EffectInstancePtr input = getInput(0);
    if (!input || PrimSelector::isBlank(pattern) || name.empty()) {
        return;
    }
    const double time = getCurrentTime();
    NodeWPtr weakNode = getNode();

    QtConcurrent::run([input, weakNode, generation, pattern, name, primvar, time]() {
        USDSetAttributeLookup::Result result;
        {
            ScenePayload scene;
            std::string error;
            if ((input->evaluateScene(EvaluateSceneArgs(time, ViewIdx(0)), &scene, &error) != eRenderRoIRetCodeOk) || scene.isEmpty()) {
                return;
            }
            SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(scene, &error);
            if (!USDSetAttributeLookup::lookupExistingAttribute(lease.getStage(), pattern, name, primvar, &result, &error)) {
                return;
            }
        }
        if (!result.found || result.unsupported || !qApp) {
            return;
        }
        QMetaObject::invokeMethod(qApp, [weakNode, generation, result]() {
            NodePtr node = weakNode.lock();
            USDSetAttribute* effect = node ? dynamic_cast<USDSetAttribute*>( node->getEffectInstance().get() ) : NULL;
            KnobChoicePtr valueType = effect ? effect->_valueType.lock() : KnobChoicePtr();
            if (!valueType || (effect->_lookupGeneration.load() != generation)) {
                return;
            }
            valueType->setValueFromID(result.typeName, 0);
            effect->rebuildValueKnob(); }, Qt::QueuedConnection);
    });
}

StatusEnum
USDSetAttribute::renderScene(const SceneRenderActionArgs& args,
                             ScenePayload* outputScene,
                             std::string* errorMessage)
{
    const ScenePayload upstream = args.getInputScene(0);
    const std::string pattern = _primPattern.getExpression();
    KnobStringPtr nameKnob = _attributeName.lock();
    KnobBoolPtr primvarKnob = _primvar.lock();
    std::string name = nameKnob ? QString::fromStdString(nameKnob->getValue()).trimmed().toStdString() : std::string();
    const bool primvar = primvarKnob && primvarKnob->getValue();
    SchemaKnobMapper::AttributeKnob description;
    KnobIPtr value;
    {
        std::lock_guard<std::mutex> l(_lock);
        value = _value;
        if (!SchemaKnobMapper::describeValueType(_builtType, &description)) {
            value.reset();
        }
    }

    if (!PrimSelector::validate(pattern, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }
    if (upstream.isEmpty() || PrimSelector::isBlank(pattern) || name.empty() || !value) {
        *outputScene = upstream;
        clearPersistentMessage(false);

        return eStatusOK;
    }
    if (primvar && (name.compare(0, kPrimvarsPrefix.size(), kPrimvarsPrefix) == 0)) {
        name = name.substr(kPrimvarsPrefix.size());
    }
    const std::string attributeName = primvar ? (kPrimvarsPrefix + name) : name;
    if (name.empty() || !PXR_NS::SdfPath::IsValidNamespacedIdentifier(attributeName)) {
        *errorMessage = tr("\"%1\" is not an attribute name: use letters, digits and underscores, namespaces separated "
                           "by colons.")
                            .arg(QString::fromStdString(attributeName))
                            .toStdString();
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }
    const PXR_NS::SdfValueTypeName scalarType = SchemaKnobMapper::getValueTypeName(description);
    const PXR_NS::SdfValueTypeName authoredType = primvar ? scalarType.GetArrayType() : scalarType;

    std::vector<Target> targets;
    USDSetAttributeLookup::Result existing;
    {
        SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(upstream, errorMessage);
        PXR_NS::UsdStageRefPtr stage = lease.getStage();
        if (!stage) {
            if (errorMessage->empty()) {
                *errorMessage = tr("USD could not compose the input scene.").toStdString();
            }
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
        std::vector<PXR_NS::SdfPath> matches;
        if (!SceneOps::selectPrims(stage, pattern, &matches, errorMessage)) {
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
        const PXR_NS::TfToken token(attributeName);
        for (const PXR_NS::SdfPath& path : matches) {
            Target target;
            target.path = path;
            const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(path);
            const PXR_NS::UsdPrimDefinition::Attribute defined = prim ? prim.GetPrimDefinition().GetAttributeDefinition(token) : PXR_NS::UsdPrimDefinition::Attribute();
            if (defined) {
                // An opinion of another type than the schema's would not compose as the schema's attribute.
                if (defined.GetTypeName() != authoredType) {
                    *errorMessage = tr("%1 on %2 is a %3 attribute of its schema, not a %4 one.")
                                        .arg(QString::fromStdString(attributeName))
                                        .arg(QString::fromStdString(path.GetString()))
                                        .arg(QString::fromStdString(defined.GetTypeName().GetAsToken().GetString()))
                                        .arg(QString::fromStdString(authoredType.GetAsToken().GetString()))
                                        .toStdString();
                    setPersistentMessage(eMessageTypeError, *errorMessage);

                    return eStatusFailed;
                }
                target.custom = false;
                target.uniform = (defined.GetVariability() == PXR_NS::SdfVariabilityUniform);
            }
            targets.push_back(target);
        }
        existing = USDSetAttributeLookup::lookup(stage, matches, name, primvar);
    }
    QStringList warnings;
    if (existing.found) {
        const QString attributeText = QString::fromStdString(attributeName);
        const QString chosenText = QString::fromStdString(scalarType.GetAsToken().GetString());
        const QString existingType = QString::fromStdString(existing.typeName);
        const QString primText = QString::fromStdString(existing.primPath);
        if (existing.unsupported) {
            warnings += tr("%1 on %2 is a %3 attribute; %3 is not supported.").arg(attributeText).arg(primText).arg(existingType);
        } else if (existingType != chosenText) {
            warnings += tr("%1 on %2 is a %3 attribute; authoring %4 over it").arg(attributeText).arg(primText).arg(existingType).arg(chosenText);
        }
        if (existing.conflict) {
            warnings += tr("%1 has different types on the matched prims: %2 on %3, %4 on %5.")
                            .arg(attributeText)
                            .arg(existingType)
                            .arg(primText)
                            .arg(QString::fromStdString(existing.conflictTypeName))
                            .arg(QString::fromStdString(existing.conflictPath));
        }
    }
    if (targets.empty()) {
        *outputScene = upstream;
        setPersistentMessage(eMessageTypeWarning, tr("The prim pattern \"%1\" matches no prim; the scene is passed through "
                                                     "unchanged.")
                                                      .arg(QString::fromStdString(pattern))
                                                      .toStdString());

        return eStatusOK;
    }

    const bool animated = SceneNodeAuthoring::isAnimated(value);
    ContentHasher facts;
    SceneNodeAuthoring::appendUpstreamMetadata(upstream, &facts);
    facts.appendU64(targets.size());
    for (const Target& target : targets) {
        facts.appendString(target.path.GetString());
        facts.appendBool(target.custom);
        facts.appendBool(target.uniform);
    }
    SceneLayerMemo::AuthorFunc author = [targets, name, attributeName, primvar, authoredType, description, value, animated](const SceneLayerEdit& edit,
                                                                                                                            std::string* error) {
        PXR_NS::UsdStageRefPtr stage = SceneNodeAuthoring::openLayerStage(edit, error);
        if (!stage) {
            if (error->empty()) {
                *error = tr("USD could not open the node's layer.").toStdString();
            }

            return false;
        }
        const auto valueAt = [&description, &value, primvar](double frame) {
            const PXR_NS::VtValue scalar = SchemaKnobMapper::getValue(description, value, frame);

            return primvar ? toOneElementArray(scalar) : scalar;
        };
        for (const Target& target : targets) {
            const PXR_NS::UsdPrim prim = stage->OverridePrim(target.path);
            PXR_NS::UsdAttribute attribute;
            if (prim && primvar) {
                attribute = PXR_NS::UsdGeomPrimvarsAPI(prim).CreatePrimvar(PXR_NS::TfToken(name), authoredType, PXR_NS::UsdGeomTokens->constant).GetAttr();
            } else if (prim) {
                attribute = prim.CreateAttribute(PXR_NS::TfToken(attributeName), authoredType, target.custom,
                                                 target.uniform ? PXR_NS::SdfVariabilityUniform : PXR_NS::SdfVariabilityVarying);
            }
            if (!SceneNodeAuthoring::authorValue(attribute, edit, animated && !target.uniform, valueAt)) {
                *error = tr("USD could not author %1 on %2.")
                             .arg(QString::fromStdString(attributeName))
                             .arg(QString::fromStdString(target.path.GetString()))
                             .toStdString();

                return false;
            }
        }

        return true;
    };

    // Of the input, the layer reads only the metadata, the prims it edits and their schemas' say on
    // the attribute.
    SceneLayerReads reads;
    reads.upstream = SceneLayerMemo::Key::eUpstreamReadFacts;
    reads.factsDigest = facts.finalize();
    reads.frameRange = animated;
    if (!emitSceneLayer(upstream, reads, author, outputScene, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }
    if (warnings.isEmpty()) {
        clearPersistentMessage(false);
    } else {
        setPersistentMessage(eMessageTypeWarning, warnings.join(QLatin1Char('\n')).toStdString());
    }

    return eStatusOK;
} // USDSetAttribute::renderScene

NATRON_NAMESPACE_EXIT
