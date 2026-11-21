#ifndef TG_BACKEND_H
#define TG_BACKEND_H
#include "tg_runtime.h"

/* Mock defers computation until WAIT and checks in-flight input integrity.
 * It validates scheduling; its timings do not model hardware performance. */
int tg_backend_open(struct tg_io *, const char *device, int mock, size_t mock_arena);
int tg_backend_query(struct tg_io *, struct tg_completion *);
void tg_backend_close(struct tg_io *);
#endif
