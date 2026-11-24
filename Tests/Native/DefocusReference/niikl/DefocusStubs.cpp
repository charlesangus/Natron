/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

// DefocusCore.cpp calls into DefocusGpu and LensTrace, whose sources are not part of the test
// candidate. The candidate never sets Settings::useGpu or LensSettings::table, so these are only
// here to link: the GPU path reports itself unavailable and the traced-lens lookup is never reached.

#include "DefocusGpu.h"
#include "LensTrace.h"

#include <cstring>

namespace defocus {

void
LensTable::lookup(double /*u*/,
                  double /*rho*/,
                  int /*channel*/,
                  KernelLookup* out) const
{
    std::memset(out, 0, sizeof(*out));
}

namespace gpu {

    bool
    available(std::string* why)
    {
        if (why) {
            *why = "the test candidate has no GPU scatter";
        }

        return false;
    }

    bool
    scatter(const Job& /*job*/,
            std::vector<float>& /*acc*/,
            std::string& err,
            const std::function<bool()>& /*aborted*/)
    {
        err = "the test candidate has no GPU scatter";

        return false;
    }

} // namespace gpu
} // namespace defocus
