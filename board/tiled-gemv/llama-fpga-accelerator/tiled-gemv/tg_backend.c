#define _POSIX_C_SOURCE 200809L
#include "tg_backend.h"
#include "q8.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

struct backend {
    int mock, fd, active;
    struct tg_job job;
    uint64_t weight_hash, y_hash;
    struct tg_completion completion;
};

static uint64_t hash(const unsigned char *p, size_t bytes)
{
    uint64_t h = UINT64_C(14695981039346656037);
    size_t i;
    for (i = 0; i < bytes; ++i) h = (h ^ p[i]) * UINT64_C(1099511628211);
    return h;
}

static int submit(struct tg_io *io, const struct tg_job *j)
{
    struct backend *b = io->opaque;
    if (b->mock) {
        size_t row = (size_t)j->nb * TG_BLOCK_BYTES;
        if (!tg_job_valid(j, &io->info)) return -EINVAL;
        if (b->active) return -EBUSY;
        if (j->sequence <= b->completion.sequence) return -ESTALE;
        b->job = *j;
        b->weight_hash = hash(io->weights + j->weight_offset, row * j->rows);
        b->y_hash = hash(io->y + j->y_offset, row);
        b->active = 1;
        memset(&b->completion, 0, sizeof(b->completion));
        b->completion.sequence = j->sequence;
        b->completion.state = TG_JOB_BUSY;
        io->info.submitted++;
        return 0;
    }
#ifdef __linux__
    return ioctl(b->fd, TG_IOC_SUBMIT, j) < 0 ? -errno : 0;
#else
    return -ENOTSUP;
#endif
}

int tg_backend_query(struct tg_io *io, struct tg_completion *c)
{
    struct backend *b = io->opaque;
    if (b->mock) {
        if (!c->sequence || c->sequence != b->completion.sequence) return -ESTALE;
        if (c->reserved || c->timeout_ms > 60000) return -EINVAL;
        c->state = b->completion.state;
        c->hardware_status = b->completion.hardware_status;
        return 0;
    }
#ifdef __linux__
    return ioctl(b->fd, TG_IOC_QUERY, c) < 0 ? -errno : 0;
#else
    return -ENOTSUP;
#endif
}

static int wait_job(struct tg_io *io, struct tg_completion *c)
{
    struct backend *b = io->opaque;
    if (b->mock) {
        size_t row = (size_t)b->job.nb * TG_BLOCK_BYTES;
        unsigned r;
        int rc = tg_backend_query(io, c);
        if (rc || !b->active) return rc;
        if (b->weight_hash != hash(io->weights + b->job.weight_offset, row * b->job.rows) ||
            b->y_hash != hash(io->y + b->job.y_offset, row)) return -EFAULT;
        for (r = 0; r < b->job.rows; ++r)
            io->out[b->job.out_offset / sizeof(float) + r] =
                tg_model_row(io->weights + b->job.weight_offset + r * row,
                             io->y + b->job.y_offset, b->job.nb);
        b->active = 0;
        b->completion.state = TG_JOB_DONE;
        b->completion.hardware_status = TG_STATUS_DONE;
        io->info.completed++;
        return tg_backend_query(io, c);
    }
#ifdef __linux__
    return ioctl(b->fd, TG_IOC_WAIT, c) < 0 ? -errno : 0;
#else
    return -ENOTSUP;
#endif
}

void tg_backend_close(struct tg_io *io)
{
    struct backend *b = io->opaque;
    if (!b) return;
    if (b->mock) {
        free(io->weights);
        free(io->y);
        free(io->out);
    }
#ifdef __linux__
    else {
        if (io->weights) munmap(io->weights, io->info.arena_bytes);
        if (io->y) munmap(io->y, io->info.y_bytes);
        if (io->out) munmap(io->out, io->info.out_bytes);
        if (b->fd >= 0) close(b->fd);
    }
#endif
    free(b);
    memset(io, 0, sizeof(*io));
}

int tg_backend_open(struct tg_io *io, const char *device, int mock, size_t mock_arena)
{
    struct backend *b;
    int rc = -ENOMEM;
    memset(io, 0, sizeof(*io));
    b = calloc(1, sizeof(*b));
    if (!b) return rc;
    b->mock = mock;
    b->fd = -1;
    io->opaque = b;
    io->submit = submit;
    io->wait = wait_job;
    if (mock) {
        io->info.version = TG_ABI_VERSION;
        io->info.engines = 2;
        io->info.arena_bytes = mock_arena;
        io->info.y_bytes = 64 * 1024;
        io->info.out_bytes = TG_MAX_ROWS * sizeof(float);
        io->weights = calloc(1, mock_arena);
        io->y = calloc(1, io->info.y_bytes);
        io->out = calloc(1, io->info.out_bytes);
        if (!io->weights || !io->y || !io->out) goto fail;
        return 0;
    }
#ifdef __linux__
    {
        long page = sysconf(_SC_PAGESIZE);
        void *p;
        b->fd = open(device, O_RDWR | O_CLOEXEC);
        if (b->fd < 0) { rc = -errno; goto fail; }
        if (ioctl(b->fd, TG_IOC_INFO, &io->info) < 0) { rc = -errno; goto fail; }
        if (io->info.version != TG_ABI_VERSION || io->info.arena_bytes > SIZE_MAX ||
            io->info.y_bytes > SIZE_MAX || io->info.out_bytes > SIZE_MAX || page <= 0) {
            rc = -EPROTO; goto fail;
        }
        p = mmap(NULL, io->info.arena_bytes, PROT_READ | PROT_WRITE,
                 MAP_SHARED, b->fd, (off_t)TG_MMAP_WEIGHTS * page);
        if (p == MAP_FAILED) { rc = -errno; goto fail; }
        io->weights = p;
        p = mmap(NULL, io->info.y_bytes, PROT_READ | PROT_WRITE,
                 MAP_SHARED, b->fd, (off_t)TG_MMAP_Y * page);
        if (p == MAP_FAILED) { rc = -errno; goto fail; }
        io->y = p;
        p = mmap(NULL, io->info.out_bytes, PROT_READ | PROT_WRITE,
                 MAP_SHARED, b->fd, (off_t)TG_MMAP_OUT * page);
        if (p == MAP_FAILED) { rc = -errno; goto fail; }
        io->out = p;
    }
    return 0;
#else
    (void)device;
    rc = -ENOTSUP;
#endif
fail:
    tg_backend_close(io);
    return rc;
}
