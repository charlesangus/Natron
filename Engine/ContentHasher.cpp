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

#include "ContentHasher.h"

#include <cstdint>
#include <cstring>
#include <new>
#include <utility>

// Header-only and defined only in this translation unit so the XXH symbols stay internal.
#define XXH_INLINE_ALL
#include "xxhash.h"

NATRON_NAMESPACE_ENTER

namespace {

// Replaces a digest of 0, which callers reserve for "not computed".
constexpr U64 kZeroDigestReplacement = 0x9E3779B97F4A7C15ULL;

constexpr std::uint64_t kCanonicalNaNBits = 0x7FF8000000000000ULL;

XXH3_state_t*
asState(void* p)
{
    return static_cast<XXH3_state_t*>(p);
}

XXH3_state_t*
createState()
{
    XXH3_state_t* s = XXH3_createState();
    if (!s) {
        throw std::bad_alloc();
    }
    XXH3_64bits_reset(s);

    return s;
}

} // anon namespace

void
ContentHasher::StateDeleter::operator()(void* state) const noexcept
{
    XXH3_freeState(asState(state));
}

ContentHasher::ContentHasher()
    : _state(createState())
{
}

ContentHasher::ContentHasher(ContentHasher&& other) noexcept = default;
ContentHasher& ContentHasher::operator=(ContentHasher&& other) noexcept = default;
ContentHasher::~ContentHasher() = default;

void
ContentHasher::appendBytes(const void* data, std::size_t size)
{
    const std::uint64_t len = size;
    XXH3_64bits_update(asState(_state.get()), &len, sizeof(len));
    if (size > 0) {
        XXH3_64bits_update(asState(_state.get()), data, size);
    }
}

void
ContentHasher::appendU64(U64 value)
{
    XXH3_64bits_update(asState(_state.get()), &value, sizeof(value));
}

void
ContentHasher::appendI64(std::int64_t value)
{
    appendU64(static_cast<U64>(value));
}

void
ContentHasher::appendBool(bool value)
{
    appendU64(value ? 1 : 0);
}

void
ContentHasher::appendDouble(double value)
{
    std::uint64_t bits;
    if (value != value) {
        bits = kCanonicalNaNBits;
    } else if (value == 0.) {
        bits = 0;
    } else {
        std::memcpy(&bits, &value, sizeof(bits));
    }
    appendU64(bits);
}

void
ContentHasher::appendString(const std::string& value)
{
    appendBytes(value.data(), value.size());
}

U64
ContentHasher::finalize() const
{
    const U64 digest = XXH3_64bits_digest(asState(_state.get()));

    return digest == 0 ? kZeroDigestReplacement : digest;
}

void
ContentHasher::reset()
{
    XXH3_64bits_reset(asState(_state.get()));
}

NATRON_NAMESPACE_EXIT
