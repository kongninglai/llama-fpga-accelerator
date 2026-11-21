/*
 * gemv_fpga_hal.h -- user-space HAL that offloads a WHOLE Q8_0 x Q8_0 GEMV
 *                    (all rows of a weight matrix against one activation
 *                    vector) to the bus-mastering accelerator via /dev/fpga_gemv.
 *
 * Header-only and independent of ggml, so it is used both by llama.cpp
 * (ggml-cpu.c, see ggml-cpu-gemv.patch) and by gemv_test.c.
 *
 * Weights are copied ONCE, on first use, into the physically-contiguous arena
 * (keyed by the tensor's data pointer; the content is assumed immutable and the
 * buffer must stay allocated for the life of the process, as ggml's model weights
 * do -- freeing and re-allocating a buffer at the same address would reuse stale
 * data).  Every later call only copies the
 * quantized activation (<= 5 KB for Qwen2.5-0.5B), runs one ioctl and copies the
 * float results out.
 *
 * Environment variables:
 *   GGML_FPGA=0             disable (pure software)
 *   GGML_FPGA_VALIDATE=N    re-compute every N-th row in software and abort on
 *                           mismatch (1 = every row; default off)
 *   GGML_FPGA_MAXCOLS=N     offload matmuls with up to N activation columns
 *                           (default 1 = decode only; prefill will stream through FPGA)
 *   GGML_FPGA_REQUIRE=1     abort instead of falling back to software
 *   GGML_FPGA_STATS=0       do not print the summary at exit
 */
#ifndef GEMV_FPGA_HAL_H
#define GEMV_FPGA_HAL_H

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "gemv_abi.h"

#define GEMV_DEVICE      "/dev/fpga_gemv"
#define GEMV_HASH_SLOTS  2048u          /* >> number of weight tensors (~170) */
#define GEMV_ARENA_ALIGN 64u

struct gemv_ent {
	const void *src;        /* tensor data pointer (key)                     */
	uint64_t    off;        /* offset in the arena                           */
	uint64_t    bytes;      /* rows * row_bytes                              */
	int         state;      /* 0 empty, 1 resident, 2 rejected (does not fit)*/
};

static struct gemv_hal_state {
	pthread_mutex_t mu;
	pthread_once_t  once;
	int             fd;
	int             ready;
	int             validate;       /* 0 = off, N = check every N-th row      */
	int             require;
	int             stats;
	int             maxcols;
	unsigned char  *arena, *ybuf, *outbuf;
	size_t          arena_size, y_size, out_size;
	size_t          used;
	size_t cache_limit, stage_size;
	unsigned long long tiles, streamed;
	int poisoned;
	unsigned        engines;
	struct gemv_ent tab[GEMV_HASH_SLOTS];
	/* statistics */
	unsigned long long calls, rows, weight_bytes, errors, checked, fallbacks;
	double t_load, t_y, t_hw, t_out, t_val;
} gemv_hal = { .mu = PTHREAD_MUTEX_INITIALIZER, .once = PTHREAD_ONCE_INIT, .fd = -1 };

static double gemv_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void gemv_hal_exit(void)
{
	struct gemv_hal_state *h = &gemv_hal;

	if (h->stats && (h->calls || h->fallbacks || h->errors)) {
		fprintf(stderr,
			"GEMV FPGA: calls=%llu rows=%llu weights_resident=%.1f MiB "
			"fallbacks=%llu errors=%llu validated_rows=%llu\n",
			h->calls, h->rows, (double)h->used / (1 << 20),
			h->fallbacks, h->errors, h->checked);
		fprintf(stderr,
			"GEMV FPGA: time: weight-load %.2fs  y-copy %.3fs  hw %.2fs  "
			"out-copy %.3fs  validate %.2fs\n",
			h->t_load, h->t_y, h->t_hw, h->t_out, h->t_val);
		fprintf(stderr, "GEMV FPGA: hardware tiles=%llu streamed GEMVs=%llu (weight-load includes staging copies)\n", h->tiles, h->streamed);
		if (h->t_hw > 0)
			fprintf(stderr, "GEMV FPGA: effective weight bandwidth %.2f GB/s (hw time only)\n",
				(double)h->weight_bytes / h->t_hw / 1e9);
	}
}

static void gemv_hal_init(void)
{
	struct gemv_hal_state *h = &gemv_hal;
	const char *s;
	struct gemv_info info;
	long page = sysconf(_SC_PAGESIZE);
	void *m;

	h->ready = 0;
	h->stats = 1;
	h->maxcols = 128;
	if ((s = getenv("GGML_FPGA_STATS")) && !strcmp(s, "0")) h->stats = 0;
	if ((s = getenv("GGML_FPGA_REQUIRE")) && strcmp(s, "0")) h->require = 1;
	if ((s = getenv("GGML_FPGA_VALIDATE"))) h->validate = atoi(s);
	if ((s = getenv("GGML_FPGA_MAXCOLS")) && atoi(s) > 0) h->maxcols = atoi(s);
	atexit(gemv_hal_exit);

	if ((s = getenv("GGML_FPGA")) && !strcmp(s, "0")) {
		fprintf(stderr, "GEMV FPGA: disabled by GGML_FPGA=0 (software baseline)\n");
		return;
	}
	h->fd = open(GEMV_DEVICE, O_RDWR);
	if (h->fd < 0) {
		fprintf(stderr, "GEMV FPGA: cannot open %s: %s (load gemv_drv.ko?) -> software\n",
			GEMV_DEVICE, strerror(errno));
		if (h->require) abort();
		return;
	}
	if (ioctl(h->fd, GEMV_IOC_INFO, &info) < 0) {
		fprintf(stderr, "GEMV FPGA: INFO ioctl failed: %s -> software\n", strerror(errno));
		if (h->require) abort();
		return;
	}
	h->arena_size = info.arena_size;
	h->y_size = info.y_size;
	h->out_size = info.out_size;
	h->engines = info.engines;

	m = mmap(NULL, h->arena_size, PROT_READ | PROT_WRITE, MAP_SHARED, h->fd,
		 (off_t)GEMV_MMAP_ARENA * page);
	if (m == MAP_FAILED) goto mmap_fail;
	h->arena = (unsigned char *)m;
	m = mmap(NULL, h->y_size, PROT_READ | PROT_WRITE, MAP_SHARED, h->fd,
		 (off_t)GEMV_MMAP_Y * page);
	if (m == MAP_FAILED) goto mmap_fail;
	h->ybuf = (unsigned char *)m;
	m = mmap(NULL, h->out_size, PROT_READ | PROT_WRITE, MAP_SHARED, h->fd,
		 (off_t)GEMV_MMAP_OUT * page);
	if (m == MAP_FAILED) goto mmap_fail;
	h->outbuf = (unsigned char *)m;

	h->stage_size = h->arena_size / 4;
	if (h->stage_size > (8u << 20)) h->stage_size = 8u << 20;
	h->stage_size &= ~(size_t)63;
	h->cache_limit = h->arena_size - h->stage_size;
	h->ready = 1;
	fprintf(stderr,
		"GEMV FPGA: enabled, %u engine(s), arena %.0f MiB, validation %s, max columns %d\n",
		h->engines, (double)h->arena_size / (1 << 20),
		h->validate ? "ON" : "off", h->maxcols);
	return;

mmap_fail:
	fprintf(stderr, "GEMV FPGA: mmap failed: %s -> software\n", strerror(errno));
	if (h->require) abort();
}

/* 1 if the accelerator can be used (device present).  Thread safe. */
static inline int gemv_fpga_enabled(void)
{
	pthread_once(&gemv_hal.once, gemv_hal_init);
	return gemv_hal.ready;
}

static inline int gemv_fpga_maxcols(void)
{
	pthread_once(&gemv_hal.once, gemv_hal_init);
	return gemv_hal.maxcols;
}

/* ------------------------------------------------------------ validation */
static float gemv_h2f(uint32_t h)
{
	const uint32_t sign = (h & 0x8000u) << 16;
	const uint32_t e = (h >> 10) & 31u;
	uint32_t m = h & 1023u, bits;

	if (e == 0) {
		if (m == 0) {
			bits = sign;
		} else {
			int ex = 113;                       /* 127 - 14 */
			while (!(m & 1024u)) { m <<= 1; --ex; }
			bits = sign | ((uint32_t)ex << 23) | ((m & 1023u) << 13);
		}
	} else if (e == 31) {
		bits = sign | 0x7f800000u | (m << 13);
	} else {
		bits = sign | ((e + 112u) << 23) | (m << 13);
	}
	{
		float f;
		memcpy(&f, &bits, sizeof(f));
		return f;
	}
}

/* generic ggml_vec_dot_q8_0_q8_0; also returns sum|terms| for the tolerance */
static float gemv_ref_row(const uint8_t *x, const uint8_t *y, unsigned nb, double *absum)
{
	float sumf = 0.0f;
	double as = 0.0;
	unsigned b, j;

	for (b = 0; b < nb; ++b) {
		const uint8_t *xb = x + (size_t)b * GEMV_BLOCK_BYTES;
		const uint8_t *yb = y + (size_t)b * GEMV_BLOCK_BYTES;
		int sumi = 0;
		float t;

		for (j = 0; j < GEMV_QK; ++j)
			sumi += (int)(int8_t)xb[2 + j] * (int)(int8_t)yb[2 + j];
		t = (float)sumi * (gemv_h2f(xb[0] | ((uint32_t)xb[1] << 8)) *
				   gemv_h2f(yb[0] | ((uint32_t)yb[1] << 8)));
		sumf += t;
		as += fabs((double)t);
	}
	*absum = as;
	return sumf;
}

/* ------------------------------------------------------------------ run */
/*
 * Compute dst[0..ne01) = W . y  on the accelerator.
 *   w      weight tensor data (ne01 rows of nb01 = (ne00/32)*34 bytes, contiguous)
 *   y_q8   activation, ne00/32 Q8_0 blocks
 *   dst    ne01 floats
 * Returns 0 on success, -1 if the caller must compute this matmul in software
 * (feature off, shape not supported, arena full, hardware error).
 */
static int gemv_fpga_run(const void *w, size_t nb01, int64_t ne00, int64_t ne01,
			 const void *y_q8, float *dst)
{
	struct gemv_hal_state *h = &gemv_hal;
	const unsigned nb = (unsigned)(ne00 / (int64_t)GEMV_QK);
	const uint64_t row_bytes = (uint64_t)nb * GEMV_BLOCK_BYTES;
	struct gemv_ent *e;
	uint64_t done_rows = 0;
	struct gemv_job job;
	uint64_t bytes, slot;
	double t0, t1;
	int rc = -1;

	if (!gemv_fpga_enabled())
		return -1;
	if (ne00 > 32768 || ne00 <= 0 || ne00 % GEMV_QK || nb > GEMV_MAX_BLOCKS || ne01 <= 0 ||
	    nb01 != row_bytes ||
	    row_bytes > h->y_size)
		return -1;
	bytes = row_bytes * (uint64_t)ne01;

	pthread_mutex_lock(&h->mu);
	if (h->poisoned) { h->fallbacks++; goto out; }

	/* find / create the arena entry for this tensor (open addressing) */
	slot = (((uintptr_t)w >> 6) * 0x9E3779B1u) % GEMV_HASH_SLOTS;
	{
		unsigned probes = 0;

		for (;;) {
			e = &h->tab[slot];
			if (e->state == 0 || e->src == w)
				break;
			if (++probes >= GEMV_HASH_SLOTS) {   /* table full: never spin forever */
				e = NULL;
				break;
			}
			slot = (slot + 1) % GEMV_HASH_SLOTS;
		}
	}
	if (e && e->state == 0) {
		const uint64_t need = (bytes + GEMV_ARENA_ALIGN - 1) & ~(uint64_t)(GEMV_ARENA_ALIGN - 1);

		e->src = w;
		e->bytes = bytes;
		if (h->used + need > h->cache_limit) {
			e->state = 2;                       /* does not fit: CPU for good */
			fprintf(stderr, "GEMV FPGA: arena full, tensor of %.1f MiB will stream through FPGA\n",
				(double)bytes / (1 << 20));
		} else {
			t0 = gemv_now();
			e->off = h->used;
			memcpy(h->arena + e->off, w, bytes);
			h->used += need;
			e->state = 1;
			h->t_load += gemv_now() - t0;
		}
	}
	const int resident = e && e->state == 1 && e->bytes == bytes;
	if (!resident) h->streamed++;
	t0 = gemv_now();
	memcpy(h->ybuf, y_q8, row_bytes);
	h->t_y += gemv_now() - t0;
	while (done_rows < (uint64_t)ne01) {
		uint64_t nr = (uint64_t)ne01 - done_rows;
		uint64_t cap = h->out_size / sizeof(float);
		if (!resident && cap > h->stage_size / row_bytes)
			cap = h->stage_size / row_bytes;
		if (nr > cap) nr = cap;
		/* Keep the next resident tile's weight base 16-byte aligned. */
		if (nr < (uint64_t)ne01 - done_rows) nr &= ~7ull;
		if (!nr) { h->fallbacks++; goto out; }
		memset(&job, 0, sizeof(job));
		job.w_off = resident ? e->off + done_rows * row_bytes : h->cache_limit;
		if (!resident) {
			t0 = gemv_now();
			memcpy(h->arena + job.w_off, (const char *)w + done_rows * row_bytes, nr * row_bytes);
			h->t_load += gemv_now() - t0;
		}
		job.nb = nb;
		job.rows = (uint32_t)nr;
		t1 = gemv_now();
		if (ioctl(h->fd, GEMV_IOC_RUN, &job) < 0 || job.status != GEMV_ST_DONE) {
			h->errors++;
			h->poisoned = 1; /* Do not touch DMA buffers again after uncertain completion. */
			h->fallbacks++;
			if (h->require) abort();
			goto out;
		}
		t0 = gemv_now();
		h->t_hw += t0 - t1;
		h->tiles++;
		memcpy(dst + done_rows, h->outbuf, nr * sizeof(float));
		h->t_out += gemv_now() - t0;
		done_rows += nr;
	}
	h->calls++;
	h->rows += (unsigned long long)ne01;
	h->weight_bytes += bytes;
	rc = 0;

	if (h->validate > 0) {
		const unsigned char *wsrc = (const unsigned char *)w;
		int64_t r;

		t0 = gemv_now();
		for (r = 0; r < ne01; r += h->validate) {
			double absum;
			const float ref = gemv_ref_row(wsrc + (size_t)r * row_bytes,
						       (const uint8_t *)y_q8, nb, &absum);
			const double tol = 5e-5 * absum;
			if (!(fabs((double)dst[r] - (double)ref) <= tol) || !isfinite(dst[r])) {
				fprintf(stderr,
					"GEMV FPGA MISMATCH: nb=%u rows=%lld row=%lld hw=%.9g sw=%.9g tol=%.3g\n",
					nb, (long long)ne01, (long long)r, (double)dst[r],
					(double)ref, tol);
				abort();
			}
			h->checked++;
		}
		h->t_val += gemv_now() - t0;
	}
out:
	pthread_mutex_unlock(&h->mu);
	return rc;
}

#endif /* GEMV_FPGA_HAL_H */
