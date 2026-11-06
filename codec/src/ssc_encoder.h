/* Native SSC encoder (T4.8): Samsung's ssc_encode on libopus v1.1 + patches/, plus the
 * Samsung-only stages (DRC limiter, headroom EQ, last-band scan). See ssc_encoder.c. */
#ifndef SSC_ENCODER_H
#define SSC_ENCODER_H

#include <stdint.h>
#include "ssc_abi.h"

struct OpusCustomMode;

/* ---- headroom (T4.7, ssc-uhq F43) ---- */
void ref_uhq_headroom(int32_t *X, int bitrate, int M, const int16_t *eb);
void ref_normal_headroom(int32_t *X, int bitrate, int M, const int16_t *eb, int stride,
                         const int32_t *table);

/* ---- DRC limiter (T4.7, ssc-uhq F44) ---- */
/* The blob's globals from drc_struct (0x120500) on: threshold, attack and release
 * coefficients, smoothed target, ramp start, last block's target; then DRC_ChNum. */
struct ref_drc {
   int32_t thr, attack, release, smooth, cur, next, ch;
};
void ref_biquad(int32_t *out, const int32_t *in, const int32_t *c, int64_t *st, int n);
int32_t ref_powercalc(const int32_t *x, int n);
int32_t ref_limit_gain(int32_t p, int32_t t, int32_t bitrate, int32_t fs);
void ref_drc_init(struct ref_drc *d, int channels);
void ref_drc_setpar(struct ref_drc *d, int32_t thr);
void ref_drc_exe(struct ref_drc *d, int32_t *x, int n);
int32_t ref_drc_frame(struct ref_drc *d, int64_t *bq, int32_t *pcm, int e, int bitrate, int fs, int bits,
                      const int32_t *c96, const int32_t *c);

/* ---- encoder ---- */
/* Samsung data the encoder needs, read from the user's blob at runtime (D28, OD7). */
struct ssc_tables {
   const struct OpusCustomMode *mode[2];   /* reference modes: [0] 44.1/48 kHz, [1] 96 kHz */
   const unsigned char *alloc_shared[2];   /* blob mode +0x28, used at bitrate <= 192000 */
   const int32_t *headroom_gain;           /* normal_headroom at bitrate >= 96000 */
   const int32_t *coeffs96, *coeffs;       /* DRC biquad */
};

struct ssc_encoder {
   struct ssc_enc_state st;                /* the blob's handle, field for field */
   const struct OpusCustomMode *mode;
   const struct ssc_tables *t;
   struct ref_drc drc;                     /* blob globals (OD8): drc_struct */
   int previous_uhq_index;                 /* blob global: last frame's end reduction */
   /* The blob's in_shared global: celt_encode_with_ec's in[], left over between frames.
    * pitch_search's xcorr global is in[827..1316) (blob 0x24d18 = in_shared + 0xcec), so
    * tone_detect's stale xcorr[244] is in[1071], the in_mem sample the previous frame
    * copied there. */
   int32_t in[2 * (864 + 108)];
   int16_t stereo_saving;                  /* alloc_trim_analysis state; unused by the bitstream */
};

/* How often each Samsung-specific branch ran (harness coverage, not encoder state). */
struct ssc_enc_stats {
   long frames, pf_on, transient, silence, cut, idx, dyn_cap, boost0, boost1, carry, anti_collapse,
      tf_select, bandE_2p29;
};
extern struct ssc_enc_stats ssc_enc_stats;

/* ssc_encoder_create(rate, channels, bits): rate 96000, 48000 or 44100; channels 2. */
int ssc_ref_encoder_init(struct ssc_encoder *e, const struct ssc_tables *t, int rate, int channels, int bits);
/* ssc_encode(st, pcm, 864, out, bitrate). pcm is interleaved int32 and is limited in place,
 * as the blob does. Returns the frame length including the 4-byte header, or < 0. */
int ssc_ref_encode(struct ssc_encoder *e, int32_t *pcm, int frame_size, unsigned char *out, int bitrate);

#endif
