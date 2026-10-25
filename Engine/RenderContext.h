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

#ifndef NATRON_ENGINE_RENDERCONTEXT_H
#define NATRON_ENGINE_RENDERCONTEXT_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "Global/GlobalDefines.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A variable name in a render context. Names in different namespaces never collide:
 * the loop namespace ("loop"), a named loop's ("loops.<name>") and plain variables (empty
 * namespace) keep `loop.t`, `loops.a.t` and `t` apart.
 **/
struct RenderContextName {
    std::string ns;
    std::string name;

    RenderContextName() = default;
    RenderContextName(const std::string& ns_,
                      const std::string& name_)
        : ns(ns_)
        , name(name_)
    {
    }

    static RenderContextName plain(const std::string& name);
    static RenderContextName loop(const std::string& name);
    static RenderContextName loops(const std::string& loopName,
                                   const std::string& name);

    bool operator<(const RenderContextName& other) const
    {
        return (ns < other.ns) || ((ns == other.ns) && (name < other.name));
    }

    bool operator==(const RenderContextName& other) const
    {
        return (ns == other.ns) && (name == other.name);
    }

    bool operator!=(const RenderContextName& other) const
    {
        return !(*this == other);
    }
};

typedef std::variant<bool,
                     std::int64_t,
                     double,
                     std::string,
                     std::vector<double>,
                     std::vector<std::int64_t>,
                     std::vector<std::string>>
    RenderContextValue;

/**
 * @brief Equality as the context hash sees it: doubles compare by their bits, so -0 and 0 differ
 * and a NaN equals the same NaN.
 **/
bool renderContextValuesEqual(const RenderContextValue& a,
                              const RenderContextValue& b);

/**
 * @brief The context variables something reads, or all of them.
 **/
class RenderContextReadSet {
public:
    RenderContextReadSet() = default;

    static RenderContextReadSet all();

    void add(const RenderContextName& name);
    void merge(const RenderContextReadSet& other);
    void setReadsAll();

    bool readsAll() const
    {
        return _readsAll;
    }

    bool empty() const
    {
        return !_readsAll && _names.empty();
    }

    bool contains(const RenderContextName& name) const;

    /**
     * @brief Sorted and unique. Empty when readsAll().
     **/
    const std::vector<RenderContextName>& names() const
    {
        return _names;
    }

    bool operator==(const RenderContextReadSet& other) const
    {
        return (_readsAll == other._readsAll) && (_names == other._names);
    }

    bool operator!=(const RenderContextReadSet& other) const
    {
        return !(*this == other);
    }

private:
    bool _readsAll = false;
    std::vector<RenderContextName> _names;
};

typedef std::shared_ptr<const RenderContextReadSet> RenderContextReadSetConstPtr;

/**
 * @brief Something attached to a knob whose value depends on context variables. A source whose
 * reads change after it was added must call invalidateRenderContextReads().
 **/
class ContextReadSource {
public:
    virtual ~ContextReadSource() { }

    virtual void appendContextReads(RenderContextReadSet* reads) const = 0;
};

typedef std::shared_ptr<ContextReadSource> ContextReadSourcePtr;

/**
 * @brief Moves whenever any read-set may differ: on adding, removing or editing a read source,
 * and on every node hash mark, which connections and expression edits raise.
 * Memoized read-sets are only valid for the generation read before computing them. MT-safe.
 **/
U64 getRenderContextReadsGeneration();
void invalidateRenderContextReads();

class RenderContext;
typedef std::shared_ptr<const RenderContext> RenderContextPtr;

/**
 * @brief An immutable map from namespaced variable names to values, shared by pointer. Its hash
 * depends only on the entries, never on the order they were set in, and is 0 only for the empty
 * context, so a node that reads no variable keeps context key 0.
 **/
class RenderContext
    : public std::enable_shared_from_this<RenderContext> {
public:
    typedef std::pair<RenderContextName, RenderContextValue> Entry;

    class Builder {
    public:
        Builder() = default;

        /**
         * @brief Starts from base's entries.
         **/
        explicit Builder(const RenderContextPtr& base);

        Builder& set(const RenderContextName& name,
                     const RenderContextValue& value);
        Builder& erase(const RenderContextName& name);

        RenderContextPtr build() const;

    private:
        std::map<RenderContextName, RenderContextValue> _entries;
    };

    static RenderContextPtr empty();

    /**
     * @brief This context with other's entries set over it: other wins on a shared name.
     **/
    RenderContextPtr overlay(const RenderContextPtr& other) const;

    /**
     * @brief Only the entries reads names; all of them if reads reads all.
     **/
    RenderContextPtr restrictTo(const RenderContextReadSet& reads) const;

    /**
     * @brief Null if name is not set.
     **/
    const RenderContextValue* find(const RenderContextName& name) const;

    bool isEmpty() const
    {
        return _entries.empty();
    }

    std::size_t size() const
    {
        return _entries.size();
    }

    /**
     * @brief Sorted by name.
     **/
    const std::vector<Entry>& entries() const
    {
        return _entries;
    }

    U64 hash() const
    {
        return _hash;
    }

    bool operator==(const RenderContext& other) const;

    bool operator!=(const RenderContext& other) const
    {
        return !(*this == other);
    }

private:
    explicit RenderContext(std::vector<Entry>&& sortedEntries);

    static RenderContextPtr create(std::vector<Entry>&& sortedEntries);

    std::vector<Entry> _entries;
    U64 _hash;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_RENDERCONTEXT_H
