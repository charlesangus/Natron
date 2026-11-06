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

#include "USDPrune.h"

#include <string>
#include <vector>

#include "Engine/ChoiceOption.h"
#include "Engine/ContentHasher.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

USDPrune::USDPrune(NodePtr node)
    : NativeEffectBase(node)
{
}

USDPrune::~USDPrune()
{
}

const NodeSpec*
USDPrune::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
USDPrune::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    _primPattern.create(this, page, kUSDPruneParamPrimPattern, tr("Prim Pattern"), tr("The prims to remove."));

    KnobChoicePtr mode = createKnob<KnobChoice>(tr("Mode"));
    mode->setName(kUSDPruneParamMode);
    mode->setHintToolTip(tr("How the matched prims are removed."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("deactivate", tr("Deactivate").toStdString(),
                                       tr("Makes each match inactive: it and its descendants are no longer on the stage, for "
                                          "drawing, rendering or later patterns.")
                                           .toStdString()));
        options.push_back(ChoiceOption("hide", tr("Hide").toStdString(),
                                       tr("Makes each match that can be drawn invisible, and so its descendants: they stay on "
                                          "the stage, and later nodes can still match and show them.")
                                           .toStdString()));
        mode->populateChoices(options);
    }
    mode->setDefaultValue((int)eModeDeactivate);
    mode->setAnimationEnabled(false);
    page->addKnob(mode);
    _mode = mode;
} // USDPrune::initializeKnobs

StatusEnum
USDPrune::renderScene(const SceneRenderActionArgs& args,
                      ScenePayload* outputScene,
                      std::string* errorMessage)
{
    const ScenePayload upstream = args.getInputScene(0);
    KnobChoicePtr modeKnob = _mode.lock();
    const std::string pattern = _primPattern.getExpression();
    const ModeEnum mode = (modeKnob && (modeKnob->getValue() == (int)eModeHide)) ? eModeHide : eModeDeactivate;

    if (!PrimSelector::validate(pattern, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }
    std::vector<PXR_NS::SdfPath> matches;
    if (!upstream.isEmpty() && !PrimSelector::isBlank(pattern)) {
        SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(upstream, errorMessage);
        PXR_NS::UsdStageRefPtr stage = lease.getStage();
        if (!stage) {
            if (errorMessage->empty()) {
                *errorMessage = tr("USD could not compose the input scene.").toStdString();
            }
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
        std::vector<PXR_NS::SdfPath> found;
        if (!SceneOps::selectPrims(stage, pattern, &found, errorMessage)) {
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
        for (const PXR_NS::SdfPath& path : found) {
            if ((mode == eModeDeactivate) || stage->GetPrimAtPath(path).IsA<PXR_NS::UsdGeomImageable>()) {
                matches.push_back(path);
            }
        }
        if (matches.empty()) {
            *outputScene = upstream;
            setPersistentMessage(eMessageTypeWarning, tr("The prim pattern \"%1\" matches no prim to remove; the scene is "
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

    SceneLayerMemo::AuthorFunc author = [this, matches, mode](const SceneLayerEdit& edit,
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
            bool ok = bool(prim);
            if (ok) {
                if (mode == eModeDeactivate) {
                    ok = prim.SetActive(false);
                } else {
                    const PXR_NS::UsdAttribute visibility = prim.CreateAttribute(PXR_NS::UsdGeomTokens->visibility, PXR_NS::SdfValueTypeNames->Token, false);
                    ok = visibility && visibility.Set(PXR_NS::UsdGeomTokens->invisible);
                }
            }
            if (!ok) {
                *error = tr("USD could not author over %1.").arg(QString::fromStdString(path.GetString())).toStdString();

                return false;
            }
        }

        return true;
    };

    // Of the input, the layer reads only the matches and the metadata: an edit upstream that keeps
    // them re-stacks the layer without authoring it again.
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
} // USDPrune::renderScene

NATRON_NAMESPACE_EXIT
