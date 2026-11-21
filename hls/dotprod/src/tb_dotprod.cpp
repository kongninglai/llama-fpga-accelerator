#include "dotprod.h"
#include <cmath>
#include <cstdio>
#include <cstring>

static uint32_t x[DOTPROD_INPUT_WORDS], y[DOTPROD_INPUT_WORDS];
static void put(uint32_t *mem, unsigned i, unsigned v) {
    const unsigned shift = 8 * (i % 4);
    mem[i / 4] = (mem[i / 4] & ~(255u << shift)) | ((v & 255u) << shift);
}
static double half_value(unsigned h) {
    const unsigned e = (h >> 10) & 31, m = h & 1023;
    double v = e == 0 ? std::ldexp(double(m), -24)
                         : std::ldexp(1.0 + double(m) / 1024, int(e) - 15);
    return (h & 32768) ? -v : v;
}
static bool run(unsigned blocks, unsigned seed, bool subnormal) {
    std::memset(x, 0, sizeof(x));
    std::memset(y, 0, sizeof(y));
    double reference = 0;
    for (unsigned b = 0; b < blocks; ++b) {
        unsigned dx = subnormal ? (1 + b % 1023) : (0x3000 + b % 1024);
        unsigned dy = subnormal ? 0x3c00 : (0x3400 + b % 1024);
        if (b & 1) dx |= 0x8000;
        put(x, 34*b, dx); put(x, 34*b+1, dx >> 8);
        put(y, 34*b, dy); put(y, 34*b+1, dy >> 8);
        int dot = 0;
        for (unsigned j = 0; j < 32; ++j) {
            seed = seed * 1664525u + 1013904223u;
            int a = int(seed >> 24) - 128;
            seed = seed * 1664525u + 1013904223u;
            int c = int(seed >> 24) - 128;
            if (j == 0) { a = -128; c = -128; }
            if (j == 31) { a = 127; c = -128; }
            put(x, 34*b+2+j, unsigned(a)); put(y, 34*b+2+j, unsigned(c));
            dot += a*c;
        }
        reference += dot * half_value(dx) * half_value(dy);
    }
    float result = 123;
    uint32_t status = 0;
    dotprod_accel(blocks*32, x, y, &result, &status);
    const double tolerance = 1e-4 + 1e-3 * std::fabs(reference);
    if (status != DOTPROD_DONE || !std::isfinite(result) ||
        std::fabs(result-reference) > tolerance) {
        std::printf("FAIL blocks=%u result=%.9g reference=%.12g status=%u\n",
                    blocks, result, reference, status);
        return false;
    }
    return true;
}
int main() {
    const unsigned sizes[] = {1, 2, 7, 8, 9, 127, 1024, 1};
    for (unsigned i = 0; i < sizeof(sizes)/sizeof(sizes[0]); ++i)
        if (!run(sizes[i], 17+i, false)) return 1;
    if (!run(9, 23, true)) return 1;
    const uint32_t invalid[] = {0, 1, 31, 33, 32769, 32800, 0xffffffffu};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        float result = 123;
        uint32_t status = 0;
        dotprod_accel(invalid[i], x, y, &result, &status);
        if (status != (DOTPROD_DONE | DOTPROD_ERROR) || result != 0) return 1;
    }
    if (!run(2, 42, false)) return 1; // Recovery after rejected commands.
    std::puts("PASS: packed inputs, signed extremes, lane boundaries, maximum length, subnormals, invalid lengths, repeated calls");
    return 0;
}
