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

#include "RenderContext.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>

#include "Engine/ContentHasher.h"

NATRON_NAMESPACE_ENTER

namespace {

std::atomic<U64>&
renderContextReadsGeneration()
{
    // Starts above 0 so that a memo stamped 0 is never current.
    static std::atomic<U64> generation(1);

    return generation;
}

U64
doubleBits(double value)
{
    U64 bits;
    std::memcpy(&bits, &value, sizeof(bits));

    return bits;
}

struct ValueHasher {
    ContentHasher* h;

    void operator()(bool v) const { h->appendBool(v); }
    void operator()(std::int64_t v) const { h->appendI64(v); }
    void operator()(double v) const { h->appendU64(doubleBits(v)); }
    void operator()(const std::string& v) const { h->appendString(v); }

    void operator()(const std::vector<double>& v) const
    {
        h->appendU64(v.size());
        for (double d : v) {
            h->appendU64(doubleBits(d));
        }
    }

    void operator()(const std::vector<std::int64_t>& v) const
    {
        h->appendU64(v.size());
        for (std::int64_t i : v) {
            h->appendI64(i);
        }
    }

    void operator()(const std::vector<std::string>& v) const
    {
        h->appendU64(v.size());
        for (const std::string& s : v) {
            h->appendString(s);
        }
    }
};

struct ValueEquals {
    bool operator()(double a,
                    double b) const
    {
        return doubleBits(a) == doubleBits(b);
    }

    bool operator()(const std::vector<double>& a,
                    const std::vector<double>& b) const
    {
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (doubleBits(a[i]) != doubleBits(b[i])) {
                return false;
            }
        }

        return true;
    }

    template <typename T>
    bool operator()(const T& a,
                    const T& b) const
    {
        return a == b;
    }

    template <typename T, typename U>
    bool operator()(const T&,
                    const U&) const
    {
        return false;
    }
};

bool
entryNameLess(const RenderContext::Entry& a,
              const RenderContext::Entry& b)
{
    return a.first < b.first;
}

} // namespace

RenderContextName
RenderContextName::plain(const std::string& name)
{
    return RenderContextName(std::string(), name);
}

RenderContextName
RenderContextName::loop(const std::string& name)
{
    return RenderContextName("loop", name);
}

RenderContextName
RenderContextName::loops(const std::string& loopName,
                         const std::string& name)
{
    return RenderContextName("loops." + loopName, name);
}

bool
renderContextValuesEqual(const RenderContextValue& a,
                         const RenderContextValue& b)
{
    return std::visit(ValueEquals(), a, b);
}

RenderContextReadSet
RenderContextReadSet::all()
{
    RenderContextReadSet reads;

    reads.setReadsAll();

    return reads;
}

void
RenderContextReadSet::add(const RenderContextName& name)
{
    if (_readsAll) {
        return;
    }
    std::vector<RenderContextName>::iterator it = std::lower_bound(_names.begin(), _names.end(), name);
    if ((it == _names.end()) || (*it != name)) {
        _names.insert(it, name);
    }
}

void
RenderContextReadSet::merge(const RenderContextReadSet& other)
{
    if (_readsAll) {
        return;
    }
    if (other._readsAll) {
        setReadsAll();

        return;
    }
    if (other._names.empty()) {
        return;
    }
    if (_names.empty()) {
        _names = other._names;

        return;
    }
    std::vector<RenderContextName> merged;
    merged.reserve(_names.size() + other._names.size());
    std::set_union(_names.begin(), _names.end(), other._names.begin(), other._names.end(), std::back_inserter(merged));
    _names.swap(merged);
}

void
RenderContextReadSet::setReadsAll()
{
    _readsAll = true;
    _names.clear();
}

bool
RenderContextReadSet::contains(const RenderContextName& name) const
{
    return _readsAll || std::binary_search(_names.begin(), _names.end(), name);
}

U64
getRenderContextReadsGeneration()
{
    return renderContextReadsGeneration().load();
}

void
invalidateRenderContextReads()
{
    renderContextReadsGeneration().fetch_add(1);
}

RenderContext::Builder::Builder(const RenderContextPtr& base)
{
    if (base) {
        for (const Entry& entry : base->entries()) {
            _entries.insert(entry);
        }
    }
}

RenderContext::Builder&
RenderContext::Builder::set(const RenderContextName& name,
                            const RenderContextValue& value)
{
    _entries[name] = value;

    return *this;
}

RenderContext::Builder&
RenderContext::Builder::erase(const RenderContextName& name)
{
    _entries.erase(name);

    return *this;
}

RenderContextPtr
RenderContext::Builder::build() const
{
    if (_entries.empty()) {
        return RenderContext::empty();
    }
    std::vector<Entry> sorted(_entries.begin(), _entries.end());

    return RenderContext::create(std::move(sorted));
}

RenderContext::RenderContext(std::vector<Entry>&& sortedEntries)
    : _entries(std::move(sortedEntries))
    , _hash(0)
{
    if (_entries.empty()) {
        return;
    }
    ContentHasher h;
    h.appendU64(_entries.size());
    ValueHasher valueHasher = { &h };
    for (const Entry& entry : _entries) {
        h.appendString(entry.first.ns);
        h.appendString(entry.first.name);
        h.appendU64(entry.second.index());
        std::visit(valueHasher, entry.second);
    }
    _hash = h.finalize();
}

RenderContextPtr
RenderContext::create(std::vector<Entry>&& sortedEntries)
{
    return RenderContextPtr(new RenderContext(std::move(sortedEntries)));
}

RenderContextPtr
RenderContext::empty()
{
    static const RenderContextPtr emptyContext = create(std::vector<Entry>());

    return emptyContext;
}

RenderContextPtr
RenderContext::overlay(const RenderContextPtr& other) const
{
    if (!other || other->isEmpty()) {
        return shared_from_this();
    }
    if (isEmpty()) {
        return other;
    }
    std::vector<Entry> merged;
    merged.reserve(_entries.size() + other->_entries.size());
    std::vector<Entry>::const_iterator mine = _entries.begin();
    std::vector<Entry>::const_iterator theirs = other->_entries.begin();
    while ((mine != _entries.end()) || (theirs != other->_entries.end())) {
        if (theirs == other->_entries.end()) {
            merged.push_back(*mine++);
        } else if ((mine == _entries.end()) || (theirs->first < mine->first)) {
            merged.push_back(*theirs++);
        } else if (mine->first < theirs->first) {
            merged.push_back(*mine++);
        } else {
            merged.push_back(*theirs++);
            ++mine;
        }
    }

    return create(std::move(merged));
}

RenderContextPtr
RenderContext::restrictTo(const RenderContextReadSet& reads) const
{
    if (reads.readsAll()) {
        return shared_from_this();
    }
    if (isEmpty() || reads.empty()) {
        return empty();
    }
    std::vector<Entry> kept;
    for (const Entry& entry : _entries) {
        if (reads.contains(entry.first)) {
            kept.push_back(entry);
        }
    }
    if (kept.size() == _entries.size()) {
        return shared_from_this();
    }
    if (kept.empty()) {
        return empty();
    }

    return create(std::move(kept));
}

const RenderContextValue*
RenderContext::find(const RenderContextName& name) const
{
    Entry probe(name, RenderContextValue());
    std::vector<Entry>::const_iterator it = std::lower_bound(_entries.begin(), _entries.end(), probe, entryNameLess);
    if ((it == _entries.end()) || (it->first != name)) {
        return 0;
    }

    return &it->second;
}

bool
RenderContext::operator==(const RenderContext& other) const
{
    if ((_hash != other._hash) || (_entries.size() != other._entries.size())) {
        return false;
    }
    for (std::size_t i = 0; i < _entries.size(); ++i) {
        if ((_entries[i].first != other._entries[i].first) || !renderContextValuesEqual(_entries[i].second, other._entries[i].second)) {
            return false;
        }
    }

    return true;
}

U64
mixRenderContextKey(U64 hash,
                    U64 contextKey)
{
    if (contextKey == 0) {
        return hash;
    }
    ContentHasher h;
    h.appendU64(hash);
    h.appendU64(contextKey);

    return h.finalize();
}

NATRON_NAMESPACE_EXIT
