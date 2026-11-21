#ifndef TILED_GEMV_ABI_H
#define TILED_GEMV_ABI_H

#include "../../hls/tiled-gemv/tiled_gemv_regs.h"
#if defined(__linux__) || defined(__KERNEL__)
#include <linux/types.h>
#include <linux/ioctl.h>
typedef __u64 tg_u64;
typedef __u32 tg_u32;
#else
/* Host mock only. Native ioctls are used exclusively on Linux. */
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint64_t tg_u64;
typedef uint32_t tg_u32;
#endif

#define TG_DEVICE "/dev/tiled_gemv"
#define TG_ABI_VERSION 1u
#define TG_MMAP_WEIGHTS 0u
#define TG_MMAP_Y 1u
#define TG_MMAP_OUT 2u
#define TG_JOB_BUSY 1u
#define TG_JOB_DONE 2u
#define TG_JOB_ERROR 3u

struct tg_info {
    tg_u64 arena_bytes, y_bytes, out_bytes;
    tg_u32 version, engines;
    tg_u64 submitted, completed;
};

/* All offsets address driver-owned DMA allocations. Sequence numbers are
 * nonzero and strictly increasing for the lifetime of an open session.
 * SUBMIT is write-only: a successful return means the job has started.
 */
struct tg_job {
    tg_u64 sequence, weight_offset, y_offset, out_offset;
    tg_u32 nb, rows;
};

/* QUERY/WAIT return the most recent job's state. Wrong sequences are ESTALE.
 * timeout_ms is a WAIT budget, not a cancellation request. On ETIMEDOUT the
 * job still owns its buffers. QUERY/WAIT can subsequently reap completion.
 */
struct tg_completion {
    tg_u64 sequence;
    tg_u32 timeout_ms, state;
    tg_u32 hardware_status, reserved;
};

#define TG_IOC_INFO   _IOR('T', 0x40, struct tg_info)
#define TG_IOC_SUBMIT _IOW('T', 0x41, struct tg_job)
#define TG_IOC_QUERY  _IOWR('T', 0x42, struct tg_completion)
#define TG_IOC_WAIT   _IOWR('T', 0x43, struct tg_completion)

/* Shared descriptor validator for the driver and the host mock. */
static inline int tg_range_ok(tg_u64 off, tg_u64 bytes, tg_u64 limit)
{
    return off <= limit && bytes <= limit - off;
}

static inline int tg_job_valid(const struct tg_job *j, const struct tg_info *i)
{
    tg_u64 row, weights;
    if (!j->sequence || !j->nb || j->nb > TG_MAX_BLOCKS ||
        !j->rows || j->rows > TG_MAX_ROWS ||
        ((j->weight_offset | j->y_offset | j->out_offset) & 15u))
        return 0;
    row = (tg_u64)j->nb * TG_BLOCK_BYTES;
    weights = row * j->rows;
    return tg_range_ok(j->weight_offset, (weights + 15u) & ~(tg_u64)15u, i->arena_bytes) &&
           tg_range_ok(j->y_offset, (row + 15u) & ~(tg_u64)15u, i->y_bytes) &&
           tg_range_ok(j->out_offset, (tg_u64)j->rows * 4u, i->out_bytes);
}

static inline tg_u32 tg_split_rows(tg_u32 rows, tg_u32 engines)
{
    tg_u32 first = ((rows / 2u) + 4u) & ~7u;
    return engines == 2 && rows >= 16 && first < rows ? first : rows;
}
#endif
