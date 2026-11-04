#!/usr/bin/env python3
"""Find rip-relative references (lea/mov/cmp with disp32) from .text to a string or address.
Usage: xref.py EXE "string literal"   |   xref.py EXE 0xVA"""
import sys, struct, re
sys.path.insert(0, __file__.rsplit('/',1)[0]); from jai_types import PE
pe = PE(sys.argv[1]); d = pe.d
text = [s for s in pe.secs if s[0] == '.text'][0]
tname, tva, tvsize, traw, trsize = text
def xrefs(target):
    hits = []
    # scan for ModRM rip-relative forms: opcode byte(s) + modrm with mod=00 rm=101, then disp32
    for off in range(traw, traw + trsize - 6):
        modrm = d[off+1]
        if (modrm & 0xC7) != 0x05: continue
        op = d[off]
        if op not in (0x8D, 0x8B, 0x89, 0x3B, 0x39, 0x83, 0x81, 0xFF, 0x0F, 0x80, 0x88, 0x8A, 0xC7, 0xC6, 0xF6, 0xF7): continue
        disp = struct.unpack_from('<i', d, off+2)[0]
        insn_end_va = pe.base + tva + (off - traw) + 6
        if op in (0x83, 0x80, 0xC6): insn_end_va += 1
        elif op in (0x81, 0xC7): insn_end_va += 4
        if insn_end_va + disp == target:
            hits.append(pe.base + tva + (off - traw) - 1)  # -1 assumes a REX prefix byte; print both
    return hits
arg = sys.argv[2]
if arg.startswith('0x'):
    target = int(arg, 16); print(f'target 0x{target:x}')
    for h in xrefs(target): print(f'  xref near 0x{h:x} (rva 0x{h-pe.base:x})')
else:
    needle = arg.encode()
    for m in re.finditer(re.escape(needle), d):
        off = m.start()
        for name, sva, vsize, raw, rsize in pe.secs:
            if raw <= off < raw + rsize:
                va = pe.base + sva + (off - raw)
                print(f'string at 0x{va:x} ({name}) rva 0x{va-pe.base:x}')
                # Jai strings are {count, data}: find the descriptor pointing at it, and code refs to either
                for h in xrefs(va): print(f'  code ref near 0x{h:x} (rva 0x{h-pe.base:x})')
                for soff in range(0, len(d)-8, 8):
                    pass
