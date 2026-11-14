module Texture;

import UtilityTypes;

int wrapTexel(int i, int size, int mode)
{
    if (mode == kWrapClamp)
        return clamp(i, 0, size - 1);
    if (mode == kWrapRepeat) {
        const int r = i % size;
        return r < 0 ? r + size : r;
    }
    if (mode == kWrapMirror) {
        const int period = 2 * size;
        int r = i % period;
        if (r < 0)
            r += period;
        return r < size ? r : period - 1 - r;
    }
    return (i < 0 || i >= size) ? -1 : i;
}

float4 fetchTexel(StructuredBuffer<float4> texels, uint offset, int width, int height, int x, int y, int wrapS, int wrapT)
{
    const int wx = wrapTexel(x, width, wrapS);
    const int wy = wrapTexel(y, height, wrapT);

    if (wx < 0 || wy < 0)
        return float4(0.0, 0.0, 0.0, 0.0);
    return texels[offset + uint(wy) * uint(width) + uint(wx)];
}

float4 sampleLevel(StructuredBuffer<uint4> images, StructuredBuffer<float4> texels, uint image, uint level, float2 uv, int wrapS, int wrapT)
{
    const uint4 record = images[image];
    uint offset = record.w;
    int width = int(record.x);
    int height = int(record.y);

    for (uint l = 0; l < level; ++l) {
        offset += uint(width) * uint(height);
        width = max(1, width / 2);
        height = max(1, height / 2);
    }

    const float x = uv.x * float(width) - 0.5;
    const float y = uv.y * float(height) - 0.5;
    const float fx = floor(x);
    const float fy = floor(y);
    const int x0 = int(fx);
    const int y0 = int(fy);
    const float ax = x - fx;
    const float ay = y - fy;
    const float4 t00 = fetchTexel(texels, offset, width, height, x0, y0, wrapS, wrapT);
    const float4 t10 = fetchTexel(texels, offset, width, height, x0 + 1, y0, wrapS, wrapT);
    const float4 t01 = fetchTexel(texels, offset, width, height, x0, y0 + 1, wrapS, wrapT);
    const float4 t11 = fetchTexel(texels, offset, width, height, x0 + 1, y0 + 1, wrapS, wrapT);

    return lerp(lerp(t00, t10, ax), lerp(t01, t11, ax), ay);
}

// Bilinear within a level, linear between the two levels around the level of detail. footprint is
// the base 2 log of the lookup's width in texture coordinates, kFinestLevel for the full-size level.
public float4 sampleImage(StructuredBuffer<uint4> images, StructuredBuffer<float4> texels, uint image, float2 uv, float footprint, int wrapS, int wrapT)
{
    const uint4 record = images[image];
    const uint levels = record.z;

    if (levels == 0)
        return float4(0.0, 0.0, 0.0, 0.0);

    const float lod = footprint + 0.5 * log2(float(record.x) * float(record.y));
    const float clamped = isnan(lod) ? 0.0 : clamp(lod, 0.0, float(levels - 1));
    const uint lower = uint(floor(clamped));
    const uint upper = min(lower + 1, levels - 1);
    const float blend = clamped - float(lower);
    const float4 a = sampleLevel(images, texels, image, lower, uv, wrapS, wrapT);

    if (blend <= 0.0 || upper == lower)
        return a;
    return lerp(a, sampleLevel(images, texels, image, upper, uv, wrapS, wrapT), blend);
}

// How much a UsdTransform2d widens a lookup, as a base 2 log like a footprint.
public float transformFootprint(TextureBinding binding)
{
    return 0.5 * log2(abs(binding.uvTransform.x * binding.uvTransform.y));
}

public float2 transformSt(TextureBinding binding, float2 st)
{
    const float2 scaled = st * binding.uvTransform.xy;
    const float c = binding.uvRotation.x;
    const float s = binding.uvRotation.y;

    return float2(c * scaled.x - s * scaled.y, s * scaled.x + c * scaled.y) + binding.uvTransform.zw;
}

float4 selectChannels(float4 v, uint first)
{
    if (first == 1)
        return float4(v.y, v.z, v.w, 0.0);
    if (first == 2)
        return float4(v.z, v.w, 0.0, 0.0);
    if (first == 3)
        return float4(v.w, 0.0, 0.0, 0.0);
    return v;
}

// A UsdUVTexture's output at texture coordinates already through its UsdTransform2d. UDIM tiles are
// found from the integer part of the coordinates, each tile covering one unit square.
public float4 sampleBinding(StructuredBuffer<TextureBinding> bindings, StructuredBuffer<uint4> images, StructuredBuffer<float4> texels, int bindingIndex, float2 uv, float footprint)
{
    const TextureBinding binding = bindings[bindingIndex];
    int image = binding.image.x;
    float2 coordinates = uv;

    if (image != kNoTexture && binding.image.y != 0) {
        const float2 tile = floor(uv);

        if (tile.x < 0.0 || tile.x > 9.0 || tile.y < 0.0 || tile.y > 9.0)
            return selectChannels(binding.fallback, binding.channels.x);
        image += int(tile.y) * 10 + int(tile.x);
        coordinates = uv - tile;
        if (images[uint(image)].z == 0)
            return selectChannels(binding.fallback, binding.channels.x);
    }
    if (image == kNoTexture)
        return selectChannels(binding.fallback, binding.channels.x);

    const float4 texel = sampleImage(images, texels, uint(image), coordinates, footprint, binding.image.z, binding.image.w);

    return selectChannels(texel * binding.scale + binding.bias, binding.channels.x);
}

// What a UsdUVTexture reads where there is nothing to look it up with.
public float4 bindingFallback(StructuredBuffer<TextureBinding> bindings, int bindingIndex)
{
    const TextureBinding binding = bindings[bindingIndex];
    return selectChannels(binding.fallback, binding.channels.x);
}
