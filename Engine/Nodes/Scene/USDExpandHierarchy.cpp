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

#include "USDExpandHierarchy.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "Engine/ChoiceOption.h"
#include "Engine/ContentHasher.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

USDExpandHierarchy::USDExpandHierarchy(NodePtr node)
    : NativeEffectBase(node)
{
}

USDExpandHierarchy::~USDExpandHierarchy()
{
}

const NodeSpec*
USDExpandHierarchy::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
USDExpandHierarchy::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobStringPtr primPattern = createKnob<KnobString>(tr("Prim Pattern"));
    primPattern->setName(kUSDExpandHierarchyParamPrimPattern);
    primPattern->setPrimPathHint(KnobString::ePrimPathHintPattern);
    primPattern->setHintToolTip(tr("The prims to fold or expand: absolute prim paths separated by spaces, whose names may "
                                   "use * (any run of characters) and ? (any one character), such as /World/Geo/*."));
    primPattern->setAnimationEnabled(false);
    page->addKnob(primPattern);
    _primPattern = primPattern;

    KnobChoicePtr mode = createKnob<KnobChoice>(tr("Mode"));
    mode->setName(kUSDExpandHierarchyParamMode);
    mode->setHintToolTip(tr("How the viewer draws the matched prims. Renders draw every prim."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("fold", tr("Fold").toStdString(),
                                       tr("Draws each match as its bounding box and unloads the payloads below it.").toStdString()));
        options.push_back(ChoiceOption("expand", tr("Expand").toStdString(),
                                       tr("Draws each match and everything below it, and opens its ancestors.").toStdString()));
        options.push_back(ChoiceOption("expandLevels", tr("Expand Levels").toStdString(),
                                       tr("Draws each match and Levels levels below it, folding deeper prims, and opens its "
                                          "ancestors.")
                                           .toStdString()));
        mode->populateChoices(options);
    }
    mode->setDefaultValue((int)eModeExpand);
    mode->setAnimationEnabled(false);
    mode->setAddNewLine(false);
    page->addKnob(mode);
    _mode = mode;

    KnobIntPtr levels = createKnob<KnobInt>(tr("Levels"));
    levels->setName(kUSDExpandHierarchyParamLevels);
    levels->setHintToolTip(tr("With Mode Expand Levels, how many levels below each match are drawn: 0 draws the match "
                              "and folds its children."));
    levels->setMinimum(0);
    levels->setDisplayMinimum(0);
    levels->setDisplayMaximum(10);
    levels->setDefaultValue(1);
    levels->setAnimationEnabled(false);
    page->addKnob(levels);
    _levels = levels;
} // USDExpandHierarchy::initializeKnobs

StatusEnum
USDExpandHierarchy::renderScene(const SceneRenderActionArgs& args,
                                ScenePayload* outputScene,
                                std::string* errorMessage)
{
    const ScenePayload upstream = args.getInputScene(0);
    KnobStringPtr patternKnob = _primPattern.lock();
    KnobChoicePtr modeKnob = _mode.lock();
    KnobIntPtr levelsKnob = _levels.lock();
    const std::string pattern = patternKnob ? patternKnob->getValue() : std::string();
    const int modeIndex = modeKnob ? modeKnob->getValue() : (int)eModeExpand;
    const ModeEnum mode = (modeIndex == (int)eModeFold) ? eModeFold : ((modeIndex == (int)eModeExpandLevels) ? eModeExpandLevels : eModeExpand);
    const int levels = (mode == eModeExpandLevels) ? std::max(0, levelsKnob ? levelsKnob->getValue() : 0) : -1;

    std::vector<PXR_NS::SdfPath> matches;
    if (!upstream.isEmpty() && (pattern.find_first_not_of(" \t\r\n") != std::string::npos)) {
        SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(upstream, errorMessage);
        PXR_NS::UsdStageRefPtr stage = lease.getStage();
        if (!stage) {
            if (errorMessage->empty()) {
                *errorMessage = tr("USD could not compose the input scene.").toStdString();
            }
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
        if (!SceneNodeAuthoring::matchPrims(stage, pattern, &matches, errorMessage)) {
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
        if (matches.empty()) {
            *outputScene = upstream;
            setPersistentMessage(eMessageTypeWarning, tr("The prim pattern \"%1\" matches no prim; the scene is passed "
                                                         "through unchanged.")
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

    // The viewer stops at the first folded prim on the way down, so an expanded match is drawn
    // only when every ancestor is open too.
    std::set<PXR_NS::SdfPath> ancestors;
    if (mode != eModeFold) {
        const std::set<PXR_NS::SdfPath> matchSet(matches.begin(), matches.end());
        for (const PXR_NS::SdfPath& path : matches) {
            for (PXR_NS::SdfPath parent = path.GetParentPath(); parent.IsPrimPath(); parent = parent.GetParentPath()) {
                if (!matchSet.count(parent) && !ancestors.insert(parent).second) {
                    break;
                }
            }
        }
    }

    SceneLayerMemo::AuthorFunc author = [this, matches, ancestors, mode, levels](const SceneLayerEdit& edit,
                                                                                 std::string* error) {
        PXR_NS::UsdStageRefPtr stage = SceneNodeAuthoring::openLayerStage(edit, error);
        if (!stage) {
            if (error->empty()) {
                *error = tr("USD could not open the node's layer.").toStdString();
            }

            return false;
        }
        static const PXR_NS::TfToken foldKey(SceneStage::kViewerFoldKey);
        static const PXR_NS::TfToken openLevelsKey(SceneStage::kViewerOpenLevelsKey);
        for (const PXR_NS::SdfPath& path : ancestors) {
            PXR_NS::UsdPrim prim = stage->OverridePrim(path);
            if (!prim) {
                *error = tr("USD could not author over %1.").arg(QString::fromStdString(path.GetString())).toStdString();

                return false;
            }
            prim.SetCustomDataByKey(foldKey, PXR_NS::VtValue(false));
        }
        for (const PXR_NS::SdfPath& path : matches) {
            PXR_NS::UsdPrim prim = stage->OverridePrim(path);
            if (!prim) {
                *error = tr("USD could not author over %1.").arg(QString::fromStdString(path.GetString())).toStdString();

                return false;
            }
            prim.SetCustomDataByKey(foldKey, PXR_NS::VtValue(mode == eModeFold));
            if (mode != eModeFold) {
                prim.SetCustomDataByKey(openLevelsKey, PXR_NS::VtValue(levels));
            }
        }

        return true;
    };

    ContentHasher facts;
    SceneNodeAuthoring::appendUpstreamMetadata(upstream, &facts);
    facts.appendU64(matches.size());
    for (const PXR_NS::SdfPath& path : matches) {
        facts.appendString(path.GetString());
    }
    SceneLayerReads reads;
    reads.upstream = SceneLayerMemo::Key::eUpstreamReadFacts;
    reads.factsDigest = facts.finalize();
    if (!emitSceneLayer(upstream, reads, author, outputScene, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }
    clearPersistentMessage(false);

    return eStatusOK;
} // USDExpandHierarchy::renderScene

NATRON_NAMESPACE_EXIT
