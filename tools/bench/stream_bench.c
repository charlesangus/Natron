// What a chain of N point ops costs on this machine under three execution strategies, on an
// RGBA float image, using all cores:
//   node-at-a-time : N full passes, each reading and writing the whole image (each node renders in full
//                    before the next starts)
//   tiled          : each tile goes through all N ops while it is hot in cache
//   fused          : one pass that applies all N ops per pixel
// Build: gcc -O2 -fopenmp -o stream_bench stream_bench.c
// Usage: stream_bench WIDTH HEIGHT N [TILE]
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double
now(void)
{
    return omp_get_wtime();
}

int
main(int argc, char** argv)
{
    int w = argc > 1 ? atoi(argv[1]) : 1920;
    int h = argc > 2 ? atoi(argv[2]) : 1080;
    int n = argc > 3 ? atoi(argv[3]) : 100;
    int tile = argc > 4 ? atoi(argv[4]) : 128;
    size_t count = (size_t)w * h * 4;
    float* a = aligned_alloc(64, count * sizeof(float));
    float* b = aligned_alloc(64, count * sizeof(float));
    for (size_t i = 0; i < count; ++i) {
        a[i] = (float)(i % 97) * 0.01f;
    }
    memcpy(b, a, count * sizeof(float));

    double t0 = now();
    for (int k = 0; k < n; ++k) {
        const float* src = (k & 1) ? b : a;
        float* dst = (k & 1) ? a : b;
        float m = 1.0f + 0.0001f * (k % 7 + 1);
#pragma omp parallel for schedule(static)
        for (size_t i = 0; i < count; ++i) {
            dst[i] = src[i] * m;
        }
    }
    double pass = now() - t0;

    memcpy(b, a, count * sizeof(float));
    int tx = (w + tile - 1) / tile, ty = (h + tile - 1) / tile;
    t0 = now();
#pragma omp parallel
    {
        float* s0 = aligned_alloc(64, (size_t)tile * tile * 4 * sizeof(float));
        float* s1 = aligned_alloc(64, (size_t)tile * tile * 4 * sizeof(float));
#pragma omp for schedule(dynamic)
        for (int t = 0; t < tx * ty; ++t) {
            int x0 = (t % tx) * tile, y0 = (t / tx) * tile;
            int x1 = x0 + tile < w ? x0 + tile : w, y1 = y0 + tile < h ? y0 + tile : h;
            size_t rw = (size_t)(x1 - x0) * 4;
            for (int y = y0; y < y1; ++y) {
                memcpy(s0 + (size_t)(y - y0) * rw, a + ((size_t)y * w + x0) * 4, rw * sizeof(float));
            }
            size_t tc = rw * (y1 - y0);
            for (int k = 0; k < n; ++k) {
                const float* src = (k & 1) ? s1 : s0;
                float* dst = (k & 1) ? s0 : s1;
                float m = 1.0f + 0.0001f * (k % 7 + 1);
                for (size_t i = 0; i < tc; ++i) {
                    dst[i] = src[i] * m;
                }
            }
            const float* res = (n & 1) ? s1 : s0;
            for (int y = y0; y < y1; ++y) {
                memcpy(b + ((size_t)y * w + x0) * 4, res + (size_t)(y - y0) * rw, rw * sizeof(float));
            }
        }
        free(s0);
        free(s1);
    }
    double tiled = now() - t0;

    t0 = now();
#pragma omp parallel for schedule(static)
    for (size_t i = 0; i < count; ++i) {
        float v = a[i];
        for (int k = 0; k < n; ++k) {
            v *= 1.0f + 0.0001f * (k % 7 + 1);
        }
        b[i] = v;
    }
    double fused = now() - t0;

    double bytes = 2.0 * count * sizeof(float) * n;
    printf("{\"w\": %d, \"h\": %d, \"n\": %d, \"tile\": %d, \"threads\": %d, "
           "\"pass_s\": %.4f, \"tiled_s\": %.4f, \"fused_s\": %.4f, \"pass_GBps\": %.2f, "
           "\"per_node_pass_ms\": %.3f, \"per_node_tiled_ms\": %.3f}\n",
           w, h, n, tile, omp_get_max_threads(), pass, tiled, fused, bytes / pass / 1e9,
           1000.0 * pass / n, 1000.0 * tiled / n);
    free(a);
    free(b);
    return 0;
}
