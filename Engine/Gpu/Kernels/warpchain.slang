import resamplecore;
import lensmodels;

// InverseWarpChain::mapRow per point. A chain buffer holds the entry count, a tag and a word offset
// per entry, then the entries' blocks, as InverseWarpChain::packGpu() writes them; each block's
// offsets are relative to its first word. Points are pixel coordinates at the chain's scale as
// two-float pairs, and J is (Jxx, Jxy, Jyx, Jyy), each entry making it D J with D the entry's
// derivative at the point.

static const uint kWarpChainMatrix = 1;
static const uint kWarpChainSplineWarp = 2;
static const uint kWarpChainLens = 3;

// The SplineWarp block's flags and words, as SplineWarpInverseMap::packGpu() writes them.
static const uint kWarpChainFieldOn = 1;
static const uint kWarpChainCrop = 2;
static const uint kWarpChainEdgeBlack = 4;
// Where the output window's crop flags sit; its rect follows them.
static const uint kWarpChainOutputWindow = 22;
// Words per grid point of the field's node table.
static const uint kWarpChainNodeWords = 8;

// The lens block's words, as LensInverseWarpMap::packGpu() writes them; the crop flags are the
// SplineWarp block's.
static const uint kWarpChainLensModel = 12;

FF warpChainFF(StructuredBuffer<uint> chain, uint index)
{
    return ffMake(asfloat(chain[index]), asfloat(chain[index + 1]));
}

FF warpChainInt(uint word)
{
    return ffMake(float(int(word)), 0.0);
}

// floor() of a pair as an integer; the low word decides when the high one is a whole number.
int ffFloorInt(FF a)
{
    const float f = floor(a.hi);
    if (f != a.hi)
        return int(f);
    return int(f) + int(floor(a.lo));
}

// (1 - f) a + f b, as SplineWarpField::mapPoints() computes it in doubles.
FF warpChainLerp(FF a, FF b, FF f)
{
    return ffAdd(ffMul(a, ffAddF(ffNeg(f), 1.0)), ffMul(b, f));
}

float4 warpChainCompose(float4 D, float4 J)
{
    return float4(D.x * J.x + D.y * J.z, D.x * J.y + D.y * J.w, D.z * J.x + D.w * J.z, D.z * J.y + D.w * J.w);
}

// The format crop of SplineWarp's and the lens's maps: a point outside the rect has no source
// under a black edge or a null rect, and is otherwise clamped half a pixel inside it.
bool warpChainCrop(StructuredBuffer<uint> chain, uint base, uint flags, inout FF x, inout FF y)
{
    if ((flags & kWarpChainCrop) == 0)
        return true;
    const FF x1 = warpChainInt(chain[base]);
    const FF y1 = warpChainInt(chain[base + 1]);
    const FF x2 = warpChainInt(chain[base + 2]);
    const FF y2 = warpChainInt(chain[base + 3]);
    if (!ffLess(x, x1) && !ffLess(x2, x) && !ffLess(y, y1) && !ffLess(y2, y))
        return true;
    const bool empty = !ffLess(x1, x2) || !ffLess(y1, y2);
    if ((flags & kWarpChainEdgeBlack) != 0 || empty)
        return false;
    x = ffMin(ffMax(x, ffAddF(x1, 0.5)), ffAddF(x2, -0.5));
    y = ffMin(ffMax(y, ffAddF(y1, 0.5)), ffAddF(y2, -0.5));
    return true;
}

// MatrixInverseWarpMap: the inverse's rows, with the projective divide; no source where w <= 0.
bool warpChainMatrix(StructuredBuffer<uint> chain, uint base, inout FF x, inout FF y, inout float4 J, bool jac)
{
    FF m[9];
    for (uint k = 0; k < 9; ++k)
        m[k] = warpChainFF(chain, base + 2 * k);
    const FF w = ffAdd(ffAdd(ffMul(m[6], x), ffMul(m[7], y)), m[8]);
    if (!(w.hi > 0.0))
        return false;
    const FF qx = ffDiv(ffAdd(ffAdd(ffMul(m[0], x), ffMul(m[1], y)), m[2]), w);
    const FF qy = ffDiv(ffAdd(ffAdd(ffMul(m[3], x), ffMul(m[4], y)), m[5]), w);
    x = qx;
    y = qy;
    if (jac)
    {
        const float d00 = ffToFloat(ffDiv(ffSub(m[0], ffMul(qx, m[6])), w));
        const float d01 = ffToFloat(ffDiv(ffSub(m[1], ffMul(qx, m[7])), w));
        const float d10 = ffToFloat(ffDiv(ffSub(m[3], ffMul(qy, m[6])), w));
        const float d11 = ffToFloat(ffDiv(ffSub(m[4], ffMul(qy, m[7])), w));
        J = warpChainCompose(float4(d00, d01, d10, d11), J);
    }
    return true;
}

// Whether the window pixel containing (x, y) has its feather bit set, as
// SplineWarpField::containingPixelInsideFeathers() decides; true without feather tables.
bool warpChainInsideFeathers(StructuredBuffer<uint> chain, uint base, FF x, FF y)
{
    const uint feathers = chain[base + 16];
    if (feathers == 0)
        return true;
    const int col = ffFloorInt(x) - int(chain[base + 18]);
    const int row = ffFloorInt(y) - int(chain[base + 19]);
    if (col < 0 || row < 0 || col >= int(chain[base + 20]) || row >= int(chain[base + 21]))
        return false;
    const uint word = chain[base + feathers + uint(row) * chain[base + 17] + uint(col) / 32];
    return ((word >> (uint(col) % 32)) & 1) != 0;
}

// SplineWarpInverseMap: the output window's crop, the field's scattered lookup, then the format
// crop. Block words: flags, the crop rect, the node table's offset, the grid's size nx and ny,
// its first lattice index i0 and j0, the lattice step and the canonical-to-pixel scales as pairs,
// the feather table's offset (0 without boundaries), its row stride and the window it covers as
// x1, y1, width and height, then the output window's crop flags and rect.
bool warpChainSplineWarp(StructuredBuffer<uint> chain, uint base, inout FF x, inout FF y, inout float4 J, bool jac)
{
    if (!warpChainCrop(chain, base + kWarpChainOutputWindow + 1, chain[base + kWarpChainOutputWindow], x, y))
        return false;
    const uint flags = chain[base];
    if ((flags & kWarpChainFieldOn) != 0 && warpChainInsideFeathers(chain, base, x, y))
    {
        const uint nodes = base + chain[base + 5];
        const int nx = int(chain[base + 6]);
        const int ny = int(chain[base + 7]);
        const int i0 = int(chain[base + 8]);
        const int j0 = int(chain[base + 9]);
        const FF step = warpChainFF(chain, base + 10);
        const FF scaleX = warpChainFF(chain, base + 12);
        const FF scaleY = warpChainFF(chain, base + 14);
        const FF gx = ffSub(ffDiv(ffDiv(x, scaleX), step), warpChainInt(uint(i0)));
        const FF gy = ffSub(ffDiv(ffDiv(y, scaleY), step), warpChainInt(uint(j0)));
        const int i = clamp(ffFloorInt(gx), 0, max(nx - 2, 0));
        const int j = clamp(ffFloorInt(gy), 0, max(ny - 2, 0));
        const FF zero = ffMake(0.0, 0.0);
        const FF one = ffMake(1.0, 0.0);
        const FF fx = ffMax(zero, ffMin(one, ffSub(gx, warpChainInt(uint(i)))));
        const FF fy = ffMax(zero, ffMin(one, ffSub(gy, warpChainInt(uint(j)))));
        const uint a = nodes + uint(j * nx + i) * kWarpChainNodeWords;
        const uint b = a + kWarpChainNodeWords;
        const uint c = a + uint(nx) * kWarpChainNodeWords;
        const uint d = c + kWarpChainNodeWords;
        x = warpChainLerp(warpChainLerp(warpChainFF(chain, a), warpChainFF(chain, b), fx),
                          warpChainLerp(warpChainFF(chain, c), warpChainFF(chain, d), fx), fy);
        y = warpChainLerp(warpChainLerp(warpChainFF(chain, a + 2), warpChainFF(chain, b + 2), fx),
                          warpChainLerp(warpChainFF(chain, c + 2), warpChainFF(chain, d + 2), fx), fy);
        if (jac)
        {
            const float u = ffToFloat(fx);
            const float v = ffToFloat(fy);
            float4 D;
            for (uint k = 0; k < 4; ++k)
            {
                const float ja = asfloat(chain[a + 4 + k]);
                const float jb = asfloat(chain[b + 4 + k]);
                const float jc = asfloat(chain[c + 4 + k]);
                const float jd = asfloat(chain[d + 4 + k]);
                D[k] = (1.0 - v) * ((1.0 - u) * ja + u * jb) + v * ((1.0 - u) * jc + u * jd);
            }
            J = warpChainCompose(D, J);
        }
    }
    return warpChainCrop(chain, base + 1, flags, x, y);
}

// LensInverseWarpMap: the crop, then the model between the render scale's pixels and its own.
// Block words: flags, the crop rect, the direction, then the scales and the lens-to-Source x
// factor as pairs, then the packed model.
bool warpChainLens(StructuredBuffer<uint> chain, uint base, inout FF x, inout FF y, inout float4 J, bool jac)
{
    const uint flags = chain[base];
    if (!warpChainCrop(chain, base + 1, flags, x, y))
        return false;
    const uint direction = chain[base + 5];
    const FF scaleX = warpChainFF(chain, base + 6);
    const FF scaleY = warpChainFF(chain, base + 8);
    const FF toSourceX = warpChainFF(chain, base + 10);
    const LensSample s = lensMap(lensModelAt(chain, base + kWarpChainLensModel), direction, ffDiv(x, scaleX), ffDiv(y, scaleY));
    if (!s.ok || !ffFinite(s.x) || !ffFinite(s.y))
        return false;
    x = ffMul(ffMul(s.x, toSourceX), scaleX);
    y = ffMul(s.y, scaleY);
    if (jac)
    {
        const float d0 = ffToFloat(ffMul(s.J.j0, toSourceX));
        const float d1 = ffToFloat(ffDiv(ffMul(ffMul(s.J.j1, toSourceX), scaleX), scaleY));
        const float d2 = ffToFloat(ffDiv(ffMul(s.J.j2, scaleY), scaleX));
        const float d3 = ffToFloat(s.J.j3);
        J = warpChainCompose(float4(d0, d1, d2, d3), J);
    }
    return true;
}

// The chain at word base of chain, downstream to upstream. False, the point having no source,
// as soon as an entry gives none; x, y and J are then left as that entry found them.
bool warpChainMap(StructuredBuffer<uint> chain, uint base, inout FF x, inout FF y, inout float4 J, bool jac)
{
    if (!ffFinite(x) || !ffFinite(y))
        return false;
    const uint count = chain[base];
    for (uint i = 0; i < count; ++i)
    {
        const uint tag = chain[base + 1 + 2 * i];
        const uint block = base + chain[base + 2 + 2 * i];
        bool ok = false;
        if (tag == kWarpChainMatrix)
            ok = warpChainMatrix(chain, block, x, y, J, jac);
        else if (tag == kWarpChainSplineWarp)
            ok = warpChainSplineWarp(chain, block, x, y, J, jac);
        else if (tag == kWarpChainLens)
            ok = warpChainLens(chain, block, x, y, J, jac);
        if (!ok)
            return false;
    }
    return true;
}
