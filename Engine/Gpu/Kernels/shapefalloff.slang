// The shape generators' falloff curves, shape distances and antialiasing on the GPU: a line-for-line
// mirror of Engine/Nodes/Generator/ShapeFalloff.h. Keep the two in step, including the order of
// the floating-point operations; kernels importing this compile with -fp-mode precise so nothing
// is contracted.
//
// A module: the shape kernels and the test entry (shapefalloff_test.slang) import it, and it
// declares no bindings of its own.

static const int kFalloffLinear = 0;
static const int kFalloffSmooth = 1;
static const int kFalloffEaseIn = 2;
static const int kFalloffEaseOut = 3;
static const int kFalloffQuadratic = 4;
static const int kFalloffCount = 5;

static const float kFarOutside = -1e30f;

struct ShapeBox
{
    float x1;
    float y1;
    float x2;
    float y2;
}

struct ShapeEdge
{
    float distance;
    float nx;
    float ny;
}

float shapeClamp01(float t)
{
    return t <= 0.f ? 0.f : (t >= 1.f ? 1.f : t);
}

float shapeEase(float t, int falloff)
{
    t = shapeClamp01(t);
    if (falloff == kFalloffSmooth)
    {
        return t * t * (3.f - 2.f * t);
    }
    if (falloff == kFalloffEaseIn)
    {
        return t * t * (2.f - t);
    }
    if (falloff == kFalloffEaseOut)
    {
        return t * (1.f + t * (1.f - t));
    }
    if (falloff == kFalloffQuadratic)
    {
        if (t < 0.5f)
        {
            return 2.f * t * t;
        }
        const float u = 1.f - t;

        return 1.f - 2.f * u * u;
    }

    return t;
}

float rampT(float px, float py, float p0x, float p0y, float p1x, float p1y)
{
    const float dx = p1x - p0x;
    const float dy = p1y - p0y;
    const float len2 = dx * dx + dy * dy;
    if (!(len2 > 0.f))
    {
        return 1.f;
    }

    return shapeClamp01(((px - p0x) * dx + (py - p0y) * dy) / len2);
}

ShapeEdge ellipseEdge(float px, float py, ShapeBox box)
{
    ShapeEdge e;
    const float rx = 0.5f * (box.x2 - box.x1);
    const float ry = 0.5f * (box.y2 - box.y1);
    if (!(rx > 0.f) || !(ry > 0.f))
    {
        e.distance = kFarOutside;
        e.nx = 1.f;
        e.ny = 0.f;

        return e;
    }
    const float qx = (px - (box.x1 + rx)) / rx;
    const float qy = (py - (box.y1 + ry)) / ry;
    const float gx = qx / rx;
    const float gy = qy / ry;
    const float g = sqrt(gx * gx + gy * gy);
    if (!(g > 0.f))
    {
        e.distance = rx < ry ? rx : ry;
        e.nx = 1.f;
        e.ny = 0.f;

        return e;
    }
    const float r = sqrt(qx * qx + qy * qy);
    e.distance = (1.f - r) * r / g;
    e.nx = gx / g;
    e.ny = gy / g;

    return e;
}

ShapeEdge roundedBoxEdge(float px, float py, ShapeBox box, float cornerX, float cornerY)
{
    ShapeEdge e;
    const float hx = 0.5f * (box.x2 - box.x1);
    const float hy = 0.5f * (box.y2 - box.y1);
    if (!(hx > 0.f) || !(hy > 0.f))
    {
        e.distance = kFarOutside;
        e.nx = 1.f;
        e.ny = 0.f;

        return e;
    }
    const float halfSide = hx < hy ? hx : hy;
    float rx = cornerX > 0.f ? cornerX : 0.f;
    float ry = cornerY > 0.f ? cornerY : 0.f;
    rx = rx < halfSide ? rx : halfSide;
    ry = ry < halfSide ? ry : halfSide;
    const float cx = px - (box.x1 + hx);
    const float cy = py - (box.y1 + hy);
    const float sx = cx < 0.f ? -1.f : 1.f;
    const float sy = cy < 0.f ? -1.f : 1.f;
    const float ax = cx * sx;
    const float ay = cy * sy;
    const float ox = ax - (hx - rx);
    const float oy = ay - (hy - ry);
    if ((rx > 0.f) && (ry > 0.f) && (ox > 0.f) && (oy > 0.f))
    {
        const float qx = ox / rx;
        const float qy = oy / ry;
        const float gx = qx / rx;
        const float gy = qy / ry;
        const float g = sqrt(gx * gx + gy * gy);
        const float r = sqrt(qx * qx + qy * qy);
        e.distance = (1.f - r) * r / g;
        e.nx = sx * gx / g;
        e.ny = sy * gy / g;

        return e;
    }
    const float dx = hx - ax;
    const float dy = hy - ay;
    if (dx <= dy)
    {
        e.distance = dx;
        e.nx = sx;
        e.ny = 0.f;
    }
    else
    {
        e.distance = dy;
        e.nx = 0.f;
        e.ny = sy;
    }

    return e;
}

// (a, b), a >= b.
float2 footprintExtents(ShapeEdge e, float pixelW, float pixelH)
{
    const float u = abs(e.nx) * pixelW;
    const float v = abs(e.ny) * pixelH;
    return float2(u >= v ? u : v, u >= v ? v : u);
}

float pixelCoverage(ShapeEdge e, float pixelW, float pixelH)
{
    const float2 ab = footprintExtents(e, pixelW, pixelH);
    const float a = ab.x;
    const float b = ab.y;
    const float s = e.distance + 0.5f * (a + b);
    if (s <= 0.f)
    {
        return 0.f;
    }
    if (s >= a + b)
    {
        return 1.f;
    }
    if (s < b)
    {
        return s * s / (2.f * a * b);
    }
    if (s <= a)
    {
        return (s - 0.5f * b) / a;
    }
    const float u = a + b - s;

    return 1.f - u * u / (2.f * a * b);
}

float coverageIntegralBelowZero(float z, float a, float b)
{
    const float s = z + 0.5f * (a + b);
    if (s <= 0.f)
    {
        return 0.f;
    }
    if (s < b)
    {
        return s * s * s / (6.f * a * b);
    }
    const float m = s - 0.5f * b;

    return b * b / (6.f * a) + (m * m - 0.25f * b * b) / (2.f * a);
}

float coverageIntegral(float z, float a, float b)
{
    if (z <= 0.f)
    {
        return coverageIntegralBelowZero(z, a, b);
    }

    return z + coverageIntegralBelowZero(-z, a, b);
}

float bandCoverage(ShapeEdge e, float softness, float pixelW, float pixelH)
{
    if (!(softness > 0.f))
    {
        return pixelCoverage(e, pixelW, pixelH);
    }
    const float2 ab = footprintExtents(e, pixelW, pixelH);
    const float a = ab.x;
    const float b = ab.y;
    const float h = 0.5f * (a + b);
    if (e.distance <= -h)
    {
        return 0.f;
    }
    const float inner = e.distance - softness;
    if (inner >= h)
    {
        return 1.f;
    }

    return shapeClamp01((coverageIntegral(e.distance, a, b) - coverageIntegral(inner, a, b)) / softness);
}

float shapeWeight(ShapeEdge e, float softness, int falloff, float pixelW, float pixelH)
{
    return shapeEase(bandCoverage(e, softness, pixelW, pixelH), falloff);
}

float ellipseT(float px, float py, ShapeBox box, float softness, float pixelW, float pixelH)
{
    return bandCoverage(ellipseEdge(px, py, box), softness, pixelW, pixelH);
}

float roundedBoxT(float px, float py, ShapeBox box, float cornerX, float cornerY, float softness, float pixelW, float pixelH)
{
    return bandCoverage(roundedBoxEdge(px, py, box, cornerX, cornerY), softness, pixelW, pixelH);
}
