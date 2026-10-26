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
