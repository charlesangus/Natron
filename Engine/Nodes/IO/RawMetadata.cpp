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

#include "RawMetadata.h"

#include <QCoreApplication>
#include <QString>

NATRON_NAMESPACE_ENTER

namespace {

int
letterColour(char c)
{
    switch (c) {
    case 'R':

        return eRawCfaColorRed;
    case 'G':

        return eRawCfaColorGreen;
    case 'B':

        return eRawCfaColorBlue;
    default:

        return -1;
    }
}

char
colourLetter(unsigned char colour)
{
    switch (colour) {
    case eRawCfaColorRed:

        return 'R';
    case eRawCfaColorGreen:

        return 'G';
    default:

        return 'B';
    }
}

template <std::size_t N>
std::optional<std::array<double, N>>
getFixedVector(const ImageMetadata& metadata,
               const char* key)
{
    const std::optional<std::vector<double>> values = metadata.getDoubleVector(key);

    if (!values || (values->size() != N)) {
        return std::nullopt;
    }
    std::array<double, N> out;
    for (std::size_t i = 0; i < N; ++i) {
        out[i] = (*values)[i];
    }

    return out;
}

template <std::size_t N>
void
setFixedVector(ImageMetadata* metadata,
               const char* key,
               const std::array<double, N>& values)
{
    metadata->setDoubleVector(key, std::vector<double>(values.begin(), values.end()));
}

} // namespace

std::optional<RawLayout>
RawLayout::fromPattern(std::string_view pattern,
                       int originX,
                       int originY)
{
    RawLayout layout;

    layout._originX = originX;
    layout._originY = originY;
    if (pattern == kRawCfaPatternNone) {
        return layout;
    }
    int period = 0;
    if (pattern.size() == 4) {
        period = 2;
    } else if (pattern.size() == static_cast<std::size_t>(kMaxPeriod * kMaxPeriod)) {
        period = kMaxPeriod;
    } else {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const int colour = letterColour(pattern[i]);
        if (colour < 0) {
            return std::nullopt;
        }
        layout._cells[i] = static_cast<unsigned char>(colour);
    }
    layout._period = period;

    return layout;
}

std::optional<RawLayout>
RawLayout::fromMetadata(const ImageMetadata& metadata)
{
    const std::optional<std::string> pattern = metadata.getString(kRawMetadataCfaPattern);

    if (!pattern) {
        return std::nullopt;
    }
    int originX = 0;
    int originY = 0;
    if (metadata.contains(kRawMetadataCfaOrigin)) {
        const std::optional<std::vector<int>> origin = metadata.getIntVector(kRawMetadataCfaOrigin);
        if (!origin || (origin->size() != 2)) {
            return std::nullopt;
        }
        originX = (*origin)[0];
        originY = (*origin)[1];
    }

    return fromPattern(*pattern, originX, originY);
}

void
RawLayout::writeTo(ImageMetadata* metadata) const
{
    metadata->setString(kRawMetadataCfaPattern, pattern());
    metadata->setIntVector(kRawMetadataCfaOrigin, std::vector<int> { _originX, _originY });
}

std::string
RawLayout::pattern() const
{
    if (_period == 0) {
        return kRawCfaPatternNone;
    }
    std::string out(static_cast<std::size_t>(_period * _period), 'G');
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = colourLetter(_cells[i]);
    }

    return out;
}

RawLayout
RawLayout::shifted(int dx,
                   int dy) const
{
    RawLayout out = *this;

    // The origin is reduced modulo the period so that repeated shifts never overflow it.
    if (_period > 0) {
        out._originX = wrap(_originX + dx);
        out._originY = wrap(_originY + dy);
    } else {
        out._originX = _originX + dx;
        out._originY = _originY + dy;
    }

    return out;
}

bool
RawLayout::operator==(const RawLayout& other) const
{
    if (_period != other._period) {
        return false;
    }
    if (_period == 0) {
        return true;
    }
    // Two layouts are the same when they give every pixel the same colour, whatever origin
    // representative each one holds.
    for (int y = 0; y < _period; ++y) {
        for (int x = 0; x < _period; ++x) {
            if (colourAt(x, y) != other.colourAt(x, y)) {
                return false;
            }
        }
    }

    return true;
}

namespace RawMetadata {
std::optional<std::vector<double>>
getBlackLevels(const ImageMetadata& metadata)
{
    std::optional<std::vector<double>> levels = metadata.getDoubleVector(kRawMetadataBlackLevels);

    if (!levels || levels->empty()) {
        return std::nullopt;
    }

    return levels;
}

void
setBlackLevels(ImageMetadata* metadata,
               const std::vector<double>& levels)
{
    metadata->setDoubleVector(kRawMetadataBlackLevels, levels);
}

std::optional<double>
getWhiteLevel(const ImageMetadata& metadata)
{
    return metadata.getDouble(kRawMetadataWhiteLevel);
}

void
setWhiteLevel(ImageMetadata* metadata,
              double level)
{
    metadata->setDouble(kRawMetadataWhiteLevel, level);
}

std::optional<std::array<double, 3>>
getWbCoeffs(const ImageMetadata& metadata)
{
    return getFixedVector<3>(metadata, kRawMetadataWbCoeffs);
}

void
setWbCoeffs(ImageMetadata* metadata,
            const std::array<double, 3>& coeffs)
{
    setFixedVector(metadata, kRawMetadataWbCoeffs, coeffs);
}

std::optional<std::array<double, 9>>
getCamToXyz(const ImageMetadata& metadata)
{
    return getFixedVector<9>(metadata, kRawMetadataCamToXyz);
}

void
setCamToXyz(ImageMetadata* metadata,
            const std::array<double, 9>& matrix)
{
    setFixedVector(metadata, kRawMetadataCamToXyz, matrix);
}

std::optional<RectI>
getCrop(const ImageMetadata& metadata)
{
    const std::optional<std::vector<int>> crop = metadata.getIntVector(kRawMetadataCrop);

    if (!crop || (crop->size() != 4)) {
        return std::nullopt;
    }

    return RectI((*crop)[0], (*crop)[1], (*crop)[2], (*crop)[3]);
}

void
setCrop(ImageMetadata* metadata,
        const RectI& crop)
{
    metadata->setIntVector(kRawMetadataCrop, std::vector<int> { crop.x1, crop.y1, crop.x2, crop.y2 });
}

std::optional<double>
getIso(const ImageMetadata& metadata)
{
    return metadata.getDouble(kRawMetadataIso);
}

void
setIso(ImageMetadata* metadata,
       double iso)
{
    metadata->setDouble(kRawMetadataIso, iso);
}

std::optional<double>
getExposure(const ImageMetadata& metadata)
{
    return metadata.getDouble(kRawMetadataExposure);
}

void
setExposure(ImageMetadata* metadata,
            double seconds)
{
    metadata->setDouble(kRawMetadataExposure, seconds);
}

std::optional<std::array<double, 3>>
getWbApplied(const ImageMetadata& metadata)
{
    return getFixedVector<3>(metadata, kRawMetadataWbApplied);
}

void
setWbApplied(ImageMetadata* metadata,
             const std::array<double, 3>& coeffs)
{
    setFixedVector(metadata, kRawMetadataWbApplied, coeffs);
}

std::optional<std::array<double, 3>>
getProcessedMaximum(const ImageMetadata& metadata)
{
    return getFixedVector<3>(metadata, kRawMetadataProcessedMaximum);
}

void
setProcessedMaximum(ImageMetadata* metadata,
                    const std::array<double, 3>& maximum)
{
    setFixedVector(metadata, kRawMetadataProcessedMaximum, maximum);
}

std::optional<RawStageEnum>
getStage(const ImageMetadata& metadata)
{
    const std::optional<std::string> stage = metadata.getString(kRawMetadataStage);

    if (!stage) {
        return std::nullopt;
    }
    if (*stage == kRawStageRaw) {
        return eRawStageRaw;
    }
    if (*stage == kRawStagePrepared) {
        return eRawStagePrepared;
    }

    return std::nullopt;
}

void
setStage(ImageMetadata* metadata,
         RawStageEnum stage)
{
    metadata->setString(kRawMetadataStage, stageName(stage));
}

const char*
stageName(RawStageEnum stage)
{
    return (stage == eRawStagePrepared) ? kRawStagePrepared : kRawStageRaw;
}

std::optional<std::string>
requireStage(const ImageMetadata& metadata,
             RawStageEnum expected,
             const std::string& nodeLabel)
{
    const std::optional<RawStageEnum> stage = getStage(metadata);
    const QString label = QString::fromUtf8(nodeLabel.c_str());

    if (stage && (*stage == expected)) {
        return std::nullopt;
    }
    if (!stage) {
        return QCoreApplication::translate("RawMetadata", "%1 needs raw camera data, but its input does not say which raw stage it is at: connect it below a Read of a raw file.").arg(label).toStdString();
    }
    if (expected == eRawStagePrepared) {
        return QCoreApplication::translate("RawMetadata", "%1 needs prepared raw data: add a RawPrepare node upstream of it.").arg(label).toStdString();
    }

    return QCoreApplication::translate("RawMetadata", "%1 needs raw data that has not been prepared yet: move it above the RawPrepare node upstream of it.").arg(label).toStdString();
}
} // namespace RawMetadata

NATRON_NAMESPACE_EXIT
