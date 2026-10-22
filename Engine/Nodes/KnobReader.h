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

#ifndef Engine_Nodes_KnobReader_h
#define Engine_Nodes_KnobReader_h

#include "Global/Macros.h"

#include <concepts>
#include <memory>
#include <type_traits>

#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

class ImageMetadata;

/**
 * @brief Reads knob values at one time and view, for one evaluation of a node. Every value a
 * node's bake() needs comes through one reader, so the evaluation sees one consistent context;
 * that is also where a per-render variable context would be captured, once knobs have one.
 * A reader is used by one thread and counts the knob values it read.
 **/
class KnobReader {
public:
    KnobReader(double time,
               ViewIdx view);

    KnobReader(const KnobReader&) = delete;
    KnobReader& operator=(const KnobReader&) = delete;

    double time() const
    {
        return _time;
    }

    ViewIdx view() const
    {
        return _view;
    }

    /**
     * @brief The value of one dimension of knob. A null knob throws std::invalid_argument:
     * knobs a node bakes exist once initializeKnobs() ran.
     **/
    template <typename KNOB>
    auto value(const std::shared_ptr<KNOB>& knob,
               int dimension = 0) const -> decltype(knob->getValueAtTime(0., 0))
    {
        if (!knob) {
            throwMissingKnob();
        }
        ++_reads;

        return knob->getValueAtTime(_time, dimension, _view);
    }

    template <typename KNOB>
    auto value(const std::weak_ptr<KNOB>& knob,
               int dimension = 0) const -> decltype(knob.lock()->getValueAtTime(0., 0))
    {
        return value(knob.lock(), dimension);
    }

    /**
     * @brief The value of one dimension of a knob the node may not have, fallback when it has not.
     **/
    template <typename KNOB, typename T>
    auto valueOr(const std::weak_ptr<KNOB>& knob,
                 T fallback,
                 int dimension = 0) const -> decltype(knob.lock()->getValueAtTime(0., 0))
    {
        const std::shared_ptr<KNOB> locked = knob.lock();

        return locked ? value(locked, dimension) : fallback;
    }

    /**
     * @brief How many knob values this reader has read.
     **/
    int readCount() const
    {
        return _reads;
    }

private:
    [[noreturn]] static void throwMissingKnob();

    double _time;
    ViewIdx _view;
    mutable int _reads;
};

/**
 * @brief A type that bakes its knobs into Params: a node, or anything computing on its behalf.
 * bake() reads each knob it needs once, through the reader, and Params holds the values.
 **/
template <typename T>
concept BakesParamsFromKnobs = requires(const T& baker, const KnobReader& reader) {
    typename T::Params;
    { baker.bake(reader) } -> std::same_as<typename T::Params>;
};

/**
 * @brief A type whose Params also depend on the metadata of the image it reads:
 * bake(reader, source), with source the metadata of its Source input at the reader's time and
 * view, such as the CFA layout of a raw image.
 **/
template <typename T>
concept BakesParamsFromSource = requires(const T& baker, const KnobReader& reader, const ImageMetadata& source) {
    typename T::Params;
    { baker.bake(reader, source) } -> std::same_as<typename T::Params>;
};

/**
 * @brief A type with exactly one of the two bake() forms.
 **/
template <typename T>
concept BakesParams = BakesParamsFromKnobs<T> != BakesParamsFromSource<T>;

/**
 * @brief Bakes baker's knobs through reader. Params must be copyable, so it can be handed to
 * worker threads by value with nothing left pointing at knobs: plain values, or immutable data
 * held through std::shared_ptr<const ...> when it is too large or unbounded to copy per bake.
 **/
template <BakesParams T>
    requires BakesParamsFromKnobs<T>
typename T::Params
bakeParams(const T& baker,
           const KnobReader& reader)
{
    static_assert(std::copyable<typename T::Params>, "Params must be copyable");

    return baker.bake(reader);
}

/**
 * @brief Bakes baker's knobs through reader, handing source to a baker that takes it and
 * ignoring it for one that does not.
 **/
template <BakesParams T>
typename T::Params
bakeParams(const T& baker,
           const KnobReader& reader,
           const ImageMetadata& source)
{
    static_assert(std::copyable<typename T::Params>, "Params must be copyable");

    if constexpr (BakesParamsFromSource<T>) {
        return baker.bake(reader, source);
    } else {
        return baker.bake(reader);
    }
}

template <BakesParams T>
    requires BakesParamsFromKnobs<T>
typename T::Params
bakeParams(const T& baker,
           double time,
           ViewIdx view)
{
    const KnobReader reader(time, view);

    return bakeParams(baker, reader);
}

template <BakesParams T>
typename T::Params
bakeParams(const T& baker,
           double time,
           ViewIdx view,
           const ImageMetadata& source)
{
    const KnobReader reader(time, view);

    return bakeParams(baker, reader, source);
}

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_KnobReader_h
