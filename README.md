# SSC UHQ on Linux: match Samsung's encoder driving logic

> **Disclaimer.** This project does NOT include or distribute Samsung's proprietary
> `libScalable_Encoder.so` codec. You must extract it yourself from official Samsung
> firmware for a device you own. See [Obtaining the encoder](#obtaining-the-encoder) below.
> The author did all the reverse engineering documented here for personal interoperability
> on their own devices, under DMCA §1201(f) and EU Directive 2009/24 Art 6.

## Goal
Make SSC UHQ (24-bit, 96 kHz) on the Galaxy Buds3 Pro sound correct from Linux. The method
is to learn from Samsung's own Bluetooth stack how a phone drives `libScalable_Encoder.so`
and packetizes SSC, then reproduce that behaviour in our openssc-based PipeWire plugin.

Phase 4 then rebuilt the encoder itself. `codec/` is libopus
v1.1 plus a patch series that reproduces `libScalable_Encoder.so` byte for byte. The plugin
runs it natively instead of running the blob under qemu.

Phase 5 brought UHQ to a phone that doesn't support it. On a Galaxy A51, a patched Bluetooth
stack and the S24U encoder stream UHQ-584 to the buds. See [Galaxy A51](#galaxy-a51-phase-5).

## Current state (2026-10-27)
- The plugin uses the native encoder rebuild (`SSCENC_BACKEND=native`, `codec/`). It is bit-exact with Samsung's blob and needs no qemu (F46, F47). The blob under qemu is the fallback (`SSCENC_BACKEND=blob`).
- The default profile is UHQ-584 (584 kbps, 96 kHz codec, bits=34).
- SPA resample quality is 10 (160 taps, cutoff 0.958, PipeWire 1.6.8's default exp window). Quality 14 (1024 taps) sounded no different (D39).
- Every profile (uhq-584, uhq-291, uhq-152, basic-229) sounds clean at shift 0 with 44.1k to 96k sources, with Wi-Fi on or off (F41). The old ranking of profiles came from listening with the encoder input too hot (F38.7, F40), so it is withdrawn.

### Graph modes

`ssc-switch status` shows `[profile mode]`, for example `[uhq direct]`.

| Mode | Graph rate | 44.1k source | 48k source | 88.2k source | 96k source |
|------|-----------|-------------|-----------|-------------|-----------|
| direct | 96k locked | SPA 44.1→96k (2.177:1) | SPA 48→96k (2:1) | SPA 88.2→96k (1.088:1) | passthrough |
| normal | 48k locked | SPA 44.1→48k (1.088:1) + BT-sink 48→96k (2:1) | passthrough + BT-sink 48→96k (2:1) | 88.2→48k (downmix!) + 48→96k | 96→48k (downmix!) + 48→96k |
| hires | 48k+96k | SPA 44.1→48k + BT-sink 48→96k | passthrough + BT-sink 48→96k | SPA 88.2→96k (1.088:1) | passthrough |

- **direct.** One SPA resample step to 96k. Use it for mixed-rate sources. A 44.1k source
  gets the hard 2.177:1 ratio, and quality 10 handles it as well as 14 did to the ear (D39).
- **normal.** A 44.1k source gets two resamples, each with a clean ratio. Use it if you only
  listen to 44.1k. It destroys 88.2k and 96k content by downsampling to 48k and back up.
- **hires.** PipeWire picks the closest allowed rate, so every source gets a clean ratio.
  This is the best default, and ssc-auto switches to it when it detects a hi-res stream.

### Hardware
- ThinkPad (Intel AX201), Manjaro, PipeWire 1.6.8 with a patched bluez5 plugin
  (HFP mic fix) and the openssc SSC codec plugin
- Galaxy Buds3 Pro, capability byte 0x3e
- Encoder: the native x86_64 helper since 2026-10-26, built with tables extracted from the S24U blob. The fallback is Samsung's S24U `libScalable_Encoder.so` under qemu-aarch64 and systemd-run.

### Config files
| File | Purpose |
|------|---------|
| `~/.config/systemd/user/wireplumber.service.d/sscenc.conf` | SSC codec env vars (profile, bits, encoder blob, RTP timestamps) |
| `~/.config/systemd/user/wireplumber.service.d/spa-bluez5-fix.conf` | HFP mic fix (patched libspa-bluez5.so path) |
| `~/.config/pipewire/pipewire.conf.d/50-ssc-uhq-rate.conf` | Graph allowed rates (default: 48k locked) |
| `~/.config/pipewire/pipewire.conf.d/51-resample-quality.conf` | SPA resample quality (10) |
| `~/.config/pipewire/pipewire-pulse.conf.d/51-resample-quality.conf` | SPA resample quality for PulseAudio clients (10) |
| `~/.config/pipewire/client.conf.d/51-resample-quality.conf` | SPA resample quality for native PW clients (10) |
| `~/.config/wireplumber/wireplumber.conf.d/52-buds-default-a2dp-ssc.conf` | Default to SSC when the buds reconnect |
| `~/.config/systemd/user/ssc-auto.service` | systemd user service for ssc-auto (disabled by default) |

`config/` holds a backup of every config, laid out to mirror the install paths. To restore
them after a PipeWire or WirePlumber update:
```sh
# 1. Restore configs
cp config/systemd-user/wireplumber.service.d/* ~/.config/systemd/user/wireplumber.service.d/
cp config/systemd-user/ssc-auto.service        ~/.config/systemd/user/
cp config/pipewire.conf.d/*                    ~/.config/pipewire/pipewire.conf.d/
cp config/pipewire-pulse.conf.d/*              ~/.config/pipewire/pipewire-pulse.conf.d/
cp config/client.conf.d/*                      ~/.config/pipewire/client.conf.d/
cp config/wireplumber.conf.d/*                 ~/.config/wireplumber/wireplumber.conf.d/
cp config/bin/*                                ~/.local/bin/
systemctl --user daemon-reload && systemctl --user restart wireplumber
```

### Rebuilding after a PipeWire update

Each PipeWire update requires rebuilding the patched `.so` files in
`~/.local/lib/spa-0.2-patched/bluez5/`. There are two patches.

**1. HFP mic fix.** The source is your `pipewire-hfp-atbcc-fix` checkout (see below).
```sh
cd pipewire-hfp-atbcc-fix && ./build-and-install.sh
```
This builds `libspa-bluez5.so` with the eSCO patches and installs it to
`~/.local/lib/spa-0.2-patched/bluez5/`. Upstream: pipewire#5506.

**2. SSC codec plugin.** The patch is `config/ssc-pipewire-1.6.8.patch`.
```sh
# Run from the ssc-uhq checkout
SSC_UHQ=$PWD
# Clone PipeWire source matching your version
VER=$(pipewire --version | awk '/Compiled with/{print $NF}')
mkdir -p ~/.cache/openssc-build && cd ~/.cache/openssc-build
git clone --depth 1 --branch $VER https://gitlab.freedesktop.org/pipewire/pipewire.git pipewire-$VER
cd pipewire-$VER

# Apply SSC patch (may need manual adjustment for new PipeWire versions)
git apply "$SSC_UHQ/config/ssc-pipewire-1.6.8.patch"

# Build just the bluez5 plugin
meson setup build '-Dsession-managers=[]' -Dexamples=disabled -Dtests=disabled
ninja -C build spa/plugins/bluez5/libspa-codec-bluez5-ssc.so

# Install SSC codec plugin alongside HFP-patched bluez5
cp build/spa/plugins/bluez5/libspa-codec-bluez5-ssc.so \
   ~/.local/lib/spa-0.2-patched/bluez5/
systemctl --user restart wireplumber
```

The build depends on three things:
- your extracted `libScalable_Encoder.so`, for example from S24 Ultra firmware via
  samloader-rs, with its path in `SSCENC_BLOB_SO`
- the native helper at `~/.local/libexec/sscenc/x86_64/`, whose build extracts the tables
  from the blob
- openssc's blob helper at `~/.local/libexec/sscenc/aarch64/`, needed for
  `SSCENC_BACKEND=blob` or `tee`

### Tools
| Tool | Purpose |
|------|---------|
| `ssc-switch {aac\|high\|uhq\|uhq291\|uhq152}` | Switch SSC profile (about 15 s Bluetooth reconnect) |
| `ssc-switch {direct\|hires\|normal}` | Switch graph mode (instant, no audio cut) |
| `ssc-switch status` | Show the current `[profile mode]` and codec details |
| `ssc-auto` | Switch graph mode automatically from stream rates (disabled by default) |

### Key findings
- The encoder is a pure codec. It does NOT resample, and its rate parameter only affects bit allocation (F26).
- The buds decode basic-mode SSC at a fixed 48 kHz DAC rate, so native 44100 Hz is not viable (F20).
- Samsung's UHQ bitrate table is 584k, 291k and 152k, NOT 442k and 250k (F11).
- Samsung's "up to 2,304 kbps" marketing figure is the raw PCM ceiling. The real maximum over classic A2DP is 584k (F23).
- bits=34 is a mode flag, not a PCM depth. Buds with capability 0x3e require it (F13).
- The encoder is a fork of fixed-point CELT from libopus v1.1 (F27, F29). Samsung changed it
  with 13 patches and added its own limiter, headroom EQ and adaptive top band. F48
  classifies every exported function.
- The native rebuild produces the same bytes as the blob on every frame tested: about 1.4 M
  frames under qemu, about 140 k on x86_64, and 23 785 live (F46, F47).
- The old UHQ noise came from the input level driving Samsung's in-encoder DRC limiter
  (F39, F43). Feeding plain 24-bit or 16-bit samples fixed it (D33, D34).
- Our decoder decodes, in sync, real frames that a phone sent to the buds, taken from a
  Galaxy A31 btsnoop capture (F50).

## Galaxy A51 (Phase 5)

A Galaxy A51 (SM-A515F, One UI 5.1, firmware A515FXXU8HWK1) streams UHQ-584 to the Buds3
Pro: 96 kHz E-mode frames of 660 B, 111 packets/s, one frame per packet. Stock, it only does
basic 48k. The user reports it sounds clean, and the frames decode with 0 errors on the PC
(F55, F56). `plan-phase5-a51.md` has the patch table and the test procedure.

**What it needs.** An unlocked bootloader and Magisk root. Flash only the Magisk-patched
`boot.img` and keep the stock vbmeta, because a custom vbmeta doesn't boot. You also need
two files from firmware you own: the A51's `libbluetooth_jni.so` from the
`com.android.btservices` APEX (sha256 `e2228cc1…6943`) and the S24 Ultra encoder (above).

**Build and install** (paths as in `tools/a51_build_module.sh`; adjust `BLOB` for yours):
```sh
tools/a51_build_module.sh                 # patches the stack, adds the encoder and the helper
adb push ~/src/ssc-blob/a51/magisk/ssc-uhq-a51-v0.4.zip /data/local/tmp/
adb shell su -c 'magisk --install-module /data/local/tmp/ssc-uhq-a51-v0.4.zip'
adb reboot
```
The zip contains Samsung's code, so keep it to yourself. Building it needs
`aarch64-linux-gnu-{as,ld,objcopy}`, `javac` and `d8` from Google's `r8.jar`.

**What the module does at boot.**
- It checks that both stock files match the firmware the patch was built for. If not, it
  mounts nothing.
- It bind-mounts the patched stack (`tools/a51_patch_jni.py`, v14) and the S24U encoder
  inside zygote's mount namespace, then restarts Bluetooth.
- It watches for an SSC stream set up at 48 kHz. That happens when the buds start the
  connection, because they then always pick basic. The watcher reconnects them from the
  phone, which gets UHQ (F57). Expect a dropout of a few seconds after the buds connect.

**What the patch changes** (33 instructions, a 14-instruction wrapper and one table; F51 to F56):
- The phone sends and advertises the UHQ2 flag (config 0x0e).
- With UHQ2, the codec, track and encoder rates become 96 kHz. The 96k bitrate table
  becomes 584/584/291/152k.
- The stack asks the audio HAL for 32-bit PCM and sizes its feeding budget for 4-byte
  samples.
- It reads, encodes and sends one 864-sample frame per packet. Two frames overflowed the
  1005 B MTU, and the split packets played distorted.
- The input is shifted right by 8 (Q31 to 2^23) before `ssc_encode`, the same scale as the
  Linux plugin's `SSCENC_INPUT_SHIFT=0`.
- User sample-rate and bit-depth preferences are ignored for SSC, since it offers one choice.

**Limits.**
- The source file's sample rate never changes the mode. The link stays whatever the
  connection set up, and AudioFlinger resamples to it (F56).
- The patch is tied to this firmware. Each patch asserts the stock instruction first, so
  `a51_patch_jni.py` fails loudly on another build.
- If zygote restarts without a reboot, the mounts are gone until the next boot.

## Building from scratch

The commands below assume you cloned the repos (ssc-uhq, openssc, pipewire-hfp-atbcc-fix)
side by side in one working directory and run them from there, unless a step says
otherwise.

**Not yet portable.** A few files still contain this machine's paths and the buds' MAC
address: the plugin patch's launcher path, `sscenc-blob-run`, `spa-bluez5-fix.conf` and
`ssc-switch`. `plan-phase4.md` lists them with their fixes under "Portability" (P1 to P10).

### Prerequisites

```sh
# Arch/Manjaro
sudo pacman -S --needed git meson ninja gcc pkgconf glib2-devel bluez-libs \
  sbc libusb opus liblc3 qemu-user-static aarch64-linux-gnu-gcc
```

### Obtaining the encoder

Samsung's `libScalable_Encoder.so` is proprietary and this repo does NOT include it. You must
extract it from official firmware for a Samsung device you own.

**Option A, adb from your phone.** This is the easiest way and works on any Galaxy with SSC support.
```sh
adb pull /system/lib64/libScalable_Encoder.so libScalable_Encoder.so
```

**Option B, official firmware.** Use this for the S24 Ultra or S26 Ultra encoder, which supports UHQ.
```sh
# Download firmware (samloader-rs, github.com/martinlopez1234/samloader-rs)
samloader download -m SM-S928B -r EUX -o firmware.zip

# Extract super.img from AP tar, then the system partition
# (extract_super_partition.py or lpunpack + dump.erofs)
dump.erofs --cat system/lib64/libScalable_Encoder.so > libScalable_Encoder.so
```

Known working encoders:
- S24 Ultra (S928BXXU6EZIF), sha256 `90c4c1cb...4ce7`, full UHQ support (E-modes)
- Galaxy A31 (A315GDXS5DWK2), sha256 `b923b408...5959`, basic modes only, no UHQ
- Galaxy A51 (A515FXXU8HWK1), sha256 `9248e264...b461`, basic modes only, no UHQ. Phase 5 replaces it with the S24 Ultra encoder.

### Building the blob helper (qemu bridge)

A helper binary runs the ARM64 encoder under qemu-aarch64:
```sh
git clone https://github.com/sachk/openssc && cd openssc
# Build the blob helper and shim libraries
make -C sscenc/blob CROSS=aarch64-linux-gnu-
mkdir -p ~/.local/libexec/sscenc/aarch64 ~/.local/libexec/sscenc/shims
cp sscenc/blob/ssc_blob_helper ~/.local/libexec/sscenc/aarch64/
cp sscenc/blob/shims/*.so ~/.local/libexec/sscenc/shims/
```

### Building the PipeWire SSC plugin

**1. HFP mic fix.** Optional, but recommended for Galaxy Buds.
```sh
git clone https://github.com/abrus861/pipewire-hfp-atbcc-fix
cd pipewire-hfp-atbcc-fix && ./build-and-install.sh
```

**2. SSC codec plugin.**
```sh
SSC_UHQ=$PWD/ssc-uhq
VER=$(pipewire --version | awk '/Compiled with/{print $NF}')
mkdir -p ~/.cache/openssc-build && cd ~/.cache/openssc-build
git clone --depth 1 --branch $VER \
  https://gitlab.freedesktop.org/pipewire/pipewire.git pipewire-$VER
cd pipewire-$VER

# Apply SSC patch
git apply "$SSC_UHQ/config/ssc-pipewire-1.6.8.patch"

# Build
meson setup build '-Dsession-managers=[]' -Dexamples=disabled -Dtests=disabled
ninja -C build spa/plugins/bluez5/libspa-codec-bluez5-ssc.so

# Install to user-local SPA path
cp build/spa/plugins/bluez5/libspa-codec-bluez5-ssc.so \
   ~/.local/lib/spa-0.2-patched/bluez5/
```

### Native encoder (recommended, Phase 4)

`codec/` rebuilds Samsung's encoder natively. `codec/README.md` describes the sources and the build. Its output is byte-identical
to the blob's (F46, F47), and it uses about 2 % of a core. Its helper speaks the blob
helper's protocol, so the launcher picks the backend from `SSCENC_BACKEND`:
- `native` runs without qemu. This setup uses it.
- `blob` is the launcher's default when the variable is unset.
- `tee` runs both, sends the blob's frames, and logs mismatches to the journal.

The build still needs your blob, because it extracts Samsung's tables from it into a
gitignored header (D28, OD7).
```sh
git -C ssc-uhq submodule update --init        # libopus v1.1
make -C ssc-uhq/codec/native BLOB=/path/to/libScalable_Encoder.so   # extracts the tables from your blob
mkdir -p ~/.local/libexec/sscenc/x86_64
cp ssc-uhq/codec/native/build/ssc_native_helper ~/.local/libexec/sscenc/x86_64/
# then add Environment=SSCENC_BACKEND=native (or tee) to sscenc.conf and restart wireplumber
```
Two more binaries run the matching decoder on the host (F50).
`codec/native/build/ssc_native_roundtrip` encodes, decodes, reports SNR and writes a WAV.
`ssc_native_decode` turns captured frames into a WAV and checks sync.

### Installing configs and scripts

```sh
cd ssc-uhq

# Configs
mkdir -p ~/.config/systemd/user/wireplumber.service.d \
         ~/.config/pipewire/pipewire.conf.d \
         ~/.config/pipewire/pipewire-pulse.conf.d \
         ~/.config/pipewire/client.conf.d \
         ~/.config/wireplumber/wireplumber.conf.d

cp config/systemd-user/wireplumber.service.d/* ~/.config/systemd/user/wireplumber.service.d/
cp config/systemd-user/ssc-auto.service        ~/.config/systemd/user/
cp config/pipewire.conf.d/*                    ~/.config/pipewire/pipewire.conf.d/
cp config/pipewire-pulse.conf.d/*              ~/.config/pipewire/pipewire-pulse.conf.d/
cp config/client.conf.d/*                      ~/.config/pipewire/client.conf.d/
cp config/wireplumber.conf.d/*                 ~/.config/wireplumber/wireplumber.conf.d/

# Scripts (the helper launcher goes where the plugin looks for it)
cp config/bin/ssc-switch config/bin/ssc-auto ~/.local/bin/
chmod +x ~/.local/bin/ssc-switch ~/.local/bin/ssc-auto
cp config/bin/sscenc-blob-run ~/.local/libexec/sscenc/

# Edit sscenc.conf: set SSCENC_BLOB_SO to YOUR encoder path
nano ~/.config/systemd/user/wireplumber.service.d/sscenc.conf

# Apply
systemctl --user daemon-reload && systemctl --user restart wireplumber
```

### Verifying

```sh
ssc-switch status          # should show [uhq direct] or [uhq normal]
pactl list cards | grep SSC  # should show a2dp-sink-ssc profile
```

## Source under analysis
The source is `libbluetooth_jni.so` from the Galaxy S26 Ultra Bluetooth APEX
(`com.android.bt.apex`), firmware S948BXXS3AZF4 (official Samsung FUS download).
- libbluetooth_jni.so has sha256 108a25d0…e0bb (14.4 MB, aarch64, stripped). The only way
  to identify its SSC functions is through the function-name strings they log.

## Files
- `config/` has the backed-up configs, scripts and the PipeWire SSC patch.
- `plan.md` has the Phase 1 objectives, tasks and acceptance criteria.
- `plan-phase2.md` covers Phase 2 quality tuning. It is mostly complete. T5.2, adaptive bitrate, remains.
- `plan-phase3.md` covers Phase 3 AVDTP timing. It is closed as moot per D21 and F20.
- `plan-phase4.md` covers the Phase 4 native codec rebuild on a libopus CELT base. The phase is done in scope (D38). Its goal was a byte-identical encoder, and tuning (O6) is out of scope. The buds now play the native encoder (`SSCENC_BACKEND=native`, no qemu). It is bit-exact with Samsung's blob on every frame tested, uses about 2 % of a core, and the user says it sounds really good (F46, F47). The decoder ends every frame with the encoder's energies (patch 0013). The original artifact came from the input level driving Samsung's DRC limiter (F43), and D33 and D34 fixed it.
- `plan-phase5-a51.md` covers Phase 5, UHQ from a Galaxy A51. T1 to T6 are done: negotiation, 96 kHz, 32-bit feeding, one frame per packet, input scale, persistence and buds-initiated connections.
- `decisions.md` is the decision log (D1 to D39).
- `findings.md` lists evidence-backed results with addresses and parameters (F1 to F57). F48 is the full function classification, and F51 to F57 cover the A51.
- `codec/` has the native encoder rebuild: libopus v1.1 (submodule), patches 0001 to 0013, Samsung's extra stages in `src/`, and the host build in `native/`.
- `a51-module/` has the Magisk module source: `service.sh` and `module.prop`, plus `helper/SscKick.java`, the root reconnect helper.
- `tools/` has analysis scripts. `helper_timing.py` measures qemu encode timing (F42), and `drc_count.py` counts limiter engagement (F43). `a51_patch_jni.py` patches the A51 stack, and `a51_build_module.sh` builds the module zip.
