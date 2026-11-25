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

#ifndef Engine_Nodes_Scene_USDExpandHierarchy_h
#define Engine_Nodes_Scene_USDExpandHierarchy_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <string>
#include <vector>

#include <QtCore/QMutex>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/ScenePayload.h"

// Free of USD: NativeRegistryAll.inc includes every node header into translation units that keep
// Py_LIMITED_API, which USD's headers cannot be compiled under.

#define PLUGINID_NATRON_USDEXPANDHIERARCHY "USDExpandHierarchy"

#define kUSDExpandHierarchyParamRuleCount "ruleCount"
// Row i's knobs are these names followed by i, from 0.
#define kUSDExpandHierarchyParamPattern "pattern"
#define kUSDExpandHierarchyParamDraw "draw"
#define kUSDExpandHierarchyParamLevels "levels"

// The most rules a node holds.
#define kUSDExpandHierarchyMaxRules 1024
// Rule rows are created this many at a time, as they are needed.
#define kUSDExpandHierarchyRuleChunk 8

NATRON_NAMESPACE_ENTER

/**
 * @brief Sets how the viewers below it draw the prims its rules name, without changing the scene.
 * Each rule is a prim pattern, a draw state, and how many levels below each match open before
 * that state applies. The node authors no layer: it passes its input's stack through and appends
 * its rules, in row order, to the draw rules the input carries, so a later row wins over an
 * earlier one and this node over the nodes above it. Viewers match the patterns against the
 * prims they have loaded and match them again after each load; renders and written files never
 * read the rules. The rule knobs stay out of the node's hash, so editing them never changes the
 * stack.
 *
 * Rows are created kUSDExpandHierarchyRuleChunk at a time as Rule Count grows, up to
 * kUSDExpandHierarchyMaxRules, and are kept once created: rows past Rule Count are hidden and
 * set no rule.
 **/
class USDExpandHierarchy
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Scene", .kind = eDataKindScene },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_USDEXPANDHIERARCHY,
        .label = "USDExpandHierarchy",
        .description = "Sets how the viewers below it draw the prims each of its rules names: as a box, by their "
                       "children, in full, in full with every payload below them loaded, or hidden. The scene "
                       "itself passes through unchanged, and renders and written files draw every prim.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindScene,
        .inputs = kInputs,
        .primPathKnob = kUSDExpandHierarchyParamPattern "0",
        .primPathKnobTakesPattern = true,
    };

    struct Rule {
        std::string pattern;
        SceneDrawStateEnum state = eSceneDrawStateFull;
        int levels = 0;
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new USDExpandHierarchy(node);
    }

    explicit USDExpandHierarchy(NodePtr node);

    virtual ~USDExpandHierarchy();

    // The shown rows, blank patterns included, in order.
    std::vector<Rule> getRules() const;

    /**
     * @brief Writes rules into the rows after the last shown row with a pattern, and shows them,
     * as far as there is room. Inside an undo step being recorded, the step takes every knob it
     * changes, so undoing it restores the rows and the count. Main thread only.
     **/
    void appendRules(const std::vector<Rule>& rules);

    // How many rules appendRules() still has room for.
    int getFreeRuleCount() const;

    /**
     * @brief The rules that set states, given in path order as a viewer sets them. A path's rule
     * comes before its ancestors', so a row below is set before the row it opens; consecutive
     * paths of one state, none an ancestor of another, share a rule that names them all.
     **/
    static std::vector<Rule> getRulesForStates(const std::vector<std::pair<std::string, SceneDrawStateEnum>>& states);

    // The draw rules the node's output adds to its input's, origin set to the node's script name.
    SceneDrawRules getDrawRules() const;

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;

    // Creates the rows the saved knobs need, before they load.
    virtual void onKnobsAboutToBeLoaded(const Serialization::NodeSerializationPtr& serialization) OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual void onEffectCreated(bool mayCreateFileDialog,
                                 const CreateNodeArgs& args) OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;
    virtual StatusEnum renderScene(const SceneRenderActionArgs& args,
                                   ScenePayload* outputScene,
                                   std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    struct Row {
        KnobStringWPtr pattern;
        KnobChoiceWPtr draw;
        KnobIntWPtr levels;
    };

    void createRow(int index);
    int createdRows() const;
    // The row after the last shown row with a pattern, where appended rules go.
    int firstFreeRow() const;
    Row rowAt(int index) const;
    // Rule Count, clamped to the rows created.
    int ruleCount() const;
    // Creates rows up to at least rows, a chunk at a time. Main thread only.
    void ensureRows(int rows,
                    bool loading);
    // Shows the rows up to Rule Count and hides the others.
    void refreshRows();
    // The row number name ends with after prefix, or -1.
    static int rowOfKnob(const std::string& name,
                         const char* prefix);

    KnobPageWPtr _page;
    KnobIntWPtr _ruleCount;
    mutable QMutex _rowsMutex;
    std::vector<Row> _rows;
    std::atomic<bool> _panelRefreshPending;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_USDExpandHierarchy_h
