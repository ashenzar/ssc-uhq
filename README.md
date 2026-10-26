# SSC UHQ on Linux: match Samsung's encoder driving logic

> **Disclaimer**: This project does NOT include or distribute Samsung's proprietary
> `libScalable_Encoder.so` codec. You must extract it yourself from official Samsung
> firmware for a device you own. See [Obtaining the encoder](#obtaining-the-encoder) below.
> All reverse engineering documented here is for personal interoperability on the author's
> own devices, under DMCA §1201(f) / EU Directive 2009/24 Art 6.

## Goal
Make SSC UHQ (24-bit / 96 kHz) on the Galaxy Buds3 Pro sound correct from Linux by
learning, from Samsung's own Bluetooth stack, exactly how a phone drives
`libScalable_Encoder.so` and packetizes SSC, then reproducing that behaviour in our
openssc-based PipeWire plugin.

## Current state (2026-10-22)
- **Default profile**: UHQ-584 (584 kbps, 96 kHz codec, bits=34)
- **SPA resample quality**: 14 (max — 1024-tap Kaiser filter, 0.993 cutoff, 155 dB stopband)
- **Quality ranking** (44.1k content): UHQ-584 > UHQ-291 > basic-229 > UHQ-152

### Graph modes

`ssc-switch status` shows `[profile mode]` (e.g., `[uhq direct]`).

| Mode | Graph rate | 44.1k source | 48k source | 88.2k source | 96k source |
|------|-----------|-------------|-----------|-------------|-----------|
| **direct** | 96k locked | SPA 44.1→96k (2.177:1) | SPA 48→96k (2:1) | SPA 88.2→96k (1.088:1) | passthrough |
| **normal** | 48k locked | SPA 44.1→48k (1.088:1) + BT-sink 48→96k (2:1) | passthrough + BT-sink 48→96k (2:1) | 88.2→48k (downmix!) + 48→96k | 96→48k (downmix!) + 48→96k |
| **hires** | 48k+96k | SPA 44.1→48k + BT-sink 48→96k | passthrough + BT-sink 48→96k | SPA 88.2→96k (1.088:1) | passthrough |

- **direct**: single-step SPA resample to 96k. Best for mixed-rate sources. Hits 44.1k with
  the hard 2.177:1 ratio but q14's 1024-tap filter handles it well.
- **normal**: double resample for 44.1k (two clean ratios). Best for 44.1k-only listening.
  Destroys 88.2k/96k content (downsamples to 48k then back up).
- **hires**: PipeWire picks closest allowed rate. Best all-rounder — clean ratios for all
  sources. What ssc-auto toggles to when hi-res is detected.

### Hardware
- ThinkPad (Intel AX201), Manjaro, PipeWire 1.6.8 with patched bluez5 plugin
  (HFP mic fix) + openssc SSC codec plugin
- Galaxy Buds3 Pro (E8:C9:13:75:5F:AD), capability byte 0x3e
- Encoder: Samsung S24U `libScalable_Encoder.so` via qemu-aarch64 + systemd-run

### Config files
| File | Purpose |
|------|---------|
| `~/.config/systemd/user/wireplumber.service.d/sscenc.conf` | SSC codec env vars (profile, bits, encoder blob, RTP timestamps) |
| `~/.config/systemd/user/wireplumber.service.d/spa-bluez5-fix.conf` | HFP mic fix (patched libspa-bluez5.so path) |
| `~/.config/pipewire/pipewire.conf.d/50-ssc-uhq-rate.conf` | Graph allowed-rates (default: 48k locked) |
| `~/.config/pipewire/pipewire.conf.d/51-resample-quality.conf` | SPA resample quality (14) |
| `~/.config/pipewire/pipewire-pulse.conf.d/51-resample-quality.conf` | SPA resample quality for PulseAudio clients (14) |
| `~/.config/pipewire/client.conf.d/51-resample-quality.conf` | SPA resample quality for native PW clients (14) |
| `~/.config/wireplumber/wireplumber.conf.d/52-buds-default-a2dp-ssc.conf` | Default to SSC on buds reconnect |
| `~/.config/systemd/user/ssc-auto.service` | systemd user service for ssc-auto (disabled by default) |

All configs are backed up in `config/` mirroring their install paths. To restore after
a PipeWire/WirePlumber update:
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

### Rebuilding after PipeWire update

The patched `.so` files in `~/.local/lib/spa-0.2-patched/bluez5/` must be rebuilt
when PipeWire is updated. Two patches are needed:

**1. HFP mic fix** (source: `~/src/pipewire-hfp-atbcc-fix`):
```sh
cd ~/src/pipewire-hfp-atbcc-fix && ./build-and-install.sh
```
This builds `libspa-bluez5.so` with the eSCO patches and installs it to
`~/.local/lib/spa-0.2-patched/bluez5/`. Upstream: pipewire#5506.

**2. SSC codec plugin** (patch: `config/ssc-pipewire-1.6.8.patch`):
```sh
# Clone PipeWire source matching your version
VER=$(pipewire --version | awk '/Compiled with/{print $NF}')
cd ~/.cache/openssc-build
git clone --depth 1 --branch $VER https://gitlab.freedesktop.org/pipewire/pipewire.git pipewire-$VER
cd pipewire-$VER

# Apply SSC patch (may need manual adjustment for new PipeWire versions)
git apply ~/src/ssc-uhq/config/ssc-pipewire-1.6.8.patch

# Build just the bluez5 plugin
meson setup build -Dsession-managers=[] -Dexamples=disabled -Dtests=disabled
ninja -C build spa/plugins/bluez5/libspa-codec-bluez5-ssc.so

# Install SSC codec plugin alongside HFP-patched bluez5
cp build/spa/plugins/bluez5/libspa-codec-bluez5-ssc.so \
   ~/.local/lib/spa-0.2-patched/bluez5/
systemctl --user restart wireplumber
```

**Dependencies**: `~/src/ssc-blob/s24u/libScalable_Encoder.so` (Samsung encoder,
extracted from S24 Ultra firmware via samloader-rs) and openssc's blob helper at
`~/.local/libexec/sscenc/`.

### Tools
| Tool | Purpose |
|------|---------|
| `ssc-switch {aac\|high\|uhq\|uhq291\|uhq152}` | Switch SSC profile (~15s BT reconnect) |
| `ssc-switch {direct\|hires\|normal}` | Switch graph mode (instant, no audio cut) |
| `ssc-switch status` | Show current `[profile mode]` and codec details |
| `ssc-auto` | Auto-toggle graph mode based on stream rates (disabled by default) |

### Key findings
- Encoder is a pure codec — does NOT resample; rate param is bit-allocation only (F26)
- Buds decode basic-mode SSC at fixed 48 kHz DAC rate — native 44100 Hz not viable (F20)
- Samsung's UHQ bitrate table: 584k/291k/152k — NOT 442k/250k (F11)
- Samsung marketing "up to 2,304 kbps" = raw PCM ceiling; real max is 584k over classic A2DP (F23)
- bits=34 is a mode flag, not PCM depth — buds with cap 0x3e require it (F13)

## Building from scratch

### Prerequisites

```sh
# Arch/Manjaro
sudo pacman -S --needed git meson ninja gcc pkgconf glib2-devel bluez-libs \
  sbc libusb opus liblc3 qemu-user-static aarch64-linux-gnu-gcc
```

### Obtaining the encoder

Samsung's `libScalable_Encoder.so` is proprietary and NOT included in this repo. You must
extract it from official firmware for a Samsung device you own.

**Option A — via adb from your phone** (easiest, any Galaxy with SSC support):
```sh
adb pull /system/lib64/libScalable_Encoder.so ~/src/ssc-blob/libScalable_Encoder.so
```

**Option B — from official firmware** (for S24 Ultra / S26 Ultra encoder with UHQ support):
```sh
# Download firmware (samloader-rs, github.com/martinlopez1234/samloader-rs)
samloader download -m SM-S928B -r EUX -o firmware.zip

# Extract super.img from AP tar, then the system partition
# (extract_super_partition.py or lpunpack + dump.erofs)
dump.erofs --cat system/lib64/libScalable_Encoder.so > ~/src/ssc-blob/s24u/libScalable_Encoder.so
```

**Known working encoders**:
- S24 Ultra (S928BXXU6EZIF): sha256 `90c4c1cb...4ce7` — full UHQ support (E-modes)
- Galaxy A31 (A315GDXS5DWK2): sha256 `b923b408...5959` — basic modes only, no UHQ

### Building the blob helper (qemu bridge)

The ARM64 encoder runs under qemu-aarch64 via a helper binary:
```sh
cd ~/src/openssc  # github.com/sachk/openssc
# Build the blob helper and shim libraries
make -C sscenc/blob CROSS=aarch64-linux-gnu-
mkdir -p ~/.local/libexec/sscenc/aarch64 ~/.local/libexec/sscenc/shims
cp sscenc/blob/ssc_blob_helper ~/.local/libexec/sscenc/aarch64/
cp sscenc/blob/shims/*.so ~/.local/libexec/sscenc/shims/
```

### Building the PipeWire SSC plugin

**1. HFP mic fix** (optional but recommended for Galaxy Buds):
```sh
git clone https://github.com/abrus861/pipewire-hfp-atbcc-fix ~/src/pipewire-hfp-atbcc-fix
cd ~/src/pipewire-hfp-atbcc-fix && ./build-and-install.sh
```

**2. SSC codec plugin**:
```sh
VER=$(pipewire --version | awk '/Compiled with/{print $NF}')
mkdir -p ~/.cache/openssc-build && cd ~/.cache/openssc-build
git clone --depth 1 --branch $VER \
  https://gitlab.freedesktop.org/pipewire/pipewire.git pipewire-$VER
cd pipewire-$VER

# Apply SSC patch
git apply ~/src/ssc-uhq/config/ssc-pipewire-1.6.8.patch

# Build
meson setup build -Dsession-managers=[] -Dexamples=disabled -Dtests=disabled
ninja -C build spa/plugins/bluez5/libspa-codec-bluez5-ssc.so

# Install to user-local SPA path
cp build/spa/plugins/bluez5/libspa-codec-bluez5-ssc.so \
   ~/.local/lib/spa-0.2-patched/bluez5/
```

### Installing configs and scripts

```sh
cd ~/src/ssc-uhq

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

# Scripts
cp config/bin/* ~/.local/bin/
chmod +x ~/.local/bin/ssc-switch ~/.local/bin/ssc-auto

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
`libbluetooth_jni.so` from the Galaxy S26 Ultra Bluetooth APEX (`com.android.bt.apex`), firmware
S948BXXS3AZF4 (official Samsung FUS download).
- libbluetooth_jni.so sha256 108a25d0…e0bb (14.4 MB, aarch64, stripped; SSC functions are
  identifiable only through the function-name strings they log).

## Files
- `config/` — backed-up configs, scripts, and PipeWire SSC patch
- `plan.md` — Phase 1 objectives, tasks, acceptance criteria
- `plan-phase2.md` — Phase 2 quality tuning (mostly complete, T5.2 adaptive bitrate remaining)
- `plan-phase3.md` — Phase 3 AVDTP timing (closed — moot per D21/F20)
- `plan-phase4.md` — Phase 4 native codec rebuild on a libopus CELT base (in progress)
- `decisions.md` — decision log (D1–D32)
- `findings.md` — evidence-backed results with addresses and parameters (F1–F29)
- `tools/` — analysis scripts
