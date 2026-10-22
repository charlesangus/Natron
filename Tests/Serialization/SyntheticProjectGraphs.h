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

#ifndef Tests_Serialization_SyntheticProjectGraphs_h
#define Tests_Serialization_SyntheticProjectGraphs_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <memory>
#include <string>
#include <vector>

#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoPoint.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/TrackMarker.h"
#include "Engine/TrackerContext.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Builds, in the current project, the small graphs the serialization tests save.
 **/
class SyntheticProjectGraphTest
    : public BaseTest {
protected:
    // Points the project's [Project] path variable at `directoryPath`, the same row
    // Project::saveProject() fills in from the save directory.
    bool setProjectPathVariable(const QString& directoryPath)
    {
        KnobPathPtr projectPathsKnob = getApp()->getProject()->getEnvVarKnob();
        if (!projectPathsKnob) {
            return false;
        }

        std::list<std::vector<std::string>> pathVariables;
        projectPathsKnob->getTable(&pathVariables);

        bool foundProjectRow = false;
        for (std::list<std::vector<std::string>>::iterator it = pathVariables.begin(); it != pathVariables.end(); ++it) {
            if ((it->size() == 2) && ((*it)[0] == NATRON_PROJECT_ENV_VAR_NAME)) {
                (*it)[1] = directoryPath.toStdString();
                foundProjectRow = true;
                break;
            }
        }
        if (!foundProjectRow) {
            std::vector<std::string> projectRow(2);
            projectRow[0] = NATRON_PROJECT_ENV_VAR_NAME;
            projectRow[1] = directoryPath.toStdString();
            pathVariables.push_back(projectRow);
        }
        projectPathsKnob->setTable(pathVariables);

        return true;
    }

    // generator -> Grade -> Write, with Grade's whitePoint red channel animated across two
    // keyframes and its green channel expressed off the red one.
    bool buildGeneratorGradeWriteGraph(const QTemporaryDir& tmp)
    {
        NodePtr generator = createNode(_generatorPluginID);
        NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
        NodePtr writer = createNativeWriter();

        if (!generator || !grade || !writer) {
            return false;
        }
        connectNodes(generator, grade, 0, true);
        connectNodes(grade, writer, 0, true);

        // The filename is spelled through the [Project] variable rather than as tmp.path() itself,
        // so the saved project depends neither on the random QTemporaryDir name nor on $TMPDIR.
        // [Project] must already resolve when the filename is set: while it is empty, the stored
        // value ends up with repeated "[Project]/" prefixes.
        if (!setProjectPathVariable(tmp.path())) {
            return false;
        }
        writer->setOutputFilesForWriter("[Project]/synthetic-generator-grade-write-output.exr");

        // Grade's color params (whitePoint included) are KnobColor, not KnobDouble.
        KnobColor* whitePoint = dynamic_cast<KnobColor*>(grade->getKnobByName(kGradeParamWhitePoint).get());
        if (!whitePoint) {
            return false;
        }
        whitePoint->setValueAtTime(1, 1., ViewSpec::all(), 0);
        whitePoint->setValueAtTime(10, 2.5, ViewSpec::all(), 0);
        whitePoint->setExpression(1, "thisNode.whitePoint.getValue(0)", false, true);

        return true;
    }

    // An outer Group with a user-added double knob, containing one plain node and a nested Group.
    bool buildGroupWithUserKnobAndNestedGroupGraph()
    {
        CreateNodeArgs outerArgs(PLUGINID_NATRON_GROUP, getApp()->getProject());
        NodePtr outerGroupNode = getApp()->createNode(outerArgs);
        if (!outerGroupNode) {
            return false;
        }
        NodeGroupPtr outerGroup = std::dynamic_pointer_cast<NodeGroup>(outerGroupNode->getEffectInstance());
        NodeCollectionPtr outerCollection = std::dynamic_pointer_cast<NodeCollection>(outerGroup);
        if (!outerGroup || !outerCollection) {
            return false;
        }

        EffectInstancePtr outerEffect = outerGroupNode->getEffectInstance();
        KnobPagePtr userPage = outerEffect->getOrCreateUserPageKnob();
        if (!userPage) {
            return false;
        }
        KnobDoublePtr userKnob = AppManager::createKnob<KnobDouble>(outerEffect.get(), std::string("myUserValue"), 1, false);
        if (!userKnob) {
            return false;
        }
        userKnob->setName("myUserValue");
        userKnob->setAsUserKnob(true);
        userKnob->setValue(3.25);
        userPage->addKnob(userKnob);

        CreateNodeArgs dotArgs(PLUGINID_NATRON_DOT, outerCollection);
        NodePtr innerDot = getApp()->createNode(dotArgs);

        CreateNodeArgs nestedGroupArgs(PLUGINID_NATRON_GROUP, outerCollection);
        NodePtr nestedGroupNode = getApp()->createNode(nestedGroupArgs);

        return bool(innerDot) && bool(nestedGroupNode);
    }

    // A RotoPaint with a plain stroke and a square bezier animated across two keyframes with an
    // animated feather, plus a Tracker holding two markers.
    bool buildRotoPaintAndTrackerGraph()
    {
        NodePtr rotoReader = createWorkingSpaceRead(std::string(NATRON_TESTS_FIXTURES_DIR "/flat-rgba-only.exr"));
        NodePtr rotoPaint = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTOPAINT));
        if (!rotoReader || !rotoPaint) {
            return false;
        }
        connectNodes(rotoReader, rotoPaint, 0, true);

        RotoContextPtr rotoContext = rotoPaint->getRotoContext();
        if (!rotoContext) {
            return false;
        }

        // RotoContext::makeStroke() inserts the item into its layer before calling
        // createNodes(), which null-derefs the not-yet-created merge node; build the stroke the
        // way RotoPaintInteract::makeStroke() does instead, creating its nodes first.
        RotoLayerPtr baseLayer = rotoContext->getOrCreateBaseLayer();
        RotoStrokeItemPtr stroke(new RotoStrokeItem(eRotoStrokeTypeSolid, rotoContext, "Stroke1", RotoLayerPtr()));
        if (!stroke) {
            return false;
        }
        stroke->createNodes();
        rotoContext->addItem(baseLayer, 0, stroke, RotoItem::eSelectionReasonOther);
        stroke->appendPoint(true, RotoPoint(10., 10., 1., 1.));
        stroke->appendPoint(false, RotoPoint(40., 60., 1., 2.));

        BezierPtr square = rotoContext->makeSquare(1., 50., 20., 1.);
        if (!square) {
            return false;
        }
        // A fresh shape has a single, unkeyed position; an explicit keyframe at its creation
        // time plus a moved point at a later time is what makes the curve itself animated,
        // as opposed to merely static control points with an animated feather alongside them.
        square->setKeyframe(1.);
        square->setPointAtIndex(false, 0, 10., 5., 45., 5., 45., 5., 45.);

        KnobDoublePtr featherKnob = square->getFeatherKnob();
        if (!featherKnob) {
            return false;
        }
        featherKnob->setValueAtTime(1, 0., ViewSpec::all(), 0);
        featherKnob->setValueAtTime(10, 6., ViewSpec::all(), 0);

        NodePtr trackerReader = createWorkingSpaceRead(std::string(NATRON_TESTS_FIXTURES_DIR "/flat-rgba-only.exr"));
        NodePtr tracker = createNode(QString::fromUtf8(PLUGINID_NATRON_TRACKER));
        if (!trackerReader || !tracker) {
            return false;
        }
        connectNodes(trackerReader, tracker, 0, true);

        TrackerContextPtr trackerContext = tracker->getTrackerContext();
        if (!trackerContext) {
            return false;
        }
        TrackMarkerPtr track1 = trackerContext->createMarker();
        TrackMarkerPtr track2 = trackerContext->createMarker();
        if (!track1 || !track2) {
            return false;
        }
        track1->setScriptName("track1");
        track1->getCenterKnob()->setValue(20., ViewSpec::all(), 0);
        track1->getCenterKnob()->setValue(30., ViewSpec::all(), 1);
        track2->setScriptName("track2");
        track2->getCenterKnob()->setValue(60., ViewSpec::all(), 0);
        track2->getCenterKnob()->setValue(70., ViewSpec::all(), 1);

        return true;
    }
};

NATRON_NAMESPACE_EXIT

#endif // Tests_Serialization_SyntheticProjectGraphs_h
