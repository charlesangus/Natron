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

#include "Engine/Optics/LensDatabase.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <utility>

#include <zlib.h>

#include "Engine/Optics/Paraxial.h"
#include "Engine/Optics/ZemaxParser.h"

namespace Natron {
namespace Optics {

    const char* const kClassicDesignsVendor = "Classic Designs";

    namespace {

        bool
        gunzip(const std::string& compressed,
               std::string* out,
               std::string* error)
        {
            z_stream stream {};
            // 16 + MAX_WBITS: a gzip header, not a raw zlib stream.
            if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
                *error = "cannot start zlib";

                return false;
            }
            stream.next_in = (Bytef*)compressed.data();
            stream.avail_in = (uInt)compressed.size();
            std::string result;
            char buffer[1 << 16];
            int status = Z_OK;
            while (status == Z_OK) {
                stream.next_out = (Bytef*)buffer;
                stream.avail_out = sizeof(buffer);
                status = inflate(&stream, Z_NO_FLUSH);
                if (status != Z_OK && status != Z_STREAM_END) {
                    break;
                }
                result.append(buffer, sizeof(buffer) - stream.avail_out);
            }
            inflateEnd(&stream);
            if (status != Z_STREAM_END) {
                *error = std::string("the archive is not valid gzip data") + (stream.msg ? std::string(": ") + stream.msg : std::string());

                return false;
            }
            *out = std::move(result);

            return true;
        }

        std::string
        lowered(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });

            return text;
        }

        bool
        startsWith(const std::string& text,
                   const char* prefix)
        {
            return text.rfind(prefix, 0) == 0;
        }

        bool
        describe(const Prescription& lens,
                 LensInfo* info,
                 std::string* error)
        {
            ParaxialFacts facts;
            if (!paraxialFacts(lens, &facts, error)) {
                return false;
            }
            info->focalLength = facts.effectiveFocalLength;
            info->fNumber = facts.fNumber;
            info->elementCount = 0;
            for (const Surface& s : lens.surfaces) {
                if (s.isGlass()) {
                    ++info->elementCount;
                }
            }
            info->aspheric = lens.hasAspheres();
            info->anamorphic = lens.hasCylinders();
            if (info->source.empty()) {
                info->source = lens.source;
            }

            return true;
        }

        std::mutex&
        sharedMutex()
        {
            static std::mutex m;

            return m;
        }

        std::string&
        sharedPath()
        {
            static std::string path;

            return path;
        }

        bool&
        sharedCreated()
        {
            static bool created = false;

            return created;
        }

    } // namespace

    std::string
    LensDatabase::defaultPath()
    {
        std::error_code ec;
        const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (ec) {
            return std::string();
        }

        return (exe.parent_path() / ".." / "Resources" / "optics" / "lenses.zmx.gz").lexically_normal().string();
    }

    LensDatabase&
    LensDatabase::shared()
    {
        std::string path;
        {
            std::lock_guard<std::mutex> lock(sharedMutex());
            sharedCreated() = true;
            path = sharedPath().empty() ? defaultPath() : sharedPath();
        }
        static LensDatabase database(path);

        return database;
    }

    bool
    LensDatabase::setSharedArchivePath(const std::string& path)
    {
        std::lock_guard<std::mutex> lock(sharedMutex());
        if (sharedCreated()) {
            return false;
        }
        sharedPath() = path;

        return true;
    }

    LensDatabase::LensDatabase(std::string archivePath)
        : _archivePath(std::move(archivePath))
    {
    }

    bool
    LensDatabase::load(std::string* error)
    {
        std::call_once(_loaded, [this] { loadOnce(); });
        if (!_ok && error) {
            *error = _error;
        }

        return _ok;
    }

    void
    LensDatabase::loadOnce()
    {
        for (const std::string& builtinId : builtinLensIds()) {
            Prescription lens;
            builtinLens(builtinId, &lens);
            LensInfo info;
            info.id = std::string(kClassicDesignsVendor) + "/" + builtinId;
            info.vendor = kClassicDesignsVendor;
            info.name = lens.name;
            std::string error;
            if (!describe(lens, &info, &error)) {
                _failures.push_back(info.id + ": " + error);
                continue;
            }
            _index[info.id] = _lenses.size();
            _lenses.push_back(info);
            _text.emplace_back();
        }

        ++_reads;
        std::ifstream file(_archivePath, std::ios::binary);
        if (!file) {
            _error = "cannot open the lens database " + _archivePath;

            return;
        }
        const std::string compressed((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::string text;
        std::string error;
        if (!gunzip(compressed, &text, &error)) {
            _error = _archivePath + ": " + error;

            return;
        }

        std::istringstream in(text);
        std::string line;
        if (!std::getline(in, line) || !startsWith(line, "#OPTICSDATABASE ")) {
            _error = _archivePath + ": no #OPTICSDATABASE header";

            return;
        }
        {
            std::istringstream header(line.substr(16));
            header >> _commit;
        }

        struct Pending {
            LensInfo info;
            std::string text;
        };
        std::vector<Pending> pending;
        while (std::getline(in, line)) {
            if (startsWith(line, "#LENSID ")) {
                pending.emplace_back();
                const std::string id = line.substr(8);
                pending.back().info.id = id;
                const size_t slash = id.find('/');
                pending.back().info.vendor = slash == std::string::npos ? std::string() : id.substr(0, slash);
                pending.back().info.name = slash == std::string::npos ? id : id.substr(slash + 1);
            } else if (pending.empty()) {
                continue;
            } else if (startsWith(line, "#SOURCE ")) {
                pending.back().info.sourcePath = line.substr(8);
            } else {
                pending.back().text += line;
                pending.back().text += '\n';
            }
        }

        for (Pending& p : pending) {
            if (p.info.vendor.empty() || _index.count(p.info.id)) {
                _failures.push_back(p.info.id + ": " + (p.info.vendor.empty() ? "no vendor in the id" : "a duplicate id"));
                continue;
            }
            Prescription lens;
            LensFileError parseError;
            if (!parseZemax(p.text, &lens, &parseError)) {
                _failures.push_back(p.info.id + ": " + parseError.text());
                continue;
            }
            p.info.title = lens.name;
            if (!describe(lens, &p.info, &error)) {
                _failures.push_back(p.info.id + ": " + error);
                continue;
            }
            _index[p.info.id] = _lenses.size();
            _lenses.push_back(std::move(p.info));
            _text.push_back(std::move(p.text));
        }
        _ok = true;
    }

    const std::vector<LensInfo>&
    LensDatabase::lenses()
    {
        load();

        return _lenses;
    }

    const LensInfo*
    LensDatabase::find(const std::string& id)
    {
        load();
        const auto it = _index.find(id);

        return it == _index.end() ? nullptr : &_lenses[it->second];
    }

    std::vector<std::string>
    LensDatabase::vendors()
    {
        load();
        std::vector<std::string> rest;
        bool classic = false;
        for (const LensInfo& lens : _lenses) {
            if (lens.vendor == kClassicDesignsVendor) {
                classic = true;
            } else if (std::find(rest.begin(), rest.end(), lens.vendor) == rest.end()) {
                rest.push_back(lens.vendor);
            }
        }
        std::sort(rest.begin(), rest.end());
        if (classic) {
            rest.insert(rest.begin(), kClassicDesignsVendor);
        }

        return rest;
    }

    std::string
    LensDatabase::sourceCommit()
    {
        load();

        return _commit;
    }

    std::vector<std::string>
    LensDatabase::loadFailures()
    {
        load();

        return _failures;
    }

    int
    LensDatabase::archiveReads() const
    {
        return _reads;
    }

    bool
    LensDatabase::prescription(const std::string& id,
                               Prescription* out,
                               std::string* error)
    {
        load();
        const auto it = _index.find(id);
        if (it == _index.end()) {
            *error = "the lens database has no lens \"" + id + "\"" + (_ok ? std::string() : " (" + _error + ")");

            return false;
        }
        {
            std::lock_guard<std::mutex> lock(_cacheMutex);
            const auto cached = _cache.find(id);
            if (cached != _cache.end()) {
                *out = *cached->second;

                return true;
            }
        }
        const LensInfo& info = _lenses[it->second];
        Prescription lens;
        if (info.vendor == kClassicDesignsVendor) {
            builtinLens(id.substr(info.vendor.size() + 1), &lens);
        } else {
            LensFileError parseError;
            if (!parseZemax(_text[it->second], &lens, &parseError)) {
                *error = id + ": " + parseError.text();

                return false;
            }
            if (lens.name.empty()) {
                lens.name = info.name;
            }
        }
        lens.id = id;
        {
            std::lock_guard<std::mutex> lock(_cacheMutex);
            _cache.emplace(id, std::make_shared<const Prescription>(lens));
        }
        *out = lens;

        return true;
    }

    bool
    lensMatches(const LensInfo& lens,
                const std::string& query)
    {
        const std::string haystack = lowered(lens.id + "\n" + lens.name + "\n" + lens.title + "\n" + lens.source);
        std::istringstream words(lowered(query));
        std::string word;
        while (words >> word) {
            if (haystack.find(word) == std::string::npos) {
                return false;
            }
        }

        return true;
    }

} // namespace Optics
} // namespace Natron
