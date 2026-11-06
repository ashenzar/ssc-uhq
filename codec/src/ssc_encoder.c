/* Native SSC encoder (T4.8). Samsung's ssc_encode is v1.1's celt_encode_with_ec() with
 * C=2, LM=3, CBR and start=0 fixed, plus Samsung's stages (ssc-uhq F43-F45, T4.8 notes):
 * - frame header and size (F35, F36), the DRC limiter on the raw input (F44);
 * - input scaled << 4 (24-bit path) or << 12 (16-bit path), no pre-emphasis filter;
 * - prefilter with tapset 0 and no tapset symbol (patch 0008, F45);
 * - no time-domain transient_analysis: only patch_transient_decision (patch 0011);
 * - headroom EQ on the MDCT (F43), the top band cut at bitrate < 192000;
 * - the adaptive top band (last-band scan, header bytes 2-3, end - idx);
 * - E mode (and 256k/328k) codes no TF; F mode forces tf_res[0..1] to the block type;
 * - dynalloc: lsb_depth 16, a different budget cap, a band 0/1 boost and a band 18-21
 *   carry-over; the intensity decision is constant (end - idx).
 * The handle is the blob's layout (ssc_abi.h); the blob's process globals (DRC state,
 * previous_uhq_index; pitch_search's xcorr stays in pitch.c) sit next to it (OD8).
 *
 * This file also holds the Samsung-only transcriptions the encoder runs: headroom and the
 * DRC chain (T4.7). It includes v1.1's celt_encoder.c for its static analysis helpers and
 * replaces celt_encoder.o in the reference build, like ref_bands.c does for bands.c. */
#include "celt_encoder.c" /* patched copy: build/<variant>/src/celt */
#include "ssc_encoder.h"
#include "trace_calls.h"
#if __has_include("ssc_mdct.h")
#include "ssc_mdct.h" /* patch 0009; STOCK=1 builds only the DRC and headroom parts */
#define HAVE_SSC_MDCT 1
#endif

/* ---- headroom (T4.7) ------------------------------------------------------------
 * Not a leveller: a fixed per-bin gain on the MDCT output, applied every frame before
 * the band energies (ssc_encode 0xc9fc / 0xcdec). uhq_headroom (E-mode) uses five Q15
 * constants keyed off eBands[18]*M and bitrate >= 270000; normal_headroom (F-mode) uses
 * the headroom_gain table (int32 entries, low 16 bits) from eBands[20]*M + 2*34 when
 * bitrate >= 96000, else two linear tapers (-3/bin from eBands[17]*M, -5/bin from
 * eBands[20]*M - 15). Both are MULT16_32_Q15 with 32-bit wrap. */
static int32_t hr_q15(int g, int32_t x)
{
   return (int32_t)((uint32_t)((x >> 16) * g) * 2u + (uint32_t)((int32_t)((x & 0xffff) * g) >> 15));
}

void ref_uhq_headroom(int32_t *X, int bitrate, int M, const int16_t *eb)
{
   int s = eb[18] * M, i;
   for (int c = 0; c < 2; c++) {
      int32_t *x = X + c * 864;
      for (i = s; i < s + 25; i++)
         x[i] = hr_q15(0x78c8, x[i]);
      if (bitrate >= 270000) {
         for (; i < 354; i++)
            x[i] = hr_q15(0x7864, x[i]);
         for (i = 354; i < 400; i++)
            x[i] = hr_q15(0x7800, x[i]);
         for (; i < 864; i++)
            x[i] = hr_q15(0x779c, x[i]);
      } else {
         for (; i < 350; i++)
            x[i] = hr_q15(0x781e, x[i]);
         for (i = 350; i < 864; i++)
            x[i] = hr_q15(0x779c, x[i]);
      }
   }
}

void ref_normal_headroom(int32_t *X, int bitrate, int M, const int16_t *eb, int stride,
                                const int32_t *table)
{
   for (int c = 0; c < 2; c++) {
      int32_t *x = X + c * stride;
      if (bitrate >= 96000) {
         for (int i = eb[20] * M + (M >> 2) * 34; i < eb[22] * M; i++)
            x[i] = hr_q15((int16_t)table[i - eb[20] * M], x[i]);
      } else {
         int s1 = eb[17] * M, s2 = eb[20] * M - 15, i;
         for (i = s1; i < s2; i++)
            x[i] = hr_q15(0x7fff - 3 * (i - s1), x[i]);
         /* The second taper restarts 0x898 below where the first one ends. */
         int g2 = 0x7767 - 3 * (s2 > s1 ? s2 - s1 : 0);
         for (i = s2; i < eb[22] * M; i++)
            x[i] = hr_q15(g2 - 5 * (i - s2), x[i]);
      }
   }
}

/* ---- DRC/limiter (T4.7) ----------------------------------------------------------
 * The level-dependent stage, on raw input before the MDCT (ssc_encode 0xbc98-0xbe24):
 *   Biquad_Narrow_int2int_stereo(narrow, pcm, coeffs, handle + 0x90, 864)
 *   powercalc(narrow) and powercalc(pcm)
 *   threshold = CalculateLimitGain(narrow, E ? total : total - narrow, bitrate, Fs), Q15,
 *               shifted << 8 always in E-mode, in F-mode only when bits == 24
 *   SSC_DRC_Exe x 8 on blocks of 108 stereo samples.
 * E-mode always filters with coeffs96andbelow; F-mode with it when bitrate < 96001, else
 * with coeffs. SSC_DRC_Exe is a block peak limiter: a block engages when more than 24 of
 * its values exceed the threshold; attack is instant (coefficient 0), release 0x3ca4a1/2^22
 * per block, and the gain ramps linearly to min(last block's target, this one's). The
 * release maps the shortfall d below unity to ceil(0.94755 d), which stalls at d = 19, so
 * after the first engaged block the gain stays at (2^22 - 19) / 2^22 for good. */

/* Transcribed from the blob's disassembly (0x6d8c). Per channel, a direct-form-I biquad on
 * x << 7 with 64-bit accumulation; the output is the accumulator's top word, and its low
 * word is carried into the next sample (error feedback), zero-extended inside a call but
 * stored sign-extended between calls. Feedback terms are kept as y << 3; the output is
 * (y + 8) >> 4. State, int64[10]: R x1 x2 y1 y2 acc, then L x1 x2 y1 y2 acc. The loop
 * runs two stereo frames at a time, so an odd n also processes frame n. */
void ref_biquad(int32_t *out, const int32_t *in, const int32_t *c, int64_t *st, int n)
{
   for (int ch = 0; ch < 2; ch++) {
      int64_t *s = st + (ch ? 0 : 5);
      int32_t x1 = (int32_t)s[0], x2 = (int32_t)s[1], y1 = (int32_t)s[2], y2 = (int32_t)s[3];
      uint64_t acc = (uint64_t)s[4];
      for (int f = 0; f < (n + 1) / 2 * 2; f++) {
         int32_t x = (int32_t)((uint32_t)in[2 * f + ch] << 7);
         acc += (uint64_t)((int64_t)x * c[0]) + (uint64_t)((int64_t)x1 * c[1]) +
                (uint64_t)((int64_t)x2 * c[2]) + (uint64_t)((int64_t)y1 * c[3]) +
                (uint64_t)((int64_t)y2 * c[4]);
         int32_t y = (int32_t)(acc >> 32);
         acc &= 0xffffffffu;
         out[2 * f + ch] = (int32_t)((uint32_t)y + 8u) >> 4;
         x2 = x1;
         x1 = x;
         y2 = y1;
         y1 = (int32_t)((uint32_t)y << 3);
      }
      s[0] = x1;
      s[1] = x2;
      s[2] = y1;
      s[3] = y2;
      s[4] = (int32_t)(uint32_t)acc;
   }
}

/* 0x6ea0: only every 32nd stereo frame contributes, each counted 8 times. n must be a
 * multiple of 32 (the blob loops until n hits 0 exactly). */
int32_t ref_powercalc(const int32_t *x, int n)
{
   uint64_t sum = 0;
   for (int f = 0; f < n; f += 32)
      for (int ch = 0; ch < 2; ch++) {
         int64_t v = x[2 * f + ch];
         sum += (uint64_t)((v * v >> 16) & ~1ll) << 4;
      }
   return (int32_t)(sum >> 9);
}

/* 0x6f0c. p = narrow-band power, t = total (E-mode) or residual (F-mode) power. Q15. */
int32_t ref_limit_gain(int32_t p, int32_t t, int32_t bitrate, int32_t fs)
{
#define PART(k) ((int32_t)((int64_t)t * (k) >> 16))
   int32_t base, off;
   if (bitrate < 96001) {
      base = 0x76c0;
      off = -80;
   } else if ((uint32_t)bitrate < 128001) {
      base = 0x78b4;
      off = -760;
   } else if (fs == 96000) {
      int hi = (uint32_t)bitrate >= 270000;
      if (PART(0xf0a4) <= p)
         return 0x7b4c;
      if (PART(0xeb85) <= p)
         return hi ? 0x7a01 : 0x7a52;
      if (PART(0xe666) <= p)
         return hi ? 0x78f4 : 0x796c;
      if (PART(0xcccc) <= p)
         return hi ? 0x785e : 0x7890;
      return 0x75d4;
   } else {
      if (PART(0xe666) <= p)
         return 0x7a94;
      if (PART(0xcccc) <= p)
         return 0x76b3;
      return p < t >> 2 ? 0x8000 : 0x741d;
   }
   if (PART(0xe666) <= p)
      return base;
   return p < t >> 2 ? 0x8000 : base + off;
#undef PART
}

void ref_drc_init(struct ref_drc *d, int channels)
{
   d->ch = channels == 2 ? 2 : 1;
   d->cur = 0x400000;
   d->next = 0x20000000;
}

void ref_drc_setpar(struct ref_drc *d, int32_t thr)
{
   d->thr = thr;
   d->attack = 0;
   d->release = 0x3ca4a1;
   d->smooth = 0x400000;
}

/* 0x70c4. Peak and count over ch * n values; the ramp covers n stereo frames. */
void ref_drc_exe(struct ref_drc *d, int32_t *x, int n)
{
   int32_t peak = 0, target = 0x400000;
   uint32_t over = 0;
   for (int i = 0; i < d->ch * n; i++) {
      int32_t a = x[i] < 0 ? (int32_t)(0u - (uint32_t)x[i]) : x[i];
      peak = a > peak ? a : peak;
      over += a > d->thr;
   }
   if (over >= 25)
      target = peak ? (int32_t)(((int64_t)d->thr << 31) / (int64_t)(uint32_t)peak) >> 9 : 0x2000;
   int32_t coef = target > d->smooth ? d->release : d->attack;
   int32_t g = (int32_t)((uint64_t)((int64_t)coef * d->smooth +
                                    (int64_t)(int32_t)(0x400000u - (uint32_t)coef) * target) >> 22);
   int32_t end = d->next < g ? d->next : g;
   int32_t step = n ? (int32_t)((uint32_t)end - (uint32_t)d->cur) / n : 0;
   d->smooth = g;
   if (d->cur != 0x400000 || step) {
      int64_t gain = d->cur;
      for (int f = 0; f < n; f++, gain += step) {
         x[2 * f] = (int32_t)((uint64_t)(gain * x[2 * f]) >> 22);
         x[2 * f + 1] = (int32_t)((uint64_t)(gain * x[2 * f + 1]) >> 22);
      }
   }
   d->cur = end;
   d->next = g;
}

/* The whole stage for one 864-sample frame, as ssc_encode runs it. Returns the threshold. */
int32_t ref_drc_frame(struct ref_drc *d, int64_t *bq, int32_t *pcm, int e, int bitrate,
                             int fs, int bits, const int32_t *c96, const int32_t *c)
{
   static int32_t narrow[864 * 2];
   ref_biquad(narrow, pcm, e || bitrate < 96001 ? c96 : c, bq, 864);
   int32_t p = ref_powercalc(narrow, 864), t = ref_powercalc(pcm, 864);
   int32_t thr = ref_limit_gain(p, e ? t : t - p, bitrate, fs);
   d->thr = e || bits == 24 ? (int32_t)((uint32_t)thr << 8) : thr;
   for (int b = 0; b < 8; b++)
      ref_drc_exe(d, pcm + b * 216, 108);
   return d->thr;
}

/* ---- encoder (T4.8) ------------------------------------------------------------- */
#ifdef HAVE_SSC_MDCT

struct ssc_enc_stats ssc_enc_stats;

#define SSC_XCORR_IN 827 /* (0x24d18 - 0x2402c) / 4 */
#define SSC_VQ_IN 864    /* (0x24dac - 0x2402c) / 4 */

static const unsigned char ssc_trim_icdf[11] = {126, 124, 119, 109, 87, 41, 19, 9, 4, 2, 0};

int ssc_ref_encoder_init(struct ssc_encoder *e, const struct ssc_tables *t, int rate, int channels, int bits)
{
   const CELTMode *mode;
   if (channels != 2)
      return OPUS_BAD_ARG; /* the blob's encoder is stereo-only past init (C=2 everywhere, F30) */
   if (rate == 96000)
      mode = t->mode[1];
   else if (rate == 48000 || rate == 44100)
      mode = t->mode[0];
   else
      return OPUS_BAD_ARG;
   memset(e, 0, sizeof *e);
   struct ssc_enc_state *st = &e->st;
   e->mode = mode;
   e->t = t;
   st->mode = mode;
   st->overlap = mode->overlap;
   st->channels = channels;
   st->end = mode->effEBands;
   st->bitrate = OPUS_BITRATE_MAX;
   st->spread_decision = SPREAD_NORMAL;
   st->delayedIntra = 1;
   st->tonal_average = 256;
   st->Fs = rate == 48000 ? 44100 : rate;
   st->create_rate = rate;
   st->bits = bits;
   st->hdr2_low = 1;
   /* Unlike v1.1's reset, oldLogE/oldLogE2 stay 0, not -28 dB. */
   e->previous_uhq_index = 0;
   ref_drc_init(&e->drc, channels);
   ref_drc_setpar(&e->drc, 0x7b8e);
   return OPUS_OK;
}

/* abs() with the blob's wrap: |INT_MIN| stays negative and never wins a max. */
static int32_t ssc_maxabs(const int32_t *x, int n, int32_t m)
{
   for (int i = 0; i < n; i++) {
      int32_t a = x[i] < 0 ? (int32_t)(0u - (uint32_t)x[i]) : x[i];
      m = a > m ? a : m;
   }
   return m;
}

/* ssc_compute_band_energies_for_lastband (F34.2): stock band energies on `count` equal
 * slices of one band, channel 1 right after channel 0. uhq selects the band idx below the
 * top one; otherwise the top band. */
static void ssc_lastband_energies(const CELTMode *m, const celt_sig *X, opus_val32 *bandE, int count, int uhq,
                                  int idx)
{
   int band = m->nbEBands - 1 - (uhq ? idx : 0);
   int lo = 8 * m->eBands[band], w = (8 * m->eBands[band + 1] - lo) / count;
   for (int c = 0; c < 2; c++)
      for (int i = 0; i < count; i++) {
         const celt_sig *x = X + c * 864 + lo + i * w;
         opus_val32 maxval = 0, sum = 0;
         for (int j = 0; j < w; j++)
            maxval = MAX32(maxval, MAX32(x[j], -x[j]));
         if (maxval <= 0) {
            bandE[i + c * count] = EPSILON;
            continue;
         }
         int shift = celt_ilog2(maxval) - 10;
         for (int j = 0; j < w; j++)
            sum = MAC16_16(sum, EXTRACT16(VSHR32(x[j], shift)), EXTRACT16(VSHR32(x[j], shift)));
         bandE[i + c * count] = EPSILON + VSHR32(EXTEND32(celt_sqrt(sum)), -shift);
      }
}

/* v1.1's dynalloc_analysis() with Samsung's two changes: the 1/4-budget cap is
 * effectiveBytes*16 (v1.1 rounds it down to a multiple of 4 bytes first), and when the
 * loop ends without hitting the cap, bands 0 and 1 get a boost of up to 10 quanta from
 * their level above 6.35 dB (band 0) / 5.96 dB (band 1) and their drop to the next band.
 * vbr = 0, lsb_depth 16, no lfe or surround. */
static void ssc_dynalloc_analysis(const opus_val16 *bandLogE, const opus_val16 *bandLogE2, int nbEBands,
                                  int end, int C, int *offsets, const opus_int16 *logN, int isTransient,
                                  const opus_int16 *eBands, int LM, int effectiveBytes)
{
   int i, c;
   opus_int32 tot_boost = 0;
   opus_val16 follower[2 * 22], noise_floor[22];
   for (i = 0; i < nbEBands; i++)
      offsets[i] = 0;
   for (i = 0; i < end; i++)
      noise_floor[i] = MULT16_16(QCONST16(0.0625f, DB_SHIFT), logN[i]) + QCONST16(.5f, DB_SHIFT) +
                       SHL16(9 - 16, DB_SHIFT) - SHL16(eMeans[i], 6) +
                       MULT16_16(QCONST16(.0062, DB_SHIFT), (i + 5) * (i + 5));
   if (effectiveBytes > 50) {
      int last = 0;
      c = 0;
      do {
         follower[c * nbEBands] = bandLogE2[c * nbEBands];
         for (i = 1; i < end; i++) {
            if (bandLogE2[c * nbEBands + i] > bandLogE2[c * nbEBands + i - 1] + QCONST16(.5f, DB_SHIFT))
               last = i;
            follower[c * nbEBands + i] =
               MIN16(follower[c * nbEBands + i - 1] + QCONST16(1.5f, DB_SHIFT), bandLogE2[c * nbEBands + i]);
         }
         for (i = last - 1; i >= 0; i--)
            follower[c * nbEBands + i] =
               MIN16(follower[c * nbEBands + i],
                     MIN16(follower[c * nbEBands + i + 1] + QCONST16(2.f, DB_SHIFT), bandLogE2[c * nbEBands + i]));
         for (i = 0; i < end; i++)
            follower[c * nbEBands + i] = MAX16(follower[c * nbEBands + i], noise_floor[i]);
      } while (++c < C);
      for (i = 0; i < end; i++) {
         follower[nbEBands + i] = MAX16(follower[nbEBands + i], follower[i] - QCONST16(4.f, DB_SHIFT));
         follower[i] = MAX16(follower[i], follower[nbEBands + i] - QCONST16(4.f, DB_SHIFT));
         follower[i] = HALF16(MAX16(0, bandLogE[i] - follower[i]) +
                              MAX16(0, bandLogE[nbEBands + i] - follower[nbEBands + i]));
      }
      if (!isTransient)
         for (i = 0; i < end; i++)
            follower[i] = HALF16(follower[i]);
      for (i = 0; i < end; i++) {
         int width, boost, boost_bits;
         if (i < 8)
            follower[i] *= 2;
         if (i >= 12)
            follower[i] = HALF16(follower[i]);
         follower[i] = MIN16(follower[i], QCONST16(4, DB_SHIFT));
         width = C * (eBands[i + 1] - eBands[i]) << LM;
         if (width < 6) {
            boost = (int)SHR32(EXTEND32(follower[i]), DB_SHIFT);
            boost_bits = boost * width << BITRES;
         } else if (width > 48) {
            boost = (int)SHR32(EXTEND32(follower[i]) * 8, DB_SHIFT);
            boost_bits = (boost * width << BITRES) / 8;
         } else {
            boost = (int)SHR32(EXTEND32(follower[i]) * width / 6, DB_SHIFT);
            boost_bits = boost * 6 << BITRES;
         }
         if ((tot_boost + boost_bits) >> BITRES >> 3 > effectiveBytes / 4) {
            offsets[i] = (effectiveBytes << BITRES << 3) / 4 - tot_boost;
            ssc_enc_stats.dyn_cap++;
            return;
         }
         offsets[i] = boost;
         tot_boost += boost_bits;
      }
   }
   {
      opus_int32 lvl = IMIN(bandLogE2[0], bandLogE2[nbEBands]) - 6500;
      opus_int32 drop = IMIN(bandLogE2[0] - bandLogE2[1], bandLogE2[nbEBands] - bandLogE2[nbEBands + 1]);
      if (lvl > 0 && drop > 0) {
         drop = IMAX(drop, 1500);
         int b = (int)(((uint32_t)(drop - 1500 + 65536) >> 7 & 0x1ff) + ((uint32_t)lvl >> 3 & 0x1fff) / 25);
         ssc_enc_stats.boost0 += IMIN(b, 10) > offsets[0];
         offsets[0] = IMAX(offsets[0], IMIN(b, 10));
      }
   }
   {
      opus_int32 lvl = IMIN(bandLogE2[1], bandLogE2[nbEBands + 1]) - 6100;
      opus_int32 drop = IMIN(bandLogE2[1] - bandLogE2[2], bandLogE2[nbEBands + 1] - bandLogE2[nbEBands + 2]);
      opus_int32 rise = IMIN(bandLogE2[1] - bandLogE2[0], bandLogE2[nbEBands + 1] - bandLogE2[nbEBands]);
      if (lvl > 0 && drop > 0 && rise > 0) {
         int b = (drop - 1700) / 160 + (int)(((uint32_t)lvl >> 3 & 0x1fff) / 25);
         ssc_enc_stats.boost1 += IMIN(b, 10) > offsets[1];
         offsets[1] = IMAX(offsets[1], IMIN(b, 10));
      }
   }
}

/* v1.1's tf_encode(), recording its symbols. */
static void ssc_tf_encode(int start, int end, int isTransient, int *tf_res, int LM, int tf_select, ec_enc *enc)
{
   int curr, i;
   int tf_select_rsv;
   int tf_changed;
   int logp;
   opus_uint32 budget;
   opus_uint32 tell;
   budget = enc->storage * 8;
   tell = ec_tell(enc);
   logp = isTransient ? 2 : 4;
   tf_select_rsv = LM > 0 && tell + logp + 1 <= budget;
   budget -= tf_select_rsv;
   curr = tf_changed = 0;
   for (i = start; i < end; i++) {
      if (tell + logp <= budget) {
         ec_enc_bit_logp(enc, tf_res[i] ^ curr, logp);
         if (tr_on)
            tr_ec_bit_logp(tf_res[i] ^ curr, logp);
         tell = ec_tell(enc);
         curr = tf_res[i];
         tf_changed |= curr;
      } else
         tf_res[i] = curr;
      logp = isTransient ? 4 : 5;
   }
   if (tf_select_rsv && tf_select_table[LM][4 * isTransient + 0 + tf_changed] !=
                           tf_select_table[LM][4 * isTransient + 2 + tf_changed]) {
      ec_enc_bit_logp(enc, tf_select, 1);
      if (tr_on)
         tr_ec_bit_logp(tf_select, 1);
   } else
      tf_select = 0;
   for (i = start; i < end; i++)
      tf_res[i] = tf_select_table[LM][4 * isTransient + 2 * tf_select + tf_res[i]];
}

int ssc_ref_encode(struct ssc_encoder *e, int32_t *pcm, int frame_size, unsigned char *out, int bitrate)
{
   struct ssc_enc_state *st = &e->st;
   const CELTMode *mode = e->mode;
   const int C = 2, LM = 3, M = 8, N = frame_size;
   const int nbEBands = mode->nbEBands, overlap = mode->overlap;
   const opus_int16 *eBands = mode->eBands;
   const int mi = mode == e->t->mode[1];
   int i, c;
   ec_enc enc;

   st->frame_bitrate = bitrate;
   st->end = nbEBands;

   /* Header tag and the two mode flags (0xb6cc-0xbc38, F35). e_hdr: byte 1 carries the
    * frame counter (E-mode header). uhq: the 24-bit path (E-mode DRC coefficients and
    * threshold scale, input << 4, uhq_headroom, no tf analysis, no 3/128 extra bytes).
    * 256k and 328k have F tags but take the uhq path. */
   int tag, e_hdr = 0, uhq = 0, early_thr = -1;
   switch (bitrate) {
   case 88000: tag = 0xf0; early_thr = 0x76f2; break;
   case 96000: tag = 0xf1; early_thr = 0x76f2; break;
   case 128000: tag = 0xf2; early_thr = 0x78b4; break;
   case 192000: tag = 0xf3; early_thr = 0x7a94; break;
   case 229000: tag = 0xf4; early_thr = 0x7a94; break;
   case 328000: tag = 0xf5; uhq = 1; break;
   case 256000: tag = 0xf6; uhq = 1; break;
   case 291000: tag = 0xe0; e_hdr = uhq = 1; early_thr = 0x7b4c00; break;
   case 308000: tag = 0xe1; e_hdr = uhq = 1; early_thr = 0x7b4c00; break;
   case 442000: tag = 0xe2; e_hdr = uhq = 1; early_thr = 0x7b4c00; break;
   case 584000: tag = 0xe3; e_hdr = uhq = 1; early_thr = 0x7b4c00; break;
   case 886000: tag = 0xe4; e_hdr = uhq = 1; early_thr = 0x7b4c00; break;
   case 250000: tag = 0xe5; e_hdr = uhq = 1; early_thr = 0x79f800; break;
   case 152000: tag = 0xe6; e_hdr = uhq = 1; early_thr = 0x79f800; break;
   default: tag = 0xf3; break;
   }
   /* An early threshold, always overwritten before SSC_DRC_Exe runs. */
   if (early_thr >= 0) {
      if (early_thr < 0x10000 && st->bits == 24)
         early_thr <<= 8;
      e->drc.thr = early_thr;
      if (tr_on)
         tr_drc_limit(early_thr);
   }
   out[0] = 0xff;
   out[1] = 0xee;
   out[2] = (unsigned char)((st->hdr2_low & 3) | st->frame_count << 4);
   out[3] = (unsigned char)tag;
   if (e_hdr)
      out[1] = (unsigned char)(0xe0 | (st->frame_count >> 4 & 0xf));

   /* Frame size (F36.2): odd payload, 4-byte header. */
   int bytes = st->Fs ? st->frame_bitrate * N / (st->Fs * 8) : 0;
   bytes += uhq ? 0 : bytes * 3 >> 7;
   bytes += ~bytes & 1;
   int nbCompressedBytes = bytes - 1;
   ec_enc_init(&enc, out + 4, nbCompressedBytes);
   if (tr_on)
      tr_ec_init(nbCompressedBytes);

   /* DRC limiter on the raw input, in place (F44). */
   {
      static int32_t narrow[864 * 2];
      ref_biquad(narrow, pcm, uhq || bitrate < 96001 ? e->t->coeffs96 : e->t->coeffs, st->biquad, N);
      int32_t p = ref_powercalc(narrow, N), tot = ref_powercalc(pcm, N);
      if (tr_on) {
         tr_powercalc(N, p);
         tr_powercalc(N, tot);
      }
      int32_t t = uhq ? tot : tot - p, thr = ref_limit_gain(p, t, bitrate, st->create_rate);
      if (tr_on)
         tr_limit_gain(p, t, bitrate, st->create_rate, thr);
      e->drc.thr = uhq || st->bits == 24 ? (int32_t)((uint32_t)thr << 8) : thr;
      if (tr_on)
         tr_drc_limit(e->drc.thr);
      for (int b = 0; b < 8; b++) {
         ref_drc_exe(&e->drc, pcm + b * (N >> 2), N >> 3);
         if (tr_on)
            tr_drc_exe(pcm + b * (N >> 2), N >> 3);
      }
   }
   ec_enc_shrink(&enc, nbCompressedBytes);
   if (tr_on)
      tr_ec_shrink(nbCompressedBytes);
   if (pcm == NULL || bytes < 3)
      return -1;

   /* From here on: v1.1's celt_encode_with_ec(), CBR, start 0, C 2, LM 3. */
   nbCompressedBytes = IMIN(nbCompressedBytes, 1275);
   int tell = ec_tell(&enc);
   int nbFilledBytes = (tell + 4) >> 3;
   int nbAvailableBytes = nbCompressedBytes - nbFilledBytes;
   int effectiveBytes = nbCompressedBytes;
   opus_int32 total_bits = nbCompressedBytes * 8;

   int32_t sample_max = ssc_maxabs(pcm, C * (N - overlap), st->overlap_max);
   st->overlap_max = ssc_maxabs(pcm + C * (N - overlap), C * overlap, 0);
   sample_max = MAX32(sample_max, st->overlap_max);
   int silence = sample_max == 0;
   if (tell == 1) {
      ec_enc_bit_logp(&enc, silence, 15);
      if (tr_on)
         tr_ec_bit_logp(silence, 15);
   } else
      silence = 0;
   if (silence) {
      tell = nbCompressedBytes * 8;
      enc.nbits_total += tell - ec_tell(&enc);
   }

   /* "Pre-emphasis": no filter (preemph[] is zero, F29), just the input scale: the 24-bit
    * path shifts << 4, the 16-bit path (F-mode unless bits == 24) << 12. */
   celt_sig *in = e->in;
   int shift = uhq || st->bits == 24 ? 4 : 12;
   for (c = 0; c < C; c++)
      for (i = 0; i < N; i++)
         in[c * (N + overlap) + overlap + i] = (celt_sig)((uint32_t)pcm[C * i + c] << shift);

   /* Prefilter (v1.1 run_prefilter, tapset 0, no tapset symbol; F45.1). */
   int pitch_index, pf_on, qg = 0;
   opus_val16 gain1, gain1q = 0;
   {
      static celt_sig _pre[2 * (864 + COMBFILTER_MAXPERIOD)];
      static opus_val16 pitch_buf[(COMBFILTER_MAXPERIOD + 864) >> 1];
      celt_sig *pre[2] = {_pre, _pre + N + COMBFILTER_MAXPERIOD};
      for (c = 0; c < C; c++) {
         OPUS_COPY(pre[c], st->prefilter_mem + c * COMBFILTER_MAXPERIOD, COMBFILTER_MAXPERIOD);
         OPUS_COPY(pre[c] + COMBFILTER_MAXPERIOD, in + c * (N + overlap) + overlap, N);
      }
      if (nbAvailableBytes < 25 || silence) {
         gain1 = 0;
         pitch_index = COMBFILTER_MINPERIOD;
      } else {
         pitch_downsample(pre, pitch_buf, COMBFILTER_MAXPERIOD + N, C, 0);
         /* The blob's xcorr is in[827..]: the coarse search writes [0, 244), tone_detect
          * may read [244], the fine search overwrites [0, 489). */
         OPUS_COPY(ssc_pitch_xcorr, in + SSC_XCORR_IN, (COMBFILTER_MAXPERIOD - 3 * COMBFILTER_MINPERIOD) >> 1);
         if (tr_on)
            tr_pitch_downsample(pitch_buf, COMBFILTER_MAXPERIOD + N);
         pitch_search(pitch_buf + (COMBFILTER_MAXPERIOD >> 1), pitch_buf, N,
                      COMBFILTER_MAXPERIOD - 3 * COMBFILTER_MINPERIOD, &pitch_index, 0);
         if (tr_on)
            tr_pitch_search(N, COMBFILTER_MAXPERIOD - 3 * COMBFILTER_MINPERIOD, pitch_index);
         pitch_index = COMBFILTER_MAXPERIOD - pitch_index;
         int t0 = pitch_index;
         gain1 = remove_doubling(pitch_buf, COMBFILTER_MAXPERIOD, COMBFILTER_MINPERIOD, N, &pitch_index,
                                 st->prefilter_period, st->prefilter_gain);
         if (tr_on)
            tr_remove_doubling(COMBFILTER_MAXPERIOD, COMBFILTER_MINPERIOD, N, t0, st->prefilter_period,
                               st->prefilter_gain, pitch_index, gain1);
         if (pitch_index > COMBFILTER_MAXPERIOD - 2)
            pitch_index = COMBFILTER_MAXPERIOD - 2;
         gain1 = MULT16_16_Q15(QCONST16(.7f, 15), gain1);
      }
      opus_val16 pf_threshold = QCONST16(.2f, 15);
      if (abs(pitch_index - st->prefilter_period) * 10 > pitch_index)
         pf_threshold += QCONST16(.2f, 15);
      if (nbAvailableBytes < 25)
         pf_threshold += QCONST16(.1f, 15);
      if (nbAvailableBytes < 35)
         pf_threshold += QCONST16(.1f, 15);
      if (st->prefilter_gain > QCONST16(.4f, 15))
         pf_threshold -= QCONST16(.1f, 15);
      if (st->prefilter_gain > QCONST16(.55f, 15))
         pf_threshold -= QCONST16(.1f, 15);
      pf_threshold = MAX16(pf_threshold, QCONST16(.2f, 15));
      if (gain1 < pf_threshold) {
         pf_on = 0;
      } else {
         if (ABS16(gain1 - st->prefilter_gain) < QCONST16(.1f, 15))
            gain1 = st->prefilter_gain;
         qg = ((gain1 + 1536) >> 10) / 3 - 1;
         qg = IMAX(0, IMIN(7, qg));
         gain1q = QCONST16(0.09375f, 15) * (qg + 1);
         pf_on = 1;
      }
      st->prefilter_period = IMAX(st->prefilter_period, COMBFILTER_MINPERIOD);
      for (c = 0; c < C; c++) {
         OPUS_COPY(in + c * (N + overlap), st->in_mem + c * overlap, overlap);
         comb_filter(in + c * (N + overlap) + overlap, pre[c] + COMBFILTER_MAXPERIOD, st->prefilter_period,
                     pitch_index, N, -st->prefilter_gain, -gain1q, 0, 0, mode->window, overlap);
         if (tr_on)
            tr_comb_filter(in + c * (N + overlap) + overlap, st->prefilter_period, pitch_index, N,
                           -st->prefilter_gain, -gain1q, overlap);
         OPUS_COPY(st->in_mem + c * overlap, in + c * (N + overlap) + N, overlap);
         OPUS_MOVE(st->prefilter_mem + c * COMBFILTER_MAXPERIOD, st->prefilter_mem + c * COMBFILTER_MAXPERIOD + N,
                   COMBFILTER_MAXPERIOD - N);
         OPUS_MOVE(st->prefilter_mem + c * COMBFILTER_MAXPERIOD + COMBFILTER_MAXPERIOD - N,
                   pre[c] + COMBFILTER_MAXPERIOD, N);
      }
      if (!pf_on) {
         if (tell + 16 <= total_bits) {
            ec_enc_bit_logp(&enc, 0, 1);
            if (tr_on)
               tr_ec_bit_logp(0, 1);
         }
      } else {
         int octave;
         ec_enc_bit_logp(&enc, 1, 1);
         if (tr_on)
            tr_ec_bit_logp(1, 1);
         pitch_index += 1;
         octave = EC_ILOG(pitch_index) - 5;
         ec_enc_uint(&enc, octave, 6);
         if (tr_on)
            tr_ec_uint(octave, 6);
         ec_enc_bits(&enc, pitch_index - (16 << octave), 4 + octave);
         if (tr_on)
            tr_ec_bits(pitch_index - (16 << octave), 4 + octave);
         pitch_index -= 1;
         ec_enc_bits(&enc, qg, 3);
         if (tr_on)
            tr_ec_bits(qg, 3);
      }
   }
   int tell_pf = ec_tell(&enc);

   /* Long MDCT, top-band cut, headroom EQ, band energies. No time-domain
    * transient_analysis: isTransient only comes from patch_transient_decision. */
   static celt_sig freq[2 * 864];
   static celt_ener bandE[2 * 22];
   opus_val16 bandLogE[2 * 22], bandLogE2[2 * 22];
   const ssc_mdct_lookup *ml = ssc_mdct_init();
   const int end0 = st->end;
   for (c = 0; c < C; c++) {
      ssc_mdct_forward(ml, in + c * (N + overlap), freq + c * N, overlap, 0, 1);
      if (tr_on)
         tr_mdct(freq + c * N, overlap, 0, 1);
   }
   int cut = bitrate < 192000 && eBands[end0 - 1] < 108;
   if (cut)
      for (c = 0; c < C; c++)
         for (i = 8 * eBands[end0 - 1]; i < N; i++)
            freq[c * N + i] = 0;
   if (uhq) {
      ref_uhq_headroom(freq, bitrate, M, eBands);
      if (tr_on)
         tr_headroom("uhq_headroom", freq, bitrate, M, 864);
   } else {
      ref_normal_headroom(freq, bitrate, M, eBands, N, e->t->headroom_gain);
      if (tr_on)
         tr_headroom("normal_headroom", freq, bitrate, M, N);
   }
   compute_band_energies(mode, freq, bandE, end0, C, M);
   if (tr_on)
      tr_band_energies(bandE, end0);
   for (i = 0; i < C * nbEBands; i++)
      if (bandE[i] >= 1 << 29) { /* amp2Log2's SHL32(bandE, 2) overflows (OD20) */
         ssc_enc_stats.bandE_2p29++;
         break;
      }
   amp2Log2(mode, end0, end0, bandE, bandLogE, C);
   if (tr_on)
      tr_amp2Log2(end0, bandLogE);
   OPUS_COPY(bandLogE2, bandLogE, C * nbEBands);

   int isTransient = 0, shortBlocks = 0;
   opus_val16 tf_estimate = 0;
   if (ec_tell(&enc) + 3 <= total_bits) {
      int r = patch_transient_decision(bandLogE, st->oldBandE, nbEBands, end0 - e->previous_uhq_index, C);
      if (tr_on)
         tr_patch_transient(bandLogE, st->oldBandE, nbEBands, end0 - e->previous_uhq_index, r);
      if (r) {
         isTransient = 1;
         shortBlocks = M;
         for (c = 0; c < C; c++)
            for (int b = 0; b < M; b++) {
               ssc_mdct_forward(ml, in + c * (N + overlap) + b * mode->shortMdctSize, freq + c * N + b, overlap, 3,
                                M);
               if (tr_on)
                  tr_mdct(freq + c * N + b, overlap, 3, M);
            }
         if (cut && !uhq)
            for (c = 0; c < C; c++)
               for (i = 8 * eBands[end0 - 1]; i < N; i++)
                  freq[c * N + i] = 0;
         if (uhq) {
            ref_uhq_headroom(freq, bitrate, M, eBands);
            if (tr_on)
               tr_headroom("uhq_headroom", freq, bitrate, M, 864);
         } else {
            ref_normal_headroom(freq, bitrate, M, eBands, N, e->t->headroom_gain);
            if (tr_on)
               tr_headroom("normal_headroom", freq, bitrate, M, N);
         }
         compute_band_energies(mode, freq, bandE, end0, C, M);
         if (tr_on)
            tr_band_energies(bandE, end0);
         amp2Log2(mode, end0, end0, bandE, bandLogE, C);
         if (tr_on)
            tr_amp2Log2(end0, bandLogE);
         for (i = 0; i < C * nbEBands; i++)
            bandLogE2[i] += HALF16(SHL16(LM, DB_SHIFT));
         tf_estimate = QCONST16(.2f, 14);
      }
   }
   if (ec_tell(&enc) + 3 <= total_bits) {
      ec_enc_bit_logp(&enc, isTransient, 3);
      if (tr_on)
         tr_ec_bit_logp(isTransient, 3);
   }

   /* Adaptive top band (F34.3, F35, F36.1): idx bands are dropped from the end; the scan
    * result goes into header bytes 2-3. */
   int idx = 0;
   {
      opus_val32 E[16];
#define SUM(a, b) ((opus_int32)((opus_uint32)E[a] + (opus_uint32)E[b])) /* 32-bit wrap */
      if (!uhq) {
         ssc_lastband_energies(mode, freq, E, 4, 0, 0);
         if (tr_on)
            tr_lastband(E, 4, 0, 0);
         out[2] += SUM(3, 7) > 30001 ? 12 : SUM(2, 6) > 30001 ? 8 : SUM(1, 5) > 30001 ? 4 : 0;
      } else if (tag == 0xe6) {
         idx = 4;
      } else if (e_hdr) {
         int code = 0;
         for (idx = 0; idx < 4 && !code; idx++) {
            ssc_lastband_energies(mode, freq, E, 8, 1, idx);
            if (tr_on)
               tr_lastband(E, 8, 1, idx);
            for (int s = 7; s >= 0 && !code; s--)
               if (SUM(s, 8 + s) > 12001)
                  code = s + 1;
         }
         idx = code ? idx - 1 : 3;
         out[2] += idx * 4;
         out[3] = (unsigned char)((code ? (code - 1) << 5 : 0) | (out[3] & 0x1f));
      }
#undef SUM
   }
   const int end = end0 - idx;

   /* The blob normalises in place (X aliases freq); a separate buffer gives the same
    * values. quant_all_bands may use up to 4*N entries. */
   static celt_norm X[4 * 864 + 64];
   normalise_bands(mode, freq, X, bandE, end, C, M);
   if (tr_on)
      tr_normalise(X, eBands, end);

   /* TF: v1.1's tf_analysis in F mode (tf_chan 0, tf_estimate 0 or 0.2), then Samsung
    * forces bands 0 and 1 to the block type. E mode codes no TF (F37) and keeps tf_res 0. */
   int tf_res[22], tf_select = 0, tf_sum;
   if (!uhq && effectiveBytes >= 15 * C) {
      int lambda = effectiveBytes < 40 ? 12 : effectiveBytes < 60 ? 6 : effectiveBytes < 100 ? 4 : 3;
      tf_select = tf_analysis(mode, end0, isTransient, tf_res, 2 * lambda, X, N, LM, &tf_sum, tf_estimate, 0);
      tf_res[0] = tf_res[1] = isTransient;
   } else {
      for (i = 0; i < end0; i++)
         tf_res[i] = isTransient;
   }

   opus_val16 error[2 * 22];
   if (tr_on)
      tr_coarse_in(end0, end, bandLogE, st->oldBandE, total_bits, nbAvailableBytes, st->force_intra,
                   st->delayedIntra);
   quant_coarse_energy(mode, 0, end, end, bandLogE, st->oldBandE, total_bits, error, &enc, C, LM, nbAvailableBytes,
                       st->force_intra, &st->delayedIntra, 1, 0, 0);
   if (tr_on)
      tr_coarse_out(end, st->oldBandE, error, st->delayedIntra);

   if (!uhq) {
      ssc_tf_encode(0, end0, isTransient, tf_res, LM, tf_select, &enc);
   } else {
      for (i = 0; i < end0; i++)
         tf_res[i] = 0;
   }

   if (ec_tell(&enc) + 4 <= total_bits) {
      if (!isTransient && nbAvailableBytes >= 10 * C) {
         if (tr_on)
            tr_spread_in(pf_on, end, st->spread_decision, st->tonal_average, st->hf_average, st->tapset_decision);
         st->spread_decision = spreading_decision(mode, X, &st->tonal_average, st->spread_decision, &st->hf_average,
                                                  &st->tapset_decision, pf_on, end, C, M);
         if (tr_on)
            tr_spread_out(st->spread_decision, st->tonal_average, st->hf_average, st->tapset_decision);
      } else
         st->spread_decision = SPREAD_NORMAL;
      ec_enc_icdf(&enc, st->spread_decision, spread_icdf, 5);
      if (tr_on)
         tr_ec_icdf(st->spread_decision, spread_icdf, 5);
   }

   int offsets[22], cap[22];
   ssc_dynalloc_analysis(bandLogE, bandLogE2, nbEBands, end, C, offsets, mode->logN, isTransient, eBands, LM,
                         effectiveBytes);
   init_caps(mode, cap, LM, C);
   int dynalloc_logp = 6;
   opus_int32 total_boost = 0;
   total_bits <<= BITRES;
   tell = ec_tell_frac(&enc);
   for (i = 0; i < end; i++) {
      int width, quanta, dynalloc_loop_logp, boost, j;
      width = C * (eBands[i + 1] - eBands[i]) << LM;
      quanta = IMIN(width << BITRES, IMAX(6 << BITRES, width));
      dynalloc_loop_logp = dynalloc_logp;
      boost = 0;
      for (j = 0; tell + (dynalloc_loop_logp << BITRES) < total_bits - total_boost && boost < cap[i]; j++) {
         int flag = j < offsets[i];
         ec_enc_bit_logp(&enc, flag, dynalloc_loop_logp);
         if (tr_on)
            tr_ec_bit_logp(flag, dynalloc_loop_logp);
         tell = ec_tell_frac(&enc);
         if (!flag)
            break;
         boost += quanta;
         total_boost += quanta;
         dynalloc_loop_logp = 1;
      }
      if (j)
         dynalloc_logp = IMAX(2, dynalloc_logp - 1);
      offsets[i] = boost;
   }
   /* Samsung: a large boost in band 18, 19 or 20 carries over to the next band when that
    * band got none and its (quantised) energy is high. Not signalled: the decoder can
    * redo it from the decoded offsets and energies. */
   {
      const opus_val16 *q = st->oldBandE;
      int o19 = offsets[19], o20 = offsets[20], o21 = offsets[21];
      if (offsets[18] > 1000 && offsets[19] == 0) {
         if (q[19] > 7000 || q[18] >> 1 < q[19])
            offsets[19] = offsets[18];
      } else if (offsets[19] > 1000 && offsets[20] == 0) {
         if (q[20] > 7000 || q[19] >> 1 < q[20])
            offsets[20] = offsets[19];
      } else if (offsets[20] > 1000 && offsets[21] == 0) {
         if (q[21] > 7000 || q[20] >> 1 < q[21])
            offsets[21] = offsets[20];
      }
      ssc_enc_stats.carry += o19 != offsets[19] || o20 != offsets[20] || o21 != offsets[21];
   }

   int dual_stereo = stereo_analysis(mode, X, LM, N);
   {
      /* hysteresis_decision(510, Samsung's thresholds, 22 - idx, prev): every threshold
       * plus its hysteresis is below 510, so the result does not depend on the tables. */
      int n = nbEBands - idx, prev = st->intensity, r = prev > n ? prev : n;
      if (tr_on)
         tr_hysteresis(510, n, prev, r);
      st->intensity = IMIN(end, IMAX(0, r));
   }

   int alloc_trim = 5;
   if (tell + (6 << BITRES) <= total_bits - total_boost) {
      AnalysisInfo analysis = {0};
      alloc_trim = alloc_trim_analysis(mode, X, bandLogE, end, LM, C, N, &analysis, &e->stereo_saving,
                                       tf_estimate, st->intensity, 0);
      ec_enc_icdf(&enc, alloc_trim, ssc_trim_icdf, 7);
      if (tr_on)
         tr_ec_icdf(alloc_trim, ssc_trim_icdf, 7);
      tell = ec_tell_frac(&enc);
   }

   int fine_quant[22], pulses[22], fine_priority[22], balance, codedBands;
   opus_int32 bits = (((opus_int32)nbCompressedBytes * 8) << BITRES) - ec_tell_frac(&enc) - 1;
   int anti_collapse_rsv = isTransient && bits >= ((LM + 2) << BITRES) ? (1 << BITRES) : 0;
   bits -= anti_collapse_rsv;
   {
      CELTMode am = *mode;
      int sel = bitrate > 192000;
      if (!sel)
         am.allocVectors = e->t->alloc_shared[mi];
      if (tr_on)
         tr_alloc_in(end, end - 1, bits, offsets, alloc_trim, dual_stereo, sel, st->lastCodedBands, st->intensity);
      codedBands = compute_allocation(&am, 0, end, offsets, cap, alloc_trim, &st->intensity, &dual_stereo, bits,
                                      &balance, pulses, fine_quant, fine_priority, C, LM, &enc, 1,
                                      st->lastCodedBands, end - 1);
      if (tr_on)
         tr_alloc_out(end, codedBands, fine_quant, pulses, fine_priority, balance, dual_stereo, st->intensity);
   }
   if (st->lastCodedBands)
      st->lastCodedBands = IMIN(st->lastCodedBands + 1, IMAX(st->lastCodedBands - 1, codedBands));
   else
      st->lastCodedBands = codedBands;

   quant_fine_energy(mode, 0, end, st->oldBandE, error, fine_quant, &enc, C);
   if (tr_on)
      tr_fine(end, fine_quant, st->oldBandE, error);

   unsigned char collapse_masks[2 * 22];
   if (tr_on)
      tr_qab_in(eBands, end, X, bandE, pulses, shortBlocks, st->spread_decision, dual_stereo, st->intensity,
                tf_res, nbCompressedBytes * (8 << BITRES) - anti_collapse_rsv, balance, codedBands, st->rng, 1);
   ssc_vq_scratch = (unsigned char *)(in + SSC_VQ_IN); /* alg_quant's buffers alias in[] (patch 0012) */
   quant_all_bands(1, mode, 0, end, X, X + N, collapse_masks, bandE, pulses, shortBlocks, st->spread_decision,
                   dual_stereo, st->intensity, tf_res, nbCompressedBytes * (8 << BITRES) - anti_collapse_rsv,
                   balance, &enc, LM, codedBands, &st->rng);
   ssc_vq_scratch = NULL;
   if (tr_on)
      tr_qab_out(end, collapse_masks, st->rng);
   if (anti_collapse_rsv > 0) {
      ec_enc_bits(&enc, st->consec_transient < 2, 1);
      if (tr_on)
         tr_ec_bits(st->consec_transient < 2, 1);
   }
   int bits_left = nbCompressedBytes * 8 - ec_tell(&enc);
   quant_energy_finalise(mode, 0, end, st->oldBandE, error, fine_quant, fine_priority, bits_left, &enc, C);
   if (tr_on)
      tr_finalise(end, bits_left, st->oldBandE);

   if (silence)
      for (i = 0; i < C * nbEBands; i++)
         st->oldBandE[i] = -QCONST16(28.f, DB_SHIFT);
   /* Samsung: the dropped top bands restart from -28 dB. */
   for (i = nbEBands - idx; i < nbEBands; i++)
      st->oldBandE[i] = st->oldBandE[nbEBands + i] = -QCONST16(28.f, DB_SHIFT);
   ssc_enc_stats.frames++;
   ssc_enc_stats.pf_on += pf_on;
   ssc_enc_stats.transient += isTransient;
   ssc_enc_stats.silence += silence;
   ssc_enc_stats.cut += cut;
   ssc_enc_stats.idx += idx > 0;
   ssc_enc_stats.anti_collapse += anti_collapse_rsv > 0;
   ssc_enc_stats.tf_select += tf_select;
   e->previous_uhq_index = idx;
   st->prefilter_period = pitch_index;
   st->prefilter_gain = gain1q;
   /* v1.1 also shifts oldLogE into oldLogE2; the blob never touches oldLogE2. */
   if (!isTransient) {
      for (i = 0; i < C * nbEBands; i++)
         st->oldLogE[i] = st->oldBandE[i];
      if (tell_pf + 3 <= nbCompressedBytes * 8)
         st->consec_transient = 0;
      else
         st->consec_transient++;
   } else {
      for (i = 0; i < C * nbEBands; i++)
         st->oldLogE[i] = MIN16(st->oldLogE[i], st->oldBandE[i]);
      st->consec_transient++;
   }
   st->rng = enc.rng;
   ec_enc_done(&enc);
   if (tr_on)
      tr_ec_done(&enc);
   st->frame_count = st->frame_count != 0xff ? st->frame_count + 1 : 0;
   return ec_get_error(&enc) ? -3 : nbCompressedBytes + 4;
}
#endif /* HAVE_SSC_MDCT */
