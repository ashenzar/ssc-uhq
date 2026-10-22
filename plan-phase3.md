# Phase 3 — Native 44100 Hz SSC Investigation

Status legend: [ ] todo, [~] in progress, [x] done, [-] dropped (with reason)

## Mission

~~Enable native 44100 Hz SSC basic-mode streaming on PipeWire without dropouts.~~

**CLOSED.** Live testing (F20) proved the Buds3 Pro decode basic-mode SSC frames at a
fixed 48 kHz DAC rate. Sending 44100 Hz frames causes 8.8% speedup and periodic silence
gaps. Samsung's phone resamples 44100→48000 before encoding — the `rate=44100` parameter
to `ssc_encoder_create` is for internal bitrate math, not playback rate. Native 44100 Hz
is not viable without buds firmware changes.

**Correct path**: PipeWire resampler quality=10 (44.1→48 kHz) is the right approach.

## Results

**D20 — quantum=864 experiment**: aligned graph quantum perfectly to SSC frame size.
Eliminated the old 5s-on/1s-off dropout pattern but revealed the real problem: audio
played 8.8% too fast with periodic gaps. The buds consume frames at 48 kHz regardless.

**D21 — Abandon native 44100 Hz basic-mode SSC.** The buds' firmware controls decode rate.
No transport timing fix can change it.

## Objectives (all closed)

**O1. RE Samsung's send_frames scheduling**
- [x] T1.1 Partial annotation of `send_frames` (0x88aa08): timestamp delta, bytes_per_tick
      accumulation, frame count loop documented in F19. Full annotation not needed.
- [-] T1.2–T1.4 Dropped — moot after F20 (buds fixed at 48 kHz decode).

**O2. RE PipeWire's media-sink scheduling**
- [-] T2.1–T2.3 Dropped — the 44100 Hz dropout was not a PipeWire timing issue; it was
      the buds playing frames at the wrong rate.

**O3. Implement timing fix**
- [x] T3.1 Tested candidate (d): quantum=864 at 44100 Hz. Result: perfect frame alignment
      but audio sped up — proved the issue is buds-side, not transport-side.
- [-] T3.2–T3.3 Dropped — no fix possible without buds firmware changes.

**O4. Validate and clean up**
- [-] T4.1 Dropped — 44100 Hz not viable.
- [x] T4.2 No regression on 48 kHz basic-229 or 96 kHz UHQ-584 — verified after revert.
- [-] T4.3 Dropped — no 44100 Hz mode to add.
- [x] T4.4 Findings F19, F20 and decisions D17–D21 documented.

## Findings

- **F19**: Samsung's `send_frames` uses elapsed-time byte accumulation, not quantum-aligned
  encoding. BT stack timer fires ~10ms, accumulates bytes, encodes when ≥ frame_byte_size.
- **F20**: Buds decode basic-mode SSC at fixed 48 kHz. Frame headers identical at 44100 and
  48000 (`ff ee 0d f4`). Sending 44100 produces 8.8% speedup + gaps. Samsung resamples
  44100→48000 before encoding.

## Decisions

- **D17**: RE first, fix second — correct approach, led to understanding.
- **D18**: Don't modify media-sink.c — remains valid.
- **D19**: 44100 Hz behind env flag — now permanently disabled (D21).
- **D20**: quantum=864 test — proved the issue was buds-side, not transport.
- **D21**: Abandon native 44100 Hz basic mode.
