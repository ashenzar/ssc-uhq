# Decisions

**D1 — Static analysis only.** Disassemble `libbluetooth_jni.so` with `aarch64-linux-gnu-objdump`
plus small Python helpers. Running it (even under qemu) is impractical: it needs the full Android
Bluetooth stack, Binder and JNI. *Why:* lowest effort that can answer the questions.

**D2 — Analyse the S26 Ultra AZF4 build.** Its encoder is byte-identical to the S24 build we run,
and the firmware zip is kept locally for re-extraction. *Why:* consistent pairing of encoder and
the code that drives it.

**D3 — Locate functions through their log strings.** The binary is stripped, but Samsung's code
logs `__func__` names; code that references a function-name string belongs to (or calls into)
that function. *Why:* only reliable symbol source available.

**D4 — Every behaviour change ships behind an env flag, default unchanged, until accepted in a
listening test.** *Why:* the user's current setup (SSC High default) must keep working.

**D5 — Document behaviour, not code.** findings.md records parameters, constants and logic in our
own words with addresses as evidence; no Samsung code is copied into the plugin. *Why:*
interoperability purpose; respects Samsung's copyright.

**D6 — Safety for listening tests:** set volume to 10% before any new mode; user confirms before
raising it. *Why:* wrong streams can produce loud noise.

**D7 — Subagents:** use them for parallel per-function analysis once the function map (O1) exists;
the mapping itself is sequential. *Why:* parallelism only helps after the map is known.

**D8 — Interoperability scope.** All reverse engineering is for personal/educational use on
devices the user owns (Galaxy Buds3 Pro, ThinkPad, Galaxy A31, extracted S24U/S26U firmware).
No distribution of Samsung code or derived work. The analysis documents observable behaviour
(parameters, constants, control flow) in our own words with address evidence, to achieve
interoperability between the user's Linux system and their Bluetooth earbuds. *Why:* legitimate
reverse engineering for interoperability, protected under DMCA §1201(f) / EU Directive 2009/24
Art 6.

**D9 — Graph rate fix over encoder-level changes.** The first fix targets PipeWire's
`clock.allowed-rates` (add 96000) rather than codec code. *Why:* eliminates the BT-internal
resampler entirely, which is the most likely cause of frequency-dependent group delay.
If this doesn't fix the bass timing, the next step is a phone BT capture.

**D10 — Use Samsung's UHQ bitrate table (584k / 291k / 152k), not SSCOnWindows' list.**
uhq-442 and uhq-250 produce silence on the buds. Disassembly of `send_frames` shows Samsung
only sends bitrates from a 3-entry lookup table: 584000, 291000, 152000. The buds likely
only decode those tiers. *Why:* uhq-442 / uhq-250 were from SSCOnWindows; Samsung's phone
never sends them.

**D11 — bits_per_sample=34 behind SSCENC_BITS env var, default stays 24.** Samsung passes
bits=34 to `ssc_encoder_create` when the buds have the bitrate_limit capability flag. Our
default stays 24 (which works) until we verify 34 in a listening test. The probe shows the
only difference is the first frame's header byte; subsequent frames are identical. *Why:*
matches D4 (every change behind a flag).
*Update: bits=34 now the default after user confirmed audible improvement.*

**D12 — Fix resampler quality first.** PipeWire's default quality 4/14 produces audible
artifacts on 44.1→96 kHz (2.177:1 ratio). Set quality=10 in `stream.properties` for all
three PipeWire config paths (daemon, native clients, PulseAudio clients). *Why:* cheapest
experiment; one config change, no code modifications.

**D13 — Warm-up init: keep as-is.** Samsung's phone calls only `ssc_encoder_create`, but
our helper calls `get_size` → `init` → `create`. Byte-level comparison shows zero
difference across 4 configs × 30 frames. The init doesn't affect encoding. *Why:*
no-op change; removing it would risk breaking for no gain.

**D14 — Skip gain processing replication.** Samsung's post-encode gain ramp (F16) is for
BT volume transitions. Gains are 1.0 during normal playback — the block is skipped entirely.
PipeWire handles volume upstream. *Why:* not a quality factor.

**D15 — 44100 Hz basic rate behind SSCENC_BASIC_RATE env var, default stays 48000.**
Samsung uses 44100 for basic mode. At 44100, each frame covers 9% more time (19.6 vs
18.0 ms), giving the encoder more coding budget. For 44.1 kHz sources, this also
eliminates the 44.1→48 kHz resampling. *Why:* needs listening test before making default;
could affect 48 kHz content negatively if graph switches to 44.1 kHz.

**D16 — Adaptive bitrate: document, don't implement yet.** Samsung's BT stack drives
sample_mode (0-3) via link quality feedback. Our fixed index-0 (max quality) works for
stable connections. *Why:* requires BlueZ link quality monitoring; low priority until
dropout issues arise.

**D17 — RE Samsung's transport timing before writing any fix.** Samsung's BT stack uses
elapsed-time-driven frame scheduling (accumulate bytes based on real clock delta, not
audio graph quantum). Our PipeWire setup uses quantum-aligned encoding. The 44100 Hz
dropouts are a scheduling mismatch, not an encoder bug. Understand Samsung's model first.
*Why:* The naive "set rate=44100" caused periodic 5s-on/1s-off dropouts. The encoder works
(verified offline). The problem is how PipeWire's media-sink times the AVDTP writes.

**D18 — Don't modify media-sink.c.** All fixes go in a2dp-codec-ssc.c or PipeWire config.
*Why:* media-sink.c is upstream PipeWire code serving all BT codecs. Patching it creates
merge conflicts on every PipeWire update and risks breaking SBC/AAC/aptX/LC3.

**D19 — 44100 Hz remains behind SSCENC_BASIC_RATE, default stays 48000.** Even after fixing
the transport timing, 44100 is opt-in.
*Why:* 48 kHz with quality=10 resampler works for all content. 44100 only benefits native
44.1 kHz sources. User preference, not a regression fix.

**D20 — Test quantum=864 first (cheapest experiment).** PipeWire's `force-quantum` can set
the graph quantum to exactly 864 samples. At 44100 Hz, quantum=864 → 19.59 ms per quantum
= exactly one SSC frame. Eliminates the quantum/frame misalignment entirely.
*Why:* Zero code changes, just PipeWire config. If it works, the entire transport timing
problem disappears for SSC at 44100 Hz.

**D21 — Abandon native 44100 Hz basic-mode SSC.** Live testing proves the buds decode
basic-mode frames at a fixed 48 kHz DAC rate. Sending 44100 Hz frames causes 8.8% speedup
and periodic gaps. Samsung's phone resamples 44100→48000 before encoding.
*Why:* The buds' firmware controls the decode rate. No amount of transport timing fixes
can change it. The quality=10 PipeWire resampler (44.1→48 kHz) is the correct path.

**D22 — UHQ-584 with graph at 48k as new default.** The double resample path
(44.1→48k at 1.088:1, then 48→96k at 2:1 integer) beats both basic-229 (lower bitrate)
and UHQ with graph at 96k (ugly 2.177:1 ratio). 584 kbps with no audible resampling
artifacts. ssc-auto disabled — no profile switching needed.
*Why:* User confirmed UHQ-584 via double resample sounds best for 44.1k content. Since
all their music is 44.1k, this is the optimal permanent config.

**D23 — ssc-auto handles both rate toggle and profile switching.** When on UHQ, only
the graph rate toggles (instant). When on AAC/basic and a 96k stream appears, ssc-auto
saves the current profile, switches to uhq-584 (~15s reconnect), unlocks 96k, and
restores the original profile when hi-res playback ends. State file in
`$XDG_RUNTIME_DIR/ssc-auto.saved` survives WirePlumber restarts. 30s cooldown before
restoring to prevent thrashing on pause/unpause.
*Why:* user wants to listen to hi-res content regardless of current profile, without
manual intervention. The 15s reconnect is unavoidable for non-UHQ→UHQ transitions but
acceptable for a rare event.

**D24 — ssc-auto service not bound to WirePlumber.** Removed `BindsTo=wireplumber.service`
from ssc-auto.service. ssc-auto now survives WirePlumber restarts (which it triggers
itself during profile switches). Transient pactl/pw-metadata failures during restart are
silently ignored; the poll loop recovers once WirePlumber is back.
*Why:* BindsTo killed ssc-auto when it triggered a profile switch (ssc-switch restarts
WirePlumber). Without BindsTo, ssc-auto stays alive through the restart and continues
monitoring after the 18s sleep.

**D25 — Keep UHQ-584 as permanent default; ssc-auto only handles rate toggling in
normal use.** The profile-switching path in ssc-auto (D23) exists for edge cases. Day-to-day
operation: UHQ-584 profile stays fixed, ssc-auto toggles graph rate between 48k (44.1k
content, double resample) and 96k (hi-res content, native). Both transitions are instant.
*Why:* Keeping UHQ as default eliminates the 15s reconnect for the common hi-res→normal
transition. Only pays the reconnect cost if user manually switches to AAC/basic and then
plays a 96k track.

**D26 — Encoder is a pure codec; all resampling external.** Tested: feeding identical PCM
with rate=44100 vs rate=48000 produces byte-identical output for all modes. The encoder
does not resample. Samsung's AudioFlinger resamples before the A2DP encoder. Our
PipeWire 44.1→48k + BT-sink 48→96k pipeline produces the same 660B E-mode frames
Samsung's pipeline does. Whether our two-step resampling (1.088:1 + 2:1) sounds better
or worse than Samsung's one-step (2.177:1) is unproven — AudioFlinger's resampler is
proprietary. See F26.
*Why:* Closes the question of whether the encoder has internal rate intelligence.
Confirms our pipeline is architecturally correct — not that it's better or worse than
Samsung's.

**D27 — Phase 4: rebuild the SSC codec natively, on a libopus CELT base.** The blob is
fixed-point CELT with an `ssc_` prefix (F27). We start from libopus source and
reverse-engineer only Samsung's changes, instead of decompiling all 64 KB of `.text`.
*Why:* the best config still has audible noise and artifacts at 96k (user, 2026-10-26).
Every external factor has been tuned, so the remaining unknown is inside the encoder,
which we cannot observe or change while it is a qemu-hosted blob.

**D28 — Phase 4 implementation follows the decompiled logic; the repo stays private and
carries no Samsung data.** This supersedes D5's "no Samsung code copied" for Phase 4.
Constant tables (band edges, alloc vectors, eMeans, windows, twiddles, header tables) are
extracted at build time from the user's own blob by a script, never committed. The
existing README rule still applies: the user supplies their own encoder.
*Why:* personal interoperability work (D8). Keeping tables out of git preserves the
"no Samsung material distributed" property and ties the build to a blob the user owns.

**D29 — Decoder before encoder.** *Why:* there has never been a way to measure SSC output
objectively. Every quality judgement so far has been a listening test. A decoder tells us
whether the 96k noise is born in the encoder, validates the bitstream understanding
independently of bit-exactness, and is the yardstick for all O6 tuning.

**D30 — Bit-exact first, improve second.** The native encoder must reproduce the blob's
bytes on the whole corpus before any behaviour change. *Why:* the blob is the only
correctness oracle. Mixing "rebuild bugs" with "intended improvements" would make both
impossible to debug, and a non-bit-exact encoder risks frames the buds decode wrongly.

**D31 — Differential testing per exported function, under qemu.** *Why:* the blob exports
nearly every internal function by name (F27), so each one can be called in isolation with
chosen inputs and its output compared with our C version. That turns the rebuild into many
small verifiable steps.

**D32 — Rule out timing and transport before investing in the rebuild (Phase 4 O0).**
*Why:* qemu-hosted encoding at 9 ms frames and AX201 Wi-Fi/BT coexistence can both cause
clicks or dropouts that sound like "artifacts". Both are cheap to measure. If either is the
cause, native execution (O5) or a radio fix matters more than encoder internals.

**D33 — Feed the encoder plain 24-bit (2^23): `SSCENC_INPUT_SHIFT` default 0, not 6.**
Supersedes the 2^29 scaling in plan.md T2.3 / discrepancy #3. At 2^29 the blob's headroom
stage rides the gain frame by frame, unsignalled, in steps up to 14.5 dB (F38.7). On the
buds, Get Lucky 88.2k sounded terrible at 6 and clean at 0 (F39.1). Changed on 2026-10-26:
`config/bin/ssc-switch` (installed to `~/.local/bin`), `~/src/ssc-capture/set_ssc.sh` and
`abtest.sh` (`SHIFT=${2:-0}`), `config/systemd-user/.../sscenc.conf`, and the plugin patch
default (`0u`). The installed plugin `.so` still has the compiled default 6, but the conf
always sets the variable.
*Why:* the user's A/B plus the measurement. The 2^29 value came from an SSCOnWindows
comment, never from the phone. Basic mode (48k) may want an even lower level (F39.4); this
decision does not settle it.

**D34 — Basic mode feeds the encoder 16-bit (2^15); `SSCENC_INPUT_SHIFT` is relative to the mode.**
Extends D33. The 48 kHz mode is linear only up to 2^15; at the 2^23 that D33 left it at, it
rides the gain like UHQ at 2^29 (F40). The plugin patch now subtracts 8 from the shift when the
encoder rate is not 96000 and rounds S24 to 16-bit. Shift 0 now means each mode's nominal level
(UHQ 2^23, basic 2^15), so `ssc-switch`/`set_ssc.sh` need no change beyond help text.
*Why:* measured on two tracks at seven levels, identical at create rate 44100 and 48000. The
installed plugin `.so` is not rebuilt; basic mode stays hot until it is.

**Note on D33/D34 (2026-10-26, F43):** where these say "headroom stage", read **DRC limiter**
(`SSC_DRC_Exe`). The headroom functions are a fixed spectral EQ. Both decisions stand: the
limiter threshold is on the 24-bit scale for UHQ and the 16-bit scale for basic with bits=34.

**D35 — The native encoder reaches the plugin through the helper launcher; the blob stays the default.**
Proposed in-session on 2026-10-26 (F47). In effect the same day: the user asked to "build native
and change sscenc to tee", and the plugin has run `SSCENC_BACKEND=tee` since 18:23 (F47.5).
Build-time table extraction (OD7) is what that build uses; the user hasn't commented on it
separately.
**Update (2026-10-26, ~18:28):** after tee showed 0 differing frames, the user switched the live
backend to `native` and reported it sounds really good (F47.6). The launcher's default without
the variable is still the blob.
- `ssc-codec/native` builds `ssc_native_helper`, which speaks `ssc_blob_helper`'s protocol.
  `config/bin/sscenc-blob-run` selects it by `SSCENC_BACKEND` (`blob` by default, `native`,
  `tee`), so the plugin itself doesn't change.
- Samsung's tables are extracted from the user's blob at build time into a gitignored header,
  not parsed at runtime (OD7).
- `harness/ssc_encoder.c` stays the one source for both the qemu harness and the native
  build (OD24).

*Why:* the native helper is bit-exact with the blob on every frame tested (F46, F47). The
launcher already isolates the encoder in its own process, so swapping helpers changes
nothing else in the pipeline. `tee` gives the live bit-exact check (T5.2) without changing
what the buds receive. The blob stays the default until the T5.3 listening test (D6, volume
10 % first).

**D36 — Phase 4 codebase: libopus v1.1 plus a patch series, in `~/src/ssc-codec`.**
Promoted on 2026-10-27 from OD2, OD3, OD5, OD6 and OD16, which were settled and in use during Phase 4.
- The rebuild lives in its own repo, `~/src/ssc-codec`, not here (OD2).
- libopus is pinned at tag `v1.1` (F29) as `third_party/opus`. Every Samsung delta is a numbered
  patch in `harness/patches/`, 0001–0013 so far (OD3, OD16). The only new source is
  `harness/ssc_encoder.c` (the `ssc_encode` top level, headroom and DRC; OD24, D35).
- Static analysis uses Ghidra 12.2 headless at `~/.local/opt/ghidra` (OD5). Transcriptions are
  checked against the disassembly, not the decompile (F43.2).
- Vinchethescript's `encoder.c` is not used (OD6). Our own decompile plus the differential harness
  replaced it.

*Why:* a patch per delta keeps each Samsung change small, named and testable on its own. A
patch's removal must fail its test (F31, F33), and stock v1.1 stays one `make STOCK=1` away.
A separate repo keeps codec work out of the plugin/config repo, and no Samsung code or tables
enter either one (D28).

**D37 — Verification method: stock C first, the reference mode from the blob, transcribe only on mismatch.**
Promoted on 2026-10-27 from OD13 and OD14.
- Each blob function is first tested against stock v1.1 C on the encoder's real domain. Only a
  mismatch leads to a patch, transcribed from the disassembly (OD14; comb_filter needed one, 0008).
- Under qemu the harness builds its reference `CELTMode` at runtime from the blob's own
  `ssc_custom_mode_create`, so the harness source carries no Samsung tables (OD13, F33). The
  native build extracts them at build time instead (OD7, D35).

*Why:* most of the blob is specialised stock code (F30, F48). Testing stock first finds the real
deltas without transcribing code that is already open source.

**D38 — Phase 4 ends at a byte-identical encoder; tuning (O6) is out of scope.**
User decision, 2026-10-27: "document it as out of scope, we only needed byte-identical for today."
The native encoder is byte-identical to Samsung's blob on every frame tested and is live (F46, F47).
O6 (T6.1–T6.3), AC6 and the T3.4 per-band map that fed it are dropped. Any future tuning is a new
phase, flag-gated with the byte-identical encoder as the default (D4, D30).
*Why:* the phase's purpose for the user was reproducing Samsung's encoder exactly, and that is
done. Tuning would by definition give up byte-identity with Samsung's output.
