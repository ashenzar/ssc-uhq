# Phase 4 — Native SSC codec rebuild

Status legend: [ ] todo, [~] in progress, [x] done, [-] dropped (with reason)

## Mission

After Phases 1–3, the best configuration (UHQ-584, 96 kHz source, 96 kHz graph, no
resampling, S26U/S24U encoder) still has audible noise and artifacts (user report,
2026-10-26). This contradicts the earlier F21 result ("excellent"). Nothing left in the
driving logic, rate path or transport explains it, and the encoder is a black box under
qemu. We cannot measure what it does because there is no SSC decoder.

Phase 4 rebuilds the codec natively from the decompiled blob. The blob is a fork of
fixed-point CELT from libopus (F27), so most of the codec is already open source. The
reverse-engineering work is Samsung's changes on top of it.

**Caveat:** a bit-exact rebuild produces the same bytes as the blob, so on its own it sounds
the same. The rebuild pays off in three ways:
1. A **decoder**, so for the first time we can measure where the noise comes from.
2. **Native execution**, which removes qemu, its timing risk and its CPU cost.
3. An **encoder we control**. CELT's decoder follows whatever allocation, trim, skip and
   spreading choices the bitstream signals, so encoder-side improvements can stay
   decodable by the buds' firmware. This is how libopus improved quality over the years
   without changing its bitstream.

## Progress (2026-10-26, end of batch 2)
~52% of the encoder's live code bytes are verified bit-exact (31 difftest tests, patches
0001–0008). The phase is ~25–30% done, weighted by objective (F33, last paragraph).

## Objectives

**O0. Rule out the cheap explanations first (D32)**
- [ ] T0.1 qemu deadline: time `ssc_encode` per frame under the helper at UHQ-584 (budget
      9.0 ms per 864-sample frame at 96 kHz). Log p50/p99/max over 5 min of music.
- [ ] T0.2 Transport: watch `pw-top` ERR/xruns and the BT sink's dropped or late packet
      logs during 96k playback. Repeat with Wi-Fi off, because the AX201 combo chip shares
      its antenna between Wi-Fi and BT.
- [ ] T0.3 Classify the artifact: periodic clicks or dropouts point to timing (fixed by
      O5). A steady noise floor, swishiness or pre-echo points to the codec (O3/O6).

**O1. Fork base and delta map**
- [x] T1.1 Identify lineage: fixed-point CELT from libopus with an `ssc_` prefix (F27).
- [x] T1.2 Pin the libopus version: **v1.1** (range 1.1-rc … 1.1 final; these can't be
      told apart in this binary). See F29. Clone at `~/src/opus`.
- [~] T1.3 (F30 first pass; F31/F33 settle every "?" row except fft/mdct) Classify every exported function as **stock**, **modified** or
      **Samsung-only**, with evidence. Expected Samsung-only: DRC (`SSC_DRC_*`,
      `CalculateLimitGain`, `Biquad_Narrow_int2int_stereo`, `powercalc`), headroom
      (`normal_headroom`, `uhq_headroom`, `headroom_process`, `headroom_gain`), framing
      (`get_header_from_bitrate`, `set_data_header`, `set_data_frame_header`,
      `get_uhq_info_from_bitrate`, `set_limit_gain_from_bitrate`), 22nd band
      (`*_lastband*`, `last_band_info_calc`), `tone_detect`, pitch/prefilter gain helpers.
- [x] T1.4 *(F33.1: +0x28/+0x30 alloc tables, +0x38 logN, +0x58 Q31 trig, int32 caps; window is a Q31 global)* Decode the remaining `CELTMode` fields for both modes (alloc vectors, logN,
      window, MDCT twiddles, PulseCache) and confirm the struct layout against libopus.

**O2. Differential test harness (D31)**
- [x] T2.1 *(done as `~/src/ssc-codec/harness/difftest`, F31)* `harness/blobcall`: an aarch64 program run under qemu. It dlopens the blob and
      calls any exported function by name with inputs from a file, then dumps its
      outputs and the global state it touches.
- [ ] T2.2 Test corpus: sines (100 Hz, 1 kHz, 15 kHz, 30 kHz), log sweeps, white and pink
      noise, castanets/transients, silence, full-scale clipping, and 3 music excerpts
      (44.1k-sourced and native 96k). Encode each with the blob at basic-229, UHQ-584,
      UHQ-291 and UHQ-152 to get golden frames.
- [ ] T2.3 Per-function golden vectors captured at the function boundary. Use a qemu
      gdbstub breakpoint inside `ssc_encode` for functions only reachable mid-frame.

**O3. Decoder first (D29)**
- [ ] T3.1 Parse Samsung's frame header (`ff e0 …` for E-mode, `ff ee …` for F-mode) and
      map header fields to bitrate, mode and band info.
- [ ] T3.2 libopus CELT decoder plus Samsung's static modes (22 bands, 864-sample frame,
      overlap 108) plus 22nd-band decoding plus every bitstream change found in O1.
- [ ] T3.3 Validate: decode golden frames and compare with the source PCM (time-aligned
      SNR, spectrogram). The decoder is right when every corpus item decodes with no
      range-decoder desync (`ec_tell` matches frame length) and plausible SNR.
- [ ] T3.4 **Measure the user's artifact**: decode the 96k path, then build a per-band
      noise-to-signal map. This answers whether the noise is born in the encoder.

**O4. Bit-exact native encoder (D30)**

Bottom-up. Each step passes its per-function vectors before the next starts:
- [x] T4.1 *(bit-exact via libopus + patches 0001/0002, F31)* Range coder (`ec_enc_*`, `ec_laplace_encode`) and fixed-point math (`frac_div32`,
      `rcp`, `rsqrt_norm`, `sqrt`, `bitexact_cos`, `amp2Log2`, `smull32`).
- [ ] T4.2 *(next: Samsung rewrite, Q31 window/trig, F33.8; tables are formula-generated)* FFT/MDCT (`ssc_fft`, `mdct_forward`) with Samsung's 1728-point twiddles.
- [~] T4.3 *(band energies, normalise, amp2Log2 bit-exact; 22nd-band energies `compute_band_energies_for_lastband` and the adaptive-end loop F33.7 remain)* Band energy, normalisation and the 22nd band.
- [x] T4.4 *(coarse with 0006, fine, finalise; eMeans is stock, F33)* Energy quantisation (coarse, fine, finalise) and `ssc_eMeans_tmp`.
- [x] T4.5 *(compute_allocation F, quant_all_bands with 0007, PVQ/theta/intensity; the 48k cache quirk F33.2 is carried through)* Allocation (`compute_allocation`, alloc vectors, PulseCache) and PVQ
      (`alg_quant`, `encode_pulses`, `quant_all_bands`, `stereo_itheta`, `haar1`).
- [~] T4.6 *(pitch downsample/xcorr/remove_doubling, spreading 0005, comb filter 0008 done; transient, tf, dynalloc, pitch_search+tone_detect, patch_transient_decision remain)* Analysis: transient, tf_select, spreading, hysteresis, pitch
      (downsample/search/remove_doubling/xcorr), comb-filter prefilter.
- [ ] T4.7 Samsung pre-processing: headroom and DRC/limiter (`uhq_headroom`,
      `normal_headroom`, `SSC_DRC_Exe`, biquad).
- [ ] T4.8 Framing and the `ssc_encode` top level, then whole-frame bit-exactness on the
      full corpus × 4 modes × multi-minute runs (state carry-over).

**O5. Integrate native encoder (D4)**
- [ ] T5.1 New backend in the PipeWire SSC plugin behind `SSCENC_BACKEND=native`. The
      default stays `blob` until the listening test passes.
- [ ] T5.2 Bit-exact check in the live pipeline: tee frames from both backends and
      compare.
- [ ] T5.3 Listening test (D6: volume 10% first). Native must sound identical to blob.
      Any difference is a bug.

**O6. Bitstream-compatible quality work (only after O3 + O5)**
- [ ] T6.1 Use the decoder to rank the T3.4 noise sources.
- [ ] T6.2 Candidate encoder-side changes, each behind a flag and each measured with the
      decoder: bypass the DRC/limiter and headroom when gain would be 1.0 anyway; spend
      fewer bits above 20 kHz in the 96k mode (bands 19–21 cover 16–48 kHz, F27) via
      allocation trim, dynalloc or band skip; and newer libopus analysis (tf, spreading,
      trim, dynalloc) ported onto Samsung's mode.
- [ ] T6.3 For each change: the buds decode it without glitches (listening), the objective
      metric improves, and the user prefers it in an A/B test.

## Acceptance criteria
- **AC1** O0 concludes with evidence whether timing or transport contributes to the artifact.
- **AC2** Function classification table (T1.3) in findings.md, with every exported
  function covered.
- **AC3** Decoder decodes 100% of golden frames without desync. The 96k artifact is
  characterised with numbers (T3.4).
- **AC4** Native encoder is bit-exact with the blob on the full corpus in all four modes.
- **AC5** Native backend runs in PipeWire at under 5% of one core at UHQ-584 and sounds
  identical to blob in a listening test.
- **AC6 (stretch)** At least one O6 change measurably reduces the 96k noise and is
  preferred by the user, with no decode glitches on the buds.

## Out of scope
- Buds firmware RE (the decoder side on the buds).
- Committing Samsung code, binaries or data tables to this repo (D28).
- Upstreaming to openssc, libopus or PipeWire.

## Open decisions (as of 2026-10-26, end of O2 batch 2)

Settled this phase (logged here; promote to D-entries when convenient):
OD2 → `~/src/ssc-codec` · OD3 → libopus `v1.1` submodule + patch series `harness/patches/` ·
OD5 → Ghidra 12.2 at `~/.local/opt/ghidra` · OD6 → Vinchethescript `encoder.c` not used ·
OD11 → docs committed · OD13 → reference CELTMode built from the blob at runtime (done,
F33) · OD14 → test against stock C first, transcribe on mismatch (comb_filter needed it:
0008) · OD16 → patch series (0001–0008).

Still open:

| # | Question | Options | Recommendation |
|---|---|---|---|
| OD1 | When to run O0 (qemu deadline / Wi-Fi coexistence / artifact classification)? | before more RE / in parallel / after | **In parallel, at the next listening session.** It's cheap and needs the user on the buds. Still the only way to know whether the 96k noise is timing or codec. |
| OD4 | Implementation language and numeric model | C fixed-point / C float / Rust | **C, `FIXED_POINT`.** Now 8 patches on v1.1 and still all integer (F33). |
| OD7 | Samsung tables (D28): how does the code get them? | build-time extraction → generated header (gitignored) / runtime load from the blob | **Both, by stage**, but narrower now (see OD17). Only eBands, the two alloc tables and the PulseCache need extraction. |
| OD8 | Process-global state | replicate globals / move into the handle | **Move into the handle.** F33.6 lists the encoder-struct fields found so far. Still to confirm which globals carry state across frames (DRC/headroom/previous_uhq_index) vs scratch. |
| OD9 | Decoder acceptance without a reference decoder | no-desync + SNR / + listening | **Both** (unchanged). |
| OD10 | Test-corpus music, especially the 96k tracks with the artifact | user's call | **User to name tracks.** Needed for T2.2 golden frames and O3. |
| OD12 | Patch 0004 and 0006's second hunk are code-read only | keep / drop | **Keep both**, flagged (F31, F33.9). 0006's 150-byte limit is unreachable (intra peaks ~80 B). |
| OD15 | When to start the decoder (O3) | now / after T4.2 / after framing (T4.8) | **Now, starting with T3.1 header + adaptive end (F33.7).** Batch 2 delivered what the decoder needs: modes, alloc, 24-bit ec, 0007's band 21. The decoder's IMDCT need not be bit-exact, so T4.2 doesn't block it. |
| OD17 | Formula-generated tables (Q31 window, win_square, Q31 trig, π/4N rotation) | generate in code / extract like the rest | **Generate in code.** All match the formula exactly (F33.1, F33.8). It's standard CELT math, so nothing Samsung-derived is needed. |
| OD18 | Alloc-table selector (alloc `+0x180`: shared +0x28 vs per-mode +0x30) | — | **Find what sets it** when porting ssc_encode (T4.8). It probably depends on mode/UHQ tier. The decoder must make the same choice, so it's needed for O3 too. |
| OD19 | 48k PulseCache band-20 quirk (F33.2) | keep / "fix" | **Keep** (bitstream compatibility). Never "fix" it in O6: the buds' decoder presumably has the same table. |
| OD20 | amp2Log2 overflow at bandE ≥ 2^29 | ignore / reproduce blob wrap | **Decide in T4.7.** If headroom guarantees bandE < 2^29, ignore it. Otherwise patch to the blob's NEON wrap. |
| OD21 | MDCT/FFT shape in the rebuild | patch mdct.c/kiss_fft.c / new `ssc_mdct.c` in the patch series | **New file**: it's a rewrite (Q31, own kfft state), not a delta. Test it against `ssc_mdct_forward` and `ssc_fft` via difftest. |

## Next-session checklist
1. **T4.2 MDCT/FFT** (F33.8): transcribe `ssc_mdct_forward` + `ssc_fft` (Q31, formula
   tables, OD17/OD21). Difftest both shifts (0 and 3) on both modes.
2. **T3.1 + F33.7**: read the frame-header logic and the last-band / adaptive-end loop
   in ssc_encode (0xcf34–0xd114; header bytes 2–3), plus `compute_band_energies_for_lastband`
   (difftest it). Then OD18 (what sets alloc `+0x180`).
3. Start the O3 decoder (OD15): libopus v1.1 decoder + 0002 (24-bit ec) + 0007 (band 21)
   + Samsung modes and cache quirk + header from item 2. First input:
   `~/src/ssc-capture/uhq584_getlucky.frames`.
4. Remaining live Samsung code: pitch_search + tone_detect, patch_transient_decision,
   headroom (T4.7, main noise suspect, also OD20), DRC/limiter.
5. OD1 / OD10 need the user: listening session and track list.
