#define _POSIX_C_SOURCE 200809L
#include "tg_runtime.h"
#include <errno.h>
#include <string.h>
#include <time.h>

double tg_seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

int tg_runtime_init(struct tg_runtime *r, struct tg_io *io, size_t cache, size_t stage)
{
    if (!r || !io || !io->submit || !io->wait || !io->weights || !io->y || !io->out ||
        io->info.version != TG_ABI_VERSION || !stage || (stage & 63) || (cache & 63) ||
        cache > io->info.arena_bytes || stage > (io->info.arena_bytes - cache) / 2)
        return -EINVAL;
    memset(r, 0, sizeof(*r));
    r->io = io;
    r->cache_bytes = cache;
    r->stage_bytes = stage;
    r->stage_offset[0] = cache;
    r->stage_offset[1] = cache + stage;
    return 0;
}

int tg_plan_prepare(struct tg_runtime *r, struct tg_plan *p, const void *weights,
                    unsigned nb, unsigned rows)
{
    size_t row, available, take, bytes;
    double t;
    if (!r || !p || r->failed || !weights || !nb || nb > TG_MAX_BLOCKS || !rows || rows > TG_MAX_ROWS)
        return -EINVAL;
    row = (size_t)nb * TG_BLOCK_BYTES;
    if (((row + 15) & ~(size_t)15) > r->io->info.y_bytes ||
        r->io->info.out_bytes < 8 * sizeof(float)) return -ENOSPC;
    available = r->cache_bytes - r->used;
    take = available / row;
    if (take >= rows && ((row * rows + 63) & ~(size_t)63) <= available) take = rows;
    else {
        if (take > rows) take = rows;
        take &= ~(size_t)7;
        while (take && ((take * row + 63) & ~(size_t)63) > available) take -= 8;
    }
    if (take < rows && r->stage_bytes / row < 8) return -ENOSPC;
    memset(p, 0, sizeof(*p));
    p->owner = r;
    p->weights = weights;
    p->nb = nb;
    p->rows = rows;
    p->resident_rows = (unsigned)take;
    p->resident_offset = r->used;
    bytes = take * row;
    t = tg_seconds();
    if (bytes) {
        size_t padded = (bytes + 63) & ~(size_t)63;
        memcpy(r->io->weights + r->used, weights, bytes);
        memset(r->io->weights + r->used + bytes, 0, padded - bytes);
        r->used += padded;
    }
    r->stats.preload_bytes += bytes;
    r->stats.preload_s += tg_seconds() - t;
    return 0;
}

struct tile {
    struct tg_job job;
    unsigned first;
    int streamed;
};

static void prepare_tile(struct tg_runtime *r, const struct tg_plan *p,
                         unsigned first, unsigned slot, struct tile *t)
{
    size_t row = (size_t)p->nb * TG_BLOCK_BYTES;
    size_t cap = r->io->info.out_bytes / sizeof(float);
    unsigned remaining;
    double start;
    memset(t, 0, sizeof(*t));
    t->first = first;
    t->streamed = first >= p->resident_rows;
    remaining = (t->streamed ? p->rows : p->resident_rows) - first;
    if (cap > TG_MAX_ROWS) cap = TG_MAX_ROWS;
    if (t->streamed && cap > r->stage_bytes / row) cap = r->stage_bytes / row;
    if (cap > remaining) cap = remaining;
    if (cap < remaining) cap &= ~(size_t)7;
    t->job.nb = p->nb;
    t->job.rows = (unsigned)cap;
    t->job.weight_offset = t->streamed ? r->stage_offset[slot] : p->resident_offset + first * row;
    if (t->streamed) {
        size_t bytes = cap * row, padded = (bytes + 15) & ~(size_t)15;
        start = tg_seconds();
        memcpy(r->io->weights + t->job.weight_offset, p->weights + first * row, bytes);
        memset(r->io->weights + t->job.weight_offset + bytes, 0, padded - bytes);
        r->stats.stage_s += tg_seconds() - start;
        r->stats.staged_bytes += bytes;
    }
}

int tg_execute(struct tg_runtime *r, const struct tg_plan *p, const void *y,
               float *dst, int overlap)
{
    struct tile cur, next;
    unsigned slot = 0, end;
    size_t row;
    int rc;
    double start;
    if (!r || !p || p->owner != r || !y || !dst) return -EINVAL;
    if (r->failed) return -EIO;
    row = (size_t)p->nb * TG_BLOCK_BYTES;
    start = tg_seconds();
    memcpy(r->io->y, y, row);
    memset(r->io->y + row, 0, ((row + 15) & ~(size_t)15) - row);
    r->stats.activation_s += tg_seconds() - start;
    prepare_tile(r, p, 0, slot, &cur);
    for (;;) {
        struct tg_completion completion;
        end = cur.first + cur.job.rows;
        if (r->sequence == UINT64_MAX) { rc = -EOVERFLOW; goto failed; }
        cur.job.sequence = ++r->sequence;
        start = tg_seconds();
        rc = r->io->submit(r->io, &cur.job);
        r->stats.submit_s += tg_seconds() - start;
        if (rc) goto failed;
        r->stats.jobs++;
        if (cur.streamed) r->stats.streamed_jobs++;
        else r->stats.resident_jobs++;
        if (overlap && end < p->rows) prepare_tile(r, p, end, slot ^ 1u, &next);
        memset(&completion, 0, sizeof(completion));
        completion.sequence = cur.job.sequence;
        completion.timeout_ms = 20000;
        start = tg_seconds();
        rc = r->io->wait(r->io, &completion);
        r->stats.wait_s += tg_seconds() - start;
        if (rc) goto failed;
        if (completion.state != TG_JOB_DONE || completion.hardware_status != TG_STATUS_DONE) {
            rc = -EIO;
            goto failed;
        }
        start = tg_seconds();
        memcpy(dst + cur.first, r->io->out, cur.job.rows * sizeof(float));
        r->stats.output_s += tg_seconds() - start;
        if (end == p->rows) return 0;
        if (!overlap) prepare_tile(r, p, end, slot ^ 1u, &next);
        cur = next;
        slot ^= 1u;
    }
failed:
    r->failed = 1;
    return rc;
}
