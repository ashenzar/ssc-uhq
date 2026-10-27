#!/usr/bin/env python3
# Usage: drc_count.py FILE RATE BITS  (96000 24 = UHQ at 2^23, 48000 16 = basic at 2^15). Counts
# 108-sample stereo blocks where the SSC limiter engages (> 24 of 216 values above the
# threshold), for the loosest and strictest CalculateLimitGain thresholds. ssc-uhq F43.4.
import subprocess, sys, numpy as np
f, rate, bits = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
raw = subprocess.run(['ffmpeg','-v','error','-i',f,'-ac','2','-ar',str(rate),'-af','aresample=resampler=soxr:precision=28','-f','s32le','-'],capture_output=True).stdout
x = np.abs((np.frombuffer(raw,'<i4') >> (32-bits)).astype(np.int64))   # plugin level: 2^23 UHQ, 2^15 basic
nb = len(x)//216; x = x[:nb*216].reshape(nb,216)
fs = 1 << (bits-1)
ts = {'uhq': (0x75d4, 0x7b4c), 'basic': (0x741d, 0x7a94)}['uhq' if bits == 24 else 'basic']
out = [f"{f.split('/')[-1][:28]:28s} {rate//1000}k {bits}-bit: {nb} blocks, peak {20*np.log10(x.max()/fs):.2f} dBFS"]
for t in ts:
    T = t << (bits-16)
    eng = (x > T).sum(1) > 24
    out.append(f"T={t/32768:.3f}: {eng.sum()} blocks engaged ({100*eng.mean():.3f}%), first at {np.argmax(eng)*108/rate if eng.any() else -1:.1f} s")
print('  '.join(out))
