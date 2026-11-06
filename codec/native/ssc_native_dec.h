/* The patched v1.1 decoder (patches 0010, 0013) on the native build's tables, set up as
 * the harness's decode_one() sets it up. For host tools; the helper does not use it. */
#ifndef SSC_NATIVE_DEC_H
#define SSC_NATIVE_DEC_H

#include "celt.h"
#include "mdct.h"
#include "modes.h"
#include "opus_custom.h"
#include "ssc_encoder.h"
#include "ssc_header.h"
#include "ssc_native.h"

#define SSC_DEC_DELAY 108 /* the overlap: decoded sample i + 108 is input sample i (F38.2) */

struct ssc_dec {
   CELTMode mode;          /* the encoder's mode plus an inverse MDCT; allocVectors per frame */
   const CELTMode *base;
   const unsigned char *alloc_shared;
   CELTDecoder *dec;
};

/* mode96: 1 for the 96 kHz mode, 0 for the 44.1/48 kHz one. Returns 0 or an error. */
static int ssc_dec_init(struct ssc_dec *d, int mode96)
{
   const struct ssc_tables *t = ssc_native_tables();
   int err = 0;
   d->base = t->mode[mode96];
   d->alloc_shared = t->alloc_shared[mode96];
   d->mode = *d->base;
   if (!clt_mdct_init(&d->mode.mdct, 1728, 3))
      return -1;
   d->dec = opus_custom_decoder_create(&d->mode, 2, &err);
   if (!d->dec || err)
      return err ? err : -1;
   opus_custom_decoder_ctl(d->dec, CELT_SET_SIGNALLING(0));
   return 0;
}

/* Decodes one frame (header included) into 864 stereo samples. Returns 864, or < 0:
 * -1 unknown header, -2 range-decoder error or short output. *h gets the header. */
static int ssc_dec_frame(struct ssc_dec *d, const unsigned char *pkt, int len, opus_int16 *out,
                         struct ssc_header *h)
{
   if (ssc_header_parse(pkt, len, d->mode.nbEBands, h))
      return -1;
   d->mode.allocVectors = h->per_mode_alloc ? d->base->allocVectors : d->alloc_shared;
   opus_custom_decoder_ctl(d->dec, CELT_SET_SSC_MODE(h->uhq ? 2 : 1));
   opus_custom_decoder_ctl(d->dec, CELT_SET_END_BAND(h->end));
   int ret = opus_custom_decode(d->dec, pkt + 4, len - 4, out, 864);
   opus_int32 range_err = 0;
   opus_custom_decoder_ctl(d->dec, CELT_GET_AND_CLEAR_ERROR(&range_err));
   return ret != 864 || range_err ? -2 : 864;
}

/* Bits the range decoder consumed in the last frame (CELT_GET_SSC_TELL). */
static int ssc_dec_tell(struct ssc_dec *d)
{
   opus_int32 tell = 0;
   opus_custom_decoder_ctl(d->dec, CELT_GET_SSC_TELL(&tell));
   return tell;
}

static void ssc_dec_destroy(struct ssc_dec *d)
{
   opus_custom_decoder_destroy(d->dec);
}

#endif
