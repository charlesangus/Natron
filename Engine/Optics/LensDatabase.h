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

#ifndef NATRON_ENGINE_OPTICS_LENSDATABASE_H
#define NATRON_ENGINE_OPTICS_LENSDATABASE_H

/*
 * The bundled lens database: the photographic lenses of OpticsDatabase (MIT, see
 * Gui/Resources/Optics/LICENSE-OpticsDatabase.txt), stripped and gzipped into lenses.zmx.gz by
 * tools/optics/convert_lenses.py, plus the built-in prescriptions as the "Classic Designs" vendor.
 *
 * Archive layout, after gunzip: a "#OPTICSDATABASE <commit> <url>" first line, then per lens a
 * "#LENSID <vendor>/<file stem>" line, a "#SOURCE <path in OpticsDatabase>" line and the lens's
 * .zmx lines up to the next #LENSID.
 */

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Engine/Optics/Prescription.h"

namespace Natron {
namespace Optics {

    // The vendor of the built-in prescriptions; their ids are "Classic Designs/<builtin id>".
    extern const char* const kClassicDesignsVendor;

    struct LensInfo {
        // "<vendor>/<file stem>".
        std::string id;
        std::string vendor;
        // The file stem, or the built-in's name.
        std::string name;
        // The file's NAME line, often a patent and example number; empty for a built-in.
        std::string title;
        // The file's NOTE lines or the built-in's citation.
        std::string source;
        // Path of the original file inside OpticsDatabase; empty for a built-in.
        std::string sourcePath;
        // Paraxial, in the y-z meridian for an anamorphic lens.
        double focalLength = 0.;
        // The file's FNUM when it has one, otherwise paraxial.
        double fNumber = 0.;
        // Surfaces followed by glass, so a cemented doublet counts two.
        int elementCount = 0;
        bool aspheric = false;
        // Any cylindrical surface.
        bool anamorphic = false;
    };

    class LensDatabase {
    public:
        // <directory of the running executable>/../Resources/optics/lenses.zmx.gz.
        static std::string defaultPath();

        // The instance nodes use, reading defaultPath() unless setSharedArchivePath() came first.
        static LensDatabase& shared();
        // False, changing nothing, once shared() has been called.
        static bool setSharedArchivePath(const std::string& path);

        explicit LensDatabase(std::string archivePath);
        LensDatabase(const LensDatabase&) = delete;
        LensDatabase& operator=(const LensDatabase&) = delete;

        // Reads and indexes the archive on the first call from any thread; later calls return the
        // first result. On failure the Classic Designs are still listed.
        bool load(std::string* error = nullptr);

        // Each accessor loads first. Classic Designs come first, then the archive in its order.
        const std::vector<LensInfo>& lenses();
        const LensInfo* find(const std::string& id);
        // Classic Designs first, then the rest sorted.
        std::vector<std::string> vendors();
        // The OpticsDatabase commit the archive was converted from.
        std::string sourceCommit();
        // "<id>: <error>" for archive lenses that failed to parse or have no paraxial focus; the
        // converter excludes those, so this is empty for a well-formed archive.
        std::vector<std::string> loadFailures();
        // How many times the archive has been read: 1 after any number of concurrent first loads.
        int archiveReads() const;

        // Parses the lens on first use and caches it. False with *error naming the id when the
        // database has no such lens.
        bool prescription(const std::string& id, Prescription* out, std::string* error);

    private:
        void loadOnce();

        std::string _archivePath;
        std::once_flag _loaded;
        bool _ok = false;
        std::string _error;
        int _reads = 0;
        std::string _commit;
        std::vector<LensInfo> _lenses;
        std::map<std::string, size_t> _index;
        // Each archive lens's .zmx text, by index into _lenses; empty for a built-in.
        std::vector<std::string> _text;
        std::vector<std::string> _failures;

        std::mutex _cacheMutex;
        std::map<std::string, std::shared_ptr<const Prescription>> _cache;
    };

    // True when every word of query is in the lens's id, name, title or source, case-insensitively; an
    // empty query matches every lens.
    bool lensMatches(const LensInfo& lens, const std::string& query);

} // namespace Optics
} // namespace Natron

#endif // NATRON_ENGINE_OPTICS_LENSDATABASE_H
