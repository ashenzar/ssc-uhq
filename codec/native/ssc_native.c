/* The native SSC encoder outside the harness (ssc-uhq O5): src/ssc_encoder.c on
 * libopus v1.1 + patches/, with Samsung's tables from build/ssc_tables.h, which
 * tools/ssc-extract-tables.py writes from the user's blob (OD7). The CELTModes are built
 * as the harness's build_mode() builds them from the blob at runtime. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "modes.h"
#include "os_support.h"
#include "rate.h"
#include "ssc_encoder.h"
#include "ssc_native.h"
#include "ssc_tables.h"

static CELTMode nmode[2];
static opus_val16 nwin[2][108];
static struct ssc_tables tables;
static int ready;

static void build_mode(int idx, int fs, const opus_int16 *eb, const unsigned char *alloc,
                       const opus_int16 *logN, const opus_int16 *cache_index,
                       const unsigned char *cache_bits, int cache_size)
{
   CELTMode *m = &nmode[idx];
   memset(m, 0, sizeof *m);
   m->Fs = fs;
   m->overlap = 108;
   m->nbEBands = 22;
   m->effEBands = 21;
   m->eBands = eb;
   m->maxLM = 3;
   m->nbShortMdcts = 8;
   m->shortMdctSize = 108;
   m->nbAllocVectors = 11;
   m->allocVectors = alloc; /* the per-mode table; ssc_encode swaps in the shared one at <= 192k */
   m->logN = logN;
   /* comb_filter takes a window but patch 0008 uses its own Q31 one; stock's Q15 formula. */
   for (int i = 0; i < 108; i++) {
      double s = sin(.5 * M_PI * (i + .5) / 108);
      nwin[idx][i] = (opus_val16)fmin(32767, floor(.5 + 32768. * sin(.5 * M_PI * s * s)));
   }
   m->window = nwin[idx];
   /* Samsung's PulseCache index and bits (48 kHz band 20 differs from stock, F33.2), and
    * stock's byte caps, which init_caps() expands to the blob's int32 caps. */
   CELTMode t = *m;
   compute_pulse_cache(&t, 3);
   opus_free((void *)t.cache.index);
   opus_free((void *)t.cache.bits);
   m->cache.size = cache_size;
   m->cache.index = cache_index;
   m->cache.bits = cache_bits;
   m->cache.caps = t.cache.caps;
}

static void init_tables(void)
{
   if (ready)
      return;
   build_mode(0, 44100, ssc_eBands48, ssc_alloc_mode48, ssc_logN48, ssc_cache_index48, ssc_cache_bits48,
              SSC_CACHE_SIZE48);
   build_mode(1, 96000, ssc_eBands96, ssc_alloc_mode96, ssc_logN96, ssc_cache_index96, ssc_cache_bits96,
              SSC_CACHE_SIZE96);
   tables.mode[0] = &nmode[0];
   tables.mode[1] = &nmode[1];
   tables.alloc_shared[0] = ssc_alloc_shared48;
   tables.alloc_shared[1] = ssc_alloc_shared96;
   tables.headroom_gain = ssc_headroom_gain;
   tables.coeffs96 = ssc_coeffs96andbelow;
   tables.coeffs = ssc_coeffs;
   ready = 1;
}

struct ssc_encoder *ssc_native_create(int rate, int channels, int bits, int *err)
{
   init_tables();
   struct ssc_encoder *e = malloc(sizeof *e);
   int r = e ? ssc_ref_encoder_init(e, &tables, rate, channels, bits) : OPUS_ALLOC_FAIL;
   if (err)
      *err = r;
   if (r != OPUS_OK) {
      free(e);
      return NULL;
   }
   return e;
}

const struct ssc_tables *ssc_native_tables(void)
{
   init_tables();
   return &tables;
}

int ssc_native_encode(struct ssc_encoder *e, int32_t *pcm, int frame_size, unsigned char *out, int bitrate)
{
   return ssc_ref_encode(e, pcm, frame_size, out, bitrate);
}

void ssc_native_destroy(struct ssc_encoder *e)
{
   free(e);
}
