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

#include "USDVariant.h"

#include <string>
#include <vector>

#include <QMetaObject>
#include <QString>
#include <QStringList>

#include "Engine/ChoiceOption.h"
#include "Engine/ContentHasher.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

namespace {
std::vector<ChoiceOption>
makeOptions(const std::vector<std::string>& names)
{
    std::vector<ChoiceOption> options;

    options.reserve(names.size());
    for (const std::string& name : names) {
        options.push_back(ChoiceOption(name, name, ""));
    }

    return options;
}

// Repopulates menu only when its entries change (repopulating moves the node's hash, which
// schedules another refresh), and points it at value when it lists it.
void
syncMenu(const KnobChoicePtr& menu,
         const std::vector<ChoiceOption>& options,
         const std::string& value)
{
    const std::vector<ChoiceOption> current = menu->getEntries_mt_safe();
    bool same = current.size() == options.size();

    for (std::size_t i = 0; same && (i < options.size()); ++i) {
        same = current[i].id == options[i].id;
    }
    if (!same) {
        menu->populateChoices(options);
    }
    for (std::size_t i = 0; i < options.size(); ++i) {
        if (options[i].id == value) {
            if (menu->getValue() != (int)i) {
                menu->setValue((int)i);
            }
            break;
        }
    }
}
} // anonymous namespace

USDVariant::USDVariant(NodePtr node)
    : NativeEffectBase(node)
    , _menuRefreshPending(false)
    , _syncingMenus(false)
{
}

USDVariant::~USDVariant()
{
}

const NodeSpec*
USDVariant::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
USDVariant::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    _primPattern.create(this, page, kUSDVariantParamPrimPattern, tr("Prim Pattern"), tr("The prims to select a variant on."));

    KnobStringPtr variantSet = createKnob<KnobString>(tr("Variant Set"));
    variantSet->setName(kUSDVariantParamVariantSet);
    variantSet->setHintToolTip(tr("The name of the variant set to select a variant of. Empty selects nothing."));
    variantSet->setAnimationEnabled(false);
    variantSet->setAddNewLine(false);
    page->addKnob(variantSet);
    _variantSet = variantSet;

    KnobChoicePtr variantSets = createKnob<KnobChoice>(tr("Variant Sets"));
    variantSets->setName(kUSDVariantParamVariantSets);
    variantSets->setHintToolTip(tr("The variant sets of the first prim the pattern matches; picking one sets Variant Set."));
    variantSets->setAnimationEnabled(false);
    variantSets->setNotifiesOnReselect(true);
    variantSets->setIsPersistent(false);
    // Only the name it sets decides the scene.
    variantSets->setEvaluateOnChange(false);
    page->addKnob(variantSets);
    _variantSets = variantSets;

    KnobStringPtr variant = createKnob<KnobString>(tr("Variant"));
    variant->setName(kUSDVariantParamVariant);
    variant->setHintToolTip(tr("The name of the variant to select. Empty selects nothing."));
    variant->setAnimationEnabled(false);
    variant->setAddNewLine(false);
    page->addKnob(variant);
    _variant = variant;

    KnobChoicePtr variants = createKnob<KnobChoice>(tr("Variants"));
    variants->setName(kUSDVariantParamVariants);
    variants->setHintToolTip(tr("The variants of the variant set on the first prim the pattern matches; picking one sets "
                                "Variant."));
    variants->setAnimationEnabled(false);
    variants->setNotifiesOnReselect(true);
    variants->setIsPersistent(false);
    variants->setEvaluateOnChange(false);
    page->addKnob(variants);
    _variants = variants;
} // USDVariant::initializeKnobs

bool
USDVariant::knobChanged(KnobI* k,
                        ValueChangedReasonEnum /*reason*/,
                        ViewSpec /*view*/,
                        double /*time*/,
                        bool /*originatedFromMainThread*/)
{
    KnobStringPtr variantSet = _variantSet.lock();
    KnobChoicePtr variantSets = _variantSets.lock();
    KnobStringPtr variant = _variant.lock();
    KnobChoicePtr variants = _variants.lock();
    KnobStringPtr pattern = _primPattern.getKnob();

    if (!variantSet || !variantSets || !variant || !variants) {
        return false;
    }
    if ((k == variantSets.get()) || (k == variants.get())) {
        KnobChoice* menu = static_cast<KnobChoice*>(k);
        const int index = menu->getValue();
        if (!_syncingMenus && (index >= 0) && (index < menu->getNumEntries())) {
            ((k == variantSets.get()) ? variantSet : variant)->setValue(menu->getEntry(index).id);
        }

        return true;
    }
    if ((k == variantSet.get()) || (k == variant.get()) || (pattern && (k == pattern.get()))) {
        scheduleMenuRefresh();

        return true;
    }

    return false;
}

void
USDVariant::onKnobsLoaded()
{
    NativeEffectBase::onKnobsLoaded();
    scheduleMenuRefresh();
}

void
USDVariant::onInputChanged(int inputNo)
{
    NativeEffectBase::onInputChanged(inputNo);
    scheduleMenuRefresh();
}

void
USDVariant::onNodeHashChanged(U64 hash)
{
    NativeEffectBase::onNodeHashChanged(hash);
    scheduleMenuRefresh();
}

void
USDVariant::scheduleMenuRefresh()
{
    if (_menuRefreshPending.exchange(true)) {
        return;
    }
    // The hooks that call this fire mid-recompute of a hash, before the upstream scene settled.
    QMetaObject::invokeMethod(this, [this]() {
        _menuRefreshPending = false;
        refreshVariantMenus(); }, Qt::QueuedConnection);
}

void
USDVariant::refreshVariantMenus()
{
    KnobStringPtr variantSet = _variantSet.lock();
    KnobChoicePtr variantSets = _variantSets.lock();
    KnobStringPtr variant = _variant.lock();
    KnobChoicePtr variants = _variants.lock();

    if (!variantSet || !variantSets || !variant || !variants) {
        return;
    }
    const std::string setName = variantSet->getValue();
    std::vector<std::string> setNames;
    std::vector<std::string> variantNames;
    EffectInstancePtr input = getInput(0);
    const std::string pattern = _primPattern.getExpression();
    ScenePayload scene;
    std::string error;
    if (input && !PrimSelector::isBlank(pattern) && PrimSelector::validate(pattern, &error) && (input->evaluateScene(EvaluateSceneArgs(getCurrentTime(), ViewIdx(0)), &scene, &error) == eRenderRoIRetCodeOk) && !scene.isEmpty()) {
        SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(scene, &error);
        std::vector<PXR_NS::SdfPath> matches;
        if (lease.isValid() && SceneOps::selectPrims(lease.getStage(), pattern, &matches, &error) && !matches.empty()) {
            PXR_NS::UsdPrim prim = lease.getStage()->GetPrimAtPath(matches.front());
            if (prim) {
                PXR_NS::UsdVariantSets sets = prim.GetVariantSets();
                setNames = sets.GetNames();
                if (!setName.empty() && sets.HasVariantSet(setName)) {
                    variantNames = sets.GetVariantSet(setName).GetVariantNames();
                }
            }
        }
    }
    _syncingMenus = true;
    syncMenu(variantSets, makeOptions(setNames), setName);
    syncMenu(variants, makeOptions(variantNames), variant->getValue());
    _syncingMenus = false;
} // USDVariant::refreshVariantMenus

StatusEnum
USDVariant::renderScene(const SceneRenderActionArgs& args,
                        ScenePayload* outputScene,
                        std::string* errorMessage)
{
    const ScenePayload upstream = args.getInputScene(0);
    const std::string pattern = _primPattern.getExpression();
    KnobStringPtr variantSetKnob = _variantSet.lock();
    KnobStringPtr variantKnob = _variant.lock();
    const std::string setName = variantSetKnob ? variantSetKnob->getValue() : std::string();
    const std::string variantName = variantKnob ? variantKnob->getValue() : std::string();

    if (!PrimSelector::validate(pattern, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }
    if (upstream.isEmpty() || PrimSelector::isBlank(pattern) || setName.empty() || variantName.empty()) {
        *outputScene = upstream;
        clearPersistentMessage(false);

        return eStatusOK;
    }
    std::string whyNot;
    if (!PXR_NS::SdfSchema::IsValidVariantIdentifier(setName).IsAllowed(&whyNot) || !PXR_NS::SdfSchema::IsValidVariantSelection(variantName).IsAllowed(&whyNot)) {
        *errorMessage = tr("\"%1\" { %2 } is not a valid variant selection: %3")
                            .arg(QString::fromStdString(setName))
                            .arg(QString::fromStdString(variantName))
                            .arg(QString::fromStdString(whyNot))
                            .toStdString();
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }

    std::vector<PXR_NS::SdfPath> matches;
    std::vector<std::string> lacking;
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
        if (!SceneOps::selectPrims(stage, pattern, &matches, errorMessage)) {
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
        for (const PXR_NS::SdfPath& path : matches) {
            const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(path);
            if (!prim || !prim.GetVariantSets().HasVariantSet(setName)) {
                lacking.push_back(path.GetString());
            }
        }
    }
    if (matches.empty()) {
        *outputScene = upstream;
        setPersistentMessage(eMessageTypeWarning, tr("The prim pattern \"%1\" matches no prim; the scene is passed through "
                                                     "unchanged.")
                                                      .arg(QString::fromStdString(pattern))
                                                      .toStdString());

        return eStatusOK;
    }

    ContentHasher facts;
    SceneNodeAuthoring::appendUpstreamMetadata(upstream, &facts);
    facts.appendU64(matches.size());
    for (const PXR_NS::SdfPath& path : matches) {
        facts.appendString(path.GetString());
    }
    SceneLayerMemo::AuthorFunc author = [matches, setName, variantName](const SceneLayerEdit& edit,
                                                                        std::string* error) {
        SceneNodeAuthoring::authorLayerMetadata(edit);
        for (const PXR_NS::SdfPath& path : matches) {
            PXR_NS::SdfPrimSpecHandle spec = PXR_NS::SdfCreatePrimInLayer(edit.layer, path);
            if (!spec) {
                *error = tr("USD could not author over %1.").arg(QString::fromStdString(path.GetString())).toStdString();

                return false;
            }
            spec->SetVariantSelection(setName, variantName);
        }

        return true;
    };

    // Of the input, the layer reads only the metadata and which prims the pattern matches.
    SceneLayerReads reads;
    reads.upstream = SceneLayerMemo::Key::eUpstreamReadFacts;
    reads.factsDigest = facts.finalize();
    reads.frameRange = false;
    if (!emitSceneLayer(upstream, reads, author, outputScene, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }
    if (lacking.empty()) {
        clearPersistentMessage(false);
    } else {
        // Not an error: a set of that name may compose there downstream, by a reference or payload.
        QStringList paths;
        for (const std::string& path : lacking) {
            paths.push_back(QString::fromStdString(path));
        }
        setPersistentMessage(eMessageTypeInfo, tr("No variant set \"%1\" on %2: the selection is authored there anyway "
                                                  "and has no effect until such a set composes there.")
                                                   .arg(QString::fromStdString(setName))
                                                   .arg(paths.join(QString::fromUtf8(", ")))
                                                   .toStdString());
    }

    return eStatusOK;
} // USDVariant::renderScene

NATRON_NAMESPACE_EXIT
