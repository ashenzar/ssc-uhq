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
