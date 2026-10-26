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
 * Ports of openfx-misc's DistortionModel.cpp and of its PFBarrel file reader (Distortion.cpp,
 * itself (C) 2011 The Pixel Farm Ltd), (C) 2013-2018 INRIA and 2018-2021 The Natron Developers,
 * GPL v2 or later.
 */

#include "LensModels.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <sstream>

#include "Engine/Nodes/Transform/LensModels3DE.h"

NATRON_NAMESPACE_ENTER

namespace LensModels {
namespace {
    enum NukeCoeff {
        kNukeK1 = 0,
        kNukeK2,
        kNukeCenterX,
        kNukeCenterY,
        kNukeSqueeze,
        kNukeAsymmetricX,
        kNukeAsymmetricY
    };

    enum PanoToolsCoeff {
        kPanoToolsA = 0,
        kPanoToolsB,
        kPanoToolsC
    };

    enum PFBarrelCoeff {
        kPFBarrelC3 = 0,
        kPFBarrelC5,
        kPFBarrelCenterX,
        kPFBarrelCenterY
    };

    void
    applyAffine(const Affine& a, double x, double y, double* ox, double* oy)
    {
        *ox = a.m00 * x + a.m01 * y + a.tx;
        *oy = a.m10 * x + a.m11 * y + a.ty;
    }

    // J = from * Jg * to, over the linear parts of the affines.
    void
    chainJacobian(const Affine& from, const double Jg[4], const Affine& to, double J[4])
    {
        const double a0 = Jg[0] * to.m00 + Jg[1] * to.m10;
        const double a1 = Jg[0] * to.m01 + Jg[1] * to.m11;
        const double a2 = Jg[2] * to.m00 + Jg[3] * to.m10;
        const double a3 = Jg[2] * to.m01 + Jg[3] * to.m11;
        J[0] = from.m00 * a0 + from.m01 * a2;
        J[1] = from.m00 * a1 + from.m01 * a3;
        J[2] = from.m10 * a0 + from.m11 * a2;
        J[3] = from.m10 * a1 + from.m11 * a3;
    }

    bool
    invert2x2(const double J[4], double out[4])
    {
        const double det = J[0] * J[3] - J[1] * J[2];
        if (!(std::fabs(det) > kMinDeterminant)) {
            return false;
        }
        out[0] = J[3] / det;
        out[1] = -J[1] / det;
        out[2] = -J[2] / det;
        out[3] = J[0] / det;
        return true;
    }

    bool
    isFiniteAffine(const Affine& a)
    {
        return std::isfinite(a.m00) && std::isfinite(a.m01) && std::isfinite(a.m10) && std::isfinite(a.m11) && std::isfinite(a.tx) && std::isfinite(a.ty);
    }

    bool
    coreNuke(const double* c, double xd, double yd, double* gx, double* gy, double J[4])
    {
        const double x = xd - c[kNukeCenterX];
        const double y = yd - c[kNukeCenterY];
        const double x2 = x * x;
        const double y2 = y * y;
        const double r2 = x2 + y2;
        const double kr = (c[kNukeK2] * r2 + c[kNukeK1]) * r2;
        // d(kr)/dx = x * dkr, d(kr)/dy = y * dkr
        const double dkr = 2. * (c[kNukeK1] + 2. * c[kNukeK2] * r2);
        const double squeeze = c[kNukeSqueeze];
        const double kry = 1. + (kr + c[kNukeAsymmetricY] * x2);
        const double krx = 1. + (kr + c[kNukeAsymmetricX] * y2) / squeeze;
        if ((krx == 0.) || (kry == 0.)) {
            return false;
        }
        *gx = x / krx + c[kNukeCenterX];
        *gy = y / kry + c[kNukeCenterY];
        const double krx_x = x * dkr / squeeze;
        const double krx_y = (y * dkr + 2. * c[kNukeAsymmetricX] * y) / squeeze;
        const double kry_x = x * dkr + 2. * c[kNukeAsymmetricY] * x;
        const double kry_y = y * dkr;
        J[0] = (krx - x * krx_x) / (krx * krx);
        J[1] = -x * krx_y / (krx * krx);
        J[2] = -y * kry_x / (kry * kry);
        J[3] = (kry - y * kry_y) / (kry * kry);
        return true;
    }

    bool
    corePanoTools(const double* c, double x, double y, double* gx, double* gy, double J[4])
    {
        const double a = c[kPanoToolsA];
        const double b = c[kPanoToolsB];
        const double cc = c[kPanoToolsC];
        const double d = 1. - (a + b + cc);
        const double r2 = x * x + y * y;
        const double r = std::sqrt(r2);
        const double scale = (a * r2 + cc) * r + b * r2 + d;
        *gx = x * scale;
        *gy = y * scale;
        J[0] = scale;
        J[1] = 0.;
        J[2] = 0.;
        J[3] = scale;
        if (r > 0.) {
            // (d scale / dr) / r
            const double ds = (3. * a * r2 + 2. * b * r + cc) / r;
            J[0] += ds * x * x;
            J[1] += ds * x * y;
            J[2] += ds * x * y;
            J[3] += ds * y * y;
        }
        return true;
    }

    bool
    corePFBarrel(const double* c, double x, double y, double* gx, double* gy, double J[4])
    {
        const double px = x - c[kPFBarrelCenterX];
        const double py = y - c[kPFBarrelCenterY];
        const double r2 = px * px + py * py;
        const double dr = r2 * (c[kPFBarrelC3] + r2 * c[kPFBarrelC5]);
        *gx = x + px * dr;
        *gy = y + py * dr;
        const double dd = 2. * (c[kPFBarrelC3] + 2. * c[kPFBarrelC5] * r2);
        J[0] = 1. + dr + dd * px * px;
        J[1] = dd * px * py;
        J[2] = dd * px * py;
        J[3] = 1. + dr + dd * py * py;
        return true;
    }

    // Solves core(u) = t for u, starting from u = t since every core is close to the identity.
    bool
    solveCore(const Model& model, double tx, double ty, double* ux, double* uy, double Jg[4])
    {
        double x = tx;
        double y = ty;
        double gx, gy;
        bool valid = evaluateCore(model, x, y, &gx, &gy, Jg);
        // Pull a start outside the domain towards the lens centre, where every model is defined.
        for (int k = 0; !valid && k < kNewtonMaxHalvings; ++k) {
            x *= 0.5;
            y *= 0.5;
            valid = evaluateCore(model, x, y, &gx, &gy, Jg);
        }
        if (!valid) {
            return false;
        }
        const double tol = kNewtonTolerance * (1. + std::sqrt(tx * tx + ty * ty));
        const double tol2 = tol * tol;
        double rx = gx - tx;
        double ry = gy - ty;
        double r2 = rx * rx + ry * ry;
        for (int iter = 0; r2 > tol2; ++iter) {
            if (iter == kNewtonMaxIterations) {
                return false;
            }
            const double det = Jg[0] * Jg[3] - Jg[1] * Jg[2];
            if (!(std::fabs(det) > kMinDeterminant)) {
                return false;
            }
            const double dx = (Jg[1] * ry - Jg[3] * rx) / det;
            const double dy = (Jg[2] * rx - Jg[0] * ry) / det;
            double lambda = 1.;
            bool accepted = false;
            for (int h = 0; h <= kNewtonMaxHalvings && !accepted; ++h) {
                const double nx = x + lambda * dx;
                const double ny = y + lambda * dy;
                double ngx, ngy, nJ[4];
                if (evaluateCore(model, nx, ny, &ngx, &ngy, nJ)) {
                    const double nrx = ngx - tx;
                    const double nry = ngy - ty;
                    const double nr2 = nrx * nrx + nry * nry;
                    if (nr2 < r2) {
                        x = nx;
                        y = ny;
                        rx = nrx;
                        ry = nry;
                        r2 = nr2;
                        Jg[0] = nJ[0];
                        Jg[1] = nJ[1];
                        Jg[2] = nJ[2];
                        Jg[3] = nJ[3];
                        accepted = true;
                    }
                }
                lambda *= 0.5;
            }
            if (!accepted) {
                return false;
            }
        }
        *ux = x;
        *uy = y;
        return true;
    }

    Model
    emptyModel(ModelType type, Direction core)
    {
        Model m {};
        m.type = type;
        m.core = core;
        return m;
    }

    // x' = sx * x + ox, y' = sy * y + oy.
    Affine
    axisAffine(double sx, double ox, double sy, double oy)
    {
        Affine a = { sx, 0., 0., sy, ox, oy };
        return a;
    }
} // anon namespace

Affine
affineCompose(const Affine& outer, const Affine& inner)
{
    Affine a;
    a.m00 = outer.m00 * inner.m00 + outer.m01 * inner.m10;
    a.m01 = outer.m00 * inner.m01 + outer.m01 * inner.m11;
    a.m10 = outer.m10 * inner.m00 + outer.m11 * inner.m10;
    a.m11 = outer.m10 * inner.m01 + outer.m11 * inner.m11;
    a.tx = outer.m00 * inner.tx + outer.m01 * inner.ty + outer.tx;
    a.ty = outer.m10 * inner.tx + outer.m11 * inner.ty + outer.ty;
    return a;
}

bool
affineInvert(const Affine& a, Affine* out)
{
    const double M[4] = { a.m00, a.m01, a.m10, a.m11 };
    double inv[4];
    if (!invert2x2(M, inv)) {
        return false;
    }
    Affine r;
    r.m00 = inv[0];
    r.m01 = inv[1];
    r.m10 = inv[2];
    r.m11 = inv[3];
    r.tx = -(inv[0] * a.tx + inv[1] * a.ty);
    r.ty = -(inv[2] * a.tx + inv[3] * a.ty);
    *out = r;
    return true;
}

void
finalizeModel(Model* model)
{
    model->valid = false;
    if (!isFiniteAffine(model->toCore) || !isFiniteAffine(model->fromCore)) {
        return;
    }
    for (int i = 0; i < kMaxCoeffs; ++i) {
        if (!std::isfinite(model->c[i])) {
            return;
        }
    }
    if (!affineInvert(model->toCore, &model->toCoreInverse) || !affineInvert(model->fromCore, &model->fromCoreInverse)) {
        return;
    }
    model->valid = true;
}

bool
evaluateCore(const Model& model, double x, double y, double* gx, double* gy, double J[4])
{
    bool ok = false;
    switch (model.type) {
    case eModelNuke:
        ok = coreNuke(model.c, x, y, gx, gy, J);
        break;
    case eModelPFBarrel:
        ok = corePFBarrel(model.c, x, y, gx, gy, J);
        break;
    case eModel3DEClassic:
    case eModel3DEAnamorphic6:
    case eModel3DEAnamorphic4:
        ok = coreAnamorphic3DE(model.c, x, y, gx, gy, J);
        break;
    case eModel3DEFisheye8:
        ok = coreFisheye3DE(model.c, x, y, gx, gy, J);
        break;
    case eModel3DEStandard:
        ok = coreStandard3DE(model.c, x, y, gx, gy, J);
        break;
    case eModelPanoTools:
        ok = corePanoTools(model.c, x, y, gx, gy, J);
        break;
    }
    return ok && std::isfinite(*gx) && std::isfinite(*gy) && std::isfinite(J[0]) && std::isfinite(J[1]) && std::isfinite(J[2]) && std::isfinite(J[3]);
}

bool
Model::map(Direction direction, double x, double y, Sample* out) const
{
    if (!valid) {
        return false;
    }
    double Jg[4];
    Sample s;
    if (direction == core) {
        double u, v, gx, gy;
        applyAffine(toCore, x, y, &u, &v);
        if (!evaluateCore(*this, u, v, &gx, &gy, Jg)) {
            return false;
        }
        applyAffine(fromCore, gx, gy, &s.x, &s.y);
        chainJacobian(fromCore, Jg, toCore, s.J);
        *out = s;
        return true;
    }
    double tx, ty, u, v;
    applyAffine(fromCoreInverse, x, y, &tx, &ty);
    if (!solveCore(*this, tx, ty, &u, &v, Jg)) {
        return false;
    }
    double Jf[4];
    chainJacobian(fromCore, Jg, toCore, Jf);
    if (!invert2x2(Jf, s.J)) {
        return false;
    }
    applyAffine(toCoreInverse, u, v, &s.x, &s.y);
    *out = s;
    return true;
}

bool
Model::distort(double x, double y, double* xd, double* yd) const
{
    Sample s;
    if (!map(eDirectionDistort, x, y, &s)) {
        return false;
    }
    *xd = s.x;
    *yd = s.y;
    return true;
}

bool
Model::undistort(double x, double y, double* xu, double* yu) const
{
    Sample s;
    if (!map(eDirectionUndistort, x, y, &s)) {
        return false;
    }
    *xu = s.x;
    *yu = s.y;
    return true;
}

bool
Model::jacobian(double x, double y, Direction direction, double J[4]) const
{
    Sample s;
    if (!map(direction, x, y, &s)) {
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        J[i] = s.J[i];
    }
    return true;
}

bool
jacobianFiniteDifference(const Model& model, Direction direction, double x, double y, double h, double J[4])
{
    Sample xn, xp, yn, yp;
    if (!model.map(direction, x - h, y, &xn) || !model.map(direction, x + h, y, &xp) || !model.map(direction, x, y - h, &yn) || !model.map(direction, x, y + h, &yp)) {
        return false;
    }
    J[0] = (xp.x - xn.x) / (2. * h);
    J[1] = (yp.x - yn.x) / (2. * h);
    J[2] = (xp.y - xn.y) / (2. * h);
    J[3] = (yp.y - yn.y) / (2. * h);
    return true;
}

Model
makeNuke(const Frame& frame, const NukeParams& params)
{
    Model m = emptyModel(eModelNuke, eDirectionUndistort);
    const double f = (std::max)(frame.x2 - frame.x1, frame.y2 - frame.y1) / 2.;
    const double xc = (frame.x1 + frame.x2) / 2.;
    const double yc = (frame.y1 + frame.y2) / 2.;
    m.toCore = axisAffine(frame.par / f, -frame.par * xc / f, 1. / f, -yc / f);
    m.fromCore = axisAffine(f / frame.par, xc, f, yc);
    m.c[kNukeK1] = params.k1;
    m.c[kNukeK2] = params.k2;
    m.c[kNukeCenterX] = params.centerX;
    m.c[kNukeCenterY] = params.centerY;
    m.c[kNukeSqueeze] = params.anamorphicSqueeze;
    m.c[kNukeAsymmetricX] = params.asymmetricX;
    m.c[kNukeAsymmetricY] = params.asymmetricY;
    finalizeModel(&m);
    return m;
}

Model
makePanoTools(const Frame& frame, const PanoToolsParams& params)
{
    Model m = emptyModel(eModelPanoTools, eDirectionDistort);
    const double w = frame.x2 - frame.x1;
    const double h = frame.y2 - frame.y1;
    // Radius 1 is the largest circle that fits in the format.
    const double f = (std::min)(w, h) / 2.;
    const double xc = (frame.x1 + frame.x2) / 2.;
    const double yc = (frame.y1 + frame.y2) / 2.;
    const double g = params.g / h;
    const double t = params.t / w;
    m.toCore = axisAffine(frame.par / f, -frame.par * xc / f, 1. / f, -yc / f);
    // Scale back to pixels, shift by (d, -e), then shear around the centre by the shifted point.
    m.fromCore.m00 = f / frame.par;
    m.fromCore.m01 = -g * f;
    m.fromCore.m10 = -t * f / frame.par;
    m.fromCore.m11 = f;
    m.fromCore.tx = xc + params.d + g * params.e;
    m.fromCore.ty = yc - params.e - t * params.d;
    m.c[kPanoToolsA] = params.a;
    m.c[kPanoToolsB] = params.b;
    m.c[kPanoToolsC] = params.c;
    finalizeModel(&m);
    return m;
}

Model
makePFBarrel(const Frame& frame, const PFBarrelParams& params)
{
    Model m = emptyModel(eModelPFBarrel, eDirectionUndistort);
    const double w = frame.x2 - frame.x1;
    const double h = frame.y2 - frame.y1;
    const double norm = std::sqrt(2. / (w * w + h * h));
    const double squeeze = params.squeeze;
    // PFBarrel evaluates at pixel corners, hence the half-pixel shift.
    m.toCore = axisAffine(norm, -(frame.x1 + 0.5) * norm, norm / squeeze, -(frame.y1 + 0.5) * norm / squeeze);
    m.fromCore = axisAffine(1. / norm, frame.x1 + 0.5, squeeze / norm, frame.y1 + 0.5);
    m.c[kPFBarrelC3] = params.c3;
    m.c[kPFBarrelC5] = params.c5;
    m.c[kPFBarrelCenterX] = params.centerX * w * norm;
    m.c[kPFBarrelCenterY] = params.centerY * h * norm / squeeze;
    finalizeModel(&m);
    return m;
}

namespace {
    class PFBarrelReader {
    public:
        explicit PFBarrelReader(const std::string& text)
            : _in(text)
        {
        }

        bool rawLine(std::string* line)
        {
            if (!std::getline(_in, *line)) {
                error = "Parse error";
                return false;
            }
            while (!line->empty() && ((*line)[line->size() - 1] == '\r')) {
                line->erase(line->size() - 1);
            }
            return true;
        }

        bool line(std::string* line)
        {
            do {
                if (!rawLine(line)) {
                    return false;
                }
            } while (line->empty() || ((*line)[0] == '#'));
            return true;
        }

        bool readDouble(double* value)
        {
            std::string l;
            if (!line(&l)) {
                return false;
            }
            const char* begin = l.c_str();
            char* end = 0;
            errno = 0;
            const double v = std::strtod(begin, &end);
            if ((end == begin) || (errno != 0) || !isBlank(end) || !std::isfinite(v)) {
                error = "Not a number: " + l;
                return false;
            }
            *value = v;
            return true;
        }

        bool readInt(int* value)
        {
            std::string l;
            if (!line(&l)) {
                return false;
            }
            const char* begin = l.c_str();
            char* end = 0;
            errno = 0;
            const long v = std::strtol(begin, &end, 10);
            if ((end == begin) || (errno != 0) || !isBlank(end) || (v < -2147483647L) || (v > 2147483647L)) {
                error = "Not an integer: " + l;
                return false;
            }
            *value = (int)v;
            return true;
        }

        // True when only blank and comment lines remain.
        bool atEnd()
        {
            std::string l;
            while (std::getline(_in, l)) {
                while (!l.empty() && (l[l.size() - 1] == '\r')) {
                    l.erase(l.size() - 1);
                }
                if (!isBlank(l.c_str()) && (l[0] != '#')) {
                    error = "Unexpected data after the last key: " + l;
                    return false;
                }
            }
            return true;
        }

        std::string error;

    private:
        static bool isBlank(const char* s)
        {
            for (; *s; ++s) {
                if ((*s != ' ') && (*s != '\t')) {
                    return false;
                }
            }
            return true;
        }

        std::istringstream _in;
    };
} // anon namespace

bool
parsePFBarrelFile(const std::string& text, PFBarrelFile* file, std::string* error)
{
    PFBarrelReader reader(text);
    PFBarrelFile f = PFBarrelFile();
    std::string ln;
    bool ok = reader.rawLine(&ln);
    if (ok) {
        if (ln == "#PFBarrel 2011 v1") {
            f.version = 1;
        } else if (ln == "#PFBarrel 2011 v2") {
            f.version = 2;
        } else {
            reader.error = "Bad header: " + ln;
            ok = false;
        }
    }
    ok = ok && reader.readInt(&f.originalWidth) && reader.readInt(&f.originalHeight);
    f.originalPixelAspect = 1.;
    if (ok && (f.version == 2)) {
        ok = reader.readDouble(&f.originalPixelAspect);
    }
    ok = ok && reader.readInt(&f.undistortedWidth) && reader.readInt(&f.undistortedHeight);
    if (ok && ((f.originalWidth <= 0) || (f.originalHeight <= 0) || (f.originalPixelAspect <= 0.) || (f.undistortedWidth <= 0) || (f.undistortedHeight <= 0))) {
        reader.error = "Bad dimensions";
        ok = false;
    }
    ok = ok && reader.line(&ln);
    if (ok) {
        if (ln == "Low Order") {
            f.highOrder = false;
        } else if (ln == "High Order") {
            f.highOrder = true;
        } else {
            reader.error = "Bad model: " + ln;
            ok = false;
        }
    }
    int nKeys = 0;
    ok = ok && reader.readDouble(&f.squeeze) && reader.readInt(&nKeys);
    if (ok && (f.squeeze <= 0.)) {
        reader.error = "Bad squeeze";
        ok = false;
    }
    if (ok && (nKeys < 1)) {
        reader.error = "Bad key count";
        ok = false;
    }
    for (int i = 0; ok && i < nKeys; ++i) {
        PFBarrelKey key = PFBarrelKey();
        ok = reader.readInt(&key.frame) && reader.readDouble(&key.c3) && reader.readDouble(&key.c5) && reader.readDouble(&key.centerX) && reader.readDouble(&key.centerY);
        if (ok) {
            if (!f.highOrder) {
                key.c5 = 0.;
            }
            f.keys.push_back(key);
        }
    }
    ok = ok && reader.atEnd();
    if (!ok) {
        if (error) {
            *error = reader.error;
        }
        return false;
    }
    *file = f;
    return true;
}
} // namespace LensModels

NATRON_NAMESPACE_EXIT
