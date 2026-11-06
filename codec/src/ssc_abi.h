/* The blob's data layouts that the harness reads or builds: the static CELTMode, the
 * frame struct compute_allocation takes, and the encoder handle. Offsets come from the
 * disassembly (ssc-uhq F28.2, F33, F44, T4.8); the asserts pin them. */
#ifndef SSC_ABI_H
#define SSC_ABI_H

#include <stdint.h>

/* Samsung's CELTMode layout (F28.2, resolved in F33). The blob's static modes are read
 * at runtime and turned into a libopus CELTMode for the reference, so no Samsung table
 * ends up in this source (D28). */
struct ssc_mode {
   int32_t Fs, overlap;              /* +0x00 */
   int32_t nbEBands, effEBands;      /* +0x08 */
   const int16_t *eBands;            /* +0x10 */
   int32_t nbShortMdcts, shortMdctSize; /* +0x18 */
   int32_t nbAllocVectors, pad;      /* +0x20 */
   const unsigned char *allocVectors;   /* +0x28: shared table */
   const unsigned char *allocVectorsMode; /* +0x30: per-mode table */
   const int16_t *logN;              /* +0x38 */
   int32_t mdct_n, mdct_maxshift;    /* +0x40 */
   const void *kfft[2];              /* +0x48: shift 0 and shift 3 */
   const int32_t *trig;              /* +0x58: Q31 */
   int32_t cache_size, pad2;         /* +0x60 */
   const int16_t *cache_index;       /* +0x68 */
   const unsigned char *cache_bits;  /* +0x70 */
   const unsigned char *cache_caps;  /* +0x78 */
};

/* Samsung's compute_allocation(st, alloc, enc, end, signalBandwidth) packs libopus'
 * arguments into a frame struct (F33). C=2, LM=3, start=0, encode=1. */
struct ssc_alloc {
   int32_t total;             /* +0x000 */
   int32_t ebits[22];         /* +0x004 */
   int32_t bits[22];          /* +0x05c: pulses */
   int32_t pad0;
   const int32_t *cap;        /* +0x0b8 */
   int32_t offsets[22];       /* +0x0c0 */
   int32_t fine_priority[22]; /* +0x118 */
   int32_t alloc_trim;        /* +0x170 */
   int32_t dual_stereo;       /* +0x174 */
   int32_t balance;           /* +0x178 */
   int32_t pad1;
   int32_t alloc_sel;         /* +0x180: 0 = mode +0x28 (shared), else +0x30 (per-mode) */
};
_Static_assert(__builtin_offsetof(struct ssc_alloc, cap) == 0xb8, "alloc.cap");
_Static_assert(__builtin_offsetof(struct ssc_alloc, alloc_sel) == 0x180, "alloc.sel");

/* The encoder fields compute_allocation reads: +0x00 mode, +0x2c lastCodedBands (prev),
 * +0x50 intensity (in/out). */
struct ssc_alloc_st {
   const struct ssc_mode *mode;
   int32_t pad[9];
   int32_t prev;              /* +0x2c */
   int32_t pad2[8];
   int32_t intensity;         /* +0x50 */
};
_Static_assert(__builtin_offsetof(struct ssc_alloc_st, prev) == 0x2c, "st.prev");
_Static_assert(__builtin_offsetof(struct ssc_alloc_st, intensity) == 0x50, "st.intensity");

/* Samsung: spreading_decision(st, X, update_hf, end) with C=2, M=8, and the state it
 * keeps in the encoder struct: +0x00 mode, +0x20 last decision, +0x28 average,
 * +0x30 hf_average, +0x34 tapset_decision. Logic changes are patch 0005. */
struct ssc_spread_st {
   const struct ssc_mode *mode;
   int32_t pad[6];
   int32_t last_decision;  /* +0x20 */
   int32_t pad2;
   int32_t average;        /* +0x28 */
   int32_t pad3;
   int32_t hf_average;     /* +0x30 */
   int32_t tapset;         /* +0x34 */
};

/* The encoder handle (ssc_encoder_init 0x9f54, ssc_encode), v1.1's OpusCustomEncoder
 * trimmed and extended. ssc_encoder_get_size() is (6*nbEBands + 4*overlap + 4096)*C +
 * 0xdc; the arrays start at +0xe0, so for C=2 the last 4 bytes of oldLogE2 lie past
 * that size (inside malloc's rounding). The fields the blob never writes after init
 * are named unk*. */
struct ssc_enc_state {
   const void *mode;          /* +0x00 */
   int32_t overlap;           /* +0x08 */
   int32_t channels;          /* +0x0c */
   int32_t force_intra;       /* +0x10 */
   int32_t end;               /* +0x14: nbEBands, rewritten every frame */
   int32_t bitrate;           /* +0x18: -1 (OPUS_BITRATE_MAX), never set */
   uint32_t rng;              /* +0x1c */
   int32_t spread_decision;   /* +0x20 */
   int32_t delayedIntra;      /* +0x24 */
   int32_t tonal_average;     /* +0x28 */
   int32_t lastCodedBands;    /* +0x2c */
   int32_t hf_average;        /* +0x30 */
   int32_t tapset_decision;   /* +0x34 */
   int32_t prefilter_period;  /* +0x38 */
   int16_t prefilter_gain;    /* +0x3c */
   int16_t unk3e;
   int32_t consec_transient;  /* +0x40 */
   int32_t unk44, unk48;
   int32_t overlap_max;       /* +0x4c */
   int32_t intensity;         /* +0x50 */
   int32_t unk54[3];
   int32_t Fs;                /* +0x60: the create rate, 44100 when created at 48000 */
   int32_t create_rate;       /* +0x64 */
   int32_t frame_bitrate;     /* +0x68: ssc_encode's bitrate argument */
   int32_t bits;              /* +0x6c: ssc_encoder_create's bits */
   int32_t unk70[7];
   uint16_t frame_count;      /* +0x8c: header nibbles, wraps at 0xff */
   uint16_t hdr2_low;         /* +0x8e: header byte 2 bits 0-1, 1 */
   int64_t biquad[10];        /* +0x90: DRC narrow-band filter (F44.1) */
   int32_t in_mem[2 * 108];   /* +0xe0 */
   int32_t prefilter_mem[2 * 1024];
   int16_t oldBandE[2 * 22];
   int16_t oldLogE[2 * 22];
   int16_t oldLogE2[2 * 22];
};
_Static_assert(__builtin_offsetof(struct ssc_enc_state, overlap_max) == 0x4c, "st.overlap_max");
_Static_assert(__builtin_offsetof(struct ssc_enc_state, Fs) == 0x60, "st.Fs");
_Static_assert(__builtin_offsetof(struct ssc_enc_state, frame_count) == 0x8c, "st.frame_count");
_Static_assert(__builtin_offsetof(struct ssc_enc_state, in_mem) == 0xe0, "st.in_mem");
_Static_assert(sizeof(struct ssc_enc_state) == 0x2548, "st size");

#endif
