#!/usr/bin/env python3
"""String cross-references for a stripped aarch64 ELF disassembled with objdump.

usage: xref.py BINARY ASM 'REGEX' [--funcs]
Indexes NUL-terminated strings in .rodata, resolves adrp+add (and adrp+ldr literal) address
computations in the objdump listing, and prints every code location that references a string
matching REGEX, with the enclosing function start (nearest preceding `bti c` / `paciasp`).
With --funcs, prints one line per enclosing function instead.
"""
import bisect
import re
import subprocess
import sys

ADRP = re.compile(r"^\s*([0-9a-f]+):\s+adrp\s+(x\d+),\s+([0-9a-f]+)")
ADD = re.compile(r"^\s*([0-9a-f]+):\s+add\s+(x\d+),\s+(x\d+),\s+#0x([0-9a-f]+)")
START = re.compile(r"^\s*([0-9a-f]+):\s+(bti\s+c|paciasp)\b")
INSN = re.compile(r"^\s*([0-9a-f]+):")


def rodata_strings(path):
    out = subprocess.run(["readelf", "-S", "-W", path], capture_output=True, text=True).stdout
    m = re.search(r"\.rodata\s+PROGBITS\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", out)
    vaddr, off, size = (int(x, 16) for x in m.groups())
    data = open(path, "rb").read()[off:off + size]
    strings, i = {}, 0
    while i < len(data):
        j = data.find(b"\0", i)
        if j < 0:
            break
        if j - i >= 4:
            try:
                s = data[i:j].decode("utf-8")
                if s.isprintable():
                    strings[vaddr + i] = s
            except UnicodeDecodeError:
                pass
        i = j + 1
    return strings


def main():
    binary, asm, pattern = sys.argv[1:4]
    funcs_only = "--funcs" in sys.argv
    strings = rodata_strings(binary)
    rx = re.compile(pattern)
    wanted = {a: s for a, s in strings.items() if rx.search(s)}
    # also index addresses inside strings (merged/suffix-shared strings)
    starts_sorted = sorted(strings)
    pages, func_starts, hits = {}, [], []
    with open(asm) as f:
        for line in f:
            m = START.match(line)
            if m:
                func_starts.append(int(m.group(1), 16))
            m = ADRP.match(line)
            if m:
                pages[m.group(2)] = int(m.group(3), 16)
                continue
            m = ADD.match(line)
            if m and m.group(3) in pages:
                addr = pages[m.group(3)] + int(m.group(4), 16)
                if addr in wanted:
                    hits.append((int(m.group(1), 16), addr))
                else:
                    k = bisect.bisect_right(starts_sorted, addr) - 1
                    if k >= 0:
                        base = starts_sorted[k]
                        if base in wanted and addr < base + len(strings[base]):
                            hits.append((int(m.group(1), 16), base))
    func_starts.sort()
    seen = {}
    for pc, addr in hits:
        k = bisect.bisect_right(func_starts, pc) - 1
        fn = func_starts[k] if k >= 0 else 0
        if funcs_only:
            seen.setdefault(fn, []).append(wanted[addr])
        else:
            print(f"{pc:08x} in fn {fn:08x}  ->  {addr:08x} {wanted[addr]!r}")
    if funcs_only:
        for fn, names in sorted(seen.items()):
            uniq = list(dict.fromkeys(names))
            print(f"fn {fn:08x}: " + " | ".join(u[:70] for u in uniq[:6]))
    print(f"# {len(wanted)} matching strings, {len(hits)} references", file=sys.stderr)


if __name__ == "__main__":
    main()
