# Phase 2 — SSC Quality Tuning

Status legend: [ ] todo, [~] in progress, [x] done, [-] dropped (with reason)

## Mission

Close the remaining subjective quality gap on UHQ-584 ("bass chirping louder than
vocals") and optimise the full signal chain for real-world listening.

## Objectives

**O1. Fix PipeWire resampler quality for 44.1→96 kHz upsampling**
- [x] T1.1 Verify/fix the `resample.quality` config — original `stream.properties` only
      in `pipewire.conf.d/`. Added to `pipewire-pulse.conf.d/` (PulseAudio clients like
      Deezer) and `client.conf.d/` (native PipeWire clients). All three set quality=10.
- [x] T1.2 Listening test: UHQ-584 with 96k source = excellent (native, zero resampling).
      UHQ with 44.1k = less codec noise but worse resampling (2.177:1) than basic-229's
      44.1→48k (1.088:1). Use UHQ for hi-res, basic-229 for daily. See F21.

**O2. Remove warm-up init from blob helper**
- [x] T2.1 Built no-init variant; compared 30 frames × 4 configs (UHQ-584, UHQ-291,
      basic-229@48k, basic-229@44.1k). All IDENTICAL byte-for-byte. The `get_size`+`init`
      calls have zero effect on encoder output. No change needed.
- [-] T2.2 Dropped — output is identical, no listening test needed.

**O3. Source-rate matching — avoid resampling entirely**
- [x] T3.1 Added 44100 to PipeWire `clock.allowed-rates` (50-ssc-uhq-rate.conf now
      lists `[ 44100 48000 96000 ]`).
- [x] T3.2 Auto-switch: `ssc-auto` monitors stream rates. On UHQ, toggles graph rate
      instantly (48k↔96k via pw-metadata). On AAC/basic, also switches profile to uhq-584
      (~15s reconnect) and restores when hi-res ends. State file for WirePlumber restart
      survival, 30s cooldown. Systemd service: ssc-auto.service (enabled, not bound to WP).
- [x] T3.3 Plugin patched: `SSCENC_BASIC_RATE=44100` sets basic mode to 44100 Hz
      (Samsung's rate). codec_init now accepts 44100. Plugin rebuilt and installed.
      Encoder comparison: same frame sizes and modes at both rates; effective bitrate at
      44100 is 235 kbps vs 256 kbps at 48000 (more time per frame → more coding budget).
- [-] T3.4 Dropped — buds decode basic-mode at fixed 48 kHz; 44100 Hz not viable (D21, F20).

**O4. Trace Samsung's post-encode gain processing**
- [x] T4.1 Traced 0x88ba94-0x88bbfc: it's a **volume ramp** for L/R channels (two float
      gain multipliers at struct offset 72). When BOTH gains == 1.0 (normal playback),
      the entire block is skipped. Only active during BT volume transitions (fade-in/out).
      At UHQ (96k), no PCM reformatting; at basic, S24_3LE→S32LE unpacking follows (we
      already feed S32). See findings.md F16.
- [-] T4.2 Dropped — processing is a volume ramp, irrelevant to steady-state encoding.

**O5. Adaptive bitrate**
- [x] T5.1 Traced: `sample_mode` (byte at bitrate-mgmt struct offset 0) selects table
      index (0-3, clamped). Mode is incremented by the BT stack based on link quality
      (counter at 0x88c828); reset to 0 at 0x88e518/0x88e728. Our fixed bitrate uses
      index 0 (max quality) permanently. See findings.md F17.
- [ ] T5.2 Implement sample_mode switching in the plugin, gated behind an env flag.

## Acceptance criteria
- **AC1** Resampler quality verified effective (pw-dump shows quality=10 on active streams).
- **AC2** Blob helper warm-up init effect documented; removed if output is identical or
  listening test shows no regression.
- **AC3** Listening test: UHQ-584 with all O1–O2 fixes sounds at least as good as basic-229.
  If not, document why and recommend the better default.
- **AC4** Source-rate matching works: 44.1 kHz content plays through 48 kHz graph with
  UHQ-584 (double resample: 44.1→48k + 48→96k). Native 44100 Hz abandoned (D21, F20).
- **AC5** Post-encode gain processing documented (volume ramp, skipped — D14, F16).

## Decisions

**D12 — Fix resampler quality first, cheapest experiment.** One config change, immediate test.
*Why:* PipeWire's default quality 4/14 is known to produce audible artifacts on large ratio
conversions (44.1→96 kHz = 2.177:1). Quality 10 uses a longer filter with better stopband.

**D13 — Test warm-up init removal as an isolated experiment.** Build a second blob helper
without the `get_size`/`init` calls; encode the same FLAC; binary-diff the output.
*Why:* Samsung doesn't call init. The init may set shared library globals differently than
create, altering the encoder's internal state.

**D14 — Source-rate matching over resampler tuning long-term.** Avoiding resampling entirely
is always better than improving the resampler. For 44.1 kHz sources + basic-229, the graph
should run at 48 kHz (or 44.1 kHz if the codec supports it) with no rate conversion.
*Why:* Zero resampling = zero resampler artifacts.

## Out of scope
- Buds firmware RE (deferred to a future phase).
- Distribution of any Samsung code.
- Upstream contributions to openssc / PipeWire.
