import pixelwrap;
import shapefalloff;

// The shape generators per pixel: rampPixel (Engine/Nodes/Generator/Ramp.h), radialPixel
// (Radial.h), rectanglePixel (Rectangle.h) and colorWheelPixel (ColorWheel.h), with the same
// operations in the same order. Each op loads its node's pixel-params struct word by word from
// wrapAux; the word offsets below follow those structs' field order.

float shapePixelCoord(int i, float pixelSize)
{
    return (float(i) + 0.5f) * pixelSize;
}

float4 shapeLerpColor(float4 color0, float4 color1, float w)
{
    const float v = 1.f - w;
    return v * color0 + w * color1;
}

// RampPixelParams: 16 words.
struct RampOp : IPixelOp
{
    float p0x;
    float p0y;
    float p1x;
    float p1y;
    float pixelW;
    float pixelH;
    int falloff;
    float4 color0;
    float4 color1;

    [mutating] void load(StructuredBuffer<uint> params)
    {
        p0x = asfloat(params[0]);
        p0y = asfloat(params[1]);
        p1x = asfloat(params[2]);
        p1y = asfloat(params[3]);
        pixelW = asfloat(params[4]);
        pixelH = asfloat(params[5]);
        falloff = int(params[6]);
        // Word 7 is reserved.
        color0 = wrapParamFloat4(params, 8);
        color1 = wrapParamFloat4(params, 12);
    }

    float4 apply(float4 v, int2 xy)
    {
        const float px = shapePixelCoord(xy.x, pixelW);
        const float py = shapePixelCoord(xy.y, pixelH);
        const float t = rampT(px, py, p0x, p0y, p1x, p1y);
        return shapeLerpColor(color0, color1, shapeEase(t, falloff));
    }
}

// ShapeBoxPixelParams: 20 words, shared by Radial and Rectangle.
struct ShapeBoxPixel
{
    ShapeBox box;
    float cornerX;
    float cornerY;
    float softness;
    int falloff;
    float pixelW;
    float pixelH;
    float4 color0;
    float4 color1;
}

ShapeBoxPixel loadShapeBoxPixel(StructuredBuffer<uint> params)
{
    ShapeBoxPixel p;
    p.box.x1 = asfloat(params[0]);
    p.box.y1 = asfloat(params[1]);
    p.box.x2 = asfloat(params[2]);
    p.box.y2 = asfloat(params[3]);
    p.cornerX = asfloat(params[4]);
    p.cornerY = asfloat(params[5]);
    p.softness = asfloat(params[6]);
    p.falloff = int(params[7]);
    p.pixelW = asfloat(params[8]);
    p.pixelH = asfloat(params[9]);
    // Words 10 and 11 are reserved.
    p.color0 = wrapParamFloat4(params, 12);
    p.color1 = wrapParamFloat4(params, 16);
    return p;
}

struct RadialOp : IPixelOp
{
    ShapeBoxPixel p;

    [mutating] void load(StructuredBuffer<uint> params)
    {
        p = loadShapeBoxPixel(params);
    }

    float4 apply(float4 v, int2 xy)
    {
        const float px = shapePixelCoord(xy.x, p.pixelW);
        const float py = shapePixelCoord(xy.y, p.pixelH);
        const ShapeEdge edge = ellipseEdge(px, py, p.box);
        const float w = shapeWeight(edge, p.softness, p.falloff, p.pixelW, p.pixelH);
        return shapeLerpColor(p.color0, p.color1, w);
    }
}

struct RectangleOp : IPixelOp
{
    ShapeBoxPixel p;

    [mutating] void load(StructuredBuffer<uint> params)
    {
        p = loadShapeBoxPixel(params);
    }

    float4 apply(float4 v, int2 xy)
    {
        const float px = shapePixelCoord(xy.x, p.pixelW);
        const float py = shapePixelCoord(xy.y, p.pixelH);
        const ShapeEdge edge = roundedBoxEdge(px, py, p.box, p.cornerX, p.cornerY);
        const float w = shapeWeight(edge, p.softness, p.falloff, p.pixelW, p.pixelH);
        return shapeLerpColor(p.color0, p.color1, w);
    }
}

// colorWheelHsvToRgb: hue in turns.
float3 colorWheelHsvToRgb(float h, float s, float v)
{
    if (s == 0.f)
    {
        return float3(v, v, v);
    }
    h *= 6.f;
    const float sector = floor(h);
    const float f = h - sector;
    int i = int(sector) % 6;
    if (i < 0)
    {
        i += 6;
    }
    const float p = v * (1.f - s);
    const float q = v * (1.f - s * f);
    const float t = v * (1.f - s * (1.f - f));
    if (i == 0)
        return float3(v, t, p);
    if (i == 1)
        return float3(q, v, p);
    if (i == 2)
        return float3(p, v, t);
    if (i == 3)
        return float3(p, q, v);
    if (i == 4)
        return float3(t, p, v);
    return float3(v, p, q);
}

// ColorWheelPixelParams: 12 words.
struct ColorWheelOp : IPixelOp
{
    float cx;
    float cy;
    float radius;
    float softness;
    float pixelW;
    float pixelH;
    float centerSaturation;
    float edgeSaturation;
    float centerValue;
    float edgeValue;
    float rotateTurns;

    [mutating] void load(StructuredBuffer<uint> params)
    {
        cx = asfloat(params[0]);
        cy = asfloat(params[1]);
        radius = asfloat(params[2]);
        softness = asfloat(params[3]);
        pixelW = asfloat(params[4]);
        pixelH = asfloat(params[5]);
        centerSaturation = asfloat(params[6]);
        edgeSaturation = asfloat(params[7]);
        centerValue = asfloat(params[8]);
        edgeValue = asfloat(params[9]);
        rotateTurns = asfloat(params[10]);
        // Word 11 is reserved.
    }

    float4 apply(float4 v, int2 xy)
    {
        if (!(radius > 0.f))
        {
            return float4(0.f, 0.f, 0.f, 0.f);
        }
        const float px = shapePixelCoord(xy.x, pixelW);
        const float py = shapePixelCoord(xy.y, pixelH);
        ShapeBox disc;
        disc.x1 = cx - radius;
        disc.y1 = cy - radius;
        disc.x2 = cx + radius;
        disc.y2 = cy + radius;
        const float alpha = bandCoverage(ellipseEdge(px, py, disc), softness, pixelW, pixelH);
        if (!(alpha > 0.f))
        {
            return float4(0.f, 0.f, 0.f, 0.f);
        }
        const float dx = (px - cx) / radius;
        const float dy = (py - cy) / radius;
        const float d = sqrt(dx * dx + dy * dy);
        float hue = d > 0.f ? -atan2(dy, dx) * 0.15915494309189535f : 0.f;
        hue += rotateTurns;
        hue -= floor(hue);
        const float saturation = centerSaturation + d * (edgeSaturation - centerSaturation);
        const float value = centerValue + d * (edgeValue - centerValue);
        const float3 rgb = colorWheelHsvToRgb(hue, saturation, value);
        return float4(rgb.x * alpha, rgb.y * alpha, rgb.z * alpha, alpha);
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ramp_main(uint3 tid : SV_DispatchThreadID)
{
    RampOp op;
    runWrapped<RampOp>(op, tid);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void radial_main(uint3 tid : SV_DispatchThreadID)
{
    RadialOp op;
    runWrapped<RadialOp>(op, tid);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void rectangle_main(uint3 tid : SV_DispatchThreadID)
{
    RectangleOp op;
    runWrapped<RectangleOp>(op, tid);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void colorwheel_main(uint3 tid : SV_DispatchThreadID)
{
    ColorWheelOp op;
    runWrapped<ColorWheelOp>(op, tid);
}
