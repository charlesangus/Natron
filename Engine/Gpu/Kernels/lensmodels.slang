import resamplecore;

// LensModels::Model::map() (Engine/Nodes/Transform/LensModels.cpp and LensModels3DE.cpp) line for
// line, for kernels that bind a packed model. Values are carried in two floats, as resamplecore.slang
// carries positions, since Vulkan devices need not have doubles: the model's doubles arrive as float
// pairs, and the cores, the Newton inverse and its tolerance are the CPU's, about 2^-44 from its
// double results where the CPU's rounding to float is 2^-24 away. LensModelGpu in
// Engine/Nodes/Transform/LensModelsGpu.h mirrors the words of a packed model.
//
// The only departure is the 3DE fisheye's equisolid-to-rectilinear step, which the CPU writes with
// asin, cos and tan of the field angle and which is evaluated here from the half-angle identities,
// as two-float transcendentals do not exist.

// The packed model: valid, type, core, a reserved word, the affines toCore, fromCore,
// toCoreInverse and fromCoreInverse as m00 m01 m10 m11 tx ty, then the coefficients.
static const uint kLensModelWords = 100;
static const uint kLensAffineBase = 4;
static const uint kLensAffineWords = 12;
static const uint kLensCoeffBase = 52;

static const uint kLensToCore = 0;
static const uint kLensFromCore = 1;
static const uint kLensToCoreInverse = 2;
static const uint kLensFromCoreInverse = 3;

// LensModels::Direction.
static const uint kLensDistort = 0;
static const uint kLensUndistort = 1;

// LensModels::ModelType.
static const uint kLensNuke = 0;
static const uint kLensPFBarrel = 1;
static const uint kLens3DEClassic = 2;
static const uint kLens3DEAnamorphic6 = 3;
static const uint kLens3DEFisheye8 = 4;
static const uint kLens3DEStandard = 5;
static const uint kLens3DEAnamorphic4 = 6;
static const uint kLensPanoTools = 7;

// LensModels' Newton constants.
static const int kLensNewtonMaxIterations = 20;
static const int kLensNewtonMaxHalvings = 10;
static const float kLensNewtonTolerance = 1e-12;
static const float kLensMinDeterminant = 1e-14;

// Coefficient indices, as the CPU's enums name them.
static const uint kNukeK1 = 0;
static const uint kNukeK2 = 1;
static const uint kNukeCenterX = 2;
static const uint kNukeCenterY = 3;
static const uint kNukeSqueeze = 4;
static const uint kNukeAsymmetricX = 5;
static const uint kNukeAsymmetricY = 6;
static const uint kPanoToolsA = 0;
static const uint kPanoToolsB = 1;
static const uint kPanoToolsC = 2;
static const uint kPFBarrelC3 = 0;
static const uint kPFBarrelC5 = 1;
static const uint kPFBarrelCenterX = 2;
static const uint kPFBarrelCenterY = 3;
static const uint kAnamorphicY = 9;
static const uint kFisheyeFocal = 0;
static const uint kFisheyeC2 = 1;
static const uint kFisheyeC4 = 2;
static const uint kFisheyeC6 = 3;
static const uint kFisheyeC8 = 4;
static const uint kStandardC2 = 0;
static const uint kStandardU1 = 1;
static const uint kStandardV1 = 2;
static const uint kStandardC4 = 3;
static const uint kStandardU3 = 4;
static const uint kStandardV3 = 5;

///////////////////////////////////////////////////////////////////////////////
// Two-float helpers resamplecore.slang does not have.

FF lensF(float v)
{
    return ffMake(v, 0.0);
}

FF ffSub(FF a, FF b)
{
    return ffAdd(a, ffNeg(b));
}

// Exact: a power of two.
FF ffTwice(FF a)
{
    return ffMake(a.hi * 2.0, a.lo * 2.0);
}

// The square root of max(a, 0): one Newton step from the device's float root, which need not be
// correctly rounded.
FF ffSqrt(FF a)
{
    if (!(a.hi > 0.0))
        return lensF(0.0);
    precise float s = sqrt(a.hi);
    const FF r = ffSub(a, ffMul(lensF(s), lensF(s)));
    precise float c = (r.hi + r.lo) / (2.0 * s);
    return ffQuick(s, c);
}

bool ffFinite(FF a)
{
    return isFiniteBits(a.hi) && isFiniteBits(a.lo);
}

// A normalized pair is zero exactly when its high word is.
bool ffIsZero(FF a)
{
    return a.hi == 0.0;
}

// The value rounded to float once, as the CPU's (float) of its double.
float ffToFloat(FF a)
{
    precise float v = a.hi + a.lo;
    return v;
}

///////////////////////////////////////////////////////////////////////////////
// The packed model.

struct LensModel
{
    StructuredBuffer<uint> buf;
    uint base;
    bool valid;
    uint type;
    uint core;
};

LensModel lensModelAt(StructuredBuffer<uint> buf, uint base)
{
    LensModel m;
    m.buf = buf;
    m.base = base;
    m.valid = buf[base] != 0;
    m.type = buf[base + 1];
    m.core = buf[base + 2];
    return m;
}

FF lensWord(LensModel m, uint word)
{
    return ffMake(asfloat(m.buf[m.base + word]), asfloat(m.buf[m.base + word + 1]));
}

FF lensC(LensModel m, uint i)
{
    return lensWord(m, kLensCoeffBase + 2 * i);
}

// p' = M p + t, M = [[m00, m01], [m10, m11]].
struct LensAffine
{
    FF m00;
    FF m01;
    FF m10;
    FF m11;
    FF tx;
    FF ty;
};

LensAffine lensAffine(LensModel m, uint index)
{
    const uint w = kLensAffineBase + index * kLensAffineWords;
    LensAffine a;
    a.m00 = lensWord(m, w);
    a.m01 = lensWord(m, w + 2);
    a.m10 = lensWord(m, w + 4);
    a.m11 = lensWord(m, w + 6);
    a.tx = lensWord(m, w + 8);
    a.ty = lensWord(m, w + 10);
    return a;
}

// Jxx = dx'/dx, Jxy = dx'/dy, Jyx = dy'/dx, Jyy = dy'/dy, as LensModels::Sample::J.
struct LensJacobian
{
    FF j0;
    FF j1;
    FF j2;
    FF j3;
};

struct LensCore
{
    bool ok;
    FF gx;
    FF gy;
    LensJacobian J;
};

// A mapped point: the position and the Jacobian of the map there, when ok.
struct LensSample
{
    bool ok;
    FF x;
    FF y;
    LensJacobian J;
};

LensJacobian lensJacobian(FF j0, FF j1, FF j2, FF j3)
{
    LensJacobian J;
    J.j0 = j0;
    J.j1 = j1;
    J.j2 = j2;
    J.j3 = j3;
    return J;
}

LensCore lensCore(bool ok, FF gx, FF gy, LensJacobian J)
{
    LensCore g;
    g.ok = ok;
    g.gx = gx;
    g.gy = gy;
    g.J = J;
    return g;
}

LensCore lensCoreFailed()
{
    const FF zero = lensF(0.0);
    return lensCore(false, zero, zero, lensJacobian(zero, zero, zero, zero));
}

LensSample lensSampleFailed()
{
    const FF zero = lensF(0.0);
    LensSample s;
    s.ok = false;
    s.x = zero;
    s.y = zero;
    s.J = lensJacobian(zero, zero, zero, zero);
    return s;
}

struct FF2
{
    FF x;
    FF y;
};

FF2 lensApplyAffine(LensAffine a, FF x, FF y)
{
    FF2 r;
    r.x = ffAdd(ffAdd(ffMul(a.m00, x), ffMul(a.m01, y)), a.tx);
    r.y = ffAdd(ffAdd(ffMul(a.m10, x), ffMul(a.m11, y)), a.ty);
    return r;
}

// J = from * Jg * to, over the linear parts of the affines.
LensJacobian lensChainJacobian(LensAffine from, LensJacobian Jg, LensAffine to)
{
    const FF a0 = ffAdd(ffMul(Jg.j0, to.m00), ffMul(Jg.j1, to.m10));
    const FF a1 = ffAdd(ffMul(Jg.j0, to.m01), ffMul(Jg.j1, to.m11));
    const FF a2 = ffAdd(ffMul(Jg.j2, to.m00), ffMul(Jg.j3, to.m10));
    const FF a3 = ffAdd(ffMul(Jg.j2, to.m01), ffMul(Jg.j3, to.m11));
    return lensJacobian(ffAdd(ffMul(from.m00, a0), ffMul(from.m01, a2)), ffAdd(ffMul(from.m00, a1), ffMul(from.m01, a3)),
                        ffAdd(ffMul(from.m10, a0), ffMul(from.m11, a2)), ffAdd(ffMul(from.m10, a1), ffMul(from.m11, a3)));
}

FF lensDeterminant(LensJacobian J)
{
    return ffSub(ffMul(J.j0, J.j3), ffMul(J.j1, J.j2));
}

bool lensInvert2x2(LensJacobian J, out LensJacobian inverse)
{
    const FF det = lensDeterminant(J);
    inverse = J;
    if (!(abs(det.hi) > kLensMinDeterminant))
        return false;
    inverse = lensJacobian(ffDiv(J.j3, det), ffNeg(ffDiv(J.j1, det)), ffNeg(ffDiv(J.j2, det)), ffDiv(J.j0, det));
    return true;
}

///////////////////////////////////////////////////////////////////////////////
// The cores.

LensCore coreNuke(LensModel m, FF xd, FF yd)
{
    const FF k1 = lensC(m, kNukeK1);
    const FF k2 = lensC(m, kNukeK2);
    const FF cx = lensC(m, kNukeCenterX);
    const FF cy = lensC(m, kNukeCenterY);
    const FF squeeze = lensC(m, kNukeSqueeze);
    const FF ax = lensC(m, kNukeAsymmetricX);
    const FF ay = lensC(m, kNukeAsymmetricY);
    const FF x = ffSub(xd, cx);
    const FF y = ffSub(yd, cy);
    const FF x2 = ffMul(x, x);
    const FF y2 = ffMul(y, y);
    const FF r2 = ffAdd(x2, y2);
    const FF kr = ffMul(ffAdd(ffMul(k2, r2), k1), r2);
    // d(kr)/dx = x * dkr, d(kr)/dy = y * dkr
    const FF dkr = ffTwice(ffAdd(k1, ffTwice(ffMul(k2, r2))));
    const FF kry = ffAddF(ffAdd(kr, ffMul(ay, x2)), 1.0);
    const FF krx = ffAddF(ffDiv(ffAdd(kr, ffMul(ax, y2)), squeeze), 1.0);
    if (ffIsZero(krx) || ffIsZero(kry))
        return lensCoreFailed();
    const FF gx = ffAdd(ffDiv(x, krx), cx);
    const FF gy = ffAdd(ffDiv(y, kry), cy);
    const FF krx_x = ffDiv(ffMul(x, dkr), squeeze);
    const FF krx_y = ffDiv(ffAdd(ffMul(y, dkr), ffMul(ffTwice(ax), y)), squeeze);
    const FF kry_x = ffAdd(ffMul(x, dkr), ffMul(ffTwice(ay), x));
    const FF kry_y = ffMul(y, dkr);
    const FF krx2 = ffMul(krx, krx);
    const FF kry2 = ffMul(kry, kry);
    return lensCore(true, gx, gy,
                    lensJacobian(ffDiv(ffSub(krx, ffMul(x, krx_x)), krx2), ffDiv(ffNeg(ffMul(x, krx_y)), krx2),
                                 ffDiv(ffNeg(ffMul(y, kry_x)), kry2), ffDiv(ffSub(kry, ffMul(y, kry_y)), kry2)));
}

LensCore corePanoTools(LensModel m, FF x, FF y)
{
    const FF a = lensC(m, kPanoToolsA);
    const FF b = lensC(m, kPanoToolsB);
    const FF cc = lensC(m, kPanoToolsC);
    const FF d = ffSub(lensF(1.0), ffAdd(ffAdd(a, b), cc));
    const FF r2 = ffAdd(ffMul(x, x), ffMul(y, y));
    const FF r = ffSqrt(r2);
    const FF scale = ffAdd(ffAdd(ffMul(ffAdd(ffMul(a, r2), cc), r), ffMul(b, r2)), d);
    const FF zero = lensF(0.0);
    LensJacobian J = lensJacobian(scale, zero, zero, scale);
    if (r.hi > 0.0)
    {
        // (d scale / dr) / r
        const FF ds = ffDiv(ffAdd(ffAdd(ffMulF(ffMul(a, r2), 3.0), ffTwice(ffMul(b, r))), cc), r);
        J.j0 = ffAdd(J.j0, ffMul(ffMul(ds, x), x));
        J.j1 = ffAdd(J.j1, ffMul(ffMul(ds, x), y));
        J.j2 = ffAdd(J.j2, ffMul(ffMul(ds, x), y));
        J.j3 = ffAdd(J.j3, ffMul(ffMul(ds, y), y));
    }
    return lensCore(true, ffMul(x, scale), ffMul(y, scale), J);
}

LensCore corePFBarrel(LensModel m, FF x, FF y)
{
    const FF c3 = lensC(m, kPFBarrelC3);
    const FF c5 = lensC(m, kPFBarrelC5);
    const FF px = ffSub(x, lensC(m, kPFBarrelCenterX));
    const FF py = ffSub(y, lensC(m, kPFBarrelCenterY));
    const FF r2 = ffAdd(ffMul(px, px), ffMul(py, py));
    const FF dr = ffMul(r2, ffAdd(c3, ffMul(r2, c5)));
    const FF dd = ffTwice(ffAdd(c3, ffTwice(ffMul(c5, r2))));
    const FF cross = ffMul(ffMul(dd, px), py);
    const FF base = ffAddF(dr, 1.0);
    return lensCore(true, ffAdd(x, ffMul(px, dr)), ffAdd(y, ffMul(py, dr)),
                    lensJacobian(ffAdd(base, ffMul(ffMul(dd, px), px)), cross, cross, ffAdd(base, ffMul(ffMul(dd, py), py))));
}

// q = 1 + a . (X, Y, X^2, XY, Y^2, X^3, X^2 Y, X Y^2, Y^3) with X = x^2, Y = y^2, and dq/dX, dq/dY,
// the nine terms of a starting at coefficient `first`.
struct LensFactor
{
    FF q;
    FF qX;
    FF qY;
};

LensFactor anamorphicFactor(LensModel m, uint first, FF X, FF Y)
{
    const FF a0 = lensC(m, first);
    const FF a1 = lensC(m, first + 1);
    const FF a2 = lensC(m, first + 2);
    const FF a3 = lensC(m, first + 3);
    const FF a4 = lensC(m, first + 4);
    const FF a5 = lensC(m, first + 5);
    const FF a6 = lensC(m, first + 6);
    const FF a7 = lensC(m, first + 7);
    const FF a8 = lensC(m, first + 8);
    const FF XX = ffMul(X, X);
    const FF XY = ffMul(X, Y);
    const FF YY = ffMul(Y, Y);
    LensFactor f;
    FF q = ffAddF(ffMul(a0, X), 1.0);
    q = ffAdd(q, ffMul(a1, Y));
    q = ffAdd(q, ffMul(a2, XX));
    q = ffAdd(q, ffMul(a3, XY));
    q = ffAdd(q, ffMul(a4, YY));
    q = ffAdd(q, ffMul(a5, ffMul(XX, X)));
    q = ffAdd(q, ffMul(a6, ffMul(XX, Y)));
    q = ffAdd(q, ffMul(a7, ffMul(XY, Y)));
    q = ffAdd(q, ffMul(a8, ffMul(YY, Y)));
    f.q = q;
    FF qX = ffAdd(a0, ffTwice(ffMul(a2, X)));
    qX = ffAdd(qX, ffMul(a3, Y));
    qX = ffAdd(qX, ffMulF(ffMul(a5, XX), 3.0));
    qX = ffAdd(qX, ffTwice(ffMul(a6, XY)));
    qX = ffAdd(qX, ffMul(a7, YY));
    f.qX = qX;
    FF qY = ffAdd(a1, ffMul(a3, X));
    qY = ffAdd(qY, ffTwice(ffMul(a4, Y)));
    qY = ffAdd(qY, ffMul(a6, XX));
    qY = ffAdd(qY, ffTwice(ffMul(a7, XY)));
    qY = ffAdd(qY, ffMulF(ffMul(a8, YY), 3.0));
    f.qY = qY;
    return f;
}

LensCore coreAnamorphic3DE(LensModel m, FF x, FF y)
{
    const FF X = ffMul(x, x);
    const FF Y = ffMul(y, y);
    const LensFactor fx = anamorphicFactor(m, 0, X, Y);
    const FF two_xy = ffTwice(ffMul(x, y));
    const LensFactor fy = anamorphicFactor(m, kAnamorphicY, X, Y);
    return lensCore(true, ffMul(x, fx.q), ffMul(y, fy.q),
                    lensJacobian(ffAdd(fx.q, ffTwice(ffMul(X, fx.qX))), ffMul(two_xy, fx.qY), ffMul(two_xy, fy.qX),
                                 ffAdd(fy.q, ffTwice(ffMul(Y, fy.qY)))));
}

// Equisolid-angle to rectilinear: r = 2 f sin(phi / 2) becomes R = f tan(phi), defined for phi
// below 90 degrees only. With h = sin(phi / 2) = r / (2 f): cos(phi) = 1 - 2 h^2, which is positive
// exactly when phi is below 90 degrees, tan(phi) = 2 h cos(phi / 2) / cos(phi), so the CPU's
// s = R / r is cos(phi / 2) / cos(phi), and its dR/dr = 1 / (cos(phi)^2 cos(phi / 2)).
LensCore coreFisheye3DE(LensModel m, FF x, FF y)
{
    const FF f = lensC(m, kFisheyeFocal);
    if (!(f.hi > 0.0))
        return lensCoreFailed();
    FF px = x;
    FF py = y;
    const FF zero = lensF(0.0);
    LensJacobian Je = lensJacobian(lensF(1.0), zero, zero, lensF(1.0));
    const FF r2 = ffAdd(ffMul(x, x), ffMul(y, y));
    if (r2.hi > 0.0)
    {
        const FF h2 = ffDiv(r2, ffMulF(ffMul(f, f), 4.0));
        if (!(h2.hi < 1.0))
            return lensCoreFailed();
        const FF cosPhi = ffSub(lensF(1.0), ffTwice(h2));
        if (!(cosPhi.hi > 0.0))
            return lensCoreFailed();
        const FF cosHalf = ffSqrt(ffSub(lensF(1.0), h2));
        const FF s = ffDiv(cosHalf, cosPhi);
        const FF dRdr = ffDiv(lensF(1.0), ffMul(ffMul(cosPhi, cosPhi), cosHalf));
        const FF k = ffDiv(ffSub(dRdr, s), r2);
        px = ffMul(x, s);
        py = ffMul(y, s);
        const FF kxy = ffMul(ffMul(k, x), y);
        Je = lensJacobian(ffAdd(s, ffMul(ffMul(k, x), x)), kxy, kxy, ffAdd(s, ffMul(ffMul(k, y), y)));
    }
    const FF C2 = lensC(m, kFisheyeC2);
    const FF C4 = lensC(m, kFisheyeC4);
    const FF C6 = lensC(m, kFisheyeC6);
    const FF C8 = lensC(m, kFisheyeC8);
    const FF R2 = ffAdd(ffMul(px, px), ffMul(py, py));
    const FF q = ffAddF(ffMul(R2, ffAdd(C2, ffMul(R2, ffAdd(C4, ffMul(R2, ffAdd(C6, ffMul(R2, C8))))))), 1.0);
    const FF dq = ffAdd(C2, ffMul(R2, ffAdd(ffTwice(C4), ffMul(R2, ffAdd(ffMulF(C6, 3.0), ffMulF(ffMul(R2, C8), 4.0))))));
    const FF Jr0 = ffAdd(q, ffMul(ffMul(ffTwice(dq), px), px));
    const FF Jr1 = ffMul(ffMul(ffTwice(dq), px), py);
    const FF Jr3 = ffAdd(q, ffMul(ffMul(ffTwice(dq), py), py));
    return lensCore(true, ffMul(px, q), ffMul(py, q),
                    lensJacobian(ffAdd(ffMul(Jr0, Je.j0), ffMul(Jr1, Je.j2)), ffAdd(ffMul(Jr0, Je.j1), ffMul(Jr1, Je.j3)),
                                 ffAdd(ffMul(Jr1, Je.j0), ffMul(Jr3, Je.j2)), ffAdd(ffMul(Jr1, Je.j1), ffMul(Jr3, Je.j3))));
}

LensCore coreStandard3DE(LensModel m, FF x, FF y)
{
    const FF C2 = lensC(m, kStandardC2);
    const FF C4 = lensC(m, kStandardC4);
    const FF u3 = lensC(m, kStandardU3);
    const FF v3 = lensC(m, kStandardV3);
    const FF x2 = ffMul(x, x);
    const FF y2 = ffMul(y, y);
    const FF xy = ffMul(x, y);
    const FF r2 = ffAdd(x2, y2);
    const FF K = ffAdd(ffAddF(ffMul(C2, r2), 1.0), ffMul(ffMul(C4, r2), r2));
    const FF Kr = ffAdd(C2, ffTwice(ffMul(C4, r2)));
    const FF U = ffAdd(lensC(m, kStandardU1), ffMul(u3, r2));
    const FF V = ffAdd(lensC(m, kStandardV1), ffMul(v3, r2));
    const FF rx = ffAdd(r2, ffTwice(x2));
    const FF ry = ffAdd(r2, ffTwice(y2));
    const FF twoXY = ffTwice(xy);
    const FF gx = ffAdd(ffAdd(ffMul(x, K), ffMul(rx, U)), ffMul(twoXY, V));
    const FF gy = ffAdd(ffAdd(ffMul(y, K), ffMul(ry, V)), ffMul(twoXY, U));
    // 2 x2 y and 2 x y2, doubled again where the CPU has 4 x2 y and 4 x y2.
    const FF twoX2Y = ffTwice(ffMul(x2, y));
    const FF twoXY2 = ffTwice(ffMul(x, y2));
    FF J0 = ffAdd(K, ffMul(ffTwice(x2), Kr));
    J0 = ffAdd(J0, ffMul(ffMulF(x, 6.0), U));
    J0 = ffAdd(J0, ffMul(ffMul(ffTwice(x), u3), rx));
    J0 = ffAdd(J0, ffMul(ffTwice(y), V));
    J0 = ffAdd(J0, ffMul(ffTwice(twoX2Y), v3));
    FF J1 = ffMul(twoXY, Kr);
    J1 = ffAdd(J1, ffMul(ffTwice(y), U));
    J1 = ffAdd(J1, ffMul(ffMul(ffTwice(y), u3), rx));
    J1 = ffAdd(J1, ffMul(ffTwice(x), V));
    J1 = ffAdd(J1, ffMul(ffTwice(twoXY2), v3));
    FF J2 = ffMul(twoXY, Kr);
    J2 = ffAdd(J2, ffMul(ffTwice(x), V));
    J2 = ffAdd(J2, ffMul(ffMul(ffTwice(x), v3), ry));
    J2 = ffAdd(J2, ffMul(ffTwice(y), U));
    J2 = ffAdd(J2, ffMul(ffTwice(twoX2Y), u3));
    FF J3 = ffAdd(K, ffMul(ffTwice(y2), Kr));
    J3 = ffAdd(J3, ffMul(ffMulF(y, 6.0), V));
    J3 = ffAdd(J3, ffMul(ffMul(ffTwice(y), v3), ry));
    J3 = ffAdd(J3, ffMul(ffTwice(x), U));
    J3 = ffAdd(J3, ffMul(ffTwice(twoXY2), u3));
    return lensCore(true, gx, gy, lensJacobian(J0, J1, J2, J3));
}

LensCore lensEvaluateCore(LensModel m, FF x, FF y)
{
    LensCore g = lensCoreFailed();
    switch (m.type)
    {
    case kLensNuke:
        g = coreNuke(m, x, y);
        break;
    case kLensPFBarrel:
        g = corePFBarrel(m, x, y);
        break;
    case kLens3DEClassic:
    case kLens3DEAnamorphic6:
    case kLens3DEAnamorphic4:
        g = coreAnamorphic3DE(m, x, y);
        break;
    case kLens3DEFisheye8:
        g = coreFisheye3DE(m, x, y);
        break;
    case kLens3DEStandard:
        g = coreStandard3DE(m, x, y);
        break;
    case kLensPanoTools:
        g = corePanoTools(m, x, y);
        break;
    default:
        break;
    }
    g.ok = g.ok && ffFinite(g.gx) && ffFinite(g.gy) && ffFinite(g.J.j0) && ffFinite(g.J.j1) && ffFinite(g.J.j2) && ffFinite(g.J.j3);
    return g;
}

///////////////////////////////////////////////////////////////////////////////
// Model::map().

// The core at (tx, ty) when `solve` is false. When it is true, the CPU's solveCore(): core(u) = t
// solved for u, starting from u = t since every core is close to the identity, the result's
// (gx, gy) being u and its J the core's Jacobian there. Every evaluation of the core, the start's
// halvings towards the lens centre and the Newton line search alike, is the one call in the loop
// below, as the kernel would otherwise carry the cores inlined once per call; the sequence of
// points evaluated and the stopping rules are the CPU's.
LensCore lensEvaluateOrSolve(LensModel m, FF tx, FF ty, bool solve)
{
    // The point evaluated next.
    FF cx = tx;
    FF cy = ty;
    bool started = false;
    int halvings = 0;
    // The iterate, its residual and the core's Jacobian there, once started.
    FF x = tx;
    FF y = ty;
    FF rx = lensF(0.0);
    FF ry = lensF(0.0);
    FF r2 = lensF(0.0);
    LensJacobian Jg = lensCoreFailed().J;
    FF tol2 = lensF(0.0);
    int iter = 0;
    // The Newton step from the iterate and the line search along it.
    FF dx = lensF(0.0);
    FF dy = lensF(0.0);
    float lambda = 1.0;
    int h = 0;
    const int kEvaluations = 1 + kLensNewtonMaxHalvings + kLensNewtonMaxIterations * (kLensNewtonMaxHalvings + 1);
    for (int evaluation = 0; evaluation < kEvaluations; ++evaluation)
    {
        const LensCore n = lensEvaluateCore(m, cx, cy);
        if (!started)
        {
            if (!n.ok)
            {
                // Pull a start outside the domain towards the lens centre, where every model is
                // defined; a point evaluated forward is simply outside it.
                if (!solve || halvings == kLensNewtonMaxHalvings)
                    return lensCoreFailed();
                ++halvings;
                cx = ffHalf(cx);
                cy = ffHalf(cy);
                continue;
            }
            if (!solve)
                return n;
            started = true;
            x = cx;
            y = cy;
            Jg = n.J;
            rx = ffSub(n.gx, tx);
            ry = ffSub(n.gy, ty);
            r2 = ffAdd(ffMul(rx, rx), ffMul(ry, ry));
            // The tolerance only decides when to stop, so float is enough for it.
            precise float tol = kLensNewtonTolerance * (1.0 + sqrt(tx.hi * tx.hi + ty.hi * ty.hi));
            tol2 = lensF(tol * tol);
        }
        else
        {
            bool accepted = false;
            if (n.ok)
            {
                const FF nrx = ffSub(n.gx, tx);
                const FF nry = ffSub(n.gy, ty);
                const FF nr2 = ffAdd(ffMul(nrx, nrx), ffMul(nry, nry));
                if (ffLess(nr2, r2))
                {
                    x = cx;
                    y = cy;
                    rx = nrx;
                    ry = nry;
                    r2 = nr2;
                    Jg = n.J;
                    accepted = true;
                }
            }
            if (!accepted)
            {
                // A step that does not reduce the residual is halved at most
                // kLensNewtonMaxHalvings times.
                lambda *= 0.5;
                ++h;
                if (h > kLensNewtonMaxHalvings)
                    return lensCoreFailed();
                // lambda is a power of two, so the scaled step is exact.
                cx = ffAdd(x, ffMake(dx.hi * lambda, dx.lo * lambda));
                cy = ffAdd(y, ffMake(dy.hi * lambda, dy.lo * lambda));
                continue;
            }
        }
        if (!ffLess(tol2, r2))
            return lensCore(true, x, y, Jg);
        if (iter == kLensNewtonMaxIterations)
            return lensCoreFailed();
        ++iter;
        const FF det = lensDeterminant(Jg);
        if (!(abs(det.hi) > kLensMinDeterminant))
            return lensCoreFailed();
        dx = ffDiv(ffSub(ffMul(Jg.j1, ry), ffMul(Jg.j3, rx)), det);
        dy = ffDiv(ffSub(ffMul(Jg.j2, rx), ffMul(Jg.j0, ry)), det);
        lambda = 1.0;
        h = 0;
        cx = ffAdd(x, dx);
        cy = ffAdd(y, dy);
    }
    return lensCoreFailed();
}

// LensModels::Model::map(): the point (x, y) mapped in `direction`, in pixels, with the Jacobian
// of the map there; not ok outside the model's domain or when Newton does not converge.
LensSample lensMap(LensModel m, uint direction, FF x, FF y)
{
    LensSample s = lensSampleFailed();
    if (!m.valid)
        return s;
    const LensAffine toCore = lensAffine(m, kLensToCore);
    const LensAffine fromCore = lensAffine(m, kLensFromCore);
    const bool forward = direction == m.core;
    const FF2 t = lensApplyAffine(lensAffine(m, forward ? kLensToCore : kLensFromCoreInverse), x, y);
    const LensCore g = lensEvaluateOrSolve(m, t.x, t.y, !forward);
    if (!g.ok)
        return s;
    const LensJacobian Jf = lensChainJacobian(fromCore, g.J, toCore);
    LensJacobian J = Jf;
    if (!forward && !lensInvert2x2(Jf, J))
        return s;
    // Forward, (gx, gy) is the core's value, taken back to pixels; inverse, it is the solution u.
    const FF2 p = lensApplyAffine(lensAffine(m, forward ? kLensFromCore : kLensToCoreInverse), g.gx, g.gy);
    s.ok = true;
    s.x = p.x;
    s.y = p.y;
    s.J = J;
    return s;
}
