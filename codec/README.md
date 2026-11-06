# codec: native SSC encoder

Our rebuild of Samsung's SSC encoder (`libScalable_Encoder.so`) for x86_64. It produces the
same bytes as the blob on every frame tested (F46, F47) and lets the PipeWire plugin run
without qemu (`SSCENC_BACKEND=native`).

SSC is a fork of fixed-point CELT from libopus v1.1 (F27, F29). The rebuild is libopus v1.1
plus our patches and Samsung's extra encoder stages, written from scratch.

This directory holds only our own source. No Samsung code, decompiler output or table is
committed (D28). The build extracts the tables it needs from your own copy of the blob into
the gitignored `native/build/ssc_tables.h`.

## Layout

| Path | Contents |
|------|----------|
| `third_party/opus` | libopus at tag v1.1 (BSD-3), a git submodule |
| `patches/` | 0001 to 0013, one confirmed Samsung delta each, applied to v1.1's `celt/` (F31 to F46) |
| `src/ssc_encoder.c` | `ssc_encode`: v1.1's `celt_encode_with_ec` plus Samsung's headroom EQ, DRC limiter and adaptive top band |
| `src/ssc_abi.h`, `src/ssc_encoder.h` | the encoder handle, laid out as the blob lays it out |
| `src/ssc_header.h` | SSC frame header tags and their bitrates |
| `src/trace.h`, `src/trace_calls.h` | call-trace hooks; `-DSSC_NO_TRACE` compiles them out |
| `native/` | the host build: `libsscenc.a`, the plugin helper and three test tools |
| `tools/ssc-extract-tables.py` | writes `ssc_tables.h` from the blob |

## Build

You need `gcc`, `make`, `python3`, and the S24 Ultra or S26 Ultra blob. The table extractor
accepts only sha256 `90c4c1cb...4ce7` (S24U S928BXXU6EZIF; the S26U AZF4 copy is identical).
See "Obtaining the encoder" in the top-level README.

```sh
git submodule update --init codec/third_party/opus     # from the ssc-uhq root
make -C codec/native BLOB=/path/to/libScalable_Encoder.so
```

`BLOB` defaults to `~/src/ssc-blob/s24u/libScalable_Encoder.so`. The Makefile copies v1.1's
`celt/` into `native/build/src`, applies `patches/` in order, extracts the tables, and builds
into `native/build/`.

## Install for the plugin

```sh
mkdir -p ~/.local/libexec/sscenc/x86_64
cp codec/native/build/ssc_native_helper ~/.local/libexec/sscenc/x86_64/
```

Then set `Environment=SSCENC_BACKEND=native` in
`~/.config/systemd/user/wireplumber.service.d/sscenc.conf` and run
`systemctl --user daemon-reload && systemctl --user restart wireplumber`. Reinstall the helper
after every change to the encoder.

`ssc_native_helper` speaks the protocol of openssc's `ssc_blob_helper`. It takes
`[blob.so] rate channels bits bitrate` as arguments and writes an int32 0 when ready. Then,
per frame, it reads `u32 n | int32 pcm[2n]` with n = 864 and writes `i32 len | bytes`. With
`SSCENC_TEE_CMD` set to a blob helper command, it also runs the blob on every frame, passes the
blob's frame on, and logs each frame where the two differ to stderr. The launcher sets this up
for `SSCENC_BACKEND=tee`.

## Test tools

```sh
native/build/ssc_native_bench pcm.s32 96000 584000                              # encode timing
native/build/ssc_native_roundtrip pcm.s32 96000 584000 out.wav [bits [shift]]   # encode, decode, SNR
native/build/ssc_native_decode frames 96000|48000 out.wav                       # decode captured frames
```

Input PCM is S32LE stereo at 2^31 full scale (`ffmpeg -i track.flac -ac 2 -ar 96000 -f s32le
pcm.s32`). The tools shift it down to the plugin's level. `ssc_native_decode` reads
`u32 len | bytes` frames and fails a frame on an unknown header, a range-decoder error, or a
range coder that stops short of the payload or its zero padding. Both decoders write 16-bit
stereo WAV with the decoder's 108-sample delay removed. They use the patched libopus decoder
(patches 0010 and 0013).
