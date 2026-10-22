# Findings

Binary: `libbluetooth_jni.so` (S26 Ultra S948BXXS3AZF4 Bluetooth APEX), sha256 108a25d0…e0bb.
Tools: `tools/xref.py` (string → code references), `tools/fn.py ADDR` (annotated function listing).
Addresses are virtual addresses (= file offsets in this ELF).

## F1 Function map (AC1)
Located through `__func__` / file-name strings each function logs (D3). Source files:
`system/stack/a2dp/a2dp_vendor_ssc.cc` (codec info/config) and
`system/stack/a2dp/a2dp_vendor_ssc_encoder.c` (encoder driver).

| Address | Function (from its log strings) |
|---|---|
| 0x88713c | A2DP_IsVendorSourceCodecValidSsc |
| 0x887234 | A2DP_ParseInfoSsc |
| 0x8873bc | A2DP_IsVendorPeerSinkCodecValidSsc |
| 0x887470 | A2DP_VendorUsesRtpHeaderSsc |
| 0x8874e4 | A2DP_VendorCodecTypeEqualsSsc |
| 0x8878fc | A2DP_VendorCodecEqualsSsc |
| 0x887d58 | A2DP_VendorGetBitRateSsc |
| 0x887dd0 | A2DP_VendorGetTrackSampleRateSsc (logs "uhq ver2") |
| 0x887f70 | A2DP_VendorGetTrackBitsPerSampleSsc |
| 0x888064 | A2DP_VendorGetTrackBitrateLimitationSsc |
| 0x888108 | A2DP_VendorGetSscType |
| 0x888204 | A2DP_VendorGetPacketTimestampSsc |
| 0x8888b8 | A2DP_VendorGetEncoderInterfaceSsc |
| 0x8889cc | A2DP_VendorAdjustCodecSsc |
| 0x888a68 | A2DP_VendorInitCodecConfigSsc / A2DP_BuildInfoSsc |
| 0x888bac, 0x888cb0, 0x888d38, 0x889948 | A2dpCodecConfigSsc methods (config selection; call A2DP_BuildInfoSsc) |
| 0x889c0c | A2DP_VendorLoadEncoderSsc / UnloadEncoderSsc (resolves ssc_encoder_create/destroy) |
| 0x88a168 | (a2dp_vendor_ssc_encoder helper) |
| 0x88a1ec | a2dp_vendor_ssc_encoder_init / _update / _reset_encoder |
| 0x88a640 | a2dp_vendor_ssc_set_current_encoder_info |
| 0x88a7a8 | a2dp_vendor_ssc_encoder_cleanup |
| 0x88a82c | a2dp_vendor_ssc_feeding_reset |
| 0x88a954 | a2dp_vendor_ssc_feeding_flush |
| 0x88aa08 | a2dp_vendor_ssc_send_frames (contains a2dp_ssc_get_num_frame, a2dp_ssc_encode_frames, …_on_dual_play) |
| 0x75f424, 0x77c264 | A2dpSscToHalConfig (HAL / offload path) |

## F2 Packet timestamp function
`A2DP_VendorGetPacketTimestampSsc` (0x888204): logs, then `*p_timestamp = *(uint32_t *)p_data`
(0x888264–0x88826c), returns true. As in AOSP, this getter serves the receive (sink) path; the
source-side timestamp rule is in the encoder (see F-encoder, pending).

## F3 Encoder accepts variable frame sizes (O2 partial)

Probing the S24U encoder at 96 kHz / 584 kbps with different `frame_samples` values:

| frame_samples | ms @ 96k | output bytes | status |
|---|---|---|---|
| 128 | 1.33 | 100 | OK |
| 256 | 2.67 | 198 | OK |
| 480 | 5.00 | 368 | OK |
| 512 | 5.33 | 392 | OK |
| 768 | 8.00 | 588 | OK |
| 864 | 9.00 | 660 | OK (mode switch after frame 0 on sine) |
| 1024 | 10.67 | 782 | OK |
| 1728 | 18.00 | — | crash |

The encoder accepts 128–1024 samples at 96 kHz. The crash at 1728 is
`SSCENC_BLOB_MAX_FRAME_SAMPLES = 8192`, so internal to Samsung's code.
All frame sizes produce consistent E-mode headers (`ff e0`).

## F4 Golden-frame comparison: A31 cannot do true UHQ

Comparing against SSCOnWindows' golden frames:

| Mode | Our S24U | Golden (A31) | Match |
|---|---|---|---|
| 48k/229k | `ff ee 0d f4` 576B | `ff ee 0d f4` 576B | bytes 0–312 identical; diverge at 313+ |
| 96k/584k | `ff e0 01 e3` 660B | `ff e0 01 e3` 660B | bytes 0–3 identical; diverge at byte 4 |
| A31 at 96k | `ff ee 0d f2` 676B | `ff e0 01 e3` 660B | **F-mode header, wrong size** |

The A31 encoder produces F-mode frames (`ff ee`) at 96 kHz — it falls back to 48 kHz
encoding logic. The S24U encoder produces true E-mode frames (`ff e0`). Therefore
SSCOnWindows' golden frames are NOT valid UHQ references. At 48 kHz, both encoders
produce nearly identical frames (floating-point drift divergence at ~byte 313).

## F5 UHQ frame modes are content-adaptive

Encoding 30 s of "Get Lucky" FLAC:

- **48k/229k**: all 1666 frames have mode `f4`, zero mode changes
- **96k/584k**: 3333 frames with 1144 mode changes; modes include e3 (most common),
  03, c3, 23, a3, 43, 63, 83. The encoder adapts coding mode per-frame based on
  audio content. This is normal behaviour and the buds decode all modes correctly.

## F6 PipeWire resamples 48→96 kHz for UHQ (likely root cause of bass timing)

`pw-metadata` shows:
```
clock.rate = 48000
clock.allowed-rates = [ 48000 ]
```

When the SSC UHQ codec requests 96 kHz, the BT `media-sink.c` detects the rate mismatch
(line 647: `resampling = (port->current_format.info.raw.rate != this->process_rate)`)
and activates the SPA rate matcher to resample 48→96 kHz. This resampler introduces
**frequency-dependent group delay** that is larger at low frequencies, which would
explain the user's "offbeat bass" complaint — bass timing is shifted relative to
mid/high frequencies.

**Fix applied**: `~/.config/pipewire/pipewire.conf.d/50-ssc-uhq-rate.conf` adds 96000
to `default.clock.allowed-rates`. After PipeWire restart, the graph will switch to
96 kHz when the UHQ codec is active, eliminating the BT-internal resampling.
The graph returns to 48 kHz when the UHQ sink disconnects.

**Status**: awaiting listening test (T6.2).

## F7 Packetization (O3 partial, from code inspection)

From the codec source (`a2dp-codec-ssc.c`):
- One SSC frame per RTP packet (`NEED_FLUSH_ALL` after each encode call)
- RTP header: v=2, pt=96, ssrc=0xff
- Timestamp: `++frame_counter` when `SSCENC_RTP_TS=frames` (per-packet counter);
  otherwise PipeWire's sample-count timestamp
- Frame size: `SSCENC_FRAME_SAMPLES = 864` samples for all modes

## F8 Capability negotiation (O4 partial, from code inspection)

Capability byte sent to buds (from profile):
- `SSC_CAP_BASIC_48K = 0x0c` (bitrate_limit | hifi_24) → basic/high modes
- `SSC_CAP_UHQ_TEST = 0x0e` (basic_48k | uhq2) → UHQ modes

The `validate_config` function sets rate=96000 when the `UHQ2` bit (0x02) is present;
otherwise rate=48000. The buds respond with capabilities byte 0x3e.

## F9 ssc_encoder_create parameters (T2.1 — from disassembly)

**A2DP_VendorLoadEncoderSsc** (0x889c0c) resolves three symbols via `dlsym`:

| Symbol | Stored at | Purpose |
|---|---|---|
| `ssc_encoder_create` | 0xdb8628 | Create encoder handle |
| `ssc_encoder_destroy` | 0xdb8630 | Destroy encoder handle |
| `ssc_encode` | 0xdb8638 | Encode one frame |

Then calls (0x889d04–0x889d40):
```
handle = ssc_encoder_create(rate, channels, bits, &error)
```

| Arg | Value | Evidence |
|---|---|---|
| rate (w0) | 96000 if stored rate==96000; **44100** otherwise | 0x889d24: `sub w9,w8,#0x17000; cmp w9,#0x700` → checks rate==96000; fallback=44100 (0x889d14) |
| channels (w1) | 2 | 0x889d1c: `mov w1, #0x2` |
| bits (w2) | from 0xdbf308 = **34** for our buds | 0x889d38: `ldrb w2, [x19, #0x6c9c]` → 0xdbf308; see F13 |
| &error (x3) | stack pointer | 0x889d18: `add x3, sp, #0x3c` |

Return value (encoder handle) → 0xdbf320. A second handle at 0xdbf328 for dual-play.

**Discrepancy**: our openssc passes `bits=24`, Samsung passes `bits=34`.
**Discrepancy**: our openssc passes `rate=48000` for basic, Samsung passes `44100`.

## F10 ssc_encode per-frame call (T2.2 / T3.1 — from disassembly)

At 0x88b97c–0x88b9a8 inside `a2dp_vendor_ssc_send_frames`:
```
ret = ssc_encode(handle, output, 864, input + offset, bitrate)
```

| Arg | Value | Evidence |
|---|---|---|
| handle (x0) | from 0xdbf320 | 0x88b990 |
| output (x1) | stack buffer (sp+0x6cb0) | 0x88b97c–0x88b980 |
| frame_samples (w2) | **864**, hardcoded | 0x88b9a0: `mov w2, #0x360` |
| input (x3) | PCM + accumulated offset | 0x88b994, 0x88b9a4 |
| bitrate (w4) | from 0xdbf2e8, per-frame lookup | 0x88b99c: `ldr w4, [x20, #744]` |

Return value = encoded frame size in bytes. The bitrate is set per-frame from
lookup tables (F11), enabling adaptive bitrate.

Confirms T2.2 (864 samples, previously from probing) and T3.1 (1 frame per packet —
the loop encodes frames one at a time and dispatches each).

## F11 Bitrate lookup tables (T2.4 — from .rodata)

Three tables indexed by `sample_mode` (0–3), selected by runtime flags:

**UHQ mode** (table at 0xf06e0, when `[0xdb8673] == 1`):

| Index | Bitrate | Notes |
|---|---|---|
| 0 | 584 000 | max quality |
| 1 | 584 000 | |
| 2 | 291 000 | mid quality |
| 3 | 152 000 | low quality |

**Basic mode, flag=1** (table at 0xf1d90, when `[0xdb8672] bit 0` set):

| Index | Bitrate |
|---|---|
| 0–2 | 128 000 |
| 3 | 88 000 |

**Basic mode, flag=0** (table at 0xf0570):

| Index | Bitrate |
|---|---|
| 0–1 | 192 000 |
| 2 | 128 000 |
| 3 | 88 000 |

**Default** (no table match): 328 000.

**Key finding**: Samsung's UHQ sub-bitrates are **584k / 291k / 152k**, NOT
442k / 250k. Our uhq-442 and uhq-250 profiles pass bitrate values the encoder
doesn't expect — this explains why they produce silence.

## F12 Quality-to-rate table (T2.4 — from disassembly)

In `a2dp_vendor_ssc_set_current_encoder_info` (0x88a6dc–0x88a74c), quality
field at `[x0+0x14]` maps to encoder sample rate stored at 0xdbf304:

| Quality | Rate (Hz) |
|---|---|
| 2 | 48 000 |
| 4 | 88 200 |
| 8 | 96 000 |
| 16 | 176 400 |
| 32 | 192 000 |
| other | 44 100 |

Quality values are power-of-2 flags matching Android audio quality levels.
Quality 8 = UHQ 96 kHz.

## F13 bits_per_sample from sink capabilities (T2.4 — from disassembly)

In `a2dp_vendor_ssc_set_current_encoder_info` (0x88a754–0x88a784), the sink
capability byte at `[x0+0x18]` is checked in priority order:

| Cap bit | Condition | bits_per_sample |
|---|---|---|
| bit 3 (0x08) | bitrate_limit | **34** |
| bit 2 (0x04) | hifi / 24-bit | 32 |
| bit 1 (0x02) | UHQ | 24 |
| none | — | 16 |

Our Buds3 Pro respond with capability 0x3e = `0b00111110` → bit 3 set → **bits=34**.
This value is stored at 0xdbf308 and passed to `ssc_encoder_create`.

The value 34 is NOT a PCM bit depth — it's a mode flag to the encoder. The actual
PCM input is S24_32 (24-bit samples in 32-bit containers, scaled to 2^29).

## F14 Global state layout (partial)

| Address | Size | Content |
|---|---|---|
| 0xdb8620 | 8 | Encoder .so handle (dlopen) |
| 0xdb8628 | 8 | ssc_encoder_create fn ptr |
| 0xdb8630 | 8 | ssc_encoder_destroy fn ptr |
| 0xdb8638 | 8 | ssc_encode fn ptr |
| 0xdb8640 | 0x6ce0 | Main encoder state struct |
| 0xdb866c | — | Encoder info sub-struct (sample_rate[+0], bits[+4], channels[+5], bitrate_lim[+6], is_uhq2[+7]) |
| 0xdb8670 | 1 | Sample format byte (0x18=24-bit) |
| 0xdb8673 | 1 | UHQ flag (1=UHQ mode active) |
| 0xdb8684 | 4 | Accumulated PCM bytes |
| 0xdb8688 | 4 | bytes_per_tick |
| 0xdbf2d0 | — | Bitrate management sub-struct |
| 0xdbf2e8 | 4 | Current bitrate for ssc_encode |
| 0xdbf2f0 | 4 | Encode-call counter |
| 0xdbf2f4 | 1 | Dual-play flag |
| 0xdbf304 | 4 | Encoder sample rate (from quality table) |
| 0xdbf308 | 1 | Encoder bits_per_sample (34/32/24/16) |
| 0xdbf318 | 4 | Timer interval ms (10 for UHQ) |
| 0xdbf320 | 8 | Encoder handle (primary) |
| 0xdbf328 | 8 | Encoder handle (dual-play) |

## F15 Listening test results (AC5 / AC6 — 2026-10-21)

With 96 kHz graph rate fix (F6) + bits=34 (F13) both active:

- **UHQ-584**: bass timing fixed, continuous playback stable. User confirms no offbeat bass.
- **UHQ-291**: plays correctly (was silent as uhq-442). Bass slightly less stable at
  high volume/pitch, expected at lower bitrate.
- **UHQ-152**: plays correctly (was silent as uhq-250). More audible compression artifacts.
- **bits=34**: user confirms audible improvement over bits=24.
- **SSC High (basic-229)** and **AAC**: no regression.

All acceptance criteria met.

## F16 Post-encode gain processing (T4.1 — from disassembly)

At 0x88ba94–0x88bbfc in `send_frames`, after all frames in a tick are encoded:

1. Load two float gains from encoder-state struct offset 72 (left, right channels).
2. If **both** gains == 1.0 → skip entirely (branch to 0x88bbfc). Normal playback
   always takes this path.
3. If any gain ≠ 1.0:
   - Check sample format byte at 0xdb8670 == 0x18 (packed 24-bit).
   - If 24-bit: call 0x88cf4c(buffer) — unpacks S24_3LE to S32, applies float gain
     per-sample, repacks. Allocates 2 × 3456-byte temp buffers (one per channel).
   - If not 24-bit: inline fixed-point gain (fcvtzs + mul).
4. After gain: store updated gain value at struct offset 72.

This is a **volume ramp** for BT transport volume changes (fade-in, soft transitions).
Not a quality-affecting DSP step. We do not replicate it — PipeWire handles volume upstream.

Additionally, for basic mode (not UHQ), at 0x88bc18–0x88bd28 Samsung unpacks S24_3LE
PCM (3 bytes/sample) to S32LE (4 bytes/sample) using NEON TBL instructions before the
next encode iteration. Our plugin feeds S24_32 (32-bit containers) natively, so this
step is unnecessary.

## F17 Adaptive bitrate mechanism (T5.1 — from disassembly)

The per-frame bitrate selection in `send_frames`:

1. Read `sample_mode` byte from bitrate-management struct offset 0 (0xdbf2d0).
2. Clamp to range 0–3: `csel w20, w8, #3, cc` at 0x88b298.
3. Check UHQ flag at 0xdb8673:
   - If UHQ == 1: load from table at 0xf06e0 (UHQ table: 584k/584k/291k/152k).
   - If UHQ == 0: check another flag at struct base.
     - Flag bit 0 set: table at 0xf1d90 (basic-1: 128k/128k/128k/88k).
     - Flag bit 0 clear: table at 0xf0570 (basic-0: 192k/192k/128k/88k).
4. Selected bitrate stored at 0xdbf2e8, passed to `ssc_encode` as the 5th arg.

`sample_mode` update mechanism:
- Incremented at 0x88c828 (`str w9=[w19+1], [x24]`) — called by the BT stack when
  link quality degrades (counter-based: mode 0→1→2→3 as conditions worsen).
- Reset to 0 at 0x88e518 and 0x88e728 — called when conditions improve or on init.
- Comparison points: w19 == 15 and w19 == 24 trigger special handling (likely
  accumulated-degradation thresholds).

Our plugin uses fixed bitrate (index 0 always). To implement adaptive: monitor BT RSSI
or `org.bluez.MediaTransport1` properties; map to sample_mode 0–3; update the bitrate
passed to ssc_encode per-frame.

## F18 Encoder rate: 44100 vs 48000 for basic mode

Samsung passes rate=44100 to `ssc_encoder_create` for basic mode (F9 at 0x889d14).
Our plugin passes 48000. Comparison (30 frames of 1 kHz sine, bits=34, 229 kbps):

| Rate | Frame size | Mode | Effective bitrate | Duration per frame |
|------|-----------|------|-------------------|--------------------|
| 44100 | 576 B | f4 | 235 kbps | 19.6 ms |
| 48000 | 576 B | f4 | 256 kbps | 18.0 ms |

Same frame sizes and modes; the encoder produces structurally identical output. The
difference is temporal: at 44100, each 864-sample frame spans 9% more audio time,
giving the encoder more coding budget per unit of time. For 44.1 kHz source material,
using rate=44100 also eliminates PipeWire's 44.1→48 kHz resampling.

Plugin support: `SSCENC_BASIC_RATE=44100` env var in sscenc.conf. Requires 44100 in
PipeWire `clock.allowed-rates`.

## F19 Samsung's send_frames scheduling model (Phase 3 — from disassembly)

`a2dp_vendor_ssc_send_frames` (0x88aa08) uses **elapsed-time accumulation**:

1. **Timestamp delta** (0x88aac8–0x88aaec):
   - Read last_timestamp from `[encoder_info + 36]` (0xdb8690).
   - If valid and current > last: `elapsed_us = current - last`.
   - Fallback (first call or time backward): `elapsed_us = timer_interval_ms × 1000`.
   - Store current timestamp for next call.

2. **Bytes accumulation** (0x88abd4–0x88ac38):
   - `bytes_per_tick_divisor` at `[encoder_info + 0x6cac]` (0xdb8d18).
   - `new_bytes = (field_28 × elapsed_us) / (divisor × 1000)`.
   - `accumulated += new_bytes` → stored at `[encoder_info + 24]` (0xdb8684).
   - Logs: `"counter {} (bytes per tick {})"`.

3. **Frame size selection** (0x88ac74–0x88acdc):
   - UHQ + 32-bit: 6912 bytes (864 × 4 × 2). Dual-play: 13824.
   - Basic + 24-bit packed (S24_3LE): 5184 bytes (864 × 3 × 2).
   - Basic + 16-bit: 3456 bytes (864 × 2 × 2).

4. **Encode loop** (0x88acdc–0x88ad70):
   - While `accumulated >= frame_byte_size`: encode one frame, subtract frame_byte_size.
   - Logs: `"SCHED FRAME TX ({})"`.
   - All frames encoded this tick are dispatched in sequence.

5. **Timer interval** (0xdbf318):
   - UHQ: explicitly set to **10 ms** at 0x88a4e8.
   - Basic: READS existing value (not explicitly set in encoder_init). After memset, this
     is 0. But only used as fallback — subsequent calls use actual elapsed time.

**Key difference from PipeWire**: Samsung decouples frame scheduling from the audio source
quantum. The BT stack's own timer drives send_frames; bytes accumulate based on wall-clock
time, and frames are encoded whenever there's enough data. PipeWire's media-sink ties
encoding to the graph quantum delivery, so if the quantum doesn't align with 864 samples
at the codec rate, the flush timer drifts against data delivery.

**At 44100 Hz basic (24-bit packed)**:
- Frame = 5184 bytes. At 44100 × 3 × 2 = 264600 bytes/sec → 19.59 ms per frame.
- With 10 ms timer ticks: ~2646 bytes/tick → need ~2 ticks (20 ms) per frame.
- The accumulator naturally produces 1 frame every ~2 ticks, with the remainder
  carrying over. This handles the non-integer alignment gracefully.

## F20 Buds decode basic-mode SSC at fixed 48 kHz (Phase 3 — from live test)

Testing basic-229 with `SSCENC_BASIC_RATE=44100`, PipeWire graph at 44100 Hz,
quantum=864 (exact frame alignment):

- **Symptom**: audio sounds **sped up** (~8.8%) with periodic silence gaps.
- **Explanation**: buds decode 864-sample basic-mode frames at 48000 Hz DAC rate
  regardless of encoder input rate. Each frame plays in 18.0 ms (864/48000) but arrives
  every 19.59 ms (864/44100) → buds finish playback before next frame, gap accumulates.
- **Speed ratio**: 48000/44100 = 1.088 → 8.8% faster, ≈1.5 semitones pitch shift.
- **Frame headers identical**: `ff ee 0d f4` at both 44100 and 48000 — buds cannot
  distinguish rates from the bitstream.

**Conclusion**: native 44100 Hz basic-mode SSC is NOT viable. The buds always play basic
frames at 48 kHz. Samsung's phone likely resamples 44100→48000 before encoding despite
passing `rate=44100` to `ssc_encoder_create` — that parameter probably only affects internal
bitrate/timing math, not the decoder's playback rate. The resampling happens in Android's
AudioFlinger before the A2DP encoder stage.

This makes Phase 3 objective O1–O3 (AVDTP timing fix for 44100 Hz) moot. The resampler
quality=10 path (44.1→48 kHz) is the correct approach for 44.1 kHz sources.

## F21 UHQ listening test with 96k source and resampler quality=10 (T1.2)

Tested UHQ-584 with three source types:

| Source rate | Graph rate | Resampling path | Result |
|-------------|-----------|-----------------|--------|
| 96 kHz FLAC | 96000 | none | **Excellent** — no noise, no artifacts, native path |
| 44.1 kHz FLAC | 96000 | 44.1→96k (2.177:1) | Less codec noise than basic-229 (584 vs 229 kbps) but resampling artifacts audible |
| 44.1 kHz FLAC on basic-229 | 48000 | 44.1→48k (1.088:1) | More codec noise, better resampling (smaller ratio) |

The BT driver (UHQ at 96k) forces the graph to 96k via `allowed-rates`. Metadata
`clock.rate` shows 48000 (default) but actual driver Format confirms 96000.

**Conclusion**: UHQ is the right mode for 96k hi-res content. For 44.1k music, basic-229
is better because 44.1→48k resampling (1.088:1) is much cleaner than 44.1→96k (2.177:1).
Resampler quality=10 helps both paths but can't overcome the large ratio on UHQ.

Workflow: `ssc-switch high` for daily listening, `ssc-switch uhq` for hi-res.

## F22 Double resample path: UHQ-584 for 44.1k content (Phase 2 — from live test)

UHQ-584 with graph locked at 48 kHz (allowed-rates=[48000]) produces the best quality
for 44.1 kHz source material:

| Path | Resample chain | Quality |
|------|---------------|---------|
| basic-229, graph 48k | 44.1→48k (1.088:1) | 229 kbps, some codec noise |
| UHQ-584, graph 96k | 44.1→96k (2.177:1) | 584 kbps, resampling artifacts |
| **UHQ-584, graph 48k** | **44.1→48k (1.088:1) + 48→96k (2:1)** | **584 kbps, no audible artifacts** |

The BT media-sink does 48→96k internally when the graph doesn't match the codec rate.
The 2:1 integer ratio is nearly lossless. Combined with PipeWire's quality=10 resampler
for the 44.1→48k step, the double resample path produces cleaner output than either
the direct 44.1→96k path or the 229 kbps basic mode.

User confirmed: "much cleaner now definitely, without any white noise at all."

**New default**: UHQ-584 with graph locked at 48k, replacing basic-229.

## F23 UHQ bitrate tiers vs Samsung marketing claims

Samsung markets SSC UHQ as "~512 kbps up to 2,304 kbps." The 2,304 kbps figure is
exactly uncompressed stereo 24-bit/48 kHz PCM (48000 × 24 × 2 = 2,304,000 bps). From
our RE of the S24U encoder's bitrate table (F11), the actual operating tiers are:

| Tier | Bitrate | sample_mode index |
|------|---------|-------------------|
| Max  | 584 kbps | 0, 1 |
| Mid  | 291 kbps | 2 |
| Low  | 152 kbps | 3 |

584 kbps is the real maximum over classic A2DP with the S24U encoder blob. Possible
explanations for the marketing figure:
1. **SSC HiFi** — a newer codec mode (distinct from SSC UHQ) introduced with S24/Buds3 Pro
   that may use a different encoding path with higher bitrate tables, not yet RE'd.
2. **Newer encoder blobs** — S25/S26 may have additional bitrate table entries.
3. **BT 5.4 / LE Audio** — higher throughput transport unavailable over classic A2DP.
4. **Marketing ceiling** — "up to uncompressed PCM rate" is technically true but not the
   operating bitrate.

No evidence of hidden higher-bitrate e-modes in the S24U blob. All bitrate table entries
at 0xf06e0 are accounted for: 584k, 584k, 291k, 152k (4 entries, indexed by sample_mode 0–3).

## F24 UHQ tier quality comparison for 44.1k content (listening test)

Tested all three UHQ tiers with graph locked at 48 kHz (double resample path F22):

| Profile | Bitrate | Graph | Resample chain | User assessment |
|---------|---------|-------|----------------|-----------------|
| UHQ-584 | 584 kbps | 48k | 44.1→48k + 48→96k | "much cleaner, without any white noise at all" |
| UHQ-291 | 291 kbps | 48k | 44.1→48k + 48→96k | "clean double resample but with some white-noise" |
| UHQ-152 | 152 kbps | 48k | 44.1→48k + 48→96k | "time-machine sound effects feeling" (codec artifacts) |
| basic-229 | 229 kbps | 48k | 44.1→48k | codec noise floor (same as AAC baseline) |

Ranking for 44.1k content: **UHQ-584 > UHQ-291 > basic-229 > UHQ-152**.

UHQ-584 at 48k graph is definitively the best path for 44.1k content. The double resample
(1.088:1 + 2:1 integer) at 584 kbps eliminates both the codec noise floor of basic-229
and the resampling artifacts of direct 44.1→96k. UHQ-291 is usable but reintroduces
some noise. UHQ-152's compression artifacts make it worse than basic-229.

## F25 Two independent control layers in the SSC audio path

The SSC audio setup on Linux has two independent control layers:

| Layer | What it controls | Changed via | Latency |
|-------|-----------------|-------------|---------|
| SSC profile | Codec mode + bitrate (basic-229, uhq-584, etc.) | sscenc.conf + WirePlumber restart | ~15s BT reconnect |
| Graph rate | PipeWire resample path (48k locked vs 96k unlocked) | pw-metadata `clock.allowed-rates` | Instant, no audio cut |

When on a UHQ profile, only the graph rate needs toggling for hi-res content — instant
and seamless. When on a non-UHQ profile (AAC, basic), both layers must switch to play
96k content, incurring the 15s reconnect.

ssc-auto handles both layers automatically:
- Already on UHQ → rate toggle only (instant)
- On AAC/basic → saves current profile, switches to uhq-584 (15s), unlocks 96k,
  restores original when hi-res ends (15s), with 30s cooldown to prevent thrashing

State survives WirePlumber restarts via `$XDG_RUNTIME_DIR/ssc-auto.saved`.

## F26 Encoder does NOT resample internally — rate param controls bit allocation only

**Hypothesis** (disproved): Samsung's encoder resamples internally when given rate ≠ codec
native rate (e.g., 44100→96k for UHQ). This would explain why the double resample path
works well (F22) — the encoder was "designed for it."

**Why we missed this during Phase 3**: Phase 3 investigated AVDTP **transport timing** —
whether PipeWire's quantum-aligned scheduling vs Samsung's elapsed-time accumulation (F19)
caused the 44100 Hz dropouts. When D20 (quantum=864) eliminated timing as the cause, and
F20 showed buds decode at fixed 48 kHz, we concluded "Samsung resamples before encoding"
and closed Phase 3. We attributed the resampling to Android's AudioFlinger (upstream of
the BT stack) but never tested the encoder blob itself. The key test — feeding identical
PCM with different rate parameters — was never run.

**Test**: Fed the S24U encoder identical PCM samples with different rate parameters.

### Test 1: rate=44100 vs rate=48000 (same PCM)

| Mode | rate=44100 | rate=48000 | Match |
|------|-----------|-----------|-------|
| UHQ-584 | 1279B | 1279B | **BYTE-IDENTICAL** |
| UHQ-291 | 716B | 716B | **BYTE-IDENTICAL** |
| UHQ-152 | 376B | 376B | **BYTE-IDENTICAL** |
| basic-229 | 576B | 576B | **BYTE-IDENTICAL** |

The encoder produces **byte-identical output** at rate=44100 vs rate=48000 for all modes.
The rate parameter has zero effect when both values are ≤48000.

### Test 2: rate=96000 vs rate=48000 (same PCM)

| Mode | rate=96000 | rate=48000 | Ratio |
|------|-----------|-----------|-------|
| UHQ-584 | 660B | 1279B | 1.94× |
| UHQ-291 | 330B | 716B | 2.17× |
| UHQ-152 | 174B | 376B | 2.16× |

Headers (bytes 0–9) identical between rates; content diverges from byte 10. Frame size
approximately doubles at non-96k rates — the encoder allocates more bits per frame because
each 864-sample frame covers more audio time (18 ms vs 9 ms).

### Test 3: unsupported rates

| Rate | Result |
|------|--------|
| 88200 | **SEGFAULT** |
| 176400 | **SEGFAULT** |
| 192000 | **SEGFAULT** |

The encoder only accepts 44100, 48000, and 96000 Hz. Other rates from Samsung's quality
table (F12) crash the S24U blob.

### Conclusions

1. **The encoder does NOT resample.** It compresses 864 raw PCM samples as-is. The `rate`
   parameter is a bit-allocation mode switch, not a content transformation.

2. **Rate has two modes**: rate ≤ 48000 → "basic" frame size; rate = 96000 → "UHQ" frame
   size (~half). Within the ≤48000 range, 44100 and 48000 are treated identically.

3. **Samsung's rate=44100 for basic mode (F9) is cosmetic.** The encoder ignores the
   difference between 44100 and 48000. Samsung's AudioFlinger resamples 44.1→48k before
   the A2DP encoder stage; the rate=44100 param is either legacy or for timing math
   outside the encoder (F19 bytes_per_tick calculation).

4. **The resampling pipeline** (Samsung vs ours):
   - Samsung: AudioFlinger(44.1→96k, 2.177:1) → encoder(rate=96k, 660B frames) → buds
   - Our setup: PipeWire(44.1→48k, 1.088:1) + BT-sink(48→96k, 2:1) → encoder(rate=96k, 660B) → buds
   - Both produce the same 660B E-mode frames the buds expect. The resampling approaches
     differ — Samsung does one step at 2.177:1, we split into two steps with a 2:1 integer
     second stage — but we cannot claim one sounds better than the other without a controlled
     A/B comparison on the same buds. Samsung's AudioFlinger resampler is proprietary and
     may be well-optimised for 2.177:1. Our listening tests show no artifacts, but that is
     not a comparative claim against Samsung's pipeline.

5. **The double resample quality (F22) is NOT because the encoder was "designed for it".**
   It works because the encoder is a pure codec with no rate intelligence — it compresses
   whatever 96k PCM it receives. The quality of the final audio depends entirely on the
   upstream resampler, which is external to the encoder on both Samsung and Linux.
