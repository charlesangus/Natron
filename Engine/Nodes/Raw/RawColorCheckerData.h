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
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7): src/common/colorchecker.h,
 * the CIE Lab references and layouts of the colour checker charts color calibration
 * (channelmixerrgb) calibrates against. The values were extracted from that header by script and
 * are darktable's to the digit. darktable is distributed under the GNU GPL, version 3 or (at your
 * option) any later version, and so is this port: GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2020-2023 darktable developers.
 *
 *     darktable is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     darktable is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef Engine_Nodes_Raw_RawColorCheckerData_h
#define Engine_Nodes_Raw_RawColorCheckerData_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <cstddef>

NATRON_NAMESPACE_ENTER

namespace RawColorCheckerData {
/**
 * @brief dt_color_checker_targets: the charts, in darktable's order, which is also the order of
 * the node's Chart choice.
 **/
enum class ColorCheckerTarget : int {
    Xrite24_2000 = 0,
    Xrite24_2014 = 1,
    Spyder24 = 2,
    Spyder24V2 = 3,
    Spyder48 = 4,
    Spyder48V2 = 5,
    SpyderPhoto = 6,
    Last
};

/**
 * @brief dt_color_checker_patch: a patch's mnemonic name, its CIE Lab (D50) reference, and its
 * centre relative to the chart's guides (the white dots), as fractions of the grid along x and y,
 * y pointing down.
 **/
struct ColorCheckerPatch {
    const char* name;
    float Lab[3];
    float x;
    float y;
};

/**
 * @brief dt_color_checker_t.
 **/
struct ColorChecker {
    const char* name;
    const char* author;
    const char* date;
    const char* manufacturer;
    ColorCheckerTarget type;
    // Format ratio of the chart, guide to guide.
    float ratio;
    // Radius of a patch, as a fraction of the chart's diagonal.
    float radius;
    std::size_t patches;
    // Dimension along the x and y axes.
    std::size_t size[2];
    // The patch closest to 20 % neutral grey, to pure white and to pure black.
    std::size_t middleGrey;
    std::size_t white;
    std::size_t black;
    const ColorCheckerPatch* values;
};

// xrite_24_2000
inline constexpr ColorCheckerPatch kXrite242000Values[] = {
    { "A1", { 37.986f, 13.555f, 14.059f }, 0.087f, 0.125f },
    { "A2", { 65.711f, 18.13f, 17.81f }, 0.25f, 0.125f },
    { "A3", { 49.927f, -4.88f, -21.905f }, 0.417f, 0.125f },
    { "A4", { 43.139f, -13.095f, 21.905f }, 0.584f, 0.125f },
    { "A5", { 55.112f, 8.844f, -25.399f }, 0.751f, 0.125f },
    { "A6", { 70.719f, -33.397f, -0.199f }, 0.918f, 0.125f },
    { "B1", { 62.661f, 36.067f, 57.096f }, 0.087f, 0.375f },
    { "B2", { 40.02f, 10.41f, -45.964f }, 0.25f, 0.375f },
    { "B3", { 51.124f, 48.239f, 16.248f }, 0.417f, 0.375f },
    { "B4", { 30.325f, 22.976f, -21.587f }, 0.584f, 0.375f },
    { "B5", { 72.532f, -23.709f, 57.255f }, 0.751f, 0.375f },
    { "B6", { 71.941f, 19.363f, 67.857f }, 0.918f, 0.375f },
    { "C1", { 28.778f, 14.179f, -50.297f }, 0.087f, 0.625f },
    { "C2", { 55.261f, -38.342f, 31.37f }, 0.25f, 0.625f },
    { "C3", { 42.101f, 53.378f, 28.19f }, 0.417f, 0.625f },
    { "C4", { 81.733f, 4.039f, 79.819f }, 0.584f, 0.625f },
    { "C5", { 51.935f, 49.986f, -14.574f }, 0.751f, 0.625f },
    { "C6", { 51.038f, -28.631f, -28.638f }, 0.918f, 0.625f },
    { "D1", { 96.539f, -0.425f, 1.186f }, 0.087f, 0.875f },
    { "D2", { 81.257f, -0.638f, -0.335f }, 0.25f, 0.875f },
    { "D3", { 66.766f, -0.734f, -0.504f }, 0.417f, 0.875f },
    { "D4", { 50.867f, -0.153f, -0.27f }, 0.584f, 0.875f },
    { "D5", { 35.656f, -0.421f, -1.231f }, 0.751f, 0.875f },
    { "D6", { 20.461f, -0.079f, -0.973f }, 0.918f, 0.875f },
};

inline constexpr ColorChecker kXrite242000 = {
    "Xrite ColorChecker 24 before 2014",
    "X-Rite",
    "3/27/2000",
    "X-Rite/Gretag Macbeth",
    ColorCheckerTarget::Xrite24_2000,
    2.f / 3.f, // ratio
    0.055f, // radius
    24, // patches
    { 4, 6 }, // size
    21, // middle grey
    18, // white
    23, // black
    kXrite242000Values,
};

// xrite_24_2014
inline constexpr ColorCheckerPatch kXrite242014Values[] = {
    { "A1", { 37.54f, 14.37f, 14.92f }, 0.087f, 0.125f },
    { "A2", { 64.66f, 19.27f, 17.5f }, 0.25f, 0.125f },
    { "A3", { 49.32f, -3.82f, -22.54f }, 0.417f, 0.125f },
    { "A4", { 43.46f, -12.74f, 22.72f }, 0.584f, 0.125f },
    { "A5", { 54.94f, 9.61f, -24.79f }, 0.751f, 0.125f },
    { "A6", { 70.48f, -32.26f, -0.37f }, 0.918f, 0.125f },
    { "B1", { 62.73f, 35.83f, 56.5f }, 0.087f, 0.375f },
    { "B2", { 39.43f, 10.75f, -45.17f }, 0.25f, 0.375f },
    { "B3", { 50.57f, 48.64f, 16.67f }, 0.417f, 0.375f },
    { "B4", { 30.1f, 22.54f, -20.87f }, 0.584f, 0.375f },
    { "B5", { 71.77f, -24.13f, 58.19f }, 0.751f, 0.375f },
    { "B6", { 71.51f, 18.24f, 67.37f }, 0.918f, 0.375f },
    { "C1", { 28.37f, 15.42f, -49.8f }, 0.087f, 0.625f },
    { "C2", { 54.38f, -39.72f, 32.27f }, 0.25f, 0.625f },
    { "C3", { 42.43f, 51.05f, 28.62f }, 0.417f, 0.625f },
    { "C4", { 81.8f, 2.67f, 80.41f }, 0.584f, 0.625f },
    { "C5", { 50.63f, 51.28f, -14.12f }, 0.751f, 0.625f },
    { "C6", { 49.57f, -29.71f, -28.32f }, 0.918f, 0.625f },
    { "D1", { 95.19f, -1.03f, 2.93f }, 0.087f, 0.875f },
    { "D2", { 81.29f, -0.57f, 0.44f }, 0.25f, 0.875f },
    { "D3", { 66.89f, -0.75f, -0.06f }, 0.417f, 0.875f },
    { "D4", { 50.76f, -0.13f, 0.14f }, 0.584f, 0.875f },
    { "D5", { 35.63f, -0.46f, -0.48f }, 0.751f, 0.875f },
    { "D6", { 20.64f, 0.07f, -0.46f }, 0.918f, 0.875f },
};

inline constexpr ColorChecker kXrite242014 = {
    "Xrite ColorChecker 24 after 2014",
    "X-Rite",
    "3/28/2015",
    "X-Rite/Gretag Macbeth",
    ColorCheckerTarget::Xrite24_2014,
    2.f / 3.f, // ratio
    0.055f, // radius
    24, // patches
    { 4, 6 }, // size
    21, // middle grey
    18, // white
    23, // black
    kXrite242014Values,
};

// spyder_24
inline constexpr ColorCheckerPatch kSpyder24Values[] = {
    { "A1", { 96.04f, 2.16f, 2.6f }, 0.107f, 0.844f },
    { "A2", { 80.44f, 1.17f, 2.05f }, 0.264f, 0.844f },
    { "A3", { 65.52f, 0.69f, 1.86f }, 0.421f, 0.844f },
    { "A4", { 49.62f, 0.58f, 1.56f }, 0.579f, 0.844f },
    { "A5", { 33.55f, 0.35f, 1.4f }, 0.736f, 0.844f },
    { "A6", { 16.91f, 1.43f, -0.81f }, 0.893f, 0.844f },
    { "B1", { 47.12f, -32.5f, -28.75f }, 0.107f, 0.615f },
    { "B2", { 50.49f, 53.45f, -13.55f }, 0.264f, 0.615f },
    { "B3", { 83.61f, 3.36f, 87.02f }, 0.421f, 0.615f },
    { "B4", { 41.05f, 60.75f, 31.17f }, 0.579f, 0.615f },
    { "B5", { 54.14f, -40.8f, 34.75f }, 0.736f, 0.615f },
    { "B6", { 24.75f, 13.78f, -49.48f }, 0.893f, 0.615f },
    { "C1", { 60.94f, 38.21f, 61.31f }, 0.107f, 0.385f },
    { "C2", { 37.8f, 7.3f, -43.04f }, 0.264f, 0.385f },
    { "C3", { 49.81f, 48.5f, 15.76f }, 0.421f, 0.385f },
    { "C4", { 28.88f, 19.36f, -24.48f }, 0.579f, 0.385f },
    { "C5", { 72.45f, -23.6f, 60.47f }, 0.736f, 0.385f },
    { "C6", { 71.65f, 23.74f, 72.28f }, 0.893f, 0.385f },
    { "D1", { 70.19f, -31.9f, 1.98f }, 0.107f, 0.155f },
    { "D2", { 54.38f, 8.84f, -25.71f }, 0.264f, 0.155f },
    { "D3", { 42.03f, -15.8f, 22.93f }, 0.421f, 0.155f },
    { "D4", { 48.82f, -5.11f, -23.08f }, 0.579f, 0.155f },
    { "D5", { 65.1f, 18.14f, 18.68f }, 0.736f, 0.155f },
    { "D6", { 36.13f, 14.15f, 15.78f }, 0.893f, 0.155f },
};

inline constexpr ColorChecker kSpyder24 = {
    "Datacolor SpyderCheckr 24 before 2018",
    "Aurélien PIERRE",
    "dec, 9 2016",
    "DataColor",
    ColorCheckerTarget::Spyder24,
    2.f / 3.f, // ratio
    0.035f, // radius
    24, // patches
    { 4, 6 }, // size
    3, // middle grey
    0, // white
    5, // black
    kSpyder24Values,
};

// spyder_24_v2
inline constexpr ColorCheckerPatch kSpyder24V2Values[] = {
    { "A1", { 96.04f, 2.16f, 2.6f }, 0.107f, 0.844f },
    { "A2", { 80.44f, 1.17f, 2.05f }, 0.264f, 0.844f },
    { "A3", { 65.52f, 0.69f, 1.86f }, 0.421f, 0.844f },
    { "A4", { 49.62f, 0.58f, 1.56f }, 0.579f, 0.844f },
    { "A5", { 33.55f, 0.35f, 1.4f }, 0.736f, 0.844f },
    { "A6", { 16.91f, 1.43f, -0.81f }, 0.893f, 0.844f },
    { "B1", { 47.12f, -32.5f, -28.75f }, 0.107f, 0.615f },
    { "B2", { 50.49f, 53.45f, -13.55f }, 0.264f, 0.615f },
    { "B3", { 83.61f, 3.36f, 87.02f }, 0.421f, 0.615f },
    { "B4", { 41.05f, 60.75f, 31.17f }, 0.579f, 0.615f },
    { "B5", { 54.14f, -40.8f, 34.75f }, 0.736f, 0.615f },
    { "B6", { 24.75f, 13.78f, -49.48f }, 0.893f, 0.615f },
    { "C1", { 60.94f, 38.21f, 61.31f }, 0.107f, 0.385f },
    { "C2", { 37.8f, 7.3f, -43.04f }, 0.264f, 0.385f },
    { "C3", { 49.81f, 48.5f, 15.76f }, 0.421f, 0.385f },
    { "C4", { 28.88f, 19.36f, -24.48f }, 0.579f, 0.385f },
    { "C5", { 72.45f, -23.57f, 60.47f }, 0.736f, 0.385f },
    { "C6", { 71.65f, 23.74f, 72.28f }, 0.893f, 0.385f },
    { "D1", { 70.19f, -31.85f, 1.98f }, 0.107f, 0.155f },
    { "D2", { 54.38f, 8.84f, -25.71f }, 0.264f, 0.155f },
    { "D3", { 42.03f, -15.78f, 22.93f }, 0.421f, 0.155f },
    { "D4", { 48.82f, -5.11f, -23.08f }, 0.579f, 0.155f },
    { "D5", { 65.1f, 18.14f, 18.68f }, 0.736f, 0.155f },
    { "D6", { 36.13f, 14.15f, 15.78f }, 0.893f, 0.155f },
};

inline constexpr ColorChecker kSpyder24V2 = {
    "Datacolor SpyderCheckr 24 after 2018",
    "Aurélien PIERRE",
    "dec, 9 2016",
    "DataColor",
    ColorCheckerTarget::Spyder24V2,
    2.f / 3.f, // ratio
    0.035f, // radius
    24, // patches
    { 4, 6 }, // size
    3, // middle grey
    0, // white
    5, // black
    kSpyder24V2Values,
};

// spyder_48
inline constexpr ColorCheckerPatch kSpyder48Values[] = {
    { "A1", { 61.35f, 34.81f, 18.38f }, 0.071f, 0.107f },
    { "A2", { 75.5f, 5.84f, 50.42f }, 0.071f, 0.264f },
    { "A3", { 66.82f, -25.1f, 23.47f }, 0.071f, 0.421f },
    { "A4", { 60.53f, -22.6f, -20.4f }, 0.071f, 0.579f },
    { "A5", { 59.66f, -2.03f, -28.46f }, 0.071f, 0.736f },
    { "A6", { 59.15f, 30.83f, -5.72f }, 0.071f, 0.893f },
    { "B1", { 82.68f, 5.03f, 3.02f }, 0.175f, 0.107f },
    { "B2", { 82.25f, -2.42f, 3.78f }, 0.175f, 0.264f },
    { "B3", { 82.29f, 2.2f, -2.04f }, 0.175f, 0.421f },
    { "B4", { 24.89f, 4.43f, 0.78f }, 0.175f, 0.579f },
    { "B5", { 25.16f, -3.88f, 2.13f }, 0.175f, 0.736f },
    { "B6", { 26.13f, 2.61f, -5.03f }, 0.175f, 0.893f },
    { "C1", { 85.42f, 9.41f, 14.49f }, 0.279f, 0.107f },
    { "C2", { 74.28f, 9.05f, 27.21f }, 0.279f, 0.264f },
    { "C3", { 64.57f, 12.39f, 37.24f }, 0.279f, 0.421f },
    { "C4", { 44.49f, 17.23f, 26.24f }, 0.279f, 0.579f },
    { "C5", { 25.29f, 7.95f, 8.87f }, 0.279f, 0.736f },
    { "C6", { 22.67f, 2.11f, -1.1f }, 0.279f, 0.893f },
    { "D1", { 92.72f, 1.89f, 2.76f }, 0.384f, 0.107f },
    { "D2", { 88.85f, 1.59f, 2.27f }, 0.384f, 0.264f },
    { "D3", { 73.42f, 0.99f, 1.89f }, 0.384f, 0.421f },
    { "D4", { 57.15f, 0.57f, 1.19f }, 0.384f, 0.579f },
    { "D5", { 41.57f, 0.24f, 1.45f }, 0.384f, 0.736f },
    { "D6", { 25.65f, 1.24f, 0.05f }, 0.384f, 0.893f },
    { "E1", { 96.04f, 2.16f, 2.6f }, 0.616f, 0.107f },
    { "E2", { 80.44f, 1.17f, 2.05f }, 0.616f, 0.264f },
    { "E3", { 65.52f, 0.69f, 1.86f }, 0.616f, 0.421f },
    { "E4", { 49.62f, 0.58f, 1.56f }, 0.616f, 0.579f },
    { "E5", { 33.55f, 0.35f, 1.4f }, 0.616f, 0.736f },
    { "E6", { 16.91f, 1.43f, -0.81f }, 0.616f, 0.893f },
    { "F1", { 47.12f, -32.5f, -28.75f }, 0.721f, 0.107f },
    { "F2", { 50.49f, 53.45f, -13.55f }, 0.721f, 0.264f },
    { "F3", { 83.61f, 3.36f, 87.02f }, 0.721f, 0.421f },
    { "F4", { 41.05f, 60.75f, 31.17f }, 0.721f, 0.579f },
    { "F5", { 54.14f, -40.8f, 34.75f }, 0.721f, 0.736f },
    { "F6", { 24.75f, 13.78f, -49.48f }, 0.721f, 0.893f },
    { "G1", { 60.94f, 38.21f, 61.31f }, 0.825f, 0.107f },
    { "G2", { 37.8f, 7.3f, -43.04f }, 0.825f, 0.264f },
    { "G3", { 49.81f, 48.5f, 15.76f }, 0.825f, 0.421f },
    { "G4", { 28.88f, 19.36f, -24.48f }, 0.825f, 0.579f },
    { "G5", { 72.45f, -23.6f, 60.47f }, 0.825f, 0.736f },
    { "G6", { 71.65f, 23.74f, 72.28f }, 0.825f, 0.893f },
    { "H1", { 70.19f, -31.9f, 1.98f }, 0.929f, 0.107f },
    { "H2", { 54.38f, 8.84f, -25.71f }, 0.929f, 0.264f },
    { "H3", { 42.03f, -15.8f, 22.93f }, 0.929f, 0.421f },
    { "H4", { 48.82f, -5.11f, -23.08f }, 0.929f, 0.579f },
    { "H5", { 65.1f, 18.14f, 18.68f }, 0.929f, 0.736f },
    { "H6", { 36.13f, 14.15f, 15.78f }, 0.929f, 0.893f },
};

inline constexpr ColorChecker kSpyder48 = {
    "Datacolor SpyderCheckr 48 before 2018",
    "Aurélien PIERRE",
    "dec, 9 2016",
    "DataColor",
    ColorCheckerTarget::Spyder48,
    2.f / 3.f, // ratio
    0.035f, // radius
    48, // patches
    { 8, 6 }, // size
    21, // middle grey
    24, // white
    29, // black
    kSpyder48Values,
};

// spyder_48_v2
inline constexpr ColorCheckerPatch kSpyder48V2Values[] = {
    { "A1", { 61.35f, 34.81f, 18.38f }, 0.071f, 0.107f },
    { "A2", { 75.5f, 5.84f, 50.42f }, 0.071f, 0.264f },
    { "A3", { 66.82f, -25.1f, 23.47f }, 0.071f, 0.421f },
    { "A4", { 60.53f, -22.62f, -20.4f }, 0.071f, 0.579f },
    { "A5", { 59.66f, -2.03f, -28.46f }, 0.071f, 0.736f },
    { "A6", { 59.15f, 30.83f, -5.72f }, 0.071f, 0.893f },
    { "B1", { 82.68f, 5.03f, 3.02f }, 0.175f, 0.107f },
    { "B2", { 82.25f, -2.42f, 3.78f }, 0.175f, 0.264f },
    { "B3", { 82.29f, 2.2f, -2.04f }, 0.175f, 0.421f },
    { "B4", { 24.89f, 4.43f, 0.78f }, 0.175f, 0.579f },
    { "B5", { 25.16f, -3.88f, 2.13f }, 0.175f, 0.736f },
    { "B6", { 26.13f, 2.61f, -5.03f }, 0.175f, 0.893f },
    { "C1", { 85.42f, 9.41f, 14.49f }, 0.279f, 0.107f },
    { "C2", { 74.28f, 9.05f, 27.21f }, 0.279f, 0.264f },
    { "C3", { 64.57f, 12.39f, 37.24f }, 0.279f, 0.421f },
    { "C4", { 44.49f, 17.23f, 26.24f }, 0.279f, 0.579f },
    { "C5", { 25.29f, 7.95f, 8.87f }, 0.279f, 0.736f },
    { "C6", { 22.67f, 2.11f, -1.1f }, 0.279f, 0.893f },
    { "D1", { 92.72f, 1.89f, 2.76f }, 0.384f, 0.107f },
    { "D2", { 88.85f, 1.59f, 2.27f }, 0.384f, 0.264f },
    { "D3", { 73.42f, 0.99f, 1.89f }, 0.384f, 0.421f },
    { "D4", { 57.15f, 0.57f, 1.19f }, 0.384f, 0.579f },
    { "D5", { 41.57f, 0.24f, 1.45f }, 0.384f, 0.736f },
    { "D6", { 25.65f, 1.24f, 0.05f }, 0.384f, 0.893f },
    { "E1", { 96.04f, 2.16f, 2.6f }, 0.616f, 0.107f },
    { "E2", { 80.44f, 1.17f, 2.05f }, 0.616f, 0.264f },
    { "E3", { 65.52f, 0.69f, 1.86f }, 0.616f, 0.421f },
    { "E4", { 49.62f, 0.58f, 1.56f }, 0.616f, 0.579f },
    { "E5", { 33.55f, 0.35f, 1.4f }, 0.616f, 0.736f },
    { "E6", { 16.91f, 1.43f, -0.81f }, 0.616f, 0.893f },
    { "F1", { 47.12f, -32.52f, -28.75f }, 0.721f, 0.107f },
    { "F2", { 50.49f, 53.45f, -13.55f }, 0.721f, 0.264f },
    { "F3", { 83.61f, 3.36f, 87.02f }, 0.721f, 0.421f },
    { "F4", { 41.05f, 60.75f, 31.17f }, 0.721f, 0.579f },
    { "F5", { 54.14f, -40.76f, 34.75f }, 0.721f, 0.736f },
    { "F6", { 24.75f, 13.78f, -49.48f }, 0.721f, 0.893f },
    { "G1", { 60.94f, 38.21f, 61.31f }, 0.825f, 0.107f },
    { "G2", { 37.8f, 7.3f, -43.04f }, 0.825f, 0.264f },
    { "G3", { 49.81f, 48.5f, 15.76f }, 0.825f, 0.421f },
    { "G4", { 28.88f, 19.36f, -24.48f }, 0.825f, 0.579f },
    { "G5", { 72.45f, -23.57f, 60.47f }, 0.825f, 0.736f },
    { "G6", { 71.65f, 23.74f, 72.28f }, 0.825f, 0.893f },
    { "H1", { 70.19f, -31.85f, 1.98f }, 0.929f, 0.107f },
    { "H2", { 54.38f, 8.84f, -25.71f }, 0.929f, 0.264f },
    { "H3", { 42.03f, -15.78f, 22.93f }, 0.929f, 0.421f },
    { "H4", { 48.82f, -5.11f, -23.08f }, 0.929f, 0.579f },
    { "H5", { 65.1f, 18.14f, 18.68f }, 0.929f, 0.736f },
    { "H6", { 36.13f, 14.15f, 15.78f }, 0.929f, 0.893f },
};

inline constexpr ColorChecker kSpyder48V2 = {
    "Datacolor SpyderCheckr 48 after 2018",
    "Aurélien PIERRE",
    "dec, 9 2016",
    "DataColor",
    ColorCheckerTarget::Spyder48V2,
    2.f / 3.f, // ratio
    0.035f, // radius
    48, // patches
    { 8, 6 }, // size
    21, // middle grey
    24, // white
    29, // black
    kSpyder48V2Values,
};

// spyder_photo
inline constexpr ColorCheckerPatch kSpyderPhotoValues[] = {
    { "A1", { 61.35f, 34.81f, 18.38f }, 0.068f, 0.146f },
    { "A2", { 75.5f, 5.84f, 50.42f }, 0.068f, 0.288f },
    { "A3", { 66.82f, -25.1f, 23.47f }, 0.068f, 0.429f },
    { "A4", { 60.53f, -22.62f, -20.4f }, 0.068f, 0.571f },
    { "A5", { 59.66f, -2.03f, -28.46f }, 0.068f, 0.712f },
    { "A6", { 59.15f, 30.83f, -5.72f }, 0.068f, 0.854f },
    { "B1", { 82.68f, 5.03f, 3.02f }, 0.168f, 0.146f },
    { "B2", { 82.25f, -2.42f, 3.78f }, 0.168f, 0.288f },
    { "B3", { 82.29f, 2.2f, -2.04f }, 0.168f, 0.429f },
    { "B4", { 24.89f, 4.43f, 0.78f }, 0.168f, 0.571f },
    { "B5", { 25.16f, -3.88f, 2.13f }, 0.168f, 0.712f },
    { "B6", { 26.13f, 2.61f, -5.03f }, 0.168f, 0.854f },
    { "C1", { 85.42f, 9.41f, 14.49f }, 0.268f, 0.146f },
    { "C2", { 74.28f, 9.05f, 27.21f }, 0.268f, 0.288f },
    { "C3", { 64.57f, 12.39f, 37.24f }, 0.268f, 0.429f },
    { "C4", { 44.49f, 17.23f, 26.24f }, 0.268f, 0.571f },
    { "C5", { 25.29f, 7.95f, 8.87f }, 0.268f, 0.712f },
    { "C6", { 22.67f, 2.11f, -1.1f }, 0.268f, 0.854f },
    { "D1", { 92.72f, 1.89f, 2.76f }, 0.368f, 0.146f },
    { "D2", { 88.85f, 1.59f, 2.27f }, 0.368f, 0.288f },
    { "D3", { 73.42f, 0.99f, 1.89f }, 0.368f, 0.429f },
    { "D4", { 57.15f, 0.57f, 1.19f }, 0.368f, 0.571f },
    { "D5", { 41.57f, 0.24f, 1.45f }, 0.368f, 0.712f },
    { "D6", { 25.65f, 1.24f, 0.05f }, 0.368f, 0.854f },
    { "E1", { 96.04f, 2.16f, 2.6f }, 0.632f, 0.146f },
    { "E2", { 80.44f, 1.17f, 2.05f }, 0.632f, 0.288f },
    { "E3", { 65.52f, 0.69f, 1.86f }, 0.632f, 0.429f },
    { "E4", { 49.62f, 0.58f, 1.56f }, 0.632f, 0.571f },
    { "E5", { 33.55f, 0.35f, 1.4f }, 0.632f, 0.712f },
    { "E6", { 16.91f, 1.43f, -0.81f }, 0.632f, 0.854f },
    { "F1", { 47.12f, -32.52f, -28.75f }, 0.732f, 0.146f },
    { "F2", { 50.49f, 53.45f, -13.55f }, 0.732f, 0.288f },
    { "F3", { 83.61f, 3.36f, 87.02f }, 0.732f, 0.429f },
    { "F4", { 41.05f, 60.75f, 31.17f }, 0.732f, 0.571f },
    { "F5", { 54.14f, -40.76f, 34.75f }, 0.732f, 0.712f },
    { "F6", { 24.75f, 13.78f, -49.48f }, 0.732f, 0.854f },
    { "G1", { 60.94f, 38.21f, 61.31f }, 0.832f, 0.146f },
    { "G2", { 37.8f, 7.3f, -43.04f }, 0.832f, 0.288f },
    { "G3", { 49.81f, 48.5f, 15.76f }, 0.832f, 0.429f },
    { "G4", { 28.88f, 19.36f, -24.48f }, 0.832f, 0.571f },
    { "G5", { 72.45f, -23.57f, 60.47f }, 0.832f, 0.712f },
    { "G6", { 71.65f, 23.74f, 72.28f }, 0.832f, 0.854f },
    { "H1", { 70.19f, -31.85f, 1.98f }, 0.932f, 0.146f },
    { "H2", { 54.38f, 8.84f, -25.71f }, 0.932f, 0.288f },
    { "H3", { 42.03f, -15.78f, 22.93f }, 0.932f, 0.429f },
    { "H4", { 48.82f, -5.11f, -23.08f }, 0.932f, 0.571f },
    { "H5", { 65.1f, 18.14f, 18.68f }, 0.932f, 0.712f },
    { "H6", { 36.13f, 14.15f, 15.78f }, 0.932f, 0.854f },
};

inline constexpr ColorChecker kSpyderPhoto = {
    "Datacolor SpyderCheckr Photo",
    "Daniel Hauck",
    "dec, 20 2022",
    "DataColor",
    ColorCheckerTarget::SpyderPhoto,
    106.f / 150.f, // ratio
    0.059f, // radius
    48, // patches
    { 8, 6 }, // size
    24, // middle grey
    21, // white
    29, // black
    kSpyderPhotoValues,
};

/**
 * @brief dt_get_color_checker(): Last (and anything out of range) gives the Xrite chart after 2014.
 **/
inline const ColorChecker&
colorChecker(ColorCheckerTarget target)
{
    switch (target) {
    case ColorCheckerTarget::Xrite24_2000:
        return kXrite242000;
    case ColorCheckerTarget::Xrite24_2014:
        return kXrite242014;
    case ColorCheckerTarget::Spyder24:
        return kSpyder24;
    case ColorCheckerTarget::Spyder24V2:
        return kSpyder24V2;
    case ColorCheckerTarget::Spyder48:
        return kSpyder48;
    case ColorCheckerTarget::Spyder48V2:
        return kSpyder48V2;
    case ColorCheckerTarget::SpyderPhoto:
        return kSpyderPhoto;
    case ColorCheckerTarget::Last:
        break;
    }

    return kXrite242014;
}

/**
 * @brief dt_color_checker_get_index(): the patch at coordinates (column, line); patches are
 * stored column-major.
 **/
inline std::size_t
colorCheckerIndex(const ColorChecker& checker,
                  const std::size_t coordinates[2])
{
    const std::size_t height = checker.size[1];

    return (std::min)(height * coordinates[0] + coordinates[1], checker.patches - 1);
}

/**
 * @brief dt_color_checker_get_coordinates(): the (column, line) of patch index.
 **/
inline void
colorCheckerCoordinates(const ColorChecker& checker,
                        std::size_t* coordinates,
                        std::size_t index)
{
    const std::size_t idx = (std::min)(index, checker.patches - 1);
    const std::size_t height = checker.size[1];
    const std::size_t numCol = idx / height;
    const std::size_t numLin = idx - numCol * height;

    coordinates[0] = (std::min)(numCol, checker.size[0] - 1);
    coordinates[1] = (std::min)(numLin, checker.size[1] - 1);
}
} // namespace RawColorCheckerData

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Raw_RawColorCheckerData_h
