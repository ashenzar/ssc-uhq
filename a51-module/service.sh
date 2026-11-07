#!/system/bin/sh
# Bind-mount the patched Bluetooth stack and the S24U SSC encoder into zygote's mount
# namespace, then restart Bluetooth so it forks with them. A mount in Magisk's own
# namespace (post-fs-data) doesn't reach app processes (F51 deploy notes).
MODDIR=${0%/*}
LOG=$MODDIR/service.log
JNI=/apex/com.android.btservices/lib64/libbluetooth_jni.so
ENC=/system/lib64/libScalable_Encoder.so
JNI_STOCK=e2228cc137f74928042e66d04b563612935961e41960463ac9eddfd620416943
ENC_STOCK=9248e26412080fde53f48323746888a89cb98ca11c628b04348b8b4a0f2ab461

exec >>"$LOG" 2>&1
echo "--- $(date) boot"
until [ "$(getprop sys.boot_completed)" = 1 ]; do sleep 2; done

# Refuse to mount over anything but the firmware the patch was built for (A515FXXU8HWK1).
for pair in "$JNI $JNI_STOCK" "$ENC $ENC_STOCK"; do
  set -- $pair
  have=$(sha256sum "$1" | cut -d' ' -f1)
  [ "$have" = "$2" ] || { echo "stock hash mismatch on $1: $have, not mounting"; exit 0; }
done

for f in libbluetooth_jni.so libScalable_Encoder.so; do
  chown 0:0 "$MODDIR/files/$f"; chmod 0644 "$MODDIR/files/$f"
  chcon u:object_r:system_lib_file:s0 "$MODDIR/files/$f"
done

for z in $(pidof zygote64) $(pidof zygote); do
  nsenter -t "$z" -m -- mount -o bind "$MODDIR/files/libbluetooth_jni.so" "$JNI" &&
  nsenter -t "$z" -m -- mount -o bind "$MODDIR/files/libScalable_Encoder.so" "$ENC" &&
  echo "mounted in zygote pid $z"
done

kill $(pidof com.android.bluetooth)
sleep 5
bt=$(pidof com.android.bluetooth)
echo "bluetooth pid $bt"
sha256sum "/proc/$bt/root$JNI" "/proc/$bt/root$ENC"

# T6 (F57): the buds pick basic 48k when they start the connection. When the stack sets up
# an SSC stream at 48 kHz, reconnect from the phone side, which gets UHQ. At most once a
# minute, so a peer that really wants basic can't make it loop.
DEX=$MODDIR/files/ssckick.dex
chcon u:object_r:system_file:s0 "$DEX"
last=0
logcat -T 1 -v brief -s bt_stack:I | while read -r line; do
  case "$line" in *"codecType: SSC"*"sampleRateHz: 48000"*) ;; *) continue ;; esac
  now=$(date +%s)
  [ $((now - last)) -lt 60 ] && continue
  last=$now
  sleep 2
  echo "$(date) SSC set up at 48 kHz, reconnecting from the phone"
  CLASSPATH=$DEX app_process / SscKick reconnect Buds
done &
