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

#include "USDConfigureLayer.h"

#include <list>
#include <string>
#include <vector>

#include <QString>

#include "Engine/ChoiceOption.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobLayerCustomData.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/ScenePayloadImpl.h"

NATRON_NAMESPACE_ENTER

struct USDConfigureLayer::Knobs {
    KnobChoiceWPtr appliesTo;
    KnobStringWPtr savePath;
    KnobStringWPtr defaultPrim;
    KnobStringWPtr comment;
    KnobDoubleWPtr startTime;
    KnobDoubleWPtr endTime;
    KnobDoubleWPtr timeCodesPerSecond;
    KnobDoubleWPtr framesPerSecond;
    KnobChoiceWPtr upAxis;
    KnobDoubleWPtr metersPerUnit;
    KnobFileWPtr colorConfiguration;
    KnobStringWPtr colorManagementSystem;
    KnobStringWPtr renderSettingsPrimPath;
    KnobLayerCustomDataWPtr customData;
    AuthoredKnobSet authored;
};

USDConfigureLayer::USDConfigureLayer(NodePtr node)
    : NativeEffectBase(node)
    , _knobs(new Knobs)
{
}

USDConfigureLayer::~USDConfigureLayer()
{
}

const NodeSpec*
USDConfigureLayer::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
USDConfigureLayer::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobChoicePtr appliesTo = createKnob<KnobChoice>(tr("Applies To"));
    appliesTo->setName(kUSDConfigureLayerParamAppliesTo);
    appliesTo->setHintToolTip(tr("Which layers go into the file this node configures."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("preceding", tr("Preceding Layers").toStdString(),
                                       tr("The layers added before this node, back to the last read file, merge, break or "
                                          "region. Across a merge it gathers every branch's unclaimed layers.")
                                           .toStdString()));
        options.push_back(ChoiceOption("following", tr("Following Layers").toStdString(),
                                       tr("The layers added after this node, up to a USDLayerEnd, a merge, a break or the "
                                          "USDWrite.")
                                           .toStdString()));
        appliesTo->populateChoices(options);
    }
    appliesTo->setDefaultValue((int)eAppliesToPreceding);
    appliesTo->setAnimationEnabled(false);
    page->addKnob(appliesTo);
    _knobs->appliesTo = appliesTo;

    KnobStringPtr savePath = createKnob<KnobString>(tr("Save Path"));
    savePath->setName(kUSDConfigureLayerParamSavePath);
    savePath->setHintToolTip(tr("The file the layers are written to. A relative path is relative to the USDWrite's output "
                                "directory, and the USDWrite's path variables apply. Blank, the layers are written into the "
                                "file of the region around them, with this node's metadata."));
    savePath->setAnimationEnabled(false);
    page->addKnob(savePath);
    _knobs->savePath = savePath;

    KnobStringPtr defaultPrim = createKnob<KnobString>(tr("Default Prim"));
    defaultPrim->setName(kUSDConfigureLayerParamDefaultPrim);
    defaultPrim->setHintToolTip(tr("The name of the root prim a reference to the file targets when it names none."));
    defaultPrim->setAnimationEnabled(false);
    _knobs->authored.add(this, page, defaultPrim);
    _knobs->defaultPrim = defaultPrim;

    KnobStringPtr comment = createKnob<KnobString>(tr("Comment"));
    comment->setName(kUSDConfigureLayerParamComment);
    comment->setHintToolTip(tr("Free text stored in the file's header."));
    comment->setAnimationEnabled(false);
    comment->setAsMultiLine();
    _knobs->authored.add(this, page, comment);
    _knobs->comment = comment;

    struct DoubleField {
        KnobDoubleWPtr* slot;
        const char* name;
        QString label;
        QString hint;
        double value;
    };
    const DoubleField doubles[] = {
        { &_knobs->startTime, kUSDConfigureLayerParamStartTime, tr("Start Time"), tr("The file's first time code."), 1. },
        { &_knobs->endTime, kUSDConfigureLayerParamEndTime, tr("End Time"), tr("The file's last time code."), 1. },
        { &_knobs->timeCodesPerSecond, kUSDConfigureLayerParamTimeCodesPerSecond, tr("Time Codes Per Second"),
          tr("How many time codes make one second of the file's animation."), 24. },
        { &_knobs->framesPerSecond, kUSDConfigureLayerParamFramesPerSecond, tr("Frames Per Second"),
          tr("The playback rate the file suggests."), 24. },
    };
    for (const DoubleField& field : doubles) {
        KnobDoublePtr knob = createKnob<KnobDouble>(field.label);
        knob->setName(field.name);
        knob->setHintToolTip(field.hint);
        knob->setDefaultValue(field.value);
        knob->setAnimationEnabled(false);
        _knobs->authored.add(this, page, knob);
        *field.slot = knob;
    }

    KnobChoicePtr upAxis = createKnob<KnobChoice>(tr("Up Axis"));
    upAxis->setName(kUSDConfigureLayerParamUpAxis);
    upAxis->setHintToolTip(tr("The axis that points up in the file's scene."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("Y", "Y", ""));
        options.push_back(ChoiceOption("Z", "Z", ""));
        upAxis->populateChoices(options);
    }
    upAxis->setDefaultValue((int)SceneMetadata::eUpAxisY);
    upAxis->setAnimationEnabled(false);
    _knobs->authored.add(this, page, upAxis);
    _knobs->upAxis = upAxis;

    KnobDoublePtr metersPerUnit = createKnob<KnobDouble>(tr("Meters Per Unit"));
    metersPerUnit->setName(kUSDConfigureLayerParamMetersPerUnit);
    metersPerUnit->setHintToolTip(tr("The length of one scene unit in the file, in meters."));
    metersPerUnit->setDefaultValue(0.01);
    metersPerUnit->setAnimationEnabled(false);
    _knobs->authored.add(this, page, metersPerUnit);
    _knobs->metersPerUnit = metersPerUnit;

    KnobFilePtr colorConfiguration = createKnob<KnobFile>(tr("Color Configuration"));
    colorConfiguration->setName(kUSDConfigureLayerParamColorConfiguration);
    colorConfiguration->setHintToolTip(tr("The colour management configuration the file's colour spaces refer to."));
    colorConfiguration->setAnimationEnabled(false);
    _knobs->authored.add(this, page, colorConfiguration);
    _knobs->colorConfiguration = colorConfiguration;

    KnobStringPtr colorManagementSystem = createKnob<KnobString>(tr("Color Management System"));
    colorManagementSystem->setName(kUSDConfigureLayerParamColorManagementSystem);
    colorManagementSystem->setHintToolTip(tr("The colour management system that reads the colour configuration, such as OCIO."));
    colorManagementSystem->setAnimationEnabled(false);
    _knobs->authored.add(this, page, colorManagementSystem);
    _knobs->colorManagementSystem = colorManagementSystem;

    KnobStringPtr renderSettingsPrimPath = createKnob<KnobString>(tr("Render Settings Prim Path"));
    renderSettingsPrimPath->setName(kUSDConfigureLayerParamRenderSettingsPrimPath);
    renderSettingsPrimPath->setHintToolTip(tr("The path of the render settings prim a renderer uses when told none."));
    renderSettingsPrimPath->setAnimationEnabled(false);
    _knobs->authored.add(this, page, renderSettingsPrimPath);
    _knobs->renderSettingsPrimPath = renderSettingsPrimPath;

    KnobLayerCustomDataPtr customData = createKnob<KnobLayerCustomData>(tr("Custom Data"));
    customData->setName(kUSDConfigureLayerParamCustomData);
    customData->setHintToolTip(tr("Entries stored in the file's custom data, one per row. Type is one of String, String Array, "
                                  "Token, Token Array, Float, Integer and Boolean. Array values are separated by spaces; a "
                                  "String Array item holding spaces is quoted."));
    page->addKnob(customData);
    _knobs->customData = customData;
} // USDConfigureLayer::initializeKnobs

bool
USDConfigureLayer::knobChanged(KnobI* k,
                               ValueChangedReasonEnum reason,
                               ViewSpec /*view*/,
                               double /*time*/,
                               bool /*originatedFromMainThread*/)
{
    return _knobs->authored.onKnobChanged(k, reason);
}

StatusEnum
USDConfigureLayer::renderScene(const SceneRenderActionArgs& args,
                               ScenePayload* outputScene,
                               std::string* errorMessage)
{
    const ScenePayload input = args.getInputScene(0);
    const std::string node = getNode()->getFullyQualifiedName();

    SceneLayerRegionSettings settings;
    settings.node = node;
    KnobStringPtr savePath = _knobs->savePath.lock();
    if (savePath) {
        settings.savePath = savePath->getValue();
    }
    _knobs->authored.forEachAuthored([&](const KnobIPtr& knob) {
        const std::string& name = knob->getName();
        if (KnobString* text = dynamic_cast<KnobString*>(knob.get())) {
            const std::string value = text->getValue();
            if (name == kUSDConfigureLayerParamDefaultPrim) {
                settings.defaultPrim = value;
            } else if (name == kUSDConfigureLayerParamComment) {
                settings.comment = value;
            } else if (name == kUSDConfigureLayerParamColorManagementSystem) {
                settings.colorManagementSystem = value;
            } else if (name == kUSDConfigureLayerParamRenderSettingsPrimPath) {
                settings.renderSettingsPrimPath = value;
            }
        } else if (KnobFile* file = dynamic_cast<KnobFile*>(knob.get())) {
            settings.colorConfiguration = file->getValue();
        } else if (KnobDouble* number = dynamic_cast<KnobDouble*>(knob.get())) {
            const double value = number->getValueAtTime(args.time);
            if (name == kUSDConfigureLayerParamStartTime) {
                settings.startTime = value;
            } else if (name == kUSDConfigureLayerParamEndTime) {
                settings.endTime = value;
            } else if (name == kUSDConfigureLayerParamTimeCodesPerSecond) {
                settings.timeCodesPerSecond = value;
            } else if (name == kUSDConfigureLayerParamFramesPerSecond) {
                settings.framesPerSecond = value;
            } else if (name == kUSDConfigureLayerParamMetersPerUnit) {
                settings.metersPerUnit = value;
            }
        } else if (KnobChoice* choice = dynamic_cast<KnobChoice*>(knob.get())) {
            settings.upAxis = (choice->getValue() == (int)SceneMetadata::eUpAxisZ) ? SceneMetadata::eUpAxisZ : SceneMetadata::eUpAxisY;
        }
    });

    KnobLayerCustomDataPtr customData = _knobs->customData.lock();
    if (customData) {
        std::list<std::vector<std::string>> rows;
        customData->getTable(&rows);
        int rowNumber = 0;
        for (const std::vector<std::string>& row : rows) {
            ++rowNumber;
            SceneLayerCustomDatum datum;
            std::string error;
            if (!SceneOps::parseCustomDatum(row.size() > 0 ? row[0] : std::string(), row.size() > 1 ? row[1] : std::string(),
                                            row.size() > 2 ? row[2] : std::string(), &datum, &error)) {
                *errorMessage = tr("Custom Data row %1: %2").arg(rowNumber).arg(QString::fromStdString(error)).toStdString();
                setPersistentMessage(eMessageTypeError, *errorMessage);

                return eStatusFailed;
            }
            std::vector<SceneLayerCustomDatum>::iterator same = settings.customData.begin();
            while ((same != settings.customData.end()) && (same->name != datum.name)) {
                ++same;
            }
            if (same != settings.customData.end()) {
                *errorMessage = tr("Custom Data row %1: another row is already named %2.").arg(rowNumber).arg(QString::fromStdString(datum.name)).toStdString();
                setPersistentMessage(eMessageTypeError, *errorMessage);

                return eStatusFailed;
            }
            settings.customData.push_back(datum);
        }
    }
    clearPersistentMessage(false);

    KnobChoicePtr appliesTo = _knobs->appliesTo.lock();
    const bool following = appliesTo && (appliesTo->getValue() == (int)eAppliesToFollowing);
    const SceneLayerRegionPtr open = SceneOps::getInnermostOpenRegion(input);
    // A region a USDLayerBegin opened has no settings until a node inside it configures it.
    const bool openIsBare = open && open->settings.node.empty();

    if (following) {
        if (openIsBare && (SceneOps::countRegionEntries(input, open.get()) == 0)) {
            *outputScene = SceneOps::setOpenRegionSettings(input, settings);
        } else {
            *outputScene = SceneOps::openRegion(input, node, settings);
        }

        return eStatusOK;
    }
    if (openIsBare && (SceneOps::countRegionEntries(input, open.get()) == SceneOps::getPrecedingRun(input).size())) {
        *outputScene = SceneOps::setOpenRegionSettings(input, settings);
    } else {
        *outputScene = SceneOps::claimPreceding(input, node, settings);
    }

    return eStatusOK;
} // USDConfigureLayer::renderScene

NATRON_NAMESPACE_EXIT
