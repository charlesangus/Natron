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

#include "LensCorrectProfile.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include <lensfun.h>

NATRON_NAMESPACE_ENTER

namespace LensCorrectProfile {
namespace {

    // The arithmetic below copies lensfun's mod-coord.cpp, mod-subpix.cpp and mod-color.cpp, including
    // which values are float and which double, so that it lands on lfModifier's answers.

    constexpr double kNewtonEps = 0.00001;
    constexpr double kEpsilon = 1.0e-10;
    constexpr float kThobyK1 = 1.47F;
    constexpr float kThobyK2 = 0.713F;
    constexpr float kHuge = 1.6e16F;

    void
    erectWrap(float& x, double& theta)
    {
        if (theta < 0) {
            theta = -theta;
            x += M_PI;
        }
        if (theta > M_PI) {
            theta = 2 * M_PI - theta;
            x += M_PI;
        }
    }

    void
    applyCoord(const CoordStep& step, float* io)
    {
        const float* t = step.terms;

        switch (step.op) {
        case CoordOp::Scale:
            io[0] = io[0] * t[0];
            io[1] = io[1] * t[0];
            break;
        case CoordOp::UnDistPoly3: {
            const float invK1 = 1.0 / t[0];
            const float x = io[0];
            const float y = io[1];
            const double rd = std::sqrt(x * x + y * y);
            if (rd == 0.0) {
                break;
            }
            const float rdDivK1 = rd * invK1;
            double ru = rd;
            for (int step = 0;; step++) {
                const double fru = ru * ru * ru + ru * invK1 - rdDivK1;
                if (fru >= -kNewtonEps && fru < kNewtonEps) {
                    break;
                }
                if (step > 5) {
                    io[0] = io[1] = std::numeric_limits<float>::quiet_NaN();

                    return;
                }
                ru -= fru / (3 * ru * ru + invK1);
            }
            if (ru < 0.0) {
                io[0] = io[1] = std::numeric_limits<float>::quiet_NaN();
                break;
            }
            ru /= rd;
            io[0] = x * ru;
            io[1] = y * ru;
            break;
        }
        case CoordOp::DistPoly3: {
            const float x = io[0];
            const float y = io[1];
            const float poly2 = t[0] * (x * x + y * y) + 1;
            io[0] = x * poly2;
            io[1] = y * poly2;
            break;
        }
        case CoordOp::UnDistPoly5: {
            const float k1 = t[0];
            const float k2 = t[1];
            const float x = io[0];
            const float y = io[1];
            const double rd = std::sqrt(x * x + y * y);
            if (rd == 0.0) {
                break;
            }
            double ru = rd;
            for (int step = 0;; step++) {
                const double ru2 = ru * ru;
                const double fru = ru * (1.0 + k1 * ru2 + k2 * ru2 * ru2) - rd;
                if (fru >= -kNewtonEps && fru < kNewtonEps) {
                    break;
                }
                if (step > 5) {
                    return;
                }
                ru -= fru / (1.0 + 3 * k1 * ru2 + 5 * k2 * ru2 * ru2);
            }
            if (ru < 0.0) {
                break;
            }
            ru /= rd;
            io[0] = x * ru;
            io[1] = y * ru;
            break;
        }
        case CoordOp::DistPoly5: {
            const float x = io[0];
            const float y = io[1];
            const float ru2 = x * x + y * y;
            const float poly4 = (1.0 + t[0] * ru2 + t[1] * ru2 * ru2);
            io[0] = x * poly4;
            io[1] = y * poly4;
            break;
        }
        case CoordOp::UnDistPTLens: {
            const float a = t[0];
            const float b = t[1];
            const float c = t[2];
            const float x = io[0];
            const float y = io[1];
            const double rd = std::sqrt(x * x + y * y);
            if (rd == 0.0) {
                break;
            }
            double ru = rd;
            for (int step = 0;; step++) {
                const double fru = ru * (a * ru * ru * ru + b * ru * ru + c * ru + 1) - rd;
                if (fru >= -kNewtonEps && fru < kNewtonEps) {
                    break;
                }
                if (step > 5) {
                    return;
                }
                ru -= fru / (4 * a * ru * ru * ru + 3 * b * ru * ru + 2 * c * ru + 1);
            }
            if (ru < 0.0) {
                break;
            }
            ru /= rd;
            io[0] = x * ru;
            io[1] = y * ru;
            break;
        }
        case CoordOp::DistPTLens: {
            const float x = io[0];
            const float y = io[1];
            const float ru2 = x * x + y * y;
            const float r = std::sqrt(ru2);
            const float poly3 = t[0] * ru2 * r + t[1] * ru2 + t[2] * r + 1;
            io[0] = x * poly3;
            io[1] = y * poly3;
            break;
        }
        case CoordOp::DistACM: {
            const float x = io[0];
            const float y = io[1];
            const float ru2 = x * x + y * y;
            const float ru4 = ru2 * ru2;
            const float common = 1.0 + t[0] * ru2 + t[1] * ru4 + t[2] * ru4 * ru2 + 2 * (t[3] * y + t[4] * x);
            io[0] = x * common + t[4] * ru2;
            io[1] = y * common + t[3] * ru2;
            break;
        }
        case CoordOp::GeomFishEyeRect: {
            const float x = io[0];
            const float y = io[1];
            const float r = std::sqrt(x * x + y * y);
            float rho;
            if (r >= M_PI / 2.0) {
                rho = kHuge;
            } else if (r == 0.0) {
                rho = 1.0;
            } else {
                rho = std::tan(r) / r;
            }
            io[0] = rho * x;
            io[1] = rho * y;
            break;
        }
        case CoordOp::GeomRectFishEye: {
            const float x = io[0];
            const float y = io[1];
            const float r = std::sqrt(x * x + y * y);
            const float theta = (r == 0.0) ? 1.0f : std::atan(r) / r;
            io[0] = theta * x;
            io[1] = theta * y;
            break;
        }
        case CoordOp::GeomPanoramicRect: {
            const float x = io[0];
            const float y = io[1];
            io[0] = std::tan(x);
            io[1] = y / std::cos(x);
            break;
        }
        case CoordOp::GeomRectPanoramic: {
            const float x = io[0];
            const float y = io[1];
            io[0] = std::atan(x);
            io[1] = y * std::cos(io[0]);
            break;
        }
        case CoordOp::GeomFishEyePanoramic:
        case CoordOp::GeomFishEyeERect: {
            const float x = io[0];
            const float y = io[1];
            const double r = std::sqrt(x * x + y * y);
            const double s = (r == 0.0) ? 1.0 : (std::sin(r) / r);
            const double vx = std::cos(r);
            const double vy = s * x;
            io[0] = std::atan2(vy, vx);
            if (step.op == CoordOp::GeomFishEyePanoramic) {
                io[1] = s * y / std::sqrt(vx * vx + vy * vy);
            } else {
                io[1] = std::atan(s * y / std::sqrt(vx * vx + vy * vy));
            }
            break;
        }
        case CoordOp::GeomPanoramicFishEye: {
            const float x = io[0];
            const float y = io[1];
            const double s = std::sin(x);
            const double r = std::sqrt(s * s + y * y);
            const double theta = (r == 0.0) ? 0.0 : std::atan2(r, std::cos(x)) / r;
            io[0] = theta * s;
            io[1] = theta * y;
            break;
        }
        case CoordOp::GeomERectRect: {
            float x = io[0];
            const float y = io[1];
            double theta = -y + M_PI / 2.0;
            erectWrap(x, theta);
            io[0] = std::tan(x);
            io[1] = 1.0 / (std::tan(theta) * std::cos(x));
            break;
        }
        case CoordOp::GeomRectERect: {
            const float x = io[0];
            const float y = io[1];
            io[0] = std::atan2(x, 1);
            io[1] = std::atan2(y, std::sqrt(1 + x * x));
            break;
        }
        case CoordOp::GeomERectFishEye: {
            float x = io[0];
            const float y = io[1];
            double theta = -y + M_PI / 2;
            erectWrap(x, theta);
            const double s = std::sin(theta);
            const double vx = s * std::sin(x);
            const double vy = std::cos(theta);
            double r = std::sqrt(vx * vx + vy * vy);
            theta = std::atan2(r, s * std::cos(x));
            r = 1.0 / r;
            io[0] = theta * vx * r;
            io[1] = theta * vy * r;
            break;
        }
        case CoordOp::GeomERectPanoramic:
            io[1] = std::tan(io[1]);
            break;
        case CoordOp::GeomPanoramicERect:
            io[1] = std::atan(io[1]);
            break;
        case CoordOp::GeomOrthographicERect:
        case CoordOp::GeomEquisolidERect:
        case CoordOp::GeomThobyERect: {
            const float x = io[0];
            const float y = io[1];
            double theta;
            if (step.op == CoordOp::GeomOrthographicERect) {
                const double r = std::sqrt(x * x + y * y);
                theta = (r < 1) ? std::asin(r) : M_PI / 2.0;
            } else if (step.op == CoordOp::GeomEquisolidERect) {
                const double r = std::sqrt(x * x + y * y);
                theta = (r < 2.0) ? 2.0 * std::asin(r / 2.0) : M_PI / 2.0;
            } else {
                const double rho = std::sqrt(x * x + y * y);
                if (rho < -kThobyK1 || rho > kThobyK1) {
                    io[0] = kHuge;
                    io[1] = kHuge;
                    break;
                }
                theta = std::asin(rho / kThobyK1) / kThobyK2;
            }
            const double phi = std::atan2(y, x);
            const double s = (theta == 0.0) ? 1.0 : (std::sin(theta) / theta);
            const double vx = std::cos(theta);
            const double vy = s * theta * std::cos(phi);
            io[0] = std::atan2(vy, vx);
            io[1] = std::atan(s * theta * std::sin(phi) / std::sqrt(vx * vx + vy * vy));
            break;
        }
        case CoordOp::GeomERectOrthographic:
        case CoordOp::GeomERectThoby: {
            float x = io[0];
            const float y = io[1];
            double theta = -y + M_PI / 2;
            erectWrap(x, theta);
            const double s = std::sin(theta);
            const double vx = s * std::sin(x);
            const double vy = std::cos(theta);
            theta = std::atan2(std::sqrt(vx * vx + vy * vy), s * std::cos(x));
            x = std::atan2(vy, vx);
            const double rho = (step.op == CoordOp::GeomERectOrthographic) ? std::sin(theta) : kThobyK1 * std::sin(theta * kThobyK2);
            io[0] = rho * std::cos(x);
            io[1] = rho * std::sin(x);
            break;
        }
        case CoordOp::GeomStereographicERect: {
            const float x = io[0];
            const float y = io[1];
            const double rh = std::sqrt(x * x + y * y);
            const double c = 2.0 * std::atan(rh / 2.0);
            const double sinc = std::sin(c);
            const double cosc = std::cos(c);
            io[0] = 0;
            if (std::fabs(rh) <= kEpsilon) {
                io[1] = kHuge;
            } else {
                io[1] = std::asin(y * sinc / rh);
                if ((std::fabs(cosc) >= kEpsilon) || (std::fabs(x) >= kEpsilon)) {
                    io[0] = std::atan2(x * sinc, cosc * rh);
                } else {
                    io[0] = kHuge;
                }
            }
            break;
        }
        case CoordOp::GeomERectStereographic: {
            const float lon = io[0];
            const float lat = io[1];
            const double cosphi = std::cos(lat);
            const double ksp = 2.0 / (1.0 + cosphi * std::cos(lon));
            io[0] = ksp * cosphi * std::sin(lon);
            io[1] = ksp * std::sin(lat);
            break;
        }
        case CoordOp::GeomERectEquisolid: {
            const double lambda = io[0];
            const double phi = io[1];
            if (std::fabs(std::cos(phi) * std::cos(lambda) + 1.0) <= kEpsilon) {
                io[0] = kHuge;
                io[1] = kHuge;
            } else {
                const double k1 = std::sqrt(2.0 / (1 + std::cos(phi) * std::cos(lambda)));
                io[0] = k1 * std::cos(phi) * std::sin(lambda);
                io[1] = k1 * std::sin(phi);
            }
            break;
        }
        } // switch
    } // applyCoord

    void
    unTcaPoly3Channel(float v, float c, float b, float* io)
    {
        const float x = io[0];
        const float y = io[1];
        const double rd = std::sqrt(x * x + y * y);

        if (rd == 0.0) {
            return;
        }
        double ru = rd;
        for (int step = 0;; step++) {
            const double ru2 = ru * ru;
            const double fru = b * ru2 * ru + c * ru2 + v * ru - rd;
            if (fru >= -kNewtonEps && fru < kNewtonEps) {
                break;
            }
            if (step > 5) {
                return;
            }
            ru -= fru / (3 * b * ru2 + 2 * c * ru + v);
        }
        if (ru > 0.0) {
            ru /= rd;
            io[0] = x * ru;
            io[1] = y * ru;
        }
    }

    void
    tcaPoly3Channel(float v, float c, float b, bool noC, float* io)
    {
        const float x = io[0];
        const float y = io[1];
        const float ru2 = x * x + y * y;
        const float poly2 = noC ? b * ru2 + v : b * ru2 + c * std::sqrt(ru2) + v;

        io[0] = x * poly2;
        io[1] = y * poly2;
    }

    void
    tcaAcmChannel(const float* k, float* io)
    {
        // k: the channel's terms 0..5 (alpha for red, beta for blue).
        const float x = io[0];
        const float y = io[1];
        const float ru2 = x * x + y * y;
        const float ru4 = ru2 * ru2;
        const float common = 1.0 + k[1] * ru2 + k[2] * ru4 + k[3] * ru4 * ru2 + 2 * (k[4] * y + k[5] * x);

        io[0] = k[0] * (x * common + k[5] * ru2);
        io[1] = k[0] * (y * common + k[4] * ru2);
    }

    float
    huginScaling(double realFocal, float cropFactor, float aspectRatio, bool withAspect)
    {
        const float millimeters = withAspect ? std::hypot(36.0, 24.0) / cropFactor / std::hypot(aspectRatio, 1) / 2.0
                                             : std::hypot(36.0, 24.0) / cropFactor / 2.0;

        return realFocal / millimeters;
    }

    struct PendingStep {
        int priority;
        CoordStep step;
    };

    struct Builder {
        const lfLens& lens;
        const Request& request;
        double realFocal = 0.;
        std::vector<PendingStep> coord;
        Profile profile;

        Builder(const lfLens& l,
                const Request& r)
            : lens(l)
            , request(r)
        {
            const double width = double(r.width >= 2 ? r.width - 1 : 1);
            const double height = double(r.height >= 2 ? r.height - 1 : 1);
            lfLensCalibDistortion lcd;

            realFocal = lens.InterpolateDistortion(r.crop, r.focal, lcd) ? lcd.RealFocal : r.focal;
            profile.normScale = std::hypot(36.0, 24.0) / double(r.crop) / std::hypot(width + 1.0, height + 1.0) / realFocal;
            profile.normUnScale = 1.0 / profile.normScale;
            const double size = std::min(width, height);
            profile.centerX = (width / 2.0 + size / 2.0 * lens.CenterX) * profile.normScale;
            profile.centerY = (height / 2.0 + size / 2.0 * lens.CenterY) * profile.normScale;
        }

        void add(CoordOp op,
                 int priority,
                 const float* terms = nullptr)
        {
            PendingStep p;

            p.priority = priority;
            p.step.op = op;
            if (terms) {
                std::memcpy(p.step.terms, terms, sizeof(p.step.terms));
            }
            coord.push_back(p);
        }

        void distortion()
        {
            lfLensCalibDistortion lcd;

            if (!lens.InterpolateDistortion(request.crop, request.focal, lcd)) {
                return;
            }
            const float s = huginScaling(realFocal, lcd.CalibAttr.CropFactor, lcd.CalibAttr.AspectRatio, true);
            float* t = lcd.Terms;
            switch (lcd.Model) {
            case LF_DIST_MODEL_POLY3: {
                const float d = 1 - t[0];
                t[0] *= std::pow(s, 2) / std::pow(d, 3);
                break;
            }
            case LF_DIST_MODEL_POLY5:
                t[0] *= std::pow(s, 2);
                t[1] *= std::pow(s, 4);
                break;
            case LF_DIST_MODEL_PTLENS: {
                const float d = 1 - t[0] - t[1] - t[2];
                t[0] *= std::pow(s, 3) / std::pow(d, 4);
                t[1] *= std::pow(s, 2) / std::pow(d, 3);
                t[2] *= s / std::pow(d, 2);
                break;
            }
            default:
                break;
            }
            if (request.reverse) {
                switch (lcd.Model) {
                case LF_DIST_MODEL_POLY3:
                    if (t[0] != 0) {
                        add(CoordOp::UnDistPoly3, 250, t);
                    }
                    break;
                case LF_DIST_MODEL_POLY5:
                    add(CoordOp::UnDistPoly5, 250, t);
                    break;
                case LF_DIST_MODEL_PTLENS:
                    add(CoordOp::UnDistPTLens, 250, t);
                    break;
                default:
                    // lensfun has no inverse of the ACM model.
                    break;
                }
            } else {
                switch (lcd.Model) {
                case LF_DIST_MODEL_POLY3:
                    add(CoordOp::DistPoly3, 750, t);
                    break;
                case LF_DIST_MODEL_POLY5:
                    add(CoordOp::DistPoly5, 750, t);
                    break;
                case LF_DIST_MODEL_PTLENS:
                    add(CoordOp::DistPTLens, 750, t);
                    break;
                case LF_DIST_MODEL_ACM:
                    add(CoordOp::DistACM, 750, t);
                    break;
                default:
                    break;
                }
            }
        } // distortion

        void projection(lfLensType target)
        {
            if ((target == LF_UNKNOWN) || (target == lens.Type) || (lens.Type == LF_UNKNOWN)) {
                return;
            }
            lfLensType from = lens.Type;
            lfLensType to = target;
            if (request.reverse) {
                from = target;
                to = lens.Type;
            }
            struct Direct {
                lfLensType from, to;
                CoordOp op;
            };
            static const Direct direct[] = {
                { LF_RECTILINEAR, LF_FISHEYE, CoordOp::GeomFishEyeRect },
                { LF_RECTILINEAR, LF_PANORAMIC, CoordOp::GeomPanoramicRect },
                { LF_RECTILINEAR, LF_EQUIRECTANGULAR, CoordOp::GeomERectRect },
                { LF_FISHEYE, LF_RECTILINEAR, CoordOp::GeomRectFishEye },
                { LF_FISHEYE, LF_PANORAMIC, CoordOp::GeomPanoramicFishEye },
                { LF_FISHEYE, LF_EQUIRECTANGULAR, CoordOp::GeomERectFishEye },
                { LF_PANORAMIC, LF_RECTILINEAR, CoordOp::GeomRectPanoramic },
                { LF_PANORAMIC, LF_FISHEYE, CoordOp::GeomFishEyePanoramic },
                { LF_PANORAMIC, LF_EQUIRECTANGULAR, CoordOp::GeomERectPanoramic },
                { LF_EQUIRECTANGULAR, LF_RECTILINEAR, CoordOp::GeomRectERect },
                { LF_EQUIRECTANGULAR, LF_FISHEYE, CoordOp::GeomFishEyeERect },
                { LF_EQUIRECTANGULAR, LF_PANORAMIC, CoordOp::GeomPanoramicERect },
            };
            for (const Direct& d : direct) {
                if ((d.from == from) && (d.to == to)) {
                    add(d.op, 500);

                    return;
                }
            }
            // Otherwise through equirectangular: target to equirectangular, then to the lens's.
            switch (to) {
            case LF_RECTILINEAR:
                add(CoordOp::GeomRectERect, 500);
                break;
            case LF_FISHEYE:
                add(CoordOp::GeomFishEyeERect, 500);
                break;
            case LF_PANORAMIC:
                add(CoordOp::GeomPanoramicERect, 500);
                break;
            case LF_FISHEYE_ORTHOGRAPHIC:
                add(CoordOp::GeomOrthographicERect, 500);
                break;
            case LF_FISHEYE_STEREOGRAPHIC:
                add(CoordOp::GeomStereographicERect, 500);
                break;
            case LF_FISHEYE_EQUISOLID:
                add(CoordOp::GeomEquisolidERect, 500);
                break;
            case LF_FISHEYE_THOBY:
                add(CoordOp::GeomThobyERect, 500);
                break;
            default:
                break;
            }
            switch (from) {
            case LF_RECTILINEAR:
                add(CoordOp::GeomERectRect, 500);
                break;
            case LF_FISHEYE:
                add(CoordOp::GeomERectFishEye, 500);
                break;
            case LF_PANORAMIC:
                add(CoordOp::GeomERectPanoramic, 500);
                break;
            case LF_FISHEYE_ORTHOGRAPHIC:
                add(CoordOp::GeomERectOrthographic, 500);
                break;
            case LF_FISHEYE_STEREOGRAPHIC:
                add(CoordOp::GeomERectStereographic, 500);
                break;
            case LF_FISHEYE_EQUISOLID:
                add(CoordOp::GeomERectEquisolid, 500);
                break;
            case LF_FISHEYE_THOBY:
                add(CoordOp::GeomERectThoby, 500);
                break;
            default:
                break;
            }
        } // projection

        void scaling(float scale)
        {
            if (scale == 1.0) {
                return;
            }
            if (scale == 0.0) {
                scale = autoScale(lens, request);
                if (scale == 0.0) {
                    return;
                }
            }
            const float factor = request.reverse ? scale : 1.0 / scale;
            add(CoordOp::Scale, request.reverse ? 900 : 100, &factor);
        }

        void tca(lfLensCalibTCA lctca)
        {
            const float s = huginScaling(realFocal, lctca.CalibAttr.CropFactor, lctca.CalibAttr.AspectRatio, true);
            float* t = lctca.Terms;
            switch (lctca.Model) {
            case LF_TCA_MODEL_LINEAR:
                if (request.reverse) {
                    for (int i = 0; i < 2; i++) {
                        t[i] = 1.0 / t[i];
                    }
                }
                break;
            case LF_TCA_MODEL_POLY3:
                t[2] *= s;
                t[3] *= s;
                t[4] *= s * s;
                t[5] *= s * s;
                break;
            default:
                break;
            }
            TcaOp op = TcaOp::None;
            switch (lctca.Model) {
            case LF_TCA_MODEL_LINEAR:
                op = TcaOp::Linear;
                break;
            case LF_TCA_MODEL_POLY3:
                op = request.reverse ? TcaOp::UnPoly3 : TcaOp::Poly3;
                break;
            case LF_TCA_MODEL_ACM:
                // lensfun has no inverse of the ACM model.
                op = request.reverse ? TcaOp::None : TcaOp::ACM;
                break;
            default:
                break;
            }
            profile.tca = op;
            if (op != TcaOp::None) {
                std::memcpy(profile.tcaTerms, t, sizeof(profile.tcaTerms));
            }
        } // tca

        void vignetting()
        {
            lfLensCalibVignetting lcv;

            if (!lens.InterpolateVignetting(request.crop, request.focal, request.aperture, request.distance, lcv)) {
                return;
            }
            if (lcv.Model == LF_VIGNETTING_MODEL_PA) {
                const float s = huginScaling(realFocal, lcv.CalibAttr.CropFactor, 1.f, false);
                lcv.Terms[0] *= std::pow(s, 2);
                lcv.Terms[1] *= std::pow(s, 4);
                lcv.Terms[2] *= std::pow(s, 6);
            } else if (lcv.Model != LF_VIGNETTING_MODEL_ACM) {
                return;
            }
            profile.vignetting = request.reverse ? VignettingOp::Vignette : VignettingOp::Devignette;
            std::memcpy(profile.vignettingTerms, lcv.Terms, sizeof(profile.vignettingTerms));
        }

        void finish()
        {
            std::stable_sort(coord.begin(), coord.end(), [](const PendingStep& a, const PendingStep& b) {
                return a.priority < b.priority;
            });
            profile.nCoordSteps = std::min<int>(int(coord.size()), kMaxCoordSteps);
            for (int i = 0; i < profile.nCoordSteps; ++i) {
                profile.coord[i] = coord[i].step;
            }
        }
    };

    lfLensCalibTCA
    customTca(const Request& request)
    {
        lfLensCalibTCA lctca;

        lctca.Model = LF_TCA_MODEL_LINEAR;
        lctca.Focal = request.focal;
        std::fill(std::begin(lctca.Terms), std::end(lctca.Terms), 0.f);
        lctca.Terms[0] = request.tcaRed;
        lctca.Terms[1] = request.tcaBlue;
        lctca.CalibAttr.CropFactor = request.crop;
        lctca.CalibAttr.AspectRatio = request.height > 0 ? float(request.width) / float(request.height) : 1.f;

        return lctca;
    }
} // anonymous namespace

void
Profile::map(float xu,
             float yu,
             float out[6]) const
{
    if (!movesPixels()) {
        out[0] = out[2] = out[4] = xu;
        out[1] = out[3] = out[5] = yu;

        return;
    }
    const float x = xu * normScale - centerX;
    const float y = yu * normScale - centerY;
    out[0] = out[2] = out[4] = x;
    out[1] = out[3] = out[5] = y;
    for (int i = 0; i < nCoordSteps; ++i) {
        for (int c = 0; c < 3; ++c) {
            applyCoord(coord[i], out + 2 * c);
        }
    }
    const float* t = tcaTerms;
    switch (tca) {
    case TcaOp::None:
        break;
    case TcaOp::Linear: {
        const float px = out[0];
        const float py = out[1];
        out[0] = px * t[0];
        out[1] = py * t[0];
        out[4] = px * t[1];
        out[5] = py * t[1];
        break;
    }
    case TcaOp::Poly3: {
        const bool noC = (t[2] == 0.0 && t[3] == 0.0);
        tcaPoly3Channel(t[0], t[2], t[4], noC, out);
        tcaPoly3Channel(t[1], t[3], t[5], noC, out + 4);
        break;
    }
    case TcaOp::UnPoly3:
        unTcaPoly3Channel(t[0], t[2], t[4], out);
        unTcaPoly3Channel(t[1], t[3], t[5], out + 4);
        break;
    case TcaOp::ACM: {
        float alpha[6], beta[6];
        for (int i = 0; i < 6; ++i) {
            alpha[i] = t[2 * i];
            beta[i] = t[2 * i + 1];
        }
        tcaAcmChannel(alpha, out);
        tcaAcmChannel(beta, out + 4);
        break;
    }
    }
    for (int i = 0; i < 6; i += 2) {
        out[i] = (out[i] + centerX) * normUnScale;
        out[i + 1] = (out[i + 1] + centerY) * normUnScale;
    }
} // Profile::map

float
Profile::vignettingGain(float x,
                        float y) const
{
    if (!hasVignetting()) {
        return 1.f;
    }
    x = x * normScale - centerX;
    y = y * normScale - centerY;
    const float r2 = x * x + y * y;
    const float r4 = r2 * r2;
    const float r6 = r4 * r2;
    const float c = 1.0 + vignettingTerms[0] * r2 + vignettingTerms[1] * r4 + vignettingTerms[2] * r6;

    return vignetting == VignettingOp::Devignette ? 1.0f / c : c;
}

Profile
build(const lfLens& lens,
      const Request& request)
{
    if ((request.width <= 0) || (request.height <= 0) || !(request.crop > 0.f) || !(request.focal > 0.f)) {
        return Profile();
    }
    Builder b(lens, request);

    // darktable's order: distortion, projection, scaling, TCA, vignetting.
    if (request.distortion) {
        b.distortion();
    }
    b.projection((lfLensType)request.targetGeometry);
    b.scaling(request.scale);
    if (request.tca) {
        if (request.tcaOverride) {
            b.tca(customTca(request));
        } else {
            lfLensCalibTCA lctca;
            if (lens.InterpolateTCA(request.crop, request.focal, lctca)) {
                b.tca(lctca);
            }
        }
    }
    if (request.vignetting) {
        b.vignetting();
    }
    b.finish();

    return b.profile;
}

float
autoScale(const lfLens& lens,
          const Request& request)
{
    if ((request.width <= 0) || (request.height <= 0) || !(request.crop > 0.f) || !(request.focal > 0.f)) {
        return 1.f;
    }
    lfModifier modifier(&lens, request.focal, request.crop, request.width, request.height, LF_PF_F32, request.reverse);

    if (request.distortion) {
        modifier.EnableDistortionCorrection();
    }
    modifier.EnableProjectionTransform((lfLensType)request.targetGeometry);
    if (request.tca) {
        if (request.tcaOverride) {
            modifier.EnableTCACorrection(customTca(request));
        } else {
            modifier.EnableTCACorrection();
        }
    }
    if (request.vignetting) {
        modifier.EnableVignettingCorrection(request.aperture, request.distance);
    }

    return modifier.GetAutoScale(request.reverse);
}
} // namespace LensCorrectProfile

NATRON_NAMESPACE_EXIT
