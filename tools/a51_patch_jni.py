#!/usr/bin/env python3
"""Patch the Galaxy A51 (SM-A515F, A515FXXU8HWK1) Bluetooth stack for SSC UHQ-584.

usage: a51_patch_jni.py STOCK.so OUT.so

STOCK is libbluetooth_jni.so from com.android.btservices (sha256 e2228cc1...).
Each patch asserts the stock instruction before writing. In the text segment the
file offset equals the vaddr. Needs aarch64-linux-gnu-{as,ld,objcopy}.
See plan-phase5-a51.md (patch table) and findings F51-F55.
"""
import hashlib
import os
import re
import struct
import subprocess
import sys
import tempfile

CAVE = 0xB80CA0  # zero padding at the end of the r-x segment, mapped executable

# ssc_encode wrapper: at 96 kHz, shift the int32 PCM (Q31 from the HAL) right by 8
# to 2^23 full scale (Linux SSCENC_INPUT_SHIFT=0), then tail-call ssc_encode (x8).
CAVE_ASM = """
_start:
  adrp x9, _start + (0xc66000 - 0xb80ca0)
  ldr  w9, [x9, #0x7c]          // track sample rate global 0xc6607c
  mov  w10, #0x7700
  movk w10, #0x1, lsl #16       // 96000
  cmp  w9, w10
  b.ne 2f
  mov  x9, x1                   // x1 = PCM input, 864 frames x 2 ch x int32
  mov  x10, #432
1:
  ldr  q16, [x9]
  sshr v16.4s, v16.4s, #8
  str  q16, [x9], #16
  subs x10, x10, #1
  b.ne 1b
2:
  br   x8
"""

# (vaddr, stock instruction, new instruction). Branch targets 0x54xxxx/0xb8xxxx
# are absolute and get rewritten PC-relative.
PATCHES = [
    # F51 negotiation: send and advertise UHQ2 (0x02)
    (0x5478A8, "and w8,w8,#0x8", "and w8,w8,#0xe"),
    (0x547180, "mov w11,#0x28", "mov w11,#0x2a"),
    # F52 codec_config rate enum = 2 << (cfg & 2): 96k with UHQ2, else 48k
    (0x5476EC, "tbnz w24,#4,0x547764", "tbnz w24,#4,0x547704"),
    (0x547700, "tst w24,#0x30", "tbz w24,#4,0x5476e0"),
    (0x547704, "mov w8,#0x8", "ldr x9,[sp,#16]"),
    (0x547708, "mov w9,#0x2", "and w9,w9,#0x2"),
    (0x54770C, "mov w10,#0x10", "mov w8,#0x2"),
    (0x547710, "csel w8,w9,w8,ne", "lsl w8,w8,w9"),
    (0x547714, "mov w9,#0xffffff80", "b 0x547768"),
    # F56 ignore the user sample-rate preference (the codec switch in developer options
    # sends 48k, whose branch at 0x54796c bypasses the UHQ2 rate logic)
    (0x547630, "ldr w8,[x20,#320]", "mov w8,#0x0"),
    # F52 track-rate getter: UHQ2 -> 96000 (2-instruction stub in the freed slots)
    (0x5469C4, "tbnz w8,#2,0x546a40", "tbnz w8,#2,0x547718"),
    (0x547718, "csel w24,w10,w9,ne", "tbnz w8,#1,0x546a60"),
    (0x54771C, "b 0x54776c", "b 0x546a40"),
    # F53 codec_config bits = 32 (enum 4) with UHQ2; ignore the user bits preference
    (0x547770, None, "str wzr,[x20,#100]"),
    (0x547774, None, "b 0x547798"),
    (0x547778, None, "ldr x9,[sp,#16]"),
    (0x54777C, "ldr w4,[x20,#324]", "tbz w9,#1,0x547788"),
    (0x547780, "add x1,x1,#0x285", "mov w9,#0x4"),
    (0x547784, "add x2,x2,#0x1a6", "str w9,[x20,#100]"),
    (0x547788, "add x3,x3,#0x5a7", "ldr w8,[x20,#160]"),
    (0x54778C, "mov w0,#0x5", "b 0x547864"),
    (0x547798, "ldr w4,[x20,#324]", "mov w4,#0x0"),
    (0x547860, "ldr w8,[x20,#160]", "b 0x547778"),
    # F53 feeding bits getter: UHQ2 -> 32 (4-byte samples in the byte budget)
    (0x546B34, "mov w8,#0x18", "tst w8,#0x2"),
    (0x546B38, "cmp w19,#0x80", "mov w8,#0x18"),
    (0x546B3C, "mov w9,#0x22", "mov w9,#0x20"),
    (0x546B40, "csel w0,w9,w8,eq", "csel w0,w9,w8,ne"),
    # F54 one 864-sample frame per batch, read and packet at 96k (no fragmentation)
    (0x54899C, "mov w10,#0x3600", "mov w10,#0x1b00"),
    (0x548E54, "mov w10,#0x3600", "mov w10,#0x1b00"),
    (0x549810, "mov w10,#0x3600", "mov w10,#0x1b00"),
    # F55 every ssc_encode call goes through the >>8 cave
    (0x549124, "blr x8", "bl 0xb80ca0"),
    (0x549364, "blr x8", "bl 0xb80ca0"),
    (0x549ACC, "blr x8", "bl 0xb80ca0"),
]
# The three adrp at 0x547770-0x547778 (log call) are checked as raw words.
ADRP_STOCK = {0x547770: 0xB0FFE081, 0x547774: 0xB0FFDF62, 0x547778: 0xD0FFDEA3}

# F52 bitrate table for the 96k path: UHQ 584/584/291/152k (F11), was 328k x4
RATE_TABLE = 0x17E610


def asm(src, at):
    src = re.sub(r"0x(5[0-9a-f]{5}|b8[0-9a-f]{4})\b",
                 lambda m: ".+(%d)" % (int(m.group(1), 16) - at), src)
    with tempfile.TemporaryDirectory() as d:
        open(d + "/a.s", "w").write(src + "\n")
        subprocess.check_call(["aarch64-linux-gnu-as", "-o", d + "/a.o", d + "/a.s"])
        subprocess.check_call(["aarch64-linux-gnu-objcopy", "-O", "binary",
                               "-j", ".text", d + "/a.o", d + "/a.bin"])
        return open(d + "/a.bin", "rb").read()


def cave_bytes():
    with tempfile.TemporaryDirectory() as d:
        open(d + "/c.s", "w").write(".text\n.globl _start\n" + CAVE_ASM)
        subprocess.check_call(["aarch64-linux-gnu-as", "-o", d + "/c.o", d + "/c.s"])
        subprocess.check_call(["aarch64-linux-gnu-ld", "-Ttext=%#x" % CAVE,
                               "-o", d + "/c.elf", d + "/c.o"])
        subprocess.check_call(["aarch64-linux-gnu-objcopy", "-O", "binary",
                               "-j", ".text", d + "/c.elf", d + "/c.bin"])
        return open(d + "/c.bin", "rb").read()


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    b = bytearray(open(sys.argv[1], "rb").read())
    for off, old, new in PATCHES:
        cur = b[off:off + 4]
        if old is None:
            want = struct.pack("<I", ADRP_STOCK[off])
            assert cur == want, (hex(off), cur.hex(), want.hex())
        else:
            want = asm(old, off)
            assert cur == want, (hex(off), old, cur.hex(), want.hex())
        b[off:off + 4] = asm(new, off)
    assert struct.unpack_from("<4I", b, RATE_TABLE) == (328000,) * 4
    struct.pack_into("<4I", b, RATE_TABLE, 584000, 584000, 291000, 152000)
    cave = cave_bytes()
    assert b[CAVE:CAVE + len(cave)] == bytes(len(cave))
    b[CAVE:CAVE + len(cave)] = cave
    open(sys.argv[2], "wb").write(b)
    print(hashlib.sha256(b).hexdigest(), os.path.basename(sys.argv[2]))


if __name__ == "__main__":
    main()
