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

#ifndef Engine_Nodes_IO_RawMetadata_h
#define Engine_Nodes_IO_RawMetadata_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Engine/Nodes/IO/RawDngOpcodes.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/RectI.h"

/**
 * The image metadata keys describing an eDataKindRaw stream. The Read sets them, every raw node
 * reads them from its source, and a node that changes what they describe rewrites them in its
 * deriveOutputMetadata().
 *
 * Every position is in Natron pixel coordinates at render scale 1, y up, the same coordinates the
 * raw image itself is in. The Read turns the file's top-down sensor layout into these.
 **/
#define kRawMetadataCfaPattern "raw/cfa_pattern"
#define kRawMetadataCfaOrigin "raw/cfa_origin"
#define kRawMetadataBlackLevels "raw/black_levels"
#define kRawMetadataWhiteLevel "raw/white_level"
#define kRawMetadataWbCoeffs "raw/wb_coeffs"
#define kRawMetadataCamToXyz "raw/cam_to_xyz"
#define kRawMetadataCrop "raw/crop"
#define kRawMetadataIso "raw/iso"
#define kRawMetadataExposure "raw/exposure"
#define kRawMetadataStage "raw/stage"
#define kRawMetadataWbApplied "raw/wb_applied"
#define kRawMetadataProcessedMaximum "raw/processed_maximum"
#define kRawMetadataMake "raw/make"
#define kRawMetadataModel "raw/model"
#define kRawMetadataLinearResponseLimit "raw/linear_response_limit"
#define kRawMetadataDngD65Matrix "raw/dng_d65_matrix"
#define kRawMetadataGainMaps "raw/gain_maps"
#define kRawMetadataWbLateCorrection "raw/wb_late_correction"
#define kRawMetadataWbD65Coeffs "raw/wb_d65_coeffs"
#define kRawMetadataSensorWhiteLevel "raw/sensor_white_level"
#define kRawMetadataCatApplied "raw/cat_applied"

#define kRawCfaPatternNone "none"
#define kRawStageRaw "raw"
#define kRawStagePrepared "prepared"

NATRON_NAMESPACE_ENTER

/**
 * @brief The colour filter over one photosite.
 **/
enum RawCfaColorEnum {
    eRawCfaColorRed = 0,
    eRawCfaColorGreen = 1,
    eRawCfaColorBlue = 2,
    eRawCfaColorNone = 3
};

/**
 * @brief Where a raw stream is in the chain: straight off the sensor, or prepared (black
 * subtracted and scaled so the white level is 1).
 **/
enum RawStageEnum {
    eRawStageRaw,
    eRawStagePrepared
};

/**
 * @brief The colour filter array of a raw image: a period x period pattern of colours, and the
 * pixel its first cell sits on.
 *
 * raw/cfa_pattern holds the pattern as one letter per cell, R, G or B, row by row starting at
 * the origin's row and going up in y: "RGGB", "BGGR", "GRBG" or "GBRG" for a 2x2 Bayer, 36
 * letters for a 6x6 X-Trans, or "none" for data without a CFA (a linear DNG, a monochrome
 * sensor). raw/cfa_origin is the pixel (x, y) cell 0 sits on, (0, 0) when absent.
 *
 * A node that moves pixels in whole steps -- a crop that changes the window origin, a shift, an
 * odd offset -- must write the moved origin back with shifted() and writeTo(), or every node
 * after it reads the wrong colour for each photosite. A node that resamples a mosaic is not
 * meaningful and must not exist.
 **/
class RawLayout {
public:
    static constexpr int kMaxPeriod = 6;

    RawLayout() = default;

    /**
     * @brief A layout from a pattern string, or nullopt if pattern is neither "none" nor a square
     * pattern of R, G and B letters with a period of 2 or 6.
     **/
    static std::optional<RawLayout> fromPattern(std::string_view pattern,
                                                int originX = 0,
                                                int originY = 0);

    /**
     * @brief The layout raw/cfa_pattern and raw/cfa_origin describe, or nullopt when the pattern
     * is absent or malformed.
     **/
    static std::optional<RawLayout> fromMetadata(const ImageMetadata& metadata);

    void writeTo(ImageMetadata* metadata) const;

    bool hasCfa() const
    {
        return _period > 0;
    }

    /**
     * @brief 2 for Bayer, 6 for X-Trans, 0 without a CFA.
     **/
    int period() const
    {
        return _period;
    }

    int originX() const
    {
        return _originX;
    }

    int originY() const
    {
        return _originY;
    }

    std::string pattern() const;

    /**
     * @brief The colour of the photosite at pixel (x, y); eRawCfaColorNone without a CFA.
     **/
    RawCfaColorEnum colourAt(int x,
                             int y) const
    {
        if (_period == 0) {
            return eRawCfaColorNone;
        }
        const int col = wrap(x - _originX);
        const int row = wrap(y - _originY);

        return static_cast<RawCfaColorEnum>(_cells[row * _period + col]);
    }

    /**
     * @brief The same pattern after the image content moves by (dx, dy) pixels: what a crop
     * whose new window starts dx pixels further left (a negative dx) needs to record.
     **/
    RawLayout shifted(int dx,
                      int dy) const;

    /**
     * @brief The layout of the region starting at pixel (x1, y1) once it becomes the window at
     * the origin: shifted(-x1, -y1).
     **/
    RawLayout croppedTo(int x1,
                        int y1) const
    {
        return shifted(-x1, -y1);
    }

    bool operator==(const RawLayout& other) const;

private:
    int wrap(int v) const
    {
        const int m = v % _period;

        return (m < 0) ? (m + _period) : m;
    }

    int _period = 0;
    int _originX = 0;
    int _originY = 0;
    std::array<unsigned char, kMaxPeriod * kMaxPeriod> _cells = {};
};

/**
 * @brief Typed access to the raw/ keys. A getter returns nullopt when the key is absent or holds
 * a value of the wrong type or size.
 **/
namespace RawMetadata {
/**
 * @brief raw/black_levels: one value per cell of the 2x2 block at the CFA origin, row by row,
 * as rawspeed reports them per photosite position (a Bayer pattern has one colour per cell, its
 * two greens separately), or one value per channel for data without a CFA. Any non-empty length
 * is kept as stored.
 **/
std::optional<std::vector<double>> getBlackLevels(const ImageMetadata& metadata);
void setBlackLevels(ImageMetadata* metadata, const std::vector<double>& levels);

std::optional<double> getWhiteLevel(const ImageMetadata& metadata);
void setWhiteLevel(ImageMetadata* metadata, double level);

/**
 * @brief raw/sensor_white_level: the white level the raw file gives, which RawPrepare keeps when
 * it normalises raw/white_level to 1 (darktable's raw_white_point, by which RawDemosaic's
 * capture sharpening tells a sensor of more than 12 bits).
 **/
std::optional<double> getSensorWhiteLevel(const ImageMetadata& metadata);
void setSensorWhiteLevel(ImageMetadata* metadata, double level);

/**
 * @brief raw/wb_coeffs: the as-shot red, green and blue multipliers.
 **/
std::optional<std::array<double, 3>> getWbCoeffs(const ImageMetadata& metadata);
void setWbCoeffs(ImageMetadata* metadata, const std::array<double, 3>& coeffs);

/**
 * @brief raw/cam_to_xyz: camera RGB to XYZ under D65, row-major 3x3.
 **/
std::optional<std::array<double, 9>> getCamToXyz(const ImageMetadata& metadata);
void setCamToXyz(ImageMetadata* metadata, const std::array<double, 9>& matrix);

/**
 * @brief raw/crop: the sensor's active area, in pixels.
 **/
std::optional<RectI> getCrop(const ImageMetadata& metadata);
void setCrop(ImageMetadata* metadata, const RectI& crop);

std::optional<double> getIso(const ImageMetadata& metadata);
void setIso(ImageMetadata* metadata, double iso);

/**
 * @brief raw/exposure: the exposure time, in seconds.
 **/
std::optional<double> getExposure(const ImageMetadata& metadata);
void setExposure(ImageMetadata* metadata, double seconds);

/**
 * @brief raw/wb_applied: the red, green and blue multipliers a white balance node has already
 * applied to the data, absent until one has.
 **/
std::optional<std::array<double, 3>> getWbApplied(const ImageMetadata& metadata);
void setWbApplied(ImageMetadata* metadata, const std::array<double, 3>& coeffs);

/**
 * @brief raw/processed_maximum: per red, green and blue, the value a photosite at the white level
 * has now, which the develop nodes after the white balance scale their thresholds by (darktable's
 * processed_maximum). Absent means 1 for each.
 **/
std::optional<std::array<double, 3>> getProcessedMaximum(const ImageMetadata& metadata);
void setProcessedMaximum(ImageMetadata* metadata, const std::array<double, 3>& maximum);

/**
 * @brief raw/wb_d65_coeffs: the red, green and blue multipliers that make D65 white neutral for
 * the camera (darktable's chroma D65coeffs), which RawTemperature publishes in every mode.
 **/
std::optional<std::array<double, 3>> getWbD65Coeffs(const ImageMetadata& metadata);
void setWbD65Coeffs(ImageMetadata* metadata, const std::array<double, 3>& coeffs);

/**
 * @brief raw/wb_late_correction: per red, green and blue, D65coeffs / as_shot, which RawInputColor
 * multiplies camera RGB by before its matrix (darktable's chroma late_correction). Present only
 * after RawTemperature in As Shot to Reference.
 **/
std::optional<std::array<double, 3>> getWbLateCorrection(const ImageMetadata& metadata);
void setWbLateCorrection(ImageMetadata* metadata, const std::array<double, 3>& correction);

/**
 * @brief raw/cat_applied: 1 once a RawColorCalibration has adapted the illuminant to D50
 * (darktable's chroma adaptation, the instance that owns the CAT). Absent until one has.
 **/
bool getCatApplied(const ImageMetadata& metadata);
void setCatApplied(ImageMetadata* metadata);

/**
 * @brief raw/make and raw/model: rawspeed's canonical camera names, the spelling darktable's
 * white balance presets and colour matrix tables match on ("Canon" / "EOS 5D Mark III").
 **/
std::optional<std::string> getMake(const ImageMetadata& metadata);
void setMake(ImageMetadata* metadata, const std::string& make);

std::optional<std::string> getModel(const ImageMetadata& metadata);
void setModel(ImageMetadata* metadata, const std::string& model);

/**
 * @brief raw/linear_response_limit: the DNG LinearResponseLimit, the fraction of the white level
 * up to which the sensor responds linearly. 1 when the key is absent.
 **/
double getLinearResponseLimit(const ImageMetadata& metadata);
void setLinearResponseLimit(ImageMetadata* metadata, double limit);

/**
 * @brief raw/dng_d65_matrix: the matrix a DNG's embedded colour tags give for D65 (darktable's
 * d65_color_matrix), row-major 3x3. Absent when the file is not a DNG or carries no usable
 * colour matrix.
 **/
std::optional<std::array<double, 9>> getDngD65Matrix(const ImageMetadata& metadata);
void setDngD65Matrix(ImageMetadata* metadata, const std::array<double, 9>& matrix);

/**
 * @brief raw/gain_maps: the DNG GainMap opcodes of the file's OpcodeList2, stored by
 * RawDngOpcodes::serializeGainMaps() (map count, then per map 15 geometry values and its gains).
 * Empty when the key is absent or malformed.
 **/
std::vector<DngGainMap> getGainMaps(const ImageMetadata& metadata);
void setGainMaps(ImageMetadata* metadata, const std::vector<DngGainMap>& maps);

std::optional<RawStageEnum> getStage(const ImageMetadata& metadata);
void setStage(ImageMetadata* metadata, RawStageEnum stage);

const char* stageName(RawStageEnum stage);

/**
 * @brief The persistent error for nodeLabel when its source is not at the expected stage, or
 * nullopt when it is. The text names the node that has to go upstream, or says where nodeLabel
 * has to move.
 **/
std::optional<std::string> requireStage(const ImageMetadata& metadata,
                                        RawStageEnum expected,
                                        const std::string& nodeLabel);
} // namespace RawMetadata

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_IO_RawMetadata_h
