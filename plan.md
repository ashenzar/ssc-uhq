# Plan

Status legend: [ ] todo, [~] in progress, [x] done, [-] dropped (with reason in decisions.md)

## Objectives

**O1. Map Samsung's SSC code in libbluetooth_jni.so**
- [x] T1.1 Disassemble; build a string→address index of .rodata.
- [x] T1.2 Resolve code references (adrp+add) to the SSC function-name and log strings.
- [x] T1.3 Identify function boundaries (26+ functions mapped; see findings.md F1).

**O2. Extract how the encoder is driven (UHQ and basic)**
- [x] T2.1 Arguments to `ssc_encoder_create` — rate, channels=2, bits=34, &err (F9).
- [x] T2.2 Samples per `ssc_encode` call — 864, hardcoded (F3, confirmed by F10).
- [x] T2.3 PCM format and scaling — S24_32 shifted left 6 bits to 2^29 (from code; see encoder.c:453).
- [x] T2.4 Bitrate selection — quality-to-rate table (F12), bits-from-caps (F13),
      per-frame bitrate from lookup tables (F11): UHQ = 584k/291k/152k.
- [~] T2.5 DRC/headroom globals — gain processing observed after encode (0x88ba94+); not yet traced.

**O3. Extract packetization**
- [x] T3.1 Frames per media packet — 1 frame/packet confirmed by disassembly (F10).
- [x] T3.2 RTP header — v=2, pt=96, ssrc=0xff, no SSC media payload header (see F7).
- [x] T3.3 RTP timestamp — per-packet counter +1 (SSCENC_RTP_TS=frames; see F2, F7).

**O4. Extract capability negotiation**
- [x] T4.1 Codec info — cap byte 0x0c for basic, 0x0e for UHQ; buds respond 0x3e.
      Rate = 96 kHz when UHQ2 bit (0x02) is set (see F8).
- [x] T4.2 Bitrate selection from caps — bits_per_sample chosen from cap bits (F13);
      bitrate selected per-frame from lookup table indexed by sample_mode (F11).
      UHQ sub-bitrates are 584k/291k/152k, not 442k/250k.

**O5. Implement the differences in our plugin**
- [x] T5.1 Discrepancy table: 9 rows, all resolved. Key fixes: graph rate (F6), bits=34 (F13).
- [x] T5.2 Implemented: 50-ssc-uhq-rate.conf adds 96000 to clock.allowed-rates.
- [x] T5.3 Plugin rebuilt with bits=34 default; installed and verified.

**O6. Validate**
- [x] T6.1 Offline checks: frame sizes correct (F3), golden comparison done (F4),
      mode adaptation verified (F5), resampling root cause identified (F6).
- [x] T6.2 Listening test: UHQ-584 bass timing confirmed fixed. uhq-291 plays (uhq-152 too).
- [x] T6.3 Adopted: bits=34 default, 96k allowed-rates config, uhq-584 active profile.

### Discrepancy table (T5.1)

| # | Aspect | Samsung phone (expected) | Our setup | Fix | Status |
|---|--------|--------------------------|-----------|-----|--------|
| 1 | Graph rate during UHQ | 96 kHz native (HAL offload) | 48 kHz → resampled to 96 kHz | Add 96000 to allowed-rates | config written, test pending |
| 2 | RTP timestamp | +1 per packet (A31 capture) | +1 per packet (SSCENC_RTP_TS=frames) | already fixed | ✓ working |
| 3 | PCM scaling | 2^29 full scale (unverified on the phone) | 2^29 (shift=6 from S24_32) | already fixed | ✗ wrong: 2^29 drives the DRC limiter (called "headroom" before F43) into gain riding (F38.7, F43); now 2^23, shift 0 by default (D33, F39.1) |
| 4 | Frame size at UHQ | 864 (hardcoded, F10: 0x88b9a0) | 864 | matches | ✓ confirmed |
| 5 | Frames per packet | 1 (F10: encode loop dispatches per frame) | 1 | matches | ✓ confirmed |
| 6 | UHQ sub-bitrates | 584k / 291k / 152k (F11 table) | uhq-442 and uhq-250 = **wrong values** | Change to uhq-291 (291000) / uhq-152 (152000) | **root cause of silence found** |
| 7 | bits_per_sample to encoder | **34** (F13: cap bit 3 = bitrate_limit) | 24 | Pass 34 to ssc_encoder_create | needs testing |
| 8 | Basic-mode sample rate | **44100** (F9: fallback when not 96000) | 48000 | Pass 44100 for basic modes | needs testing |
| 9 | Bitrate per-frame | Adaptive from lookup table (F11) | Fixed for session | Low priority (works at 584k) | — |

## Acceptance criteria
- **AC1** Function map in findings.md: address + evidence (string xref) for ≥ 8 of the T1.3 functions.
- **AC2** Parameter table in findings.md for T2.1–T2.4 and T3.1–T3.3, each with instruction
  addresses as evidence; anything not determinable is marked "unknown" with the reason.
- **AC3** Discrepancy table (T5.1) complete; every row has a decision (implement / skip + why).
- **AC4** Plugin builds; flags work; encoder CPU ≤ 20% of one core at UHQ 584; no errors in logs.
- **AC5** User listening, FLAC "Get Lucky" in mpv and Deezer HiFi: UHQ bass in time (not offbeat)
  compared with AAC, continuous for ≥ 5 minutes. SSC High and AAC unchanged (no regression).
- **AC6 (stretch)** `uhq-442` and/or `uhq-250` play if Samsung's logic explains their silence.
- **AC7** findings.md documents how to reproduce; memory notes updated.

## Out of scope
- An SSC decoder (none exists on the phone).
- Adaptive bitrate implementation (record Samsung's logic if found; implement later).
- Redistributing Samsung binaries or copying their code; we document behaviour only.
- Upstreaming to openssc / PipeWire.
