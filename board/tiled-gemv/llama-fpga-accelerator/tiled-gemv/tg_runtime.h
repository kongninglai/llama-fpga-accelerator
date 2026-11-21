#ifndef TG_RUNTIME_H
#define TG_RUNTIME_H
#include <stddef.h>
#include <stdint.h>
#include "tiled_gemv_abi.h"

struct tg_io {
    struct tg_info info;
    unsigned char *weights, *y;
    float *out;
    void *opaque;
    int (*submit)(struct tg_io *, const struct tg_job *);
    int (*wait)(struct tg_io *, struct tg_completion *);
};

struct tg_stats {
    uint64_t jobs, resident_jobs, streamed_jobs, staged_bytes, preload_bytes;
    double preload_s, stage_s, submit_s, wait_s, output_s, activation_s;
};

struct tg_runtime {
    struct tg_io *io;
    size_t cache_bytes, used, stage_bytes, stage_offset[2];
    uint64_t sequence;
    int failed;
    struct tg_stats stats;
};

/* Source remains immutable and alive until the plan is no longer used.
 * Plans are explicit: no pointer-keyed global cache or automatic eviction.
 */
struct tg_plan {
    const struct tg_runtime *owner;
    const unsigned char *weights;
    unsigned nb, rows, resident_rows;
    size_t resident_offset;
};

double tg_seconds(void);
int tg_runtime_init(struct tg_runtime *, struct tg_io *, size_t cache_bytes, size_t stage_bytes);
int tg_plan_prepare(struct tg_runtime *, struct tg_plan *, const void *weights,
                    unsigned nb, unsigned rows);
/* Single calling thread. Nonzero return freezes runtime buffers after an
 * uncertain failure. Destroy/recover the backend before attempting reuse.
 * overlap=0 uses the same tiles but serializes copies for an A/B comparison.
 */
int tg_execute(struct tg_runtime *, const struct tg_plan *, const void *y,
               float *dst, int overlap);
#endif
