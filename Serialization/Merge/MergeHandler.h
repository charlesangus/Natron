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

#ifndef Serialization_Merge_MergeHandler_h
#define Serialization_Merge_MergeHandler_h

#include "Global/Macros.h"

#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Serialization/CurveSerialization.h"
#include "Serialization/KnobSerialization.h"
#include "Serialization/Merge/MergeConflict.h"
#include "Serialization/NodeSerialization.h"
#include "Serialization/RotoSerialization.h"
#include "Serialization/TrackerSerialization.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief Merges one knob of one node. A null knob is at its default. The handler sets *merged to
 * the result, or to nullopt for the default, and appends a conflict, with knob and dimension set,
 * for each choice it made; the caller names the node.
 **/
class KnobMergeHandler {
public:
    virtual ~KnobMergeHandler();

    virtual void merge(const KnobSerialization* base,
                       const KnobSerialization* mine,
                       const KnobSerialization* theirs,
                       std::optional<KnobSerialization>* merged,
                       std::vector<MergeConflict>* conflicts) const
        = 0;
};

/**
 * @brief Merges each dimension on its own: a dimension takes the side that changed it from the
 * base, and conflicts, keeping mine, only when both changed it differently. A dimension's state is
 * its value, animation, expression, link and driver together. The parametric curves, string
 * animation and choice option merge as one more unit.
 *
 * A dimension both sides animated, and changed in nothing but its curve, merges key by key (see
 * mergeCurves()) and conflicts per key; one side animating or un-animating it while the other
 * edits it conflicts for the whole dimension. Parametric curves merge key by key the same way
 * when every side has the same number of curves. Expressions and links conflict as a whole.
 **/
class ScalarKnobMergeHandler
    : public KnobMergeHandler {
public:
    void merge(const KnobSerialization* base,
               const KnobSerialization* mine,
               const KnobSerialization* theirs,
               std::optional<KnobSerialization>* merged,
               std::vector<MergeConflict>* conflicts) const override;

protected:
    /**
     * @brief Called for a dimension both sides changed differently. Sets *merged and returns true
     * when it can merge the two changes, appending any finer conflicts it raised; returns false
     * for a conflict on the whole dimension. merged starts as mine's dimension.
     **/
    virtual bool mergeDimension(const KnobSerialization* base,
                                const KnobSerialization* mine,
                                const KnobSerialization* theirs,
                                std::size_t index,
                                KnobDimensionSerialization* merged,
                                std::vector<MergeConflict>* conflicts) const;
};

/**
 * @brief The scalar handler, plus a line-based three-way merge (see mergeTextLines()) of the string
 * values of a dimension both sides changed in nothing but its value.
 **/
class TextKnobMergeHandler
    : public ScalarKnobMergeHandler {
protected:
    bool mergeDimension(const KnobSerialization* base,
                        const KnobSerialization* mine,
                        const KnobSerialization* theirs,
                        std::size_t index,
                        KnobDimensionSerialization* merged,
                        std::vector<MergeConflict>* conflicts) const override;
};

class MergeHandlerRegistry;

/**
 * @brief Merges what a node holds beside its knobs: its roto items and its tracks. merged holds
 * mine's node, with its knobs already merged; base is null when the base has no such node. The
 * handler appends its conflicts with item set where it applies; the caller names the node.
 **/
class NodeMergeHandler {
public:
    virtual ~NodeMergeHandler();

    virtual void merge(const NodeSerialization* base,
                       const NodeSerialization& theirs,
                       const MergeHandlerRegistry& handlers,
                       NodeSerialization* merged,
                       std::vector<MergeConflict>* conflicts) const
        = 0;
};

/**
 * @brief Merges a roto context item by item (mergeRotoContexts()) and a tracker context track by
 * track (mergeTrackerContexts()), for whichever the node has.
 **/
class ContentNodeMergeHandler
    : public NodeMergeHandler {
public:
    void merge(const NodeSerialization* base,
               const NodeSerialization& theirs,
               const MergeHandlerRegistry& handlers,
               NodeSerialization* merged,
               std::vector<MergeConflict>* conflicts) const override;
};

/**
 * @brief Picks the handler for a knob: the one set for the node's plug-in, else the one set for
 * the knob's type, else the scalar handler. String knobs start with the text handler. Picks the
 * node handler the same way: the one set for the plug-in, else the content handler. The knobs of
 * roto items and tracks have no plug-in of their own and go by their type.
 **/
class MergeHandlerRegistry {
public:
    MergeHandlerRegistry();

    void setPluginHandler(const std::string& pluginID,
                          std::shared_ptr<const KnobMergeHandler> handler);
    void setKindHandler(const std::string& typeName,
                        std::shared_ptr<const KnobMergeHandler> handler);
    void setNodeHandler(const std::string& pluginID,
                        std::shared_ptr<const NodeMergeHandler> handler);

    const KnobMergeHandler& handlerFor(const std::string& pluginID,
                                       const std::string& typeName) const;
    const NodeMergeHandler& nodeHandlerFor(const std::string& pluginID) const;

    static const MergeHandlerRegistry& defaults();

private:
    std::map<std::string, std::shared_ptr<const KnobMergeHandler>> _byPlugin;
    std::map<std::string, std::shared_ptr<const KnobMergeHandler>> _byKind;
    std::shared_ptr<const KnobMergeHandler> _scalar;
    std::map<std::string, std::shared_ptr<const NodeMergeHandler>> _nodeByPlugin;
    std::shared_ptr<const NodeMergeHandler> _content;
};

/**
 * @brief Merges three knob lists by knob name through handlers, a knob absent from a list being at
 * its default. base is null when there is no base. Mine's knobs come first, in mine's order.
 **/
std::list<KnobSerialization> mergeKnobLists(const MergeHandlerRegistry& handlers,
                                            const std::string& pluginID,
                                            const std::list<KnobSerialization>* base,
                                            const std::list<KnobSerialization>& mine,
                                            const std::list<KnobSerialization>& theirs,
                                            std::vector<MergeConflict>* conflicts);

/**
 * @brief Whether two knob lists hold the same state, absent knobs being at their default.
 **/
bool sameKnobLists(const std::list<KnobSerialization>& a,
                   const std::list<KnobSerialization>& b);

/**
 * @brief Whether two nodes hold the same roto items and tracks, absent knobs being at their default.
 **/
bool sameNodeContent(const NodeSerialization& a,
                     const NodeSerialization& b);

/**
 * @brief A key both sides changed differently, or added differently at one time.
 **/
struct CurveKeyConflict {
    double time = 0.;
    std::optional<KeyFrameSerialization> base;
    std::optional<KeyFrameSerialization> mine;
    std::optional<KeyFrameSerialization> theirs;
};

/**
 * @brief Merges three curves as maps from key time to key: a key that one side added, changed or
 * removed takes that side; keys at one time that both sides changed differently keep mine's key,
 * or no key when mine removed it, and are listed in conflicts in time order.
 **/
CurveSerialization mergeCurves(const CurveSerialization& base,
                               const CurveSerialization& mine,
                               const CurveSerialization& theirs,
                               std::vector<CurveKeyConflict>* conflicts);

/**
 * @brief A line-based three-way merge (diff3). The lines that each side keeps from the base split
 * the texts into hunks; a hunk takes the side that changed it, and one both sides changed
 * differently is a conflict. Returns false, leaving *merged alone, on any conflict.
 **/
bool mergeTextLines(const std::string& base,
                    const std::string& mine,
                    const std::string& theirs,
                    std::string* merged);

/**
 * @brief Merges roto contexts item by item, matching items by script name within their layer. An
 * item one side added or deleted follows that side, and the other side's changes to an item it
 * deleted keep it, with a delete-modify conflict. Items both sides kept merge their knobs through
 * handlers and their label, activation, lock and overlay colour as the other appearance does; a
 * shape whose points or strokes both sides changed differently keeps mine's with one roto-shape
 * conflict. A layer's items follow theirs' order, with mine's own items after the item they follow
 * in mine. base is null when the base has no roto context.
 **/
RotoContextSerialization mergeRotoContexts(const RotoContextSerialization* base,
                                           const RotoContextSerialization& mine,
                                           const RotoContextSerialization& theirs,
                                           const MergeHandlerRegistry& handlers,
                                           std::vector<MergeConflict>* conflicts);

/**
 * @brief Merges tracker contexts track by track, matching tracks by script name. Whether a track
 * exists follows the side that differs from the base, except that a track one side removed and
 * the other edited is kept, with a delete-modify conflict. Tracks both sides kept merge their knobs through handlers, so
 * their curves merge key by key, and their user keyframes as sets. Tracks follow theirs' order,
 * with mine's own tracks after the track they follow in mine. base is null when the base has no
 * tracker context.
 **/
TrackerContextSerialization mergeTrackerContexts(const TrackerContextSerialization* base,
                                                 const TrackerContextSerialization& mine,
                                                 const TrackerContextSerialization& theirs,
                                                 const MergeHandlerRegistry& handlers,
                                                 std::vector<MergeConflict>* conflicts);

/**
 * @brief True when the dimension holds nothing but its default value.
 **/
bool isDefaultDimension(const KnobDimensionSerialization& dimension);

/**
 * @brief True when saving the knob records nothing a default knob lacks: a user knob never is.
 **/
bool isDefaultKnob(const KnobSerialization& knob);

/**
 * @brief Whether two knobs hold the same state, a null knob being the default.
 **/
bool sameKnobState(const KnobSerialization* a,
                   const KnobSerialization* b);

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Merge_MergeHandler_h
