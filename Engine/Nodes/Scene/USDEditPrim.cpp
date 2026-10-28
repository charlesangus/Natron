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

#include "USDEditPrim.h"

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QString>

#include "Engine/ChoiceOption.h"
#include "Engine/ContentHasher.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/SchemaKnobMapper.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Serialization/KnobSerialization.h"
#include "Serialization/NodeSerialization.h"

NATRON_NAMESPACE_ENTER

namespace {
struct SchemaAttribute {
    SchemaKnobMapper::AttributeKnob description;
    KnobIWPtr knob;
};

// An attribute the node authors, with its knob held for the render.
struct AuthoredAttribute {
    SchemaKnobMapper::AttributeKnob description;
    KnobIPtr knob;
    bool animated = false;
};

bool
isApiSchema(const SchemaKnobMapper::SchemaEntry& entry)
{
    return (entry.kind == SchemaKnobMapper::eSchemaKindSingleApplyAPI) || (entry.kind == SchemaKnobMapper::eSchemaKindMultipleApplyAPI);
}

// Applies the schema to prim when it is an API schema, then authors attributes on it.
bool
authorPrim(const PXR_NS::UsdPrim& prim,
           const SchemaKnobMapper::SchemaEntry& schema,
           const std::string& instanceName,
           const std::vector<AuthoredAttribute>& attributes,
           const SceneLayerEdit& edit,
           std::string* errorMessage)
{
    const QString primPath = QString::fromStdString(prim.GetPath().GetString());

    if (isApiSchema(schema)) {
        const PXR_NS::TfToken identifier(schema.identifier);
        const bool applied = (schema.kind == SchemaKnobMapper::eSchemaKindMultipleApplyAPI) ? prim.ApplyAPI(identifier, PXR_NS::TfToken(instanceName)) : prim.ApplyAPI(identifier);
        if (!applied) {
            *errorMessage = EffectInstance::tr("USD could not apply %1 to %2.").arg(QString::fromStdString(schema.identifier)).arg(primPath).toStdString();

            return false;
        }
    }
    for (const AuthoredAttribute& attribute : attributes) {
        const std::string name = SchemaKnobMapper::getAttributeName(attribute.description, instanceName);
        const PXR_NS::SdfVariability variability = attribute.description.uniform ? PXR_NS::SdfVariabilityUniform : PXR_NS::SdfVariabilityVarying;
        const PXR_NS::UsdAttribute usdAttribute = prim.CreateAttribute(PXR_NS::TfToken(name), SchemaKnobMapper::getValueTypeName(attribute.description), false, variability);
        const SchemaKnobMapper::AttributeKnob& description = attribute.description;
        const KnobIPtr& knob = attribute.knob;
        const bool ok = SceneNodeAuthoring::authorValue(usdAttribute, edit, attribute.animated, [&description, &knob](double frame) {
            return SchemaKnobMapper::getValue(description, knob, frame);
        });
        if (!ok) {
            *errorMessage = EffectInstance::tr("USD could not author %1 on %2.").arg(QString::fromStdString(name)).arg(primPath).toStdString();

            return false;
        }
    }

    return true;
}
} // anonymous namespace

struct USDEditPrim::Implementation {
    PrimSelector primPattern;
    KnobChoiceWPtr mode;
    KnobStringWPtr primPath;
    KnobChoiceWPtr schema;
    KnobStringWPtr instanceName;
    KnobStringWPtr unsupported;
    KnobPageWPtr attributesPage;
    AuthoredKnobSet authored;
    std::atomic<bool> guiRefreshPending { false };

    // What the attribute knobs were built from; written on the main thread, read by renders.
    mutable std::mutex lock;
    std::string builtSchema;
    SchemaKnobMapper::SchemaEntry builtEntry;
    std::vector<SchemaAttribute> attributes;
};

USDEditPrim::USDEditPrim(NodePtr node)
    : NativeEffectBase(node)
    , _imp(new Implementation)
{
}

USDEditPrim::~USDEditPrim()
{
}

const NodeSpec*
USDEditPrim::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

std::string
USDEditPrim::getBuiltSchema() const
{
    std::lock_guard<std::mutex> l(_imp->lock);

    return _imp->builtSchema;
}

void
USDEditPrim::onScriptNameChanged(const std::string& /*fullyQualifiedName*/)
{
    followSceneNodeRename(_imp->primPath.lock(), getSceneNodeDefaultPrimPath(this, "Prim"));
}

void
USDEditPrim::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobChoicePtr mode = createKnob<KnobChoice>(tr("Mode"));
    mode->setName(kUSDEditPrimParamMode);
    mode->setHintToolTip(tr("Whether the node edits prims of the input scene or defines one."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("edit", tr("Edit").toStdString(),
                                       tr("Overrides the prims the prim pattern selects in the input scene.").toStdString()));
        options.push_back(ChoiceOption("define", tr("Define").toStdString(),
                                       tr("Defines the prim at the prim path, of the schema's type, and its missing ancestors as "
                                          "Xforms.")
                                           .toStdString()));
        mode->populateChoices(options);
    }
    mode->setDefaultValue((int)eModeEdit);
    mode->setAnimationEnabled(false);
    page->addKnob(mode);
    _imp->mode = mode;

    _imp->primPattern.create(this, page, kUSDEditPrimParamPrimPattern, tr("Prim Pattern"), tr("The prims to edit."));
    _imp->primPath = createScenePrimPathKnob(this, page, getSceneNodeDefaultPrimPath(this, "Prim"), tr("The path of the prim the node defines."));

    KnobChoicePtr schema = createKnob<KnobChoice>(tr("Schema"));
    schema->setName(kUSDEditPrimParamSchema);
    schema->setHintToolTip(tr("The USD schema whose attributes the node sets, by the module that provides it. A typed schema "
                              "(Camera, Mesh, SphereLight) is a prim's type, which Define gives the prim; an API schema "
                              "(CollectionAPI, LightAPI) is applied to every prim the node authors."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kUSDEditPrimSchemaNone, tr("None").toStdString(),
                                       tr("No schema: Define defines a typeless prim.").toStdString()));
        for (const SchemaKnobMapper::SchemaEntry& entry : SchemaKnobMapper::listSchemas()) {
            options.push_back(ChoiceOption(entry.identifier, entry.group + "/" + entry.identifier, std::string()));
        }
        schema->populateChoices(options);
    }
    schema->setCascading(true);
    schema->setDefaultValue(0);
    schema->setAnimationEnabled(false);
    page->addKnob(schema);
    _imp->schema = schema;

    KnobStringPtr instanceName = createKnob<KnobString>(tr("Instance Name"));
    instanceName->setName(kUSDEditPrimParamInstanceName);
    instanceName->setHintToolTip(tr("For a schema applied several times to a prim (CollectionAPI), the name of the instance the "
                                    "node applies and sets, as in collection:<name>:includeRoot."));
    instanceName->setAnimationEnabled(false);
    page->addKnob(instanceName);
    _imp->instanceName = instanceName;

    KnobStringPtr unsupported = createKnob<KnobString>(tr("Unsupported Attributes"));
    unsupported->setName(kUSDEditPrimParamUnsupported);
    unsupported->setHintToolTip(tr("The schema's attributes and relationships no knob can set: arrays, matrices and the like."));
    unsupported->setAnimationEnabled(false);
    unsupported->setAsLabel();
    unsupported->setEvaluateOnChange(false);
    unsupported->setIsPersistent(false);
    page->addKnob(unsupported);
    _imp->unsupported = unsupported;

    _imp->attributesPage = createKnob<KnobPage>(tr("Attributes"));

    refreshVisibility();
} // USDEditPrim::initializeKnobs

USDEditPrim::ModeEnum
USDEditPrim::getMode() const
{
    KnobChoicePtr mode = _imp->mode.lock();

    return (mode && (mode->getValue() == (int)eModeDefine)) ? eModeDefine : eModeEdit;
}

std::string
USDEditPrim::getSelectedSchema() const
{
    KnobChoicePtr schema = _imp->schema.lock();

    if (!schema) {
        return std::string();
    }
    const std::string id = schema->getActiveEntry().id;

    return (id == kUSDEditPrimSchemaNone) ? std::string() : id;
}

void
USDEditPrim::refreshVisibility()
{
    const bool define = (getMode() == eModeDefine);
    SchemaKnobMapper::SchemaEntry entry;
    {
        std::lock_guard<std::mutex> l(_imp->lock);
        entry = _imp->builtEntry;
    }
    const bool multipleApply = !getBuiltSchema().empty() && (entry.kind == SchemaKnobMapper::eSchemaKindMultipleApplyAPI);
    KnobStringPtr unsupported = _imp->unsupported.lock();

    setSceneKnobSecret(_imp->primPattern.getKnob(), define);
    setSceneKnobSecret(_imp->primPath.lock(), !define);
    setSceneKnobSecret(_imp->instanceName.lock(), !multipleApply);
    if (unsupported) {
        setSceneKnobSecret(unsupported, unsupported->getValue().empty());
    }
}

void
USDEditPrim::rebuildSchemaKnobs()
{
    const std::string selected = getSelectedSchema();

    if (selected == getBuiltSchema()) {
        return;
    }
    SchemaKnobMapper::SchemaDescription description;
    std::string error;
    if (!selected.empty() && !SchemaKnobMapper::describeSchema(selected, &description, &error)) {
        description = SchemaKnobMapper::SchemaDescription();
    }

    _imp->authored.deleteAll(this);
    KnobPagePtr page = _imp->attributesPage.lock();
    std::vector<SchemaAttribute> attributes;
    std::vector<std::string> skipped = description.unsupported;
    for (const SchemaKnobMapper::AttributeKnob& attribute : description.knobs) {
        std::string scriptName = SchemaKnobMapper::getKnobScriptName(attribute);
        if (getKnobByName(scriptName)) {
            scriptName = "usd_" + scriptName;
        }
        KnobIPtr knob = (page && !getKnobByName(scriptName)) ? SchemaKnobMapper::createKnob(this, attribute, scriptName) : KnobIPtr();
        if (!knob) {
            skipped.push_back(attribute.name);
            continue;
        }
        _imp->authored.add(this, page, knob);
        SchemaAttribute entry;
        entry.description = attribute;
        entry.knob = knob;
        attributes.push_back(entry);
    }
    {
        std::lock_guard<std::mutex> l(_imp->lock);
        _imp->builtSchema = selected;
        _imp->builtEntry = description.schema;
        _imp->attributes = attributes;
    }

    KnobStringPtr unsupported = _imp->unsupported.lock();
    if (unsupported) {
        std::string text;
        for (const std::string& name : skipped) {
            text += (text.empty() ? std::string() : std::string(", ")) + name;
        }
        if (!text.empty()) {
            text = tr("Not settable here: %1").arg(QString::fromStdString(text)).toStdString();
        }
        unsupported->setValue(text);
    }

    // Deferred: the knob that triggered the rebuild may be the one whose GUI is being edited.
    if (qApp && !_imp->guiRefreshPending.exchange(true)) {
        NodeWPtr weakNode = getNode();
        QMetaObject::invokeMethod(qApp, [weakNode]() {
            NodePtr node = weakNode.lock();
            USDEditPrim* effect = node ? dynamic_cast<USDEditPrim*>( node->getEffectInstance().get() ) : NULL;
            if (!effect) {
                return;
            }
            effect->_imp->guiRefreshPending.store(false);
            effect->recreateKnobs(true); }, Qt::QueuedConnection);
    }
} // USDEditPrim::rebuildSchemaKnobs

bool
USDEditPrim::knobChanged(KnobI* k,
                         ValueChangedReasonEnum reason,
                         ViewSpec /*view*/,
                         double /*time*/,
                         bool /*originatedFromMainThread*/)
{
    KnobChoicePtr schema = _imp->schema.lock();
    KnobChoicePtr mode = _imp->mode.lock();

    if (schema && (k == schema.get())) {
        rebuildSchemaKnobs();
        refreshVisibility();

        return true;
    }
    if (mode && (k == mode.get())) {
        refreshVisibility();

        return true;
    }

    return _imp->authored.onKnobChanged(k, reason);
}

// Builds the saved schema's knobs before the saved values load, so the node never rebuilds in the
// middle of the load.
void
USDEditPrim::onKnobsAboutToBeLoaded(const Serialization::NodeSerializationPtr& serialization)
{
    KnobChoicePtr schema = _imp->schema.lock();

    if (!serialization || !schema) {
        return;
    }
    for (const Serialization::KnobSerialization& knob : serialization->knobs) {
        if ((knob.name == kUSDEditPrimParamSchema) && knob.choiceOptionID) {
            schema->setValueFromID(*knob.choiceOptionID, 0);
            rebuildSchemaKnobs();
            refreshVisibility();
            break;
        }
    }
}

void
USDEditPrim::onKnobsLoaded()
{
    rebuildSchemaKnobs();
    refreshVisibility();
}

StatusEnum
USDEditPrim::renderScene(const SceneRenderActionArgs& args,
                         ScenePayload* outputScene,
                         std::string* errorMessage)
{
    const ScenePayload upstream = args.getInputScene(0);
    const ModeEnum mode = getMode();
    SchemaKnobMapper::SchemaEntry schema;
    std::vector<SchemaAttribute> built;
    bool hasSchema = false;
    {
        std::lock_guard<std::mutex> l(_imp->lock);
        hasSchema = !_imp->builtSchema.empty();
        schema = _imp->builtEntry;
        built = _imp->attributes;
    }
    const bool applies = hasSchema && isApiSchema(schema);
    KnobStringPtr instanceKnob = _imp->instanceName.lock();
    const std::string instanceName = (hasSchema && (schema.kind == SchemaKnobMapper::eSchemaKindMultipleApplyAPI) && instanceKnob) ? instanceKnob->getValue() : std::string();

    if (hasSchema && (schema.kind == SchemaKnobMapper::eSchemaKindMultipleApplyAPI) && !PXR_NS::SdfPath::IsValidIdentifier(instanceName)) {
        *errorMessage = tr("%1 is applied by instance: set the instance name to an identifier (letters, digits and "
                           "underscores).")
                            .arg(QString::fromStdString(schema.identifier))
                            .toStdString();
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }

    std::vector<AuthoredAttribute> attributes;
    {
        std::map<KnobI*, const SchemaAttribute*> byKnob;
        std::vector<KnobIPtr> held;
        for (const SchemaAttribute& attribute : built) {
            KnobIPtr knob = attribute.knob.lock();
            if (knob) {
                byKnob[knob.get()] = &attribute;
                held.push_back(knob);
            }
        }
        _imp->authored.forEachAuthored([&byKnob, &attributes](const KnobIPtr& knob) {
            std::map<KnobI*, const SchemaAttribute*>::const_iterator found = byKnob.find(knob.get());
            if (found == byKnob.end()) {
                return;
            }
            AuthoredAttribute attribute;
            attribute.description = found->second->description;
            attribute.knob = knob;
            attribute.animated = !attribute.description.uniform && SceneNodeAuthoring::isAnimated(knob);
            attributes.push_back(attribute);
        });
    }

    ContentHasher facts;
    SceneNodeAuthoring::appendUpstreamMetadata(upstream, &facts);
    SceneLayerMemo::AuthorFunc author;

    if (mode == eModeEdit) {
        const std::string pattern = _imp->primPattern.getExpression();
        if (!PrimSelector::validate(pattern, errorMessage)) {
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
        std::vector<PXR_NS::SdfPath> matches;
        if (!upstream.isEmpty() && !PrimSelector::isBlank(pattern) && (applies || !attributes.empty())) {
            SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(upstream, errorMessage);
            PXR_NS::UsdStageRefPtr stage = lease.getStage();
            if (!stage) {
                if (errorMessage->empty()) {
                    *errorMessage = tr("USD could not compose the input scene.").toStdString();
                }
                setPersistentMessage(eMessageTypeError, *errorMessage);

                return eStatusFailed;
            }
            if (!SceneOps::selectPrims(stage, pattern, &matches, errorMessage)) {
                setPersistentMessage(eMessageTypeError, *errorMessage);

                return eStatusFailed;
            }
            if (matches.empty()) {
                *outputScene = upstream;
                setPersistentMessage(eMessageTypeWarning, tr("The prim pattern \"%1\" matches no prim to edit; the scene is "
                                                             "passed through unchanged.")
                                                              .arg(QString::fromStdString(pattern))
                                                              .toStdString());

                return eStatusOK;
            }
        }
        if (matches.empty()) {
            *outputScene = upstream;
            clearPersistentMessage(false);

            return eStatusOK;
        }
        facts.appendU64(matches.size());
        for (const PXR_NS::SdfPath& path : matches) {
            facts.appendString(path.GetString());
        }
        author = [matches, schema, applies, instanceName, attributes](const SceneLayerEdit& edit,
                                                                      std::string* error) {
            PXR_NS::UsdStageRefPtr stage = SceneNodeAuthoring::openLayerStage(edit, error);
            if (!stage) {
                if (error->empty()) {
                    *error = tr("USD could not open the node's layer.").toStdString();
                }

                return false;
            }
            for (const PXR_NS::SdfPath& path : matches) {
                PXR_NS::UsdPrim prim = stage->OverridePrim(path);
                if (!prim) {
                    *error = tr("USD could not author over %1.").arg(QString::fromStdString(path.GetString())).toStdString();

                    return false;
                }
                if (!authorPrim(prim, applies ? schema : SchemaKnobMapper::SchemaEntry(), instanceName, attributes, edit, error)) {
                    return false;
                }
            }

            return true;
        };
    } else {
        PXR_NS::SdfPath path;
        if (!SceneNodeAuthoring::getPrimPath(_imp->primPath.lock(), &path, errorMessage)) {
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
        if (hasSchema && (schema.kind == SchemaKnobMapper::eSchemaKindAbstractTyped)) {
            *errorMessage = tr("%1 is an abstract schema, which no prim can have as its type: pick a concrete type or an "
                               "API schema to define a prim.")
                                .arg(QString::fromStdString(schema.identifier))
                                .toStdString();
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
        // The ancestors the input scene lacks are defined as Xforms; the others keep their type.
        std::vector<PXR_NS::SdfPath> ancestors;
        for (PXR_NS::SdfPath parent = path.GetParentPath(); !parent.IsEmpty() && !parent.IsAbsoluteRootPath(); parent = parent.GetParentPath()) {
            ancestors.insert(ancestors.begin(), parent);
        }
        std::vector<PXR_NS::SdfPath> missing = ancestors;
        if (!upstream.isEmpty() && !ancestors.empty()) {
            SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(upstream, errorMessage);
            PXR_NS::UsdStageRefPtr stage = lease.getStage();
            if (!stage) {
                if (errorMessage->empty()) {
                    *errorMessage = tr("USD could not compose the input scene.").toStdString();
                }
                setPersistentMessage(eMessageTypeError, *errorMessage);

                return eStatusFailed;
            }
            missing.clear();
            for (const PXR_NS::SdfPath& ancestor : ancestors) {
                if (!stage->GetPrimAtPath(ancestor)) {
                    missing.push_back(ancestor);
                }
            }
        }
        facts.appendU64(missing.size());
        for (const PXR_NS::SdfPath& ancestor : missing) {
            facts.appendString(ancestor.GetString());
        }
        const bool typed = hasSchema && (schema.kind == SchemaKnobMapper::eSchemaKindConcreteTyped);
        author = [path, missing, schema, typed, applies, instanceName, attributes](const SceneLayerEdit& edit,
                                                                                   std::string* error) {
            PXR_NS::UsdStageRefPtr stage = SceneNodeAuthoring::openLayerStage(edit, error);
            if (!stage) {
                if (error->empty()) {
                    *error = tr("USD could not open the node's layer.").toStdString();
                }

                return false;
            }
            // Ancestors the input has are left as overs, so the prim's type is the node's only
            // opinion on them.
            PXR_NS::SdfPrimSpecHandle spec = PXR_NS::SdfCreatePrimInLayer(edit.layer, path);
            if (!spec) {
                *error = tr("USD could not define %1.").arg(QString::fromStdString(path.GetString())).toStdString();

                return false;
            }
            spec->SetSpecifier(PXR_NS::SdfSpecifierDef);
            if (typed) {
                spec->SetTypeName(schema.identifier);
            }
            for (const PXR_NS::SdfPath& ancestor : missing) {
                PXR_NS::SdfPrimSpecHandle ancestorSpec = edit.layer->GetPrimAtPath(ancestor);
                if (ancestorSpec) {
                    ancestorSpec->SetSpecifier(PXR_NS::SdfSpecifierDef);
                    ancestorSpec->SetTypeName("Xform");
                }
            }
            PXR_NS::UsdPrim prim = stage->GetPrimAtPath(path);
            if (!prim) {
                *error = tr("USD could not define %1.").arg(QString::fromStdString(path.GetString())).toStdString();

                return false;
            }

            return authorPrim(prim, applies ? schema : SchemaKnobMapper::SchemaEntry(), instanceName, attributes, edit, error);
        };
    }

    // Of the input, the layer reads only the metadata and the prims it edits or the ancestors it
    // defines: an edit upstream that keeps them re-stacks the layer without authoring it again.
    SceneLayerReads reads;
    reads.upstream = SceneLayerMemo::Key::eUpstreamReadFacts;
    reads.factsDigest = facts.finalize();
    if (!emitSceneLayer(upstream, reads, author, outputScene, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }
    clearPersistentMessage(false);

    return eStatusOK;
} // USDEditPrim::renderScene

NATRON_NAMESPACE_EXIT
