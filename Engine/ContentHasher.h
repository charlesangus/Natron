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

#ifndef NATRON_ENGINE_CONTENTHASHER_H
#define NATRON_ENGINE_CONTENTHASHER_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <memory>
#include <string>

#include "Global/GlobalDefines.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Streaming 64-bit XXH3 hasher for content hashing. Every append is
 * canonical: equal logical values hash equal regardless of representation
 * (-0.0 vs +0.0, differing NaN payloads), and variable-length data is length-prefixed so
 * adjacent fields cannot shift bytes between each other.
 *
 * Move-only: the XXH3 state is heap allocated and not worth duplicating.
 **/
class ContentHasher {
public:
    ContentHasher();
    ContentHasher(ContentHasher&& other) noexcept;
    ContentHasher& operator=(ContentHasher&& other) noexcept;
    ~ContentHasher();

    ContentHasher(const ContentHasher&) = delete;
    ContentHasher& operator=(const ContentHasher&) = delete;

    void appendU64(U64 value);
    void appendI64(std::int64_t value);
    void appendBool(bool value);
    void appendDouble(double value);
    void appendString(const std::string& value);
    void appendBytes(const void* data, std::size_t size);

    /**
     * @brief Digest of everything appended so far; the hasher stays usable. Never 0, so that
     * 0 can mean "no hash computed".
     **/
    U64 finalize() const;

    void reset();

private:
    struct StateDeleter {
        void operator()(void* state) const noexcept;
    };

    std::unique_ptr<void, StateDeleter> _state;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_CONTENTHASHER_H
