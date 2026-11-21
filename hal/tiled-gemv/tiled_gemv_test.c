#define _POSIX_C_SOURCE 200809L
#include "tg_backend.h"
#include "q8.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *name)
{
    fprintf(stderr,
        "Usage: %s [--mock] [--device PATH] [--rows N] [--cols N]\n"
        "  [--iterations N] [--cache-mib N] [--stage-kib N]\n"
        "  [--policy stream|resident|cpu] [--serial] [--help]\n"
        "Defaults: rows=4096 cols=896 iterations=10 stage=4096 KiB, cache=remaining arena.\n"
        "Mock arena: 32 MiB. --cache-mib 0 forces streaming. Always one CPU thread.\n"
        "Validation and one warmup run are outside the execution timer.\n", name);
}

static unsigned number(const char *s)
{
    char *end;
    unsigned long n;
    errno = 0;
    n = strtoul(s, &end, 10);
    if (errno || !*s || *end || *s == '-' || n > 1000000000ul) {
        fprintf(stderr, "Invalid integer: %s\n", s);
        exit(2);
    }
    return (unsigned)n;
}

int main(int argc, char **argv)
{
    struct tg_io io = {0};
    struct tg_runtime runtime;
    struct tg_plan plan;
    struct tg_stats stats;
    unsigned rows = 4096, cols = 896, iterations = 10, cache_mib = 0, stage_kib = 4096;
    int mock = 0, overlap = 1, cache_set = 0, cpu = 0, rc = 1, a;
    const char *device = TG_DEVICE, *policy = "stream";
    unsigned char *weights = NULL, *y = NULL;
    float *out = NULL;
    size_t bytes, row_bytes, cache_bytes, stage_bytes;
    unsigned nb, i, r;
    double start, elapsed, error, preload;
    for (a = 1; a < argc; ++a) {
        const char *arg = argv[a];
        if (!strcmp(arg, "--mock")) mock = 1;
        else if (!strcmp(arg, "--serial")) overlap = 0;
        else if (!strcmp(arg, "--help")) { usage(argv[0]); return 0; }
        else {
            if (++a == argc) { usage(argv[0]); return 2; }
            if (!strcmp(arg, "--device")) device = argv[a];
            else if (!strcmp(arg, "--rows")) rows = number(argv[a]);
            else if (!strcmp(arg, "--cols")) cols = number(argv[a]);
            else if (!strcmp(arg, "--iterations")) iterations = number(argv[a]);
            else if (!strcmp(arg, "--stage-kib")) stage_kib = number(argv[a]);
            else if (!strcmp(arg, "--cache-mib")) { cache_mib = number(argv[a]); cache_set = 1; }
            else if (!strcmp(arg, "--policy")) policy = argv[a];
            else { usage(argv[0]); return 2; }
        }
    }
    if (!rows || rows > TG_MAX_ROWS || !cols || cols % TG_QK || cols > TG_MAX_BLOCKS * TG_QK ||
        !iterations || !stage_kib || stage_kib > 1024 * 1024 || cache_mib > 1024 ||
        (strcmp(policy, "stream") && strcmp(policy, "resident") && strcmp(policy, "cpu"))) {
        usage(argv[0]); return 2;
    }
    nb = cols / TG_QK;
    row_bytes = (size_t)nb * TG_BLOCK_BYTES;
    bytes = row_bytes * rows;
    weights = malloc(bytes);
    y = malloc(row_bytes);
    out = malloc((size_t)rows * sizeof(float));
    if (!weights || !y || !out) { fprintf(stderr, "Host allocation failed\n"); goto done; }
    tg_fill_q8(weights, (size_t)rows * nb, 17);
    tg_fill_q8(y, nb, 29);
    cpu = !strcmp(policy, "cpu");
    memset(&stats, 0, sizeof(stats));
    preload = 0;
    if (!cpu) {
        rc = tg_backend_open(&io, device, mock, 32u << 20);
        if (rc) { fprintf(stderr, "Backend: %s\n", strerror(-rc)); goto done; }
        stage_bytes = (size_t)stage_kib << 10;
        if (stage_bytes > io.info.arena_bytes / 2) { rc = -ENOSPC; goto done; }
        cache_bytes = cache_set ? (size_t)cache_mib << 20 :
                      (size_t)io.info.arena_bytes - 2 * stage_bytes;
        rc = tg_runtime_init(&runtime, &io, cache_bytes, stage_bytes);
        if (rc) goto done;
        rc = tg_plan_prepare(&runtime, &plan, weights, nb, rows);
        if (rc) goto done;
        preload = runtime.stats.preload_s;
        printf("Backend=%s engines=%u arena=%.1f MiB resident_rows=%u/%u stage=2x%u KiB\n",
               mock ? "MOCK (not a performance model)" : device, io.info.engines,
               (double)io.info.arena_bytes / (1u << 20), plan.resident_rows, rows, stage_kib);
        if (!strcmp(policy, "resident") && plan.resident_rows != rows) {
            puts("Resident-only policy: matrix assigned to CPU.");
            cpu = 1;
        }
    }
    /* One warmup followed by timed runs; validate outside the timed interval. */
    for (i = 0; i <= iterations; ++i) {
        if (i == 1) {
            if (!cpu) memset(&runtime.stats, 0, sizeof(runtime.stats));
            start = tg_seconds();
        }
        if (cpu) {
            for (r = 0; r < rows; ++r) out[r] = tg_model_row(weights + r * row_bytes, y, nb);
        } else {
            rc = tg_execute(&runtime, &plan, y, out, overlap);
            if (rc) { fprintf(stderr, "Execution: %s; stopping without buffer reuse\n", strerror(-rc)); goto done; }
        }
        if (i == 0 && tg_check(weights, y, out, nb, rows, &error)) {
            fprintf(stderr, "Warmup validation FAILED, max absolute error=%g\n", error);
            rc = -EIO; goto done;
        }
    }
    elapsed = tg_seconds() - start;
    if (!cpu) stats = runtime.stats;
    rc = tg_check(weights, y, out, nb, rows, &error);
    printf("%s: %ux%u, %u iterations, %.3f ms/GEMV, effective weights %.3f GB/s\n",
           cpu ? "Scalar CPU reference (not llama.cpp baseline)" : overlap ? "Double-buffered" : "Serial staging",
           rows, cols, iterations, 1000 * elapsed / iterations,
           (double)bytes * iterations / elapsed / 1e9);
    printf("Preload %.6fs (excluded); timed activation %.6fs staging %.6fs submit %.6fs wait %.6fs output %.6fs\n",
           preload, stats.activation_s, stats.stage_s, stats.submit_s, stats.wait_s, stats.output_s);
    printf("Jobs=%" PRIu64 " resident=%" PRIu64 " streamed=%" PRIu64 " staged=%.3f MiB; validation=%s max_error=%g\n",
           stats.jobs, stats.resident_jobs, stats.streamed_jobs,
           (double)stats.staged_bytes / (1u << 20), rc ? "FAIL" : "PASS", error);
done:
    if (rc) fprintf(stderr, "Test failed%s%s\n", rc < 0 ? ": " : "", rc < 0 ? strerror(-rc) : "");
    tg_backend_close(&io);
    free(weights); free(y); free(out);
    return rc ? 1 : 0;
}
