#!/usr/bin/env python3
"""Print one function from an objdump listing of libbluetooth_jni.so, annotated.

usage: fn.py ADDR [--max N]        (ADDR hex, e.g. 888204; run from ~/src/ssc-uhq/work)
Prints from ADDR to the next function start (bti c / paciasp after the first instruction),
up to N instructions (default 600). Annotations:
  ; "string"        adrp+add that computes the address of a .rodata string
  ; -> fn XXXX      bl target, with the SSC function name if known (from ssc_funcs.txt)
  ; data 0xADDR     adrp+add/ldr into a non-string address (globals, tables, GOT)
"""
import bisect
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(os.path.dirname(HERE), "work")
sys.path.insert(0, HERE)
from xref import rodata_strings  # noqa: E402

INSN = re.compile(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$")


def load_names():
    names = {}
    path = os.path.join(WORK, "ssc_funcs.txt")
    if os.path.exists(path):
        for line in open(path):
            m = re.match(r"fn ([0-9a-f]+): (.*)", line)
            if m:
                parts = [p for p in m.group(2).split(" | ") if not p.startswith("packages/") and p != "a2dp_vendor_ssc"
                         and p != "a2dp_vendor_ssc_encoder"]
                names[int(m.group(1), 16)] = "/".join(parts[:3]) or "?"
    return names


def main():
    start = int(sys.argv[1], 16)
    limit = int(sys.argv[sys.argv.index("--max") + 1]) if "--max" in sys.argv else 600
    strings = rodata_strings(os.path.join(WORK, "jni.so"))
    starts_sorted = sorted(strings)
    names = load_names()
    asm = os.path.join(WORK, "jni.asm")
    # find line offset quickly via index cache
    idx_path = asm + ".idx"
    if not os.path.exists(idx_path):
        with open(asm) as f, open(idx_path, "w") as out:
            pos = 0
            for line in f:
                m = INSN.match(line)
                if m and int(m.group(1), 16) % 0x1000 == 0:
                    out.write(f"{m.group(1)} {pos}\n")
                pos += len(line.encode())
    marks = []
    for line in open(idx_path):
        a, p = line.split()
        marks.append((int(a, 16), int(p)))
    k = bisect.bisect_right(marks, (start, 1 << 62)) - 1
    seek = marks[k][1] if k >= 0 else 0
    pages, count, began = {}, 0, False
    with open(asm, "rb") as f:
        f.seek(seek)
        for raw in f:
            line = raw.decode(errors="replace").rstrip("\n")
            m = INSN.match(line)
            if not m:
                continue
            addr, op, args = int(m.group(1), 16), m.group(2), m.group(3)
            if addr < start:
                continue
            if began and (op == "paciasp" or (op == "bti" and args.startswith("c"))):
                break
            began = True
            note = ""
            if op == "adrp":
                mm = re.match(r"(x\d+),\s+([0-9a-f]+)", args)
                if mm:
                    pages[mm.group(1)] = int(mm.group(2), 16)
            elif op in ("add", "ldr", "ldrb", "ldrh", "ldrsw", "str", "strb"):
                mm = re.match(r"(\w+),\s+\[?(x\d+),\s+#(0x[0-9a-f]+|\d+)\]?", args) if op != "add" else \
                    re.match(r"(x\d+),\s+(x\d+),\s+#(0x[0-9a-f]+|\d+)", args)
                if mm and mm.group(2) in pages:
                    target = pages[mm.group(2)] + int(mm.group(3), 0)
                    j = bisect.bisect_right(starts_sorted, target) - 1
                    if j >= 0 and starts_sorted[j] <= target < starts_sorted[j] + len(strings[starts_sorted[j]]) + 1:
                        s = strings[starts_sorted[j]][target - starts_sorted[j]:]
                        note = f'; "{s[:90]}"'
                    else:
                        note = f"; data 0x{target:x}"
            elif op == "bl":
                mm = re.match(r"([0-9a-f]+)", args)
                if mm:
                    t = int(mm.group(1), 16)
                    note = f"; -> fn {t:x} {names.get(t, '')}".rstrip()
            print(f"{addr:08x}  {op:8s} {args:60s} {note}")
            count += 1
            if count >= limit:
                print("... (truncated)")
                break


if __name__ == "__main__":
    main()
