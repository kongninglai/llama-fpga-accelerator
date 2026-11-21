#ifndef TG_Q8_H
#define TG_Q8_H
#include <stdint.h>
#include <stddef.h>

float tg_half(uint16_t bits);
void tg_fill_q8(unsigned char *p, size_t blocks, uint32_t seed);
/* Independent double-precision reference, also supplies sum(abs(terms)). */
double tg_reference_row(const unsigned char *w, const unsigned char *y,
                        unsigned nb, double *abs_sum);
/* Float accumulation model used by the asynchronous mock, not a speed baseline. */
float tg_model_row(const unsigned char *w, const unsigned char *y, unsigned nb);
int tg_check(const unsigned char *w, const unsigned char *y, const float *out,
             unsigned nb, unsigned rows, double *max_error);
#endif
