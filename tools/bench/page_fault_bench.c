// First-touch cost of a fresh anonymous buffer, allocated the way HostImageMemory::rawAlloc does.
//   alloc  mmap    : plain mmap, small pages
//          aligned : aligned_alloc(2 MiB) + MADV_HUGEPAGE (what rawAlloc does for large buffers)
//          mmap2m  : mmap aligned to 2 MiB + MADV_HUGEPAGE
//   touch  write    : one byte store per 4 KiB
//          rw       : p[off] = p[off] per 4 KiB (as GpuImportPreparer::touch does)
//          populate : MADV_POPULATE_WRITE over each thread's slice
// Optional background threads stream memcpy over a separate buffer to compete for memory bandwidth.
// Build: gcc -O2 -pthread -o page_fault_bench page_fault_bench.c
// Usage: page_fault_bench [-s GiB] [-a mmap|aligned|mmap2m] [-t write|rw|populate] [-n THREADS]
//                         [-l LOAD_THREADS] [-j]
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif
#ifndef MADV_NOHUGEPAGE
#define MADV_NOHUGEPAGE 15
#endif
#ifndef MADV_POPULATE_WRITE
#define MADV_POPULATE_WRITE 23
#endif

#define HUGE_BYTES ((size_t)2 << 20)
#define SMALL_BYTES ((size_t)4096)
#define LOAD_HALF_BYTES ((size_t)128 << 20)

enum Alloc { ALLOC_MMAP, ALLOC_ALIGNED, ALLOC_MMAP2M };
enum Touch { TOUCH_WRITE, TOUCH_RW, TOUCH_POPULATE };

typedef struct {
    unsigned char* base;
    size_t bytes;
    enum Touch touch;
    int populateFailed;
} Slice;

static double
now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static size_t
roundUp(size_t v, size_t a)
{
    return (v + a - 1) / a * a;
}

static unsigned long long
memAvailableBytes(void)
{
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) {
        return 0;
    }
    char line[256];
    unsigned long long kb = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) {
            break;
        }
    }
    fclose(f);

    return kb * 1024ULL;
}

static unsigned long long
vmstat(const char* key)
{
    FILE* f = fopen("/proc/vmstat", "r");
    if (!f) {
        return 0;
    }
    char name[128];
    unsigned long long v, out = 0;
    while (fscanf(f, "%127s %llu", name, &v) == 2) {
        if (!strcmp(name, key)) {
            out = v;
            break;
        }
    }
    fclose(f);

    return out;
}

// The active choice is the bracketed word, or the whole line for a plain number.
static void
sysSetting(const char* file, char* out, size_t cap)
{
    char path[160], line[160];
    snprintf(path, sizeof path, "/sys/kernel/mm/transparent_hugepage/%s", file);
    snprintf(out, cap, "n/a");
    FILE* f = fopen(path, "r");
    if (!f) {
        return;
    }
    if (fgets(line, sizeof line, f)) {
        char* a = strchr(line, '[');
        char* b = a ? strchr(a, ']') : NULL;
        if (a && b) {
            *b = 0;
            snprintf(out, cap, "%s", a + 1);
        } else {
            line[strcspn(line, "\n")] = 0;
            snprintf(out, cap, "%.*s", (int)cap - 1, line);
        }
    }
    fclose(f);
}

// Summed over whole VMAs, so it only isolates the buffer because the neighbouring mappings carry
// different advice and never merge with it.
static unsigned long long
anonHugeBytes(const void* buf, size_t bytes)
{
    FILE* f = fopen("/proc/self/smaps", "r");
    if (!f) {
        return 0;
    }
    uintptr_t lo = (uintptr_t)buf, hi = lo + bytes;
    char line[512];
    int inside = 0;
    unsigned long long kb = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long s, e;
        char perms[8];
        if (sscanf(line, "%llx-%llx %7s", &s, &e, perms) == 3 && strchr(line, '-') < strchr(line, ' ')) {
            inside = s < hi && e > lo;
        } else if (inside) {
            unsigned long long v;
            if (sscanf(line, "AnonHugePages: %llu kB", &v) == 1) {
                kb += v;
            }
        }
    }
    fclose(f);

    return kb * 1024ULL;
}

static void*
touchThread(void* arg)
{
    Slice* s = arg;
    if (s->touch == TOUCH_POPULATE) {
        if (madvise(s->base, s->bytes, MADV_POPULATE_WRITE) != 0) {
            s->populateFailed = errno;
        }

        return NULL;
    }
    volatile unsigned char* p = s->base;
    if (s->touch == TOUCH_WRITE) {
        for (size_t off = 0; off < s->bytes; off += SMALL_BYTES) {
            p[off] = 1;
        }
    } else {
        for (size_t off = 0; off < s->bytes; off += SMALL_BYTES) {
            p[off] = p[off];
        }
    }

    return NULL;
}

static atomic_int loadStop;

static void*
loadThread(void* arg)
{
    unsigned char* a = arg;
    unsigned char* b = a + LOAD_HALF_BYTES;
    while (!atomic_load_explicit(&loadStop, memory_order_relaxed)) {
        memcpy(b, a, LOAD_HALF_BYTES);
        memcpy(a, b, LOAD_HALF_BYTES);
    }

    return NULL;
}

static void*
mapAligned(size_t bytes)
{
    size_t span = bytes + HUGE_BYTES;
    unsigned char* raw = mmap(NULL, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) {
        return NULL;
    }
    unsigned char* aligned = (unsigned char*)roundUp((uintptr_t)raw, HUGE_BYTES);
    if (aligned > raw) {
        munmap(raw, (size_t)(aligned - raw));
    }
    size_t tail = (size_t)((raw + span) - (aligned + bytes));
    if (tail) {
        munmap(aligned + bytes, tail);
    }

    return aligned;
}

int
main(int argc, char** argv)
{
    double gib = 1.0;
    enum Alloc alloc = ALLOC_ALIGNED;
    enum Touch touch = TOUCH_WRITE;
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    int threads = nproc > 0 ? (int)nproc : 1, load = 0, json = 0;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "-j")) {
            json = 1;
        } else if (v && !strcmp(a, "-s")) {
            gib = atof(v);
            ++i;
        } else if (v && !strcmp(a, "-n")) {
            threads = atoi(v);
            ++i;
        } else if (v && !strcmp(a, "-l")) {
            load = atoi(v);
            ++i;
        } else if (v && !strcmp(a, "-a")) {
            alloc = !strcmp(v, "mmap") ? ALLOC_MMAP : !strcmp(v, "aligned") ? ALLOC_ALIGNED : ALLOC_MMAP2M;
            if (strcmp(v, "mmap") && strcmp(v, "aligned") && strcmp(v, "mmap2m")) {
                fprintf(stderr, "unknown -a %s\n", v);
                return 2;
            }
            ++i;
        } else if (v && !strcmp(a, "-t")) {
            touch = !strcmp(v, "write") ? TOUCH_WRITE : !strcmp(v, "rw") ? TOUCH_RW : TOUCH_POPULATE;
            if (strcmp(v, "write") && strcmp(v, "rw") && strcmp(v, "populate")) {
                fprintf(stderr, "unknown -t %s\n", v);
                return 2;
            }
            ++i;
        } else {
            fprintf(stderr,
                    "usage: %s [-s GiB] [-a mmap|aligned|mmap2m] [-t write|rw|populate] [-n THREADS] "
                    "[-l LOAD_THREADS] [-j]\n",
                    argv[0]);
            return 2;
        }
    }
    if (threads < 1 || load < 0 || gib <= 0) {
        fprintf(stderr, "bad -n, -l or -s\n");
        return 2;
    }

    size_t bytes = roundUp((size_t)(gib * 1073741824.0), HUGE_BYTES);
    unsigned long long avail = memAvailableBytes();
    unsigned long long need = bytes + (load ? (unsigned long long)load * 2 * LOAD_HALF_BYTES : 0);
    if (avail && need > avail / 2) {
        fprintf(stderr, "refusing %llu MiB: over half of MemAvailable (%llu MiB)\n", need >> 20, avail >> 20);
        return 3;
    }

    const char* allocName[] = {"mmap", "aligned", "mmap2m"};
    const char* touchName[] = {"write", "rw", "populate"};
    char thpEnabled[64], thpDefrag[64], thpZero[64];
    sysSetting("enabled", thpEnabled, sizeof thpEnabled);
    sysSetting("defrag", thpDefrag, sizeof thpDefrag);
    sysSetting("use_zero_page", thpZero, sizeof thpZero);

    pthread_t* loadTids = calloc((size_t)(load ? load : 1), sizeof *loadTids);
    unsigned char** loadBufs = calloc((size_t)(load ? load : 1), sizeof *loadBufs);
    for (int i = 0; i < load; ++i) {
        // NOHUGEPAGE keeps these mappings from merging with the measured buffer's VMA.
        loadBufs[i] = mmap(NULL, 2 * LOAD_HALF_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (loadBufs[i] == MAP_FAILED) {
            fprintf(stderr, "load buffer: %s\n", strerror(errno));
            return 1;
        }
        madvise(loadBufs[i], 2 * LOAD_HALF_BYTES, MADV_NOHUGEPAGE);
        memset(loadBufs[i], 1, 2 * LOAD_HALF_BYTES);
        pthread_create(&loadTids[i], NULL, loadThread, loadBufs[i]);
    }
    if (load) {
        usleep(200000);
    }

    void* buf = NULL;
    if (alloc == ALLOC_MMAP) {
        buf = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (buf == MAP_FAILED) {
            buf = NULL;
        }
    } else if (alloc == ALLOC_ALIGNED) {
        buf = aligned_alloc(HUGE_BYTES, bytes);
    } else {
        buf = mapAligned(bytes);
    }
    if (!buf) {
        fprintf(stderr, "allocation failed: %s\n", strerror(errno));
        return 1;
    }
    if (alloc != ALLOC_MMAP) {
        madvise(buf, bytes, MADV_HUGEPAGE);
    }

    // Slices are whole 2 MiB pages so two threads never fault the same huge page.
    size_t pages = bytes / HUGE_BYTES;
    if ((size_t)threads > pages) {
        threads = (int)pages;
    }
    Slice* slices = calloc((size_t)threads, sizeof *slices);
    pthread_t* tids = calloc((size_t)threads, sizeof *tids);
    for (int i = 0; i < threads; ++i) {
        size_t p0 = pages * (size_t)i / (size_t)threads, p1 = pages * (size_t)(i + 1) / (size_t)threads;
        slices[i].base = (unsigned char*)buf + p0 * HUGE_BYTES;
        slices[i].bytes = (p1 - p0) * HUGE_BYTES;
        slices[i].touch = touch;
    }

    unsigned long long thpAlloc0 = vmstat("thp_fault_alloc"), thpFallback0 = vmstat("thp_fault_fallback"),
                       thpSplit0 = vmstat("thp_split_pmd");
    struct rusage r0, r1;
    getrusage(RUSAGE_SELF, &r0);
    double t0 = now();
    for (int i = 1; i < threads; ++i) {
        pthread_create(&tids[i], NULL, touchThread, &slices[i]);
    }
    touchThread(&slices[0]);
    for (int i = 1; i < threads; ++i) {
        pthread_join(tids[i], NULL);
    }
    double faultSec = now() - t0;
    getrusage(RUSAGE_SELF, &r1);
    unsigned long long thpAlloc = vmstat("thp_fault_alloc") - thpAlloc0,
                       thpFallback = vmstat("thp_fault_fallback") - thpFallback0,
                       thpSplit = vmstat("thp_split_pmd") - thpSplit0;

    int populateErr = 0;
    for (int i = 0; i < threads; ++i) {
        populateErr = populateErr ? populateErr : slices[i].populateFailed;
    }
    if (populateErr) {
        fprintf(stderr, "MADV_POPULATE_WRITE failed: %s%s\n", strerror(populateErr),
                populateErr == EINVAL ? " (kernel older than 5.14); use -t write or -t rw" : "");
        return 4;
    }

    unsigned long long hugeBytes = anonHugeBytes(buf, bytes);

    atomic_store(&loadStop, 1);
    for (int i = 0; i < load; ++i) {
        pthread_join(loadTids[i], NULL);
        munmap(loadBufs[i], 2 * LOAD_HALF_BYTES);
    }

    double f0 = now();
    if (alloc == ALLOC_ALIGNED) {
        free(buf);
    } else {
        munmap(buf, bytes);
    }
    double freeSec = now() - f0;

    long minflt = r1.ru_minflt - r0.ru_minflt;
    double gbps = (double)bytes / 1e9 / faultSec;
    if (json) {
        printf("{\"alloc\":\"%s\",\"touch\":\"%s\",\"gib\":%.3f,\"threads\":%d,\"load\":%d,\"gbps\":%.3f,"
               "\"fault_s\":%.4f,\"minflt\":%ld,\"anon_huge_mib\":%llu,\"thp_fault_alloc\":%llu,"
               "\"thp_fault_fallback\":%llu,\"thp_split_pmd\":%llu,\"free_s\":%.4f,"
               "\"thp_enabled\":\"%s\",\"thp_defrag\":\"%s\",\"thp_use_zero_page\":\"%s\"}\n",
               allocName[alloc], touchName[touch], (double)bytes / 1073741824.0, threads, load, gbps, faultSec, minflt,
               hugeBytes >> 20, thpAlloc, thpFallback, thpSplit, freeSec, thpEnabled, thpDefrag, thpZero);
    } else {
        printf("alloc=%s touch=%s size=%.2fGiB threads=%d load=%d  %.2f GB/s (%.3fs)  minflt=%ld  "
               "AnonHuge=%lluMiB  thp_alloc=%llu thp_fallback=%llu thp_split_pmd=%llu  free=%.3fs  "
               "thp[enabled=%s defrag=%s zero_page=%s]\n",
               allocName[alloc], touchName[touch], (double)bytes / 1073741824.0, threads, load, gbps, faultSec, minflt,
               hugeBytes >> 20, thpAlloc, thpFallback, thpSplit, freeSec, thpEnabled, thpDefrag, thpZero);
    }

    free(slices);
    free(tids);
    free(loadTids);
    free(loadBufs);

    return 0;
}
