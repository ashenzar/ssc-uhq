# Phase 5: SSC UHQ-584 from a Galaxy A51

Status legend: [ ] todo, [~] in progress, [x] done, [-] dropped (with reason)

Workflow: ssc

## Mission

Make the user's Galaxy A51 (SM-A515F, stock One UI 5.1 A515FXXU8HWK1, Magisk root) stream
SSC UHQ (96 kHz, 584 kbps, E-mode) to the Galaxy Buds3 Pro, as the Linux plugin already does.
The stock stack only does basic 48k (F51). The method is a binary patch of
`libbluetooth_jni.so` plus the S24U encoder, both bind-mounted over the stock files.

## Progress (2026-11-06)

- [x] T1 Negotiation: the phone sends `SET_CONFIGURATION 0e` and the buds accept it (F51).
- [x] T2 96 kHz track, HAL and encoder rate; UHQ bitrate table (F52).
- [x] T3 32-bit HAL PCM, a feeding budget for 4-byte samples, one frame per packet (F53, F54).
- [x] T4 S24U encoder bind-mounted; input >>8 to 2^23 (F55). The user says it "sounds good". The btsnoop shows 111.1 packets/s of 672 B E-mode frames, 0 decode errors.
- [x] T5 Persistence: Magisk module (`a51-module/`, `tools/a51_build_module.sh`). After a reboot it mounts both files in zygote64 and zygote by itself, and the user heard it clean (v0.3, 2026-11-06). Log: `/data/adb/modules/ssc-uhq-a51/service.log`. Uninstall: Magisk app, or `magisk --remove-modules`.
- [x] T6 Buds-initiated connections: module v0.4 watches for an SSC stream set up at 48 kHz and reconnects from the phone, which gets UHQ (F57). User: "worked like a charm". Installed 2026-11-06, active from the next boot (tested with the same watcher started by hand).

## Patch v14 (sha `24a28104…`), built by `tools/a51_patch_jni.py`

| Group | Sites | Finding |
|---|---|---|
| Send and advertise UHQ2 | 0x5478a8, 0x547180 | F51 |
| Ignore the user sample-rate preference | 0x547630 | F56 |
| Rate enum `2 << (cfg & 2)` | 0x5476ec, 0x547700–0x547714 | F52 |
| Track-rate getter: UHQ2 → 96000 | 0x5469c4, stub 0x547718–0x54771c | F52 |
| 96k bitrate table 584/584/291/152k | 0x17e610 | F52 |
| Config bits = 32 with UHQ2; ignore user bits | cave 0x547770–0x54778c, 0x547798, 0x547860 | F53 |
| Feeding bits getter: UHQ2 → 32 | 0x546b34–0x546b40 | F53 |
| One frame per batch, read and packet | 0x54899c, 0x548e54, 0x549810 | F54 |
| `ssc_encode` wrapper, input >>8 at 96k | cave 0xb80ca0, calls 0x549124, 0x549364, 0x549acc | F55 |

Earlier builds v4–v12 are in `~/src/ssc-blob/a51/patched/`, with their scripts in `scripts/` there. Each v*n* adds one step, and F52–F55 record what each step fixed.

## Deploy by hand (testing a new build; the module does this at boot)

1. `python3 tools/a51_patch_jni.py ~/src/ssc-blob/a51/apex/libbluetooth_jni.so OUT.so`, then `adb push` OUT.so and `~/src/ssc-blob/s24u/libScalable_Encoder.so` to `/data/local/tmp/`.
2. As root, for each file: `chown 0:0`, `chmod 0644`, `chcon u:object_r:system_lib_file:s0`.
3. `Z=$(pidof zygote64 | cut -d" " -f1)`, then `nsenter -t $Z -m -- mount -o bind SRC TARGET` for:
   - `/apex/com.android.btservices/lib64/libbluetooth_jni.so`
   - `/system/lib64/libScalable_Encoder.so`
4. `kill $(pidof com.android.bluetooth)`. Check with `sha256sum /proc/<bt pid>/root/<target>`.
5. Connect **from the phone** (Settings, buds, Disconnect, then tap the name). A buds-initiated connection gives basic 48k unless the v0.4 watcher is running (F57).

Checks: `dumpsys bluetooth_manager | grep -A3 "A2DP SSC State"` shows `Rate=96000 Bits=32`. In the btsnoop (via `adb bugreport`), the frames are `ff e0`–`ff e3` and 660 B.

## Next-session checklist

1. After the next reboot, check that v0.4's watcher runs (`service.log` gets a "reconnecting" line after a buds-initiated connect) and listen once.
2. Rebuild the module after any patcher or helper change (`tools/a51_build_module.sh`, then `magisk --install-module`, then reboot). If zygote restarts without a reboot, the mounts are lost until the next boot.
3. Optional: `codec_config` reports 96000/32 while `Selectable` says 48000/24, and the Settings developer-options codec dialog may show odd values. Check that changing the codec there doesn't break the stream.
