/* Call traces of one ssc_encode frame, for the whole-frame test (T4.8).
 *
 * Blob side: ssc_encode reaches every exported function through the PLT, so difftest
 * exports wrappers with the same names (trace.list); the blob binds to them, and each
 * wrapper records its arguments and results, then calls the blob's own definition.
 * Only top-level calls are recorded (depth 0), i.e. the calls ssc_encode itself makes.
 * Reference side: ssc_encoder.c records the same calls with the same helpers.
 *
 * A record is one text line, "name key=value key=[array] ...". Two traces are compared
 * line by line; the first difference names the section of ssc_encode that diverges. */
#ifndef TRACE_H
#define TRACE_H

#include <stdint.h>

#ifdef SSC_NO_TRACE
/* Native builds (O5, native/): nothing is recorded and the calls compile away. */
#define tr_on 0
static inline void tr_begin(const char *name) { (void)name; }
static inline void tr_int(const char *key, long v) { (void)key; (void)v; }
static inline void tr_a32(const char *key, const int32_t *p, int n, int stride) { (void)key; (void)p; (void)n; (void)stride; }
static inline void tr_a16(const char *key, const int16_t *p, int n) { (void)key; (void)p; (void)n; }
static inline void tr_end(void) {}
#else
extern int tr_on;              /* record calls (both sides) */

void tr_init(void *blob);      /* the dlopen() handle the wrappers forward to */
void tr_reset(int side);       /* start a frame's trace: 0 = blob, 1 = reference */
void tr_begin(const char *name);
void tr_int(const char *key, long v);
void tr_a32(const char *key, const int32_t *p, int n, int stride);
void tr_a16(const char *key, const int16_t *p, int n);
void tr_end(void);

/* Compare the two traces of the current frame. Returns -1 when equal, else the index of
 * the first differing record (and prints both, pointing at the first differing field). */
int tr_compare(const char *what);
int tr_count(int side);
const char *tr_line(int side, int i);
#endif

#endif
