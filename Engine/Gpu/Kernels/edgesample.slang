// How every GPU path reads a source outside its image, by the source's edge mode: the twin of
// EdgeSample in Engine/Nodes/Image/EdgeSample.h, so that both sides pick the same pixel or zero.

// EdgeMode in Global/EdgeMode.h.
static const uint kEdgeStretch = 0;
static const uint kEdgeBlack = 1;

// The bit of a flags word that carries Black, in the parameter blocks that share it:
// resample.slang's, and stmap.slang's for STMap and IDistort.
static const uint kFlagEdgeBlack = 2;

// The edge mode a flags word carries, Black where `blackBit` is set.
uint edgeModeFromFlags(uint flags, uint blackBit)
{
    return (flags & blackBit) != 0 ? kEdgeBlack : kEdgeStretch;
}

// EdgeSample::coord: where a read at v along an axis of [lo, hiExclusive) lands, in `sampled`: v
// itself inside, the nearest end of the axis outside for Stretch. False when the read finds no
// pixel and is zero: outside for Black, anywhere on an empty axis.
bool edgeCoord(int v, int lo, int hiExclusive, uint mode, out int sampled)
{
    sampled = lo;
    if (hiExclusive <= lo)
        return false;
    if (v < lo)
    {
        if (mode == kEdgeBlack)
            return false;
        v = lo;
    }
    else if (v >= hiExclusive)
    {
        if (mode == kEdgeBlack)
            return false;
        v = hiExclusive - 1;
    }
    sampled = v;
    return true;
}

// EdgeSample::pixel: edgeCoord on both axes of the bounds (x1, y1, x2, y2).
bool edgePixel(int2 p, int4 bounds, uint mode, out int2 sampled)
{
    int sx;
    int sy;
    const bool hitX = edgeCoord(p.x, bounds.x, bounds.z, mode, sx);
    const bool hitY = edgeCoord(p.y, bounds.y, bounds.w, mode, sy);
    sampled = int2(sx, sy);
    return hitX && hitY;
}
