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

// Points menu at value when it lists it.
void
selectEntry(KnobChoice* menu,
            const std::string& value)
{
    const std::vector<ChoiceOption> entries = menu->getEntries_mt_safe();

    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].id == value) {
            if (menu->getValue() != (int)i) {
                menu->setValue((int)i);
            }
            break;
        }
    }
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
        same = (current[i].id == options[i].id) && (current[i].label == options[i].label);
    }
    if (!same) {
        menu->populateChoices(options);
    }
    selectEntry(menu.get(), value);
}
} // anonymous namespace

USDVariant::USDVariant(NodePtr node)
    : NativeEffectBase(node)
    , _menuRefreshPending(false)
    , _syncingMenus(false)
{
    _menuRefresh.setRerun([this]() { return refreshVariantMenus(); });
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
            const KnobStringPtr name = (k == variantSets.get()) ? variantSet : variant;
            const ChoiceOption picked = menu->getEntry(index);
            // A script can still set a disabled note row, which names nothing.
            if (!picked.disabled) {
                name->setValue(picked.id);
            }
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

std::shared_ptr<SceneMenuJob>
USDVariant::refreshVariantMenus()
{
    bool noViewer = false;
    const SceneEvaluationContext context = _menuRefresh.getContext(this, &noViewer);
    KnobStringPtr variantSet = _variantSet.lock();
    const std::string setName = variantSet ? variantSet->getValue() : std::string();
    const std::string pattern = _primPattern.getExpression();
    const EffectInstancePtr input = getInput(0);
    const double time = getCurrentTime();
    SceneMenuRefresh::WorkFunc work = [weakInput = EffectInstanceWPtr(input), pattern, setName, time, context](const SceneMenuJob& job,
                                                                                                               SceneMenuJob::Result* result) {
        result->lists.resize(2);
        std::string error;
        if (PrimSelector::isBlank(pattern) || !PrimSelector::validate(pattern, &error)) {
            return;
        }
        ScenePayload scene;
        SceneQueryPool::Lease lease;
        if (!SceneNodeAuthoring::borrowMenuScene(weakInput, time, context, job, &scene, &lease)) {
            return;
        }
        // Not the node's cache: a menu's search must not evict what an evaluation remembers.
        PrimPatternCache cache;
        SceneNodeAuthoring::PatternMatches found;
        if (!SceneNodeAuthoring::matchPattern(&cache, scene, pattern, context, 0, SceneNodeAuthoring::PatternFilter(), &lease, &found, &error)) {
            return;
        }
        result->unsearched = found.unsearched;
        if (found.paths.empty() || job.isCancelled() || !lease.isValid()) {
            return;
        }
        const PXR_NS::UsdPrim prim = lease.getStage()->GetPrimAtPath(found.paths.front());
        // Its variant sets may be authored inside the payload left unloaded.
        if (!prim || (prim.HasAuthoredPayloads() && !prim.IsLoaded())) {
            result->unsearched = 1;
            if (context.reach) {
                context.reach->add(std::vector<std::string>(1, found.paths.front().GetString()));
            }

            return;
        }
        PXR_NS::UsdVariantSets sets = prim.GetVariantSets();
        result->lists[0] = sets.GetNames();
        if (!setName.empty() && sets.HasVariantSet(setName)) {
            result->lists[1] = sets.GetVariantSet(setName).GetVariantNames();
        }
    };

    return _menuRefresh.start(this, context, noViewer, work, [this](const std::shared_ptr<SceneMenuJob>& job) { applyVariantMenus(job); });
}

bool
USDVariant::applyVariantMenus(const std::shared_ptr<SceneMenuJob>& job)
{
    if (!_menuRefresh.take(job)) {
        return false;
    }
    KnobStringPtr variantSet = _variantSet.lock();
    KnobChoicePtr variantSets = _variantSets.lock();
    KnobStringPtr variant = _variant.lock();
    KnobChoicePtr variants = _variants.lock();
    if (!variantSet || !variantSets || !variant || !variants) {
        return false;
    }
    const SceneMenuJob::Result result = job->getResult();
    std::vector<ChoiceOption> setOptions = makeOptions((result.lists.size() > 0) ? result.lists[0] : std::vector<std::string>());
    std::vector<ChoiceOption> variantOptions = makeOptions((result.lists.size() > 1) ? result.lists[1] : std::vector<std::string>());
    if (result.unsearched > 0) {
        setOptions.push_back(makeSceneUnsearchedOption(result.unsearched, job->hasNoViewer()));
        variantOptions.push_back(makeSceneUnsearchedOption(result.unsearched, job->hasNoViewer()));
    }
    _syncingMenus = true;
    syncMenu(variantSets, setOptions, variantSet->getValue());
    syncMenu(variants, variantOptions, variant->getValue());
    _syncingMenus = false;

    return true;
} // USDVariant::applyVariantMenus

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

    SceneNodeAuthoring::PatternMatches found;
    std::vector<std::string> lacking;
    {
        SceneQueryPool::Lease lease;
        if (!SceneNodeAuthoring::matchPattern(&_matches, upstream, pattern, args.context, 0, SceneNodeAuthoring::PatternFilter(), &lease, &found, errorMessage)) {
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
        if (!lease.isValid()) {
            lease = SceneQueryPool::instance().borrow(upstream, args.context.loadSet, errorMessage);
        }
        PXR_NS::UsdStageRefPtr stage = lease.getStage();
        if (!stage) {
            if (errorMessage->empty()) {
                *errorMessage = tr("USD could not compose the input scene.").toStdString();
            }
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
        std::vector<std::string> unchecked;
        for (const PXR_NS::SdfPath& path : found.paths) {
            const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(path);
            // The set may be authored inside a payload that is not loaded in this evaluation.
            if (!prim || (prim.HasAuthoredPayloads() && !prim.IsLoaded())) {
                unchecked.push_back(path.GetString());
                continue;
            }
            if (!prim.GetVariantSets().HasVariantSet(setName)) {
                lacking.push_back(path.GetString());
            }
        }
        // Loading them would let the check see their sets.
        if (args.context.reach && !unchecked.empty()) {
            args.context.reach->add(unchecked);
        }
    }
    const std::vector<PXR_NS::SdfPath>& matches = found.paths;
    if (matches.empty()) {
        *outputScene = upstream;
        setPersistentMessage(eMessageTypeWarning,
                             SceneNodeAuthoring::describeNoMatch(pattern, found, tr("the scene is passed through unchanged").toStdString()));

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
