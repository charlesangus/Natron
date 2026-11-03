// ColorBalanceRGB: darktable's color balance rgb (src/iop/colorbalancergb.c and
// data/kernels/colorbalancergb.cl, release-5.6.2, GPL-3.0-or-later), the twin of
// ColorBalanceRGBOp::pixel() and row() for a matrix working space.
import pixelwrap;
import dtcolorspaces;

static const float kMaskExponent = 0.4101205819200422;

// The parameter block, in words, as ColorBalanceRGBOp::gpu() packs it. Matrices are row-major.
static const uint kToPipe = 0;
static const uint kFromPipe = 9;
static const uint kInputMatrix = 18;
static const uint kOutputMatrix = 27;
static const uint kOutput = 36;
static const uint kFormula = 37;
static const uint kGlobal = 38;
static const uint kShadows = 41;
static const uint kHighlights = 44;
static const uint kMidtones = 47;
static const uint kChroma = 50;
static const uint kSaturation = 53;
static const uint kBrilliance = 56;
static const uint kScalars = 59;
static const uint kLut = 80;

// x^y for x >= 0, defined where Vulkan's pow is not: 0^0 is 1, as in C.
float safePow(float x, float y)
{
    return (x > 0.0) ? pow(x, y) : ((y == 0.0) ? 1.0 : 0.0);
}

float3 loadFloat3(StructuredBuffer<uint> p, uint i)
{
    return float3(asfloat(p[i]), asfloat(p[i + 1]), asfloat(p[i + 2]));
}

struct Mat3
{
    float3 r0;
    float3 r1;
    float3 r2;

    [mutating] void load(StructuredBuffer<uint> p, uint i)
    {
        r0 = loadFloat3(p, i);
        r1 = loadFloat3(p, i + 3);
        r2 = loadFloat3(p, i + 6);
    }

    float3 apply(float3 v)
    {
        return dtMul(r0, r1, r2, v);
    }
}

struct ColorBalanceRGBOp : IPixelOp
{
    Mat3 toPipe;
    Mat3 fromPipe;
    Mat3 inputMatrix;
    Mat3 outputMatrix;
    uint output;
    uint formula;
    float3 global;
    float3 shadows;
    float3 highlights;
    float3 midtones;
    float3 chroma;
    float3 saturation;
    float3 brilliance;
    float chromaGlobal;
    float saturationGlobal;
    float brillianceGlobal;
    float vibrance;
    float contrast;
    float greyFulcrum;
    float whiteFulcrum;
    float lWhite;
    float midtonesY;
    float shadowsWeight;
    float highlightsWeight;
    float midtonesWeight;
    float maskGreyFulcrum;
    float hueCos;
    float hueSin;

    [mutating] void load(StructuredBuffer<uint> p)
    {
        toPipe.load(p, kToPipe);
        fromPipe.load(p, kFromPipe);
        inputMatrix.load(p, kInputMatrix);
        outputMatrix.load(p, kOutputMatrix);
        output = p[kOutput];
        formula = p[kFormula];
        global = loadFloat3(p, kGlobal);
        shadows = loadFloat3(p, kShadows);
        highlights = loadFloat3(p, kHighlights);
        midtones = loadFloat3(p, kMidtones);
        chroma = loadFloat3(p, kChroma);
        saturation = loadFloat3(p, kSaturation);
        brilliance = loadFloat3(p, kBrilliance);
        chromaGlobal = asfloat(p[kScalars + 0]);
        saturationGlobal = asfloat(p[kScalars + 1]);
        brillianceGlobal = asfloat(p[kScalars + 2]);
        vibrance = asfloat(p[kScalars + 3]);
        contrast = asfloat(p[kScalars + 4]);
        greyFulcrum = asfloat(p[kScalars + 5]);
        whiteFulcrum = asfloat(p[kScalars + 6]);
        lWhite = asfloat(p[kScalars + 7]);
        midtonesY = asfloat(p[kScalars + 8]);
        shadowsWeight = asfloat(p[kScalars + 9]);
        highlightsWeight = asfloat(p[kScalars + 10]);
        midtonesWeight = asfloat(p[kScalars + 11]);
        maskGreyFulcrum = asfloat(p[kScalars + 12]);
        hueCos = asfloat(p[kScalars + 13]);
        hueSin = asfloat(p[kScalars + 14]);
    }

    float4 apply(float4 v, int2 xy)
    {
        const float3 pipe = toPipe.apply(v.rgb);
        float3 rgb = max(pipe, float3(0.0, 0.0, 0.0));

        float3 lms = inputMatrix.apply(rgb);
        float3 yrg = dtLmsToYrg(lms);
        float4 ych = dtYrgToYch(yrg);
        ych.x = max(ych.x, 0.0);

        // Opacity masks: shadows, mid-tones, highlights, and their complements.
        const float x = safePow(ych.x, kMaskExponent);
        const float xOffset = x - maskGreyFulcrum;
        const float xOffsetNorm = xOffset / maskGreyFulcrum;
        const float alpha = 1.0 / (1.0 + exp(xOffsetNorm * shadowsWeight));
        const float beta = 1.0 / (1.0 + exp(-xOffsetNorm * highlightsWeight));
        const float alphaComp = 1.0 - alpha;
        const float betaComp = 1.0 - beta;
        const float gamma = exp(-xOffset * xOffset * midtonesWeight / 4.0) * alphaComp * alphaComp * betaComp * betaComp * 8.0;
        const float3 opacities = float3(alpha, gamma, beta);

        if (output != 0)
        {
            const float grey = (output == 1) ? alpha : ((output == 2) ? gamma : beta);
            return float4(grey, grey, grey, v.a);
        }

        const float cosH = ych.z;
        const float sinH = ych.w;
        ych.z = hueCos * cosH - hueSin * sinH;
        ych.w = hueSin * cosH + hueCos * sinH;

        const float chromaBoost = chromaGlobal + dot(opacities, chroma);
        const float vib = vibrance * (1.0 - safePow(ych.y, abs(vibrance)));
        const float chromaFactor = max(1.0 + chromaBoost + vib, 0.0);
        ych.y *= chromaFactor;
        ych = dtGamutCheckYrg(ych);
        yrg = dtYchToYrg(ych);
        lms = dtYrgToLms(yrg);
        rgb = dtLmsToGradingRgb(lms);

        rgb += global;
        rgb *= betaComp * (alphaComp + alpha * shadows) + beta * highlights;
        [ForceUnroll]
        for (uint c = 0; c < 3; ++c)
        {
            const float sign = (rgb[c] < 0.0) ? -1.0 : 1.0;
            rgb[c] = safePow(abs(rgb[c]) / whiteFulcrum, midtones[c]) * sign * whiteFulcrum;
        }

        lms = dtGradingRgbToLms(rgb);
        yrg = dtLmsToYrg(lms);
        yrg.x = safePow(max(yrg.x / whiteFulcrum, 0.0), midtonesY) * whiteFulcrum;
        yrg.x = greyFulcrum * safePow(yrg.x / greyFulcrum, contrast);
        lms = dtYrgToLms(yrg);
        float3 xyz = dtLmsToXyz(lms);

        if (formula == 0)
        {
            float3 jab = dtXyzToJzAzBz(xyz);
            float2 jc = float2(jab.x, dtFastHypot(jab.y, jab.z));
            const float h = atan2(jab.z, jab.y);
            const float t = atan2(jc.y, jc.x);
            const float sinT = sin(t);
            const float cosT = cos(t);
            const float2 boosts = float2(1.0 + brillianceGlobal + dot(opacities, brilliance),
                                         saturationGlobal + dot(opacities, saturation));
            float2 so;
            so.x = jc.x * cosT + jc.y * sinT;
            so.y = so.x * min(max(t * boosts.y, -t), kDtHalfPi - t);
            so.x = max(so.x * boosts.x, 0.0);
            jc.x = max(so.x * cosT - so.y * sinT, 0.0);
            jc.y = max(so.x * sinT + so.y * cosT, 0.0);

            const float outMaxSatH = dtLookupGamut(wrapAux, kLut, h);
            const float sat = (jc.x > 0.0) ? dtSoftClip(jc.y / jc.x, 0.8 * outMaxSatH, outMaxSatH) : outMaxSatH;
            const float maxCAtSat = jc.x * sat;
            const float maxJAtSat = (sat > 0.0) ? jc.y / sat : jc.x;
            jc.x = (jc.x + maxJAtSat) / 2.0;
            jc.y = (jc.y + maxCAtSat) / 2.0;

            // The largest chroma at this hue and lightness that keeps L'M'S' non-negative.
            const float cosHue = cos(h);
            const float sinHue = sin(h);
            const float iz = dtJzToIz(jc.x);
            float maxC = jc.y;
            [ForceUnroll]
            for (uint i = 0; i < 3; ++i)
            {
                const float l = iz + jc.y * cosHue * kDtJzAI1[i] + jc.y * sinHue * kDtJzAI2[i];
                if (l < 0.0)
                    maxC = min(-iz / (kDtJzAI1[i] * cosHue + kDtJzAI2[i] * sinHue), maxC);
            }
            jab = float3(jc.x, maxC * cosHue, maxC * sinHue);
            xyz = dtJzAzBzToXyz(jab);
        }
        else
        {
            float3 xyY = dtD65XyzToXyY(xyz);
            float3 jch = dtXyYToUcsJCH(xyY, lWhite);
            float3 hcb = dtUcsJCHToHCB(jch);
            const float radius = dtFastHypot(hcb.y, hcb.z);
            const float sinT = (radius > 0.0) ? hcb.y / radius : 0.0;
            const float cosT = (radius > 0.0) ? hcb.z / radius : 0.0;
            const float pp = max(kDtFltMin, hcb.y);
            const float w = sinT * hcb.y + cosT * hcb.z;
            float a = max(1.0 + saturationGlobal + dot(opacities, saturation), 0.0);
            const float b = max(1.0 + brillianceGlobal + dot(opacities, brilliance), 0.0);
            const float maxA = dtFastHypot(pp, w) / pp;
            a = dtSoftClip(a, 0.5 * maxA, maxA);
            const float pPrime = (a - 1.0) * pp;
            const float wPrime = sqrt(pp * pp * (1.0 - a * a) + w * w) * b;
            hcb.y = max(cosT * pPrime + sinT * wPrime, 0.0);
            hcb.z = max(-sinT * pPrime + cosT * wPrime, 0.0);
            jch = dtUcsHCBToJCH(hcb);

            // The gamut LUT holds M², the squared colourfulness.
            const float maxColorfulness = dtLookupGamut(wrapAux, kLut, jch.z);
            const float maxChroma = dtUcsChroma(jch.x * lWhite, lWhite, maxColorfulness);
            const float3 boundaryHsb = dtUcsJCHToHSB(float3(jch.x, maxChroma, jch.z));
            float3 hsb = float3(hcb.x, (hcb.z > 0.0) ? hcb.y / hcb.z : 0.0, hcb.z);
            hsb.y = dtSoftClip(hsb.y, 0.8 * boundaryHsb.y, boundaryHsb.y);
            jch = dtUcsHSBToJCH(hsb);
            xyY = dtUcsJCHToXyY(jch, lWhite);
            xyz = dtXyYToXyz(xyY);
        }

        const float3 graded = max(outputMatrix.apply(xyz), float3(0.0, 0.0, 0.0));
        return float4(fromPipe.apply(graded), v.a);
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void colorbalancergb_wrapped(uint3 tid : SV_DispatchThreadID)
{
    ColorBalanceRGBOp op;
    runWrapped<ColorBalanceRGBOp>(op, tid);
}
