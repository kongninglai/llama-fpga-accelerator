#include "q8.h"
#include <math.h>

float tg_half(uint16_t h)
{
    unsigned e = (h >> 10) & 31u, m = h & 1023u;
    float v;
    if (e == 31) v = m ? NAN : INFINITY;
    else if (!e) v = ldexpf((float)m, -24);
    else v = ldexpf((float)(1024u + m), (int)e - 25);
    return h & 0x8000u ? -v : v;
}

static uint32_t step(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s;
}

void tg_fill_q8(unsigned char *p, size_t blocks, uint32_t seed)
{
    size_t b;
    for (b = 0; b < blocks; ++b) {
        /* Finite, varied scales including zero, subnormal, and negative. */
        uint16_t h = (uint16_t)(0x1800u + (step(&seed) & 0x1fffu));
        unsigned j;
        if (b % 19 == 0) h = 0;
        else if (b % 23 == 0) h = 0x0001;
        if (b % 7 == 0) h |= 0x8000;
        p[0] = (unsigned char)h;
        p[1] = (unsigned char)(h >> 8);
        for (j = 0; j < 32; ++j) p[j + 2] = (unsigned char)(step(&seed) >> 24);
        p += 34;
    }
}

static int dot32(const unsigned char *w, const unsigned char *y)
{
    int sum = 0;
    unsigned j;
    for (j = 0; j < 32; ++j)
        sum += (int)(int8_t)w[j + 2] * (int)(int8_t)y[j + 2];
    return sum;
}

static float scale(const unsigned char *p)
{
    return tg_half((uint16_t)(p[0] | (unsigned)p[1] << 8));
}

double tg_reference_row(const unsigned char *w, const unsigned char *y,
                        unsigned nb, double *abs_sum)
{
    double sum = 0, a = 0;
    unsigned b;
    for (b = 0; b < nb; ++b) {
        double t = (double)dot32(w, y) * (double)scale(w) * (double)scale(y);
        sum += t;
        a += fabs(t);
        w += 34;
        y += 34;
    }
    if (abs_sum) *abs_sum = a;
    return sum;
}

float tg_model_row(const unsigned char *w, const unsigned char *y, unsigned nb)
{
    float acc[8] = {0};
    unsigned b, k;
    for (b = 0; b < nb; ++b) {
        float t = (float)dot32(w, y) * (scale(w) * scale(y));
        float next = acc[7] + t;
        for (k = 7; k > 0; --k) acc[k] = acc[k - 1];
        acc[0] = next;
        w += 34;
        y += 34;
    }
    return ((acc[0] + acc[1]) + (acc[2] + acc[3])) +
           ((acc[4] + acc[5]) + (acc[6] + acc[7]));
}

int tg_check(const unsigned char *w, const unsigned char *y, const float *out,
             unsigned nb, unsigned rows, double *max_error)
{
    unsigned r;
    *max_error = 0;
    for (r = 0; r < rows; ++r) {
        double a, ref = tg_reference_row(w + (size_t)r * nb * 34, y, nb, &a);
        double err = fabs((double)out[r] - ref);
        if (err > *max_error) *max_error = err;
        if (!isfinite(out[r]) || err > 5e-5 * a + 1e-7) return -1;
    }
    return 0;
}
