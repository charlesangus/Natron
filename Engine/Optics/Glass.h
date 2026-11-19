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

#ifndef NATRON_ENGINE_OPTICS_GLASS_H
#define NATRON_ENGINE_OPTICS_GLASS_H

/*
 * Optical glass catalogues and dispersion. Glass::ior and the nearest-glass score follow realflare
 * (https://github.com/beatreichenbach/realflare v1.0.3, flare/api/lens/model.py Material.get_ior
 * and flare/adapters/database/database.py get_material, GPL-3.0-or-later); the Cauchy fallback
 * and abbeFor() come from Niik-l's LensTrace (niikl/RB-2.6 Engine/Dev/PostFX/LensTrace.cpp,
 * GPL-2.0-or-later); this port is GPL-3.0-or-later. The catalogue data is OpticsDatabase's (MIT, see
 * Gui/Resources/Optics/LICENSE-OpticsDatabase.txt), from refractiveindex.info (CC0), converted by
 * tools/optics/convert_glass.py.
 */

#include <optional>
#include <string>
#include <vector>

namespace Natron {
namespace Optics {

    // The d line (helium, 587.56 nm), where nd and Vd are measured.
    constexpr double kWavelengthDNm = 587.5618;

    struct Glass {
        std::string vendor;
        std::string name;
        double nd = 1.;
        double vd = 0.;
        // refractiveindex.info dispersion formula, wavelength in micrometres:
        //   1: n^2 - 1 = A + sum B lambda^2 / (lambda^2 - C^2)
        //   2: n^2 - 1 = A + sum B lambda^2 / (lambda^2 - C)
        //   3: n^2     = A + sum B lambda^C
        // coefficients is A followed by (B, C) pairs.
        int formula = 0;
        std::vector<double> coefficients;

        double ior(double wavelengthNm) const;
    };

    class GlassCatalogue {
    public:
        // <directory of the running executable>/../Resources/optics/glass.json.
        static std::string defaultPath();

        bool load(const std::string& path, std::string* error);
        // text is glass.json's content.
        bool parse(const std::string& text, std::string* error);

        const std::vector<Glass>& glasses() const { return _glasses; }
        // Sorted, without duplicates.
        std::vector<std::string> vendors() const;
        const Glass* find(const std::string& vendor, const std::string& name) const;

        // The vendor's glass with the least realflare score, sqrt(sum (1 - glass / target)^2) over
        // (nd, Vd); ties go to the first by name. Null when the vendor has no glasses.
        const Glass* nearest(const std::string& vendor, double nd, double vd, double* distance = nullptr) const;

    private:
        std::vector<Glass> _glasses;
    };

    // LensTrace's Abbe number estimate for a glass of index nd when the file gives none.
    double abbeFor(double nd);
    // LensTrace's Cauchy dispersion n = nd + B (1 / lambda^2 - 1 / lambda_d^2), with B fixed by
    // nd and Vd through the F and C lines.
    double cauchyIor(double nd, double vd, double wavelengthNm);

    // Beyond this distance the nearest catalogue glass is a worse guess than Cauchy dispersion.
    constexpr double kGlassMatchTolerance = 0.1;

    // The dispersion of one medium: air, a catalogue glass, or Cauchy from nd and Vd. A matched
    // glass is a copy, so a Dispersion outlives its catalogue, and its curve is shifted by
    // glassOffset so that ior(kWavelengthDNm) is the prescription's nd.
    struct Dispersion {
        std::optional<Glass> glass;
        double glassOffset = 0.;
        double nd = 1.;
        double vd = 0.;

        bool isAir() const { return !glass && nd <= 1.; }
        double ior(double wavelengthNm) const;
    };

    // The medium for a surface's nd and Vd. nd <= 1 is air. The nearest catalogue glass within
    // tolerance lends its dispersion curve, offset to pass through nd at the d line. Vd 0 is estimated with abbeFor(nd).
    // abbeOffset is added to Vd before matching, and the Cauchy fallback uses the offset Vd too, so the
    // offset shifts dispersion whether or not a catalogue glass is found.
    Dispersion matchGlass(const GlassCatalogue& catalogue,
                          const std::string& vendor,
                          double nd,
                          double vd,
                          double abbeOffset,
                          double tolerance = kGlassMatchTolerance);

} // namespace Optics
} // namespace Natron

#endif // NATRON_ENGINE_OPTICS_GLASS_H
