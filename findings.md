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

**Addendum (2026-10-26, Phase 4).** `ssc_encode`'s header table (F35) knows more E-mode
rates than the sample_mode table above: 250k, 308k, 442k and **886k** (tag `e4`). The
highest is 886k. No path that sets sample_mode reaches it, and it has never shown up in a
capture. Nothing in the encoder comes near 2,304k. At 96 kHz, 2,304 kbps would be 2,592
bytes per 864-sample frame, against ~657 at 584k.

Classic BR/EDR can't carry it either. This is inferred from the Bluetooth spec, not
measured: the fastest packet, 3-DH5, holds 1,021 bytes in 5 slots plus 1 return slot
(3.75 ms). That is ≈ 2.18 Mbps one way in theory, before L2CAP/AVDTP overhead and
retransmissions, so A2DP in practice gets ~1.0–1.5 Mbps. 2,304,000 also equals 24 bit ×
96 kHz × 1 channel. Either way it is a PCM-rate figure, not an operating bitrate.

Verdict: for this encoder over classic A2DP, the 2,304k figure is a marketing ceiling;
the real live maximum is 584k. Still not ruled out: a separate "SSC HiFi" path in a newer
blob or over LE Audio (explanations 1–3 above). Neither has been examined.

Explanation 2 is ruled out: the S26U encoder is byte-identical to the S24U one (D2). In the
S26U stack, "HiFi" is only sink capability bit 0x04, which selects `bits_per_sample=32` for
the same encoder (F13). Our Buds3 Pro set the higher-priority bit 3, so they run bits=34.
What bits=32 changes inside the encoder is untested. The encoder does differ by device: the
A31 blob (README, sha256 `b923b408…`) has basic modes only and no UHQ. So "S24U and S26U
share the encoder" holds; "nothing differs from flagship to non-flagship" does not.

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

## F27 The SSC encoder is a fork of fixed-point CELT (libopus) (Phase 4)

**Evidence (S24U blob, sha256 90c4c1cb…4ce7, identical to S26U):**

1. **Exported names** are libopus CELT internals with an `ssc_` prefix: `ec_enc_init`,
   `ec_encode`, `ec_enc_bits`, `ec_enc_uint`, `ec_enc_done`, `ec_enc_patch_initial_bits`,
   `ec_enc_shrink`, `ec_laplace_encode`, `ec_tell_frac`, `compute_allocation`, `alg_quant`,
   `encode_pulses`, `quant_all_bands`, `quant_coarse_energy`, `quant_fine_energy`,
   `quant_energy_finalise`, `compute_band_energies`, `normalise_bands`,
   `spreading_decision`, `hysteresis_decision`, `patch_transient_decision`, `haar1`,
   `stereo_itheta`, `pitch_downsample`, `pitch_search`, `remove_doubling`,
   `pitch_xcorr`, `comb_filter`, `mdct_forward`, `fft`, `_lpc`, `_autocorr`,
   `tf_select_table`, `eMeans`, `custom_mode_create`. **Fixed-point build**: `frac_div32`,
   `rcp`, `rsqrt_norm`, `bitexact_cos` exist only in libopus `FIXED_POINT`.
2. **`ssc_encoder_get_size`** (0x9f0c) computes
   `ch × (overlap×4 + nbEBands×6 + 4096) + 0xdc`. This is libopus
   `opus_custom_encoder_get_size` (`in_mem` + `COMBFILTER_MAXPERIOD`=1024 prefilter
   samples + **three** `opus_val16` per-band arrays: oldBandE/oldLogE/oldLogE2).
   libopus **1.2** added a fourth (`energyError`), so the base predates 1.2.
   *(Corrected 2026-10-26: originally written as "1.3". F29 pins the base to 1.1.)*
3. **`ssc_encoder_create`** (0xa0b8) = `opus_custom_encoder_create`: mode lookup
   (frame 864), malloc(get_size), `ssc_encoder_init`, error code -7 = `OPUS_ALLOC_FAIL`.
4. **`ssc_custom_mode_create`** (0x11ac8) returns static modes (like libopus
   `static_modes_fixed.h`): 0x1b9c0 for 44100/48000 and 0x1ba40 for 96000. Any other
   rate returns NULL with err=-1, which explains the F26 segfaults at 88.2k/176.4k/192k
   (the caller doesn't check NULL).

**Mode parameters** (both modes share overlap and frame structure):

| Field | 44.1/48k mode | 96k mode | Stock CELT 48k |
|---|---|---|---|
| frame size | 864 | 864 | 960 |
| short MDCTs × size | 8 × 108 | 8 × 108 | 8 × 120 |
| overlap | 108 | 108 | 120 |
| nbEBands / effEBands | 22 / 21 | 22 / 21 | 21 / 21 |
| MDCT n | 1728 | 1728 | 1920 |
| alloc vectors | 11 × 22 (shared table @0x3f5e, 242 B) | 11 × 22 (same) | 11 × 21 |
| PulseCache size | 346 | 334 | — |

Band edges (eBands in 108-bin units, edges in Hz):
- **48k** (@0x3f30): 0,1,2,3,4,5,6,7,8,10,12,14,16,20,24,28,34,40,48,60,72,84,**102** →
  top edge 22.7 kHz. The 22nd band (18.7–22.7 kHz) is beyond `effEBands`, coded by the
  Samsung `*_lastband*` functions.
- **96k** (@0x5988): 0…12,14,16,20,24,30,36,54,72,90,**108** → bands 19–21 are
  16–24 kHz, 24–32 kHz, 32–40 kHz and the last band is 40–48 kHz. At 96 kHz the
  864-sample frame lasts 9 ms, so frequency resolution is 55.6 Hz/bin, half the 48k
  mode's 27.8 Hz/bin. Three of 22 bands carry only ultrasonic content.

**Samsung-only code** (no libopus equivalent): DRC/limiter (`SSC_DRC_Init/SetPar/Exe`,
`CalculateLimitGain`, `set_DRC_limit_gain`, `Biquad_Narrow_int2int_stereo`, `powercalc`,
`Soundbooster_DRC_Fs`), headroom (`normal_headroom`, `uhq_headroom`, `headroom_process`,
`headroom_gain`, `margin`), framing (`get_header_from_bitrate`,
`get_uhq_info_from_bitrate`, `set_limit_gain_from_bitrate`, `set_data_header`,
`set_data_frame_header`, `previous_uhq_index`), 22nd band
(`CalculateBandEnergyLastBand`, `ssc_compute_band_energies_for_lastband`,
`last_band_info_calc`), `tone_detect`, prefilter helpers (`getPitchIndexGain`,
`getPitchFilterThreshold`, `prefilter_gain_calculation`). **The encoder contains a
bitrate-dependent limiter and headroom stage before CELT analysis.** It is a prime suspect
for level-dependent noise or pumping, to be measured with the O3 decoder.

**Consequence:** the rebuild is libopus CELT plus a bounded set of deltas, not a
from-scratch codec. A matching CELT **decoder** is also feasible, which removes the
"no SSC decoder exists" limitation that bounded Phases 1–3.

## F28 Phase 4 groundwork notes (session 2026-10-26, not yet acted on)

Loose findings gathered while scoping Phase 4. Each is either evidence for a later task or a
trap to avoid.

1. **Allocation table is custom, 11 × 22.** At 0x3f5e, 242 bytes, one column per band
   including the 22nd. Values differ from libopus `band_allocation`. For example, row 1
   is `118,110,103,93,86,…` where stock is `90,80,75,69,63,…`. The last column is 0
   except in row 10 (=1), so the 22nd band gets almost no static allocation and is coded
   by the `*_lastband*` path. (Reading it with stride 21 gives a misleading diagonal
   pattern, so use 22.)

2. **`CELTMode` layout differs from libopus** (tentative, from the relocations at
   0x1b9c0 / 0x1ba40):

   | Off | 48k mode | 96k mode | Tentative meaning |
   |---|---|---|---|
   | +0x00 | 44100, 108 | 96000, 108 | Fs, overlap |
   | +0x08 | 22, 21 | 22, 21 | nbEBands, effEBands |
   | +0x10 | →0x3f30 | →0x5988 | eBands (no `preemph[4]` before it, unlike libopus) |
   | +0x18 | 8, 108 | 8, 108 | nbShortMdcts, shortMdctSize (`maxLM` field absent or elsewhere) |
   | +0x20 | 11 | 11 | nbAllocVectors |
   | +0x28 | →0x3f5e | →0x3f5e | allocVectors (shared) |
   | +0x30 | →0x4050 | →0x59b6 | per-mode (logN?) |
   | +0x38 | →0x4142 | →0x5aa8 | per-mode (?) |
   | +0x40 | 1728, 3 | 1728, 3 | mdct.n, mdct.maxshift |
   | +0x48, +0x50 | →0x1bac0, →0x1bad8 | same | kfft (shared) |
   | +0x58 | →0x4170 | →0x4170 | shared (window or trig? overlap and n match across modes) |
   | +0x60 | 346 | 334 | cache.size |
   | +0x68..+0x78 | 3 ptrs | 3 ptrs | cache.index / bits / caps |

   Which of +0x30/+0x38/+0x58 is logN, window or trig is unresolved. Settle it in T1.4 by
   reading the code that dereferences each field.

3. **Codec state partly lives in process globals, not the encoder handle.** `.bss`/`.data`
   exports: `drc_struct`, `DRC_ChNum`, `DRC_ModeFlag`, `Soundbooster_DRC_Fs`,
   `previous_uhq_index`, `in_shared`, `_pre_shared`, `_pcmpower`, `count_for_debug`,
   `headroom_gain`, `margin`, `win_square`. Consequences:
   - Two encoder handles in one process share DRC/headroom/prefilter state, including
     Samsung's dual-play handle (0xdbf328, F14).
   - State can survive `ssc_encoder_destroy` → `create`. The D13 "warm-up init is a no-op"
     test ran in one process, so it may not have covered this.
   - Golden vectors must come from a **fresh process per run**, and the native port must
     reproduce the global-state semantics to stay bit-exact.

4. **`smull32_x86_scalable`** (0xfff8) is just `(int64)a*b >> 32`, but its name implies
   Samsung keeps an **x86 build** of this code. So the `*_asm` NEON functions
   (`ssc_comb_filter_*_asm`, `ssc_amp2Log2_asm`, `ssc_bitexact_cos_asm`) likely have
   portable C equivalents with identical results. The native port can be plain C.

5. **`print_enc_fields`** (0x15664) is a debug dumper that calls `printf` (9 `printf` call
   sites in the blob). It may print internal encoder fields and could serve as a free
   state probe for the O2 harness.

6. **There is a third encoder build.** `~/src/ssc-research/samsung-ssc-reversed` (Vinchethescript, GitHub)
   ships a blob with sha256 `9248e264…b461`. That differs from both S24U (`90c4c1cb…`) and
   A31 (`b923b408…`). Its `encoder.c` is 5647 lines of raw Hex-Rays output. The author calls
   it non-functional and NEON-broken. At most it is a navigation aid, and it was built from
   a different binary.

7. **Reusable assets already on disk:**
   - `~/src/ssc-capture/uhq584_getlucky.frames`: S24U blob output, UHQ-584 at 96k, format
     `u32 len | bytes` per frame (first frame 660 B, `ff e0 0d e3`), ~6.6k frames
     (~60 s). This is the first decoder test input for T3.3. The source PCM can be
     regenerated with `frame_stats.py`.
   - `~/src/ssc-capture/frame_stats.py` / `probe_encoder.py`: the blob-helper pipe
     protocol is argv `so rate ch bits bitrate`, stdin `u32 n | int32[2n]` PCM, stdout
     `i32 len | bytes`. That's enough for T2.2 golden frames without new tooling.
   - `~/src/ssc-capture/golden/*.golden`: SSCOnWindows frames. **Not** valid UHQ references
     (F4); use only for 48k basic mode.
   - `~/src/ssc-capture/btsnoop_hci.log`: real phone→buds capture (A31, basic mode). It is
     a decoder test input for F-mode frames only.

8. **Six qemu core dumps** of `ssc_blob_helper` in `~/src/ssc-capture/` (2026-10-21, three
   at 03:48 and three at 17:01). These are probably the F26 Test 3 runs (88.2k/176.4k/192k).
   That fits F27: `ssc_custom_mode_create` returns NULL for those rates and
   `ssc_encoder_create` dereferences it unchecked (`ldp w10,w8,[x0,#4]` at 0xa0e8). This is
   not verified from the cores. They can be deleted once confirmed (~62 MB).

9. **User report 2026-10-26 contradicts F21.** F21 rated UHQ-584 with a native 96k source
   and 96k graph "excellent, no noise". The user now hears noise and artifacts on that
   same path with the S26U-extracted encoder (byte-identical to S24U). We don't know yet
   what changed: content, volume, radio environment, or plugin/PipeWire version.
   Phase 4 O0 should note the exact track, volume and conditions.

10. **Tooling gap.** No decompiler is installed (no Ghidra, radare2 or rizin). Phase 1–3
    analysis was objdump plus `tools/fn.py`/`xref.py`. `ssc_encode` alone is ~16 KB of
    code (largest function), with CELT's `celt_encode_with_ec` likely inlined.

## F29 libopus base pinned: v1.1 (T1.2)

Method: walk the libopus tags (clone at `~/src/opus`). For each boundary between releases,
find a source change that survives into machine code, then check the blob for it. Many
boundaries are unreadable because Samsung rewrote exactly the lines that changed (see
"Samsung changes" below). The decisive checks are those in functions Samsung left
alone.

| Boundary | Source change | Blob evidence | Verdict |
|---|---|---|---|
| ≥ 1.1 | `patch_transient_decision`, exported `celt_preemphasis` appear in the 1.1 cycle | both exported | ≥ 1.1 cycle |
| < 1.2 | `get_size` gains a 4th per-band array (`energyError`) in 1.2 | `ssc_encoder_get_size` (0x9f0c) has `nbEBands×6` = 3 arrays | < 1.2 |
| < 1.1.1 | fixed-point `compute_band_energies`: 1.1 uses `shift = ilog2(maxval)-10`; 1.1.1 uses `ilog2-14+((logN>>3)+LM+1)>>1` with split SHL/SHR loops | 0x7820: `clz` then `sub #21` (= -(ilog2-10)), no logN load, `bandE = 1 + VSHR32(sqrt(sum), -shift)` with the `maxval ≤ 0x3ff` branch (0x7868) | **≤ 1.1** |
| ≥ 1.1-rc | `_celt_autocorr`: beta has `ac0 = 1+n` and always copies to `xx`; rc has `ac0 = 1+(n<<7)` and the `overlap==0` path reads `x` directly | 0x11070 loads ac0 init `120833` (rodata 0x27e0) = `1 + (944<<7)`; no windowed copy. n=944 = (COMBFILTER_MAXPERIOD 1024 + 864)/2, the CELT pitch-analysis length | **≥ 1.1-rc** |
| rc / rc2 / rc3 / 1.1 | rc→rc2: asserts only. rc2→rc3: `celt_preemphasis` temp types, `hysteresis_decision((opus_val16)(equiv_rate/1000))` | both sites rewritten by Samsung (below) | indistinguishable |

**Conclusion: base = libopus 1.1 (Dec 2013).** Use tag `v1.1`. It is codegen-equivalent
to 1.1-rc…rc3 for every function Samsung kept. Toolchain per the ELF note: Android NDK
r25c (clang 14.0.7). That only matters for instruction-level comparison, not for
bit-exact output.

**Samsung changes noticed along the way** (feed into T1.3):
- `ssc_preemphasis` (0xade4) is **rewritten**. It has no pre-emphasis filter (consistent
  with the mode struct having no `preemph[]`, F28.2). It only de-interleaves stereo
  **int32** input (8-byte stride) and shifts left by **12**, or by **4** when argument 5 ≠ 0.
  Both land at 2^27 full scale (16-bit input << 12, or 24-bit input << 4). Our plugin
  feeds 2^29 full scale (shift 6 from S24_32), so something upstream in `ssc_encode`
  (the headroom stage?) must remove ≥ 2 bits first. **Headroom scaling is load-bearing
  and must be traced in T4.7.**
- `patch_transient_decision` (0xa170) uses the 1.1.2-style `start`-based loops, but with
  **start hard-coded to 13** and the mean taken over channel 0 only (divisor `end-14`).
  So transient patching only considers bands ≥ 13.
- `quant_coarse_energy` (0x13340): the `lfe` branch is removed. `max_decay =
  MIN(16<<10, nbAvailableBytes<<7)` and `loss_distortion` cap 200 are as in 1.1.
- `ssc_encode` intensity decision (0xe974): `hysteresis_decision(510, thresholds@0x3d18,
  hysteresis@0x3d46, 22 - x, …)`. The `equiv_rate/1000` argument is **hard-coded to
  510 kbps** regardless of the real bitrate.
- `ssc_compute_band_energies` (0x776c): `M` is folded to 8 (`eBands<<3`), so every frame
  is analysed as LM=3 (8 × 108 short blocks). The per-channel loops are unrolled, and the
  second channel's offset is read from mode `+0x1c` = shortMdctSize. That confirms that
  F28.2 row.
- `_ssc_autocorr` (0x11044) is specialised to n=944 with a constant trip count.

## F30 Function classification vs libopus v1.1 (T1.3, first pass)

*Historical: every "?" row is settled and the full table, every export by name, is F48.*

Method: Ghidra 12.2 headless decompile of the S24U blob (`~/src/ssc-codec`,
`tools/ghidra-decomp.sh`); libopus v1.1 CELT built fixed-point for aarch64 with clang
(`re/ref/celt_v1.1.o`); `tools/fnmatch.py` pairs functions by name and compares size,
callees and immediate constants. Unexported statics (`FUN_*`) were identified by their
callers and callees. Borderline cases were read by hand (decompile next to source).

**Call graph.** The blob has **no indirect calls** (zero `blr`, no relocations to function
symbols). The only entry points libbluetooth_jni uses are create/destroy/encode (F9).
Everything not reachable from `ssc_encode`/`ssc_encoder_create` is dead code.

**Samsung's recurring change is specialisation, not rewriting.** Stock functions are
recompiled with fixed parameters: **C=2** (channel loops unrolled), **start=0**,
**LM=3** (`M=8`), **lfe=0**, and decoder paths removed. Example: `ssc_quant_fine_energy`
(0x113c9c) is libopus `quant_fine_energy` line for line, with `start` and `C` dropped from
the signature. Specialised functions produce the same output as stock code called with
those parameters, so the rebuild uses libopus source directly.

Legend: **S** stock (identical) · **F** stock logic, specialised (fixed params) ·
**M** modified logic · **N** Samsung-only · **D** dead (no callers) · **?** pending
differential test (O2). Confidence: ✔ verified by reading · ~ inferred from
size/callees/constants.

| Blob function | libopus v1.1 | Class | Evidence |
|---|---|---|---|
| ssc_ec_enc_init, _encode, _encode_bin, _enc_bit_logp, _enc_icdf, _enc_bits, _enc_uint, _enc_shrink, _enc_done, _tell_frac, ec_laplace_encode | entenc.c / laplace.c | S ~ | sizes within 8 B; constants Jaccard 0.67–1.0; `SSC_COPY8`/`SSC_CLEAR` replace `OPUS_MOVE`/`OPUS_CLEAR` |
| FUN_0010f88c | ec_enc_carry_out / ec_enc_normalize (static) | S ~ | only caller ec_enc_uint; 264 vs 272 B |
| ssc_haar1, ssc_rcp, ssc_sqrt, ssc_rsqrt_norm, ssc_cos_norm, ssc_frac_div32, ssc_hysteresis_decision | bands.c / mathops.c | S ~ | sizes ±8 B, constants 0.64–1.0 |
| ssc_encode_pulses | encode_pulses | S ~ | 212 vs 200 B, same callees |
| ssc_quant_fine_energy | quant_fine_energy | F ✔ | start=0, C=2 |
| ssc_quant_energy_finalise | quant_energy_finalise | F ~ | 556 vs 524 B, constants 0.71 |
| ssc_compute_band_energies | compute_band_energies | F ✔ | 1.1 formula (F29), M=8, C=2 |
| _ssc_autocorr | _celt_autocorr | F ✔ | n=944, lag 4, overlap 0 (F29) |
| ssc_quant_coarse_energy + FUN_00113734 (=quant_coarse_energy_impl) | quant_coarse_energy | F ✔ | lfe branch gone = lfe 0; rest as 1.1 (F29) |
| ssc_patch_transient_decision | patch_transient_decision | **M** ✔ | start hard-coded 13, mean over ch 0 only (F29) |
| ssc_pitch_search + FUN_001126bc (=find_best_pitch) | pitch_search | **M** ✔ | calls Samsung `tone_detect` |
| ssc_encoder_init | opus_custom_encoder_init | **M** ✔ | calls SSC_DRC_Init / SSC_DRC_SetPar; no ctl |
| ssc_custom_mode_create | opus_custom_mode_create | **M** ✔ | 2 static modes only, Samsung tables (F27, F28) |
| ssc_encode | celt_encode_with_ec | **M** ✔ | inlines transient/tf/dynalloc/prefilter/vbr analysis; adds DRC, headroom, 22nd band; equiv_rate fixed 510 (F29); does not call preemphasis |
| ssc_alg_quant + FUN_001167d0 (=exp_rotation) | alg_quant | ? | 3640 vs 1536 B; calls cos_norm/rcp directly |
| ssc_quant_all_bands + FUN_00109038 (=quant_band) + FUN_00109460 (=quant_partition, compute_theta inlined) + FUN_00109ca4 (=intensity_stereo) | bands.c | ? | callees as expected; stereo/theta paths need diff test |
| ssc_stereo_itheta, ssc_normalise_bands, ssc_spreading_decision | bands.c | ? (likely F) | same callees, sizes fit C=2 unrolling |
| ssc_compute_allocation | compute_allocation | ? (likely F) | decoder branches gone (enc-only); 2696 vs 3516 B |
| ssc_pitch_downsample, ssc_remove_doubling, _ssc_lpc, ssc_pitch_xcorr_c1/_c2 | pitch.c / celt_lpc.c | ? | same callees; xcorr split into two Samsung variants |
| ssc_comb_filter + `ssc_comb_filter_{const,function1..4}_asm` | comb_filter | ? | NEON re-implementation; bit-exactness vs C to test |
| ssc_fft, ssc_mdct_forward | opus_fft / clt_mdct_forward | ? (likely F) | N=1728 only (radix 4/3, 432=4·4·3·3·3), twiddles from Samsung mode |
| ssc_amp2Log2 + ssc_amp2Log2_asm | amp2Log2 | ? | NEON path plus C path |
| SSC_COPY8/16/32, SSC_CLEAR | OPUS_COPY / OPUS_CLEAR | N (trivial) | memcpy/memset wrappers |
| SSC_DRC_Init, SSC_DRC_SetPar, SSC_DRC_Exe, CalculateLimitGain, set_DRC_limit_gain, powercalc, Biquad_Narrow_int2int_stereo | — | **N**, live | called from ssc_encode / init: **in-encoder limiter/DRC** |
| normal_headroom, uhq_headroom | — | **N**, live | called from ssc_encode |
| ssc_compute_band_energies_for_lastband | — | **N**, live | 22nd band (F28.1) |
| tone_detect | — | **N**, live | called from pitch_search |
| ssc_preemphasis, headroom_process, get_header_from_bitrate, get_uhq_info_from_bitrate, set_limit_gain_from_bitrate, set_data_header, set_data_frame_header, last_band_info_calc, CalculateBandEnergy, CalculateBandEnergyLastBand, getPitchIndexGain, getPitchFilterThreshold, prefilter_gain_calculation, print_enc_fields, smull32_x86_scalable, ssc_ec_enc_patch_initial_bits | — | **D** | no callers. Header/bitrate logic must be inlined in ssc_encode; these copies are references only |

**Size of the real RE work** (live, non-stock code):
- Samsung-only: DRC/limiter ≈ 1.6 KB, headroom ≈ 2.7 KB, last-band energies ≈ 1.5 KB,
  tone_detect ≈ 0.4 KB.
- Modified: `ssc_encode` (16.6 KB; most of it inlined stock analysis), `pitch_search`,
  `patch_transient_decision`, `encoder_init`.
- About 15 "?" functions to confirm against stock with the O2 differential harness.

**Correction to F28.5:** `print_enc_fields` is dead code (no callers), so it can't be
used as a live probe without patching a call in.

## F31 Differential testing results, batch 1 (O2 / T1.3 "?" rows)

Harness: `~/src/ssc-codec/harness` (`make run`; `make STOCK=1 run` for pure v1.1). One
aarch64 binary under qemu dlopens the blob and links libopus v1.1 celt/ with the patch
series `harness/patches/`. Each test feeds identical pseudo-random inputs to both sides,
**restricted to the domain the encoder actually uses**, and compares every output bit for bit.

**Samsung deltas found (each one a patch on libopus v1.1):**

| Patch | Change | Evidence | Fixes (fail on stock → pass) |
|---|---|---|---|
| 0001 | `MULT32_32_Q31` is an **exact 64-bit** product `(int64)a*b>>31`, not v1.1's 16-bit partial-product approximation | `frac_div32` variant test: exact Q31 20000/20000, stock 14100/20000. Stock MULT16_32_Q15 is already exact | frac_div32, _celt_lpc, stereo_itheta (via celt_atan2p), remove_doubling |
| 0002 | **24-bit range coder**: `EC_CODE_BITS 24` (not 32) | `ssc_ec_encode` (0xf850) normalises while `rng ≤ 0x8000`, carries out `val>>15`, masks `val` to 23 bits; init rng=2^23, nbits_total=25. ec_ctx layout identical to libopus | ec (all ops incl. laplace, done, overflow), encode_pulses |
| 0003 | **PVQ search precision**: `Rxy² >> 7` kept as 32-bit (stock: `MULT16_16_Q15` → int16), score compare in 64-bit | `ssc_alg_quant` 0x16244–0x16254: `mul w1,w1,w1; lsr #7; mul x…; cmp x4,x3` | alg_quant (20000 cases incl. exp_rotation + encode_pulses) |
| 0004 | `remove_doubling`: pitch gains clamped to Q15ONE; g1's `yy` sum and `xx*yy` in 64-bit | 0x12c28/0x12eb0 `cmp #0x8000 → mov #0x7fff`; 0x12da4 64-bit `add x9,x8,x9` / `mul` | **code-read only**: no test input found where it changes the output (decisions are robust to it) |

**Consequence for the decoder (O3):** the 24-bit range coder is a bitstream-level change.
A libopus CELT decoder rebuilt with `EC_CODE_BITS 24` uses the matching `ec_dec`
constants. Combined with Samsung's mode tables, that is the expected route to an SSC
decoder.

**Confirmed bit-exact (stock logic + patches):** celt_rcp, celt_sqrt, celt_rsqrt_norm
(exhaustive), celt_cos_norm (exhaustive), bitexact_cos (exhaustive 1…16383), frac_div32,
the whole ec_enc API + ec_laplace_encode, haar1, hysteresis_decision, encode_pulses,
alg_quant (+ inlined exp_rotation), stereo_itheta, celt_pitch_xcorr (both variants),
_celt_lpc, _celt_autocorr, pitch_downsample, remove_doubling.

**Samsung signature specialisations** (needed to call or rebuild them):
- `stereo_itheta(X, Y, N)`: `stereo` fixed to **1**.
- `_ssc_lpc(lpc, ac)`: order 4. `_ssc_autocorr(x, ac)`: n=944, lag 4, no window.
- `ssc_pitch_xcorr_c1(x, x, ac)` = `celt_pitch_xcorr(x, x, ac, 940, 5)` for autocorr. It
  returns 0, not maxcorr, which is unused by its caller.
- `ssc_pitch_xcorr_c2(x, y, xcorr, len, max_pitch)`: general, but len must be a multiple
  of 8 and max_pitch a multiple of 4 (no tail code); the only call is (216, 244).
- `ssc_pitch_downsample(pre0, pre1, x_lp, len)`: pre1 must equal pre0+len (maxabs
  scans 2·len from pre0), C=2, len=1888.
- `ssc_bitexact_cos_asm(x, 16384-x, &imid, &iside)`: two lanes, 32-bit results. It differs
  from stock only for itheta < 64 or > 16320 (cos rounds to 32768, stock wraps to −32768).
  Whether compute_theta can reach those values is still to be checked.
- Call sites in `ssc_encode`: `pitch_search(pitch_buf+512, pitch_buf, N, 979, &T)` and
  `remove_doubling(pitch_buf, 1024, 15, N, &T, prefilter_period, prefilter_gain)`:
  stock run_prefilter arguments.

**More global state (extends F28.3):** alg_quant's `iy[]`/`y[]` are global arrays (144
entries each, in `in_shared` at 0x24dac / 0x24fec). The blob **crashes for band size
N > 144**, which SSC never produces (widest band 18 << 3). remove_doubling's
`yy_lookup` is also global. The prefilter buffer `pre` is reached through the GOT, so
it is global as well.

## F32 Session notes 2026-10-26 (loose findings from the O2 batch, not yet acted on)

1. **Patch series vs upstream libopus** (checked against `~/src/opus` history):

   | Patch | In later upstream? |
   |---|---|
   | 0001 exact `MULT32_32_Q31` | **Yes, identical expression.** `d6a44c55` (2015-11-20, `OPUS_FAST_INT64` flavours), shipped in 1.2 |
   | 0002 `EC_CODE_BITS 24` | **No.** 32 in every release through 1.6.1. Samsung-only, bitstream-level |
   | 0003 PVQ `Rxy²>>7`, 64-bit compare | **No.** 1.2–1.5.2 still `MULT16_16_Q15(Rxy,Rxy)`. Samsung-only |
   | 0004 pitch-gain clamp | **Clamp only.** `ddea3a6f` (2016-06-18) adds `MIN32(g,Q15ONE)` inside a new `compute_pitch_gain()` with different (sx/sy) normalisation. The blob keeps 1.1's x2y2/rsqrt normalisation (bit-exact with 1.1 + 0001) plus the clamp. Samsung's own fix, not a backport |

   Open question: did Samsung cherry-pick the 2015 `OPUS_FAST_INT64` commit, or write the
   exact product themselves? Its other macros (`MULT16_32_Q15/Q16/P16`) are mathematically
   identical to 1.1's, so this binary can't tell. The base is still 1.1 (F29:
   `compute_band_energies` predates 1.1.1).

2. **`bitexact_cos` divergence is unreachable** (F31 open point), *assuming Samsung's
   compute_qn is stock*: v1.1 `compute_qn` asserts `qn ≤ 256` and even, so a nonzero
   `itheta` is a multiple of 16384/qn ≥ 64, and the 0 / 16384 endpoints are special-cased
   before bitexact_cos. Inputs < 64 or > 16320 never occur. To confirm when
   `quant_partition` (FUN_00109460, compute_theta inlined) is tested in batch 2.

3. **Equivalent-by-construction differences** (blob ≠ stock source, same output):
   - alg_quant `pulsesLeft > N+3` fallback: the blob skips the two `yy` updates. They
     are dead, because pulsesLeft becomes 0 and the search loop doesn't run.
   - remove_doubling: the blob drops `MAX32(0, yy)` on `yy_lookup` (exact integer
     sliding sums never go negative). Stock's `else if (T1<2*minperiod)` 0.9 threshold is
     unreachable in both (T1<2·min implies T1<3·min).
   - `MULT16_32_Q15(celt_rsqrt_norm(t), xy)` calls rsqrt_norm twice in both builds
     (macro expands its argument twice). The paired calls in the blob are not a change.

4. **Domain constraints the blob imposes** (every harness test must respect them, and
   so must any future fuzzing of the native rebuild):
   - range coder `ft ≤ 2^15` (24-bit state; larger loops forever in `ec_enc_normalize`)
   - PVQ band size `N ≤ 144` (global `iy`/`y`, crash beyond), and (N,K) inside the PVQ
     table (`pvq_ok` in difftest.c)
   - `pitch_xcorr_c2`: len % 8 == 0, max_pitch % 4 == 0
   - `pitch_downsample`: pre0/pre1 contiguous
   - pitch_buf amplitude ≲ 2^11 (as pitch_downsample normalises). Louder input
     overflows stock's 32-bit energies, which the blob does in 64-bit (0004)

5. **Stock-build contrast** (`make STOCK=1 run`, 2000 iters): on pure v1.1, frac_div32,
   lpc, stereo_itheta, remove_doubling, remove_doubling_loud, ec, encode_pulses and alg_quant
   **fail**; everything else passes. Every failure is fixed by a specific patch. Caveat:
   `pitch_downsample` passes even on stock although its internal `_celt_lpc` differs
   without 0001, so that test is **not sensitive** to 0001.

6. **Unexplained:** the first `pitch_downsample` test (two separate static arrays, the
   blob reading 2·len from the first) **segfaulted**. With one contiguous buffer it passes.
   Reading past the first array should land in the second, not fault. The cause is
   unknown, and it's harmless for the encoder (its caller always passes contiguous
   buffers).

7. **Tooling lessons:**
   - Ghidra's signature recovery is unreliable on NEON-heavy functions. For example,
     `ssc_alg_quant` showed `undefined1[16]` vector "parameters", and calls to
     `ssc_ec_enc_bits` dropped the `bits` argument. Ground truth for a prototype comes
     from the function entry (`mov` of x0–x7) plus its call sites in `ssc_encode`.
   - A heuristic "read-before-write" argument scanner was tried and dropped (it
     misclassifies compares and pair loads).
   - Unexported statics are folded into the previous symbol by objdump.
     `tools/fnmatch.py` splits them at Ghidra's `FUN_` boundaries.
   - The full difftest suite runs in ~1.5 s under qemu. The early "slow run" was the
     ec test spinning in the blob's `ec_enc_normalize` (ft > 2^15, item 4).

## F33 Differential testing results, batch 2: mode-dependent functions (O2 / O4)

Harness `~/src/ssc-codec/harness` (commits 22f572b, 78c4d6c, e76b53b). The reference
`CELTMode` is built **at runtime** from `ssc_custom_mode_create(48000|96000)` (OD13),
so nothing Samsung-derived sits in the source. **31/31 tests pass bit-exact** (~2 s under
qemu). Every new patch fails on its own removal.

**1. `CELTMode` layout resolved** (closes F28.2's unknowns, T1.4):

| Off | Field | Evidence |
|---|---|---|
| +0x28 | allocVectors, **shared** 11×22 (0x3f5e) | selected when alloc-struct `+0x180` == 0 |
| +0x30 | allocVectors, **per-mode** 11×22 (48k 0x4050, 96k 0x59b6) | selected when `+0x180` ≠ 0 |
| +0x38 | logN (22 × int16) | equals `log2_frac(width, BITRES)` for every band, both modes |
| +0x40 | mdct.n=1728, maxshift=3 | |
| +0x48/+0x50 | mdct.kfft[2] (shift 0 and 3, shared) | `ssc_mdct_forward` picks `kfft[shift>>1]` |
| +0x58 | mdct.trig: **Q31 int32**, 433 entries = `round(2^31·cos(2πk/1728))` | 433/433 exact |
| +0x60..+0x78 | PulseCache size/index/bits/caps | bits and index from stock `compute_pulse_cache` (except item 2); **caps are int32 `init_caps(LM=3,C=2)` output** (`(caps+64)·C·N>>2`), not libopus' byte table |

There's **no window field**. The MDCT window is a Q31 global at 0x3d80 equal to
`round(2^31·w(i))` with CELT's window `w(i)=sin(π/2·sin²(π/2·(i+½)/108))` (108/108 exact).
`win_square` (.data 0x1ff80, used by the comb filter) is `round(2^31·w(i)²)` computed in
double (108/108). **All MDCT/window tables are formula-generated**, so the native rebuild
can compute them instead of extracting them (OD17).

Shared vs per-mode alloc: the shared table is the 48k per-mode table except for rows 8–10.
The 96k per-mode table is very different (fewer bits above band 17, nonzero last
columns in rows 5–10).

2. **48k PulseCache quirk:** `cache.index[LM][band 20]` points to the **width-18** entry
   (band 21's) at every LM (−1…3), although band 20 is 12 wide (72→84). It looks like a
   cache generated for an older `…,72,90,108` layout and never regenerated. Every
   bits2pulses on band 20 at 48k uses the wrong pulse table. That's self-consistent with
   the buds only if their decoder has the same table. **The decoder (O3) and the rebuild
   must keep it.** The 96k cache matches stock exactly.

3. **Tables that are stock:** `ssc_eMeans_tmp` == eMeans; `e_prob_model[3][0/1]` (inter
   0x5dbf, intra 0x5d91); `small_energy_icdf`; `ssc_tf_select_table` == stock row
   LM=3 only (8 bytes); exp2_table8 (0x2760); LOG2_FRAC_TABLE (0x5df0).

4. **Per-function results** (Samsung prototype → class):

   | Function | Samsung signature | Class | Patch / note |
   |---|---|---|---|
   | compute_band_energies | (m, X, bandE, end), C=2, M=8 | F ✔ | |
   | normalise_bands | (m, freq, X, bandE, end) | F ✔ | called **in place** (freq == X) |
   | amp2Log2 | (m, n, bandE, bandLogE), channel stride n | F ✔ | NEON does band pairs, so n must be 22 |
   | spreading_decision | (st, X, update_hf, end) | **M** ✔ | **0005**: hf weight 64, `hf_sum>>=3`, no narrow-band early return |
   | quant_coarse_energy (+impl) | (m, –, end, eBands, oldE, budget, error, enc, nbAvailBytes, force_intra, &delayedIntra) | **M** ✔ | **0006**: loss_distortion over `C·end` contiguous entries; two_pass=1, loss_rate=0; 150-byte intra buffer (unreachable, item 9). impl is F (C=2, LM=3, lfe=0) |
   | quant_fine_energy / quant_energy_finalise | stock minus start, C | F ✔ | |
   | compute_allocation (+interp_bits2pulses inlined) | (st, alloc*, enc, end, signalBandwidth=end−1) | F ✔ | args packed in a frame struct (item 5); table selector `+0x180` |
   | quant_all_bands (+quant_band, quant_partition/compute_theta, intensity_stereo, stereo_split inlined) | (m, end, X, cm, bandE, pulses, shortBlocks, spread, dual, intensity, tf_res, total, balance, ec, codedBands, &seed, 1) | **M** ✔ | **0007**: band 21 coded from its real data (stock redirects i ≥ effEBands to norm scratch). Y = X+864; norm scratch after Y |
   | comb_filter (+5 NEON kernels) | (y, x, T0, T1, N, g0, g1, overlap) | **M** ✔ | **0008**: tapset 0 only; Q31 `win_square`; Q31 tap gains; one rounded 64-bit sum per sample |

   Equivalent by construction (no patch needed): Samsung's quant_band has no
   `lowband`/`LM`, but the v1.1 *encoder* never folds (`resynth = !encode`), so that is
   dead-code removal. Samsung's stereo_split `(x·0xb504)>>16` equals stock's
   `(x·23170)>>15` exactly. spreading_decision starting at band 8 is equivalent because
   bands 0–7 are 1 bin wide in both modes. **F32.2 is confirmed:** compute_theta's
   inlined compute_qn is stock, and quant_all_bands is bit-exact over 20k cases, so the
   bitexact_cos divergence is unreachable.

5. **Allocation frame struct** (`sp+0x3e0` in ssc_encode, passed to compute_allocation,
   quant_fine/finalise, quant_all_bands): `+0x000` total, `+0x004` ebits[22], `+0x05c`
   bits/pulses[22], `+0x0b8` cap ptr, `+0x0c0` offsets[22], `+0x118` fine_priority[22],
   `+0x170` alloc_trim, `+0x174` dual_stereo, `+0x178` balance, `+0x180` alloc-table
   selector.

6. **Encoder-state offsets seen so far** (`st` = ssc_encode param_1; `*st` = mode):
   `+0x10` force_intra, `+0x14` end, `+0x1c` rng (seed), `+0x20` spread_decision,
   `+0x24` delayedIntra, `+0x28` tonal_average, `+0x2c` lastCodedBands, `+0x30`
   hf_average, `+0x34` tapset_decision, `+0x50` intensity. This feeds OD8 and T4.8.

7. **The coded band count varies per frame.** Allocation, coarse energy, spreading and
   normalise get `end = st->end − w26`, where `w26 ∈ 0…3` comes from Samsung's last-band
   energy loop (`compute_band_energies_for_lastband`, thresholds 12001 / 30001, ssc_encode
   0xcf34–0xd114). The same loop rewrites frame-header bytes 2–3. So the 22nd band is
   adaptive bandwidth signalled in the header. Needed for T3.1 (header parse) and T4.3.
   Because of 0006, loss_distortion differs from stock whenever end < 22.

8. **Samsung's MDCT is a rewrite** (`ssc_mdct_forward(l, in, out, overlap, shift,
   stride)`, not yet tested). It has v1.1's structure (fold, pre-rotate with
   `t[i<<shift]`/`t[(N4−i)<<shift]`, N/4 FFT, post-rotate), but with a Q31 window, Q31
   trig, 64-bit products with rounding (`+2^30>>31`, NEON `rshrn`), and an exact
   rotation by π/(4N) (`0x7fffff22/0xee4bb` for N=1728, `0x7fffc88c/0x7725c6` for 216)
   instead of v1.1's small-angle `sine` correction. Only shifts 0 and 3 exist. It uses
   `_pre_shared` and 0x212ac as scratch. `ssc_fft` (3404 B, radix 4/3 for 432/54) has a
   custom compact state (24-byte kfft structs at 0x1bac0/0x1bad8).

9. **Domain constraints added** (to F32.4):
   - amp2Log2: `bandE < 2^29` (SHL32(bandE,2) overflows; both sides wrap differently).
     Whether the encoder can reach it depends on headroom (T4.7).
   - tf_res follows the block type: `{0,−2,0,−3}` long, `{3,0,1,−1}` transient. A
     positive tf_change with B=1 makes B=0 and hangs exp_rotation (stock and blob).
   - quant_coarse_energy: `error[]` beyond `end` is undefined (an uninitialised
     error_intra copy on both sides). The intra pass peaks at ~80 bytes even for
     extreme inputs, so 0006's 150-byte limit is unreachable (like 0004).
   - quant_all_bands: X buffer ≥ 4·864 entries (norm scratch after Y).

10. **Tooling:**
    - `DT_WATCHDOG=<s>` (default 120 s per run via the Makefile) prints a backtrace;
      resolve with `aarch64-linux-gnu-addr2line -f -i`. Batch-mode gdb over qemu's
      gdbstub could not interrupt a running target, so the watchdog replaced it.
    - Leaving a patch out: `make B=build/noN PATCHES="…"`.
    - `pkill -f` self-matched again when the pattern and the qemu command shared one
      shell line. Use `pidof qemu-aarch64`.
    - Stock-run caveats: under `STOCK=1`, quant_all_bands hangs (band 21 is
      quantised from uninitialised scratch) and is cut off by the watchdog.

**Progress estimate (end of session, 2026-10-26).** By live encoder code bytes, 32.2 KB of
62.4 KB (**~52%**) is verified bit-exact. Remaining: `ssc_encode` 16.6 KB (mostly inlined
stock analysis: transient/tf/dynalloc/prefilter/VBR, plus framing), ssc_fft 3.4 KB,
last-band energies 1.5 KB, headroom 2.7 KB, pitch_search + find_best_pitch 1.9 KB, mdct
0.9 KB, DRC/limiter ~1.6 KB, patch_transient_decision 0.5 KB, tone_detect 0.4 KB,
encoder_init/create 0.6 KB. Weighted by the Phase 4 objectives (O0 0%, O1 ~90%, O2 ~40%,
O3 0%, O4 ~45%, O5/O6 0%), the phase is about **25–30%** done.

## F34 MDCT/FFT, last-band energies, header cutoff (batch 3)

Uncommitted on top of e76b53b: patch **0009** (`ssc_mdct.c`), the Makefile compiles files a patch creates, and three new difftests. **35/35 tests pass** bit-exact at 2000 iterations (~5 s under qemu).

**1. MDCT/FFT (T4.2, OD21).** New file, not a delta on `mdct.c` (OD21). Q31 window, trig and twiddles are formula-generated (OD17). Bit-exact against `ssc_fft` (432 and 54 points) and `ssc_mdct_forward` (shift 0 stride 1, shift 3 stride 8) on both modes, including our FFT run on the blob's own kfft state. Three fixes after the first transcription, all visible in the first failing run:

- `bitrev[]` is already an int32 index (`2 * complex index`). Indexing with `2 * bitrev[k]` scrambles the FFT.
- Q31 cannot hold −1. `sin`/`cos` of ±1 is stored as −2147483647, same clamp as libopus. The only twiddle that hit it was index 108 imag (`−π/2`).
- The second kfft field is the shift, 0 or 3, not a boolean. The 54-point state has 3. The FFT still branches on "zero vs not".

The blob's second radix-3 stage runs one block past the buffer (49×9 for 432, 7×9 for 54). That block starts at the first sample after `nfft` and is not reproduced. Outputs inside `[0, nfft)` match without it.

**2. Last-band energies (T4.3).** `ssc_compute_band_energies_for_lastband(mode, X, bandE, count, uhq, idx)` is stock `compute_band_energies` on `count` equal slices of one band. No patch. `uhq == 0` slices the top band (live call: count 4). `uhq != 0` slices the band `idx` below the top, idx 0 being the top (live call: count 8, idx 0..3). Sample bounds are `8 * eBands[band + i/count …]`, so a 144-sample top band becomes four slices of 36 or eight of 18. Channel 1 is stored right after channel 0 (`bandE[i + c*count]`), not at stride `nbEBands`. Bit-exact on the live calls for both modes.

**3. Adaptive end and header bytes 2–3 (T3.1).** In `ssc_encode` at 0xcf34–0xd114. Energies land at `sp+0x190`. A slice is "hot" when channel0 + channel1 exceed the threshold.

- Non-UHQ, count 4, threshold 30001. Tests slices 3, 2, 1 (not slice 0) and adds 12, 8, 4 or 0 to header byte 2. `idx` stays 0, so `end` does not change.
- UHQ, count 8, threshold 12001, `idx` from 0 upward. Tests slices 7..0. The first hot slice sets a code of 8 (top slice) down to 1. If all four bands are cold, code = 0 and `idx` = 3. Header byte 2 += `idx * 4`. Header byte 3 keeps its low 5 bits; its top 3 bits become `code - 1` when code > 0, else 0. `end = st->end - idx`.

Bytes 0–1 are written as the halfword `0xeeff`. E-mode bitrates then overwrite byte 1 with `e0` (F35); F-mode leaves `ff ee`. Byte 3’s initial tag and the E/F split are in F35, not in this paragraph.

**4. OD18, alloc-table selector.** At the top of `ssc_encode` (0xb71c). Frame struct `+0x180` is 1 iff the bitrate argument is > 192000 (`0x2ee00`), else 0. 0 selects the shared alloc table (mode `+0x28`); anything else selects the per-mode table (`+0x30`). UHQ-584, UHQ-291 and basic-229 are above that line; UHQ-152 is below it.

**Progress estimate (end of batch 3).** 38.0 KB of 62.4 KB (**~61%**) of live encoder code is verified bit-exact. Still open: `ssc_encode` 16.6 KB, headroom 2.7 KB, pitch_search + find_best_pitch 1.9 KB, DRC/limiter ~1.6 KB, patch_transient_decision 0.5 KB, tone_detect 0.4 KB, encoder_init/create 0.6 KB. Weighted by objective (O0 0%, O1 ~90%, O2 ~45%, O3 ~15% — header known, no decoder yet — O4 ~60%, O5/O6 0%), the phase is about **35%** done.

## F35 Header bitrate tags and the E/F split (read from ssc_encode, not yet in a decoder)

The prologue at 0xb6cc–0xbc38 matches the captured headers in F4. `ssc_encode` writes `ff ee` first. A later store to byte 1 runs only for the E-mode bitrate set and forces that byte to `e0`.

**Byte 3, before the adaptive rewrite of its top 3 bits** (exact bitrate match):

| Bitrate | Byte 3 | Bytes 0–1 |
|---|---|---|
| 88 000 | `f0` | `ff ee` |
| 96 000 | `f1` | `ff ee` |
| 128 000 | `f2` | `ff ee` |
| 192 000 | `f3` | `ff ee` |
| 229 000 | `f4` | `ff ee` |
| 328 000 | `f5` | `ff ee` |
| 256 000 | `f6` | `ff ee` |
| 291 000 | `e0` | `ff e0` |
| 308 000 | `e1` | `ff e0` |
| 442 000 | `e2` | `ff e0` |
| 584 000 | `e3` | `ff e0` |
| 886 000 | `e4` | `ff e0` |
| 250 000 | `e5` | `ff e0` |
| 152 000 | `e6` | `ff e0` |

48k/229k captures are `ff ee .. f4` and 96k/584k captures are `ff e0 .. e3` (F4). The `bits == 24` test in this prologue does not change the tag.

The E-mode set is also the set that runs the 8-slice scan (F34.3). F-mode runs the 4-slice scan. That flag is not "sample rate is 96 kHz" and it is not "bitrate > 192000" (OD18). 152k is E-mode and below 192k; 229k, 256k and 328k are F-mode and above 192k.

**UHQ-152 skips the scan.** Its tag is `e6`. `ssc_encode` compares byte 3 with `e6` and, on a match, does not call `ssc_compute_band_energies_for_lastband` and does not touch bytes 2–3 again. It forces the amount subtracted from `st->end` to 4. `st->end` was stored as `nbEBands` (22) at encoder+0x14, so this path encodes 18 bands. The adaptive path subtracts `idx` (0..3) from the same base.

**Byte 3 after the 8-slice scan.** The top 3 bits become `code - 1` (code 8..1), or 0 when code is 0. The low 5 bits stay the tag. For 584k the tag is `e3`, so the eight results are `e3 c3 a3 83 63 43 23 03`. Code 0 and code 1 both leave `03`. That is the set F5 observed on 96k/584k (e3, 03, c3, 23, a3, 43, 63, 83). `idx` is added to byte 2 as `idx * 4`, which is what separates code 0 from code 1.

**Byte 2’s base is still unread.** Before the add, the prologue stores a byte built from the halfword at encoder+140 and the low 2 bits of encoder+142. 48k/229k captures keep byte 2 at `0d` for every frame (F4, F5), so on that material the base and the 4-slice choice do not vary. Reproducing the base is T4.8 work. The decoder only has to read the byte.

**Last-band edges are not “whatever mode is loaded”.** With the F-mode argument the blob uses one fixed end table, the 48k top band split into four (672, 708, 744, 780, 816). The start still comes from the mode’s `eBands[21]`. A 96k mode plus an F-mode bitrate would start at 720 and end on that 48k table. The difftest does not cover that pair. Live calls are 48k with F-mode and 96k with E-mode (eight slices of a 96k band). OD22.

## F36 Frame header parser and the first decoder (T3.1, start of T3.2)

Uncommitted, on top of the batch-3 tree: `harness/ssc_header.h`, patch **0010**, and difftest entries `header` and `decode`. `make -C harness run` is 37/37 green. `header` checks the parser, the size formula, and all 6666 frames of `~/src/ssc-capture/uhq584_getlucky.frames`. `decode` checks live `ssc_encode` lengths and that the patched decoder returns 864 samples. It does not claim range-coder sync.

**1. F35's "byte 1 forced to e0" is only the first store.** E-mode then writes `0xe0 | (frame_counter >> 4)`, so byte 1 is `e0`..`ef`. When the high nibble is `e`, byte 1 is `ee` and collides with F-mode; the bitrate tag decides. Byte 2 bits 0–1 stay 1 (the halfword at encoder+0x8e, initialised by the `0x10000` store). Bits 2–3 are `idx` in E-mode, or the F-mode 4-slice addend. Bits 4–7 are the frame counter's low nibble. The counter is the short at encoder+0x8c; it increments in the `ssc_encode` epilogue and wraps at 0xff. On the 6666-frame capture: every frame is 660 bytes, tag low 5 bits always `03`, byte 1 is `e0`..`ef`, byte 2 bits 0–1 are always 1.

**2. Frame size, confirmed against live encodes.** `bytes = bitrate * 864 / (Fs * 8)`. F-mode adds `(bytes * 3) >> 7`. Then force odd, and the return value is that plus 3 (4-byte header, payload is one less than the odd count). `Fs` is the mode rate: init stores the create rate at encoder+96, then overwrites it with 44100 when the create rate is exactly 48000. 96000 and 44100 stay. Checked: 584k/96k → 660, 152k/96k → 174, 229k created at 48000 → 576.

**3. What the decoder already matches.** The range coder starts at byte 4. There is no pitch tapset bit (patch 0008 / 0010). The transient bit on the wire is the inverse of the flag `ssc_encode` stores at sp+236: internal transient encodes 0, non-transient encodes 1, both with logp 3. TF logp is chosen from the internal flag (`4 then 5` when transient, `2 then 4` otherwise), so a stock `tf_decode` that trusts the wire bit uses the same logps. `ssc_tf_select_table` is only the LM=3 row (8 bytes at 0x2838). The encoder indexes it with `wire_bit * 4`, which is stock `tf_select_table[3][4*isTransient + …]`. Whether the frame writes TF at all is not the sp+224 guess below; F37 measured it. F-mode does, E-mode does not.

**4. Where sync still failed, before F37.** With tapset omitted and stock `tf_decode` on every frame, noise frames get through silence, pitch (absent on this noise), transient, coarse energy, TF, spread, dynalloc, trim and fine energy with `ec_get_error` still 0. The PVQ `ec_dec_uint` errors (N=3, K=128 and N=9, K=26) were a shifted read, not a bad codebook. Resolved in F37. The inverse MDCT is not in the tree: `compute_inv_mdcts` returns immediately when `mode->window` is NULL, so the PCM is not meaningful yet.

**Progress estimate.** Encoder bytes verified bit-exact are unchanged at 38.0 KB of 62.4 KB (**~61%**). By objective: O0 0%, O1 ~90%, O2 ~45%, O3 ~55% (range sync on the three live modes, no PCM yet; F37), O4 ~60%, O5/O6 0%. The phase is about **45%** done.

## F37 E-mode has no TF bits; range sync on the three live modes (T3.2)

The pulse-decoder mismatch was not a PVQ bug. `alg_quant` roundtrips at N=3, K=128, and `quant_all_bands` roundtrips a full 584k-shaped frame when the decoder is given the whole range buffer (front bytes and the raw bits at the end). The illegal `ec_dec_uint` values (`t=65750` against `V(3,128)=65538`, and `t=257` against `qn+1=257`) were later symbols read at the wrong place.

**1. What the live encoder writes, from the symbol trace.** Coarse energy's tell matches the decoder on every checked frame. The next symbols do not. On 96 kHz E-mode (584k and 152k, transient or not) the symbol after coarse is the spread icdf (`ftb=5`). There is no TF. On 48 kHz F-mode (229k) TF is stock `tf_decode`: a transient frame uses logp 2 then 4, a steady frame uses logp 4 then 5, then the tf_select bit, then spread. F36.3's "UHQ frames do write TF" came from reading sp+224 the wrong way around. They don't.

**2. The other setup bug that had to be fixed first.** The blob's `CELTMode+0x78` is the expanded int32 `init_caps(LM=3,C=2)` result. Stock `init_caps` indexes `cache.caps` as a byte table. Pointing the decoder at the int32 array reads off the end and invents caps, so the allocation (and then PVQ) diverges even when the signalling is right. `build_mode` keeps the blob's `cache.index` and `cache.bits` (48 kHz band 20, F33.2) and uses stock's byte caps. `compute_allocation` tests still pass the int32 caps in directly; only the decoder calls `init_caps`.

**3. What sync means now.** `decode` selects `CELT_SET_SSC_MODE` from the header: 2 for E-mode (no TF), 1 for F-mode (stock TF, still no tapset bit). On 24 frames each of 584k, 152k and 229k, `ec_get_error` stays 0 and the tell finishes within 1 bit of `8*(len-4)`. A one-bit gap is the range coder's ceiling, not spare payload. Inverse MDCT is still absent (`window == NULL`).

**Pitfall.** Initialising a decoder with `ec_range_bytes()` drops the raw end-bits. A single `ec_enc_uint` of 256 with `ft=257` then decodes as 257 and sets the range error. The packet length is the encoder's storage, and that is what the decoder must see.

## F38 Decoder PCM (T3.2 done, T3.3): synthesis, band 21, padding, and the input-scale gain riding

Uncommitted in `~/src/ssc-codec` on top of F37: patch **0010** extended, harness `build_mode` and the `decode` test rewritten. `make -C harness run` is 37/37 green. Music runs are report-only (`DT_MUSIC`, see the codec README).

**1. Synthesis.** The harness gives the decoder mode stock's Q15 window and a v1.1 `clt_mdct_init(1728, 3)` lookup, both formula-generated (OD17). Samsung's Q31 forward MDCT (patch 0009) keeps stock's 4/N scaling and no pre-emphasis (F29: `ssc_preemphasis` has no filter, `preemph[]` is all zero, so stock `deemphasis` is a pass-through). A stock inverse is enough: on every input the output is the input delayed by exactly **108 samples** (the overlap). I/O gain on synthetic input (±8000, `bits` 34 or 24 give the same result): 96 kHz `out = in / 256` (input `<<4` to 2^27, output `>>12`); 48 kHz `out = in` (input `<<12`, the 16-bit path).

**2. Band 21 was never denormalised.** `celt_decode_with_ec` clamps `effEnd` to `effEBands` (21), so the 22nd band was decoded from the stream (patch 0007 makes the encoder code it) and then dropped. 0010 now skips the clamp when `ssc_mode != 0`. Tones then come back in band 21 at 26 dB (584k).

**3. F-mode band 21 is folded by design (inferred).** At 48 kHz, band 21 decodes with the right energy (out/in 0.85–0.88) and no correlation, for noise and for a pure tone. Both 48 kHz alloc tables (`+0x28`, `+0x30`) hold 0 for band 21 in every vector except the top one, which holds 1. The 96 kHz per-mode table gives band 21 real bits (2, 10 in the top vectors). Range sync means the decoder runs the encoder's allocation, so the band is almost certainly spread/folded with no pulses. Pulse counts were not read directly. The test leaves this band out and prints it.

**4. Bits the decoder leaves unread are zero padding.** On tones every band reaches its cap. The 584k frames then end with `tell` ≈ 4253 of 5248 bits, and the payload holds a 124-byte zero run between the range-coded front (ends at payload offset 345) and the raw end bits. That is the CELT encoder's `ec_enc_done` padding, not a desync. The sync rule is now: no range error, and a gap > 1 bit must be covered by a zero run of at least `gap/8 - 1` bytes. Noise still lands within 1 bit.

**5. Reconstruction quality (`decode` test, 24 frames each).** Tones, one per band centre: every coded band comes back at gain 0.9–1.1 and 19–43 dB SNR (584k: 23–43 dB, bands 0–21; 152k: 25–41 dB, bands 0–17, silent above `end` 18; 229k: 19–35 dB, bands 0–20). White noise is coarse in the high bands (584k: 31 dB in bands 0–17, 4–8 dB in 18–21), as CELT allocates.

**6. Real capture.** All 6666 frames of `uhq584_getlucky.frames` decode in sync. 914 of them are padded, and `end` is 19 (3121 frames), 20 (3513), 21 (8) or 22 (24). The 16-bit output has rms 14084 (−7.3 dBFS) and 0.6 % of samples at full scale. Source PCM for that capture is unknown; the user says its Get Lucky source is a 44.1 kHz master.

**7. At the plugin's 2^29 input scale, the encoder rides the gain frame by frame, and nothing signals it.** Measured by encoding and decoding 20 s of a native 96 kHz/24-bit FLAC (Nirvana, *MTV Unplugged in New York*, SIDE-1 from 120 s, −20 dBFS rms) and of Get Lucky (−11.6 dBFS), 584k and 152k, fitting gain per frame at lag 108:

| Input full scale | Overall gain / SNR (584k) | Frame SNR p10 / p50 | Gain step between frames p50 / p90 / max |
|---|---|---|---|
| 2^29 (plugin, `SSCENC_INPUT_SHIFT=6`) | Nirvana 3.85 / 9.2 dB; Get Lucky 1.56 / 6.4 dB | 16 / 23 dB; 14 / 22 dB | 0.9 / 2.4 / 10.2 dB; 1.2 / 3.7 / 14.5 dB |
| 2^25 | | 24 / 35 dB; 16 / 25 dB | p90 1.0 / 3.0 dB |
| 2^24 | | 33 / 36 dB; 22 / 34 dB | p90 0.1 / 1.5 dB |
| **2^23 (plain 24-bit)** | **Nirvana 1.00 / 35.1 dB; Get Lucky 1.00 / 37.5 dB** | **33 / 36 dB; 35 / 41 dB** | **≤ 0.06 / 0.10 / 0.23 dB** |

At 2^29, per-frame gain falls as rms^−0.75 (a ~4:1 compressor around a −8 dBFS output median), with jumps up to 14.5 dB between consecutive 9 ms frames. 584k and 152k ride identically, so the gain is applied before quantisation: it is the headroom/limiter stage (T4.7). That answers F29's open point: `ssc_encode` does not remove 2 bits from a 2^29 input. It rides the gain instead. The range-coded payload is fully consumed (§4) and header bytes 0–3 are fully mapped (F36), so the gain is not signalled; the buds play the compressed signal. At 2^23 the encoder is linear and the decoded loudness matches the source.

The 2^29 choice traces to a comment in `config/ssc-pipewire-1.6.8.patch` ("as SSCOnWindows feeds it") and `plan.md` T2.3/discrepancy #3, which cites openssc's `encoder.c`. Nothing in these notes shows what the **phone** feeds. So this is either our bug, or Samsung's sound with a hot source. OD23.

**8. Listening after the OD23 change (user report, 2026-10-26): not a valid test, corrected the same day.** The user added `SSCENC_INPUT_SHIFT=0`, selected uhq with `ssc-switch uhq`, and reported it "still sounds wrong". The first handoff took that to mean the artifact is probably not the codec. That was wrong. `ssc-switch` takes the shift as an optional second argument that defaults to 6 (`config/bin/ssc-switch:61`). It calls `~/src/ssc-capture/set_ssc.sh PROFILE 6`, which deletes every `Environment=SSCENC_INPUT_SHIFT=` line and appends `=6` (`set_ssc.sh:7,15–16`; `ssc-switch:71` deletes it too). The live conf afterwards held `SSCENC_INPUT_SHIFT=6` plus a commented `#…=6` line, and wireplumber was restarted at 12:51. **So the buds were still fed at 2^29, and OD23 is untested.** To test shift 0: `ssc-switch uhq 0`. Pitfall: any manual edit of the shift is overwritten by the next `ssc-switch`/`set_ssc.sh` run unless the shift is passed as an argument.

**Not covered yet.** 48 kHz F-mode on music: synthetic input shows a `<<12` (16-bit) input path at 48 kHz, so 2^29 input there may be far hotter still. There is no PCM comparison for the capture, no spectrogram, and the basic-mode golden/btsnoop frames have not been through the decoder.

**Progress estimate.** Encoder bytes verified bit-exact are unchanged: 38.0 KB of 62.4 KB (**~61%**). By objective: O0 0 %, O1 ~90 %, O2 ~45 %, O3 ~80 % (T3.1, T3.2 done; T3.3 synthetic + 96k music + capture sync; T3.4 partly answered by §7, no per-band map), O4 ~60 %, O5/O6 0 %. The phase is about **50 %** done.

## F39 Listening A/B: the input shift is the artifact (OD23), and the runtime path is clean

**1. Listening (user, 2026-10-26, buds, uhq-584).** Track: Daft Punk "Get Lucky", 88.2 kHz hi-res file played in mpv. At `SSCENC_INPUT_SHIFT=6` (2^29) it "sounded terrible". At `ssc-switch uhq 0` (2^23, confirmed live by `ssc-switch status`: "input shift: 0") it "sounds good without audible artifacts". This is the first valid shift-0 test; F38.8's earlier attempt ran at 6. It matches the measurement in F38.7: at 2^29 the encoder's headroom stage rides the gain frame by frame (unsignalled steps up to 14.5 dB on this track); at 2^23 it is linear. One track, one listener, no blind A/B. The native 96 kHz source (Nirvana) and 44.1/48k content were not re-heard at shift 0.

**2. Runtime check during the shift-0 playback (`pw-top -b`, 30 × 1 s, `pw-dump`, user journal).**
- Encoder: Samsung's blob, still under qemu (`systemd-run … qemu-aarch64 … ~/.local/libexec/sscenc/aarch64/ssc_blob_helper …/s24u/libScalable_Encoder.so 96000 2 34 584000`). No part of `~/src/ssc-codec` is in the live path.
- Sink `bluez_output.E8_C9_13_75_5F_AD.1` (node 94): driver at rate 96000, quantum 2048 (21.3 ms), format S24_32LE, ERR 0 in all 30 samples, B/Q ≤ 0.09.
- mpv stream (node 99): S32LE at 88200 (`node.rate = 1/88200`), ERR 0, B/Q ≤ 0.04.
- User journal since the 12:51 wireplumber restart: no xrun/underrun/drop messages.
- Limit: pw-top times only the PipeWire cycle. Encode time inside the qemu helper is not visible here, so T0.1 is still open.

**3. Who resamples 88.2 → 96 kHz.** PipeWire's audioconvert (SPA's native resampler) inside mpv's stream adapter, at `resample.quality = 14`. mpv delivers 88.2 kHz natively. The graph cannot run at 88.2 kHz: `clock.allowed-rates = [ 48000 96000 ]` and the SSC sink only accepts 96000. The blob never resamples (D26). Samsung's own resampling on the phone happens in the Android audio stack, which is not in our blob. Since the same resampler path sounds bad at shift 6 and clean at shift 0, it is not this artifact's cause.

**4. Open from this.** Basic mode (48 kHz, 229k) takes the `<<12` 16-bit input path (F38.1). There, even shift 0 (S24 at 2^23) would be 8 bits hotter than the encoder's nominal input, and the plugin's `SSCENC_INPUT_SHIFT` only shifts left (0–8). Unmeasured on music; needs a right shift (or a negative shift) to test.

## F40 Basic mode (48 kHz, 229k) takes 16-bit input: at the plugin's 2^23 it rides the gain (F39.4)

Measured with the `decode` test's music path, now selectable by rate (`DT_MUSIC_RATE=48000`, or `44100` to create the encoder at 44100 as the phone does, F9). Inputs: 20 s from 120 s of Nirvana *MTV Unplugged* SIDE-1 (96k/24, −20 dBFS rms) and the 88.2k/24 album "Get Lucky", each resampled to 48 kHz with soxr. That is what PipeWire does in basic mode, and no native 48k file is at hand. Basic-229 with bits=34, gain fitted per frame at lag 108. "Gain/io" is decoded loudness relative to the source (1.00 = same dBFS).

| Input full scale | Nirvana: gain/io, SNR · frame SNR p10/p50 · gain step p90/max | Get Lucky: same |
|---|---|---|
| 2^29 (old plugin, shift 6) | 3.65, 9.9 dB · 17/24 dB · 2.1/8.8 dB | 1.36, 9.1 dB · 16/22 dB · 3.0/13.5 dB |
| **2^23 (plugin now, shift 0)** | **3.60, 9.8 dB · 17/24 dB · 2.1/9.0 dB** | **1.34, 9.2 dB · 16/23 dB · 3.1/13.6 dB** |
| 2^19 | 3.61, 9.7 dB · 17/24 · 2.1/9.4 | 1.34, 9.1 dB · 16/22 · 3.0/13.6 |
| 2^17 | 3.30, 14.5 dB · 22/29 · 1.1/5.3 | 1.34, 9.4 dB · 16/23 · 2.9/9.8 |
| 2^16 | 2.00, 29.2 dB · 27/30 · 0.15/0.32 | 1.29, 12.0 dB · 18/25 · 1.9/5.6 |
| **2^15 (16-bit)** | **1.00, 28.8 dB · 27/29 · 0.16/0.31** | **0.99, 26.4 dB · 23/29 · 0.15/0.51** |
| 2^14 | 0.50, 28.8 dB · 27/29 · 0.16/0.31 | 0.50, 26.4 dB · 23/29 · 0.15/0.43 |

**1. The 48 kHz mode's nominal input is 16-bit.** Linear (gain 1.00, steps ≤ 0.5 dB) at 2^15 and below; from 2^16 (Get Lucky) or 2^17 (Nirvana) up, the headroom stage rides the gain exactly as UHQ does above 2^23 (F38.7). Between 2^29 and 2^19 the output loudness hardly moves (gain/io 3.6 and 1.34 regardless of input): the stage is a leveller, so a hotter input changes nothing but the riding. Creating at 44100 instead of 48000 gives identical numbers (Get Lucky at 2^23 and at 2^15), so the scale follows the mode, not the create rate. It matches F38.1's synthetic I/O: 96 kHz shifts input `<<4`, 48 kHz `<<12`, both to Q27 full scale.

**2. So the plugin's basic modes were 8 bits hot even after D33**, and 14 bits hot before it (shift 6). Every basic-mode listening result so far was taken under heavy gain riding, including the README quality ranking (UHQ-584 > UHQ-291 > basic-229 > UHQ-152). That ranking is void until it is re-heard. The UHQ entries in it ran at 2^29 too.

**3. Plugin change (D34, `config/ssc-pipewire-1.6.8.patch`).** `SSCENC_INPUT_SHIFT` now counts 6 dB steps above the mode's nominal level. When the encoder rate is not 96000, the plugin subtracts 8, rounds S24 to 16-bit (`(v + 128) >> 8`, clamped to int16), and feeds that. Scripts are unchanged (`ssc-switch high` still writes 0). The patch applies cleanly to a fresh 1.6.8 tree and `encoder.c` compiles with `-Wall -Wextra` (the only warning, unused `encode_fake_s16`, was already there). **The installed plugin is not rebuilt**, so live basic mode still runs at 2^23 (this fix) and its compiled UHQ default is still 6 (D33).

**Not covered.** No listening at basic mode with the fix. Only 229k measured (192k/256k/328k use the same mode). 24→16 is rounded, not dithered; its noise (−101 dBFS) is far below the codec's ~29 dB SNR. The plugin's `sscenc_encode_s16` blob branch shifts 16-bit input `<<14` into the 24-bit sign-extension and would wrap; it is unreachable in the blob build (S24_32 is negotiated), left as is.

## F41 Shift-0 UHQ listening across source rates; plugin rebuilt with D34

**1. Listening (user, 2026-10-26, buds, uhq-584, shift 0).** Tracks at 44.1, 48, 88.2 and 96 kHz all "sounded good". In a blind test the user could not tell the source rates apart, which is what they expected. Together with F39.1 this closes the UHQ artifact: at 2^23 every source rate sounds clean. Not covered: uhq-291/152, and a Wi-Fi-off run (T0.2).

**2. Plugin rebuilt and installed (D34 live).** Fresh PipeWire 1.6.8 clone at `~/src/openssc/pipewire-1.6.8` (excluded locally in openssc's `.git/info/exclude`), current `config/ssc-pipewire-1.6.8.patch` applied cleanly, `ninja … libspa-codec-bluez5-ssc.so` built with only the old `encode_fake_s16` warning. Installed to `~/.local/lib/spa-0.2-patched/bluez5/`. The previous file is kept as `libspa-codec-bluez5-ssc.so.pre-d34` (sha256 752c3c88…; new c2908cf8…). WirePlumber maps the new file. The compiled `SSCENC_INPUT_SHIFT` default is now 0, and basic mode feeds 16-bit. Only the SSC codec plugin was rebuilt. The clone has the SSC patch but not the three HFP patches from `~/src/pipewire-hfp-atbcc-fix/patches`. The loaded `libspa-bluez5.so` is still the 2026-10-20 build, which has the HFP fix and the SSC core hooks; `…hfp-only.bak` has no SSC strings. `ssc-switch high` then gave basic-229, S24 48 kHz sink, shift 0, graph `[ 48000 96000 ]`. Basic mode has not been listened to yet.

**3. Basic-mode listening with D34 (user, 2026-10-26, buds, basic-229 via `ssc-switch high`, shift 0 = 16-bit input).** Tracks at 44.1, 48, 88.2 and 96 kHz: "no audible artifacts, clean sound". This is the first basic-mode listen at the encoder's nominal level. Every earlier one ran 8–14 bits hot (F40.2). With F41.1, both live modes now sound clean from every source rate. Not covered: a profile re-ranking at the right levels, basic bitrates other than 229k, and Wi-Fi off.

**4. All profiles, and Wi-Fi off (user, 2026-10-26, buds, shift 0).** Every profile from uhq-584 down to basic-229 "sounds good", including uhq-291 and uhq-152. The user gave no ranking between them. Disabling Wi-Fi "didn't change anything at all", so AX201 Wi-Fi/BT coexistence plays no audible part (T0.2). The README's old quality ranking is replaced by this result.

**Pitfall.** In zsh, the README's `meson setup build -Dsession-managers=[] …` fails with "no matches found". Quote it: `'-Dsession-managers=[]'`.

## F42 qemu encode time is far inside the deadline (T0.1); O0 closed

`tools/helper_timing.py` drives the live helper path (`~/.local/libexec/sscenc/sscenc-blob-run` → systemd-run → qemu-aarch64 → `ssc_blob_helper` + S24U blob). It speaks the plugin's protocol: read the ready word (int32 0), then per frame write u32 864 + int32 PCM and read int32 length + payload. It times each round trip, so pipe and systemd-run overhead are included. i7-10510U, 2026-10-26, while the buds were connected on the live helper (uhq-152).

| Run | Frames | p50 | p90 | p99 | p99.9 | max | Budget | Over budget | Mean load |
|---|---|---|---|---|---|---|---|---|---|
| uhq-584, 96k, 2^23, Nirvana 300 s | 33 331 | 0.83 | 1.06 | 1.75 | 6.10 | 11.35 ms | 9.00 ms | 6 | 10 % |
| uhq-584, 96k, 2^23, Get Lucky 240 s | 26 664 | 0.83 | 1.07 | 1.57 | 2.28 | 6.50 ms | 9.00 ms | 0 | 10 % |
| basic-229, 48k, 2^15, Nirvana 300 s | 16 664 | 0.81 | 1.01 | 1.39 | 1.87 | 4.89 ms | 18.00 ms | 0 | 5 % |

Startup (spawn to ready word) took 2–38 ms, and the first frame 26–33 ms (qemu translating cold code). That is a one-off, absorbed at stream start. The six overruns in the first run were isolated (no repeat on the second UHQ track). The sink cycle is 2048 samples (21.3 ms at 96k, F39.2), which carries ~2.4 frames, so one 11 ms frame does not miss a cycle. Encoding under qemu is ~10 % of one core at uhq-584, against AC5's 5 % target for the native backend.

**Conclusion (AC1).** Timing and transport do not contribute to the artifact. Encode time has ~10× headroom (F42), the graph shows no xruns (F39.2), Wi-Fi coexistence changes nothing (F41.4), and every profile sounds clean once the input level is right (F41). The artifact was the input level alone (F38.7, F40). T0.4 (decoding over-the-air frames) is not needed.

**Pitfall.** Forgetting the ready word shifts every reply by one frame and makes the encode look nearly free (p50 0.76 ms with a "0.0 ms" first frame and a 0-length reply in the set).

## F43 Headroom is a fixed EQ (bit-exact); the level riding is the DRC limiter (T4.7)

Harness tests `headroom` and `drc` in `~/src/ssc-codec/harness/difftest.c` (uncommitted). The full suite passes, 39 tests.

**1. Correction: "headroom" never rode the gain.** F38.7, F40, D33 and D34 put the gain riding on "the headroom stage". It is the **DRC/limiter**. The `<<4`/`<<12` input scaling is `ssc_preemphasis`'s (F29), not headroom's. Order in `ssc_encode`: biquad + `powercalc` ×2 + `CalculateLimitGain` → `set_DRC_limit_gain` → `SSC_DRC_Exe` ×8 on raw input (0xbcd8–0xbe24); then MDCT; then `uhq_headroom`/`normal_headroom` on the MDCT bins (0xc9fc/0xca1c, 0xcdec/0xce18); then `ssc_compute_band_energies`.

**2. Headroom = fixed per-bin gain on the spectrum, bit-exact.** Stateless, signal-independent, applied every frame at any level. Arguments: `(X, bitrate, M=8, eBands)` plus a channel stride (864) for `normal_headroom`. Each bin gets `MULT16_32_Q15(g, x)` with 32-bit wrap.
- `uhq_headroom` (E-mode). From `s = eBands[18]·8`: bins `[s, s+25)` 0x78c8 (−0.50 dB). If bitrate ≥ 270000: `[s+25, 354)` 0x7864, `[354, 400)` 0x7800, `[400, 864)` 0x779c (−0.59 dB). Otherwise: `[s+25, 350)` 0x781e, `[350, 864)` 0x779c.
- `normal_headroom` (F-mode). If bitrate ≥ 96000: bins from `eBands[20]·8 + 68` to `eBands[22]·8` take the `.data` table `headroom_gain` (240 int32, low 16 bits, index `bin − eBands[20]·8`). Nothing writes the table. Otherwise two tapers: from `s1 = eBands[17]·8` (320), `0x7fff − 3·(i−s1)`; from `s2 = eBands[20]·8 − 15` (561), `0x7767 − 3·(s2−s1) − 5·(i−s2)`. The second taper restarts 0x898 below where the first ends.

  The test transcribes both and compares them against the blob on 4000 frames (all 14 bitrates, realistic and full-range int32 spectra), with no mismatch. The blob's actual per-bin gains corrected the second taper; the decompile alone gave it wrong. The native encoder needs `headroom_gain` extracted (OD7).

**3. The level-dependent stage is `SSC_DRC_Exe`, a block peak limiter with global state.** It runs 8 calls per frame, each on 108 stereo samples of raw input. A block engages when more than 24 of its 216 values exceed the threshold `drc_struct`. The target gain is then `threshold / peak` (Q22), otherwise 1.0. Attack is instant (coefficient 0); release is 0x3ca4a1/2^22 = 0.9475 per block. The gain ramps linearly within a block. `SSC_DRC_Init` and `SSC_DRC_SetPar` set the globals at 0x120504–0x120514.
- **Threshold, per frame:** `CalculateLimitGain(narrow-band power, total or residual power, bitrate, Fs)` returns Q15. At 96k it gives 0x75d4–0x7b4c (0.921–0.963). At 48k (Fs 44100) it gives 0x741d–0x7a94, or 0x8000, which never fires. E-mode always shifts it `<<8` (24-bit scale). F-mode shifts it only when `bits == 24`, so with `bits=34` the basic-mode threshold sits on the **16-bit scale**. That is why basic mode wants 2^15 (F40) and UHQ wants 2^23 (F38.7): above that, nearly every block exceeds the threshold and the limiter rides.
- **Never back to unity.** The release maps the shortfall `d` below 2^22 to `ceil(0.94755·d)`, which stalls at d = 19. After the first engaged block the gain stays at (2^22 − 19)/2^22 (−0.00004 dB) for good. Every sample then becomes `(x·(2^22−19)) >> 22`, a change of up to ±19 LSB at 24-bit. Inaudible, but required for bit-exactness. Verified on the blob: 200 quiet blocks with up to 24 peaks before any engagement come out unchanged; after one loud block, 1000 of 1000 quiet blocks match that formula exactly.

**4. How often it fires at the right input level** (whole tracks, raw blocks, loosest/strictest threshold; `tools/drc_count.py FILE 96000 24` or `FILE 48000 16`):

| Track (peak) | UHQ 96k at 2^23 | Basic 48k at 2^15 |
|---|---|---|
| Get Lucky 88.2k master (0.00 dBFS) | 0.34–1.16 % of blocks, from 0.1 s | 0.45–1.49 %, from 0.1 s |
| Nirvana SIDE-1 (−1.67 dBFS) | 0 | 0 |
| Arctic Monkeys "Do I Wanna Know" (−0.03 dBFS) | 0.002–0.19 %, from 6.7 s | 0.014–0.17 % |

So the limiter still fires on hot masters at the correct level. It reduces the gain by at most ~0.85 dB (threshold ≥ 0.907), which fits the ≤ 0.23 dB frame-gain steps measured at 2^23 (F38.7). It is inaudible (F41). On quiet masters it never fires, and the stage is an exact pass-through.

**Answer to "is the headroom stage needed for byte-exactness?"** Yes, and it is now done. It is fixed EQ, not level-dependent. The DRC limiter is also needed: hot masters engage it at any input level, and after that its −19/2^22 tail changes every sample. Its 1.7 KB (`SSC_DRC_*`, `CalculateLimitGain`, `powercalc`, `Biquad_Narrow_int2int_stereo`) is the next T4.7 step. `drc` checks behaviour only, not a full transcription.

**Progress.** Headroom adds 2.67 KB (`uhq_headroom` 1428 B + `normal_headroom` 1240 B). Verified bytes: 40.7 KB of 62.4 KB (**~65%**). By objective: O0 100 %, O1 ~90 %, O2 ~45 %, O3 ~80 %, O4 ~63 %, O5/O6 0 %.

## F44 The DRC limiter chain is bit-exact (T4.7 done)

Five harness tests in `~/src/ssc-codec/harness/difftest.c` replace F43's behaviour-only `drc` (uncommitted): `drc_powercalc`, `drc_biquad`, `drc_limitgain`, `drc_exe` and `drc_chain`. The full suite passes, 43 tests in ~20 s. Each transcription was taken from the disassembly, not the decompile, and each test fails when its reference is mutated (no error feedback, sign-extended carry, count ≥ 24, no lookahead min, unshifted F-mode threshold, the 270000 edge, and so on).

**1. `Biquad_Narrow_int2int_stereo` (0x6d8c)** is a direct-form-I biquad per channel on `x << 7` (32-bit wrap), with 64-bit accumulation. The output word is `y = acc >> 32`, and the low word carries into the next sample as error feedback. Feedback taps hold `y << 3`, and the output is `(y + 8) >> 4`. Inside a call the carry is zero-extended. Between calls it is stored sign-extended, and the test catches that difference. State is int64[10] at handle +0x90: R x1 x2 y1 y2 acc, then L x1 x2 y1 y2 acc. The loop runs two stereo frames per iteration. Coefficients (5 int32, OD7): `coeffs96andbelow` at 0x3d00 is used in E-mode always and in F-mode when the bitrate is < 96001. `coeffs` at 0x3cec is used in F-mode at ≥ 96001. Both are low-passes. The output goes to the global `_pcmpower`.

**2. `powercalc` (0x6ea0)** samples **one stereo frame in 32**: frames 0, 32, …, 832, so 27 of 864. Each sample adds `((x² >> 16) & ~1) << 4`, and the result is `(u64 sum) >> 9`. n must be a multiple of 32; otherwise the blob never leaves the loop. So the threshold choice rests on 27 sample pairs per frame, not on the frame's power.

**3. `CalculateLimitGain` (0x6f0c)** matches the decompile. The 96k E-mode ladder compares the narrow-band power with total × {0.94, 0.92, 0.90, 0.80}. The 270000 edge is `≥`. The 48k ladder uses {0.90, 0.80, ¼}. At bitrate ≤ 96000 or ≤ 128000 there is a two-step ladder (0x76c0/0x7670, 0x78b4/0x75bc). 0x8000 means "never" and is returned when the narrow band is below a quarter of the total. The test saw all 16 distinct values.

**4. `SSC_DRC_Exe` (0x70c4).** Globals run from `drc_struct` (0x120500): threshold, attack 0, release 0x3ca4a1, smoothed target, ramp start, last target; `DRC_ChNum` sits at 0x120518. Steps:
- The peak (abs with INT_MIN wrap) and the count over the threshold run over `ChNum·n` values.
- The block engages at an unsigned count ≥ 25. Its target is `(int32)((thr << 31) / (u32)peak) >> 9`, or 0x2000 at peak 0.
- `g = (coef·smooth + (2^22 − coef)·target) >>> 22`. The coefficient is the release one when target > smooth, otherwise attack.
- **One-block lookahead:** the ramp runs from the current gain to `min(last block's g, this g)`, in `(end − cur) / n` steps (32-bit sdiv). Each sample becomes `(gain·x) >>> 22`.
- The ramp is skipped only when the current gain is exactly 2^22 and the step is 0.

`SSC_DRC_Init(Fs, ch)` sets the gain to 2^22, the last target to 2^29 and ChNum. `SSC_DRC_SetPar(mode, thr)` sets the threshold, the coefficients and smooth = 2^22. `set_DRC_limit_gain` only writes the threshold.

**5. Whole stage, frame by frame**, in `ssc_encode`'s order against the blob's pieces, comparing the PCM, the threshold, the biquad state and every global after each frame. It runs on synthetic signals and on 108 s each of Get Lucky (RAM album), Arctic Monkeys "Do I Wanna Know" and Nirvana SIDE-1 from 120 s. Every track goes through 10 configurations (the four E-mode rates, basic 229/96/328/128 at 48k/44.1k with bits=34, and bits=24 at 229/88), each at the plugin's level and 64× hotter. That is 12000 frames per run with no mismatch. Thresholds covered both scales (0x741d … 0x800000).

At the plugin's level the gain is moving in 4487 of 12000 Get Lucky frames at UHQ (37 %) and in 4925 at basic-229. For Arctic Monkeys it is 0 at UHQ and 73 at basic-229 over these 108 s; for Nirvana, 0. So "fires on 0.3–1.2 % of blocks" (F43.4) turns into a gain that is still releasing for a third of the frames of a hot master. Each engagement takes ~180 blocks (~23 frames) to release, and the gain stays within ~0.85 dB of unity. It is inaudible (F41) but has to be bit-exact.

**Progress.** The chain adds 1.67 KB (Biquad 276 B, powercalc 108, CalculateLimitGain 424, SSC_DRC_Exe 696, Init 92, SetPar 56, set_DRC_limit_gain 16). Verified bytes: 42.3 KB of 62.4 KB (**~68 %**). T4.7 is done. By objective: O0 100 %, O1 ~90 %, O2 ~45 %, O3 ~80 %, O4 ~66 %, O5/O6 0 %. The phase is about **57 %** done.

## F45 pitch_search, tone_detect and patch_transient_decision are bit-exact (T4.6, exported part)

Patch 0011 (`~/src/ssc-codec/harness/patches/0011-pitch-tone-detect-transient-bands.patch`, uncommitted) and three harness tests: `tone_detect`, `pitch_search` and `patch_transient`. The full suite passes, 46 tests in ~25 s (2026-10-26). The transcriptions come from the disassembly; `tone_detect` was also checked instruction by instruction, and this time the decompile was right.

**1. `ssc_pitch_search` (0x12234, 1160 B)** is v1.1 `pitch_search` with two changes:
- **No `x_lp4`/`y_lp4` normalisation.** The function has no maxabs or `clz`, so `shift` is always 0. The fine search is unshifted, and the second `find_best_pitch` gets yshift 1. Stock shifts whenever the pitch buffer goes above 2^11 after the LPC.
- **`tone_detect(xcorr, best_pitch, maxcorr, max_pitch>>2)`** runs right after the coarse `find_best_pitch`, and its return value is ignored.

Its buffers are `.bss` globals: `x_lp4` at 0x247a0, `y_lp4` at 0x24950 and `xcorr` at 0x24d18. These are file addresses; Ghidra adds 0x100000. `x_lp4` and `y_lp4` share scratch with `remove_doubling`'s `yy_lookup` at 0x247a0. The `ssc_encode` call is `pitch_search(pitch_buf+512, pitch_buf, 864, 979, &pitch)`, followed by `pitch_index = 1024 − pitch`, as in stock. `find_best_pitch` is **not** inlined (the plan was wrong about that): it is a static function at 0x126bc (724 B) and matches stock. Its NEON `Syy` sum rounds `len` up to a multiple of 8, which changes nothing for the real lengths, 216 and 432.

**2. State across calls.** `tone_detect` reads `xcorr[hi+1]`. When the coarse best is lag 243, that read is `xcorr[244]`. The coarse search writes only [0, 244), so the value there is left over from the previous call's fine search, which writes [0, 489). The reference therefore keeps `xcorr` in a global, `ssc_pitch_xcorr`, which moves into the handle with OD8. After every call the test compares the blob's whole 489-entry global (`dladdr` base + 0x24d18) with the reference's. **Correction (F46.3):** inside `ssc_encode` this global is part of `in[]`, so the carried entry is `in[1071]` (an in_mem sample or `alg_quant` scratch), not the previous fine search's value.

**3. `tone_detect` (0x12088, 428 B)** is Samsung-only:
- The threshold is `(int64)maxcorr·30512 >> 15` (0.931·maxcorr). It returns 0 unless both candidates reach it.
- Take `lo`/`hi` of the two candidates. The spacing is `d = hi − lo`, minus 1 if `xcorr[hi+1] ≤ xcorr[hi−1]`. If `d < 5` it returns 0.
- The first lag `lo+i` (i = 2 … d−1) above the threshold resets `d = i`. It subtracts 1 when that lag beats its right neighbour and the left neighbour beats the right one too. Then `d < 5` or `d > n/2` returns 0.
- It walks forward from `hi+d` to n−1 in steps of d, or d+1 when the right neighbour is ≥. Then it walks back from `hi−d` to lag 0 the same way. Every step needs `max(xcorr[j], xcorr[j±1]) ≥` the threshold.
- On success, `best_pitch` becomes {the last forward position, that − d}, or that − d − 1 when the left neighbour is ≥. For a harmonic tone it therefore picks the longest-lag pair.

**4. `ssc_patch_transient_decision` (0xa170, 508 B)** is v1.1's function restricted to the upper bands:
- The spreading starts at band 13, over both channels of `oldE`, and the backward pass also stops at 13.
- The mean covers bands 13 … end−2 of **channel 0 of `newE` only** and is divided by end−14. It returns `mean > 1.0` (1024), or 0 when end ≤ 14.
- Stereo is hard-coded, so there is no C parameter. The decompile hid the return value; it shows up as an argument to `__stack_chk_fail`.

In `ssc_encode` the call is `(bandLogE, oldBandE, nbEBands, *(int*)(st+0x14) − previous_uhq_index)`, behind the stock gate (`ec_tell+3 ≤ total_bits`, not already transient). The `end − previous_uhq_index` argument is for T4.8.

**5. Validation.**
- **Synthetic cases:** random, tie-heavy (quantised to M/16, near-silence with maxcorr=1 and threshold 0) and crafted edges (peaks at the threshold; lo=0 with hi=n/2). `tone_detect` fires in 210 of 3000 cases and `patch_transient` in 319 of 3000.
- **Music:** 108 s each of Get Lucky (RAM), Arctic Monkeys "Do I Wanna Know" and Nirvana SIDE-1 from 120 s at 96 kHz, plus Get Lucky at 48 kHz. Each frame goes through `pitch_downsample` and then `pitch_search` (`DT_PITCH_MUSIC`): 41992 frames, no mismatch, full `xcorr` state included.
- **Stock v1.1 for comparison:** it differs on 450 of 2000 synthetic cases, but on only 1–4 frames per 108 s of music. The changes rarely matter on music, but bit-exactness needs them.
- **Mutation:** 26 mutants, 24 caught. Two survive:
  - `v >= next` in the first-peak adjustment is equivalent: it is reachable only at i=2, where d ≤ 2 returns 0 either way.
  - Zeroing the stale `xcorr[244]` matters only when the coarse best is lag 243 and hi−lo sits at the d=5 boundary. It is covered by composition: the state comparison plus the direct test, which reads `xc[n]`.

**Progress.** This batch adds 2.82 KB (`pitch_search` 1160 B, `find_best_pitch` 724, `tone_detect` 428, `patch_transient_decision` 508). Verified bytes: 45.1 KB of 62.4 KB (**~72 %**). Left: `ssc_encode` 16.6 KB and `encoder_init`/create 0.6 KB. T4.6 is done except transient_analysis, tf_analysis and dynalloc, which are inlined into `ssc_encode` and land with T4.8. By objective: O0 100 %, O1 ~90 %, O2 ~45 %, O3 ~80 %, O4 ~70 %, O5/O6 0 %. The phase is about **59 %** done.

## F46 The native `ssc_encode` is bit-exact (T4.8)

New in `~/src/ssc-codec/harness` (uncommitted): `ssc_encoder.c`/`.h` (the native encoder; the headroom and DRC transcriptions moved here from `difftest.c`), `ssc_abi.h` (the blob's layouts, including the encoder handle), `trace.c`/`.h`, `trace_calls.h`, `trace.list`, patches **0012** and **0013**, and the `encode` test. The full suite has 47 tests and passes in ~45 s (2026-10-26).

**1. Probe: every call `ssc_encode` makes is observable.** `ssc_encode` reaches all exported functions through the PLT, including the range coder (`ssc_ec_enc_bit_logp`, `_icdf`, `_uint`, `_bits`, `_done`). `difftest` exports wrappers under those names (`-Wl,--dynamic-list=trace.list`), and the blob, dlopened with `RTLD_NOW`, binds to them. Each wrapper records its arguments and results, then calls the blob's own definition (`dlsym` on the blob handle). Only depth-0 calls are recorded, the ones `ssc_encode` makes itself. The reference records the same calls through the same helpers, so a frame compares as two text traces (~86 records at 584k). The first differing record names the section. This also does T2.3's job (golden vectors at every function boundary) without gdb. Patch 0009's `ssc_mdct_forward` shares a name with a wrapper, so the reference build renames it with `-D`.

**2. What `ssc_encode` is:** v1.1's `celt_encode_with_ec` with C=2, LM=3, CBR (`st->bitrate` stays −1, no VBR) and start=0, plus the following. `ssc_encoder.c` includes the patched `celt_encoder.c` and reuses its static `tf_analysis`, `stereo_analysis` and `alloc_trim_analysis` unchanged.
- **Two flags from the bitrate, not one.** *E header* (byte 1 = `e0 | counter>>4`): 152, 250, 291, 308, 442, 584 and 886k. *uhq*: those plus **256k and 328k**, which carry F tags (`f6`, `f5`). uhq selects the E-mode DRC coefficients and threshold scale, input `<<4` (24-bit), `uhq_headroom`, no tf analysis or TF symbols, and no 3/128 extra bytes. So 256k/328k frames are F-tagged but take the 24-bit path; F40's 16-bit nominal level holds only for 88/96/128/192/229k. Unknown bitrates get tag `f3` and neither flag. An early `set_DRC_limit_gain` (0x7b4c00, 0x79f800, 0x76f2/0x78b4/0x7a94, the last three `<<8` when bits=24) is always overwritten before `SSC_DRC_Exe`.
- **Input.** The DRC limits `pcm` in place (F44), then sample_max (v1.1, abs wrapping at INT_MIN) and the silence bit. The pre-emphasis is a shift only: `<<4` on the uhq path or with bits=24, otherwise `<<12`.
- **Prefilter:** v1.1's `run_prefilter`, gated by nbAvailableBytes ≥ 25 and not silence, with tapset 0 and no tapset symbol (patch 0008). Threshold, qg and the gain hold are stock.
- **No time-domain `transient_analysis`.** `isTransient` comes only from `patch_transient_decision` (gate `tell+3 ≤ total_bits`, end − previous_uhq_index). `tf_estimate` is 0, or 0.2 after a patched transient, and `tf_chan` is 0. A transient reruns 16 short MDCTs, headroom and the energies, and adds 1.5 to bandLogE2.
- **Top-band cut:** at bitrate < 192000, the long-MDCT bins from `8·eBands[21]` up are zeroed in both channels. After a transient re-MDCT the cut is applied only on the non-uhq path.
- **Adaptive end** (F34.3): the last-band scan picks idx (152k: always 4). `end − idx` goes to normalise, coarse energy, spread, dynalloc, stereo/intensity/trim, allocation (signalBandwidth `end − idx − 1`), fine energy, PVQ and finalise. tf_analysis, tf_encode, coarse energy's first argument and the band energies use all 22 bands.
- **TF** (non-uhq only, gate 15·C ≤ bytes): stock tf_analysis with lambda 24/12/8/6, then **`tf_res[0]` and `tf_res[1]` are forced to `isTransient`**, then stock tf_encode. On the uhq path `tf_res` is all 0 and nothing is coded (F37).
- **Dynalloc:** stock analysis with **lsb_depth 16** (noise floor constant −6.5 dB, `0xe600`). The 1/4-budget cap is **`effectiveBytes·16 − tot_boost`** (disassembly 0xe558–0xe570; v1.1 rounds to `(effectiveBytes/4)<<6`). If the cap isn't hit, a **band 0/1 boost** follows, up to 10 quanta, on bandLogE2:
  - band 0: `lvl = min(E[0], E[22]) − 6500`, `drop = min(E[0]−E[1], E[22]−E[23])`. If both > 0: `((max(drop,1500) − 1500) >> 7) + (lvl>>3 & 0x1fff)/25`.
  - band 1: `lvl = min(E[1], E[23]) − 6100`, `drop = min(E[1]−E[2], E[23]−E[24])`, `rise = min(E[1]−E[0], E[23]−E[22])`. If all > 0: `(drop − 1700)/160 + (lvl>>3 & 0x1fff)/25`.
  - Each result raises that band's offset. The dynalloc symbols themselves are stock, over `end − idx` bands.
- **Band 18–21 carry-over, after the dynalloc symbols and unsignalled:** if offsets[18] > 1000 and offsets[19] = 0, and the quantised oldBandE[19] > 7000 or oldBandE[18]/2 < oldBandE[19], then offsets[19] = offsets[18]. Otherwise the same for 19→20, otherwise for 20→21. The allocation uses the result. **A decoder must redo this**, since it has both inputs; patch 0010 does not yet.
- **Intensity is constant.** `hysteresis_decision(510, tables@0x3d18/0x3d46, 22−idx, prev)` has 23-entry Samsung tables, all below 510 − hysteresis. It returns N (or prev when prev > N), so intensity is always `end − idx`, and the native encoder needs neither table (OD7).
- **Trim:** stock, tilt over `end − idx`; `trim_icdf` at 0x3d74 is stock's. **Allocation:** table selector bitrate > 192000 (F34.4), stock anti-collapse reserve.
- **Epilogue:** stock, except: bands dropped by idx get **oldBandE = −28 dB** (v1.1's decoder sets bands above end to 0); **oldLogE2 is never updated**; `previous_uhq_index = idx`; the frame counter wraps at 0xff. It returns `nbCompressedBytes + 4`, or −3 on a range-coder error.
- **Init** (`ssc_encoder_init`): stock reset values, but oldLogE/oldLogE2 stay 0 (not −28) and `end` = effEBands (21) until the first frame sets 22. Then `SSC_DRC_Init`/`SetPar(0x7b8e)` and `previous_uhq_index = 0`.

**3. Hidden state: `ssc_encode`'s `in[]` is the global `in_shared` (0x2402c), and other scratch lives inside it.** pitch_search's `xcorr` (0x24d18) is `in[827..1316)`, and `alg_quant`'s `iy`, `y` and `signx` (0x24dac, 0x24fec, 0x2522c) are `in[864..)`, `in[1008..)` and `in[1152..)`. The comb filter rewrites all of that after the pitch search, so the one value that crosses frames is **`in[1071]`**, which `tone_detect` reads as its stale `xcorr[244]`. It holds the in_mem sample the previous frame's prefilter copied there, or `y[126..127]` from the last `alg_quant` call with N ≥ 128 after it. **This corrects F45.2:** in `ssc_encode` the carried entry is not the previous fine search's value (that holds only when `pitch_search` is called on its own, as in its test). The reference keeps `in[]` in the encoder (OD8). Before `pitch_search` it seeds `ssc_pitch_xcorr` from `in[827..]`, and patch 0012 lets `alg_quant` use the blob's offsets in that buffer (`ssc_vq_scratch`). `tf_analysis` also uses `in[0..282)` as scratch, but nothing reads that back.

**4. Validation** (`encode` test, fresh blob and reference handles per configuration; 21 configurations: 96k at 584/442/886/308/291/250/152k, and 48k and 44.1k creates at 229/328/256/192/128/96/88k. The first music runs below used the first 15, without the 44.1k creates other than 229k). Each frame compares the return value, the bytes, the whole handle (0x08–0x2548), the DRC globals, `previous_uhq_index` and `in_shared[282..)`. Synthetic frames also compare the trace.
- Synthetic, the T2.2 corpus as generated 20-frame segments: white noise, three tones, silence and gated noise, loud/quiet bursts, dynalloc peaks (a band-1 tone plus a tone stepping through bands 18–20), 100 Hz + 1 kHz, 15 kHz + 30 kHz (20 kHz at 48k), a 20 Hz–Nyquist log sweep, pink noise with castanet-like clicks, and a full-scale sine clipped 12 dB over. 200 frames × 21 configurations, all match.
- Music at the plugin's level (2^23 on the 24-bit path, 2^15 on the 16-bit one): 30 s each of Nirvana SIDE-1 (from 120 s), Get Lucky (RAM) and Arctic Monkeys "Do I Wanna Know", resampled to 96k and 48k. That is 3333 or 1666 frames per configuration, ~110 000 frames in all, with no difference. Each track takes ~90 s under qemu without tracing.
- 30 s of Get Lucky on all 21 configurations, 6 bits hot (the old 2^29 plugin level, where the limiter rides hard, F38.7), 8 bits hot (int32 full scale) and with bits=24: all match, decoder included. No band energy reached 2^29 in any of them, so OD20's `amp2Log2` overflow is not reached through `ssc_encode`.
- Branches reached (synthetic + Get Lucky): prefilter on, patched transients, silence, the cut, idx > 0, the dynalloc cap, both boosts, the carry-over (3–7 frames per configuration, synthetic only; never on the music), anti-collapse and tf_select = 1.
- Decoding: since 0013, the test also decodes every blob frame with the patched decoder. No frame has a range error, and the decoder's `oldBandE` equals the encoder's after every frame, carry-over frames included. Removing either half of 0013 fails within ~80 frames (−28 reset) or ~165 frames (carry-over).
- Mutations (22, one at a time, synthetic): every one is caught except these.
  - Equivalent on all live inputs: the stock cap (confirmed in the disassembly instead), exact-tie boundaries (`> 7000`, `> 12001`, `> 30001`), and the nbAvailableBytes 25 gate (no live frame is that small).
  - Visible only in the trace: normalise's end, the hysteresis return value, and the transient-path cut on uhq.
  - Not reached in 200 frames: the frame-counter wrap (music reaches it), and `in[1071]` seeding (see §5).

**5. Decoder consequences (patch 0013, `ssc_header.h`).**
- The band 18–21 carry-over and the −28 dB restart of dropped bands are not in the bitstream. A v1.1-based decoder gets the allocation wrong on carry-over frames, with no range error, and predicts a returning band's coarse energy from 0. 0013 mirrors both (`ssc_mode != 0`) and adds a ctl that exposes the decoder's `oldBandE`.
- 256k and 328k frames have F tags but no TF symbols and no 3/128 pad. The header parser now sets `uhq` for them and the decoder uses E-mode TF handling, so the old `e_mode` switch would have desynced both rates. F38/F40 measured only 584/152/229k, so no earlier result changes.
- The buds' decoder presumably does both too, or Samsung's own frames would not decode.

**6. The blob breaks the AArch64 calling convention.** `ssc_amp2Log2` (and its `_asm` kernel), `ssc_alg_quant` and `ssc_comb_filter_function3/4_asm` write v8/v9 (for example `dup v8.2s` at 0x158dc, `sshr v8.4s` at 0x16e34) without saving them. AAPCS64 makes d8–d15 callee-saved, so a caller that keeps a floating-point value in one of them across `ssc_encode` gets it corrupted. That bit the harness: GCC inlined the synthetic generator into the frame loop and kept `amp * .3` in d8, so from about frame 22 the tone and sweep segments came out as zeros. Both encoders got the same zeroed PCM, so no comparison was wrong; the generator is now `noinline`. It is harmless in the plugin, where the blob runs alone in `ssc_blob_helper` under qemu, and on the phone as long as libbluetooth_jni holds no floats across the call. A native encoder doesn't have the problem. Earlier `decode`-test tone runs may also have encoded partly silent tones; their checks compare what was actually fed, so they stay valid but cover fewer frames.

- Five minutes each of the three tracks on all 21 configurations (Arctic Monkeys: its full 272 s): 1 357 034 frames, all bit-exact, decoder included. The frame counter wraps several times per run.

**7. Not covered yet.** A direct test of the stale-`in[1071]` read inside `ssc_encode`. It needs a coarse pitch at lag 243 at the `tone_detect` boundary, which no input here produced. The state itself is compared every frame (`in_shared[282..)`).

**Progress.** `ssc_encode` (16.6 KB) and init/create (0.6 KB) are verified, so all 62.4 KB of live encoder code is bit-exact against the blob on the inputs above (**100 %** of bytes; T4.8's long-run and corpus checks remain). By objective: O0 100 %, O1 ~90 %, O2 ~70 % (T2.3 done by the trace, T2.2 corpus not built), O3 ~80 %, O4 ~95 %, O5/O6 0 %. The phase is about **68 %** done.

## F47 The native encoder runs outside qemu, bit-exact, at ~2 % of a core (O5 start)

New, uncommitted:
- in `~/src/ssc-codec`: `native/` (Makefile, `ssc_native.c`/`.h`, `ssc_native_helper.c`, `ssc_native_bench.c`), `tools/ssc-extract-tables.py`, `tools/helper_compare.py`, and an `SSC_NO_TRACE` switch in `harness/trace.h`;
- in ssc-uhq: `config/bin/sscenc-blob-run` (a tracked copy of the live launcher, plus a backend switch) and the plugin patch's 256k/328k level fix (§4).

**1. Build.** `make -C native` builds libopus v1.1 + patches 0001–0013 + `harness/ssc_encoder.c` for x86_64. `tools/ssc-extract-tables.py` writes `native/build/ssc_tables.h` from the user's blob: both static modes (resolved through the blob's `R_AARCH64_RELATIVE` relocations at 0x1b9c0/0x1ba40), `headroom_gain`, and the biquad coefficients. It checks the blob's sha256 and the mode fields, and the output is gitignored (D28, OD7). `ssc_native.c` builds the CELTModes the way the harness's `build_mode` does.

**2. Bit-exact on x86_64.** `tools/helper_compare.py` feeds the same PCM, at the plugin's level, to openssc's `ssc_blob_helper` under qemu and to `ssc_native_helper`. On 30 s of Nirvana, Get Lucky and Arctic Monkeys, on all 21 configurations, every frame is identical (63 runs, ~140 000 frames). So the aarch64 harness result (F46) carries over to the host compiler: the code has no architecture-dependent behaviour that the corpus reaches.

**3. CPU (AC5).** `ssc_native_bench` times encoding in-process: 5 minutes of music, i7-10510U, `-O2`, no `-march`, and a busy machine (load average ~10).

| Configuration | Track | p50 / p99 per frame | Share of one core at real time |
|---|---|---|---|
| uhq-584 | Get Lucky | 0.23 / 0.39 ms | 2.7 % |
| uhq-886 | Get Lucky | 0.19 / 0.39 ms | 2.3 % |
| uhq-152 | Get Lucky | 0.10 / 0.21 ms | 1.3 % |
| basic-229 | Get Lucky | 0.18 / 0.37 ms | 1.1 % |
| uhq-584 | Nirvana, 3 runs | 0.32–0.35 / 1.0–4.1 ms | 3.7–4.9 % |

Measured again with my background runs stopped (the live plugin helper and a desktop session still running): uhq-584 takes **1.9 %** on Nirvana and **1.8 %** on Get Lucky (p50 0.15 ms, p99 0.31 ms). The earlier spread came from load, part of it from the tee recursion below. The budget is 9 ms per frame at 96 kHz; the helper's pipe round trip adds ~0.05 ms; qemu ran the blob at ~10 % (F42). **AC5's CPU half (< 5 % of one core at uhq-584) is met** with margin. Its other half, sounding identical, is T5.3.

**4. 256k and 328k at 48 kHz are fed 8 bits too quietly by the plugin.**
- F46.2 found that these bitrates take the 24-bit path. The plugin patch (D34) drops every non-96k rate to 16-bit.
- `decode` test, Get Lucky resampled to 48 kHz, 20 s, gain relative to the source:

  | Bitrate | Fed 2^15 | Fed 2^23 |
  |---|---|---|
  | 229k | 0.988 (26.6 dB) | 1.56 (3.8 dB, the limiter rides, F40) |
  | 256k | **0.0039** (−48 dB) | 0.988 (27.0 dB) |
  | 328k | **0.0039** (−48 dB) | 0.988 (28.0 dB) |

  So at the plugin's current level, 256k and 328k would play 48 dB low on the buds. That assumes the buds decode those frames at the same scale as our decoder, which matched by ear at 229k.
- Only profiles chosen through `SSCENC_PROFILE` reach these rates: basic-256, basic-328, force-high, basic-max, top-basic, open-basic-256/328. `ssc-switch` offers none of them. F41's "every profile sounds good" covered the ssc-switch profiles.
- Fixed in `config/ssc-pipewire-1.6.8.patch`: the 8-bit drop now skips 256000 and 328000. It applies to a fresh 1.6.8 tree and `encoder.c` compiles with `-Wall -Wextra`. **The installed plugin is not rebuilt.**

**5. Backend switch, not installed.**
- `config/bin/sscenc-blob-run` keeps the live launcher's blob branch byte for byte (`SSCENC_BACKEND` unset or `blob`).
- `native` execs `~/.local/libexec/sscenc/x86_64/ssc_native_helper`, or `SSCENC_NATIVE_HELPER`.
- `tee` (T5.2) runs the native helper with the blob branch as `SSCENC_TEE_CMD`. It sends the blob's frames, so the live sound doesn't change, and logs every differing frame to stderr (the journal).
- Tested through the launcher, including systemd-run + qemu: 1000 frames at uhq-584 and 1000 at basic-229, 0 differ. A fake blob at another bitrate shows the mismatch report working.
- The plugin needs no rebuild for this: it already execs the launcher and passes its environment.
- **Installed and live, 2026-10-26 18:23.** The launcher went to `~/.local/libexec/sscenc/sscenc-blob-run` (the old one is kept as `sscenc-blob-run.pre-d35`) and the helper to `~/.local/libexec/sscenc/x86_64/ssc_native_helper`, the same sha256 as `native/build`. `Environment=SSCENC_BACKEND=tee` is in `~/.config/systemd/user/wireplumber.service.d/sscenc.conf` and in the repo copy; `ssc-switch uhq` applied it.
- `ps` shows WirePlumber → `ssc_native_helper 96000 2 34 584000` → systemd-run → qemu `ssc_blob_helper`, with the sink RUNNING at uhq-584. After 70 s of the user's playback (~7800 frames) the journal has no `tee:` line; any of the first ten mismatches would have been logged at once.
- The helper prints a progress line only every 100 000 frames (~15 min at 96 kHz) and a summary (`tee: done, N frames, M differ`) when the stream closes.

**6. Live results (2026-10-26).**
- Tee at uhq-584: the journal has `tee: done, 21235 frames, 0 differ` (18:26:55) and `tee: done, 2550 frames, 0 differ` (18:27:29), so 23 785 live frames matched. A third stream ended at a WirePlumber restart before writing its summary.
- The user then set `SSCENC_BACKEND=native` themselves (live `sscenc.conf`, tee line commented out) and reported **"sounds really good on native"**. That is the T5.3 listening result: uhq-584, the user's own playback, no blind A/B. Native output is byte-identical to the blob's on every frame checked, so it can't sound different; the listening confirms the path works end to end (plugin → native helper → buds).
- At 18:30 WirePlumber's environment had `SSCENC_BACKEND=native` and the sink was suspended. The live encoder is now native, without qemu.

**Pitfall.** The tee child must not inherit `SSCENC_TEE_CMD`. When it did, a test that used the native helper as the "blob" recursed until it was killed. The helper now unsets it before the exec.

## F48 Function classification, final (T1.3 done, AC2)

Supersedes F30's table: it covers **all 85 exported functions** of the S24U blob (`readelf --dyn-syms`, FUNC, defined) by full name, plus the 7 unexported statics on live paths. Class, patch and test come from F31, F33, F34, F43–F46; nothing here is new evidence except §1 and the three create/destroy/get_size rows.

**1. Reachability, mechanical** (2026-10-26, `objdump -d` + a call-graph walk from the four API entry points; the blob has no `blr`, and its 76 `br` are all PLT stubs). **69 exports are live, 16 are dead**, and the dead set is exactly F30's D row. The live statics are exactly F30's seven `FUN_*`. The other call targets are libc (`memcpy`, `memset`, `malloc`, `free`, `__stack_chk_fail`); `printf` is called only from dead code (`print_enc_fields`).

Legend as F30: **S** stock · **F** stock logic, specialised (C=2, LM=3, start=0, enc-only) · **M** modified · **N** Samsung-only · **D** dead · **API** entry point. Patches are `~/src/ssc-codec/harness/patches/`; 0001 (exact `MULT32_32_Q31`) and 0002 (24-bit range coder) are library-wide, so every caller of those macros carries them. "encode" means covered only through the whole-frame `encode` test (bytes, handle, globals, and the PLT trace for the traced calls, F46).

| Exported function | libopus v1.1 | Class | Patch | Test |
|---|---|---|---|---|
| ssc_encoder_get_size | opus_custom_encoder_get_size | API, F | | encode (handle size) |
| ssc_encoder_create | opus_custom_encoder_create | API, F | | encode |
| ssc_encoder_destroy | opus_custom_encoder_destroy | API, S | | (a tail call to `free`) |
| ssc_encode | celt_encode_with_ec | API, **M** | 0008, 0011, `ssc_encoder.c` | encode |
| ssc_encoder_init | opus_custom_encoder_init | **M** | `ssc_encoder.c` | encode (handle after init) |
| ssc_custom_mode_create | opus_custom_mode_create | **M** | tables extracted (OD7) | modes |
| ssc_ec_enc_init, ssc_ec_encode, ssc_ec_encode_bin, ssc_ec_enc_bit_logp, ssc_ec_enc_icdf, ssc_ec_enc_uint, ssc_ec_enc_bits, ssc_ec_enc_shrink, ssc_ec_enc_done, ssc_ec_tell_frac | entenc.c / entcode.c | **M** | 0002 | ec |
| ssc_ec_laplace_encode | ec_laplace_encode | S (on 0002) | | ec |
| ssc_rcp, ssc_sqrt, ssc_rsqrt_norm, ssc_cos_norm | mathops.c | S | | rcp, sqrt, rsqrt_norm, cos_norm (exhaustive where cheap) |
| ssc_bitexact_cos_asm | bitexact_cos ×2 | F (two lanes, int32 out) | | bitexact_cos (exhaustive 1…16383); differs only outside compute_theta's domain (F33.4) |
| ssc_frac_div32 | frac_div32 | S (on 0001) | 0001 | frac_div32, frac_div32_variants |
| ssc_haar1, ssc_hysteresis_decision | bands.c | S | | haar1, hysteresis_decision |
| ssc_encode_pulses | encode_pulses | S (on 0002) | | encode_pulses |
| ssc_alg_quant | alg_quant (+ exp_rotation) | **M** | 0003, 0012 | alg_quant |
| ssc_stereo_itheta | stereo_itheta | F (stereo=1, on 0001) | 0001 | stereo_itheta |
| ssc_compute_band_energies | compute_band_energies | F | | band_energies |
| ssc_normalise_bands | normalise_bands | F (in place) | | normalise_bands |
| ssc_amp2Log2, ssc_amp2Log2_asm | amp2Log2 | F (n=22, NEON pairs) | | amp2Log2 |
| ssc_spreading_decision | spreading_decision | **M** | 0005 | spreading_decision |
| ssc_quant_coarse_energy | quant_coarse_energy | **M** | 0006 | coarse_energy |
| ssc_quant_fine_energy | quant_fine_energy | F | | fine_energy |
| ssc_quant_energy_finalise | quant_energy_finalise | F | | energy_finalise |
| ssc_compute_allocation | compute_allocation (+ interp_bits2pulses) | F (frame struct, F33.5) | | compute_allocation |
| ssc_quant_all_bands | quant_all_bands | **M** | 0007 | quant_all_bands |
| ssc_patch_transient_decision | patch_transient_decision | **M** | 0011 | patch_transient |
| ssc_pitch_downsample | pitch_downsample | F (C=2, len 1888) | | pitch_downsample |
| ssc_pitch_search | pitch_search | **M** (no lp4 shift, calls tone_detect) | 0011 | pitch_search |
| ssc_remove_doubling | remove_doubling | **M** | 0004 (code-read only, OD12) | remove_doubling, remove_doubling_loud |
| ssc_pitch_xcorr_c1, ssc_pitch_xcorr_c2 | celt_pitch_xcorr | F (two fixed-shape variants) | | pitch_xcorr_c1, pitch_xcorr_c2 |
| _ssc_lpc | _celt_lpc | F (order 4, on 0001) | 0001 | lpc |
| _ssc_autocorr | _celt_autocorr | F (n 944, lag 4) | | autocorr |
| ssc_comb_filter, ssc_comb_filter_const_asm, ssc_comb_filter_function1_asm, ssc_comb_filter_function2_asm, ssc_comb_filter_function3_asm, ssc_comb_filter_function4_asm | comb_filter | **M** (NEON, tapset 0, Q31) | 0008 | comb_filter |
| ssc_fft | opus_fft | **M** (rewrite: Q31, radix 4/3, compact state) | 0009 (`ssc_mdct.c`) | fft |
| ssc_mdct_forward | clt_mdct_forward | **M** (rewrite: Q31 window/trig, exact π/4N rotation) | 0009 | mdct_forward, mdct_tables |
| ssc_compute_band_energies_for_lastband | — (stock compute_band_energies on slices) | **N** | | lastband_energies |
| tone_detect | — | **N** | 0011 | tone_detect |
| normal_headroom, uhq_headroom | — | **N** | `ssc_encoder.c` | headroom |
| SSC_DRC_Init, SSC_DRC_SetPar, set_DRC_limit_gain | — | **N** | `ssc_encoder.c` | drc_exe, drc_chain, encode |
| SSC_DRC_Exe | — | **N** | `ssc_encoder.c` | drc_exe, drc_chain |
| CalculateLimitGain | — | **N** | `ssc_encoder.c` | drc_limitgain |
| powercalc | — | **N** | `ssc_encoder.c` | drc_powercalc |
| Biquad_Narrow_int2int_stereo | — | **N** | `ssc_encoder.c` | drc_biquad |
| SSC_COPY8, SSC_COPY16, SSC_COPY32, SSC_CLEAR | OPUS_COPY / OPUS_CLEAR | N (trivial) | | encode (20 call sites) |
| ssc_preemphasis, headroom_process, get_header_from_bitrate, get_uhq_info_from_bitrate, set_limit_gain_from_bitrate, set_data_header, set_data_frame_header, last_band_info_calc, CalculateBandEnergy, CalculateBandEnergyLastBand, getPitchIndexGain, getPitchFilterThreshold, prefilter_gain_calculation, print_enc_fields, smull32_x86_scalable, ssc_ec_enc_patch_initial_bits | — | **D** | | none needed |

| Live static (Ghidra address) | libopus v1.1 | Class | Covered by |
|---|---|---|---|
| FUN_0010f88c | ec_enc_carry_out / normalize | M (0002) | ec |
| FUN_00113734 | quant_coarse_energy_impl | F | coarse_energy |
| FUN_001126bc | find_best_pitch | S (NEON `Syy`, F45.1) | pitch_search |
| FUN_001167d0 | exp_rotation | S | alg_quant |
| FUN_00109038, FUN_00109460, FUN_00109ca4 | quant_band, quant_partition (+ compute_theta), intensity_stereo | F | quant_all_bands |

**2. Create, destroy, get_size** (read from the disassembly at 0xa0b8, 0xa16c, 0x9f0c). `ssc_encoder_get_size(C, Fs)` = `C·(4·overlap + 6·nbEBands + 4096) + 0xdc`, which is v1.1's formula in fixed point (int32 `in_mem` and `prefilter_mem`, three int16 band-energy arrays) with the frame size fixed at 864. `ssc_encoder_create(Fs, C, bits, &err)` inlines it, `malloc`s, calls `ssc_encoder_init(st, Fs, C, bits)`, frees on error, and reports −7 (OPUS_ALLOC_FAIL) when malloc fails, as stock does. `ssc_encoder_destroy` is a tail call to `free`.

**3. Count by class.** Of the 69 live exports (counted from the table): API 4, S 9, F 14, M 27 (10 range-coder entry points under 0002 and 6 comb-filter symbols under 0008, so 13 distinct changes), N 15 (4 of them trivial copies). The Samsung-only and modified code is all in the patch series or `ssc_encoder.c`, and every live function is covered by a harness test, directly or through `encode`.

## F49 The stale `in[1071]` read is reached often and never reaches the output (closes F46.7)

F46.7 left a direct test of the stale read open, "no input here produced" a coarse pitch at lag 243. It turns out to be common. What never happens is the read changing anything downstream, so no natural input can drive a byte-level test of it.

**Method** (2026-10-27, x86_64, `native/build/libsscenc.a`; a scratch probe, not kept). Each frame is encoded from a saved copy of the handle twice: once as is, and once with `in[1071]` set to 0, `INT32_MAX` and `−INT32_MAX` in turn. Between them those three values flip `xcorr[244] ≤ xcorr[242]` either way. A scratch copy of the patched `pitch.c` counts three things:
- the calls where both candidates pass the threshold and `hi + 1 == max_pitch`, the stale read;
- whether `tone_detect`'s result changes when that comparison is forced each way;
- `pitch_search`'s return value in each run.

| Input | Frames | Stale read | Decides `tone_detect` | `pitch_search` differs | Bytes differ |
|---|---|---|---|---|---|
| Get Lucky, Do I Wanna Know, Nirvana SIDE-1 (240 s each, plugin level) at 96k/584k, 96k/152k, 48k/229k, 48k/96k | 239 996 | 31 120 (1.7–22 % per run) | 8 | 0 | 0 |
| Harmonic tones, f0 20 Hz–2 kHz in 1 % steps, 40 frames each, at 96k/584k and 48k/229k | 37 040 | 7 674 | 0 | 0 | 0 |

**Why it is absorbed.** With `hi = max_pitch − 1 = 243` the forward walk never runs, so `top = hi` and `best_pitch[0]` keeps the coarse best. Only `best_pitch[1]` (`top − d`, possibly −1) moves, by one coarse lag. The fine search evaluates ±2 around twice each candidate. Its two windows overlap and its winner sat in the same place every time. `pitch_search` then rewrites `xcorr[0..489)`, so no state differs after the frame either.

**Consequences.**
- The reference keeps seeding `ssc_pitch_xcorr` from `in[]` (D30): it costs nothing, and the function-level `pitch_search` test still shows the read matters there (F45.5).
- The encode test's per-frame `in_shared[282..)` comparison stays the guard. A dedicated test is not planned.
- This is not a proof. A fine-search maximum sitting exactly at a window edge near `lo` could still leak the stale value. None of ~277 000 frames did.

## F50 Native decoder tools; real phone→buds frames decode in sync; spectrograms (T3.3 done)

New, uncommitted, in `~/src/ssc-codec/native`:
- `ssc_native_dec.h`, the decoder set up as the harness's `decode_one`. It is the encoder's
  mode plus a v1.1 inverse MDCT, with the alloc table chosen per frame from the header.
- Two tools, `ssc_native_roundtrip` (encode, decode, SNR, WAV) and `ssc_native_decode`
  (`u32 len | bytes` frames → WAV, with the decode test's sync check).
- A `ssc_native_tables()` accessor in `ssc_native.c`.

The helper doesn't call the new code. `helper_compare.py` after the rebuild: 5 configurations × 3000 frames, identical. The installed helper was not replaced (nothing it does changed).

**1. Every capture we have decodes in sync, with a negative control.**

| Frames | Source | Mode | Result | Same frames, other mode |
|---|---|---|---|---|
| `ssc-capture/btsnoop_hci.log`, 12 455 frames, 225 s | **Galaxy A31 → Buds, over the air** (older, basic-only encoder; F-mode 192k, 484 B, header `ff ee xx f3`) | 48k | 0 header, range or sync errors; the range coder ends within 1 bit of the payload on all but 6 padded frames; RTP sequence has no gaps | 4190 range errors |
| `uhq584_getlucky.frames`, 6666 frames | S24U blob, uhq-584 | 96k | 0 errors; end 19/20/21/22 = 3121/3513/8/24 and 914 padded, exactly the qemu harness's numbers (F38.6) | 2445 range errors, 165 out of sync |
| `golden/*.golden`, 16 frames each | SSCOnWindows: 128k, 192k, 229k (48k mode); 250k, 442k, 584k (96k mode) | as listed | 0 errors | 2–7 range errors per file |

The A31 capture matters most. It is a different Samsung encoder build, its frames went over the air, and the buds played them. Our decoder reads them in sync on the S24U blob's 48k tables, with the shared alloc table (192k is not > 192000) and patches 0010/0013. The output is plausible music: mean −20.6 dBFS, peak −3.6 dBFS, harmonics and transients up to ~23 kHz, with a visible step at ~19.6 kHz. So for basic 192k, the A31's bitstream, tables included, is the one our decoder assumes. Extractor: a few lines over `parse_btsnoop.py`'s `l2cap_frames` (outgoing AVDTP media channel, RTP header stripped). It lived in a session scratch dir and is easy to rebuild.

**2. Native round trip, every configuration.** 240 s each of Get Lucky (m1), "Do I Wanna Know" (m2) and Nirvana SIDE-1 (m3), resampled to 96k or 48k at the plugin's level, through all 21 configurations. That is 1 119 972 frames, byte-identical to the blob's (F47), with **no decode error**. Waveform SNR (both channels, from frame 2, gain 1):

| Config | m1 | m2 | m3 |
|---|---|---|---|
| 96k 886k / 584k / 442k | 37.8 / 34.4 / 30.9 | 43.5 / 40.7 / 35.0 | 38.2 / 29.6 / 26.5 |
| 96k 308k / 291k / 250k | 27.8 / 27.2 / 26.3 | 32.3 / 31.9 / 30.9 | 25.0 / 24.7 / 24.1 |
| 96k 152k | 22.7 | 23.7 | 20.9 |
| 48k 328k / 256k (24-bit path, F46.2) | 28.3 / 27.0 | 33.9 / 30.1 | 33.1 / 29.6 |
| 48k 229k / 192k / 128k | 26.5 / 25.4 / 22.6 | 28.6 / 27.4 / 23.0 | 28.2 / 26.9 / 22.4 |
| 48k 96k / 88k | 19.8 / 19.4 | 20.4 / 19.6 | 20.0 / 19.2 |

The 44.1k creates give the same numbers as 48k on the same file (same mode). Get Lucky's fitted gain is 0.975–0.995: the DRC limiter riding a hot master (F44.5); the others are within 0.6 % of 1. Waveform SNR is no quality score for a perceptual codec. It is the baseline the O6 flags get measured against (T6.1).

**3. Spectrograms** (ffmpeg `showspectrumpic`, Nirvana SIDE-1 60–80 s, left channel; regenerate from the round-trip WAVs).
- **Source:** music to ~21 kHz, then the master's noise floor up to 48 kHz.
- **584k:** indistinguishable by eye, the >24 kHz floor included.
- **152k:** a hard cut at ~16 kHz, the end-18 + top-band-cut path (F46.2).
- **584k error signal** (decoded − source, 29.7 dB below the source, matching the measured SNR):
  - it follows the music's spectrum ~30 dB down;
  - it steps at band edges, near 15–16, 24 and 32 kHz;
  - above ~32 kHz it sits at the source's own noise-floor level, so the top band is reproduced as energy, not waveform.

  That last point is where T6.1 starts: in the 96k mode most of the error *power* above 24 kHz is the coded noise floor of the master, inaudible by construction.

**Pitfall.** ffmpeg's `amix` with `weights=1 -1` adds the inputs, not subtracts. Use `amerge` and `pan=stereo|c0=c0-c2|c1=c1-c3` for a difference signal.
