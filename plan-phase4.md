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
- [ ] T1.3 Classify every exported function as **stock**, **modified** or
      **Samsung-only**, with evidence. Expected Samsung-only: DRC (`SSC_DRC_*`,
      `CalculateLimitGain`, `Biquad_Narrow_int2int_stereo`, `powercalc`), headroom
      (`normal_headroom`, `uhq_headroom`, `headroom_process`, `headroom_gain`), framing
      (`get_header_from_bitrate`, `set_data_header`, `set_data_frame_header`,
      `get_uhq_info_from_bitrate`, `set_limit_gain_from_bitrate`), 22nd band
      (`*_lastband*`, `last_band_info_calc`), `tone_detect`, pitch/prefilter gain helpers.
- [ ] T1.4 Decode the remaining `CELTMode` fields for both modes (alloc vectors, logN,
      window, MDCT twiddles, PulseCache) and confirm the struct layout against libopus.

**O2. Differential test harness (D31)**
- [ ] T2.1 `harness/blobcall`: an aarch64 program run under qemu. It dlopens the blob and
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
- [ ] T4.1 Range coder (`ec_enc_*`, `ec_laplace_encode`) and fixed-point math (`frac_div32`,
      `rcp`, `rsqrt_norm`, `sqrt`, `bitexact_cos`, `amp2Log2`, `smull32`).
- [ ] T4.2 FFT/MDCT (`ssc_fft`, `mdct_forward`) with Samsung's 1728-point twiddles.
- [ ] T4.3 Band energy, normalisation and the 22nd band.
- [ ] T4.4 Energy quantisation (coarse, fine, finalise) and `ssc_eMeans_tmp`.
- [ ] T4.5 Allocation (`compute_allocation`, alloc vectors, PulseCache) and PVQ
      (`alg_quant`, `encode_pulses`, `quant_all_bands`, `stereo_itheta`, `haar1`).
- [ ] T4.6 Analysis: transient, tf_select, spreading, hysteresis, pitch
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

## Open decisions (to settle at the start of next session)

Each has a recommendation. Settle it, then record it as a D-entry in decisions.md.

| # | Question | Options | Recommendation |
|---|---|---|---|
| OD1 | Which first: O0 (timing/radio checks) or O1/O2 (version pin + harness)? | O0 first / O1–O2 first / both in parallel | **O0 first.** It takes about an hour and may reframe priorities. If clicks or dropouts show up, O5 (native) is the fix, not O6. |
| OD2 | Where does the rebuild code live? | Inside `ssc-uhq/` / new private repo `~/src/ssc-codec` / fork of openssc | **New private repo** `~/src/ssc-codec`. The codec has its own build, tests and libopus base. `ssc-uhq` stays the investigation log (plans, decisions, findings). |
| OD3 | How to bring in libopus? | git submodule pinned to the T1.2 version / vendored copy / fetch at build | **Submodule pinned to `v1.1`** (T1.2 done, F29; BSD-3). Samsung deltas go in separate files so they're easy to diff against upstream. |
| OD4 | Implementation language and numeric model | C fixed-point (match blob) / C float / Rust | **C, `FIXED_POINT`**. Bit-exactness (D30) requires the same integer arithmetic. Float can be an O6 experiment later. |
| OD5 | Decompiler | Install Ghidra (headless + GUI) / keep objdump + tools / rizin | **Install Ghidra.** objdump was fine for Phases 1–3, but a 16 KB `ssc_encode` with inlined CELT needs decompiled pseudo-C to diff against libopus source. |
| OD6 | Use the Vinchethescript `encoder.c`? | Ignore / navigation aid only / start from it | **Navigation aid only** (F28.6). It was built from a different binary, is unverified, and its author calls it broken. Our own Ghidra output on the S24U blob is the reference. |
| OD7 | Table extraction (D28) | Python script → generated C header at build time / runtime load from the blob | **Build-time script** (`tools/extract_tables.py <blob> > tables.h`, gitignored output). Same pattern as the encoder: the user supplies the blob. |
| OD8 | Global-state semantics (F28.3) | Replicate process globals exactly / move into the handle and accept non-bit-exactness with multiple instances | **Move state into the handle**, but bit-exact for the single-instance, fresh-process case, which is the only case PipeWire uses. Note the deviation. |
| OD9 | Decoder acceptance without a reference decoder | No-desync + SNR threshold / also listen via a WAV render | **Both.** `ec_tell` must match the frame length exactly, SNR must be plausible per mode, and the user listens to the decoded WAV next to the source. |
| OD10 | Test corpus music | Which tracks for the 3 music excerpts (44.1k-sourced + native 96k) | **User's call.** Include the 96k track(s) where the artifacts were heard (F28.9), plus "Get Lucky" for continuity with earlier phases. |
| OD11 | Commit Phase 4 docs | Commit now / after OD answers | **Commit now** (plan + D27–D32 + F27–F28). Not yet committed as of this session. |

## Next-session checklist
1. Settle OD1–OD11 and log them as D33+.
2. Commit the docs (OD11).
3. If OD1 = O0: run T0.1–T0.3 with the exact track/volume from F28.9.
4. Install Ghidra (OD5), import the S24U blob, and apply the exported symbol names.
5. T1.2: clone libopus, build pre-1.3 tags fixed-point for aarch64, and compare
   `ec_enc_*`, `quant_coarse_energy` and `compute_allocation` to pin the version.
