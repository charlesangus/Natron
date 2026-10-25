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

#include "Posterize.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"
#include "Engine/NodeMetadata.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {

// The dither matrices of openfx-misc's Quantize: Bayer, and void-and-cluster from
// http://caca.zoy.org/study/part2.html.
const unsigned short kBayer2[2][2] = {
    { 1, 2 },
    { 3, 0 }
};

const unsigned short kBayer4[4][4] = {
    { 5, 9, 6, 10 },
    { 13, 1, 14, 2 },
    { 7, 11, 4, 8 },
    { 15, 3, 12, 0 }
};

const unsigned short kBayer8[8][8] = {
    { 0, 32, 8, 40, 2, 34, 10, 42 },
    { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44, 4, 36, 14, 46, 6, 38 },
    { 60, 28, 52, 20, 62, 30, 54, 22 },
    { 3, 35, 11, 43, 1, 33, 9, 41 },
    { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47, 7, 39, 13, 45, 5, 37 },
    { 63, 31, 55, 23, 61, 29, 53, 21 }
};

const unsigned short kVac14[14][14] = {
    { 131, 187, 8, 78, 50, 18, 134, 89, 155, 102, 29, 95, 184, 73 },
    { 22, 86, 113, 171, 142, 105, 34, 166, 9, 60, 151, 128, 40, 110 },
    { 168, 137, 45, 28, 64, 188, 82, 54, 124, 189, 80, 13, 156, 56 },
    { 7, 61, 186, 121, 154, 6, 108, 177, 24, 100, 38, 176, 93, 123 },
    { 83, 148, 96, 17, 88, 133, 44, 145, 69, 161, 139, 72, 30, 181 },
    { 115, 27, 163, 47, 178, 65, 164, 14, 120, 48, 5, 127, 153, 52 },
    { 190, 58, 126, 81, 116, 21, 106, 77, 173, 92, 191, 63, 99, 12 },
    { 76, 144, 4, 185, 37, 149, 192, 39, 135, 23, 117, 31, 170, 132 },
    { 35, 172, 103, 66, 129, 79, 3, 97, 57, 159, 70, 141, 53, 94 },
    { 114, 20, 49, 158, 19, 146, 169, 122, 183, 11, 104, 180, 2, 165 },
    { 152, 87, 182, 118, 91, 42, 67, 25, 84, 147, 43, 85, 125, 68 },
    { 16, 136, 71, 10, 193, 112, 160, 138, 51, 111, 162, 26, 194, 46 },
    { 174, 107, 41, 143, 33, 74, 1, 101, 195, 15, 75, 140, 109, 90 },
    { 32, 62, 157, 98, 167, 119, 179, 59, 36, 130, 175, 55, 0, 150 }
};

const unsigned short kVac25[25][25] = {
    { 165, 530, 106, 302, 540, 219, 477, 100, 231, 417, 314, 223, 424, 37, 207, 434, 326, 22, 448, 338, 111, 454, 523, 278, 579 },
    { 334, 19, 410, 495, 57, 352, 158, 318, 598, 109, 509, 157, 524, 282, 606, 83, 225, 539, 163, 234, 607, 313, 206, 71, 470 },
    { 251, 608, 216, 135, 275, 609, 415, 29, 451, 204, 397, 21, 373, 107, 462, 348, 482, 120, 362, 508, 33, 147, 572, 388, 142 },
    { 447, 77, 345, 565, 439, 104, 215, 546, 279, 69, 567, 311, 585, 258, 177, 17, 266, 601, 55, 428, 270, 461, 331, 26, 560 },
    { 164, 271, 486, 186, 16, 336, 457, 150, 342, 471, 245, 161, 56, 396, 496, 555, 385, 146, 321, 190, 526, 97, 182, 511, 297 },
    { 429, 553, 49, 374, 536, 263, 575, 43, 501, 124, 368, 538, 450, 121, 309, 84, 210, 449, 561, 79, 356, 610, 256, 378, 58 },
    { 105, 315, 156, 244, 423, 118, 183, 408, 220, 611, 15, 198, 293, 596, 221, 375, 581, 39, 238, 500, 287, 14, 437, 139, 595 },
    { 227, 403, 590, 478, 68, 612, 295, 517, 87, 312, 413, 515, 78, 433, 13, 476, 134, 340, 414, 160, 466, 213, 547, 324, 456 },
    { 542, 141, 12, 335, 214, 357, 11, 381, 242, 469, 159, 265, 383, 176, 545, 285, 197, 503, 108, 576, 51, 187, 98, 200, 34 },
    { 358, 489, 277, 570, 96, 441, 554, 123, 534, 52, 556, 112, 605, 330, 70, 392, 613, 28, 288, 361, 232, 602, 300, 502, 267 },
    { 102, 195, 399, 152, 484, 264, 166, 289, 427, 192, 298, 407, 25, 249, 520, 114, 233, 444, 543, 170, 498, 131, 452, 66, 562 },
    { 310, 586, 54, 531, 346, 42, 614, 354, 23, 588, 491, 151, 468, 353, 187, 483, 369, 153, 85, 425, 10, 276, 371, 174, 420 },
    { 32, 459, 222, 304, 136, 421, 103, 458, 230, 339, 67, 260, 578, 93, 544, 9, 280, 594, 327, 248, 582, 472, 50, 615, 254 },
    { 537, 359, 91, 600, 475, 212, 525, 168, 558, 128, 455, 370, 179, 301, 405, 209, 467, 48, 442, 127, 355, 184, 332, 481, 126 },
    { 286, 175, 436, 273, 31, 377, 306, 36, 412, 294, 616, 8, 473, 60, 603, 116, 347, 532, 191, 568, 61, 522, 90, 218, 391 },
    { 592, 62, 514, 122, 552, 149, 617, 241, 513, 81, 202, 272, 557, 333, 226, 507, 255, 72, 305, 402, 229, 418, 296, 551, 7 },
    { 411, 317, 236, 416, 337, 480, 64, 389, 132, 350, 487, 404, 89, 162, 435, 44, 419, 618, 113, 505, 20, 604, 138, 465, 188 },
    { 493, 133, 580, 6, 169, 259, 320, 548, 193, 593, 40, 178, 512, 364, 591, 144, 319, 196, 386, 261, 351, 205, 384, 76, 269 },
    { 38, 349, 208, 504, 440, 99, 490, 5, 426, 243, 322, 574, 281, 4, 237, 460, 527, 3, 549, 155, 577, 47, 533, 316, 619 },
    { 394, 519, 82, 268, 325, 566, 199, 299, 119, 529, 75, 400, 125, 492, 344, 86, 217, 308, 463, 80, 395, 284, 474, 117, 201 },
    { 95, 235, 422, 620, 143, 45, 372, 597, 453, 343, 185, 479, 247, 569, 171, 409, 584, 129, 365, 239, 488, 94, 224, 438, 559 },
    { 283, 541, 18, 194, 401, 516, 262, 148, 41, 250, 621, 24, 329, 92, 446, 27, 291, 485, 35, 622, 180, 535, 379, 30, 341 },
    { 443, 145, 363, 494, 246, 101, 445, 550, 390, 499, 115, 432, 521, 211, 623, 253, 528, 189, 430, 307, 53, 323, 130, 624, 172 },
    { 46, 589, 292, 63, 599, 328, 203, 74, 290, 181, 376, 274, 140, 393, 59, 367, 88, 380, 137, 506, 252, 571, 431, 240, 497 },
    { 382, 228, 464, 167, 398, 2, 573, 366, 518, 1, 583, 73, 563, 303, 510, 154, 564, 257, 587, 65, 406, 173, 0, 360, 110 }
};

// The thresholds of a side x side matrix m: an entry k of the side² ranks becomes
// (k + 0.5) / side², so a fraction at or below it takes the lower level. This is Quantize's
// test, (fraction + 0.5 / side²)·side² <= k + 1, with the half step moved to the threshold.
std::vector<float>
thresholdTable(const unsigned short* m,
               int side)
{
    const int n = side * side;
    std::vector<float> t((size_t)n);

    for (int i = 0; i < n; ++i) {
        t[(size_t)i] = (float)((m[i] + 0.5) / n);
    }

    return t;
}

KnobDoublePtr
addDouble(KnobHolder* holder,
          const KnobPagePtr& page,
          const char* name,
          const char* label,
          const char* hint,
          double defaultValue,
          double minimum,
          double displayMin,
          double displayMax)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, std::string(label));

    knob->setName(name);
    knob->setHintToolTip(std::string(hint));
    knob->setMinimum(minimum);
    knob->setMaximum(DBL_MAX);
    knob->setDisplayMinimum(displayMin);
    knob->setDisplayMaximum(displayMax);
    knob->setDefaultValue(defaultValue);
    page->addKnob(knob);

    return knob;
}

int
wrapIndex(int v,
          int side)
{
    const int r = v % side;

    return (r < 0) ? (r + side) : r;
}

#ifdef NATRON_HAVE_GPU
// The parameter block's header in posterize.slang, followed by the matrix's thresholds.
const int kGpuHeaderWords = 7;
#endif
} // anonymous namespace

std::uint32_t
PosterizeOp::hash(std::uint32_t a)
{
    a = (a ^ 61u) ^ (a >> 16);
    a = a + (a << 3);
    a = a ^ (a >> 4);
    a = a * 0x27d4eb2du;
    a = a ^ (a >> 15);

    return a;
}

int
PosterizeOp::matrixSize(int dither)
{
    switch (dither) {
    case eDitherBayer2:
        return 2;
    case eDitherBayer4:
        return 4;
    case eDitherBayer8:
        return 8;
    case eDitherBlueNoise14:
        return 14;
    case eDitherBlueNoise25:
        return 25;
    default:
        return 0;
    }
}

const float*
PosterizeOp::thresholds(int dither)
{
    static const std::vector<float> bayer2 = thresholdTable(&kBayer2[0][0], 2);
    static const std::vector<float> bayer4 = thresholdTable(&kBayer4[0][0], 4);
    static const std::vector<float> bayer8 = thresholdTable(&kBayer8[0][0], 8);
    static const std::vector<float> vac14 = thresholdTable(&kVac14[0][0], 14);
    static const std::vector<float> vac25 = thresholdTable(&kVac25[0][0], 25);

    switch (dither) {
    case eDitherBayer2:
        return bayer2.data();
    case eDitherBayer4:
        return bayer4.data();
    case eDitherBayer8:
        return bayer8.data();
    case eDitherBlueNoise14:
        return vac14.data();
    case eDitherBlueNoise25:
        return vac25.data();
    default:
        return nullptr;
    }
}

void
PosterizeOp::createKnobs(KnobHolder* holder,
                         const KnobPagePtr& page)
{
    levels = addDouble(holder, page, kPosterizeParamLevels, "Levels",
                       "The number of levels per channel from Range Min to Range Max, both included. It need not be a "
                       "whole number. Below 2 the image passes through.",
                       16., 0., 2., 256.);
    rangeMin = addDouble(holder, page, kPosterizeParamRangeMin, "Range Min",
                         "The lowest level. Values below it step on at the same spacing.",
                         0., -DBL_MAX, 0., 1.);
    rangeMax = addDouble(holder, page, kPosterizeParamRangeMax, "Range Max",
                         "The highest level. Values above it step on at the same spacing. At or below Range Min the image "
                         "passes through.",
                         1., -DBL_MAX, 0., 1.);
    {
        KnobChoicePtr knob = AppManager::createKnob<KnobChoice>(holder, std::string("Dither"));
        knob->setName(kPosterizeParamDither);
        knob->setHintToolTip(std::string("How the banding is broken up. A dithered value takes the level below or the one "
                                         "above it, the one above more often the closer the value is to it."));
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("none", "None", "No dithering: abrupt steps between levels."));
        options.push_back(ChoiceOption("bayer2x2", "Ordered (Bayer 2x2)", "Ordered dithering with a 2x2 Bayer matrix."));
        options.push_back(ChoiceOption("bayer4x4", "Ordered (Bayer 4x4)", "Ordered dithering with a 4x4 Bayer matrix."));
        options.push_back(ChoiceOption("bayer8x8", "Ordered (Bayer 8x8)", "Ordered dithering with an 8x8 Bayer matrix."));
        options.push_back(ChoiceOption("vac14x14", "Blue Noise (14x14)", "Ordered dithering with a 14x14 void-and-cluster blue-noise matrix."));
        options.push_back(ChoiceOption("vac25x25", "Blue Noise (25x25)", "Ordered dithering with a 25x25 void-and-cluster blue-noise matrix."));
        options.push_back(ChoiceOption("random", "Random", "White noise from a hash of the pixel position, the channel and the seed."));
        knob->populateChoices(options);
        knob->setDefaultValue((int)eDitherNone);
        knob->setAnimationEnabled(false);
        knob->setIsMetadataSlave(true);
        page->addKnob(knob);
        dither = knob;
    }
    {
        KnobIntPtr knob = AppManager::createKnob<KnobInt>(holder, std::string("Seed"));
        knob->setName(kPosterizeParamSeed);
        knob->setHintToolTip(std::string("The random dither's seed: change it to give different instances different noise."));
        knob->setDefaultValue(2000);
        knob->setAddNewLine(false);
        page->addKnob(knob);
        seed = knob;
    }
    {
        KnobBoolPtr knob = AppManager::createKnob<KnobBool>(holder, std::string("Static Seed"));
        knob->setName(kPosterizeParamStaticSeed);
        knob->setHintToolTip(std::string("Keep the random dither the same on every frame. Off, the frame is hashed into the "
                                         "seed and the noise changes every frame."));
        knob->setDefaultValue(false);
        knob->setAnimationEnabled(false);
        knob->setIsMetadataSlave(true);
        page->addKnob(knob);
        staticSeed = knob;
    }
} // PosterizeOp::createKnobs

PosterizeOp::Params
PosterizeOp::bake(const KnobReader& reader) const
{
    Params p;
    const double n = reader.value(levels);
    const double low = reader.value(rangeMin);
    const double high = reader.value(rangeMax);
    const int mode = reader.value(dither);

    p.dither = ((mode >= 0) && (mode < eDitherCount)) ? mode : (int)eDitherNone;
    // A random dither's seed: the knob's, hashed with the frame unless it is static, as
    // Quantize did.
    std::uint32_t s = hash((std::uint32_t)reader.value(seed));
    if (!reader.value(staticSeed)) {
        const float timeF = (float)reader.time();
        std::uint32_t timeBits = 0;
        std::memcpy(&timeBits, &timeF, sizeof(timeBits));
        s = hash(timeBits ^ s);
    }
    p.seed = s;
    const double range = high - low;
    if (!(n >= 2.) || !(range > 0.)) {
        return p;
    }
    p.identity = false;
    p.rangeMin = (float)low;
    p.binScale = (float)(n / range);
    p.levelScale = (float)((n - 1.) / range);
    p.outScale = (float)(range / (n - 1.));

    return p;
}

bool
PosterizeOp::isIdentity(const Params& params) const
{
    return params.identity;
}

float
PosterizeOp::quantize(const Params& params,
                      float v,
                      int x,
                      int y,
                      int bit)
{
    const float d = v - params.rangeMin;
    float level;

    if (params.dither == eDitherNone) {
        // Quantize's rounding: the bins are half open at the bottom above 0, so that a value
        // exactly on a level's upper bin edge stays on that level.
        const float t = d * params.binScale;
        if (t <= 0.f) {
            level = std::floor(t);
        } else {
            const float t1 = t - 1.f;
            level = std::ceil(t1);
        }
    } else {
        const float lv = d * params.levelScale;
        const float fv = std::floor(lv);
        const float f = lv - fv;
        float threshold;
        if (params.dither == eDitherRandom) {
            const std::uint32_t h = hash(hash(hash(params.seed ^ (std::uint32_t)x) ^ (std::uint32_t)y) ^ (std::uint32_t)bit);
            threshold = (float)(h >> 8) * (1.f / 16777216.f);
        } else {
            const int side = matrixSize(params.dither);
            threshold = thresholds(params.dither)[wrapIndex(x, side) * side + wrapIndex(y, side)];
        }
        if (f <= threshold) {
            level = fv;
        } else {
            level = fv + 1.f;
        }
    }
    const float o = level * params.outScale;

    return o + params.rangeMin;
}

void
PosterizeOp::row(const Params& params,
                 const RowIO& io) const
{
    const int nComps = io.nComps;
    int bit[4] = { 0, 1, 2, 3 };
    bool process[4] = { false, false, false, false };

    for (int c = 0; (c < nComps) && (c < 4); ++c) {
        bit[c] = pixelKernelChannelBit(nComps, c);
        process[c] = io.channels[bit[c]];
    }
    const float* src = io.src[0];
    float* dst = io.dst;
    for (int i = 0; i < io.width; ++i, src += nComps, dst += nComps) {
        const int x = io.x0 + i;
        for (int c = 0; (c < nComps) && (c < 4); ++c) {
            if (process[c]) {
                dst[c] = quantize(params, src[c], x, io.y, bit[c]);
            }
        }
    }
}

GpuOpRef
PosterizeOp::gpu(const Params& params) const
{
#ifdef NATRON_HAVE_GPU
    const int side = matrixSize(params.dither);
    const int kind = (params.dither == eDitherNone) ? eKindNone : ((params.dither == eDitherRandom) ? eKindRandom : eKindOrdered);
    std::vector<std::uint32_t> words((size_t)(kGpuHeaderWords + side * side), 0u);
    const auto putFloat = [&words](int i, float v) {
        std::memcpy(&words[(size_t)i], &v, sizeof(float));
    };

    words[0] = (std::uint32_t)kind;
    words[1] = (std::uint32_t)side;
    words[2] = params.seed;
    putFloat(3, params.rangeMin);
    putFloat(4, params.binScale);
    putFloat(5, params.levelScale);
    putFloat(6, params.outScale);
    const float* table = thresholds(params.dither);
    for (int i = 0; table && (i < side * side); ++i) {
        putFloat(kGpuHeaderWords + i, table[i]);
    }

    return GpuOpRef::packed((int)gpu::GpuKernelId::posterize_wrapped, std::move(words));
#else
    Q_UNUSED(params);

    return GpuOpRef::none();
#endif
}

Posterize::Posterize(NodePtr node)
    : ImageNode<PosterizeOp>(node)
{
}

Posterize::~Posterize()
{
}

PosterizeOp::Params
Posterize::bakePosterize(double time,
                         ViewIdx view) const
{
    return bakeParams(op(), time, view);
}

void
Posterize::posterizeRow(const PosterizeOp::Params& params,
                        const RowIO& io) const
{
    op().row(params, io);
}

StatusEnum
Posterize::getPreferredMetadata(NodeMetadata& metadata)
{
    KnobChoicePtr mode = op().dither.lock();
    KnobBoolPtr fixed = op().staticSeed.lock();

    if (mode && fixed && (mode->getValue() == PosterizeOp::eDitherRandom) && !fixed->getValue()) {
        metadata.setIsFrameVarying(true);
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
