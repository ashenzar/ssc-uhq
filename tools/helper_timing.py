#!/usr/bin/env python3
# Usage: helper_timing.py RATE BITRATE SHIFT FILE SECONDS   (SHIFT 0 = UHQ 2^23, 8 = basic 2^15). ssc-uhq F42.
# T0.1: per-frame round-trip time of the live helper path (sscenc-blob-run -> systemd-run -> qemu).
import os, subprocess, struct, sys, time
rate, bitrate, shift, src, seconds = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], int(sys.argv[5])
env = dict(os.environ, SSCENC_BLOB_SO=os.path.expanduser('~/src/ssc-blob/s24u/libScalable_Encoder.so'))
h = subprocess.Popen([os.path.expanduser('~/.local/libexec/sscenc/sscenc-blob-run'), str(rate), '2', '34', str(bitrate)],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env, bufsize=0)
ff = subprocess.Popen(['ffmpeg', '-v', 'error', '-ss', '60', '-t', str(seconds), '-i', src, '-ac', '2', '-ar', str(rate),
                       '-f', 's32le', '-'], stdout=subprocess.PIPE)
import numpy as np
def rd(n):
    b = b''
    while len(b) < n:
        c = h.stdout.read(n - len(b))
        if not c: raise SystemExit('helper closed')
        b += c
    return b
t0 = time.perf_counter(); assert struct.unpack('<i', rd(4))[0] == 0   # ready word, as start_blob reads it
startup = (time.perf_counter() - t0) * 1e3
fb = 864 * 2 * 4; times = []; lens = set()
while True:
    raw = ff.stdout.read(fb)
    if len(raw) < fb: break
    pcm = (np.frombuffer(raw, '<i4') >> (8 + shift)).astype('<i4').tobytes()   # S32 -> 2^23 (shift 0) or 2^15 (shift 8)
    t = time.perf_counter()
    h.stdin.write(struct.pack('<I', 864) + pcm)
    n = struct.unpack('<i', rd(4))[0]
    if n > 0: rd(n)
    times.append((time.perf_counter() - t) * 1e3); lens.add(n)
h.stdin.close(); h.wait()
budget = 864 / rate * 1e3
a = np.array(times[1:])
print(f'{rate} {bitrate}: {len(a)} frames, len {sorted(lens)}, startup {startup:.0f} ms, first frame {times[0]:.1f} ms; '
      f'p50 {np.percentile(a,50):.2f} p90 {np.percentile(a,90):.2f} p99 {np.percentile(a,99):.2f} '
      f'p99.9 {np.percentile(a,99.9):.2f} max {a.max():.2f} ms; budget {budget:.2f} ms; over budget {int((a>budget).sum())}; '
      f'mean load {a.mean()/budget*100:.0f}% of real time')
