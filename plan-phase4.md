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

Workflow: ssc

## Progress (2026-10-26, end of session: native encoder live on the buds; F46, F47)
**The buds now play the native encoder.** `~/src/ssc-codec/harness/ssc_encoder.c` (v1.1's
`celt_encode_with_ec` plus Samsung's stages, on libopus v1.1 + patches 0001–0013) is a native
`ssc_encode`. It matches the blob on every frame tested:
- under qemu: the T2.2 corpus, 5 minutes × 3 tracks × 21 configurations (1.36 M frames),
  inputs up to int32 full scale, and bits=24 (F46);
- on x86_64: 63 runs, ~140 000 frames, at ~2 % of a core at uhq-584 (F47);
- live in tee mode: 23 785 frames, 0 differ.

The user switched the plugin to `SSCENC_BACKEND=native` and reports it "sounds really good"
(F47.6). qemu is out of the audio path; the blob remains available as `SSCENC_BACKEND=blob`.

The decoder got the two unsignalled encoder rules (patch 0013) and the 256k/328k TF fix. Two
side findings:
- The blob breaks the AArch64 calling convention (F46.6).
- The plugin feeds 256k/328k 48 dB too low. Fixed in the patch, not rebuilt (F47.4).

**2026-10-27 (housekeeping session, F48–F50):** T1.3 closed with the full classification (F48,
AC2). The stale `in[1071]` read is measured and closed: reached often, never visible in the output
(F49). T3.3 closed (F50): the new native decode tools decode every capture in sync, including 12 455
real A31→buds frames, and give a native encode/decode/SNR path for O6. D36/D37 record the settled ODs.

**Scope cut (user, 2026-10-27, D38):** the phase's goal was a byte-identical encoder, and it is met.
O6 (tuning) and the T3.4 per-band map that fed it are out of scope.

By objective: O0 100 %, O1 100 %, O2 100 %, O3 100 %, O4 100 %, O5 100 %; O6 out of scope. **The
in-scope work is done (100 %).** AC1–AC5 are met, and AC6 is dropped with O6. Left, all
optional or the user's: the plugin rebuild for the 256k/328k level fix (F47.4), OD10 (corpus
tracks), live tee on the other profiles, and committing both repos. Before the cut it was ~83 %.

## Objectives

**O0. Rule out the cheap explanations first (D32)**
- [x] T0.1 *(p50 0.83 ms, p99 1.75 ms, ~10 % of real time at uhq-584; `tools/helper_timing.py`, F42)* qemu deadline: time `ssc_encode` per frame under the helper at UHQ-584 (budget
      9.0 ms per 864-sample frame at 96 kHz). Log p50/p99/max over 5 min of music.
- [x] T0.2 *(pw-top 30 s at uhq-584/shift 0: ERR 0, B/Q ≤ 0.09, journal clean, F39.2; Wi-Fi off: no audible change, F41.4)* Transport: watch `pw-top` ERR/xruns and the BT sink's dropped or late packet
      logs during 96k playback. Repeat with Wi-Fi off, because the AX201 combo chip shares
      its antenna between Wi-Fi and BT.
- [x] T0.3 *(input level: shift 6 terrible, shift 0 clean on Get Lucky 88.2k, F39.1; the F38.7 gain riding)* Classify the artifact: periodic clicks or dropouts point to timing (fixed by
      O5). A steady noise floor, swishiness or pre-echo points to the codec (O3/O6).
- [-] T0.4 *(not needed: no timing-type artifact; every profile clean, F41, F42)* Decode what actually goes over the air: `btmon -w` during 96k playback on the
      laptop, extract the SSC frames from the A2DP media packets (adapt
      `~/src/ssc-capture/parse_btsnoop.py`), and run them through the O3 decoder (sync,
      then WAV for listening). Clean decoded audio with a bad sound on the buds clears the
      encoder and points to transport, timing or the buds. Check RTP sequence/timestamp
      gaps and packet spacing in the same capture (T0.2).

**O1. Fork base and delta map**
- [x] T1.1 Identify lineage: fixed-point CELT from libopus with an `ssc_` prefix (F27).
- [x] T1.2 Pin the libopus version: **v1.1** (range 1.1-rc … 1.1 final; these can't be
      told apart in this binary). See F29. Clone at `~/src/opus`.
- [x] T1.3 *(F48: all 85 exports by name, 69 live / 16 dead from a call-graph walk, each live one with its class, patch and test; F30 was the first pass)* Classify every exported function as **stock**, **modified** or
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
- [x] T2.2 *(generated in the `encode` test instead of stored golden frames: every item below as a 20-frame
      segment, the blob encoding on the fly; music per OD10, F46.4)* Test corpus: sines (100 Hz, 1 kHz, 15 kHz, 30 kHz), log sweeps, white and pink
      noise, castanets/transients, silence, full-scale clipping, and 3 music excerpts
      (44.1k-sourced and native 96k). Encode each with the blob at basic-229, UHQ-584,
      UHQ-291 and UHQ-152 to get golden frames.
- [x] T2.3 *(done differently: the `encode` test's PLT-interposition trace records every call `ssc_encode`
      makes, arguments and results, and compares it with the reference's, F46.1)* Per-function golden vectors captured at the function boundary. Use a qemu
      gdbstub breakpoint inside `ssc_encode` for functions only reachable mid-frame.

**O3. Decoder first (D29)**
- [x] T3.1 *(parser in `ssc_header.h`, capture + live encode, F36)* Parse Samsung's frame header
      and map it to bitrate, E/F mode, `end` band and the alloc table.
- [x] T3.2 *(patch 0010: no tapset bit, end band + alloc table from the header, E-mode
      skips TF, F-mode uses stock `tf_decode` (F37); band 21 denormalised, stock Q15
      window + v1.1 inverse MDCT from the harness mode, output delayed by 108 (F38.1–2).)*
      libopus CELT decoder plus Samsung's static modes (22 bands, 864-sample frame,
      overlap 108) plus 22nd-band decoding plus every bitstream change found in O1.
- [x] T3.3 *(F50: natively, 1.12 M round-trip frames over 21 configurations, the S24U capture, the SSCOnWindows golden frames and **12 455 real A31→buds frames from the btsnoop capture** all decode with no error (the wrong mode fails on each); spectrograms of source, 584k, 152k and the 584k error. Earlier: the `encode` test decodes every blob frame of 21 configurations with no range error and the
      encoder's exact oldBandE, after patch 0013 (carry-over, −28 dB restart) and the 256k/328k uhq fix, F46.5.
      `decode` test: sync on noise, tones and all 6666 capture frames; per-band
      gain/SNR on tones; 96k music report via `DT_MUSIC` (F38.4–7); 48k F-mode music via `DT_MUSIC_RATE` (F40).
      )*
      Validate: decode golden frames and compare with the source PCM (time-aligned
      SNR, spectrogram). The decoder is right when every corpus item decodes with no
      range-decoder desync (`ec_tell` matches frame length) and plausible SNR.
- [x] T3.4 *(answered: the noise is born in the encoder's limiter stage when fed 2^29
      (F38.7, F43), confirmed by listening (F39.1). The per-band map was only input to O6 and is
      out of scope with it (D38).)*
      **Measure the user's artifact**: decode the 96k path, then build a per-band
      noise-to-signal map. This answers whether the noise is born in the encoder.

**O4. Bit-exact native encoder (D30)**

Bottom-up. Each step passes its per-function vectors before the next starts:
- [x] T4.1 *(bit-exact via libopus + patches 0001/0002, F31)* Range coder (`ec_enc_*`, `ec_laplace_encode`) and fixed-point math (`frac_div32`,
      `rcp`, `rsqrt_norm`, `sqrt`, `bitexact_cos`, `amp2Log2`, `smull32`).
- [x] T4.2 *(patch 0009, F34.1)* FFT/MDCT (`ssc_fft`, `ssc_mdct_forward`): Q31, formula tables, shifts 0 and 3, both modes.
- [x] T4.3 *(band energies, normalise, amp2Log2, and `ssc_compute_band_energies_for_lastband` bit-exact, F34.2; the adaptive-end scan bit-exact inside the native `ssc_encode`, F46)* Band energy, normalisation and the 22nd band.
- [x] T4.4 *(coarse with 0006, fine, finalise; eMeans is stock, F33)* Energy quantisation (coarse, fine, finalise) and `ssc_eMeans_tmp`.
- [x] T4.5 *(compute_allocation F, quant_all_bands with 0007, PVQ/theta/intensity; the 48k cache quirk F33.2 is carried through)* Allocation (`compute_allocation`, alloc vectors, PulseCache) and PVQ
      (`alg_quant`, `encode_pulses`, `quant_all_bands`, `stereo_itheta`, `haar1`).
- [x] T4.6 *(pitch downsample/xcorr/remove_doubling, spreading 0005, comb filter 0008; pitch_search,
      find_best_pitch, tone_detect and patch_transient_decision with 0011, F45. transient_analysis,
      tf_analysis and dynalloc are not exported (inlined in `ssc_encode`) and moved to T4.8, as the F44 handoff planned)* Analysis: transient, tf_select, spreading, hysteresis, pitch
      (downsample/search/remove_doubling/xcorr), comb-filter prefilter.
- [x] T4.7 *(headroom bit-exact: fixed per-bin EQ on the MDCT, test `headroom`, F43; DRC chain
      bit-exact: biquad, powercalc, CalculateLimitGain, SSC_DRC_Exe and the whole stage frame by
      frame on synthetic input and 3 tracks × 10 configs, tests `drc_*`, F44)* Samsung pre-processing: headroom and DRC/limiter (`uhq_headroom`,
      `normal_headroom`, `SSC_DRC_Exe`, biquad).
- [x] T4.8 *(native `ssc_encode` in `harness/ssc_encoder.c`, bit-exact on 21 configurations: the T2.2
      corpus as 200 generated frames each, 5 minutes × 3 tracks (1.36 M frames), 30 s hot (+6, +8 bits) and with
      bits=24, with the handle, globals and hidden scratch compared every frame and every frame decoded, F46.
      The stale `in[1071]` read is reached on ~13 % of music frames but never changes the output, F49; its state is compared)* Framing and the `ssc_encode` top level, then whole-frame bit-exactness on the
      full corpus × 4 modes × multi-minute runs (state carry-over).

**O5. Integrate native encoder (D4)**
- [x] T5.1 *(native build, tables extractor, `ssc_native_helper` with the blob helper's protocol, and the
      launcher switch `SSCENC_BACKEND=native|tee` (F47, D35); installed 2026-10-26, and the live backend is
      native since ~18:28 on the user's switch, F47.6)* New backend in the PipeWire SSC plugin behind `SSCENC_BACKEND=native`. The
      default stays `blob` until the listening test passes.
- [x] T5.2 *(live tee at uhq-584: 23 785 frames, 0 differ (journal `tee: done` lines, F47.6). The other
      profiles are covered offline: all 21 configurations byte-identical on x86_64, F47.2)* Bit-exact check in the live pipeline: tee frames from both backends and
      compare.
- [x] T5.3 *(user, 2026-10-26, uhq-584 on native: "sounds really good on native", F47.6; no blind A/B,
      but the output is byte-identical to the blob's)* Listening test (D6: volume 10% first). Native must sound identical to blob.
      Any difference is a bug.

**O6. Bitstream-compatible quality work (only after O3 + O5)** — **[-] out of scope (D38,
user, 2026-10-27):** the phase needed byte-identical, not tuned. Kept as the record of the idea.
- [-] T6.1 Use the decoder to rank the T3.4 noise sources.
- [-] T6.2 Candidate encoder-side changes, each behind a flag and each measured with the
      decoder: bypass the DRC/limiter and headroom when gain would be 1.0 anyway; spend
      fewer bits above 20 kHz in the 96k mode (bands 19–21 cover 16–48 kHz, F27) via
      allocation trim, dynalloc or band skip; and newer libopus analysis (tf, spreading,
      trim, dynalloc) ported onto Samsung's mode.
- [-] T6.3 For each change: the buds decode it without glitches (listening), the objective
      metric improves, and the user prefers it in an A/B test.

## Acceptance criteria
- **AC1** O0 concludes with evidence whether timing or transport contributes to the artifact.
- **AC2** Function classification table (T1.3) in findings.md, with every exported
  function covered. *(Met 2026-10-26: F48.)*
- **AC3** Decoder decodes 100% of golden frames without desync. The 96k artifact is
  characterised with numbers (T3.4). *(First half met: F46.5, F50. Second half: the artifact was the
  input level, measured in F38.7/F39. Met; the per-band map is dropped with O6, D38.)*
- **AC4** Native encoder is bit-exact with the blob on the full corpus in all four modes.
- **AC5** Native backend runs in PipeWire at under 5% of one core at UHQ-584 and sounds
  identical to blob in a listening test. *(Met 2026-10-26: ~1.9 % of a core (F47.3), byte-identical
  output, and the user's listening on native (F47.6).)*
- **AC6 (stretch)** *(Dropped with O6, D38.)* At least one O6 change measurably reduces the 96k noise and is
  preferred by the user, with no decode glitches on the buds.

## Out of scope
- Buds firmware RE (the decoder side on the buds).
- Committing Samsung code, binaries or data tables to this repo (D28).
- Upstreaming to openssc, libopus or PipeWire.
- Encoder tuning (O6) and the per-band error map (T3.4) that fed it (D38, user, 2026-10-27). The
  native encoder stays byte-identical to Samsung's blob. `ssc_native_roundtrip` and F50.2/F50.3
  are the starting point if it is ever reopened, as a new phase.

## Open decisions (as of 2026-10-27)

Settled this phase: OD2, OD3, OD5, OD6, OD16 → **D36** (codebase) · OD13, OD14 → **D37**
(verification method) · OD11 → docs committed (done, a one-off action). OD18 → alloc `+0x180` is 1 iff bitrate > 192000 (F34.4). OD21 → `ssc_mdct.c` in patch 0009, bit-exact (F34.1). OD15 → decoder started and producing PCM (F37, F38). OD1 → O0 done, timing ruled out (F42). OD23 → `SSCENC_INPUT_SHIFT` default 0 (D33, F39); relative to the mode, basic at 2^15 (D34, F40).

Still open:

| # | Question | Options | Recommendation |
|---|---|---|---|
| OD4 | Implementation language and numeric model | C fixed-point / C float / Rust | **C, `FIXED_POINT`.** Thirteen patches on v1.1 (0010 and 0013 are the decoder) plus `harness/ssc_encoder.c`, all integer and bit-exact (F46). |
| OD7 | Samsung tables (D28): how does the code get them? | build-time extraction → generated header (gitignored) / runtime load from the blob | **Build-time extraction, implemented and in use live (F47.1, D35); no separate user comment yet.** `tools/ssc-extract-tables.py` writes the gitignored `native/build/ssc_tables.h` from the user's blob (sha256-checked): both static modes (eBands, shared and per-mode alloc tables, logN, PulseCache index/bits; caps from stock `init_caps`), `headroom_gain` and the biquad coefficients. The intensity tables aren't needed (F46.2). The harness keeps reading the blob at runtime. |
| OD8 | Process-global state | replicate globals / move into the handle | **Move into the handle**, now fully mapped (F46.3). `struct ssc_encoder` holds the blob-layout handle (`ssc_abi.h`), the DRC globals (`struct ref_drc`), `previous_uhq_index`, and `in[]` (the blob's `in_shared`): `in[1071]` carries into the next frame's `tone_detect`. Two globals remain in the patched libopus: `ssc_pitch_xcorr` (pitch.c, seeded from `in[]` each frame) and the `ssc_vq_scratch` pointer (vq.c, patch 0012). Both are fine for one encoder per process and need passing in before two encoders can share one. Everything else in .bss is per-frame scratch. |
| OD9 | Decoder acceptance without a reference decoder | no-desync + SNR / + listening | **Both** (unchanged). |
| OD10 | Test-corpus music for T2.2 golden frames | user's call | **Proposed, not confirmed by the user:** Nirvana *MTV Unplugged* SIDE-1 (96k/24, peaks −1.7 dBFS, never engages the limiter); "Get Lucky" album 88.2k/24 (0 dBFS, engages it from 0.1 s); Arctic Monkeys "Do I Wanna Know" 44.1k/24 (hot modern master). Paths in F40/F43.4. Feed them at the plugin's levels (2^23 UHQ, 2^15 basic). |
| OD12 | Patch 0004 and 0006's second hunk are code-read only | keep / drop | **Keep both**, flagged (F31, F33.9). 0006's 150-byte limit is unreachable (intra peaks ~80 B). |
| OD17 | Formula-generated tables (Q31 window, win_square, Q31 trig, π/4N rotation) | generate in code / extract like the rest | **Generate in code.** All match the formula exactly (F33.1, F33.8). It's standard CELT math, so nothing Samsung-derived is needed. |
| OD19 | 48k PulseCache band-20 quirk (F33.2) | keep / "fix" | **Keep** (bitstream compatibility). Never "fix" it in O6: the buds' decoder presumably has the same table. |
| OD20 | amp2Log2 overflow at bandE ≥ 2^29 | ignore / reproduce blob wrap | **Not needed for now.** Through `ssc_encode` no band energy reached 2^29 on 30 s of Get Lucky at any level up to int32 full scale (8 bits over the plugin's level), all 21 configurations, all bit-exact with stock `amp2Log2` (F46.4). Revisit only if a frame mismatches there. |
| OD24 | Where the native encoder lives for O5 | keep `harness/ssc_encoder.c` + patches as the source of truth, built into a library / move into a patch on `celt_encoder.c` | **Keep the file, implemented (D35).** `native/` builds it with `-DSSC_NO_TRACE` into `libsscenc.a` and the helper. The coverage counters (`ssc_enc_stats`) stay; they cost nothing. |
| OD22 | 96 kHz mode with an F-mode bitrate (88/96/128/192/229/256/328 kbit/s) | support / reject | **Reject until a capture shows it.** The F-mode last-band table is the 48k split and does not match the 96k top band (F35). Live pairs are 48k+F and 96k+E. |

## Next-session checklist (2026-10-27, phase done in scope; native live)

Workflow: ssc. Repos: `~/src/ssc-uhq` (docs, plugin patch, tools, launcher) and
`~/src/ssc-codec` (harness, native build). Both have uncommitted work from this session and
earlier ones.
- In ssc-codec: patches 0009–0013, the `harness/` sources (`ssc_header.h`, `ssc_abi.h`,
  `ssc_encoder.c`/`.h`, `trace.*`, `trace_calls.h`, `trace.list`, `difftest.c`, Makefile),
  `native/`, `tools/ssc-extract-tables.py`, `tools/helper_compare.py`, `.gitignore`, README.
- In ssc-uhq: the docs, `config/` (launcher, plugin patch, `sscenc.conf` with
  `SSCENC_BACKEND=native`), `tools/helper_timing.py`, `tools/drc_count.py`. The `skills/`
  deletions are the user's.

New since: in ssc-codec `native/ssc_native_dec.h`, `ssc_native_roundtrip.c`, `ssc_native_decode.c`,
the `ssc_native_tables()` accessor, Makefile targets and a README paragraph; in ssc-uhq F48–F50,
D36–D37 and this plan.

The user commits; don't commit unasked. Last verified runs: `make -C harness run`, 47 tests, all
pass (2026-10-27, 83 s); `make -C native` builds clean and `tools/helper_compare.py` is identical
on 5 configurations × 3000 frames after the decoder tools went in (2026-10-27; 63 runs on
2026-10-26); live tee 23 785 frames, 0 differ (2026-10-26).

**Live state.** The plugin runs `SSCENC_BACKEND=native`
(`~/.local/libexec/sscenc/x86_64/ssc_native_helper`, no qemu).
- Back to the blob: set `SSCENC_BACKEND=blob` (or delete the line) in
  `~/.config/systemd/user/wireplumber.service.d/sscenc.conf` and run `ssc-switch uhq`.
- Check again in tee mode: `SSCENC_BACKEND=tee`, then `journalctl --user -u wireplumber |
  grep 'tee:'`.

After any change to `harness/ssc_encoder.c` or the patches:
- re-run `make -C harness run`;
- rebuild `make -C native`;
- re-run `tools/helper_compare.py`;
- only then copy `native/build/ssc_native_helper` to `~/.local/libexec/sscenc/x86_64/`. That
  helper is what the buds hear; it is replaced at the next stream.

Phase 4's in-scope work is done (D38). Nothing is queued. If tuning comes back, it is a new
phase that starts from the out-of-scope note above.

Needs the user (optional): rebuild the plugin for the 256k/328k level fix when convenient (F47.4; only
`SSCENC_PROFILE` basic-256/328 and friends). Say if build-time table extraction (OD7) should
change. OD10 (corpus tracks) is still only proposed.

Inputs from this session (gone with the scratch dir; rebuild if needed): the music as S32LE,
`ffmpeg -t 240 -i TRACK -ac 2 -ar 96000|48000 -f s32le`; the A31 frames from
`~/src/ssc-capture/btsnoop_hci.log` (F50.1).

Optional: live tee on other profiles (`ssc-switch high`/`uhq291`/`uhq152`; offline they are
already byte-identical). The stale `in[1071]` read is closed without a test (F49): music reaches it
on ~13 % of frames, but no frame's output depends on it.

Pitfalls: the blob clobbers d8–d15 (F46.6), so harness code must not keep floats live across
a blob call (`enc_signal` is `noinline` for that). `pgrep -f`/`pkill -f` match their own
command line; use `-x` with the process name. `ssc-switch`/`set_ssc.sh`/`abtest.sh` rewrite `SSCENC_INPUT_SHIFT` on every
switch (default 0 since D33; `ssc-auto` calls `ssc-switch uhq` without a shift). The
plugin was rebuilt on 2026-10-26 (F41.2) from `~/src/openssc/pipewire-1.6.8`; the old
build is `….so.pre-d34`. The build tree in `~/.cache/openssc-build` is stale (pre-D33). `cp` is aliased to `cp -i` in this shell. Tones whose bins are multiples of 8 repeat every 432 samples and fool the lag
search (detune them). `end` varies per frame on music (19–22 at 584k). The 16-bit decoder
output clips at 2^29, which spreads energy above `end`. The helper sends a ready word (int32 0)
before the first frame; skip it or every timing is off by one frame (F42). The decompile got
`normal_headroom`'s second taper wrong (F43.2), so check every transcription against the blob,
not the decompile. The rebuilt plugin's `encoder.c` comment still says "headroom stage"; it's
cosmetic, so it wasn't regenerated. Music inputs for `drc_chain` (`DT_DRC_MUSIC`) and
`pitch_search` (`DT_PITCH_MUSIC`, the same S32LE stereo at 2^31 format) live only in a
session scratch directory. Recreate them with the codec README's ffmpeg line; the tracks are in
`~/Descargas` (F44.5, OD10); RAM is in `~/Descargas/Random Access Memories`, Arctic Monkeys in
`~/Descargas/Arctic_Monkeys-AM`. The `encode` test traces only its first 10 frames per
configuration by default: tracing costs more than encoding. Don't rebuild `build/ssc`
while a background run is using it (copy the harness, or `make B=build/dev`). `difftest`
exports the blob's PLT names (`trace.list`), so a new libopus symbol that matches a blob
export would hijack the blob's call; keep the list explicit. One mutation survives `drc_exe` and is equivalent (`>` vs `>=`
in the attack/release choice). Two survive the pitch tests (F45.5): one equivalent, and the stale
`xcorr[244]`, covered by composition. Which DRC thresholds the default synthetic run reaches
depends on the RNG sequence, and new tests shift it (the full suite saw 0x76c0..0x800000), so use
music to cover the edges. `powercalc` never returns unless n is a multiple of 32. Blob
globals that aren't exported are reached at `dladdr` base + file address (Ghidra address −
0x100000), as `pitch_search`'s test does. Random int32 test values almost never tie, so
`<=` vs `<` mutants need quantised or crafted inputs.
