#include "tg_backend.h"
#include "q8.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(struct tg_job) == 40, "job ABI");
_Static_assert(sizeof(struct tg_info) == 48, "info ABI");
_Static_assert(sizeof(struct tg_completion) == 24, "completion ABI");

static struct tg_runtime *observed;
static int (*real_submit)(struct tg_io *, const struct tg_job *);
static int (*real_wait)(struct tg_io *, struct tg_completion *);
static uint64_t staged_at_submit;
static unsigned lookahead_observed;

static int observe_submit(struct tg_io *io, const struct tg_job *j)
{
    staged_at_submit = observed->stats.staged_bytes;
    return real_submit(io, j);
}
static int observe_wait(struct tg_io *io, struct tg_completion *c)
{
    if (observed->stats.staged_bytes > staged_at_submit) ++lookahead_observed;
    return real_wait(io, c);
}

static size_t align64(size_t n) { return (n + 63) & ~(size_t)63; }

static void run_case(unsigned rows, unsigned nb, unsigned resident, int overlap)
{
    struct tg_io io;
    struct tg_runtime runtime;
    struct tg_plan plan;
    size_t row = (size_t)nb * 34, stage = align64(row * 8), cache = align64(row * resident);
    unsigned char *w = malloc(row * rows), *y = malloc(row);
    float *out = malloc(rows * sizeof(float));
    uint64_t preload;
    unsigned iteration;
    double error;
    assert(w && y && out);
    tg_fill_q8(w, (size_t)rows * nb, rows + nb);
    assert(tg_backend_open(&io, TG_DEVICE, 1, cache + 2 * stage) == 0);
    assert(tg_runtime_init(&runtime, &io, cache, stage) == 0);
    assert(tg_plan_prepare(&runtime, &plan, w, nb, rows) == 0);
    if (resident == rows) assert(plan.resident_rows == rows);
    if (!resident) assert(plan.resident_rows == 0);
    preload = runtime.stats.preload_bytes;
    real_submit = io.submit;
    real_wait = io.wait;
    io.submit = observe_submit;
    io.wait = observe_wait;
    observed = &runtime;
    lookahead_observed = 0;
    for (iteration = 0; iteration < 3; ++iteration) {
        tg_fill_q8(y, nb, 71 + iteration);
        memset(out, 0xa5, rows * sizeof(float));
        assert(tg_execute(&runtime, &plan, y, out, overlap) == 0);
        assert(tg_check(w, y, out, nb, rows, &error) == 0);
        assert(runtime.stats.preload_bytes == preload);
    }
    assert(runtime.stats.staged_bytes == (uint64_t)(rows - plan.resident_rows) * row * 3);
    assert(io.info.submitted == io.info.completed);
    if (!overlap || plan.resident_rows == rows) assert(!lookahead_observed);
    else if (plan.resident_rows || rows > stage / row) assert(lookahead_observed);
    tg_backend_close(&io);
    free(w); free(y); free(out);
}

static void test_descriptors(void)
{
    struct tg_io io;
    struct tg_job j = { .sequence = 1, .nb = 3, .rows = 17 };
    struct tg_completion c = { .sequence = 1 };
    unsigned char saved;
    assert(tg_backend_open(&io, TG_DEVICE, 1, 8192) == 0);
    assert(tg_job_valid(&j, &io.info));
    j.weight_offset = UINT64_MAX - 15;
    assert(!tg_job_valid(&j, &io.info));
    j.weight_offset = 1;
    assert(!tg_job_valid(&j, &io.info));
    j.weight_offset = 0;
    j.nb = 0;
    assert(io.submit(&io, &j) == -EINVAL);
    j.nb = TG_MAX_BLOCKS + 1;
    assert(io.submit(&io, &j) == -EINVAL);
    j.nb = 3;
    j.rows = TG_MAX_ROWS + 1;
    assert(io.submit(&io, &j) == -EINVAL);
    j.rows = 17;
    j.out_offset = io.info.out_bytes;
    assert(!tg_job_valid(&j, &io.info));
    j.out_offset = 0;
    assert(tg_split_rows(17, 2) == 8);
    assert(tg_split_rows(17, 1) == 17);
    tg_fill_q8(io.weights, 17 * 3, 7);
    tg_fill_q8(io.y, 3, 8);
    assert(io.submit(&io, &j) == 0);
    assert(tg_backend_query(&io, &c) == 0 && c.state == TG_JOB_BUSY);
    assert(io.submit(&io, &j) == -EBUSY);
    c.sequence = 99;
    assert(io.wait(&io, &c) == -ESTALE);
    c.sequence = 1;
    /* Deliberately corrupt an active input: mock must catch buffer reuse. */
    saved = io.weights[5];
    io.weights[5] ^= 1;
    assert(io.wait(&io, &c) == -EFAULT);
    io.weights[5] = saved;
    assert(io.wait(&io, &c) == 0 && c.state == TG_JOB_DONE);
    assert(io.wait(&io, &c) == 0); /* completion is latched */
    assert(io.submit(&io, &j) == -ESTALE);
    tg_backend_close(&io);
}

static int timeout_wait(struct tg_io *io, struct tg_completion *c)
{
    (void)io; (void)c;
    return -ETIMEDOUT;
}

static void test_failure_and_plans(void)
{
    struct tg_io io;
    struct tg_runtime runtime;
    struct tg_plan a, b;
    unsigned char w[17 * 34], y[34];
    float out[17];
    uint64_t submitted;
    assert(tg_backend_open(&io, TG_DEVICE, 1, 8192) == 0);
    tg_fill_q8(w, 17, 8);
    tg_fill_q8(y, 1, 11);
    assert(tg_runtime_init(&runtime, &io, 512, 1024) == 0);
    assert(tg_plan_prepare(&runtime, &a, w, 1, 17) == 0);
    assert(a.resident_rows == 8);
    assert(tg_plan_prepare(&runtime, &b, w, 1, 17) == 0);
    assert(b.resident_rows == 0);
    assert(tg_execute(&runtime, &a, y, out, 1) == 0);
    assert(tg_execute(&runtime, &b, y, out, 1) == 0);
    io.wait = timeout_wait;
    assert(tg_execute(&runtime, &a, y, out, 1) == -ETIMEDOUT);
    submitted = io.info.submitted;
    assert(tg_execute(&runtime, &a, y, out, 1) == -EIO);
    assert(io.info.submitted == submitted);
    assert(tg_plan_prepare(&runtime, &b, w, 1, 17) != 0);
    tg_backend_close(&io);
}

int main(void)
{
    static const unsigned rows[] = {1, 7, 8, 15, 16, 17, 33, 65};
    static const unsigned blocks[] = {1, 3, 8, 28, 33, 1024};
    unsigned i, j;
    int overlap;
    assert(tg_half(0x0001) == ldexpf(1, -24));
    assert(tg_half(0x3c00) == 1 && tg_half(0xbc00) == -1);
    assert(isinf(tg_half(0x7c00)) && isnan(tg_half(0x7e00)));
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i)
        for (j = 0; j < sizeof(blocks) / sizeof(blocks[0]); ++j)
            for (overlap = 0; overlap <= 1; ++overlap) {
                run_case(rows[i], blocks[j], 0, overlap);
                run_case(rows[i], blocks[j], rows[i], overlap);
                if (rows[i] > 8) run_case(rows[i], blocks[j], 8, overlap);
            }
    test_descriptors();
    test_failure_and_plans();
    puts("PASS: Q8 numerics, resident/partial/streamed plans, lookahead, tails, ABI bounds, ownership, timeout freeze");
    return 0;
}
