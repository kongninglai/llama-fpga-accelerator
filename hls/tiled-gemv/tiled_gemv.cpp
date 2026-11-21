// Derived from ../bus-mastering-gemv/gemv_q8_0.cpp.
// Two 128-bit readers, cached activations, 32 parallel INT8 products per block.
// II pragmas are targets; inspect synthesis and routed timing before deployment.
#include "tiled_gemv.h"
#include <hls_stream.h>
#include <string.h>

typedef ap_uint<272> blk_t;   // one block: bytes 0-1 fp16 d (bits 15:0), bytes 2..33 qs (bits 271:16)
typedef ap_uint<256> q_t;     // the 32 int8 quants of one block

// ------------------------------------------------------------ small helpers
// nb is valid when 1..MAX; every process derives the SAME "active" values from
// the raw arguments, so an invalid request makes all stages do zero work.
static inline uint32_t act_nb(uint32_t nb, uint32_t rows) {
#pragma HLS INLINE
    return (rows != 0 && rows <= TG_MAX_ROWS && nb >= 1 && nb <= TG_MAX_BLOCKS) ? nb : 0u;
}
static inline uint32_t act_rows(uint32_t nb, uint32_t rows) {
#pragma HLS INLINE
    return act_nb(nb, rows) ? rows : 0u;
}
static inline uint32_t num_blocks(uint32_t nb, uint32_t rows) {
#pragma HLS INLINE
    return act_rows(nb, rows) * act_nb(nb, rows);          // <= 2^18 * 2^10 = 2^28
}
static inline uint32_t num_words(uint32_t blocks) {
#pragma HLS INLINE
    // ceil(blocks * 34 / 16); 64-bit intermediate, result < 2^32
    return (uint32_t)(((uint64_t)blocks * TG_BLOCK_BYTES + 15u) >> 4);
}

// IEEE half -> float, bit exact INCLUDING subnormals (the Xilinx half/float IP
// would flush them).  Pure integer logic.
static float half_bits_to_float(ap_uint<16> h) {
#pragma HLS INLINE
    const uint32_t hv   = h.to_uint();
    const uint32_t sign = (hv & 0x8000u) << 16;
    const uint32_t e    = (hv >> 10) & 31u;
    const uint32_t m    = hv & 1023u;
    uint32_t bits;
    if (e == 0) {
        // zero or subnormal: value = m * 2^-24; p = index of the leading one
        uint32_t p = 0;
        for (int k = 0; k < 10; ++k) {
#pragma HLS UNROLL
            if ((m >> k) & 1u) p = (uint32_t)k;
        }
        const uint32_t mn = (m << (10u - p)) & 1023u;       // hidden bit dropped
        bits = (m == 0) ? sign : (sign | ((103u + p) << 23) | (mn << 13));
    } else if (e == 31) {
        bits = sign | 0x7f800000u | (m << 13);              // inf / nan
    } else {
        bits = sign | ((e + 112u) << 23) | (m << 13);
    }
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

// ------------------------------------------------------------------ stages
// Burst-read the activation, then the weights, from DDR into two streams.
// Both pointers live on the SAME AXI bundle, and Vitis HLS only allows one
// process per bundle to read it (dataflow check HLS 200-1013), so the y and w
// reads are two sequential loops of ONE process.  y is tiny (<= 34 KB) and
// must be on chip before compute can start anyway.
static void fetch(const tg_word_t *y, const tg_word_t *w,
                  hls::stream<tg_word_t> &ys, hls::stream<tg_word_t> &ws,
                  uint32_t nb, uint32_t rows) {
    const uint32_t ywords = num_words(act_nb(nb, rows));
    const uint32_t wwords = num_words(num_blocks(nb, rows));
    for (uint32_t i = 0; i < ywords; ++i) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=60 max=2176
        ys.write(y[i]);
    }
    for (uint32_t i = 0; i < wwords; ++i) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=60 max=4352
        ws.write(w[i]);
    }
}

// 128-bit word stream -> 272-bit block stream.
//
// 8 blocks = 272 bytes = exactly 17 words, so the alignment pattern repeats
// every 17 words.  Block j (0..7) of a group occupies bytes [34j, 34j+34),
// i.e. words 2j .. 2j+2 starting 2j bytes into word 2j.  It is therefore
// complete when word 2j+2 (the 3rd word of its window) arrives, which lets us
// emit it from a 3-word window with a static shift of 16*j bits and keep the
// loop at ONE word per cycle with no bubbles.
static void unpack(hls::stream<tg_word_t> &in, hls::stream<blk_t> &out,
                   uint32_t total_blocks, uint32_t total_words) {
    const uint32_t groups = (total_blocks + 7u) >> 3;
    const uint32_t iters  = groups * 17u;
    tg_word_t prev2 = 0, prev1 = 0;
    uint32_t k = 0;            // word position inside the 17-word group
    uint32_t emitted = 0;      // blocks produced so far
    for (uint32_t i = 0; i < iters; ++i) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=17 max=9248
        tg_word_t cur = 0;
        if (i < total_words) cur = in.read();     // words past the end are never needed
        if (k >= 2 && (k & 1u) == 0) {
            const uint32_t j = (k >> 1) - 1;       // block index inside the group
            ap_uint<384> win;
            win.range(127, 0)   = prev2;           // word k-2
            win.range(255, 128) = prev1;           // word k-1
            win.range(383, 256) = cur;             // word k
            const ap_uint<384> sh = win >> (j * 16u);
            if (emitted < total_blocks) {
                const blk_t b = sh.range(271, 0);
                out.write(b);
            }
            ++emitted;
        }
        prev2 = prev1;
        prev1 = cur;
        k = (k == 16) ? 0u : k + 1u;
    }
}

static void unpack_w(hls::stream<tg_word_t> &in, hls::stream<blk_t> &out, uint32_t nb, uint32_t rows) {
    const uint32_t blocks = num_blocks(nb, rows);
    unpack(in, out, blocks, num_words(blocks));
}
static void unpack_y(hls::stream<tg_word_t> &in, hls::stream<blk_t> &out, uint32_t nb, uint32_t rows) {
    const uint32_t blocks = act_nb(nb, rows);
    unpack(in, out, blocks, num_words(blocks));
}

// Keep the activation on chip: int8 quants and the (already fp32) scale.
static void absorb_y(hls::stream<blk_t> &in, q_t yq[TG_MAX_BLOCKS], float ys[TG_MAX_BLOCKS],
                     uint32_t nb, uint32_t rows) {
    const uint32_t n = act_nb(nb, rows);
    for (uint32_t i = 0; i < n; ++i) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=28 max=1024
        const blk_t b = in.read();
        const ap_uint<16> d = b.range(15, 0);
        const q_t q = b.range(271, 16);
        ys[i] = half_bits_to_float(d);
        yq[i] = q;
    }
}

// One result per row:   sum over blocks of  (float)sumi * (dx * dy)
// Same per-block arithmetic as ggml_vec_dot_q8_0_q8_0 (generic C); only the
// ORDER of the float additions differs (8 interleaved partial sums so the
// float adder latency does not limit the loop to II>1).
static void compute(hls::stream<blk_t> &in, const q_t yq[TG_MAX_BLOCKS], const float ys[TG_MAX_BLOCKS],
                    hls::stream<float> &out, uint32_t nb_raw, uint32_t rows_raw) {
    const uint32_t nb    = act_nb(nb_raw, rows_raw);
    const uint32_t total = num_blocks(nb_raw, rows_raw);

    float acc[8];
#pragma HLS ARRAY_PARTITION variable=acc complete dim=1
    for (int k = 0; k < 8; ++k) {
#pragma HLS UNROLL
        acc[k] = 0.0f;
    }

    uint32_t b = 0;                       // block index inside the current row
    for (uint32_t i = 0; i < total; ++i) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=28 max=100000
        const blk_t blk = in.read();
        const q_t   xq  = blk.range(271, 16);
        const q_t   yqv = yq[b];
        const ap_uint<16> dxb = blk.range(15, 0);
        const float dx  = half_bits_to_float(dxb);
        const float dy  = ys[b];

        // 32 signed 8x8 products, explicit adder tree
        int32_t p[32];
#pragma HLS ARRAY_PARTITION variable=p complete dim=1
        for (int j = 0; j < 32; ++j) {
#pragma HLS UNROLL
            const ap_uint<8> xb = xq.range(8 * j + 7, 8 * j);
            const ap_uint<8> yb = yqv.range(8 * j + 7, 8 * j);
            const ap_int<8>  xs = xb;     // two's-complement reinterpretation
            const ap_int<8>  ysg = yb;
            p[j] = (int32_t)(xs * ysg).to_int();
        }
        for (int s = 16; s > 0; s >>= 1) {
#pragma HLS UNROLL
            for (int j = 0; j < s; ++j) {
#pragma HLS UNROLL
                p[j] += p[j + s];
            }
        }
        const float prod = (float)p[0] * (dx * dy);

        // 8-lane rotating accumulator; first block of a row restarts all lanes
        const bool first = (b == 0);
        float a[8], n[8];
#pragma HLS ARRAY_PARTITION variable=a complete dim=1
#pragma HLS ARRAY_PARTITION variable=n complete dim=1
        for (int k = 0; k < 8; ++k) {
#pragma HLS UNROLL
            a[k] = first ? 0.0f : acc[k];
        }
        n[0] = a[7] + prod;
        for (int k = 1; k < 8; ++k) {
#pragma HLS UNROLL
            n[k] = a[k - 1];
        }
        for (int k = 0; k < 8; ++k) {
#pragma HLS UNROLL
            acc[k] = n[k];
        }

        if (b + 1 == nb) {                // last block of the row
            const float s01 = n[0] + n[1];
            const float s23 = n[2] + n[3];
            const float s45 = n[4] + n[5];
            const float s67 = n[6] + n[7];
            out.write((s01 + s23) + (s45 + s67));
            b = 0;
        } else {
            ++b;
        }
    }
}

static void store(hls::stream<float> &in, float *out, uint32_t nb, uint32_t rows) {
    const uint32_t r = act_rows(nb, rows);
    for (uint32_t i = 0; i < r; ++i) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=8 max=64
        out[i] = in.read();
    }
}

static void run_pipeline(const tg_word_t *w0, const tg_word_t *y0, float *out0,
                         const tg_word_t *w1, const tg_word_t *y1, float *out1,
                         uint32_t nb, uint32_t rows0, uint32_t rows1) {
#pragma HLS INLINE off
#pragma HLS DATAFLOW
    // ---------------- engine 0
    hls::stream<tg_word_t> yw0("yw0"), ww0("ww0");
    hls::stream<blk_t>       yb0("yb0"), wb0("wb0");
    hls::stream<float>       res0("res0");
    q_t   yq0[TG_MAX_BLOCKS];
    float ys0[TG_MAX_BLOCKS];
#pragma HLS STREAM variable=yw0  depth=64
#pragma HLS STREAM variable=ww0  depth=128
#pragma HLS STREAM variable=yb0  depth=8
#pragma HLS STREAM variable=wb0  depth=32
#pragma HLS STREAM variable=res0 depth=64

    fetch   (y0, w0, yw0, ww0, nb, rows0);
    unpack_y(yw0, yb0, nb, rows0);
    absorb_y(yb0, yq0, ys0, nb, rows0);
    unpack_w(ww0, wb0, nb, rows0);
    compute (wb0, yq0, ys0, res0, nb, rows0);
    store   (res0, out0, nb, rows0);

    // ---------------- engine 1 (identical)
    hls::stream<tg_word_t> yw1("yw1"), ww1("ww1");
    hls::stream<blk_t>       yb1("yb1"), wb1("wb1");
    hls::stream<float>       res1("res1");
    q_t   yq1[TG_MAX_BLOCKS];
    float ys1[TG_MAX_BLOCKS];
#pragma HLS STREAM variable=yw1  depth=64
#pragma HLS STREAM variable=ww1  depth=128
#pragma HLS STREAM variable=yb1  depth=8
#pragma HLS STREAM variable=wb1  depth=32
#pragma HLS STREAM variable=res1 depth=64

    fetch   (y1, w1, yw1, ww1, nb, rows1);
    unpack_y(yw1, yb1, nb, rows1);
    absorb_y(yb1, yq1, ys1, nb, rows1);
    unpack_w(ww1, wb1, nb, rows1);
    compute (wb1, yq1, ys1, res1, nb, rows1);
    store   (res1, out1, nb, rows1);

}

extern "C" void tiled_gemv_accel(const tg_word_t *w0, const tg_word_t *y0, float *out0,
                                 const tg_word_t *w1, const tg_word_t *y1, float *out1,
                                 uint32_t nb, uint32_t rows0, uint32_t rows1,
                                 uint32_t *status) {
    // ---- AXI4 masters: one bundle (= one AXI master port) per engine.
    // `depth` only affects co-simulation and must equal TG_DEPTH_* (header).
#pragma HLS INTERFACE m_axi port=w0   offset=slave bundle=gmem0 depth=17408 max_read_burst_length=64 num_read_outstanding=16
#pragma HLS INTERFACE m_axi port=y0   offset=slave bundle=gmem0 depth=2176 max_read_burst_length=64 num_read_outstanding=16
#pragma HLS INTERFACE m_axi port=out0 offset=slave bundle=gmem0 depth=64   max_write_burst_length=64 num_write_outstanding=4
#pragma HLS INTERFACE m_axi port=w1   offset=slave bundle=gmem1 depth=17408 max_read_burst_length=64 num_read_outstanding=16
#pragma HLS INTERFACE m_axi port=y1   offset=slave bundle=gmem1 depth=2176 max_read_burst_length=64 num_read_outstanding=16
#pragma HLS INTERFACE m_axi port=out1 offset=slave bundle=gmem1 depth=64   max_write_burst_length=64 num_write_outstanding=4

    // ---- AXI4-Lite slave: control + base addresses + sizes (explicit offsets)
#pragma HLS INTERFACE s_axilite port=return bundle=CTRL
#pragma HLS INTERFACE s_axilite port=w0     bundle=CTRL offset=0x10
#pragma HLS INTERFACE s_axilite port=y0     bundle=CTRL offset=0x20
#pragma HLS INTERFACE s_axilite port=out0   bundle=CTRL offset=0x30
#pragma HLS INTERFACE s_axilite port=w1     bundle=CTRL offset=0x40
#pragma HLS INTERFACE s_axilite port=y1     bundle=CTRL offset=0x50
#pragma HLS INTERFACE s_axilite port=out1   bundle=CTRL offset=0x60
#pragma HLS INTERFACE s_axilite port=nb     bundle=CTRL offset=0x70
#pragma HLS INTERFACE s_axilite port=rows0  bundle=CTRL offset=0x80
#pragma HLS INTERFACE s_axilite port=rows1  bundle=CTRL offset=0x90
#pragma HLS INTERFACE s_axilite port=status bundle=CTRL offset=0xA0

    if (nb == 0 || nb > TG_MAX_BLOCKS ||
        rows0 > TG_MAX_ROWS || rows1 > TG_MAX_ROWS) {
        *status = TG_STATUS_DONE | TG_STATUS_ERROR;
        return;
    }
    run_pipeline(w0, y0, out0, w1, y1, out1, nb, rows0, rows1);
    *status = TG_STATUS_DONE;
}
