#include "tiled_gemv.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static unsigned byte_at(const std::vector<tg_word_t> &v, size_t p) {
    return v[p / 16].range((p % 16) * 8 + 7, (p % 16) * 8).to_uint();
}

static void byte_set(std::vector<tg_word_t> &v, size_t p, unsigned value) {
    v[p / 16].range((p % 16) * 8 + 7, (p % 16) * 8) = value;
}

static void fill(std::vector<tg_word_t> &v, unsigned blocks, unsigned seed) {
    for (size_t i = 0; i < v.size(); ++i) v[i] = 0;
    for (unsigned b = 0; b < blocks; ++b) {
        unsigned h = 0x2000u + ((b * 79u + seed) & 0x1fffu);
        if (b % 23 == 0) h = 1; // subnormal
        if (b % 19 == 0) h = 0;
        if (b % 7 == 0) h |= 0x8000;
        byte_set(v, b * 34, h & 255);
        byte_set(v, b * 34 + 1, h >> 8);
        for (unsigned j = 0; j < 32; ++j) {
            seed = seed * 1664525u + 1013904223u;
            byte_set(v, b * 34 + 2 + j, seed >> 24);
        }
    }
}

static double half(unsigned h) {
    unsigned e = (h >> 10) & 31, m = h & 1023;
    double v = e ? std::ldexp(double(1024 + m), int(e) - 25) : std::ldexp(double(m), -24);
    return h & 0x8000 ? -v : v;
}

static void check(const std::vector<tg_word_t> &w, const std::vector<tg_word_t> &y,
                  const std::vector<float> &out, unsigned nb, unsigned rows) {
    for (unsigned r = 0; r < rows; ++r) {
        double expected = 0, abs_sum = 0;
        for (unsigned b = 0; b < nb; ++b) {
            size_t x = (size_t(r) * nb + b) * 34, a = size_t(b) * 34;
            int sum = 0;
            for (unsigned j = 0; j < 32; ++j) {
                int xv = byte_at(w, x + 2 + j), yv = byte_at(y, a + 2 + j);
                if (xv >= 128) xv -= 256;
                if (yv >= 128) yv -= 256;
                sum += xv * yv;
            }
            double term = sum * half(byte_at(w, x) | byte_at(w, x + 1) << 8) *
                                 half(byte_at(y, a) | byte_at(y, a + 1) << 8);
            expected += term;
            abs_sum += std::fabs(term);
        }
        if (!std::isfinite(out[r]) || std::fabs(out[r] - expected) > 5e-5 * abs_sum + 1e-7) {
            std::fprintf(stderr, "row %u nb %u: got %.9g expected %.9g\n", r, nb, out[r], expected);
            std::exit(1);
        }
    }
    for (size_t i = rows; i < out.size(); ++i) {
        if (out[i] != -123456.0f) {
            std::fprintf(stderr, "Output guard overwritten at %zu\n", i);
            std::exit(1);
        }
    }
}

static void run(unsigned nb, unsigned rows0, unsigned rows1, bool valid = true) {
    std::vector<tg_word_t> w0(TG_DEPTH_W), w1(TG_DEPTH_W), y0(TG_DEPTH_Y), y1(TG_DEPTH_Y);
    std::vector<float> out0(TG_DEPTH_OUT + 1, -123456.0f), out1(TG_DEPTH_OUT + 1, -123456.0f);
    uint32_t status = 0;
    if (valid) {
        if ((size_t(nb) * rows0 * 34 + 15) / 16 > TG_DEPTH_W ||
            (size_t(nb) * rows1 * 34 + 15) / 16 > TG_DEPTH_W ||
            rows0 > TG_DEPTH_OUT || rows1 > TG_DEPTH_OUT) std::abort();
        fill(w0, nb * rows0, 17);
        fill(w1, nb * rows1, 91);
        fill(y0, nb, 31);
        fill(y1, nb, 71);
    }
    tiled_gemv_accel(w0.data(), y0.data(), out0.data(), w1.data(), y1.data(), out1.data(),
                     nb, rows0, rows1, &status);
    if (status != (valid ? TG_STATUS_DONE : TG_STATUS_DONE | TG_STATUS_ERROR)) std::abort();
    check(w0, y0, out0, nb, valid ? rows0 : 0);
    check(w1, y1, out1, nb, valid ? rows1 : 0);
}

int main() {
    run(1, 1, 0);
    run(3, 17, 7);
    run(28, 31, 17);
    run(33, 0, 19);
    run(1024, 2, 1);
    run(8, 0, 0);
    run(0, 8, 8, false);
    run(1025, 1, 1, false);
    run(1, 1, TG_MAX_ROWS + 1, false);
    run(3, 17, 7); // restart after invalid requests
    std::puts("PASS: dual engines, disabled engine, tails, max width, invalid requests, restart");
    return 0;
}
