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

/*
 * The DNG colour matrix selection and the linear response limit are ported from darktable,
 * DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7): the illuminant table and the d65_color_matrix
 * selection of src/common/exif.cc (Copyright (C) 2009-2026 darktable developers), here read
 * through rawspeed's TIFF parser instead of Exiv2. darktable is distributed under the GNU GPL
 * version 3 or later, so this file is too.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "RawDecodeSupport.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <future>
#include <list>
#include <map>
#include <mutex>
#include <system_error>
#include <utility>
#include <vector>

#include <QDebug>

#include "RawSpeed-API.h"
#include "tiff/TiffEntry.h"
#include "tiff/TiffIFD.h"
#include "tiff/TiffTag.h"

#include "Engine/AppManager.h"
#include "Engine/MemoryInfo.h"
#include "Engine/Nodes/IO/RawDngOpcodes.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/IO/RawspeedRuntime.h"
#include "Engine/Nodes/Raw/RawColorMath.h"
#include "Engine/Settings.h"

NATRON_NAMESPACE_ENTER

namespace RawDecodeSupport {
namespace {
    // The share of the project's RAM cache the decoded files may hold.
    const double kDecodeCacheShare = 0.25;

    // When no Settings exist (a tool or a test without an app), the project cache is taken to be
    // the default share of RAM.
    const double kFallbackRamShare = 0.5;

    const std::size_t kMaxCachedHeaders = 4096;

    struct FileKey {
        std::string path;
        std::filesystem::file_time_type mtime;
        std::uintmax_t size = 0;

        bool operator==(const FileKey& other) const
        {
            return path == other.path && mtime == other.mtime && size == other.size;
        }
    };

    struct Decoded {
        FileKey key;
        rawspeed::RawImage image;
        std::shared_ptr<const Header> header;
        // The active area's top-left in the file's top-down, uncropped layout.
        int cropX = 0;
        int cropY = 0;
        int uncroppedHeight = 0;
        int cpp = 1;
        bool isFloat = false;
        std::size_t bytes = 0;

        explicit Decoded(rawspeed::RawImage raw)
            : key()
            , image(std::move(raw))
        {
        }
    };

    struct Outcome {
        std::shared_ptr<const Decoded> decoded;
        std::string error;
    };

    struct HeaderEntry {
        std::filesystem::file_time_type mtime;
        std::uintmax_t size = 0;
        std::shared_ptr<const Header> header;
    };

    std::mutex g_mutex;
    std::map<std::string, HeaderEntry> g_headers;
    // Most recently used first.
    std::list<std::shared_ptr<const Decoded>> g_decoded;
    std::size_t g_decodedBytes = 0;
    std::list<std::pair<FileKey, std::shared_future<Outcome>>> g_inFlight;
    // A decode that straddles an eviction may describe the file as it was before, so it is only
    // cached when no eviction intervened.
    std::uint64_t g_evictions = 0;
    std::optional<std::size_t> g_capacityOverride;
    std::function<void(const std::string&)> g_decodedHook;

    bool
    fail(std::string* error,
         const std::string& message)
    {
        if (error) {
            *error = message;
        }

        return false;
    }

    bool
    statFile(const std::string& path,
             FileKey* key,
             std::string* error)
    {
        std::error_code ec;
        const std::filesystem::path fsPath(path);

        key->path = path;
        key->mtime = std::filesystem::last_write_time(fsPath, ec);
        if (!ec) {
            key->size = std::filesystem::file_size(fsPath, ec);
        }
        if (ec) {
            return fail(error, "Could not read " + path + ": " + ec.message());
        }

        return true;
    }

    char
    cfaLetter(rawspeed::CFAColor colour)
    {
        switch (colour) {
        case rawspeed::CFAColor::RED:
            return 'R';
        case rawspeed::CFAColor::GREEN:
        case rawspeed::CFAColor::FUJI_GREEN:
            return 'G';
        case rawspeed::CFAColor::BLUE:
            return 'B';
        default:
            return 0;
        }
    }

    // rawspeed has no exception type of its own for an unsupported camera; these are the texts
    // its camera support checks throw.
    bool
    isUnsupportedCamera(const std::string& what)
    {
        return what.find("not supported") != std::string::npos || what.find("Unsupported") != std::string::npos;
    }

    void
    identify(rawspeed::RawDecoder& decoder,
             std::string* make,
             std::string* model)
    {
        const rawspeed::ImageMetaData& meta = decoder.mRaw->metadata;

        *make = meta.canonical_make.empty() ? meta.make : meta.canonical_make;
        *model = meta.canonical_model.empty() ? meta.model : meta.canonical_model;
        if (!make->empty() || !model->empty()) {
            return;
        }
        try {
            if (const rawspeed::TiffRootIFD* root = dynamic_cast<const rawspeed::TiffRootIFD*>(decoder.getRootIFD())) {
                const rawspeed::TiffID id = root->getID();
                *make = id.make;
                *model = id.model;
            }
        } catch (const std::exception&) {
        }
    }

    std::optional<double>
    exposureTime(rawspeed::RawDecoder& decoder)
    {
        try {
            const rawspeed::TiffIFD* root = decoder.getRootIFD();
            const rawspeed::TiffEntry* entry = root ? root->getEntryRecursive(rawspeed::TiffTag::EXPOSURETIME) : nullptr;
            if (entry) {
                const double seconds = entry->getFloat();
                if (std::isfinite(seconds) && seconds > 0.) {
                    return seconds;
                }
            }
        } catch (const std::exception&) {
        }

        return std::nullopt;
    }

    std::optional<double>
    linearResponseLimit(rawspeed::RawDecoder& decoder)
    {
        try {
            const rawspeed::TiffIFD* root = decoder.getRootIFD();
            const rawspeed::TiffEntry* entry = root ? root->getEntryRecursive(rawspeed::TiffTag::LINEARRESPONSELIMIT) : nullptr;
            if (entry && entry->count == 1) {
                const double limit = entry->getFloat();
                if (std::isfinite(limit)) {
                    return limit;
                }
            }
        } catch (const std::exception&) {
        }

        return std::nullopt;
    }

    // rawspeed parses a makernote into a TiffRootIFD under the IFD holding the MakerNote tag. A
    // TiffRootIFD under another root is an embedded file's tree (a RAF's JPEG), not a makernote.
    void
    collectMakerNotes(const rawspeed::TiffIFD* ifd,
                      bool parentIsRoot,
                      bool isTop,
                      std::vector<const rawspeed::TiffIFD*>* notes)
    {
        const bool isRoot = dynamic_cast<const rawspeed::TiffRootIFD*>(ifd) != nullptr;
        if (isRoot && !isTop && !parentIsRoot) {
            notes->push_back(ifd);
        }
        for (const rawspeed::TiffIFDOwner& sub : ifd->getSubIFDs()) {
            collectMakerNotes(sub.get(), isRoot, false, notes);
        }
    }

    std::optional<long>
    makerNoteValue(const std::vector<const rawspeed::TiffIFD*>& notes,
                   uint16_t tag,
                   uint32_t index)
    {
        const rawspeed::TiffTag key = static_cast<rawspeed::TiffTag>(tag);
        for (const rawspeed::TiffIFD* note : notes) {
            if (!note->hasEntry(key)) {
                continue;
            }
            const rawspeed::TiffEntry* entry = note->getEntry(key);
            if (index >= entry->count) {
                return std::nullopt;
            }
            switch (entry->type) {
            case rawspeed::TiffDataType::BYTE:
            case rawspeed::TiffDataType::UNDEFINED:
                return (long)entry->getByte(index);
            case rawspeed::TiffDataType::SHORT:
                return (long)entry->getU16(index);
            case rawspeed::TiffDataType::SSHORT:
                return (long)entry->getI16(index);
            case rawspeed::TiffDataType::LONG:
                return (long)entry->getU32(index);
            case rawspeed::TiffDataType::SLONG:
                return (long)entry->getI32(index);
            default:
                return std::nullopt;
            }
        }

        return std::nullopt;
    }

    // darktable's _check_highlight_preservation (src/common/exif.cc), on the makernote of the
    // camera's make, with its order: the first tag found decides, even when its value adds
    // nothing (a Nikon's ColorSpace hides its ActiveDLighting). Olympus keeps Gradation in a
    // makernote sub-IFD and Canon CR3 in a CMT box, neither of which rawspeed parses, and darktable
    // rates both below one EV, which its denoise ignores.
    std::optional<double>
    highlightPreservation(rawspeed::RawDecoder& decoder,
                          const std::string& make)
    {
        try {
            const rawspeed::TiffIFD* root = decoder.getRootIFD();
            if (!root) {
                return std::nullopt;
            }
            std::vector<const rawspeed::TiffIFD*> notes;
            collectMakerNotes(root, false, true, &notes);
            if (notes.empty()) {
                return std::nullopt;
            }
            std::string lower(make);
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (lower.find("canon") != std::string::npos) {
                if (const std::optional<long> state = makerNoteValue(notes, 0x4018, 2)) {
                    return (*state == 0) ? 0.50 : (*state == 1) ? 0.33
                        : (*state == 2)                         ? 0.66
                                                                : 0.;
                }
            } else if (lower.find("fuji") != std::string::npos) {
                std::optional<long> dr = makerNoteValue(notes, 0x1403, 0);
                if (!dr) {
                    dr = makerNoteValue(notes, 0x140b, 0);
                }
                if (dr) {
                    return (*dr == 200) ? 1. : (*dr == 400) ? 2.
                                                            : 0.;
                }
            } else if (lower.find("nikon") != std::string::npos) {
                if (const std::optional<long> colorSpace = makerNoteValue(notes, 0x001e, 0)) {
                    return (*colorSpace == 4) ? 2. : 0.;
                }
                if (const std::optional<long> adl = makerNoteValue(notes, 0x0022, 0)) {
                    switch (*adl) {
                    case 3:
                        return 0.33;
                    case 5:
                        return 0.66;
                    case 7:
                        return 1.00;
                    case 8:
                        return 1.10;
                    case 9:
                        return 1.20;
                    case 10:
                        return 1.30;
                    case 11:
                        return 1.33;
                    default:
                        return 0.;
                    }
                }
            } else if (lower.find("pentax") != std::string::npos || lower.find("ricoh") != std::string::npos) {
                if (const std::optional<long> expansion = makerNoteValue(notes, 0x0069, 0)) {
                    return (*expansion == 1) ? 1. : 0.;
                }
            }
        } catch (const std::exception&) {
        }

        return std::nullopt;
    }

    // rawspeed never applies GainMap opcodes, so the OpcodeList2 bytes are parsed here. A list that
    // does not parse leaves the key out.
    std::vector<DngGainMap>
    gainMaps(rawspeed::RawDecoder& decoder)
    {
        try {
            const rawspeed::TiffIFD* root = decoder.getRootIFD();
            const rawspeed::TiffEntry* entry = root ? root->getEntryRecursive(rawspeed::TiffTag::OPCODELIST2) : nullptr;
            if (!entry || (entry->type != rawspeed::TiffDataType::UNDEFINED && entry->type != rawspeed::TiffDataType::BYTE)) {
                return {};
            }
            std::vector<uint8_t> bytes(entry->count);
            for (uint32_t i = 0; i < entry->count; ++i) {
                bytes[i] = entry->getByte(i);
            }
            std::string error;
            if (std::optional<std::vector<DngGainMap>> maps = RawDngOpcodes::parseOpcodeList2GainMaps(bytes.data(), bytes.size(), &error)) {
                return std::move(*maps);
            }
            qWarning() << "Ignoring the DNG OpcodeList2:" << error.c_str();
        } catch (const std::exception&) {
        }

        return {};
    }

    // DNG 1.6 tags that rawspeed's TiffTag does not name.
    const rawspeed::TiffTag kCalibrationIlluminant3 = static_cast<rawspeed::TiffTag>(0xCD31);
    const rawspeed::TiffTag kColorMatrix3 = static_cast<rawspeed::TiffTag>(0xCD32);
    const rawspeed::TiffTag kCameraCalibration3 = static_cast<rawspeed::TiffTag>(0xCD33);

    // The DNG SDK's EXIF LightSource numbering, as darktable's dt_dng_illuminant_t indexes it,
    // with the Bradford adaptation from each illuminant to D65.
    struct Illuminant {
        int temperature;
        std::array<float, 9> adaptation;
    };

    const int kIlluminantUnknown = 0;
    const int kIlluminantD65 = 21;
    const int kIlluminantOther = 25;

    const std::array<Illuminant, kIlluminantOther + 1>&
    illuminants()
    {
        static const std::array<Illuminant, kIlluminantOther + 1> table = { {
            { 0, { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f } },
            { 5500, { 0.9726856f, -0.0135482f, 0.0361731f, -0.0167463f, 1.0049102f, 0.0120598f, 0.0070026f, -0.0116372f, 1.1869548f } },
            { 4230, { 0.9212269f, -0.0449128f, 0.1211620f, -0.0553723f, 1.0277243f, 0.0403563f, 0.0235086f, -0.0391019f, 1.6390644f } },
            { 2850, { 0.8446965f, -0.1179225f, 0.3948108f, -0.1366303f, 1.1041226f, 0.1291718f, 0.0798489f, -0.1348999f, 3.1924009f } },
            { 5500, { 0.9726856f, -0.0135482f, 0.0361731f, -0.0167463f, 1.0049102f, 0.0120598f, 0.0070026f, -0.0116372f, 1.1869548f } },
            { 0, { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f } },
            { 0, { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f } },
            { 0, { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f } },
            { 0, { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f } },
            { 5500, { 0.9726856f, -0.0135482f, 0.0361731f, -0.0167463f, 1.0049102f, 0.0120598f, 0.0070026f, -0.0116372f, 1.1869548f } },
            { 6500, { 1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f } },
            { 7500, { 1.0206905f, 0.0091588f, -0.0228796f, 0.0115005f, 0.9984917f, -0.0076762f, -0.0043619f, 0.0072053f, 0.8853432f } },
            { 6430, { 1.0096114f, 0.0061501f, 0.0068113f, 0.0102539f, 0.9888663f, 0.0015575f, 0.0023119f, -0.0044823f, 1.0525915f } },
            { 5000, { 0.9554129f, -0.0231280f, 0.0637169f, -0.0283629f, 1.0099053f, 0.0211824f, 0.0124188f, -0.0206922f, 1.3330592f } },
            { 4150, { 0.9147843f, -0.0492842f, 0.1202810f, -0.0622085f, 1.034984f, 0.0404480f, 0.0228014f, -0.0375807f, 1.6259804f } },
            { 3450, { 0.8805388f, -0.0774890f, 0.2293784f, -0.0932136f, 1.0589267f, 0.0757827f, 0.0453660f, -0.0760107f, 2.2417979f } },
            { 2940, { 0.8488316f, -0.1107439f, 0.3471428f, -0.1310107f, 1.0986874f, 0.1141548f, 0.0694025f, -0.1167541f, 2.9109462f } },
            { 2850, { 0.8446965f, -0.1179225f, 0.3948108f, -0.1366303f, 1.1041226f, 0.1291718f, 0.0798489f, -0.1348999f, 3.1924009f } },
            { 4871, { 0.9415037f, -0.0321240f, 0.0584672f, -0.0428238f, 1.0250998f, 0.0203309f, 0.0101511f, -0.0161170f, 1.2847354f } },
            { 6774, { 0.9904476f, -0.0071683f, -0.0116156f, -0.0123712f, 1.0155950f, -0.0029282f, -0.0035635f, 0.0067697f, 0.9181569f } },
            { 5500, { 0.9726856f, -0.0135482f, 0.0361731f, -0.0167463f, 1.0049102f, 0.0120598f, 0.0070026f, -0.0116372f, 1.1869548f } },
            { 6500, { 1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f } },
            { 7500, { 1.0206905f, 0.0091588f, -0.0228796f, 0.0115005f, 0.9984917f, -0.0076762f, -0.0043619f, 0.0072053f, 0.8853432f } },
            { 5000, { 0.9555766f, -0.0230393f, 0.0631636f, -0.0282895f, 1.0099416f, 0.0210077f, 0.0122982f, -0.0204830f, 1.3299098f } },
            { 3200, { 0.8663030f, -0.0913083f, 0.2771784f, -0.1090504f, 1.0746895f, 0.0913841f, 0.0550856f, -0.0924636f, 2.5119387f } },
            { 6500, { 1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f } },
        } };

        return table;
    }

    using Mat9f = std::array<float, 9>;

    Mat9f
    identity9()
    {
        return { 1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f };
    }

    // Single precision like darktable's mat3mul(), so the product matches its logged matrix.
    Mat9f
    multiply9(const Mat9f& a,
              const Mat9f& b)
    {
        Mat9f out;

        for (int k = 0; k < 3; ++k) {
            for (int i = 0; i < 3; ++i) {
                float x = 0.f;
                for (int j = 0; j < 3; ++j) {
                    x += a[(std::size_t)(3 * k + j)] * b[(std::size_t)(3 * j + i)];
                }
                out[(std::size_t)(3 * k + i)] = x;
            }
        }

        return out;
    }

    std::optional<Mat9f>
    readMatrix(const rawspeed::TiffIFD* root,
               rawspeed::TiffTag tag)
    {
        const rawspeed::TiffEntry* entry = root->getEntryRecursive(tag);

        if (!entry || entry->count != 9) {
            return std::nullopt;
        }
        Mat9f out;
        for (uint32_t i = 0; i < 9; ++i) {
            out[i] = entry->getFloat(i);
        }

        return out;
    }

    // The embedded DNG colour matrix for D65, as darktable's dt_exif_read_exif_data() builds
    // d65_color_matrix: the colour matrix of the illuminant nearest above D65 (the first one with
    // a matrix when none is labelled), times its camera calibration and the analog balance,
    // adapted to D65. A ForwardMatrix is not part of it. Absent for a file that is not a DNG or
    // has no usable colour matrix.
    std::optional<std::array<double, 9>>
    dngD65Matrix(rawspeed::RawDecoder& decoder)
    {
        try {
            const rawspeed::TiffIFD* root = decoder.getRootIFD();
            if (!root || !root->hasEntryRecursive(rawspeed::TiffTag::DNGVERSION)) {
                return std::nullopt;
            }

            const std::array<rawspeed::TiffTag, 3> illuminantTags = { rawspeed::TiffTag::CALIBRATIONILLUMINANT1, rawspeed::TiffTag::CALIBRATIONILLUMINANT2, kCalibrationIlluminant3 };
            const std::array<rawspeed::TiffTag, 3> colorTags = { rawspeed::TiffTag::COLORMATRIX1, rawspeed::TiffTag::COLORMATRIX2, kColorMatrix3 };
            const std::array<rawspeed::TiffTag, 3> calibrationTags = { rawspeed::TiffTag::CAMERACALIBRATION1, rawspeed::TiffTag::CAMERACALIBRATION2, kCameraCalibration3 };

            Mat9f analogBalance = identity9();
            if (const rawspeed::TiffEntry* entry = root->getEntryRecursive(rawspeed::TiffTag::ANALOGBALANCE)) {
                if (entry->count == 3) {
                    analogBalance[0] = entry->getFloat(0);
                    analogBalance[4] = entry->getFloat(1);
                    analogBalance[8] = entry->getFloat(2);
                }
            }

            std::array<int, 3> illuminant = { kIlluminantUnknown, kIlluminantUnknown, kIlluminantUnknown };
            std::array<std::optional<Mat9f>, 3> color;
            std::array<Mat9f, 3> calibration = { identity9(), identity9(), identity9() };
            for (std::size_t k = 0; k < 3; ++k) {
                if (const rawspeed::TiffEntry* entry = root->getEntryRecursive(illuminantTags[k])) {
                    illuminant[k] = (int)std::min<uint32_t>((uint32_t)kIlluminantOther, entry->getU32());
                }
                color[k] = readMatrix(root, colorTags[k]);
                if (color[k]) {
                    if (const std::optional<Mat9f> cc = readMatrix(root, calibrationTags[k])) {
                        calibration[k] = *cc;
                    }
                }
                if (illuminant[k] == kIlluminantOther) {
                    illuminant[k] = kIlluminantD65;
                }
            }

            const int d65Temperature = illuminants()[(std::size_t)kIlluminantD65].temperature;
            int selected = -1;
            int selectedTemperature = 0;
            int deltaMin = d65Temperature;
            for (std::size_t k = 0; k < 3; ++k) {
                if (illuminant[k] == kIlluminantUnknown) {
                    continue;
                }
                const int temperature = illuminants()[(std::size_t)illuminant[k]].temperature;
                const int delta = std::abs(temperature - d65Temperature);
                if (temperature > selectedTemperature && delta <= deltaMin) {
                    selected = (int)k;
                    selectedTemperature = temperature;
                    deltaMin = delta;
                }
            }

            if (selected == -1) {
                for (std::size_t k = 0; k < 3; ++k) {
                    if (illuminant[k] == kIlluminantUnknown && color[k]) {
                        selected = (int)k;
                        illuminant[k] = kIlluminantD65;
                        break;
                    }
                }
            }
            if (selected < 0) {
                return std::nullopt;
            }

            // A labelled illuminant without a ColorMatrix has no matrix to adapt.
            const std::size_t s = (std::size_t)selected;
            if (!color[s]) {
                return std::nullopt;
            }
            const Mat9f cameraToXyz = multiply9(multiply9(analogBalance, calibration[s]), *color[s]);
            const Mat9f d65 = multiply9(illuminants()[(std::size_t)illuminant[s]].adaptation, cameraToXyz);
            std::array<double, 9> out;
            for (std::size_t i = 0; i < 9; ++i) {
                if (!std::isfinite(d65[i])) {
                    return std::nullopt;
                }
                out[i] = (double)d65[i];
            }

            return out;
        } catch (const std::exception&) {
        }

        return std::nullopt;
    }

    // Fills decoded->header from the decoded image, or returns false with the reason.
    bool
    describe(const std::string& path,
             rawspeed::RawDecoder& decoder,
             Decoded* decoded,
             std::string* error)
    {
        rawspeed::RawImageData& raw = *decoded->image;
        std::shared_ptr<Header> header = std::make_shared<Header>();

        identify(decoder, &header->make, &header->model);

        const rawspeed::iPoint2D uncropped = raw.getUncroppedDim();
        const rawspeed::iPoint2D crop = raw.getCropOffset();
        const int width = raw.dim.x;
        const int height = raw.dim.y;
        if (width <= 0 || height <= 0 || uncropped.x <= 0 || uncropped.y <= 0) {
            return fail(error, "rawspeed decoded no pixels from " + path);
        }
        decoded->cropX = crop.x;
        decoded->cropY = crop.y;
        decoded->uncroppedHeight = uncropped.y;
        decoded->cpp = (int)raw.getCpp();
        decoded->isFloat = raw.getDataType() == rawspeed::RawImageType::F32;
        decoded->bytes = (std::size_t)raw.pitch * (std::size_t)uncropped.y + raw.mBadPixelMap.size();

        header->format = RectI(0, 0, uncropped.x, uncropped.y);
        header->window = RectI(crop.x, uncropped.y - crop.y - height, crop.x + width, uncropped.y - crop.y);
        header->pixelAspect = (raw.metadata.pixelAspectRatio > 0.) ? raw.metadata.pixelAspectRatio : 1.;

        ImageMetadata& metadata = header->metadata;
        const RectI& window = header->window;

        if (!raw.blackLevelSeparate) {
            raw.calculateBlackAreas();
        }
        std::array<int, 4> separate = { 0, 0, 0, 0 };
        if (raw.blackLevelSeparate) {
            const rawspeed::Array1DRef<int> levels = *raw.blackLevelSeparate->getAsArray1DRef();
            for (int i = 0; i < 4 && i < levels.size(); ++i) {
                separate[(std::size_t)i] = std::max(0, levels(i));
            }
        }

        if (raw.isCFA) {
            if (decoded->cpp != 1) {
                return fail(error, "Unsupported raw layout in " + path + ": a colour filter array with " + std::to_string(decoded->cpp) + " samples per pixel");
            }
            header->channels = 1;
            const rawspeed::iPoint2D size = raw.cfa.getSize();
            const int period = size.x;
            if (size.x != size.y || (period != 2 && period != 6)) {
                return fail(error, "Unsupported colour filter array in " + path + ": " + std::to_string(size.x) + "x" + std::to_string(size.y));
            }
            // rawspeed keeps a Bayer pattern relative to the active area, but an X-Trans one
            // relative to the uncropped sensor (darktable's loader relies on the same).
            const bool xtrans = raw.cfa.getDcrawFilter() == 9;
            const int colOffset = xtrans ? crop.x : 0;
            const int rowOffset = xtrans ? crop.y : 0;
            std::string pattern;
            for (int r = 0; r < period; ++r) {
                for (int c = 0; c < period; ++c) {
                    const char letter = cfaLetter(raw.cfa.getColorAt(c + colOffset, height - 1 - r + rowOffset));
                    if (!letter) {
                        return fail(error, "Unsupported colour filter array in " + path + ": " + raw.cfa.asString());
                    }
                    pattern += letter;
                }
            }
            const std::optional<RawLayout> layout = RawLayout::fromPattern(pattern, window.x1, window.y1);
            if (!layout) {
                return fail(error, "Unsupported colour filter array in " + path + ": " + pattern);
            }
            layout->writeTo(&metadata);

            // rawspeed's per-photosite black levels are indexed by the parity of the uncropped,
            // top-down position; the keys index the 2x2 block at the CFA origin, going up.
            std::vector<double> black(4, 0.);
            for (int r = 0; r < 2; ++r) {
                for (int c = 0; c < 2; ++c) {
                    const int fileRow = uncropped.y - 1 - (window.y1 + r);
                    const int fileCol = window.x1 + c;
                    black[(std::size_t)(r * 2 + c)] = separate[(std::size_t)(2 * (fileRow & 1) + (fileCol & 1))];
                }
            }
            RawMetadata::setBlackLevels(&metadata, black);
        } else {
            if (decoded->cpp != 1 && decoded->cpp != 3) {
                return fail(error, "Unsupported raw layout in " + path + ": " + std::to_string(decoded->cpp) + " samples per pixel without a colour filter array");
            }
            header->channels = decoded->cpp;
            metadata.setString(kRawMetadataCfaPattern, kRawCfaPatternNone);
            // dt_imageio_open_rawspeed_sraw(): one sample per pixel without a CFA is a monochrome
            // sensor.
            if (decoded->cpp == 1) {
                RawMetadata::setMonochrome(&metadata);
            }
            std::vector<double> black;
            for (int c = 0; c < header->channels; ++c) {
                black.push_back(separate[(std::size_t)std::min(c, 3)]);
            }
            RawMetadata::setBlackLevels(&metadata, black);
        }

        double white = decoded->isFloat ? 1. : 65535.;
        if (raw.whitePoint) {
            // darktable's marker for float data that is already normalised: 1.0f's bit pattern.
            white = (decoded->isFloat && *raw.whitePoint == 0x3F800000) ? 1. : (double)*raw.whitePoint;
        }
        RawMetadata::setWhiteLevel(&metadata, white);

        if (raw.metadata.wbCoeffs) {
            const std::array<float, 4>& wb = *raw.metadata.wbCoeffs;
            if (std::isfinite(wb[0]) && std::isfinite(wb[1]) && std::isfinite(wb[2]) && wb[0] > 0.f && wb[1] > 0.f && wb[2] > 0.f) {
                RawMetadata::setWbCoeffs(&metadata, { (double)wb[0], (double)wb[1], (double)wb[2] });
            }
        }

        if (raw.metadata.colorMatrix.size() >= 9) {
            RawColorMath::Mat3 xyzToCam;
            bool valid = true;
            for (std::size_t i = 0; i < 9; ++i) {
                const rawspeed::NotARational<int>& value = raw.metadata.colorMatrix[i];
                if (value.den == 0) {
                    valid = false;
                    break;
                }
                xyzToCam[i] = (double)value;
            }
            if (valid) {
                if (const std::optional<RawColorMath::Mat3> camToXyz = RawColorMath::invert(xyzToCam)) {
                    RawMetadata::setCamToXyz(&metadata, *camToXyz);
                }
            }
        }

        RawMetadata::setCrop(&metadata, window);
        RawMetadata::setSensor(&metadata, header->format);
        if (raw.metadata.isoSpeed > 0) {
            RawMetadata::setIso(&metadata, (double)raw.metadata.isoSpeed);
        }
        if (const std::optional<double> seconds = exposureTime(decoder)) {
            RawMetadata::setExposure(&metadata, *seconds);
        }
        RawMetadata::setMake(&metadata, header->make);
        RawMetadata::setModel(&metadata, header->model);
        if (const std::optional<double> limit = linearResponseLimit(decoder)) {
            RawMetadata::setLinearResponseLimit(&metadata, *limit);
        }
        if (const std::optional<double> ev = highlightPreservation(decoder, header->make)) {
            RawMetadata::setHighlightPreservation(&metadata, *ev);
        }
        if (const std::optional<std::array<double, 9>> d65 = dngD65Matrix(decoder)) {
            RawMetadata::setDngD65Matrix(&metadata, *d65);
        }
        if (const std::vector<DngGainMap> maps = gainMaps(decoder); !maps.empty()) {
            RawMetadata::setGainMaps(&metadata, maps);
        }
        RawMetadata::setStage(&metadata, eRawStageRaw);

        decoded->header = header;

        return true;
    } // describe

    Outcome
    decodeFile(const FileKey& key)
    {
        Outcome outcome;
        std::string databaseError;
        const rawspeed::CameraMetaData* cameras = RawspeedRuntime::cameraMetaData(&databaseError);

        if (!cameras) {
            outcome.error = databaseError;

            return outcome;
        }

        std::string make;
        std::string model;
        // Declared before the decoder so that it outlives it: the decoder's TIFF entries, which
        // identify() reads on failure, point into the file's bytes.
        decltype(rawspeed::FileReader(nullptr).readFile()) file;
        std::unique_ptr<rawspeed::RawDecoder> decoder;
        try {
            file = rawspeed::FileReader(key.path.c_str()).readFile();
            rawspeed::RawParser parser(file.second);
            decoder = parser.getDecoder(cameras);
            if (!decoder) {
                outcome.error = "rawspeed has no decoder for " + key.path;

                return outcome;
            }
            decoder->failOnUnknown = true;
            try {
                decoder->checkSupport(cameras);
            } catch (const std::exception& e) {
                identify(*decoder, &make, &model);
                outcome.error = isUnsupportedCamera(e.what()) ? unsupportedCameraMessage(make, model, e.what()) : ("Could not decode " + key.path + ": " + e.what());

                return outcome;
            }
            decoder->decodeRaw();
            decoder->decodeMetaData(cameras);

            std::shared_ptr<Decoded> decoded = std::make_shared<Decoded>(decoder->mRaw);
            decoded->key = key;
            if (!describe(key.path, *decoder, decoded.get(), &outcome.error)) {
                return outcome;
            }
            outcome.decoded = decoded;
        } catch (const std::exception& e) {
            const std::string what = e.what();
            if (decoder && isUnsupportedCamera(what)) {
                identify(*decoder, &make, &model);
                outcome.error = unsupportedCameraMessage(make, model, what);
            } else {
                outcome.error = "Could not decode " + key.path + ": " + what;
            }
        }

        return outcome;
    } // decodeFile

    std::size_t
    capacityLocked()
    {
        if (g_capacityOverride) {
            return *g_capacityOverride;
        }
        double ramShare = kFallbackRamShare;
        if (appPTR) {
            if (const SettingsPtr settings = appPTR->getCurrentSettings()) {
                ramShare = settings->getRamMaximumPercent();
            }
        }

        return (std::size_t)(kDecodeCacheShare * ramShare * (double)getSystemTotalRAM_conditionnally());
    }

    void
    trimLocked()
    {
        const std::size_t capacity = capacityLocked();

        while (g_decodedBytes > capacity && g_decoded.size() > 1) {
            g_decodedBytes -= g_decoded.back()->bytes;
            g_decoded.pop_back();
        }
    }

    void
    cacheHeaderLocked(const FileKey& key,
                      const std::shared_ptr<const Header>& header)
    {
        if (g_headers.size() >= kMaxCachedHeaders && g_headers.find(key.path) == g_headers.end()) {
            g_headers.clear();
        }
        HeaderEntry& entry = g_headers[key.path];
        entry.mtime = key.mtime;
        entry.size = key.size;
        entry.header = header;
    }

    std::shared_ptr<const Decoded>
    acquire(const FileKey& key,
            std::string* error)
    {
        std::shared_future<Outcome> pending;
        std::promise<Outcome> promise;
        std::uint64_t evictions = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (std::list<std::shared_ptr<const Decoded>>::iterator it = g_decoded.begin(); it != g_decoded.end(); ++it) {
                if ((*it)->key == key) {
                    g_decoded.splice(g_decoded.begin(), g_decoded, it);

                    return g_decoded.front();
                }
            }
            for (const std::pair<FileKey, std::shared_future<Outcome>>& flight : g_inFlight) {
                if (flight.first == key) {
                    pending = flight.second;
                    break;
                }
            }
            if (!pending.valid()) {
                g_inFlight.emplace_back(key, promise.get_future().share());
                evictions = g_evictions;
            }
        }
        if (pending.valid()) {
            const Outcome& outcome = pending.get();
            if (!outcome.decoded) {
                fail(error, outcome.error);
            }

            return outcome.decoded;
        }

        const Outcome outcome = decodeFile(key);

        std::function<void(const std::string&)> hook;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            hook = g_decodedHook;
            for (std::list<std::pair<FileKey, std::shared_future<Outcome>>>::iterator it = g_inFlight.begin(); it != g_inFlight.end(); ++it) {
                if (it->first == key) {
                    g_inFlight.erase(it);
                    break;
                }
            }
            if (outcome.decoded && evictions == g_evictions) {
                g_decoded.push_front(outcome.decoded);
                g_decodedBytes += outcome.decoded->bytes;
                cacheHeaderLocked(key, outcome.decoded->header);
                trimLocked();
            }
        }
        promise.set_value(outcome);
        if (hook) {
            hook(key.path);
        }
        if (!outcome.decoded) {
            fail(error, outcome.error);
        }

        return outcome.decoded;
    } // acquire

    template <typename T>
    void
    copyRows(rawspeed::Array2DRef<T> data,
             const Decoded& decoded,
             const RectI& window,
             float* dst,
             std::size_t rowStride)
    {
        const std::size_t cpp = (std::size_t)decoded.cpp;
        const std::size_t rowValues = (std::size_t)window.width() * cpp;

        for (int y = window.y1; y < window.y2; ++y) {
            const int fileRow = decoded.uncroppedHeight - 1 - y;
            const T* src = &data(fileRow, 0) + (std::size_t)window.x1 * cpp;
            float* out = dst + (std::size_t)(y - window.y1) * rowStride;
            for (std::size_t i = 0; i < rowValues; ++i) {
                out[i] = (float)src[i];
            }
        }
    }
} // anonymous namespace

std::shared_ptr<const Header>
readHeader(const std::string& path,
           std::string* error)
{
    FileKey key;

    if (!statFile(path, &key, error)) {
        return std::shared_ptr<const Header>();
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const std::map<std::string, HeaderEntry>::const_iterator it = g_headers.find(path);
        if (it != g_headers.end() && it->second.mtime == key.mtime && it->second.size == key.size) {
            return it->second.header;
        }
    }
    const std::shared_ptr<const Decoded> decoded = acquire(key, error);

    return decoded ? decoded->header : std::shared_ptr<const Header>();
}

bool
decode(const std::string& path,
       const RectI& window,
       float* dst,
       std::size_t rowStride,
       std::string* error)
{
    FileKey key;

    if (!statFile(path, &key, error)) {
        return false;
    }
    const std::shared_ptr<const Decoded> decoded = acquire(key, error);
    if (!decoded) {
        return false;
    }
    const Header& header = *decoded->header;
    if (window.x2 < window.x1 || window.y2 < window.y1 || window.x1 < header.window.x1 || window.x2 > header.window.x2 || window.y1 < header.window.y1 || window.y2 > header.window.y2) {
        return fail(error, "Requested window is outside the active area of " + path);
    }
    if (window.isNull()) {
        return true;
    }
    if (!dst) {
        return fail(error, "No destination buffer");
    }
    if (rowStride < (std::size_t)window.width() * (std::size_t)decoded->cpp) {
        return fail(error, "Row stride is smaller than a window row");
    }
    rawspeed::RawImageData& raw = *decoded->image;
    if (decoded->isFloat) {
        copyRows(raw.getF32DataAsUncroppedArray2DRef(), *decoded, window, dst, rowStride);
    } else {
        copyRows(raw.getU16DataAsUncroppedArray2DRef(), *decoded, window, dst, rowStride);
    }

    return true;
}

std::string
unsupportedCameraMessage(const std::string& make,
                         const std::string& model,
                         const std::string& detail)
{
    std::string name;
    if (model.compare(0, make.size(), make) == 0) {
        name = model;
    } else {
        name = make.empty() ? model : (make + " " + model);
    }
    const std::string camera = name.empty() ? std::string("An unidentified camera") : ("The camera " + name);
    std::string message = camera + " is not supported by rawspeed " + RawspeedRuntime::version() + ", the camera raw decoder of this build.";
    if (!detail.empty()) {
        message += " (" + detail + ")";
    }

    return message;
}

void
clearCaches()
{
    std::lock_guard<std::mutex> lock(g_mutex);

    g_headers.clear();
    g_decoded.clear();
    g_decodedBytes = 0;
    ++g_evictions;
}

void
evict(const std::vector<std::string>& paths)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    for (const std::string& path : paths) {
        g_headers.erase(path);
        for (std::list<std::shared_ptr<const Decoded>>::iterator it = g_decoded.begin(); it != g_decoded.end();) {
            if ((*it)->key.path == path) {
                g_decodedBytes -= (*it)->bytes;
                it = g_decoded.erase(it);
            } else {
                ++it;
            }
        }
    }
    ++g_evictions;
}

std::size_t
decodeCacheCapacity()
{
    std::lock_guard<std::mutex> lock(g_mutex);

    return capacityLocked();
}

void
setDecodeCacheCapacityForTests(std::optional<std::size_t> bytes)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    g_capacityOverride = bytes;
    trimLocked();
}

std::size_t
decodeCacheBytes()
{
    std::lock_guard<std::mutex> lock(g_mutex);

    return g_decodedBytes;
}

void
setDecodedHookForTests(const std::function<void(const std::string&)>& hook)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    g_decodedHook = hook;
}
} // namespace RawDecodeSupport

NATRON_NAMESPACE_EXIT
