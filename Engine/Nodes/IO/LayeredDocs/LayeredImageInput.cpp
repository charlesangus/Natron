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

#include "LayeredImageInput.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <utility>

#include "Engine/FileStampRegistry.h"

NATRON_NAMESPACE_ENTER

namespace LayeredDocs {
namespace {
    // OIIO takes a plain function pointer as a format's creator, so each format gets a slot whose
    // creator knows the slot's index.
    const int kSlots = 16;

    struct Registry {
        std::mutex mutex;
        std::array<Format, kSlots> slots;
        std::map<std::string, int> slotOf;
        std::uint64_t generation = 0;
    };

    Registry&
    registry()
    {
        static Registry instance;

        return instance;
    }

    Format
    formatInSlot(int slot)
    {
        Registry& r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);

        return r.slots[(std::size_t)slot];
    }

    struct CachedDocument {
        FileIdentity identity;
        std::string format;
        std::shared_ptr<const LayeredDocument> document;
        std::uint64_t lastUse = 0;
    };

    // Documents hold their decoded planes and their readers' buffers, so the cache keeps at most
    // this many of them and, beyond the most recently used one, at most this many bytes.
    const std::size_t kMaxCachedDocuments = 8;
    const std::size_t kMaxCachedDocumentBytes = (std::size_t)512 << 20;

    std::mutex g_documentsMutex;
    std::map<std::string, CachedDocument> g_documents;
    std::uint64_t g_documentUses = 0;

    // Drops the least recently used documents until the cache fits its bounds. Inputs still
    // reading a dropped document keep it. Call with g_documentsMutex held.
    void
    trimDocumentsLocked()
    {
        while (g_documents.size() > 1) {
            std::size_t bytes = 0;
            std::map<std::string, CachedDocument>::iterator oldest = g_documents.begin();
            for (std::map<std::string, CachedDocument>::iterator it = g_documents.begin(); it != g_documents.end(); ++it) {
                bytes += it->second.document->residentBytes();
                if (it->second.lastUse < oldest->second.lastUse) {
                    oldest = it;
                }
            }
            if (g_documents.size() <= kMaxCachedDocuments && bytes <= kMaxCachedDocumentBytes) {
                return;
            }
            g_documents.erase(oldest);
        }
    }

    void
    trimDocuments()
    {
        std::lock_guard<std::mutex> lock(g_documentsMutex);

        trimDocumentsLocked();
    }

    std::shared_ptr<const LayeredDocument>
    documentOf(const std::string& path,
               const Format& format,
               std::string* error)
    {
        const FileIdentity identity = FileStampRegistry::statNow(path);

        if (!identity.exists) {
            *error = "Could not read " + path;

            return std::shared_ptr<const LayeredDocument>();
        }
        {
            std::lock_guard<std::mutex> lock(g_documentsMutex);
            const std::map<std::string, CachedDocument>::iterator it = g_documents.find(path);
            if (it != g_documents.end() && it->second.identity == identity && it->second.format == format.name) {
                it->second.lastUse = ++g_documentUses;

                return it->second.document;
            }
        }

        const std::shared_ptr<const LayeredDocument> document = format.read(path, error);
        if (!document) {
            if (error->empty()) {
                *error = "Could not read " + path;
            }

            return document;
        }
        if (FileStampRegistry::statNow(path) == identity) {
            std::lock_guard<std::mutex> lock(g_documentsMutex);
            CachedDocument& entry = g_documents[path];
            entry.identity = identity;
            entry.format = format.name;
            entry.document = document;
            entry.lastUse = ++g_documentUses;
            trimDocumentsLocked();
        }

        return document;
    }

    class Input
        : public OIIO::ImageInput {
    public:
        explicit Input(int slot)
            : _slot(slot)
            , _name(formatInSlot(slot).name)
        {
        }

        ~Input() override
        {
            close();
        }

        const char* format_name() const override
        {
            return _name.c_str();
        }

        int supports(OIIO::string_view feature) const override
        {
            return feature == "multiimage" || feature == "arbitrary_metadata";
        }

        bool open(const std::string& name,
                  OIIO::ImageSpec& newspec) override
        {
            const Format format = formatInSlot(_slot);

            if (!format.read) {
                errorfmt("{} is not a registered layered document format", _name);

                return false;
            }
            std::string error;
            _document = documentOf(name, format, &error);
            if (!_document) {
                errorfmt("{}", error);

                return false;
            }
            _subimage = 0;
            m_spec = _document->specOf(0);
            newspec = m_spec;

            return true;
        }

        bool close() override
        {
            if (_document) {
                _document.reset();
                trimDocuments();
            }
            _subimage = -1;

            return true;
        }

        int current_subimage() const override
        {
            return _subimage;
        }

        bool seek_subimage(int subimage,
                           int miplevel) override
        {
            if (!_document || miplevel != 0 || subimage < 0 || subimage >= _document->subimageCount()) {
                return false;
            }
            if (subimage != _subimage) {
                _subimage = subimage;
                m_spec = _document->specOf(subimage);
            }

            return true;
        }

        using OIIO::ImageInput::read_native_scanlines;
        using OIIO::ImageInput::read_native_tiles;

        bool read_native_scanline(int subimage,
                                  int miplevel,
                                  int y,
                                  int z,
                                  void* data) override
        {
            return read_native_scanlines(subimage, miplevel, y, y + 1, z, data);
        }

        bool read_native_scanlines(int subimage,
                                   int miplevel,
                                   int ybegin,
                                   int yend,
                                   int /*z*/,
                                   void* data) override
        {
            std::lock_guard<OIIO::ImageInput> lock(*this);

            if (!seek_subimage(subimage, miplevel)) {
                errorfmt("No subimage {} in this document", subimage);

                return false;
            }

            return readInto(subimage, m_spec.x, m_spec.x + m_spec.width, ybegin, yend, data);
        }

        bool read_native_tile(int subimage,
                              int miplevel,
                              int x,
                              int y,
                              int /*z*/,
                              void* data) override
        {
            std::lock_guard<OIIO::ImageInput> lock(*this);

            if (!seek_subimage(subimage, miplevel) || m_spec.tile_width <= 0 || m_spec.tile_height <= 0) {
                errorfmt("No tiled subimage {} in this document", subimage);

                return false;
            }

            return readInto(subimage, x, x + m_spec.tile_width, y, y + m_spec.tile_height, data);
        }

        bool read_native_tiles(int subimage,
                               int miplevel,
                               int xbegin,
                               int xend,
                               int ybegin,
                               int yend,
                               int /*zbegin*/,
                               int /*zend*/,
                               void* data) override
        {
            std::lock_guard<OIIO::ImageInput> lock(*this);

            if (!seek_subimage(subimage, miplevel) || m_spec.tile_width <= 0 || m_spec.tile_height <= 0) {
                errorfmt("No tiled subimage {} in this document", subimage);

                return false;
            }

            return readInto(subimage, xbegin, xend, ybegin, yend, data);
        }

    private:
        // Fills `data`, laid out as columns [x1, x2) by rows [y1, y2), from the part of that
        // rectangle inside the current subimage's data window. An edge tile reaches past the
        // window; its outside is zero.
        bool readInto(int subimage,
                      int x1,
                      int x2,
                      int y1,
                      int y2,
                      void* data)
        {
            const int cx1 = std::max(x1, m_spec.x);
            const int cy1 = std::max(y1, m_spec.y);
            const int cx2 = std::min(x2, m_spec.x + m_spec.width);
            const int cy2 = std::min(y2, m_spec.y + m_spec.height);
            std::string error;

            if (x2 < x1 || y2 < y1) {
                errorfmt("An empty region was asked for");

                return false;
            }
            if (cx1 == x1 && cy1 == y1 && cx2 == x2 && cy2 == y2) {
                if (!_document->readRegion(subimage, x1, y1, x2, y2, data, &error)) {
                    errorfmt("{}", error);

                    return false;
                }

                return true;
            }

            const std::size_t pixelBytes = m_spec.pixel_bytes(true);
            const std::size_t rowBytes = (std::size_t)(x2 - x1) * pixelBytes;
            unsigned char* out = static_cast<unsigned char*>(data);
            std::memset(out, 0, rowBytes * (std::size_t)(y2 - y1));
            if (cx2 <= cx1 || cy2 <= cy1) {
                return true;
            }
            const std::size_t partRowBytes = (std::size_t)(cx2 - cx1) * pixelBytes;
            std::vector<unsigned char> part(partRowBytes * (std::size_t)(cy2 - cy1));
            if (!_document->readRegion(subimage, cx1, cy1, cx2, cy2, part.data(), &error)) {
                errorfmt("{}", error);

                return false;
            }
            for (int y = cy1; y < cy2; ++y) {
                std::memcpy(out + (std::size_t)(y - y1) * rowBytes + (std::size_t)(cx1 - x1) * pixelBytes, part.data() + (std::size_t)(y - cy1) * partRowBytes, partRowBytes);
            }

            return true;
        }

        int _slot;
        std::string _name;
        std::shared_ptr<const LayeredDocument> _document;
        int _subimage = -1;
    };

    template <int N>
    OIIO::ImageInput*
    createInput()
    {
        return new Input(N);
    }

    template <std::size_t... I>
    constexpr std::array<OIIO::ImageInput::Creator, sizeof...(I)>
    creatorsOf(std::index_sequence<I...>)
    {
        return { &createInput<(int)I>... };
    }

    const std::array<OIIO::ImageInput::Creator, kSlots> kCreators = creatorsOf(std::make_index_sequence<kSlots>());
} // anonymous namespace

bool
registerFormat(const Format& format,
               std::string* error)
{
    (void)OIIO::get_string_attribute("format_list");

    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    std::map<std::string, int>::const_iterator it = r.slotOf.find(format.name);
    if (it == r.slotOf.end()) {
        if (format.name.empty() || OIIO::is_imageio_format_name(format.name)) {
            if (error) {
                *error = "\"" + format.name + "\" is already an image format";
            }

            return false;
        }
        const int slot = (int)r.slotOf.size();
        if (slot >= kSlots) {
            if (error) {
                *error = "No slot is left for the layered document format " + format.name;
            }

            return false;
        }
        std::vector<const char*> extensions;
        for (const std::string& ext : format.extensions) {
            extensions.push_back(ext.c_str());
        }
        extensions.push_back(nullptr);
        r.slots[(std::size_t)slot] = format;
        OIIO::declare_imageio_format(format.name, kCreators[(std::size_t)slot], extensions.data(), nullptr, nullptr, "Natron");
        it = r.slotOf.insert(std::make_pair(format.name, slot)).first;
    }
    r.slots[(std::size_t)it->second] = format;
    ++r.generation;

    return true;
}

void
unregisterFormat(const std::string& name)
{
    {
        Registry& r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        const std::map<std::string, int>::const_iterator it = r.slotOf.find(name);
        if (it == r.slotOf.end()) {
            return;
        }
        r.slots[(std::size_t)it->second].extensions.clear();
        r.slots[(std::size_t)it->second].read = nullptr;
        ++r.generation;
    }
    clearDocumentCache();
}

void
registerBuiltinFormats()
{
    static std::once_flag once;

    std::call_once(once, []() {
        Format ora;
        ora.name = "natron_ora";
        ora.extensions = { "ora" };
        ora.read = readOra;
        registerFormat(ora, nullptr);

        Format psd;
        psd.name = "natron_psd";
        psd.extensions = { "psd", "psb", "pdd" };
        psd.read = readPsd;
        registerFormat(psd, nullptr);

        Format kra;
        kra.name = "natron_kra";
        kra.extensions = { "kra" };
        kra.read = readKra;
        registerFormat(kra, nullptr);

        Format xcf;
        xcf.name = "natron_xcf";
        xcf.extensions = { "xcf" };
        xcf.read = readXcf;
        registerFormat(xcf, nullptr);
    });
}

bool
isFormatName(const std::string& name)
{
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);

    return r.slotOf.count(name) > 0;
}

std::map<std::string, std::string>
extensionClaims()
{
    registerBuiltinFormats();

    std::map<std::string, std::string> claims;
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    for (const Format& format : r.slots) {
        if (!format.read) {
            continue;
        }
        for (const std::string& ext : format.extensions) {
            claims.insert(std::make_pair(ext, format.name));
        }
    }

    return claims;
}

std::uint64_t
registryGeneration()
{
    registerBuiltinFormats();

    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);

    return r.generation;
}

std::string
formatForExtension(const std::string& extension)
{
    const std::map<std::string, std::string> claims = extensionClaims();
    const std::map<std::string, std::string>::const_iterator it = claims.find(extension);

    return it == claims.end() ? std::string() : it->second;
}

OIIO::ImageInput::unique_ptr
open(const std::string& path,
     const std::string& formatName,
     std::string* error)
{
    registerBuiltinFormats();

    OIIO::ImageInput::unique_ptr input = OIIO::ImageInput::create(formatName);
    if (!input) {
        const std::string detail = OIIO::geterror();
        *error = detail.empty() ? "No reader for " + formatName : detail;

        return input;
    }
    OIIO::ImageSpec spec;
    if (!input->open(path, spec)) {
        const std::string detail = input->geterror();
        *error = detail.empty() ? "Could not open " + path : detail;
        input.reset();
    }

    return input;
}

void
clearDocumentCache()
{
    std::lock_guard<std::mutex> lock(g_documentsMutex);

    g_documents.clear();
}
} // namespace LayeredDocs

NATRON_NAMESPACE_EXIT
