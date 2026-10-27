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

#ifndef Engine_Nodes_NodeSpec_h
#define Engine_Nodes_NodeSpec_h

#include "Global/Macros.h"

#include <cstddef>
#include <span>
#include <string_view>

#include "Global/Enums.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief What an input is to the node reading it. A Mask input gets the host's
 * enableMask_<label>/maskChannel_<label> selectors. A Variadic input stands for a run of
 * InputSpec::count numbered inputs and must be the last entry of a spec.
 **/
enum class InputRole : unsigned char {
    Source,
    Aux,
    Mask,
    Variadic
};

/**
 * @brief Which frames of an input the node reads: only the one being rendered, or any.
 **/
enum class InputTime : unsigned char {
    Same,
    Any
};

/**
 * @brief What the node reads outside an input's image: zero, or the nearest pixel.
 **/
enum class InputBoundary : unsigned char {
    Zero,
    Nearest
};

/**
 * @brief The layer an input is read in: the plane being rendered, the layer a knob selects,
 * or whatever layer another input is read in.
 **/
struct InputLayer {
    enum class Kind : unsigned char {
        Plane,
        Select,
        PairWith
    };

    Kind kind = Kind::Plane;
    std::string_view knob = {};
    int pairedInput = -1;

    static constexpr InputLayer plane()
    {
        return InputLayer {};
    }

    static constexpr InputLayer select(std::string_view knobName)
    {
        return InputLayer { Kind::Select, knobName, -1 };
    }

    static constexpr InputLayer pairWith(int inputNb)
    {
        return InputLayer { Kind::PairWith, {}, inputNb };
    }
};

/**
 * @brief Statically describes one input, or for InputRole::Variadic a run of count inputs
 * labelled label followed by firstNumber, firstNumber + 1, ...
 **/
struct InputSpec {
    std::string_view label;
    DataKindEnum kind = eDataKindImage;
    InputRole role = InputRole::Source;
    bool optional = false;
    InputTime time = InputTime::Same;
    InputBoundary boundary = InputBoundary::Zero;
    InputLayer layer = {};
    int count = 1;
    int firstNumber = 0;
};

/**
 * @brief Statically describes a native node: identity, grouping (a menu path whose levels are
 * separated by '/'), versions, its registration (icon resource path, empty for none, and the
 * deprecated and internal-use-only flags), whether it is a writer, the kind it outputs, the
 * reverse-flow variables it reads and its inputs. The spans view storage the spec does not own,
 * which for a constexpr spec is a constexpr array of static storage duration.
 **/
struct NodeSpec {
    std::string_view id;
    std::string_view label;
    std::string_view description = {};
    std::string_view grouping = PLUGIN_GROUP_OTHER;
    int majorVersion = 1;
    int minorVersion = 0;
    std::string_view icon = {};
    bool deprecated = false;
    bool internal = false;
    bool isWriter = false;
    DataKindEnum outputKind = eDataKindImage;
    // Names only: nothing reads reverse-flow variables yet, so nothing consumes this either.
    std::span<const std::string_view> rfvReads = {};
    std::span<const InputSpec> inputs = {};
    // The script name of the KnobString naming the prims the node works on, for a node that has one.
    std::string_view primPathKnob = {};

    /**
     * @brief The InputSpec an input number falls in, and for a Variadic one the input's index
     * within its run. spec is null for an input number out of range.
     **/
    struct ResolvedInput {
        const InputSpec* spec;
        int indexInRun;
    };

    constexpr int inputCount() const
    {
        int n = 0;

        for (const InputSpec& input : inputs) {
            n += input.count;
        }

        return n;
    }

    constexpr ResolvedInput resolveInput(int inputNb) const
    {
        if (inputNb < 0) {
            return ResolvedInput { nullptr, 0 };
        }
        int first = 0;
        for (const InputSpec& input : inputs) {
            if (inputNb < first + input.count) {
                return ResolvedInput { &input, inputNb - first };
            }
            first += input.count;
        }

        return ResolvedInput { nullptr, 0 };
    }
};

/**
 * @brief The number of the first Source input of spec, or -1 when it has none.
 **/
constexpr int
nodeSpecSourceInput(const NodeSpec& spec)
{
    int first = 0;

    for (const InputSpec& input : spec.inputs) {
        if (input.role == InputRole::Source) {
            return first;
        }
        first += input.count;
    }

    return -1;
}

/**
 * @brief The kind the first Source input of spec takes, or eDataKindPolymorphic when it has none.
 **/
constexpr DataKindEnum
nodeSpecSourceKind(const NodeSpec& spec)
{
    for (const InputSpec& input : spec.inputs) {
        if (input.role == InputRole::Source) {
            return input.kind;
        }
    }

    return eDataKindPolymorphic;
}

constexpr bool
nodeSpecMasksAreImages(const NodeSpec& spec)
{
    for (const InputSpec& input : spec.inputs) {
        if ((input.role == InputRole::Mask) && (input.kind != eDataKindImage)) {
            return false;
        }
    }

    return true;
}

constexpr bool
nodeSpecVariadicIsLast(const NodeSpec& spec)
{
    for (std::size_t i = 0; i + 1 < spec.inputs.size(); ++i) {
        if (spec.inputs[i].role == InputRole::Variadic) {
            return false;
        }
    }

    return true;
}

constexpr bool
nodeSpecCountsAreValid(const NodeSpec& spec)
{
    for (const InputSpec& input : spec.inputs) {
        if ((input.role == InputRole::Variadic) ? (input.count < 1) : (input.count != 1)) {
            return false;
        }
    }

    return true;
}

constexpr bool
nodeSpecLayerPairsAreValid(const NodeSpec& spec)
{
    const int n = spec.inputCount();

    for (int i = 0; i < n; ++i) {
        int current = i;
        // An acyclic chain visits at most n inputs, so one that takes n hops repeats an input.
        for (int hops = 0;; ++hops) {
            const InputLayer& layer = spec.resolveInput(current).spec->layer;
            if (layer.kind != InputLayer::Kind::PairWith) {
                break;
            }
            if ((layer.pairedInput < 0) || (layer.pairedInput >= n) || (hops == n)) {
                return false;
            }
            current = layer.pairedInput;
        }
    }

    return true;
}

constexpr bool
isValidNodeSpec(const NodeSpec& spec)
{
    return nodeSpecMasksAreImages(spec) && nodeSpecVariadicIsLast(spec) && nodeSpecCountsAreValid(spec) && nodeSpecLayerPairsAreValid(spec);
}

/**
 * @brief What NativeEffectBase::nodeSpec() returns for a constexpr spec. Checking the spec here
 * makes an invalid one a compile error in the node that declares it.
 **/
template <const NodeSpec& SPEC>
const NodeSpec*
staticNodeSpec()
{
    static_assert(nodeSpecMasksAreImages(SPEC), "a Mask input must take eDataKindImage");
    static_assert(nodeSpecVariadicIsLast(SPEC), "a Variadic input must be the last InputSpec");
    static_assert(nodeSpecCountsAreValid(SPEC), "only a Variadic input has a count, and it is at least 1");
    static_assert(nodeSpecLayerPairsAreValid(SPEC), "InputLayer::pairWith() must name another input of the node, and its chain must end without repeating an input");

    return &SPEC;
}

/**
 * @brief How a node's output at a time and view relates to its inputs: computed by the node
 * (None), one input's output at a time (Input), or transparent black (Black).
 **/
struct Route {
    enum class Kind : unsigned char {
        None,
        Input,
        Black
    };

    Kind kind = Kind::None;
    int inputNb = -1;
    double time = 0.;

    static constexpr Route none()
    {
        return Route {};
    }

    static constexpr Route input(int nb,
                                 double t)
    {
        return Route { Kind::Input, nb, t };
    }

    static constexpr Route black()
    {
        return Route { Kind::Black, -1, 0. };
    }
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_NodeSpec_h
