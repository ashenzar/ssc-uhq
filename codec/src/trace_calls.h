/* One record per call ssc_encode makes, in the blob's argument order. The blob's wrappers
 * (trace.c) and the reference encoder (ssc_encoder.c) both record through these, so a
 * difference between the traces is a difference in values, never in formatting.
 * Arrays the blob leaves undefined (error[] past end, X past eBands[end]) are cut. */
#ifndef TRACE_CALLS_H
#define TRACE_CALLS_H

#include "entcode.h"
#include "trace.h"

static inline void tr_ec_init(uint32_t size) { tr_begin("ec_init"); tr_int("size", size); tr_end(); }
static inline void tr_ec_shrink(uint32_t size) { tr_begin("ec_shrink"); tr_int("size", size); tr_end(); }
static inline void tr_ec_bit_logp(int val, unsigned logp)
{
   tr_begin("bit_logp"); tr_int("v", val); tr_int("logp", logp); tr_end();
}
static inline void tr_ec_icdf(int s, const unsigned char *icdf, unsigned ftb)
{
   tr_begin("icdf"); tr_int("s", s); tr_int("ftb", ftb); tr_int("icdf0", icdf[0]); tr_end();
}
static inline void tr_ec_uint(uint32_t fl, uint32_t ft) { tr_begin("uint"); tr_int("fl", fl); tr_int("ft", ft); tr_end(); }
static inline void tr_ec_bits(uint32_t fl, unsigned bits) { tr_begin("bits"); tr_int("fl", fl); tr_int("n", bits); tr_end(); }
static inline void tr_ec_done(const ec_enc *e)
{
   tr_begin("ec_done"); tr_int("tell", ec_tell((ec_enc *)e)); tr_int("rng", e->rng); tr_int("offs", e->offs);
   tr_int("end_offs", e->end_offs); tr_int("err", e->error); tr_end();
}

static inline void tr_drc_limit(int thr) { tr_begin("set_DRC_limit_gain"); tr_int("thr", thr); tr_end(); }
static inline void tr_powercalc(int n, int32_t r) { tr_begin("powercalc"); tr_int("n", n); tr_int("p", r); tr_end(); }
static inline void tr_limit_gain(int32_t p, int32_t t, int32_t br, int32_t fs, int32_t r)
{
   tr_begin("CalculateLimitGain"); tr_int("p", p); tr_int("t", t); tr_int("br", br); tr_int("fs", fs);
   tr_int("ret", r); tr_end();
}
static inline void tr_drc_exe(const int32_t *x, int n)
{
   tr_begin("SSC_DRC_Exe"); tr_int("n", n); tr_a32("x", x, 2 * n, 1); tr_end();
}

static inline void tr_pitch_downsample(const int16_t *x_lp, int len)
{
   tr_begin("pitch_downsample"); tr_int("len", len); tr_a16("x_lp", x_lp, len >> 1); tr_end();
}
static inline void tr_pitch_search(int len, int max_pitch, int pitch)
{
   tr_begin("pitch_search"); tr_int("len", len); tr_int("max", max_pitch); tr_int("pitch", pitch); tr_end();
}
static inline void tr_remove_doubling(int maxp, int minp, int N, int t0, int prev_period, int prev_gain,
                                      int t0_out, int r)
{
   tr_begin("remove_doubling"); tr_int("maxp", maxp); tr_int("minp", minp); tr_int("N", N); tr_int("T0", t0);
   tr_int("prev_period", prev_period); tr_int("prev_gain", prev_gain); tr_int("T0_out", t0_out);
   tr_int("ret", r); tr_end();
}
static inline void tr_comb_filter(const int32_t *y, int T0, int T1, int N, int g0, int g1, int overlap)
{
   tr_begin("comb_filter"); tr_int("T0", T0); tr_int("T1", T1); tr_int("N", N); tr_int("g0", g0);
   tr_int("g1", g1); tr_int("overlap", overlap); tr_a32("y", y, N, 1); tr_end();
}
static inline void tr_mdct(const int32_t *out, int overlap, int shift, int stride)
{
   tr_begin("mdct_forward"); tr_int("overlap", overlap); tr_int("shift", shift); tr_int("stride", stride);
   tr_a32("out", out, 864 >> shift, stride); tr_end();
}
static inline void tr_headroom(const char *name, const int32_t *X, int bitrate, int M, int stride)
{
   tr_begin(name); tr_int("br", bitrate); tr_int("M", M); tr_int("stride", stride);
   tr_a32("X0", X, 864, 1); tr_a32("X1", X + stride, 864, 1); tr_end();
}
static inline void tr_band_energies(const int32_t *bandE, int end)
{
   tr_begin("band_energies"); tr_int("end", end); tr_a32("E0", bandE, end, 1); tr_a32("E1", bandE + 22, end, 1);
   tr_end();
}
static inline void tr_amp2Log2(int n, const int16_t *bandLogE)
{
   tr_begin("amp2Log2"); tr_int("n", n); tr_a16("logE", bandLogE, 44); tr_end();
}
static inline void tr_patch_transient(const int16_t *newE, const int16_t *oldE, int nb, int end, int r)
{
   tr_begin("patch_transient"); tr_int("nb", nb); tr_int("end", end); tr_a16("newE", newE, 44);
   tr_a16("oldE", oldE, 44); tr_int("ret", r); tr_end();
}
static inline void tr_lastband(const int32_t *bandE, int count, int uhq, int idx)
{
   tr_begin("lastband"); tr_int("count", count); tr_int("uhq", uhq); tr_int("idx", idx);
   tr_a32("E", bandE, 2 * count, 1); tr_end();
}
static inline void tr_normalise(const int16_t *X, const int16_t *eBands, int end)
{
   tr_begin("normalise_bands"); tr_int("end", end); tr_a16("X0", X, 8 * eBands[end]);
   tr_a16("X1", X + 864, 8 * eBands[end]); tr_end();
}

static inline void tr_coarse_in(int a, int end, const int16_t *E, const int16_t *oldE, uint32_t budget,
                                int nbAvail, int force_intra, int32_t delayedIntra)
{
   tr_begin("coarse"); tr_int("a", a); tr_int("end", end); tr_int("budget", budget); tr_int("nbAvail", nbAvail);
   tr_int("force_intra", force_intra); tr_int("delayedIntra", delayedIntra); tr_a16("E", E, 44);
   tr_a16("oldE", oldE, 44);
}
static inline void tr_coarse_out(int end, const int16_t *oldE, const int16_t *error, int32_t delayedIntra)
{
   tr_a16("oldE_out", oldE, 44); tr_a16("err0", error, end); tr_a16("err1", error + 22, end);
   tr_int("delayedIntra_out", delayedIntra); tr_end();
}
static inline void tr_spread_in(int update_hf, int end, int last, int avg, int hf, int tapset)
{
   tr_begin("spreading_decision"); tr_int("update_hf", update_hf); tr_int("end", end); tr_int("last", last);
   tr_int("avg", avg); tr_int("hf", hf); tr_int("tapset", tapset);
}
static inline void tr_spread_out(int r, int avg, int hf, int tapset)
{
   tr_int("ret", r); tr_int("avg_out", avg); tr_int("hf_out", hf); tr_int("tapset_out", tapset); tr_end();
}
static inline void tr_hysteresis(int val, int N, int prev, int r)
{
   tr_begin("hysteresis_decision"); tr_int("val", val); tr_int("N", N); tr_int("prev", prev); tr_int("ret", r);
   tr_end();
}
static inline void tr_alloc_in(int end, int sbw, int total, const int32_t *offsets, int trim, int dual, int sel,
                               int prev, int intensity)
{
   tr_begin("compute_allocation"); tr_int("end", end); tr_int("sbw", sbw); tr_int("total", total);
   tr_a32("offsets", offsets, end, 1); tr_int("trim", trim); tr_int("dual", dual); tr_int("sel", sel);
   tr_int("prev", prev); tr_int("intensity", intensity);
}
static inline void tr_alloc_out(int end, int r, const int32_t *ebits, const int32_t *bits, const int32_t *fp,
                                int balance, int dual, int intensity)
{
   tr_int("ret", r); tr_a32("ebits", ebits, end, 1); tr_a32("pulses", bits, end, 1); tr_a32("fine_prio", fp, end, 1);
   tr_int("balance", balance); tr_int("dual_out", dual); tr_int("intensity_out", intensity); tr_end();
}
static inline void tr_fine(int end, const int *fq, const int16_t *oldE, const int16_t *error)
{
   tr_begin("fine"); tr_int("end", end); tr_a32("fq", (const int32_t *)fq, end, 1); tr_a16("oldE", oldE, 44);
   tr_a16("err0", error, end); tr_a16("err1", error + 22, end); tr_end();
}
static inline void tr_qab_in(const int16_t *eBands, int end, const int16_t *X, const int32_t *bandE,
                             const int *pulses, int shortBlocks, int spread, int dual, int intensity,
                             const int *tf_res, int32_t total, int32_t balance, int codedBands, uint32_t seed, int x)
{
   tr_begin("quant_all_bands"); tr_int("end", end); tr_int("B", shortBlocks); tr_int("spread", spread);
   tr_int("dual", dual); tr_int("intensity", intensity); tr_int("total", total); tr_int("balance", balance);
   tr_int("coded", codedBands); tr_int("seed", seed); tr_int("x", x);
   tr_a32("tf_res", (const int32_t *)tf_res, end, 1); tr_a32("pulses", (const int32_t *)pulses, end, 1);
   tr_a32("E0", bandE, end, 1); tr_a32("E1", bandE + 22, end, 1);
   tr_a16("X0", X, 8 * eBands[end]); tr_a16("X1", X + 864, 8 * eBands[end]);
}
static inline void tr_qab_out(int end, const unsigned char *cm, uint32_t seed)
{
   int32_t m[44];
   for (int i = 0; i < 2 * end; i++)
      m[i] = cm[i];
   tr_a32("cm", m, 2 * end, 1); tr_int("seed_out", seed); tr_end();
}
static inline void tr_finalise(int end, int bits_left, const int16_t *oldE)
{
   tr_begin("finalise"); tr_int("end", end); tr_int("bits_left", bits_left); tr_a16("oldE", oldE, 44); tr_end();
}

#endif
