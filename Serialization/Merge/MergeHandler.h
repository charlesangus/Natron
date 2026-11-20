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

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Serialization/KnobSerialization.h"
#include "Serialization/Merge/MergeConflict.h"

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
 **/
class ScalarKnobMergeHandler
    : public KnobMergeHandler {
public:
    void merge(const KnobSerialization* base,
               const KnobSerialization* mine,
               const KnobSerialization* theirs,
               std::optional<KnobSerialization>* merged,
               std::vector<MergeConflict>* conflicts) const override;
};

/**
 * @brief Picks the handler for a knob: the one set for the node's plug-in, else the one set for
 * the knob's type, else the scalar handler.
 **/
class MergeHandlerRegistry {
public:
    MergeHandlerRegistry();

    void setPluginHandler(const std::string& pluginID,
                          std::shared_ptr<const KnobMergeHandler> handler);
    void setKindHandler(const std::string& typeName,
                        std::shared_ptr<const KnobMergeHandler> handler);

    const KnobMergeHandler& handlerFor(const std::string& pluginID,
                                       const std::string& typeName) const;

    static const MergeHandlerRegistry& defaults();

private:
    std::map<std::string, std::shared_ptr<const KnobMergeHandler>> _byPlugin;
    std::map<std::string, std::shared_ptr<const KnobMergeHandler>> _byKind;
    std::shared_ptr<const KnobMergeHandler> _scalar;
};

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
