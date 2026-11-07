#!/bin/sh
# Build the A51 Magisk module zip: patch the stock stack, add the S24U encoder.
# usage: tools/a51_build_module.sh [OUT.zip]
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
BLOB=$HOME/src/ssc-blob
OUT=${1:-$BLOB/a51/magisk/ssc-uhq-a51-v0.4.zip}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cp -r "$REPO/a51-module/." "$WORK/"
mkdir -p "$WORK/files"
python3 "$REPO/tools/a51_patch_jni.py" "$BLOB/a51/apex/libbluetooth_jni.so" "$WORK/files/libbluetooth_jni.so"
cp "$BLOB/s24u/libScalable_Encoder.so" "$WORK/files/libScalable_Encoder.so"
# Reconnect helper (T6): javac against the plain JDK, then d8 from r8.jar.
R8=${R8:-$BLOB/tools/r8-8.5.35.jar}
mkdir -p "$WORK/cls"
javac --release 11 -d "$WORK/cls" "$REPO/a51-module/helper/SscKick.java"
java -cp "$R8" com.android.tools.r8.D8 --min-api 33 --output "$WORK/cls" "$WORK/cls/SscKick.class"
mv "$WORK/cls/classes.dex" "$WORK/files/ssckick.dex"
rm -rf "$WORK/cls" "$WORK/helper"
rm -f "$OUT"
(cd "$WORK" && zip -qr9 "$OUT" .)
echo "$OUT"
